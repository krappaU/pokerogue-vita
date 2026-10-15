// SPDX-License-Identifier: AGPL-3.0-only
// A small DOM: node tree, HTML parser, CSS selectors, the element types the game touches,
// document, window, screen, location, navigator, FontFace / document.fonts.
// Nothing here renders: the only visible output is the WebGL canvas.
(function (g) {
  "use strict";
  const N = g.__native, rt = g.__rt;
  const SW = N.screenWidth, SH = N.screenHeight;

  // ---------------------------------------------------------------- nodes
  class Node extends EventTarget {
    constructor(nodeType, nodeName) {
      super();
      this.nodeType = nodeType;
      this.nodeName = nodeName;
      this.parentNode = null;
      this.childNodes = [];
      this.ownerDocument = null;
    }
    get parentElement() { return this.parentNode && this.parentNode.nodeType === 1 ? this.parentNode : null; }
    get firstChild() { return this.childNodes[0] || null; }
    get lastChild() { return this.childNodes[this.childNodes.length - 1] || null; }
    get nextSibling() { const p = this.parentNode; if (!p) return null; return p.childNodes[p.childNodes.indexOf(this) + 1] || null; }
    get previousSibling() { const p = this.parentNode; if (!p) return null; return p.childNodes[p.childNodes.indexOf(this) - 1] || null; }
    get isConnected() { let n = this; while (n.parentNode) n = n.parentNode; return n === g.document; }
    hasChildNodes() { return this.childNodes.length > 0; }
    appendChild(c) { return this.insertBefore(c, null); }
    insertBefore(c, ref) {
      if (c.nodeType === 11) { for (const k of c.childNodes.slice()) this.insertBefore(k, ref); return c; }
      if (c.parentNode) c.parentNode.removeChild(c);
      const i = ref ? this.childNodes.indexOf(ref) : -1;
      if (i < 0) this.childNodes.push(c); else this.childNodes.splice(i, 0, c);
      c.parentNode = this;
      c.ownerDocument = g.document || null;
      if (c._connected && c.isConnected) c._connected();
      return c;
    }
    removeChild(c) {
      const i = this.childNodes.indexOf(c);
      if (i >= 0) this.childNodes.splice(i, 1);
      c.parentNode = null;
      return c;
    }
    replaceChild(n, old) { this.insertBefore(n, old); this.removeChild(old); return old; }
    remove() { if (this.parentNode) this.parentNode.removeChild(this); }
    contains(n) { for (; n; n = n.parentNode) if (n === this) return true; return false; }
    get textContent() { return this.childNodes.map((c) => c.textContent).join(""); }
    set textContent(v) { this.childNodes = []; if (v !== "" && v != null) this.appendChild(new Text(String(v))); }
    cloneNode(deep) {
      const c = this.nodeType === 3 ? new Text(this.data) : createElement(this.localName);
      if (this.nodeType === 1) { for (const [k, v] of Object.entries(this._attrs)) c.setAttribute(k, v); }
      if (deep) for (const k of this.childNodes) c.appendChild(k.cloneNode(true));
      return c;
    }
    getRootNode() { let n = this; while (n.parentNode) n = n.parentNode; return n; }
    compareDocumentPosition() { return 0; }
  }
  Node.ELEMENT_NODE = 1; Node.TEXT_NODE = 3; Node.COMMENT_NODE = 8; Node.DOCUMENT_NODE = 9; Node.DOCUMENT_FRAGMENT_NODE = 11;

  class Text extends Node {
    constructor(data = "") { super(3, "#text"); this.data = String(data); }
    get textContent() { return this.data; }
    set textContent(v) { this.data = String(v); }
    get nodeValue() { return this.data; }
  }
  class Comment extends Node {
    constructor(data = "") { super(8, "#comment"); this.data = data; }
    get textContent() { return ""; }
  }
  class DocumentFragment extends Node {
    constructor() { super(11, "#document-fragment"); }
    querySelector(s) { return querySelectorAll(this, s, true)[0] || null; }
    querySelectorAll(s) { return querySelectorAll(this, s, false); }
    get children() { return this.childNodes.filter((c) => c.nodeType === 1); }
    append(...n) { for (const x of n) this.appendChild(typeof x === "string" ? new Text(x) : x); }
  }

  // ---------------------------------------------------------------- style, classList, dataset
  const camel = (s) => s.replace(/-([a-z])/g, (_, c) => c.toUpperCase());
  const kebab = (s) => s.replace(/[A-Z]/g, (c) => "-" + c.toLowerCase());
  function makeStyle(owner) {
    const props = {};
    const api = {
      setProperty(k, v) { props[camel(k)] = String(v); if (owner._styleChanged) owner._styleChanged(); },
      getPropertyValue(k) { return props[camel(k)] ?? ""; },
      removeProperty(k) { const old = props[camel(k)]; delete props[camel(k)]; return old ?? ""; },
      get cssText() { return Object.entries(props).map(([k, v]) => `${kebab(k)}: ${v};`).join(" "); },
      set cssText(t) {
        for (const k of Object.keys(props)) delete props[k];
        for (const decl of String(t).split(";")) { const i = decl.indexOf(":"); if (i > 0) props[camel(decl.slice(0, i).trim())] = decl.slice(i + 1).trim(); }
      },
      get length() { return Object.keys(props).length; },
      item(i) { return kebab(Object.keys(props)[i] || ""); },
    };
    return new Proxy(api, {
      get(t, k) { if (k in t) return t[k]; return typeof k === "string" ? props[k] ?? "" : undefined; },
      set(t, k, v) {
        if (k === "cssText") { t.cssText = v; return true; }
        props[k] = v == null ? "" : String(v);
        if (owner._styleChanged) owner._styleChanged();
        return true;
      },
    });
  }
  class DOMTokenList {
    constructor(el) { this._el = el; }
    _get() { return (this._el.getAttribute("class") || "").split(/\s+/).filter(Boolean); }
    _set(l) { this._el.setAttribute("class", l.join(" ")); }
    add(...c) { const l = this._get(); for (const x of c) if (!l.includes(x)) l.push(x); this._set(l); }
    remove(...c) { this._set(this._get().filter((x) => !c.includes(x))); }
    contains(c) { return this._get().includes(c); }
    toggle(c, force) { const has = this.contains(c); const want = force === undefined ? !has : !!force; if (want && !has) this.add(c); if (!want && has) this.remove(c); return want; }
    replace(a, b) { const l = this._get(); const i = l.indexOf(a); if (i < 0) return false; l[i] = b; this._set(l); return true; }
    item(i) { return this._get()[i] ?? null; }
    get length() { return this._get().length; }
    get value() { return this._el.getAttribute("class") || ""; }
    forEach(fn) { this._get().forEach(fn); }
    [Symbol.iterator]() { return this._get()[Symbol.iterator](); }
    toString() { return this.value; }
  }
  function makeDataset(el) {
    const key = (k) => "data-" + kebab(k);
    return new Proxy({}, {
      get(_, k) { return typeof k === "string" ? el.getAttribute(key(k)) ?? undefined : undefined; },
      set(_, k, v) { el.setAttribute(key(k), v); return true; },
      deleteProperty(_, k) { el.removeAttribute(key(k)); return true; },
      has(_, k) { return el.hasAttribute(key(k)); },
      ownKeys() { return Object.keys(el._attrs).filter((a) => a.startsWith("data-")).map((a) => camel(a.slice(5))); },
      getOwnPropertyDescriptor(_, k) { return el.hasAttribute(key(k)) ? { enumerable: true, configurable: true, value: el.getAttribute(key(k)) } : undefined; },
    });
  }

  // ---------------------------------------------------------------- elements
  class Element extends Node {
    constructor(tag) {
      super(1, tag.toUpperCase());
      this.localName = tag.toLowerCase();
      this.tagName = this.nodeName;
      this._attrs = {};
      this.style = makeStyle(this);
      this.classList = new DOMTokenList(this);
      this.dataset = makeDataset(this);
      this.scrollTop = this.scrollLeft = 0;
    }
    get id() { return this._attrs.id || ""; }
    set id(v) { this.setAttribute("id", v); }
    get className() { return this._attrs.class || ""; }
    set className(v) { this.setAttribute("class", v); }
    getAttribute(k) { k = String(k).toLowerCase(); return k in this._attrs ? this._attrs[k] : null; }
    setAttribute(k, v) { k = String(k).toLowerCase(); this._attrs[k] = String(v); if (k === "style") this.style.cssText = v; if (this._attrChanged) this._attrChanged(k); }
    hasAttribute(k) { return String(k).toLowerCase() in this._attrs; }
    removeAttribute(k) { delete this._attrs[String(k).toLowerCase()]; }
    toggleAttribute(k, f) { const has = this.hasAttribute(k); if (f === undefined ? has : !f) { this.removeAttribute(k); return false; } this.setAttribute(k, ""); return true; }
    getAttributeNames() { return Object.keys(this._attrs); }
    get attributes() { return Object.entries(this._attrs).map(([name, value]) => ({ name, value })); }
    get children() { return this.childNodes.filter((c) => c.nodeType === 1); }
    get childElementCount() { return this.children.length; }
    get firstElementChild() { return this.children[0] || null; }
    get lastElementChild() { const c = this.children; return c[c.length - 1] || null; }
    get nextElementSibling() { const p = this.parentNode; if (!p) return null; const c = p.childNodes.filter((x) => x.nodeType === 1); return c[c.indexOf(this) + 1] || null; }
    get previousElementSibling() { const p = this.parentNode; if (!p) return null; const c = p.childNodes.filter((x) => x.nodeType === 1); return c[c.indexOf(this) - 1] || null; }
    append(...n) { for (const x of n) this.appendChild(typeof x === "string" ? new Text(x) : x); }
    prepend(...n) { for (const x of n.reverse()) this.insertBefore(typeof x === "string" ? new Text(x) : x, this.firstChild); }
    replaceChildren(...n) { this.childNodes = []; this.append(...n); }
    before(...n) { for (const x of n) this.parentNode && this.parentNode.insertBefore(typeof x === "string" ? new Text(x) : x, this); }
    after(...n) { const next = this.nextSibling; for (const x of n) this.parentNode && this.parentNode.insertBefore(typeof x === "string" ? new Text(x) : x, next); }
    insertAdjacentElement(pos, el) {
      if (pos === "beforebegin") this.before(el); else if (pos === "afterend") this.after(el);
      else if (pos === "afterbegin") this.prepend(el); else this.appendChild(el);
      return el;
    }
    insertAdjacentHTML(pos, html) { const f = parseFragment(html); for (const n of f.childNodes.slice()) this.insertAdjacentElement(pos, n); }
    get innerHTML() { return this.childNodes.map(serialize).join(""); }
    set innerHTML(html) { this.childNodes = []; const f = parseFragment(String(html)); for (const n of f.childNodes.slice()) this.appendChild(n); }
    get outerHTML() { return serialize(this); }
    get innerText() { return this.textContent; }
    set innerText(v) { this.textContent = v; }
    querySelector(s) { return querySelectorAll(this, s, true)[0] || null; }
    querySelectorAll(s) { return querySelectorAll(this, s, false); }
    getElementsByTagName(t) { return querySelectorAll(this, t, false); }
    getElementsByClassName(c) { return querySelectorAll(this, c.split(/\s+/).map((x) => "." + x).join(""), false); }
    matches(s) { return matchesSelector(this, s); }
    closest(s) { for (let n = this; n && n.nodeType === 1; n = n.parentNode) if (matchesSelector(n, s)) return n; return null; }
    // Layout: everything fills the screen unless it has explicit pixel sizes in its style.
    _px(v, d) { const m = /^(-?[\d.]+)px$/.exec(v || ""); return m ? +m[1] : d; }
    getBoundingClientRect() {
      const w = this._px(this.style.width, this.width > 0 && this.localName !== "canvas" ? this.width : SW);
      const h = this._px(this.style.height, this.height > 0 && this.localName !== "canvas" ? this.height : SH);
      const x = this._px(this.style.marginLeft, this._px(this.style.left, 0));
      const y = this._px(this.style.marginTop, this._px(this.style.top, 0));
      return { x, y, left: x, top: y, width: w, height: h, right: x + w, bottom: y + h, toJSON() { return this; } };
    }
    getClientRects() { return [this.getBoundingClientRect()]; }
    get clientWidth() { return this.getBoundingClientRect().width; }
    get clientHeight() { return this.getBoundingClientRect().height; }
    get offsetWidth() { return this.clientWidth; }
    get offsetHeight() { return this.clientHeight; }
    get offsetLeft() { return this.getBoundingClientRect().left; }
    get offsetTop() { return this.getBoundingClientRect().top; }
    get offsetParent() { return this.parentNode; }
    get scrollWidth() { return this.clientWidth; }
    get scrollHeight() { return this.clientHeight; }
    focus() { if (g.document) g.document.activeElement = this; this.dispatchEvent(new FocusEvent("focus")); }
    blur() { if (g.document && g.document.activeElement === this) g.document.activeElement = g.document.body; this.dispatchEvent(new FocusEvent("blur")); }
    click() { this.dispatchEvent(new MouseEvent("click", { bubbles: true })); }
    scrollIntoView() {} scrollTo() {} scroll() {}
    setPointerCapture() {} releasePointerCapture() {} hasPointerCapture() { return false; }
    requestFullscreen() { return Promise.reject(new DOMException("Fullscreen is not supported", "NotSupportedError")); }
    animate() { return { finished: Promise.resolve(), cancel() {}, onfinish: null }; }
    attachShadow() { return new DocumentFragment(); }
    get tabIndex() { return +(this._attrs.tabindex || -1); }
    set tabIndex(v) { this.setAttribute("tabindex", v); }
    get hidden() { return this.hasAttribute("hidden"); }
    set hidden(v) { this.toggleAttribute("hidden", !!v); }
  }
  class HTMLElement extends Element {}
  class HTMLDivElement extends HTMLElement {}
  class HTMLSpanElement extends HTMLElement {}
  class HTMLBodyElement extends HTMLElement {}
  class HTMLHeadElement extends HTMLElement {}
  class HTMLHtmlElement extends HTMLElement {}
  class HTMLStyleElement extends HTMLElement { get sheet() { return { cssRules: [], insertRule() { return 0; }, deleteRule() {} }; } }
  class HTMLMetaElement extends HTMLElement {}
  class SVGElement extends Element {}

  class HTMLScriptElement extends HTMLElement {
    get src() { return this._attrs.src ? N.resolveURL(g.location.href, this._attrs.src) : ""; }
    set src(v) { this.setAttribute("src", v); }
    get type() { return this._attrs.type || ""; }
    set type(v) { this.setAttribute("type", v); }
    get text() { return this.textContent; }
    set text(v) { this.textContent = v; }
    // Scripts inserted by JS run like in a browser. The game's newer index.html is a small shell that
    // fetches asset-manifest.json and appends the entry <script type="module"> this way.
    _connected() {
      if (this._parsed || this._started) return;
      this._started = true;
      const type = (this.type || "").toLowerCase();
      const done = (ok) => g.setTimeout(() => this.dispatchEvent(new Event(ok ? "load" : "error")), 0);
      if (type === "module") {
        if (!this.src) { N.log("[script] inline module scripts are not supported"); return done(false); }
        N.log("[script] importing " + this.src);
        const t0 = N.now();
        N.importModule(this.src).then(
          () => { N.log("[script] " + this.src + " evaluated after " + (N.now() - t0).toFixed(0) + " ms"); done(true); },
          (e) => { N.log("[script] " + this.src + " failed: " + e + "\n" + (e && e.stack || "")); done(false); },
        );
      } else if (!type || /javascript|ecmascript/.test(type)) {
        if (this.src) {
          g.fetch(this.src).then((r) => r.text()).then((code) => { (0, eval)(code); done(true); })
            .catch((e) => { N.log("[script] " + this.src + " failed: " + e); done(false); });
        } else {
          try { (0, eval)(this.textContent); } catch (e) { N.log("[script] inline script failed: " + e); }
        }
      }
    }
  }

  // <link rel=modulepreload|preload|stylesheet>: Vite's preload helper waits for load/error.
  class HTMLLinkElement extends HTMLElement {
    get href() { return this._attrs.href ? N.resolveURL(g.location.href, this._attrs.href) : ""; }
    set href(v) { this.setAttribute("href", v); }
    get rel() { return this._attrs.rel || ""; }
    set rel(v) { this.setAttribute("rel", v); }
    get as() { return this._attrs.as || ""; }
    set as(v) { this.setAttribute("as", v); }
    get relList() { return { supports: (r) => r === "modulepreload" || r === "preload" }; }
    _connected() { if (this._parsed) return; g.setTimeout(() => this.dispatchEvent(new Event("load")), 0); }
  }

  class HTMLInputElement extends HTMLElement {
    constructor(tag) { super(tag); this._value = ""; this.selectionStart = this.selectionEnd = 0; this.disabled = false; this.readOnly = false; }
    get value() { return this._value; }
    set value(v) { this._value = String(v ?? ""); this.selectionStart = this.selectionEnd = this._value.length; }
    get type() { return this._attrs.type || "text"; }
    set type(v) { this.setAttribute("type", v); }
    get name() { return this._attrs.name || ""; }
    set name(v) { this.setAttribute("name", v); }
    get placeholder() { return this._attrs.placeholder || ""; }
    set placeholder(v) { this.setAttribute("placeholder", v); }
    get maxLength() { return +(this._attrs.maxlength ?? -1); }
    set maxLength(v) { this.setAttribute("maxlength", v); }
    get checked() { return this.hasAttribute("checked"); }
    set checked(v) { this.toggleAttribute("checked", !!v); }
    select() { this.selectionStart = 0; this.selectionEnd = this._value.length; }
    setSelectionRange(a, b) { this.selectionStart = a; this.selectionEnd = b; }
    // Focus is where the Vita's system keyboard will open (Phase 3). For now, report it.
    focus() { super.focus(); if (rt.onInputFocus) rt.onInputFocus(this); }
  }
  class HTMLTextAreaElement extends HTMLInputElement {}
  class HTMLButtonElement extends HTMLElement {}
  class HTMLFormElement extends HTMLElement { submit() {} reset() {} }
  class HTMLAnchorElement extends HTMLElement {
    get href() { return this._attrs.href ? N.resolveURL(g.location.href, this._attrs.href) : ""; }
    set href(v) { this.setAttribute("href", v); }
  }
  class HTMLIFrameElement extends HTMLElement { get contentWindow() { return null; } }

  class HTMLMediaElement extends HTMLElement {
    constructor(tag) { super(tag); this.paused = true; this.currentTime = 0; this.duration = NaN; this.volume = 1; this.muted = false; this.loop = false; this.readyState = 0; this.playbackRate = 1; }
    canPlayType(t) { return /audio\/(mpeg|mp3|wav|x-wav|ogg|mp4|x-m4a|aac)/.test(t) ? "probably" : ""; }
    load() {}
    play() { this.paused = false; return Promise.resolve(); }
    pause() { this.paused = true; }
    get src() { return this._attrs.src || ""; }
    set src(v) { this.setAttribute("src", v); }
  }
  class HTMLAudioElement extends HTMLMediaElement {}
  // Video is not decoded: a video "loads" instantly as a 2x2 black frame so the game's loader and
  // Video objects keep working (WebGL uploads the frame as an ImageData-like source).
  class HTMLVideoElement extends HTMLMediaElement {
    constructor(t) {
      super(t);
      this.videoWidth = this.videoHeight = 2;
      this.width = this.height = 2;
      this.data = new Uint8ClampedArray(16);
      this.duration = 1;
      this.playsInline = true;
      this.preload = "auto";
      this.crossOrigin = null;
    }
    canPlayType(t) { return /video\/(mp4|webm)/.test(t) ? "probably" : super.canPlayType(t); }
    get src() { return this._attrs.src || ""; }
    set src(v) { this.setAttribute("src", v); this.load(); }
    load() {
      if (this._loading) return;
      this._loading = true;
      g.setTimeout(() => {
        this._loading = false;
        this.readyState = 4;
        for (const e of ["loadstart", "loadedmetadata", "loadeddata", "canplay", "canplaythrough"]) this.dispatchEvent(new Event(e));
      }, 0);
    }
    play() { this.paused = false; g.setTimeout(() => this.dispatchEvent(new Event("playing")), 0); return Promise.resolve(); }
    requestVideoFrameCallback() { return 0; }
    cancelVideoFrameCallback() {}
  }

  // ---------------------------------------------------------------- images
  async function fetchBytes(url) {
    if (url.startsWith("blob:")) {
      const b = rt.blobs.get(url);
      if (!b) throw new Error("unknown blob URL " + url);
      return b.arrayBuffer();
    }
    if (url.startsWith("data:")) {
      const i = url.indexOf(",");
      const meta = url.slice(5, i), data = url.slice(i + 1);
      if (/;base64$/.test(meta)) {
        const s = atob(data);
        const u = new Uint8Array(s.length);
        for (let j = 0; j < s.length; j++) u[j] = s.charCodeAt(j);
        return u.buffer;
      }
      return new TextEncoder().encode(decodeURIComponent(data)).buffer;
    }
    const r = await g.fetch(url);
    if (!r.ok) throw new Error("HTTP " + r.status + " for " + url);
    return r.arrayBuffer();
  }
  rt.fetchBytes = fetchBytes;

  class HTMLImageElement extends HTMLElement {
    constructor(tag = "img") {
      super(tag);
      this._native = null;
      this.naturalWidth = this.naturalHeight = 0;
      this._w = this._h = 0;
      this.complete = true;
      this.crossOrigin = null;
      this.decoding = "auto";
      this.loading = "eager";
      this._seq = 0;
    }
    get width() { return this._w || this.naturalWidth; }
    set width(v) { this._w = +v; }
    get height() { return this._h || this.naturalHeight; }
    set height(v) { this._h = +v; }
    get src() { return this._src || ""; }
    set src(v) {
      v = String(v);
      this._src = v.startsWith("blob:") || v.startsWith("data:") ? v : N.resolveURL(g.location.href, v);
      this._attrs.src = v;
      this.complete = false;
      const seq = ++this._seq;
      if (!v) return;
      fetchBytes(this._src).then((ab) => {
        if (seq !== this._seq) return;
        const bmp = N.decodeImage(ab);
        this._native = bmp;
        this.naturalWidth = bmp.width;
        this.naturalHeight = bmp.height;
        this.complete = true;
        this.dispatchEvent(new Event("load"));
      }).catch((e) => {
        if (seq !== this._seq) return;
        this.complete = true;
        N.log("[img] failed: " + this._src + " (" + e + ")");
        this.dispatchEvent(new Event("error"));
      });
    }
    decode() {
      if (this.complete && this._native) return Promise.resolve();
      return new Promise((res, rej) => { this.addEventListener("load", () => res(), { once: true }); this.addEventListener("error", () => rej(new DOMException("decode failed", "EncodingError")), { once: true }); });
    }
  }
  function Image(w, h) { const i = new HTMLImageElement("img"); if (w) i.width = w; if (h) i.height = h; return i; }
  Image.prototype = HTMLImageElement.prototype;
  function Audio(src) { const a = new HTMLAudioElement("audio"); if (src) a.src = src; return a; }
  Audio.prototype = HTMLAudioElement.prototype;

  // ---------------------------------------------------------------- canvas
  class HTMLCanvasElement extends HTMLElement {
    constructor(tag = "canvas") { super(tag); this._width = 300; this._height = 150; this._ctx = null; this._kind = null; this._native = null; }
    get width() { return this._width; }
    set width(v) { this._width = Math.max(0, v | 0); this._resized(); }
    get height() { return this._height; }
    set height(v) { this._height = Math.max(0, v | 0); this._resized(); }
    _resized() { if (this._kind === "2d") this._native.resize(this._width, this._height); if (this._ctx && this._ctx._resized) this._ctx._resized(); }
    getContext(type, attrs) {
      if (type === "2d") {
        if (this._kind && this._kind !== "2d") return null;
        if (!this._ctx) { this._kind = "2d"; this._native = N.createCanvas2D(this._width, this._height); this._ctx = new g.CanvasRenderingContext2D(this, attrs); }
        return this._ctx;
      }
      if (type === "webgl" || type === "experimental-webgl") {
        if (this._kind && this._kind !== "webgl") return null;
        if (!this._ctx) { this._kind = "webgl"; this._ctx = rt.createWebGL(this, attrs); }
        return this._ctx;
      }
      return null;
    }
    toDataURL() { return "data:,"; }
    toBlob(cb) { g.setTimeout(() => cb(new g.Blob([])), 0); }
    transferControlToOffscreen() { throw new DOMException("not supported", "NotSupportedError"); }
    captureStream() { return null; }
    // Canvas display size (Phaser's scale manager sets style width/height).
    getBoundingClientRect() {
      const w = this._px(this.style.width, this._width), h = this._px(this.style.height, this._height);
      const x = this._px(this.style.marginLeft, 0), y = this._px(this.style.marginTop, 0);
      return { x, y, left: x, top: y, width: w, height: h, right: x + w, bottom: y + h, toJSON() { return this; } };
    }
  }

  const tagClasses = {
    canvas: HTMLCanvasElement, img: HTMLImageElement, input: HTMLInputElement, textarea: HTMLTextAreaElement,
    link: HTMLLinkElement, script: HTMLScriptElement, style: HTMLStyleElement, audio: HTMLAudioElement,
    video: HTMLVideoElement, div: HTMLDivElement, span: HTMLSpanElement, body: HTMLBodyElement,
    head: HTMLHeadElement, html: HTMLHtmlElement, meta: HTMLMetaElement, button: HTMLButtonElement,
    form: HTMLFormElement, a: HTMLAnchorElement, iframe: HTMLIFrameElement,
  };
  const SVG_TAGS = new Set(["svg", "path", "rect", "g", "circle", "title", "defs", "use", "polygon", "line"]);
  function createElement(tag) {
    tag = String(tag).toLowerCase();
    const C = tagClasses[tag] || (SVG_TAGS.has(tag) ? SVGElement : HTMLElement);
    const el = new C(tag);
    el.ownerDocument = g.document || null;
    return el;
  }

  // ---------------------------------------------------------------- selectors
  // Supports: tag, #id, .class, [attr], [attr=v], [attr^=v], [attr$=v], [attr*=v], [attr~=v], :not(x),
  // descendant ( ), child (>), and comma-separated groups.
  function parseCompound(s) {
    const c = { tag: null, id: null, classes: [], attrs: [], nots: [] };
    const re = /^(\*|[a-zA-Z][\w-]*)|#([\w-]+)|\.([\w-]+)|\[\s*([\w-:]+)\s*(?:([~^$*|]?=)\s*(?:"([^"]*)"|'([^']*)'|([^\]\s]*)))?\s*\]|:not\(([^)]*)\)|:([\w-]+)/g;
    let m;
    while ((m = re.exec(s))) {
      if (m[1]) c.tag = m[1] === "*" ? null : m[1].toLowerCase();
      else if (m[2]) c.id = m[2];
      else if (m[3]) c.classes.push(m[3]);
      else if (m[4]) c.attrs.push({ name: m[4].toLowerCase(), op: m[5], value: m[6] ?? m[7] ?? m[8] });
      else if (m[9]) c.nots.push(m[9]);
      if (m.index === re.lastIndex) re.lastIndex++;
    }
    return c;
  }
  function matchCompound(el, c) {
    if (el.nodeType !== 1) return false;
    if (c.tag && el.localName !== c.tag) return false;
    if (c.id && el.id !== c.id) return false;
    if (c.classes.length) { const cl = (el._attrs.class || "").split(/\s+/); for (const x of c.classes) if (!cl.includes(x)) return false; }
    for (const a of c.attrs) {
      const v = el.getAttribute(a.name);
      if (v === null) return false;
      if (!a.op) continue;
      if (a.op === "=" && v !== a.value) return false;
      if (a.op === "^=" && !v.startsWith(a.value)) return false;
      if (a.op === "$=" && !v.endsWith(a.value)) return false;
      if (a.op === "*=" && !v.includes(a.value)) return false;
      if (a.op === "~=" && !v.split(/\s+/).includes(a.value)) return false;
      if (a.op === "|=" && v !== a.value && !v.startsWith(a.value + "-")) return false;
    }
    for (const n of c.nots) if (matchesSelector(el, n)) return false;
    return true;
  }
  const selCache = new Map();
  function parseSelector(sel) {
    let groups = selCache.get(sel);
    if (groups) return groups;
    groups = String(sel).split(",").map((part) => {
      const tokens = part.trim().replace(/\s*>\s*/g, " > ").split(/\s+/);
      const steps = [];
      let comb = " ";
      for (const t of tokens) { if (t === ">") { comb = ">"; continue; } steps.push({ comb, c: parseCompound(t) }); comb = " "; }
      return steps;
    });
    selCache.set(sel, groups);
    return groups;
  }
  function matchSteps(el, steps, i) {
    if (!matchCompound(el, steps[i].c)) return false;
    if (i === 0) return true;
    if (steps[i].comb === ">") return el.parentNode && el.parentNode.nodeType === 1 ? matchSteps(el.parentNode, steps, i - 1) : false;
    for (let p = el.parentNode; p && p.nodeType === 1; p = p.parentNode) if (matchSteps(p, steps, i - 1)) return true;
    return false;
  }
  function matchesSelector(el, sel) { return parseSelector(sel).some((steps) => matchSteps(el, steps, steps.length - 1)); }
  function querySelectorAll(root, sel, first) {
    const groups = parseSelector(sel), out = [];
    const walk = (n) => {
      for (const c of n.childNodes) {
        if (c.nodeType !== 1) continue;
        if (groups.some((steps) => matchSteps(c, steps, steps.length - 1))) { out.push(c); if (first) return true; }
        if (walk(c)) return true;
      }
      return false;
    };
    walk(root);
    return out;
  }

  // ---------------------------------------------------------------- HTML parser
  const VOID = new Set(["area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "source", "track", "wbr"]);
  const RAW = new Set(["script", "style", "textarea", "title"]);
  const ENT = { amp: "&", lt: "<", gt: ">", quot: '"', apos: "'", nbsp: " ", copy: "©", eacute: "é" };
  const decodeEnt = (s) => s.replace(/&(#x?[0-9a-fA-F]+|\w+);/g, (m, e) => e[0] === "#" ? String.fromCodePoint(e[1] === "x" ? parseInt(e.slice(2), 16) : +e.slice(1)) : ENT[e] ?? m);

  function parseInto(root, html, markParsed) {
    let i = 0, cur = root;
    const len = html.length;
    while (i < len) {
      const lt = html.indexOf("<", i);
      if (lt < 0 || lt > i) {
        const text = html.slice(i, lt < 0 ? len : lt);
        if (text.trim()) cur.appendChild(new Text(decodeEnt(text)));
        if (lt < 0) break;
        i = lt;
      }
      if (html.startsWith("<!--", i)) { const e = html.indexOf("-->", i + 4); cur.appendChild(new Comment(html.slice(i + 4, e < 0 ? len : e))); i = e < 0 ? len : e + 3; continue; }
      if (html[i + 1] === "!" || html[i + 1] === "?") { const e = html.indexOf(">", i); i = e < 0 ? len : e + 1; continue; }
      if (html[i + 1] === "/") {
        const e = html.indexOf(">", i);
        const name = html.slice(i + 2, e).trim().toLowerCase();
        for (let n = cur; n && n !== root; n = n.parentNode) if (n.localName === name) { cur = n.parentNode; break; }
        i = e < 0 ? len : e + 1;
        continue;
      }
      const m = /^<([a-zA-Z][\w-]*)/.exec(html.slice(i, i + 64));
      if (!m) { cur.appendChild(new Text("<")); i++; continue; }
      const tag = m[1].toLowerCase();
      const el = createElement(tag);
      if (markParsed) el._parsed = true;
      i += m[0].length;
      const attrRe = /\s*([^\s=/>"']+)(?:\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]+)))?/y;
      for (;;) {
        attrRe.lastIndex = i;
        const a = attrRe.exec(html);
        if (!a || !a[0].trim()) break;
        el._attrs[a[1].toLowerCase()] = decodeEnt(a[2] ?? a[3] ?? a[4] ?? "");
        i = attrRe.lastIndex;
      }
      if (el._attrs.style) el.style.cssText = el._attrs.style;
      const gt = html.indexOf(">", i);
      const selfClose = html[gt - 1] === "/";
      i = gt < 0 ? len : gt + 1;
      cur.appendChild(el);
      if (VOID.has(tag) || selfClose) continue;
      if (RAW.has(tag)) {
        const end = html.toLowerCase().indexOf("</" + tag, i);
        const text = html.slice(i, end < 0 ? len : end);
        if (text) el.appendChild(new Text(tag === "textarea" || tag === "title" ? decodeEnt(text) : text));
        const e = end < 0 ? len : html.indexOf(">", end);
        i = e < 0 ? len : e + 1;
        continue;
      }
      cur = el;
    }
  }
  function parseFragment(html) { const f = new DocumentFragment(); parseInto(f, html, false); return f; }
  function serialize(n) {
    if (n.nodeType === 3) return n.data.replace(/&/g, "&amp;").replace(/</g, "&lt;");
    if (n.nodeType === 8) return "<!--" + n.data + "-->";
    if (n.nodeType !== 1) return n.childNodes.map(serialize).join("");
    const attrs = Object.entries(n._attrs).map(([k, v]) => ` ${k}="${String(v).replace(/"/g, "&quot;")}"`).join("");
    return VOID.has(n.localName) ? `<${n.localName}${attrs}>` : `<${n.localName}${attrs}>${n.childNodes.map(serialize).join("")}</${n.localName}>`;
  }

  // ---------------------------------------------------------------- FontFace / document.fonts
  const fontFaces = new Set();
  // The game declares the same font file several times (CSS @font-face plus FontFace objects for
  // different families and languages). Download each URL once; the native side also shares
  // identical font data. Entries are dropped shortly after so the bytes don't stay in the JS heap.
  const fontDownloads = new Map();
  function fontBytes(url) {
    let p = fontDownloads.get(url);
    if (!p) {
      p = fetchBytes(url);
      fontDownloads.set(url, p);
      p.finally(() => g.setTimeout(() => fontDownloads.delete(url), 30000));
    }
    return p;
  }
  // Does a CSS unicode-range ("U+0000-00FF, U+3000-30FF, U+4E??") include "A" (U+0041)?
  function coversLatin(range) {
    for (const part of String(range).split(",")) {
      const m = /U\+([0-9a-f?]+)(?:-([0-9a-f]+))?/i.exec(part.trim());
      if (!m) continue;
      const lo = parseInt(m[1].replace(/\?/g, "0"), 16), hi = m[2] ? parseInt(m[2], 16) : parseInt(m[1].replace(/\?/g, "f"), 16);
      if (lo <= 0x41 && hi >= 0x41) return true;
    }
    return false;
  }
  class FontFace {
    constructor(family, source, desc = {}) {
      this.family = String(family).replace(/^["']|["']$/g, "");
      this.style = desc.style || "normal";
      this.weight = String(desc.weight || "normal");
      this.display = desc.display || "auto";
      this.unicodeRange = desc.unicodeRange || "U+0-10FFFF";
      this.status = "unloaded";
      this._source = source;
      this._promise = null;
      if (source instanceof ArrayBuffer || ArrayBuffer.isView(source)) this.load();
    }
    get loaded() { return this._promise || Promise.resolve(this); }
    load() {
      if (this._promise) return this._promise;
      this.status = "loading";
      const src = this._source;
      const bytes = typeof src === "string"
        ? fontBytes(N.resolveURL(g.location.href, (/url\(\s*["']?([^"')]+)["']?\s*\)/.exec(src) || [, src])[1]))
        : Promise.resolve(src instanceof ArrayBuffer ? src : src.buffer.slice(src.byteOffset, src.byteOffset + src.byteLength));
      this._promise = bytes.then((ab) => {
        const bold = /bold|[6-9]00/.test(this.weight), italic = this.style === "italic";
        if (!N.registerFont(this.family, ab, bold, italic, coversLatin(this.unicodeRange))) throw new Error("bad font data");
        this.status = "loaded";
        rt.fontGeneration = (rt.fontGeneration | 0) + 1; // invalidates cached text metrics (fastpath.js)
        N.log("[font] loaded " + this.family);
        return this;
      }).catch((e) => { this.status = "error"; N.log("[font] failed " + this.family + ": " + e); throw e; });
      return this._promise;
    }
  }
  const fontSet = rt.tracked(Object.assign(new EventTarget(), {
    status: "loaded",
    add(f) { fontFaces.add(f); if (f.status === "unloaded") f.load().catch(() => {}); return this; },
    delete(f) { return fontFaces.delete(f); },
    clear() { fontFaces.clear(); },
    has(f) { return fontFaces.has(f); },
    forEach(fn) { fontFaces.forEach((f) => fn(f, f, this)); },
    check() { return true; },
    get size() { return fontFaces.size; },
    [Symbol.iterator]() { return fontFaces.values(); },
    values() { return fontFaces.values(); },
    // load("16px emerald") resolves once the matching faces have loaded.
    load(spec) {
      const fam = String(spec).replace(/^.*?\d+(\.\d+)?(px|pt|em|rem)\s+/, "").split(",")[0].trim().replace(/^["']|["']$/g, "").toLowerCase();
      const match = [...fontFaces].filter((f) => f.family.toLowerCase() === fam);
      return Promise.all(match.map((f) => f.load()));
    },
    get ready() { return Promise.all([...fontFaces].map((f) => f.loaded.catch(() => {}))).then(() => fontSet); },
  }), "document.fonts");

  // ---------------------------------------------------------------- document
  class Document extends Node {
    constructor() {
      super(9, "#document");
      this.readyState = "loading";
      this.visibilityState = "visible";
      this.hidden = false;
      this.title = "";
      this.referrer = "";
      this.characterSet = "UTF-8";
      this.compatMode = "CSS1Compat";
      this.fullscreenElement = null;
      this.fullscreenEnabled = false;
      this.pointerLockElement = null;
      this.fonts = fontSet;
      this.documentElement = createElement("html");
      this.head = createElement("head");
      this.body = createElement("body");
      this.documentElement.appendChild(this.head);
      this.documentElement.appendChild(this.body);
      this.appendChild(this.documentElement);
      this.activeElement = this.body;
    }
    get defaultView() { return g; }
    get location() { return g.location; }
    get scrollingElement() { return this.documentElement; }
    get children() { return [this.documentElement]; }
    createElement(tag) { return createElement(tag); }
    createElementNS(ns, tag) { return createElement(tag); }
    createTextNode(t) { return new Text(t); }
    createComment(t) { return new Comment(t); }
    createDocumentFragment() { return new DocumentFragment(); }
    createEvent() { return new Event(""); }
    getElementById(id) { return querySelectorAll(this, "#" + id, true)[0] || null; }
    querySelector(s) { return querySelectorAll(this, s, true)[0] || null; }
    querySelectorAll(s) { return querySelectorAll(this, s, false); }
    getElementsByTagName(t) { return querySelectorAll(this, t, false); }
    getElementsByClassName(c) { return querySelectorAll(this, c.split(/\s+/).map((x) => "." + x).join(""), false); }
    getElementsByName(n) { return querySelectorAll(this, `[name="${n}"]`, false); }
    hasFocus() { return true; }
    exitFullscreen() { return Promise.resolve(); }
    exitPointerLock() {}
    elementFromPoint() { return this.body; }
    execCommand() { return false; }
  }

  // ---------------------------------------------------------------- window
  const href = N.baseURL;
  const loc = new URL(href);
  const location = rt.tracked({
    get href() { return loc.href; }, set href(v) { N.log("[location] navigation ignored: " + v); },
    get protocol() { return loc.protocol; }, get host() { return loc.host; }, get hostname() { return loc.hostname; },
    get port() { return loc.port; }, get pathname() { return loc.pathname; }, get search() { return loc.search; },
    get hash() { return loc.hash; }, set hash(v) { loc.hash = v; }, get origin() { return loc.origin; },
    assign(v) { N.log("[location] navigation ignored: " + v); },
    replace(v) { N.log("[location] navigation ignored: " + v); },
    reload() { N.log("[location] reload requested"); rt.reloadRequested = true; },
    toString() { return loc.href; },
  }, "location");
  const history = { length: 1, state: null, pushState(s) { this.state = s; }, replaceState(s) { this.state = s; }, back() {}, forward() {}, go() {} };

  const screen = rt.tracked({
    width: SW, height: SH, availWidth: SW, availHeight: SH, colorDepth: 24, pixelDepth: 24,
    orientation: Object.assign(new EventTarget(), { type: "landscape-primary", angle: 0, lock: () => Promise.reject(new DOMException("not supported", "NotSupportedError")), unlock() {} }),
  }, "screen");

  const media = (q) => {
    const m = /\((min|max)-(width|height):\s*(\d+)px\)/.exec(q);
    let matches = false;
    if (m) { const v = m[2] === "width" ? SW : SH; matches = m[1] === "min" ? v >= +m[3] : v <= +m[3]; }
    else if (/orientation:\s*landscape/.test(q)) matches = true;
    else if (/pointer:\s*coarse|hover:\s*none/.test(q)) matches = false;
    return Object.assign(new EventTarget(), { matches, media: q, onchange: null, addListener() {}, removeListener() {} });
  };
  class ResizeObserver { constructor(cb) { this.cb = cb; } observe() {} unobserve() {} disconnect() {} }
  class IntersectionObserver { constructor(cb) { this.cb = cb; } observe() {} unobserve() {} disconnect() {} takeRecords() { return []; } }
  class MutationObserver { constructor(cb) { this.cb = cb; } observe() {} disconnect() {} takeRecords() { return []; } }

  Object.assign(g, {
    Node, Text, Comment, DocumentFragment, Element, HTMLElement, HTMLDivElement, HTMLSpanElement, HTMLBodyElement,
    HTMLHeadElement, HTMLHtmlElement, HTMLStyleElement, HTMLMetaElement, HTMLScriptElement, HTMLLinkElement,
    HTMLInputElement, HTMLTextAreaElement, HTMLButtonElement, HTMLFormElement, HTMLAnchorElement, HTMLIFrameElement,
    HTMLMediaElement, HTMLAudioElement, HTMLVideoElement, HTMLImageElement, HTMLCanvasElement, SVGElement,
    Document, DOMTokenList, Image, Audio, FontFace, ResizeObserver, IntersectionObserver, MutationObserver,
    window: g, self: g, top: g, parent: g, frames: g, globalThis: g,
    location, history, screen,
    innerWidth: SW, innerHeight: SH, outerWidth: SW, outerHeight: SH, devicePixelRatio: 1,
    scrollX: 0, scrollY: 0, pageXOffset: 0, pageYOffset: 0, screenX: 0, screenY: 0, screenLeft: 0, screenTop: 0,
    closed: false, name: "", origin: loc.origin, isSecureContext: true, frameElement: null, length: 0,
    matchMedia: media,
    getComputedStyle: (el) => el && el.style ? el.style : makeStyle({}),
    scrollTo() {}, scrollBy() {}, scroll() {}, focus() {}, blur() {}, print() {}, stop() {}, moveTo() {}, resizeTo() {},
    open(url) { N.log("[window] open ignored: " + url); return null; },
    close() {},
    alert(m) { N.log("[alert] " + m); },
    confirm(m) { N.log("[confirm] " + m); return true; },
    prompt(m, d) { N.log("[prompt] " + m); return d ?? null; },
    postMessage() {},
    getSelection() { return { removeAllRanges() {}, addRange() {}, toString: () => "" }; },
    visualViewport: Object.assign(new EventTarget(), { width: SW, height: SH, scale: 1, offsetLeft: 0, offsetTop: 0, pageLeft: 0, pageTop: 0 }),
  });
  // window is an EventTarget too.
  for (const k of ["addEventListener", "removeEventListener", "dispatchEvent"]) g[k] = EventTarget.prototype[k];
  g.document = rt.tracked(new Document(), "document");
  g.document.documentElement.parentNode = g.document;

  // DOMParser: XML is parsed with the HTML parser (tag names lowercased), which is enough for the
  // XML the game loads (bitmap font descriptors). Phaser reads them with getElementsByTagName.
  class DOMParser {
    parseFromString(text, type) {
      const doc = new Document();
      doc.head.remove();
      doc.body.remove();
      doc.childNodes = [];
      const frag = new DocumentFragment();
      parseInto(frag, String(text), true);
      for (const n of frag.childNodes.slice()) doc.appendChild(n);
      doc.documentElement = doc.childNodes.find((n) => n.nodeType === 1) || null;
      doc.contentType = type;
      return doc;
    }
  }
  class XMLSerializer { serializeToString(n) { return serialize(n); } }
  g.DOMParser = DOMParser;
  g.XMLSerializer = XMLSerializer;

  rt.parseInto = parseInto;
  rt.createElement = createElement;
})(globalThis);
