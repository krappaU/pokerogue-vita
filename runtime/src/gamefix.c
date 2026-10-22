// SPDX-License-Identifier: AGPL-3.0-only
// Rewrites of PokeRogue's own code that give the same results with less work, applied to a chunk's
// source before it is compiled (js_host.c's loader and precompile.c's workers), the way js/webgl.js
// rewrites the game's shaders. Each rewrite is an exact piece of the minified bundle with the
// variable names left open (@1..@9 and @A..@Z stand for an identifier; a repeated one is the same
// name; different ones may be the same name too, as a minifier reuses short names). If the
// game changes that code, nothing matches and its own code runs. config.txt gamefix=0 turns them off.
//
// sprite colours: PokeRogue's sprite pipeline (src/pipelines/sprite.ts) builds four 128-value colour
// arrays per Pokémon sprite per frame with 128 `arr.splice(arr.length, 0, ...four)` calls. For a
// sprite that isn't shiny (onBatch) or has no fusion colours (onBind) every value is 0, and the
// arrays only reach the GPU through `arr.flat()` (a copy). Those cases now take a shared array of
// 128 zeros (globalThis.__pkrZero128, js/core.js) instead of the loop. For a shiny sprite (onBatch)
// the arrays come from the variant colour cache's entry, a JSON object the game replaces but never
// changes (src/sprites/variant.ts), parsing 64 hex colours on the way: they are kept per entry
// (globalThis.__pkrMemo, a WeakMap) and built once. The arrays are only ever read, so sharing them
// is safe. On the Vita the pipeline's methods were ~37% of a battle frame's JS (0.78, log108; a
// shiny Pokémon was on the field). Since 0.90 the arrays go to the uniform setters without the
// flat() copy (they are flat already: numbers spread in by splice). Phaser skips a uniform write
// when it gets the same array as last time, so the shared ones (zeros, a shiny's kept colours) are
// uploaded once instead of four 128-value uploads per Pokémon sprite per frame.
//
// arena uniforms once per frame: after the tints above, FieldSpritePipeline.onBind still makes nine
// uniform calls per sprite, most of them writing what the previous sprite wrote. A bind in the same
// frame, on the same shader, with the same sprite values (ignoreTimeTint, terrainColorRatio, time)
// as the pipeline's previous bind now returns before them: each would have found its uniform
// already holding that value (Phaser compares) and done nothing. (0.89, log140: ~3% of a Temple
// battle frame's JS in that method's own code.)
//
// starter slots from one starter list: the starter select screen's setup (after every game reset:
// a new run rebuilds the UI) creates its 81 slots with `getAllStarters()[i]`, and each call goes
// through all ~1300 species to list the starters again: 4.7 s of an 11 s frame on the Vita (0.89,
// log141). The list is now made once for the loop; nothing in it changes the species data.
//
// starter filters read once per filtering: StarterSelectUiHandler.filterStarters runs its filter over
// every starter and reads the filter bar ten times per starter (getVals: the dropdown's options
// filtered and mapped into a new array each time); ~0.65 of the 0.94 s each filter change took on
// the Vita (log141). Within one filtering the filter bar doesn't change and the arrays are only read
// (includes, some), so filterStarters now runs the original (as pkrFilterStarters) with getVals
// remembering each result for that run, and puts getVals back afterwards.
//
// party icons of destroyed slots dropped: PokemonIconAnimHelper (party screen) keeps every icon
// added to it, and the party screen destroys its slots (clearPartySlots) without taking their icons
// out, so each opening adds the party's icons again; two endless tweens set the y of all of them
// (every 200 ms, and every frame of a jump). In a long session that was hundreds of destroyed sprites
// per frame. The two loops now drop an icon whose sprite was destroyed (no scene) instead of moving it.
//
// arena tints per frame: FieldSpritePipeline.onBind (every field and Pokémon sprite) works out the
// arena's day, dusk, night, override and terrain colours (five new arrays, `isOutside` three times)
// for each sprite, though they only change between frames (game logic runs in Phaser's update, the
// binds in its render). They are kept on the pipeline for the frame (game.loop.frame), computed in
// the same order; the same arrays passed again also let Phaser skip the repeated uniform writes
// (its WebGLShader compares the value with the last one). 0.81's battle frames (log111): ~12% of
// the frame's JS in that method, isOutside and map.

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"

