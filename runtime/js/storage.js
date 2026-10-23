// SPDX-License-Identifier: AGPL-3.0-only
// localStorage / sessionStorage and document.cookie, persisted as JSON files in the data dir.
// Writes are batched: at most once per second, and the files are replaced atomically.
(function (g) {
  "use strict";
  const N = g.__native, rt = g.__rt;

  function load(file) {
    try { const s = N.readFile(file); return s ? JSON.parse(s) : {}; } catch (e) { N.log("[storage] " + file + " unreadable: " + e); return {}; }
  }
  const dirty = new Set();
  let lastFlush = 0;
  const writers = {};
  // The files are written by a thread of the host's (writeFileLater), not this one: on the Vita a
  // write took 300 ms. A forced flush (before a restart) waits until they are on the card.
  const write = N.writeFileLater || N.writeFile;
  function flush(force) {
    const now = N.now();
    if (!force && now - lastFlush < 1000) return;
    lastFlush = now;
    for (const f of dirty) if (!write(f, JSON.stringify(writers[f]()))) N.log("[storage] failed to write " + f);
    dirty.clear();
    if (force && N.waitWrites) N.waitWrites();
  }
  rt.frameHooks.push(() => { if (dirty.size) flush(false); });
  rt.flushStorage = () => flush(true);

  function makeStorage(file) {
    const data = file ? new Map(Object.entries(load(file))) : new Map();
    if (file) writers[file] = () => Object.fromEntries(data);
    const touch = () => { if (file) dirty.add(file); };
    const api = {
      getItem(k) { k = String(k); return data.has(k) ? data.get(k) : null; },
      setItem(k, v) {
        k = String(k);
        data.set(k, String(v));
        touch();
        if (file && k.startsWith("sessionData") && N.gcAtSave) N.gcAtSave(); // the run saved (main.c gc_at_save)
      },
      removeItem(k) { data.delete(String(k)); touch(); },
      clear() { data.clear(); touch(); },
      key(i) { return [...data.keys()][i] ?? null; },
      get length() { return data.size; },
    };
    // Property-style access (localStorage.foo) as in browsers.
    return new Proxy(api, {
      get(t, k) { if (k in t) return t[k]; return typeof k === "string" && data.has(k) ? data.get(k) : undefined; },
      set(t, k, v) { t.setItem(k, v); return true; },
      deleteProperty(t, k) { t.removeItem(k); return true; },
      has(t, k) { return k in t || data.has(k); },
      ownKeys() { return [...data.keys()]; },
      getOwnPropertyDescriptor(t, k) { return data.has(k) ? { enumerable: true, configurable: true, value: data.get(k) } : undefined; },
    });
  }
  // The offline game (local mode) keeps its saves in local storage: keep them apart from the
  // online game's settings.
  const offline = String(N.baseURL || "").startsWith("https://offline.invalid");
  g.localStorage = makeStorage(offline ? "localStorage-offline.json" : "localStorage.json");
  g.sessionStorage = makeStorage(null);
  g.Storage = function Storage() {};

  // ---------------------------------------------------------------- cookies
  // { name: { value, expires (ms epoch or 0 = session), domain } }
  const jar = load("cookies.json");
  writers["cookies.json"] = () => {
    const out = {};
    for (const [k, c] of Object.entries(jar)) if (c.expires) out[k] = c; // session cookies are not saved
    return out;
  };
  function expire() {
    const now = Date.now();
    for (const [k, c] of Object.entries(jar)) if (c.expires && c.expires <= now) delete jar[k];
  }
  Object.defineProperty(g.document, "cookie", {
    get() { expire(); return Object.entries(jar).map(([k, c]) => k + "=" + c.value).join("; "); },
    set(str) {
      const parts = String(str).split(";").map((s) => s.trim());
      const [nv, ...attrs] = parts;
      const i = nv.indexOf("=");
      if (i < 0) return;
      const name = nv.slice(0, i).trim(), value = nv.slice(i + 1).trim();
      let expires = 0, domain = "";
      for (const a of attrs) {
        const [k, v = ""] = a.split("=");
        const key = k.trim().toLowerCase();
        if (key === "expires") { const t = Date.parse(v); if (!isNaN(t)) expires = t; }
        else if (key === "max-age") expires = Date.now() + (+v) * 1000;
        else if (key === "domain") domain = v.trim();
      }
      if (expires && expires <= Date.now()) delete jar[name];
      else jar[name] = { value, expires, domain };
      dirty.add("cookies.json");
    },
    configurable: true,
  });
  // Cookie header for a request: only to the page's own site (pokerogue.net and its subdomains).
  const site = g.location.hostname.split(".").slice(-2).join(".");
  rt.cookieHeader = (url) => {
    let host;
    try { host = new URL(url).hostname; } catch { return ""; }
    if (host !== site && !host.endsWith("." + site)) return "";
    expire();
    return Object.entries(jar).map(([k, c]) => k + "=" + c.value).join("; ");
  };
})(globalThis);
