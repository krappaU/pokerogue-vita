// SPDX-License-Identifier: AGPL-3.0-only
// Pokemon sprites unloaded when nothing uses them, the way a garbage collector works: not at once,
// at a new biome or run (the old route's wild Pokemon won't come back soon) and when the JS heap gets
// large (checked each second; a pass at most once a minute, after 20 more sprites were loaded).
//
// Each species the game shows loads a sprite atlas ("pkmn__..." textures: ~88 Phaser frames) and its
// animation, ~0.27 MB of JS on the desktop, and keeps them for the rest of the session; the starter
// select screen loads one for every species the cursor visits. log225 ran the Vita out of memory
// after an hour with ~350 more of them than earlier sessions (docs/phase1-findings.md). The game
// loads every Pokemon sprite through loadPokemonAtlas and makes its animation when missing, and both
// skip what already exists, so a sprite unloaded here is loaded again (from the disk cache) when the
// game next shows that Pokemon.
//
// Unloaded: "pkmn__" atlas textures (from an image, not a canvas) and the animations made from them,
// loaded at least a minute ago, that no object uses: none on any scene's display or update list
// (containers included), in the player's or enemy's party, held directly by the battle scene or a UI
// handler, or among all live sprites (every Sprite's animation state, js/fastpath.js 9), whether it
// shows the texture or plays or waits to play an animation of it; nor any sprite of a Pokemon in
// either party, shown or not (front, back, fusion). Never while a loader is busy.
// Switch: __rt.spriteGc = false.
(function (g) {
  "use strict";
  const N = g.__native, rt = g.__rt;
  const PREFIX = "pkmn__";
  const MIN_AGE_MS = 60000;    // loaded within the last minute: probably about to be shown
  const CHECK_EVERY_MS = 1000; // how often the frame hook looks
  const SETTLE_MS = 2000;      // after a new biome, let its first wave's loads start first
  const PRESSURE_EVERY_MS = 60000;
  const PRESSURE_SHARE = 0.27; // of the fixed heap: 69 of 256 MB on the Vita
  const PRESSURE_NEW_SPRITES = 20;
  const stats = { passes: 0, unloaded: 0, lastMs: 0 };
  const added = new Map(); // texture key -> when it was added (unknown: loaded before the hook)
  let hooked = null, lastCheck = 0, lastBiome, lastArena = null, pending = null, lastPass = 0, loadedSincePass = 0;
  const start = N.now();

  function isGameObject(o) { return o && typeof o === "object" && typeof o.setVisible === "function" && "scene" in o; }

  // The keys of every texture and animation some object uses.
  function inUse(game, roots) {
    const keys = new Set(), seen = new Set();
    const anim = (a) => {
      if (!a) return;
      if (typeof a === "string") { keys.add(a); return; }
      if (typeof a === "object") {
        if (typeof a.key === "string") keys.add(a.key);
        const f = a.frames && a.frames[0];
        if (f && f.textureKey) keys.add(f.textureKey);
      }
    };
    const visit = (o) => {
      if (!o || typeof o !== "object" || seen.has(o)) return;
      seen.add(o);
      if (o.texture && o.texture.key) keys.add(o.texture.key);
      const a = o.anims;
      if (a && typeof a === "object") {
        anim(a.currentAnim), anim(a.nextAnim);
        if (Array.isArray(a.nextAnimsQueue)) a.nextAnimsQueue.forEach(anim);
      }
      if (Array.isArray(o.list)) for (const c of o.list) visit(c);
    };
    for (const sc of game.scene.scenes) {
      const sys = sc.sys;
      if (!sys) continue;
      if (sys.displayList) for (const o of sys.displayList.list) visit(o);
      const ul = sys.updateList;
      if (ul) for (const k of ["_active", "_pending", "_toProcess"]) if (Array.isArray(ul[k])) for (const o of ul[k]) visit(o);
    }
    for (const r of roots) {
      if (Array.isArray(r)) r.forEach(visit);
      else visit(r);
    }
    // every live sprite, on a list or not
    const am = game.anims;
    let states = am && am._pkrAnimStates;
    if (!states && am && am._events) {
      const ev = am._events[Phaser.Animations.Events.REMOVE_ANIMATION];
      states = ev ? (Array.isArray(ev) ? ev : [ev]).map((l) => l.context).filter(Boolean) : [];
    }
    for (const st of states || []) {
      if (!st) continue;
      visit(st.parent);
      anim(st.currentAnim), anim(st.nextAnim);
      if (Array.isArray(st.nextAnimsQueue)) st.nextAnimsQueue.forEach(anim);
    }
    return keys;
  }

  // Objects the battle scene and the UI handlers hold directly (one level, arrays too): an object
  // made but not added to any list yet.
  function heldRoots(s) {
    const roots = [];
    const take = (holder) => {
      for (const k of Object.keys(holder)) {
        const v = holder[k];
        if (isGameObject(v)) roots.push(v);
        else if (Array.isArray(v) && v.length && isGameObject(v[0])) roots.push(v);
      }
    };
    take(s);
    if (s.ui && Array.isArray(s.ui.handlers)) for (const h of s.ui.handlers) if (h) take(h);
    if (typeof s.getPlayerParty === "function") roots.push(s.getPlayerParty());
    if (typeof s.getEnemyParty === "function") roots.push(s.getEnemyParty());
    return roots;
  }

  // Every sprite key a party Pokemon may read by name, shown or not: the fusion palette reads the
  // front and back atlases of both halves with textures.get (calculateFusionPaletteColors), and an
  // own Pokemon on the field shows only its back sprite.
  const SPRITE_KEY_FNS = ["getSpriteKey", "getBattleSpriteKey", "getFusionSpriteKey", "getFusionBattleSpriteKey"];
  const KEY_ARGS = [[], [false], [true], [false, true], [true, true], [true, false]];
  function partyKeys(s, keys) {
    const parties = [];
    if (typeof s.getPlayerParty === "function") parties.push(s.getPlayerParty());
    if (typeof s.getEnemyParty === "function") parties.push(s.getEnemyParty());
    for (const party of parties) for (const p of party || []) {
      if (!p) continue;
      for (const fn of SPRITE_KEY_FNS) {
        if (typeof p[fn] !== "function") continue;
        for (const args of KEY_ARGS) {
          let k;
          try { k = p[fn](...args); } catch (e) { continue; }
          if (typeof k === "string" && k.startsWith(PREFIX)) keys.add(k), keys.add(k.replace(PREFIX, PREFIX + "back__"));
        }
      }
    }
  }

  function loaderBusy(game) {
    for (const sc of game.scene.scenes) {
      const l = sc.sys && sc.sys.load;
      if (!l) continue;
      if ((typeof l.isLoading === "function" && l.isLoading()) || (l.list && l.list.size) || (l.inflight && l.inflight.size) ||
          (l.queue && l.queue.size)) return true;
    }
    return false;
  }

  // One pass. Returns how many were unloaded, or -1 if it couldn't run now.
  function pass(game, reason, minAge = MIN_AGE_MS) {
    const s = game.scene.getScene("battle");
    if (!s || loaderBusy(game)) return -1;
    const t0 = N.now(), tm = game.textures, am = game.anims;
    const used = inUse(game, heldRoots(s));
    partyKeys(s, used);
    const byTexture = new Map(); // texture key -> its animations' keys
    const entries = am && am.anims && am.anims.entries; // Phaser's CustomMap: a plain object
    if (entries) for (const k of Object.keys(entries)) {
      const a = entries[k], f = a && a.frames && a.frames[0], tk = f && f.textureKey;
      if (tk && tk.startsWith(PREFIX)) (byTexture.get(tk) || byTexture.set(tk, []).get(tk)).push(k);
    }
    let total = 0, kept = 0, young = 0, n = 0;
    for (const key of Object.keys(tm.list)) {
      if (!key.startsWith(PREFIX)) continue;
      total++;
      const tex = tm.list[key], src = tex && tex.source && tex.source[0], img = src && src.image;
      if (!img || typeof img.getContext === "function") continue; // a canvas the game drew: not an atlas
      if (used.has(key) || (byTexture.get(key) || []).some((a) => used.has(a))) { kept++; continue; }
      if (t0 - (added.has(key) ? added.get(key) : start) < minAge) { young++; continue; }
      // no object plays them (above), so no animation state needs telling: out of the map quietly
      // (AnimationManager.remove would call every sprite's state, thousands, for each)
      for (const a of byTexture.get(key) || []) { am.anims.delete(a); am.removeMix(a); }
      tm.remove(key);
      added.delete(key);
      n++;
    }
    const ms = N.now() - t0;
    stats.passes++, stats.unloaded += n, stats.lastMs = ms;
    lastPass = N.now(), loadedSincePass = young;
    if (n || reason !== "memory") {
      const [live] = N.jsMemory();
      N.log("[sprites] " + reason + ": unloaded " + n + " of " + total + " Pokemon sprites (" + kept + " in use, " + young +
        " loaded in the last minute), " + ms.toFixed(0) + " ms; JS left by the last collection " + (live / 1048576).toFixed(0) + " MB");
    }
    return n;
  }

  function hook(game) {
    hooked = game;
    game.textures.on(Phaser.Textures.Events.ADD, (key) => {
      if (typeof key === "string" && key.startsWith(PREFIX)) added.set(key, N.now()), loadedSincePass++;
    });
    game.textures.on(Phaser.Textures.Events.REMOVE, (key) => { added.delete(key); });
  }

  function poll() {
    if (rt.spriteGc === false) return;
    const now = N.now();
    if (now - lastCheck < CHECK_EVERY_MS) return;
    lastCheck = now;
    const game = rt.renderer && rt.renderer.game;
    if (!game || !game.textures || !game.scene) return;
    if (hooked !== game) hook(game);
    const s = game.scene.getScene("battle");
    if (!s) return;
    // the biome (biomeId; biomeType in older builds), and a new Arena (a new run, even in the same biome)
    const arena = s.arena, biome = arena ? (arena.biomeId !== undefined ? arena.biomeId : arena.biomeType) : undefined;
    if (arena && biome !== undefined && (biome !== lastBiome || arena !== lastArena)) {
      if (lastArena) pending = { reason: arena !== lastArena ? "new run" : "new biome", since: now };
      lastBiome = biome, lastArena = arena;
    }
    // under memory pressure, only once enough sprites came since the last pass to be worth the walk
    // over every object (late in a run the heap stays large with nothing new to unload)
    if (!pending && now - lastPass > PRESSURE_EVERY_MS && loadedSincePass >= PRESSURE_NEW_SPRITES) {
      const [live, heap] = N.jsMemory();
      const limit = rt.spriteGcLimitMB ? rt.spriteGcLimitMB * 1048576 : heap * PRESSURE_SHARE;
      if (limit && live > limit) pending = { reason: "memory", since: now - SETTLE_MS };
    }
    if (pending && now - pending.since >= SETTLE_MS && pass(game, pending.reason) >= 0) pending = null;
  }

  rt.spriteGcStats = () => Object.assign({}, stats);
  rt.spriteGcNow = (reason, minAge) => { const game = rt.renderer && rt.renderer.game; return game ? pass(game, reason || "asked", minAge) : -1; };
  rt.spriteGcInUse = (game, roots) => inUse(game, roots || []); // tests/phaser
  rt.frameHooks.push(() => { try { poll(); } catch (e) { rt.spriteGc = false; N.log("[sprites] stopped: " + e + "\n" + (e && e.stack)); } });
})(globalThis);