typedef struct {
    const char *name, *find, *replace;
    const char *before; // must also occur, with the same names, in the 800 bytes before the match
    const char *marker; // a chunk without this text is skipped quickly
    int max;            // matches to rewrite (0: one; a second copy would be a different game build)
} fix;

static const fix FIXES[] = {
    {"sprite colours (not shiny)",
     "else for(let @1=0;@1<32;@1++)@2.splice(@2.length,0,...@3),@4.splice(@4.length,0,...@3);"
     "this.set4fv(`baseVariantColors`,@2.flat()).set4fv(`variantColors`,@4.flat())",
     "else @2=@4=globalThis.__pkrZero128;"
     "this.set4fv(`baseVariantColors`,@2).set4fv(`variantColors`,@4)",
     "@3=[0,0,0,0],@2=[],@4=[];", "baseVariantColors", 0},
    {"sprite colours (no fusion)",
     "@1=[0,0,0,0],@2=[],@3=[];for(let @4=0;@4<32;@4++)@2.splice(@2.length,0,...@4<@5.length?@5[@4].map(@6=>@6/255):@1),"
     "@3.splice(@3.length,0,...@4<@7.length?@7[@4]:@1);"
     "this.set4fv(`spriteColors`,@2.flat()).set4iv(`fusionSpriteColors`,@3.flat())",
     "@1=[0,0,0,0],@2=[],@3=[];if(@5.length===0&&@7.length===0)@2=@3=globalThis.__pkrZero128;"
     "else for(let @4=0;@4<32;@4++)@2.splice(@2.length,0,...@4<@5.length?@5[@4].map(@6=>@6/255):@1),"
     "@3.splice(@3.length,0,...@4<@7.length?@7[@4]:@1);"
     "this.set4fv(`spriteColors`,@2).set4iv(`fusionSpriteColors`,@3)",
     NULL, "baseVariantColors", 0},
    {"shiny colours kept",
     "&&Object.hasOwn(@1,@2)){let @3=Object.keys(@1[@2]);for(let @4=0;@4<32;@4++)if(@4<@3.length){"
     "let @5=Array.from(Object.values(@6(@3[@4]))),@7=Array.from(Object.values(@6(@1[@2][@3[@4]])));"
     "@8.splice(@8.length,0,...@5.map(@A=>@A/255)),@9.splice(@9.length,0,...@7.map(@B=>@B/255))}"
     "else @8.splice(@8.length,0,...@C),@9.splice(@9.length,0,...@C)}else ",
     "&&Object.hasOwn(@1,@2)){let pkrK=@1[@2],pkrC=typeof pkrK===`object`&&pkrK?globalThis.__pkrMemo.get(pkrK):void 0;"
     "if(pkrC)@8=pkrC[0],@9=pkrC[1];else{let @3=Object.keys(@1[@2]);for(let @4=0;@4<32;@4++)if(@4<@3.length){"
     "let @5=Array.from(Object.values(@6(@3[@4]))),@7=Array.from(Object.values(@6(@1[@2][@3[@4]])));"
     "@8.splice(@8.length,0,...@5.map(@A=>@A/255)),@9.splice(@9.length,0,...@7.map(@B=>@B/255))}"
     "else @8.splice(@8.length,0,...@C),@9.splice(@9.length,0,...@C);"
     "typeof pkrK===`object`&&pkrK&&globalThis.__pkrMemo.set(pkrK,[@8,@9])}}else ",
     "@C=[0,0,0,0],@8=[],@9=[];", "baseVariantColors", 0},
    {"arena tints per frame",
     ".setBoolean(`isOutside`,@1.arena.isOutside()).set3fv(`overrideTint`,@2().map(@3=>@3/255))"
     ".set3fv(`dayTint`,@1.arena.getDayTint().map(@4=>@4/255)).set3fv(`duskTint`,@1.arena.getDuskTint().map(@5=>@5/255))"
     ".set3fv(`nightTint`,@1.arena.getNightTint().map(@6=>@6/255)).set3fv(`terrainColor`,@7(@1.arena.terrainType).map(@8=>@8/255))",
     ".setBoolean(`isOutside`,(this.__pkrT&&this.__pkrT.f===this.game.loop.frame?this.__pkrT:(this.__pkrT={f:this.game.loop.frame,"
     "o:@1.arena.isOutside(),ot:@2().map(@3=>@3/255),d:@1.arena.getDayTint().map(@4=>@4/255),"
     "du:@1.arena.getDuskTint().map(@5=>@5/255),n:@1.arena.getNightTint().map(@6=>@6/255),"
     "tc:@7(@1.arena.terrainType).map(@8=>@8/255)})).o).set3fv(`overrideTint`,this.__pkrT.ot)"
     ".set3fv(`dayTint`,this.__pkrT.d).set3fv(`duskTint`,this.__pkrT.du).set3fv(`nightTint`,this.__pkrT.n)"
     ".set3fv(`terrainColor`,this.__pkrT.tc)",
     NULL, "terrainColorRatio", 0},
    {"arena uniforms once per frame",
     "let @1=@2.pipelineData,@3=!!@1.ignoreTimeTint,@4=@1.terrainColorRatio??0,"
     "@5=@6.currentBattle?.waveIndex?(@6.currentBattle.waveIndex+@6.waveCycleOffset)%40/40:@7();this.set1f(`time`,@5)",
     "let @1=@2.pipelineData,@3=!!@1.ignoreTimeTint,@4=@1.terrainColorRatio??0,"
     "@5=@6.currentBattle?.waveIndex?(@6.currentBattle.waveIndex+@6.waveCycleOffset)%40/40:@7(),"
     "pkrS=this.__pkrS,pkrF=this.game.loop.frame;"
     "if(pkrS&&pkrS.f===pkrF&&pkrS.s===this.currentShader&&pkrS.m===@3&&pkrS.v===@4&&pkrS.y===@5)return;"
     "this.__pkrS={f:pkrF,s:this.currentShader,m:@3,v:@4,y:@5};this.set1f(`time`,@5)",
     NULL, "terrainColorRatio", 0},
    {"party icons of destroyed slots dropped",
     "for(let[@1,@2]of this.icons)@1.y=@2.restY+this.getCurrentOffset(@2.mode)",
     "for(let[@1,@2]of this.icons)@1.scene?@1.y=@2.restY+this.getCurrentOffset(@2.mode):this.icons.delete(@1)",
     NULL, "updateJumpIcons", 2},
    {"starter slots from one starter list",
     "for(let @1=0;@1<81;@1++){let @2=calcStarterContainerPosition(@1),@3=new StarterContainer(@4.getAllStarters()[@1])",
     "for(let @1=0,pkrA=@4.getAllStarters();@1<81;@1++){let @2=calcStarterContainerPosition(@1),@3=new StarterContainer(pkrA[@1])",
     NULL, "calcStarterContainerPosition", 0},
    {"starter filters read once per filtering",
     "filterStarters(){this.filteredStarterIds=@1.getAllStarters().filter(",
     "filterStarters(){let pkrB=this.filterBar,pkrG=pkrB.getVals,pkrH=Object.hasOwn(pkrB,`getVals`),pkrM=new Map;"
     "pkrB.getVals=function(pkrK){let pkrV=pkrM.get(pkrK);return pkrV===void 0&&pkrM.set(pkrK,pkrV=pkrG.call(this,pkrK)),pkrV};"
     "try{return this.pkrFilterStarters()}finally{pkrH?pkrB.getVals=pkrG:delete pkrB.getVals}}"
     "pkrFilterStarters(){this.filteredStarterIds=@1.getAllStarters().filter(",
     NULL, "filteredStarterIds", 0},
};
#define NFIX (int)(sizeof FIXES / sizeof FIXES[0])

