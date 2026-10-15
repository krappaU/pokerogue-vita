// SPDX-License-Identifier: AGPL-3.0-only
// Remembers i18next translations. PokeRogue looks up thousands of translated strings while it
// builds its screens (35% of the setup frame on the desktop runtime), and rebuilds most of them when
// it returns to the title or starts a run; i18next's lookup is regex-heavy and slow in QuickJS.
//
// The game bundles i18next and doesn't expose it, so the moment i18next creates its Translator
// (`this.translator = new Translator(...)`) is caught with a one-time setter, and
// Translator.prototype.translate gets a cache. Cached: string results of a string key with no
// options or only primitive option values, per language. Cleared when translations are added or
// removed (namespaces load lazily) and when it grows large.
(function (g) {
  "use strict";
  const N = g.__native;
  const cache = new Map();
  let hits = 0, misses = 0;
  const watched = new WeakSet();

  function patch(Translator) {
    if (!Translator || Translator.__pkrCached || typeof Translator.translate !== "function") return;
    Translator.__pkrCached = true;
    const orig = Translator.translate;
    Translator.translate = function (keys, opt, lastKey) {
      if (typeof keys !== "string" || lastKey !== undefined) return orig.apply(this, arguments);
      const store = this.resourceStore;
      if (store && !watched.has(store) && typeof store.on === "function") {
        watched.add(store);
        store.on("added", () => cache.clear());
        store.on("removed", () => cache.clear());
      }
      let k = (this.language || "") + "\u0001" + keys;
      if (opt !== undefined && opt !== null) {
        if (typeof opt !== "object") return orig.apply(this, arguments);
        for (const p in opt) {
          const v = opt[p], t = typeof v;
          if (t === "object" ? v !== null : t === "function" || t === "symbol") return orig.apply(this, arguments);
          k += "\u0001" + p + "\u0002" + t[0] + String(v);
        }
      }
      const hit = cache.get(k);
      if (hit !== undefined) {
        hits++;
        return hit;
      }
      misses++;
      const r = orig.apply(this, arguments);
      if (typeof r === "string") {
        if (cache.size > 30000) cache.clear();
        cache.set(k, r);
      }
      return r;
    };
    N.log("[i18n] translation cache installed");
  }

  Object.defineProperty(Object.prototype, "translator", {
    configurable: true,
    get() { return undefined; },
    set(v) {
      Object.defineProperty(this, "translator", { value: v, writable: true, configurable: true, enumerable: true });
      if (v && typeof v === "object" && typeof v.translate === "function") {
        patch(Object.getPrototypeOf(v));
        delete Object.prototype.translator; // one instance is enough: the class is shared
      }
    },
  });

  g.__rt.i18nStats = () => ({ hits, misses, size: cache.size });
})(globalThis);
