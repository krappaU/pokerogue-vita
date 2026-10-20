// SPDX-License-Identifier: AGPL-3.0-only
// fetch, XMLHttpRequest, Headers, Request, Response, Blob, File, FileReader, FormData,
// on the native HTTP client (src/net.c). Cookies are attached here from the document's jar.
(function (g) {
  "use strict";
  const N = g.__native, rt = g.__rt;

  // ---------------------------------------------------------------- Blob / File
  class Blob {
    constructor(parts = [], opts = {}) {
      this.type = String(opts.type || "").toLowerCase();
      const chunks = parts.map((p) => {
        if (p instanceof Blob) return p._bytes;
        if (p instanceof ArrayBuffer) return new Uint8Array(p);
        if (ArrayBuffer.isView(p)) return new Uint8Array(p.buffer, p.byteOffset, p.byteLength);
        return new Uint8Array(N.utf8Encode(String(p)));
      });
      const size = chunks.reduce((n, c) => n + c.length, 0);
      this._bytes = new Uint8Array(size);
      let o = 0;
      for (const c of chunks) { this._bytes.set(c, o); o += c.length; }
    }
    get size() { return this._bytes.length; }
    arrayBuffer() { return Promise.resolve(this._bytes.slice().buffer); }
    bytes() { return Promise.resolve(this._bytes.slice()); }
    text() { return Promise.resolve(N.utf8Decode(this._bytes)); }
    slice(start = 0, end = this.size, type = "") { const b = new Blob([], { type }); b._bytes = this._bytes.slice(start, end); return b; }
    stream() { throw new TypeError("Blob.stream is not supported"); }
  }
  class File extends Blob {
    constructor(parts, name, opts = {}) { super(parts, opts); this.name = String(name); this.lastModified = opts.lastModified || Date.now(); }
  }
  class FileReader extends EventTarget {
    constructor() { super(); this.readyState = 0; this.result = null; this.error = null; }
    _read(blob, conv) {
      this.readyState = 1;
      g.setTimeout(() => {
        this.result = conv(blob._bytes);
        this.readyState = 2;
        this.dispatchEvent(new ProgressEvent("load"));
        this.dispatchEvent(new ProgressEvent("loadend"));
      }, 0);
    }
    readAsArrayBuffer(b) { this._read(b, (u) => u.slice().buffer); }
    readAsText(b) { this._read(b, (u) => N.utf8Decode(u)); }
    readAsDataURL(b) {
      this._read(b, (u) => {
        let s = "";
        for (let i = 0; i < u.length; i++) s += String.fromCharCode(u[i]);
        return "data:" + (b.type || "application/octet-stream") + ";base64," + btoa(s);
      });
    }
    readAsBinaryString(b) { this._read(b, (u) => { let s = ""; for (let i = 0; i < u.length; i++) s += String.fromCharCode(u[i]); return s; }); }
    abort() {}
  }
  class FormData {
    constructor() { this._e = []; }
    append(k, v) { this._e.push([String(k), v]); }
    set(k, v) { this.delete(k); this.append(k, v); }
    get(k) { const e = this._e.find((x) => x[0] === k); return e ? e[1] : null; }
    getAll(k) { return this._e.filter((x) => x[0] === k).map((x) => x[1]); }
    has(k) { return this._e.some((x) => x[0] === k); }
    delete(k) { this._e = this._e.filter((x) => x[0] !== k); }
    entries() { return this._e[Symbol.iterator](); }
    [Symbol.iterator]() { return this.entries(); }
  }

  // ---------------------------------------------------------------- Headers
  class Headers {
    constructor(init) {
      this._m = new Map();
      if (init instanceof Headers) init.forEach((v, k) => this.append(k, v));
      else if (Array.isArray(init)) for (const [k, v] of init) this.append(k, v);
      else if (init) for (const k of Object.keys(init)) this.append(k, init[k]);
    }
    append(k, v) { k = String(k).toLowerCase(); const old = this._m.get(k); this._m.set(k, old != null ? old + ", " + v : String(v)); }
    set(k, v) { this._m.set(String(k).toLowerCase(), String(v)); }
    get(k) { return this._m.get(String(k).toLowerCase()) ?? null; }
    has(k) { return this._m.has(String(k).toLowerCase()); }
    delete(k) { this._m.delete(String(k).toLowerCase()); }
    forEach(fn, t) { for (const [k, v] of this._m) fn.call(t, v, k, this); }
    keys() { return this._m.keys(); }
    values() { return this._m.values(); }
    entries() { return this._m.entries(); }
    getSetCookie() { return []; }
    [Symbol.iterator]() { return this._m.entries(); }
  }
  function parseHeaders(text) {
    const h = new Headers();
    let status = 0, statusText = "";
    for (const line of String(text).split(/\r?\n/)) {
      const m = /^HTTP\/[\d.]+\s+(\d+)\s*(.*)$/.exec(line);
      if (m) { status = +m[1]; statusText = m[2]; continue; }
      const i = line.indexOf(":");
      if (i > 0) h.append(line.slice(0, i).trim(), line.slice(i + 1).trim());
    }
    return { headers: h, status, statusText };
  }

  // ---------------------------------------------------------------- body helpers
  function bodyBytes(body) {
    if (body == null) return null;
    if (typeof body === "string") return body;
    if (body instanceof Blob) return body._bytes;
    if (body instanceof ArrayBuffer || ArrayBuffer.isView(body)) return body;
    if (body instanceof URLSearchParams) return body.toString();
    if (body instanceof FormData) return new URLSearchParams(body._e.map(([k, v]) => [k, String(v)])).toString();
    return String(body);
  }
  function contentTypeFor(body) {
    if (typeof body === "string") return "text/plain;charset=UTF-8";
    if (body instanceof URLSearchParams || body instanceof FormData) return "application/x-www-form-urlencoded;charset=UTF-8";
    if (body instanceof Blob && body.type) return body.type;
    return null;
  }

  // Cookies the game set via document.cookie go with requests to the page's own site.
  function withCookies(url, flat) {
    const c = rt.cookieHeader ? rt.cookieHeader(url) : "";
    if (c && !flat.some((x, i) => i % 2 === 0 && x.toLowerCase() === "cookie")) flat.push("Cookie", c);
    return flat;
  }

  const GET_RETRIES = 2;

  // Which battle background the game loads, for bug reports ("the bug was on one of the maps").
  let lastArena = "";
  function noteArena(url) {
    const m = /\/images\/arenas\/([a-z0-9_]+?)_bg\b/.exec(url);
    if (m && m[1] !== lastArena) N.log("[arena] " + (lastArena = m[1]));
  }

  // Low-level request: resolves {status, statusText, headers, body: ArrayBuffer, url}.
  function request(method, url, headers, body, signal) {
    noteArena(url);
    return new Promise((resolve, reject) => {
      if (url.startsWith("blob:") || url.startsWith("data:")) {
        rt.fetchBytes(url).then((ab) => resolve({ status: 200, statusText: "OK", headers: new Headers(), body: ab, url }), reject);
        return;
      }
      const flat = [];
      headers.forEach((v, k) => flat.push(k, v));
      withCookies(url, flat);
      let done = false, id = 0, attempt = 0;
      // A GET that fails at the transport level (timeout with 0 bytes, reset) is repeated: the game
      // awaits every asset, and one lost download left a wave-60 load stuck for good (0.42 log).
      const retries = method === "GET" || method === "HEAD" ? GET_RETRIES : 0;
      const send = () => {
        id = N.http(method, url, flat, bodyBytes(body), (status, headerText, ab, err, finalURL) => {
          if (done) return;
          if (err && attempt < retries) {
            attempt++;
            N.log("[net] retry " + attempt + "/" + retries + ": " + url);
            g.setTimeout(() => { if (!done) send(); }, 500 * attempt);
            return;
          }
          done = true;
          if (err) return reject(new TypeError("Failed to fetch: " + err));
          const p = parseHeaders(headerText);
          resolve({ status, statusText: p.statusText, headers: p.headers, body: ab, url: finalURL });
        });
      };
      send();
      if (signal) {
        if (signal.aborted) { done = true; N.httpAbort(id); reject(signal.reason); return; }
        signal.addEventListener("abort", () => { if (!done) { done = true; N.httpAbort(id); reject(signal.reason); } });
      }
    });
  }

  // ---------------------------------------------------------------- fetch
  class Body {
    _consume() {
      if (this.bodyUsed) return Promise.reject(new TypeError("Body has already been consumed"));
      this.bodyUsed = true;
      return Promise.resolve(this._body || new ArrayBuffer(0));
    }
    arrayBuffer() { return this._consume(); }
    bytes() { return this._consume().then((b) => new Uint8Array(b)); }
    text() { return this._consume().then((b) => N.utf8Decode(b)); }
    json() { return this.text().then((t) => JSON.parse(t)); }
    blob() { return this._consume().then((b) => new Blob([b], { type: this.headers.get("content-type") || "" })); }
    formData() { return Promise.reject(new TypeError("formData() is not supported")); }
    get body() { return null; }
  }
  class Request extends Body {
    constructor(input, init = {}) {
      super();
      const base = input instanceof Request ? input : null;
      this.url = base ? base.url : new URL(String(input), g.location.href).href;
      this.method = String(init.method || (base && base.method) || "GET").toUpperCase();
      this.headers = new Headers(init.headers || (base && base.headers));
      this._body = init.body !== undefined ? init.body : base ? base._body : null;
      this.signal = init.signal || (base && base.signal) || null;
      this.credentials = init.credentials || "same-origin";
      this.mode = init.mode || "cors";
      this.cache = init.cache || "default";
      this.redirect = init.redirect || "follow";
      this.referrer = "about:client";
      this.bodyUsed = false;
    }
    clone() { return new Request(this); }
  }
  class Response extends Body {
    constructor(body = null, init = {}) {
      super();
      this.status = init.status ?? 200;
      this.statusText = init.statusText ?? "";
      this.headers = init.headers instanceof Headers ? init.headers : new Headers(init.headers);
      this.ok = this.status >= 200 && this.status < 300;
      this.redirected = false;
      this.type = "basic";
      this.url = init.url || "";
      const b = bodyBytes(body);
      this._body = b == null ? null : typeof b === "string" ? N.utf8Encode(b) : b instanceof ArrayBuffer ? b : b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength);
      this.bodyUsed = false;
    }
    clone() { const r = new Response(this._body, this); r.url = this.url; return r; }
    static json(data, init = {}) { const h = new Headers(init.headers); h.set("content-type", "application/json"); return new Response(JSON.stringify(data), { ...init, headers: h }); }
    static error() { const r = new Response(null, { status: 0 }); r.type = "error"; return r; }
    static redirect(url, status = 302) { return new Response(null, { status, headers: { location: url } }); }
  }

  g.fetch = function (input, init = {}) {
    const req = new Request(input, init);
    const ct = contentTypeFor(req._body);
    if (ct && !req.headers.has("content-type")) req.headers.set("content-type", ct);
    return request(req.method, req.url, req.headers, req._body, req.signal).then((r) => {
      const res = new Response(null, { status: r.status, statusText: r.statusText, headers: r.headers, url: r.url });
      res._body = r.body;
      res.redirected = r.url !== req.url;
      return res;
    });
  };

  // ---------------------------------------------------------------- XMLHttpRequest
  class XMLHttpRequestEventTarget extends EventTarget {}
  class XMLHttpRequest extends XMLHttpRequestEventTarget {
    constructor() {
      super();
      this.readyState = 0;
      this.status = 0;
      this.statusText = "";
      this.response = null;
      this.responseType = "";
      this.responseURL = "";
      this.timeout = 0;
      this.withCredentials = false;
      this.upload = new XMLHttpRequestEventTarget();
      this._headers = new Headers();
      this._resHeaders = new Headers();
      this._abort = null;
    }
    get responseText() {
      if (this.responseType !== "" && this.responseType !== "text") throw new DOMException("responseText is only available for text responses", "InvalidStateError");
      return this.response ?? "";
    }
    get responseXML() { return null; }
    open(method, url, async = true) {
      this._method = String(method).toUpperCase();
      this._url = new URL(String(url), g.location.href).href;
      this._setState(1);
    }
    setRequestHeader(k, v) { this._headers.append(k, v); }
    overrideMimeType() {}
    getResponseHeader(k) { return this._resHeaders.get(k); }
    getAllResponseHeaders() { let s = ""; this._resHeaders.forEach((v, k) => { s += k + ": " + v + "\r\n"; }); return s; }
    _setState(s) { this.readyState = s; this.dispatchEvent(new Event("readystatechange")); }
    _event(type, loaded = 0, total = 0) { this.dispatchEvent(new ProgressEvent(type, { lengthComputable: total > 0, loaded, total })); }
    send(body = null) {
      const ctrl = new AbortController();
      this._abort = ctrl;
      const ct = contentTypeFor(body);
      if (ct && !this._headers.has("content-type")) this._headers.set("content-type", ct);
      this._event("loadstart");
      let timer = 0;
      if (this.timeout > 0) timer = g.setTimeout(() => { ctrl.abort(); this._setState(4); this._event("timeout"); this._event("loadend"); }, this.timeout);
      request(this._method, this._url, this._headers, body, ctrl.signal).then((r) => {
        g.clearTimeout(timer);
        this.status = r.status;
        this.statusText = r.statusText;
        this.responseURL = r.url;
        this._resHeaders = r.headers;
        this._setState(2);
        this._setState(3);
        const ab = r.body;
        switch (this.responseType) {
          case "arraybuffer": this.response = ab; break;
          case "blob": this.response = new Blob([ab], { type: r.headers.get("content-type") || "" }); break;
          case "json": try { this.response = JSON.parse(N.utf8Decode(ab)); } catch { this.response = null; } break;
          case "document": this.response = null; break;
          default: this.response = N.utf8Decode(ab);
        }
        this._setState(4);
        this._event("progress", ab.byteLength, ab.byteLength);
        this._event("load", ab.byteLength, ab.byteLength);
        this._event("loadend", ab.byteLength, ab.byteLength);
      }, (err) => {
        g.clearTimeout(timer);
        if (ctrl.signal.aborted) return;
        this.status = 0;
        this._setState(4);
        this._event("error");
        this._event("loadend");
      });
    }
    abort() {
      if (this._abort) this._abort.abort();
      this._setState(4);
      this._event("abort");
      this._event("loadend");
      this.readyState = 0;
    }
  }
  XMLHttpRequest.UNSENT = 0; XMLHttpRequest.OPENED = 1; XMLHttpRequest.HEADERS_RECEIVED = 2; XMLHttpRequest.LOADING = 3; XMLHttpRequest.DONE = 4;

  Object.assign(g, { Blob, File, FileReader, FormData, Headers, Request, Response, XMLHttpRequest, XMLHttpRequestEventTarget });
  g.ReadableStream = undefined;
  g.WebSocket = function () { throw new Error("WebSocket is not supported"); };
  g.EventSource = undefined;
})(globalThis);