static bool ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$';
}

typedef struct {
    const char *at[36];
    size_t len[36];
} caps;

// "@1".."@9", "@A".."@Z": the capture's slot, else -1.
static int slot(const char *p) {
    if (p[0] != '@') return -1;
    if (p[1] >= '1' && p[1] <= '9') return p[1] - '0';
    if (p[1] >= 'A' && p[1] <= 'Z') return 10 + p[1] - 'A';
    return -1;
}

// Matches pattern p at s (end e). Returns the length matched, or 0.
static size_t match_at(const char *s, const char *e, const char *p, caps *c) {
    const char *s0 = s;
    while (*p) {
        int k = slot(p);
        if (k >= 0) {
            p += 2;
            if (c->at[k]) { // the same name again
                if ((size_t)(e - s) < c->len[k] || memcmp(s, c->at[k], c->len[k]) || (s + c->len[k] < e && ident_char(s[c->len[k]])))
                    return 0;
                s += c->len[k];
                continue;
            }
            if (s >= e || !ident_char(*s) || (*s >= '0' && *s <= '9'))
                return 0;
            const char *n = s;
            while (n < e && ident_char(*n)) n++;
            c->at[k] = s, c->len[k] = (size_t)(n - s);
            s = n;
            continue;
        }
        if (s >= e || *s != *p) return 0;
        s++, p++;
    }
    return (size_t)(s - s0);
}

