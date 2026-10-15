// SPDX-License-Identifier: AGPL-3.0-only
// Core browser globals: console, timers, requestAnimationFrame, events, URL, text encoding, base64,
// crypto, and a small Intl polyfill (QuickJS has no Intl). Runs first; later files build on it.
(function (g) {
  "use strict";
  const N = g.__native;
  const rt = (g.__rt = { missing: new Set() });

  // ---------------------------------------------------------------- missing-API tracking
  // Objects created through tracked() log the first access to any property they lack, which is how
  // we find out what the game expects from the browser.
  rt.tracked = function (obj, label) {
    const proto = Object.getPrototypeOf(obj);
    const fallback = new Proxy(proto || {}, {
      get(target, key, receiver) {
        if (key in target) return Reflect.get(target, key, receiver);
        if (typeof key === "string" && !key.startsWith("__") && !rt.missing.has(label + "." + key)) {
          rt.missing.add(label + "." + key);
          N.log("[missing] " + label + "." + key);
        }
        return undefined;
      },
    });
    Object.setPrototypeOf(obj, fallback);
    return obj;
  };

  // ---------------------------------------------------------------- console
  function fmt(v, depth = 0) {
    if (typeof v === "string") return v;
    if (v instanceof Error) return v.stack ? String(v) + "\n" + v.stack : String(v);
    if (v === null || typeof v !== "object") return String(v);
    if (depth > 1) return Array.isArray(v) ? "[Array]" : "[Object]";
    try {
      if (Array.isArray(v)) return "[" + v.slice(0, 20).map((x) => fmt(x, depth + 1)).join(", ") + (v.length > 20 ? ", …" : "") + "]";
      const keys = Object.keys(v).slice(0, 12);
      return (v.constructor && v.constructor.name !== "Object" ? v.constructor.name + " " : "") +
        "{" + keys.map((k) => k + ": " + fmt(v[k], depth + 1)).join(", ") + (Object.keys(v).length > 12 ? ", …" : "") + "}";
    } catch (e) {
      return "[object]";
    }
  }
  function format(args) {
    if (typeof args[0] === "string" && /%[sdoOif]/.test(args[0])) {
      let i = 1;
      const s = args[0].replace(/%[sdoOifc]/g, (m) => (m === "%c" ? (i++, "") : i < args.length ? fmt(args[i++]) : m));
      return [s, ...args.slice(i).map((a) => fmt(a))].join(" ");
    }
    return args.map((a) => fmt(a)).join(" ");
  }
  const log = (level) => (...args) => N.log((level ? level + " " : "") + format(args));
  const timers = new Map();
  g.console = {
    log: log(""), info: log(""), debug: log("[debug]"), warn: log("[warn]"), error: log("[error]"),
    trace: log("[trace]"), table: log("[table]"), dir: log(""), group: log("[group]"), groupCollapsed: log("[group]"),
    groupEnd() {}, assert(c, ...a) { if (!c) log("[assert]")(...a); },
    time(l = "default") { timers.set(l, N.now()); },
    timeEnd(l = "default") { const t = timers.get(l); if (t != null) log("")(l + ": " + (N.now() - t).toFixed(1) + " ms"); timers.delete(l); },
    timeLog(l = "default") { const t = timers.get(l); if (t != null) log("")(l + ": " + (N.now() - t).toFixed(1) + " ms"); },
    count() {}, countReset() {},
  };

  // ---------------------------------------------------------------- the time
  // The console's clock can be hours off; once a server has told the real time (net.c), "now"
  // is corrected by the difference. Dates built from explicit values are unchanged.
  const RealDate = Date;
  const nowMs = () => RealDate.now() + N.clockOffset();
  function PDate(...a) {
    if (!new.target) return new RealDate(nowMs()).toString(); // Date() called as a function
    return a.length ? new RealDate(...a) : new RealDate(nowMs());
  }
  Object.setPrototypeOf(PDate, RealDate);
  PDate.prototype = RealDate.prototype;
  Object.defineProperty(RealDate.prototype, "constructor", { value: PDate, writable: true, configurable: true });
  PDate.now = nowMs;
  g.Date = PDate;

  // ---------------------------------------------------------------- timers and frames
  let timerSeq = 1;
  const pending = new Map(); // id -> {at, fn, args, interval}
  let rafQueue = [], rafSeq = 1;
  const start = N.now();
  g.performance = {
    now: () => N.now() - start,
    timeOrigin: Date.now(),
    mark() {}, measure() {}, getEntriesByName: () => [], getEntriesByType: () => [], clearMarks() {}, clearMeasures() {},
    memory: undefined,
  };
  g.setTimeout = (fn, ms, ...args) => {
    const id = timerSeq++;
    pending.set(id, { at: N.now() + Math.max(0, +ms || 0), fn, args, interval: 0 });
    return id;
  };
  g.setInterval = (fn, ms, ...args) => {
    const id = timerSeq++;
    const iv = Math.max(4, +ms || 0);
    pending.set(id, { at: N.now() + iv, fn, args, interval: iv });
    return id;
  };
  g.clearTimeout = g.clearInterval = (id) => { pending.delete(id); };
  g.setImmediate = (fn, ...args) => g.setTimeout(fn, 0, ...args);
  g.requestAnimationFrame = (fn) => { const id = rafSeq++; rafQueue.push({ id, fn }); return id; };
  g.cancelAnimationFrame = (id) => { rafQueue = rafQueue.filter((r) => r.id !== id); };
  g.requestIdleCallback = (fn) => g.setTimeout(() => fn({ didTimeout: false, timeRemaining: () => 8 }), 1);
  g.cancelIdleCallback = g.clearTimeout;
  g.queueMicrotask = (fn) => { Promise.resolve().then(fn); };

  function report(e) { N.log("[js] uncaught: " + (e && e.stack ? e + "\n" + e.stack : e)); }

  rt.frameHooks = [];
  rt.onFrame = function (nowAbs) {
    const now = nowAbs;
    // Timers due by now, in due order. New timers scheduled meanwhile wait for the next frame.
    const due = [];
    for (const [id, t] of pending) if (t.at <= now) due.push([id, t]);
    due.sort((a, b) => a[1].at - b[1].at);
    for (const [id, t] of due) {
      if (!pending.has(id)) continue;
      if (t.interval) t.at = now + t.interval; else pending.delete(id);
      try { typeof t.fn === "function" ? t.fn(...t.args) : (0, eval)(String(t.fn)); } catch (e) { report(e); }
    }
    for (const h of rt.frameHooks) { try { h(now); } catch (e) { report(e); } }
    const q = rafQueue;
    rafQueue = [];
    const ts = now - start;
    for (const r of q) { try { r.fn(ts); } catch (e) { report(e); } }
  };

  // ---------------------------------------------------------------- events
  class Event {
    constructor(type, init = {}) {
      this.type = type;
      this.bubbles = !!init.bubbles;
      this.cancelable = !!init.cancelable;
      this.composed = !!init.composed;
      this.defaultPrevented = false;
      this.timeStamp = g.performance.now();
      this.target = this.currentTarget = this.srcElement = null;
      this.isTrusted = false;
      this._stop = false;
      this._stopNow = false;
    }
    preventDefault() { if (this.cancelable) this.defaultPrevented = true; }
    stopPropagation() { this._stop = true; }
    stopImmediatePropagation() { this._stop = this._stopNow = true; }
    composedPath() { return this._path || []; }
    get returnValue() { return !this.defaultPrevented; }
  }
  class CustomEvent extends Event { constructor(t, i = {}) { super(t, i); this.detail = i.detail ?? null; } }
  class UIEvent extends Event { constructor(t, i = {}) { super(t, i); this.view = g; this.detail = i.detail || 0; } }
  class KeyboardEvent extends UIEvent {
    constructor(t, i = {}) {
      super(t, i);
      Object.assign(this, { key: i.key || "", code: i.code || "", keyCode: i.keyCode || 0, which: i.keyCode || 0,
        charCode: 0, location: 0, repeat: !!i.repeat, altKey: !!i.altKey, ctrlKey: !!i.ctrlKey,
        metaKey: !!i.metaKey, shiftKey: !!i.shiftKey, isComposing: false });
    }
    getModifierState() { return false; }
  }
  class MouseEvent extends UIEvent {
    constructor(t, i = {}) {
      super(t, i);
      const x = i.clientX || 0, y = i.clientY || 0;
      Object.assign(this, { clientX: x, clientY: y, pageX: x, pageY: y, screenX: x, screenY: y, x, y,
        offsetX: x, offsetY: y, movementX: 0, movementY: 0, button: i.button || 0, buttons: i.buttons || 0,
        altKey: false, ctrlKey: false, metaKey: false, shiftKey: false, relatedTarget: null });
    }
    getModifierState() { return false; }
  }
  class PointerEvent extends MouseEvent {
    constructor(t, i = {}) {
      super(t, i);
      Object.assign(this, { pointerId: i.pointerId || 1, pointerType: i.pointerType || "mouse", isPrimary: true,
        width: 1, height: 1, pressure: i.pressure || 0 });
    }
  }
  class TouchEvent extends UIEvent {
    constructor(t, i = {}) { super(t, i); this.touches = i.touches || []; this.changedTouches = i.changedTouches || []; this.targetTouches = i.targetTouches || []; }
  }
  class WheelEvent extends MouseEvent { constructor(t, i = {}) { super(t, i); this.deltaX = i.deltaX || 0; this.deltaY = i.deltaY || 0; this.deltaZ = 0; this.deltaMode = 0; } }
  class FocusEvent extends UIEvent {}
  class ErrorEvent extends Event { constructor(t, i = {}) { super(t, i); this.message = i.message || ""; this.error = i.error; } }
  class ProgressEvent extends Event { constructor(t, i = {}) { super(t, i); this.lengthComputable = !!i.lengthComputable; this.loaded = i.loaded || 0; this.total = i.total || 0; } }
  class GamepadEvent extends Event { constructor(t, i = {}) { super(t, i); this.gamepad = i.gamepad || null; } }
  class PromiseRejectionEvent extends Event { constructor(t, i = {}) { super(t, i); this.promise = i.promise; this.reason = i.reason; } }

  const listenersKey = Symbol("listeners");
  class EventTarget {
    addEventListener(type, fn, opts) {
      if (!fn) return;
      const map = (this[listenersKey] ||= new Map());
      let list = map.get(type);
      if (!list) map.set(type, (list = []));
      const capture = typeof opts === "boolean" ? opts : !!(opts && opts.capture);
      if (list.some((l) => l.fn === fn && l.capture === capture)) return;
      list.push({ fn, capture, once: !!(opts && opts.once), signal: opts && opts.signal });
      if (opts && opts.signal) opts.signal.addEventListener("abort", () => this.removeEventListener(type, fn, opts));
    }
    removeEventListener(type, fn, opts) {
      const list = this[listenersKey] && this[listenersKey].get(type);
      if (!list) return;
      const capture = typeof opts === "boolean" ? opts : !!(opts && opts.capture);
      const i = list.findIndex((l) => l.fn === fn && l.capture === capture);
      if (i >= 0) list.splice(i, 1);
    }
    // Dispatch along the parent chain (capture, target, bubble). Elements expose parentNode;
    // document's parent is window.
    dispatchEvent(ev) {
      const path = [];
      for (let n = this; n; n = n === g.document ? g : n.parentNode || (n.nodeType ? null : null)) {
        path.push(n);
        if (n === g) break;
      }
      ev.target = ev.srcElement = this;
      ev._path = path;
      const invoke = (node, phase) => {
        const list = node[listenersKey] && node[listenersKey].get(ev.type);
        if (list) {
          ev.currentTarget = node;
          ev.eventPhase = phase;
          for (const l of list.slice()) {
            if (phase === 1 && !l.capture) continue;
            if (phase === 3 && l.capture) continue;
            if (l.once) node.removeEventListener(ev.type, l.fn, l.capture);
            try { typeof l.fn === "function" ? l.fn.call(node, ev) : l.fn.handleEvent(ev); } catch (e) { report(e); }
            if (ev._stopNow) break;
          }
        }
        const handler = node["on" + ev.type];
        if (phase !== 1 && typeof handler === "function" && !ev._stopNow) {
          ev.currentTarget = node;
          try { if (handler.call(node, ev) === false) ev.preventDefault(); } catch (e) { report(e); }
        }
      };
      for (let i = path.length - 1; i > 0 && !ev._stop; i--) invoke(path[i], 1);
      if (!ev._stop) invoke(this, 2);
      if (ev.bubbles) for (let i = 1; i < path.length && !ev._stop; i++) invoke(path[i], 3);
      ev.currentTarget = null;
      return !ev.defaultPrevented;
    }
  }

  Object.assign(g, { Event, CustomEvent, UIEvent, KeyboardEvent, MouseEvent, PointerEvent, TouchEvent, WheelEvent,
    FocusEvent, ErrorEvent, ProgressEvent, GamepadEvent, PromiseRejectionEvent, EventTarget });

  // ---------------------------------------------------------------- AbortController / DOMException
  class DOMException extends Error {
    constructor(message = "", name = "Error") { super(message); this.name = name; }
  }
  class AbortSignal extends EventTarget {
    constructor() { super(); this.aborted = false; this.reason = undefined; this.onabort = null; }
    throwIfAborted() { if (this.aborted) throw this.reason; }
    static abort(reason) { const c = new AbortController(); c.abort(reason); return c.signal; }
    static timeout(ms) { const c = new AbortController(); g.setTimeout(() => c.abort(new DOMException("timeout", "TimeoutError")), ms); return c.signal; }
    static any(signals) { const c = new AbortController(); for (const s of signals) { if (s.aborted) c.abort(s.reason); else s.addEventListener("abort", () => c.abort(s.reason)); } return c.signal; }
  }
  class AbortController {
    constructor() { this.signal = new AbortSignal(); }
    abort(reason = new DOMException("The operation was aborted.", "AbortError")) {
      if (this.signal.aborted) return;
      this.signal.aborted = true;
      this.signal.reason = reason;
      this.signal.dispatchEvent(new Event("abort"));
    }
  }
  Object.assign(g, { DOMException, AbortSignal, AbortController });

  // ---------------------------------------------------------------- text encoding, base64
  class TextEncoder {
    get encoding() { return "utf-8"; }
    encode(s = "") { return new Uint8Array(N.utf8Encode(String(s))); }
    encodeInto(s, dest) { const b = this.encode(s); const n = Math.min(b.length, dest.length); dest.set(b.subarray(0, n)); return { read: s.length, written: n }; }
  }
  class TextDecoder {
    constructor(label = "utf-8") { this.encoding = String(label).toLowerCase(); }
    decode(buf) {
      if (buf == null) return "";
      if (this.encoding !== "utf-8" && this.encoding !== "utf8") {
        const u = ArrayBuffer.isView(buf) ? new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength) : new Uint8Array(buf);
        let s = "";
        for (let i = 0; i < u.length; i++) s += String.fromCharCode(u[i]);
        return s;
      }
      return N.utf8Decode(buf);
    }
  }
  const B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  g.btoa = (s) => {
    s = String(s);
    let out = "";
    for (let i = 0; i < s.length; i += 3) {
      const a = s.charCodeAt(i), b = s.charCodeAt(i + 1), c = s.charCodeAt(i + 2);
      if (a > 255 || b > 255 || c > 255) throw new DOMException("Invalid character", "InvalidCharacterError");
      const n = (a << 16) | ((b || 0) << 8) | (c || 0);
      out += B64[(n >> 18) & 63] + B64[(n >> 12) & 63] + (i + 1 < s.length ? B64[(n >> 6) & 63] : "=") + (i + 2 < s.length ? B64[n & 63] : "=");
    }
    return out;
  };
  g.atob = (s) => {
    s = String(s).replace(/[\s=]/g, "");
    let out = "", buf = 0, bits = 0;
    for (let i = 0; i < s.length; i++) {
      const v = B64.indexOf(s[i]);
      if (v < 0) throw new DOMException("Invalid character", "InvalidCharacterError");
      buf = (buf << 6) | v;
      bits += 6;
      if (bits >= 8) { bits -= 8; out += String.fromCharCode((buf >> bits) & 255); }
    }
    return out;
  };
  Object.assign(g, { TextEncoder, TextDecoder });

  // ---------------------------------------------------------------- URL
  class URLSearchParams {
    constructor(init = "") {
      this._list = [];
      if (typeof init === "string") {
        for (const part of init.replace(/^\?/, "").split("&")) {
          if (!part) continue;
          const i = part.indexOf("=");
          const dec = (x) => decodeURIComponent(x.replace(/\+/g, " "));
          this._list.push(i < 0 ? [dec(part), ""] : [dec(part.slice(0, i)), dec(part.slice(i + 1))]);
        }
      } else if (init && typeof init === "object") {
        const entries = Symbol.iterator in init ? init : Object.entries(init);
        for (const [k, v] of entries) this._list.push([String(k), String(v)]);
      }
    }
    append(k, v) { this._list.push([String(k), String(v)]); this._update(); }
    delete(k) { this._list = this._list.filter(([x]) => x !== k); this._update(); }
    get(k) { const e = this._list.find(([x]) => x === k); return e ? e[1] : null; }
    getAll(k) { return this._list.filter(([x]) => x === k).map(([, v]) => v); }
    has(k) { return this._list.some(([x]) => x === k); }
    set(k, v) {
      const i = this._list.findIndex(([x]) => x === k);
      if (i < 0) this._list.push([String(k), String(v)]);
      else { this._list[i][1] = String(v); this._list = this._list.filter(([x], j) => x !== k || j === i); }
      this._update();
    }
    sort() { this._list.sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0)); this._update(); }
    forEach(fn, thisArg) { for (const [k, v] of this._list) fn.call(thisArg, v, k, this); }
    keys() { return this._list.map(([k]) => k)[Symbol.iterator](); }
    values() { return this._list.map(([, v]) => v)[Symbol.iterator](); }
    entries() { return this._list.map((e) => [e[0], e[1]])[Symbol.iterator](); }
    [Symbol.iterator]() { return this.entries(); }
    get size() { return this._list.length; }
    toString() { const enc = (x) => encodeURIComponent(x).replace(/%20/g, "+"); return this._list.map(([k, v]) => enc(k) + "=" + enc(v)).join("&"); }
    _update() { if (this._url) this._url._search = this._list.length ? "?" + this.toString() : ""; }
  }

  const URL_RE = /^([a-zA-Z][a-zA-Z0-9+.-]*:)(?:\/\/(?:([^:@/]*)(?::([^@/]*))?@)?([^:/?#]*)(?::(\d+))?)?([^?#]*)(\?[^#]*)?(#.*)?$/;
  const blobs = new Map();
  let blobSeq = 1;
  class URL {
    constructor(url, base) {
      url = String(url);
      if (base !== undefined && !/^[a-zA-Z][a-zA-Z0-9+.-]*:/.test(url)) url = N.resolveURL(String(base instanceof URL ? base.href : base), url);
      const m = URL_RE.exec(url);
      if (!m) throw new TypeError("Invalid URL: " + url);
      this._protocol = m[1].toLowerCase();
      this.username = m[2] || "";
      this.password = m[3] || "";
      this._hostname = (m[4] || "").toLowerCase();
      this._port = m[5] || "";
      this._pathname = m[6] || (m[4] !== undefined ? "/" : "");
      this._search = m[7] && m[7] !== "?" ? m[7] : "";
      this._hash = m[8] && m[8] !== "#" ? m[8] : "";
      this._params = null;
    }
    get protocol() { return this._protocol; }
    set protocol(v) { this._protocol = String(v).replace(/:?$/, ":"); }
    get hostname() { return this._hostname; }
    set hostname(v) { this._hostname = String(v); }
    get port() { return this._port; }
    set port(v) { this._port = String(v); }
    get host() { return this._hostname + (this._port ? ":" + this._port : ""); }
    set host(v) { const [h, p] = String(v).split(":"); this._hostname = h; this._port = p || ""; }
    get origin() { return /^(https?|wss?):$/.test(this._protocol) ? this._protocol + "//" + this.host : "null"; }
    get pathname() { return this._pathname; }
    set pathname(v) { v = String(v); this._pathname = v.startsWith("/") ? v : "/" + v; }
    get search() { return this._search; }
    set search(v) { v = String(v); this._search = v && v !== "?" ? (v.startsWith("?") ? v : "?" + v) : ""; this._params = null; }
    get hash() { return this._hash; }
    set hash(v) { v = String(v); this._hash = v && v !== "#" ? (v.startsWith("#") ? v : "#" + v) : ""; }
    get searchParams() {
      if (!this._params) { this._params = new URLSearchParams(this._search); this._params._url = this; }
      return this._params;
    }
    get href() {
      const auth = this.username ? this.username + (this.password ? ":" + this.password : "") + "@" : "";
      const slashes = this._hostname || /^(https?|wss?|file|ftp):$/.test(this._protocol) ? "//" : "";
      return this._protocol + slashes + auth + this.host + this._pathname + this._search + this._hash;
    }
    set href(v) { Object.assign(this, new URL(v)); }
    toString() { return this.href; }
    toJSON() { return this.href; }
    static canParse(u, b) { try { new URL(u, b); return true; } catch { return false; } }
    static parse(u, b) { try { return new URL(u, b); } catch { return null; } }
    static createObjectURL(blob) { const id = "blob:" + g.location.origin + "/" + (blobSeq++).toString(16); blobs.set(id, blob); return id; }
    static revokeObjectURL(id) { blobs.delete(id); }
  }
  rt.blobs = blobs;
  Object.assign(g, { URL, URLSearchParams, webkitURL: URL });

  // ---------------------------------------------------------------- crypto
  function randomBytes(arr) {
    arr.set(new Uint8Array(N.randomBytes(arr.length)));
    return arr;
  }
  g.crypto = {
    getRandomValues(a) {
      const u8 = new Uint8Array(a.buffer, a.byteOffset, a.byteLength);
      randomBytes(u8);
      return a;
    },
    randomUUID() {
      const b = randomBytes(new Uint8Array(16));
      b[6] = (b[6] & 0x0f) | 0x40;
      b[8] = (b[8] & 0x3f) | 0x80;
      const h = [...b].map((x) => x.toString(16).padStart(2, "0")).join("");
      return h.slice(0, 8) + "-" + h.slice(8, 12) + "-" + h.slice(12, 16) + "-" + h.slice(16, 20) + "-" + h.slice(20);
    },
    subtle: undefined,
  };

  if (typeof g.structuredClone !== "function") {
    g.structuredClone = function clone(v, seen = new Map()) {
      if (v === null || typeof v !== "object") return v;
      if (seen.has(v)) return seen.get(v);
      if (v instanceof Date) return new Date(v.getTime());
      if (v instanceof RegExp) return new RegExp(v.source, v.flags);
      if (ArrayBuffer.isView(v)) return v.slice();
      if (v instanceof ArrayBuffer) return v.slice(0);
      if (v instanceof Map) { const m = new Map(); seen.set(v, m); for (const [k, x] of v) m.set(clone(k, seen), clone(x, seen)); return m; }
      if (v instanceof Set) { const s = new Set(); seen.set(v, s); for (const x of v) s.add(clone(x, seen)); return s; }
      const out = Array.isArray(v) ? [] : {};
      seen.set(v, out);
      for (const k of Object.keys(v)) out[k] = clone(v[k], seen);
      return out;
    };
  }

  // ---------------------------------------------------------------- Intl (minimal, English-centric)
  if (typeof g.Intl === "undefined") {
    const group = (s) => s.replace(/\B(?=(\d{3})+(?!\d))/g, ",");
    class NumberFormat {
      constructor(locale, opts = {}) { this.opts = opts; this.locale = Array.isArray(locale) ? locale[0] : locale || "en-US"; }
      format(n) {
        n = Number(n);
        if (!isFinite(n)) return String(n);
        const o = this.opts;
        let s;
        if (o.maximumFractionDigits !== undefined || o.minimumFractionDigits !== undefined) {
          const max = o.maximumFractionDigits ?? Math.max(o.minimumFractionDigits || 0, 3);
          s = n.toFixed(max);
          if ((o.minimumFractionDigits ?? 0) < max) s = s.replace(/(\.\d*?)0+$/, "$1").replace(/\.$/, "");
        } else {
          s = String(Math.round(n * 1000) / 1000);
        }
        const neg = s.startsWith("-");
        if (neg) s = s.slice(1);
        const [i, f] = s.split(".");
        let out = (o.useGrouping === false ? i : group(i)) + (f ? "." + f : "");
        if (o.style === "percent") out = group(String(Math.round(n * 100))) + "%";
        if (o.style === "currency") out = (o.currency === "USD" ? "$" : (o.currency || "") + " ") + out;
        return (neg ? "-" : "") + out;
      }
      formatToParts(n) { return [{ type: "integer", value: this.format(n) }]; }
      resolvedOptions() { return { locale: this.locale, ...this.opts }; }
      static supportedLocalesOf(l) { return Array.isArray(l) ? l : [l]; }
    }
    class PluralRules {
      constructor(locale, opts = {}) { this.type = opts.type || "cardinal"; this.locale = Array.isArray(locale) ? locale[0] : locale || "en"; }
      select(n) {
        n = Math.abs(Number(n));
        if (this.type === "ordinal") {
          const t = n % 10, h = n % 100;
          if (t === 1 && h !== 11) return "one";
          if (t === 2 && h !== 12) return "two";
          if (t === 3 && h !== 13) return "few";
          return "other";
        }
        return n === 1 ? "one" : "other";
      }
      resolvedOptions() {
        return { locale: this.locale, type: this.type,
          pluralCategories: this.type === "ordinal" ? ["one", "two", "few", "other"] : ["one", "other"] };
      }
      static supportedLocalesOf(l) { return Array.isArray(l) ? l : [l]; }
    }
    class DateTimeFormat {
      constructor(locale, opts = {}) { this.opts = opts; this.locale = Array.isArray(locale) ? locale[0] : locale || "en-US"; }
      format(d = new Date()) {
        d = new Date(d);
        const p = (x) => String(x).padStart(2, "0");
        return `${d.getMonth() + 1}/${d.getDate()}/${d.getFullYear()}` +
          (this.opts.hour ? `, ${p(d.getHours())}:${p(d.getMinutes())}` : "");
      }
      formatToParts(d) { return [{ type: "literal", value: this.format(d) }]; }
      resolvedOptions() { return { locale: this.locale, timeZone: "UTC", calendar: "gregory", numberingSystem: "latn", ...this.opts }; }
      static supportedLocalesOf(l) { return Array.isArray(l) ? l : [l]; }
    }
    class Collator {
      constructor(locale, opts = {}) { this.opts = opts; }
      compare(a, b) { a = String(a); b = String(b); if (this.opts.sensitivity === "base") { a = a.toLowerCase(); b = b.toLowerCase(); } return a < b ? -1 : a > b ? 1 : 0; }
      resolvedOptions() { return { locale: "en", ...this.opts }; }
    }
    class RelativeTimeFormat {
      format(v, unit) { unit = String(unit).replace(/s$/, ""); const n = Math.abs(v); const u = n === 1 ? unit : unit + "s"; return v < 0 ? `${n} ${u} ago` : `in ${n} ${u}`; }
    }
    class ListFormat { constructor(l, o = {}) { this.o = o; } format(list) { list = [...list]; if (list.length < 2) return list.join(""); return list.slice(0, -1).join(", ") + (this.o.type === "disjunction" ? " or " : " and ") + list[list.length - 1]; } }
    class Segmenter { segment(s) { return [...String(s)].map((c, i) => ({ segment: c, index: i, input: s })); } }
    class Locale { constructor(tag) { this.baseName = String(tag); const [l, r] = this.baseName.split("-"); this.language = l; this.region = r; } toString() { return this.baseName; } }
    g.Intl = { NumberFormat, PluralRules, DateTimeFormat, Collator, RelativeTimeFormat, ListFormat, Segmenter, Locale,
      getCanonicalLocales: (l) => (Array.isArray(l) ? l : l ? [l] : []), supportedValuesOf: () => [] };
    const nf = new NumberFormat("en-US");
    Number.prototype.toLocaleString = function (locale, opts) { return opts ? new NumberFormat(locale, opts).format(this) : nf.format(this); };
  }
})(globalThis);
