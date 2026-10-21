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
// (globalThis.__pkrMemo, a WeakMap) and built once. The arrays are only ever read (flat()), so
// sharing them is safe. On the Vita the pipeline's methods were ~37% of a battle frame's JS (0.78,
// log108; a shiny Pokémon was on the field).

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"

typedef struct {
    const char *name, *find, *replace;
    const char *before; // must also occur, with the same names, in the 800 bytes before the match
} fix;

static const fix FIXES[] = {
    {"sprite colours (not shiny)",
     "else for(let @1=0;@1<32;@1++)@2.splice(@2.length,0,...@3),@4.splice(@4.length,0,...@3);"
     "this.set4fv(`baseVariantColors`,@2.flat()).set4fv(`variantColors`,@4.flat())",
     "else @2=@4=globalThis.__pkrZero128;"
     "this.set4fv(`baseVariantColors`,@2.flat()).set4fv(`variantColors`,@4.flat())",
     "@3=[0,0,0,0],@2=[],@4=[];"},
    {"sprite colours (no fusion)",
     "@1=[0,0,0,0],@2=[],@3=[];for(let @4=0;@4<32;@4++)@2.splice(@2.length,0,...@4<@5.length?@5[@4].map(@6=>@6/255):@1),"
     "@3.splice(@3.length,0,...@4<@7.length?@7[@4]:@1);",
     "@1=[0,0,0,0],@2=[],@3=[];if(@5.length===0&&@7.length===0)@2=@3=globalThis.__pkrZero128;"
     "else for(let @4=0;@4<32;@4++)@2.splice(@2.length,0,...@4<@5.length?@5[@4].map(@6=>@6/255):@1),"
     "@3.splice(@3.length,0,...@4<@7.length?@7[@4]:@1);",
     NULL},
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
     "@C=[0,0,0,0],@8=[],@9=[];"},
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
    static const char marker[] = "baseVariantColors";
    bool any = false;
    for (size_t i = 0; !any && i + sizeof marker - 1 <= len; i++)
        any = src[i] == 'b' && !memcmp(src + i, marker, sizeof marker - 1);
    if (!any) return NULL;

    char *cur = NULL;
    size_t cur_len = len;
    const char *in = src;
    char applied[200] = "";
    for (int f = 0; f < NFIX; f++) {
        const fix *x = &FIXES[f];
        const char *e = in + cur_len;
        int hits = 0;
        // Each match only once: a second copy of the same code would be a different game build.
        for (const char *s = in; s < e; s++) {
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
            free(cur);
            cur = out, in = out, cur_len = o;
            hits++;
            break;
        }
        if (hits) snprintf(applied + strlen(applied), sizeof applied - strlen(applied), "%s%s", applied[0] ? ", " : "", x->name);
    }
    if (!cur) return NULL;
    const char *base = strrchr(url, '/');
    host_log("[gamefix] %s: %s", base ? base + 1 : url, applied);
    *out_len = cur_len;
    return cur;
}