// Appends template t with the captures filled in.
static void expand(char *out, size_t *o, const char *t, const caps *c) {
    for (; *t; t++) {
        int k = slot(t);
        if (k >= 0) {
            t++;
            memcpy(out + *o, c->at[k], c->len[k]);
            *o += c->len[k];
        } else {
            out[(*o)++] = *t;
        }
    }
}

static size_t expanded_len(const char *t, const caps *c) {
    size_t n = 0;
    for (; *t; t++) {
        int k = slot(t);
        if (k >= 0) n += c->len[k], t++;
        else n++;
    }
    return n;
}

// The fixed source (malloc'd, *out_len set), or NULL when nothing applies.
char *gamefix_source(const char *url, const char *src, size_t len, size_t *out_len) {
    if (g_cfg.no_gamefix || !src) return NULL;
    // Cheap test first: every chunk passes through here.
    bool has_marker[NFIX], any = false;
    for (int f = 0; f < NFIX; f++) {
        const char *mk = FIXES[f].marker;
        size_t ml = strlen(mk);
        has_marker[f] = false;
        for (size_t i = 0; !has_marker[f] && i + ml <= len; i++) has_marker[f] = src[i] == mk[0] && !memcmp(src + i, mk, ml);
        any |= has_marker[f];
    }
    if (!any) return NULL;

    char *cur = NULL;
    size_t cur_len = len;
    const char *in = src;
    char applied[400] = "";
    for (int f = 0; f < NFIX; f++) {
        const fix *x = &FIXES[f];
        if (!has_marker[f]) continue;
        const char *e = in + cur_len;
        int hits = 0, max = x->max > 0 ? x->max : 1;
        for (const char *s = in; s < e && hits < max; s++) {
            if (x->find[0] == '@' ? s > in && ident_char(s[-1]) : *s != x->find[0]) continue;
            caps c = {0};
            size_t m = match_at(s, e, x->find, &c);
            if (!m) continue;
            if (x->before) {
                size_t blen = expanded_len(x->before, &c);
                char *b = malloc(blen + 1);
                size_t bo = 0;
                expand(b, &bo, x->before, &c);
                b[bo] = 0;
                const char *from = s - in > 800 ? s - 800 : in;
                bool found = false;
                for (const char *q = from; !found && q + blen <= s; q++) found = !memcmp(q, b, blen);
                free(b);
                if (!found) continue;
            }
            size_t rlen = expanded_len(x->replace, &c);
            char *out = malloc(cur_len - m + rlen + 1);
            if (!out) break;
            size_t o = (size_t)(s - in);
            memcpy(out, in, o);
            expand(out, &o, x->replace, &c);
            memcpy(out + o, s + m, (size_t)(e - (s + m)));
            o += (size_t)(e - (s + m));
            out[o] = 0;
            size_t next = (size_t)(s - in) + rlen; // go on after the rewritten text
            free(cur);
            cur = out, in = out, cur_len = o, e = in + cur_len;
            s = in + next - 1;
            hits++;
        }
        if (hits) snprintf(applied + strlen(applied), sizeof applied - strlen(applied), "%s%s", applied[0] ? ", " : "", x->name);
    }
    if (!cur) return NULL;
    const char *base = strrchr(url, '/');
    host_log("[gamefix] %s: %s", base ? base + 1 : url, applied);
    *out_len = cur_len;
    return cur;
}
