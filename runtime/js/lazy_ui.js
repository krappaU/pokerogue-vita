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
// localStorage PKR_LAZY_UI=0 turns this off.
(function (g) {
  "use strict";
  const P = g.Phaser;
  if (!P) return;
  try { if (g.localStorage.getItem("PKR_LAZY_UI") === "0") return; } catch (e) { /* storage unavailable: keep going */ }
  const DEFER = new Set(["StarterSelectUiHandler", "PokedexUiHandler", "PokedexPageUiHandler", "TestDialogueUiHandler",
                         "EggSummaryUiHandler"]);
  const KEEP = new Set(["constructor", "setup", "destroy"]);
  let deferredCount = 0, builtCount = 0;

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
        if (h && h.constructor && DEFER.has(h.constructor.name) && typeof h.setup === "function") defer(h, this);
      }
    }
    return origSetName.apply(this, arguments);
  };
  g.__rt.lazyUiStats = () => ({ deferred: deferredCount, built: builtCount });
})(globalThis);
