// SPDX-License-Identifier: AGPL-3.0-only
// Builds some of PokeRogue's UI screens when they are first used instead of at startup.
//
// The game's UI creates ~48 screen handlers and calls setup() on all of them whenever it starts or
// returns to the title; a few build hundreds of objects for screens a player may never open in a
// session (starter select: one container per species; the Pokedex; a developer dialogue tester).
// On the Vita that was about half of the 30-40 s screen rebuilds. Here those handlers' setup() only
// notes where their objects would have gone; the real setup runs the first time anything calls a
// method on the handler (show, processInput, ...), and the objects it creates are moved back to
// the position they would have had (the UI container's child order is the drawing order).
// Handlers are picked by class name; if the game renames them, nothing is deferred.
//
// Second, opening the starter select or the Pokedex resets the filter bar: every dropdown's
// reset (and every option toggle of the "caught" filter) calls the screen's change callback, which
// re-filters and re-sorts all species, about 20 full passes; show() then runs it once more. While
// resetFilters() runs, the dropdowns' change callbacks are held back and the last one runs once at
// the end, so the screen ends in the same state.
// localStorage PKR_LAZY_UI=0 turns both off.
(function (g) {
  "use strict";
  const P = g.Phaser;
  if (!P) return;
  try { if (g.localStorage.getItem("PKR_LAZY_UI") === "0") return; } catch (e) { /* storage unavailable: keep going */ }
  const DEFER = new Set(["StarterSelectUiHandler", "PokedexUiHandler", "PokedexPageUiHandler", "TestDialogueUiHandler",
                         "EggSummaryUiHandler"]);
  const KEEP = new Set(["constructor", "setup", "destroy"]);
  const COALESCE = new Set(["StarterSelectUiHandler", "PokedexUiHandler"]);
  let deferredCount = 0, builtCount = 0, heldBack = 0;

  // Patches the class's resetFilters (once): dropdown change callbacks during it run once, at the end.
  function coalesceFilterResets(proto) {
    if (!proto || proto._pkrCoalesced || typeof proto.resetFilters !== "function") return;
    proto._pkrCoalesced = true;
    const orig = proto.resetFilters;
    proto.resetFilters = function () {
      const dds = this.filterBar && Array.isArray(this.filterBar.dropDowns) ? this.filterBar.dropDowns : null;
      if (!dds || g.__rt.coalesceFilters === false) return orig.apply(this, arguments);
      const saved = dds.map((d) => d.onChange);
      let last = null;
      dds.forEach((d, i) => {
        const f = saved[i];
        if (typeof f === "function") d.onChange = function () { heldBack++; last = [f, this, arguments]; };
      });
      let r;
      try { r = orig.apply(this, arguments); } finally { dds.forEach((d, i) => { d.onChange = saved[i]; }); }
      if (last) { heldBack--; last[0].apply(last[1], last[2]); }
      return r;
    };
  }

  // Moves the objects added to `list` since `before` so that they follow `anchor` (null: the start).
  function restoreOrder(list, before, anchor) {
    const added = list.filter((o) => !before.has(o));
    if (!added.length) return;
    const rest = list.filter((o) => before.has(o));
    let at = anchor ? rest.indexOf(anchor) + 1 : 0;
    if (anchor && at === 0) at = rest.length; // anchor gone: leave them on top
    rest.splice(at, 0, ...added);
    for (let i = 0; i < rest.length; i++) list[i] = rest[i];
    list.length = rest.length;
  }

  function defer(h, ui) {
    const realSetup = h.setup;
    const names = new Set();
    for (let p = Object.getPrototypeOf(h); p && p !== Object.prototype; p = Object.getPrototypeOf(p)) {
      for (const k of Object.getOwnPropertyNames(p)) {
        if (KEEP.has(k) || names.has(k)) continue;
        const d = Object.getOwnPropertyDescriptor(p, k);
        if (d && typeof d.value === "function") names.add(k);
      }
    }
    let state = 0; // 0: setup not reached yet, 1: deferred, 2: built
    let uiAnchor = null, dlAnchor = null, dl = null;
    function unwrap() {
      for (const k of names) delete h[k];
      delete h.setup;
    }
    function ensure() {
      if (state === 2) return;
      unwrap();
      if (state === 0) { state = 2; return; } // used before the UI's setup reached it: as before
      state = 2;
      builtCount++;
      const uiBefore = new Set(ui.list), dlBefore = dl ? new Set(dl.list) : null;
      realSetup.call(h);
      restoreOrder(ui.list, uiBefore, uiAnchor);
      if (dl) restoreOrder(dl.list, dlBefore, dlAnchor);
    }
    h.setup = function () {
      uiAnchor = ui.list.length ? ui.list[ui.list.length - 1] : null;
      dl = ui.scene && ui.scene.sys && ui.scene.sys.displayList;
      dlAnchor = dl && dl.list.length ? dl.list[dl.list.length - 1] : null;
      state = 1;
      deferredCount++;
    };
    for (const k of names) {
      h[k] = function () {
        ensure();
        return h[k].apply(this, arguments);
      };
    }
  }

  // UI.setup() starts with this.setName("ui-<mode>"), then calls setup() on every handler.
  const GO = P.GameObjects.GameObject.prototype, origSetName = GO.setName;
  GO.setName = function (name) {
    if (typeof name === "string" && name.startsWith("ui-") && Array.isArray(this.handlers) && !this._pkrLazyUi) {
      this._pkrLazyUi = true;
      for (const h of this.handlers) {
        if (!h || !h.constructor) continue;
        if (COALESCE.has(h.constructor.name)) coalesceFilterResets(Object.getPrototypeOf(h));
        if (DEFER.has(h.constructor.name) && typeof h.setup === "function") defer(h, this);
      }
    }
    return origSetName.apply(this, arguments);
  };
  g.__rt.lazyUiStats = () => ({ deferred: deferredCount, built: builtCount, heldBack });
})(globalThis);
