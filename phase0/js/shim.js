// SPDX-License-Identifier: AGPL-3.0-only
// Minimal fake browser environment, just enough for Phaser 3 to boot its WebGL renderer
// with every GL call stubbed out. The point is to measure the JS cost of a frame
// (scene update, tweens, sprite batching, vertex buffer fill), not the GPU.
//
// Host contract (provided by main.c / node-driver.js):
//   globalThis.__log(str)           print a line
//   globalThis.__now()              monotonic milliseconds (float)
// Shim exports:
//   globalThis.__tick(nowMs)        run due timers + one rAF round, like a browser frame
//   globalThis.__missing            Set of API names Phaser touched that we don't implement

(function (g) {
  "use strict";
  const missing = new Set();
  g.__missing = missing;

  // Unknown properties fall through to a logging proxy prototype. Own properties stay fast.
  function tracking(obj, label) {
    const fallback = new Proxy({}, {
      get(_t, key) {
        if (typeof key === "string" && !key.startsWith("__")) missing.add(label + "." + key);
        return undefined;
      },
    });
    Object.setPrototypeOf(obj, fallback);
    return obj;
  }

  const noop = function () {};

  // ---------------------------------------------------------------- events
  class EventTarget {
    addEventListener(type, fn) {
      ((this.__listeners ||= {})[type] ||= []).push(fn);
    }
    removeEventListener(type, fn) {
      const l = this.__listeners && this.__listeners[type];
      if (l) {
        const i = l.indexOf(fn);
        if (i >= 0) l.splice(i, 1);
      }
    }
    dispatchEvent(ev) {
      const l = this.__listeners && this.__listeners[ev.type];
      if (l) for (const fn of l.slice()) fn.call(this, ev);
      return true;
    }
  }

  // ---------------------------------------------------------------- WebGL stub
  const GL_CONSTS = (
    "DEPTH_BUFFER_BIT STENCIL_BUFFER_BIT COLOR_BUFFER_BIT POINTS LINES LINE_LOOP LINE_STRIP TRIANGLES " +
    "TRIANGLE_STRIP TRIANGLE_FAN ZERO ONE SRC_COLOR ONE_MINUS_SRC_COLOR SRC_ALPHA ONE_MINUS_SRC_ALPHA " +
    "DST_ALPHA ONE_MINUS_DST_ALPHA DST_COLOR ONE_MINUS_DST_COLOR SRC_ALPHA_SATURATE FUNC_ADD BLEND_EQUATION " +
    "FUNC_SUBTRACT FUNC_REVERSE_SUBTRACT BLEND_DST_RGB BLEND_SRC_RGB CONSTANT_COLOR ONE_MINUS_CONSTANT_COLOR " +
    "CONSTANT_ALPHA ONE_MINUS_CONSTANT_ALPHA BLEND_COLOR ARRAY_BUFFER ELEMENT_ARRAY_BUFFER ARRAY_BUFFER_BINDING " +
    "ELEMENT_ARRAY_BUFFER_BINDING STREAM_DRAW STATIC_DRAW DYNAMIC_DRAW BUFFER_SIZE BUFFER_USAGE CURRENT_VERTEX_ATTRIB " +
    "FRONT BACK FRONT_AND_BACK CULL_FACE BLEND DITHER STENCIL_TEST DEPTH_TEST SCISSOR_TEST POLYGON_OFFSET_FILL " +
    "SAMPLE_ALPHA_TO_COVERAGE SAMPLE_COVERAGE NO_ERROR INVALID_ENUM INVALID_VALUE INVALID_OPERATION OUT_OF_MEMORY " +
    "CW CCW LINE_WIDTH VIEWPORT SCISSOR_BOX COLOR_CLEAR_VALUE MAX_TEXTURE_SIZE MAX_VIEWPORT_DIMS " +
    "MAX_TEXTURE_IMAGE_UNITS MAX_COMBINED_TEXTURE_IMAGE_UNITS MAX_VERTEX_ATTRIBS MAX_RENDERBUFFER_SIZE " +
    "MAX_VERTEX_UNIFORM_VECTORS MAX_FRAGMENT_UNIFORM_VECTORS MAX_VARYING_VECTORS ALIASED_POINT_SIZE_RANGE " +
    "ALIASED_LINE_WIDTH_RANGE DONT_CARE FASTEST NICEST GENERATE_MIPMAP_HINT BYTE UNSIGNED_BYTE SHORT UNSIGNED_SHORT " +
    "INT UNSIGNED_INT FLOAT DEPTH_COMPONENT ALPHA RGB RGBA LUMINANCE LUMINANCE_ALPHA UNSIGNED_SHORT_4_4_4_4 " +
    "UNSIGNED_SHORT_5_5_5_1 UNSIGNED_SHORT_5_6_5 FRAGMENT_SHADER VERTEX_SHADER COMPILE_STATUS DELETE_STATUS " +
    "LINK_STATUS VALIDATE_STATUS ATTACHED_SHADERS ACTIVE_UNIFORMS ACTIVE_ATTRIBUTES SHADING_LANGUAGE_VERSION " +
    "CURRENT_PROGRAM NEVER LESS EQUAL LEQUAL GREATER NOTEQUAL GEQUAL ALWAYS KEEP REPLACE INCR DECR INVERT " +
    "INCR_WRAP DECR_WRAP VENDOR RENDERER VERSION NEAREST LINEAR NEAREST_MIPMAP_NEAREST LINEAR_MIPMAP_NEAREST " +
    "NEAREST_MIPMAP_LINEAR LINEAR_MIPMAP_LINEAR TEXTURE_MAG_FILTER TEXTURE_MIN_FILTER TEXTURE_WRAP_S " +
    "TEXTURE_WRAP_T TEXTURE_2D TEXTURE TEXTURE_CUBE_MAP TEXTURE_BINDING_2D TEXTURE_BINDING_CUBE_MAP ACTIVE_TEXTURE " +
    "REPEAT CLAMP_TO_EDGE MIRRORED_REPEAT FLOAT_VEC2 FLOAT_VEC3 FLOAT_VEC4 INT_VEC2 INT_VEC3 INT_VEC4 BOOL " +
    "BOOL_VEC2 BOOL_VEC3 BOOL_VEC4 FLOAT_MAT2 FLOAT_MAT3 FLOAT_MAT4 SAMPLER_2D SAMPLER_CUBE " +
    "VERTEX_ATTRIB_ARRAY_ENABLED HIGH_FLOAT MEDIUM_FLOAT LOW_FLOAT FRAMEBUFFER RENDERBUFFER RGBA4 RGB5_A1 RGB565 " +
    "DEPTH_COMPONENT16 STENCIL_INDEX8 DEPTH_STENCIL COLOR_ATTACHMENT0 DEPTH_ATTACHMENT STENCIL_ATTACHMENT " +
    "DEPTH_STENCIL_ATTACHMENT NONE FRAMEBUFFER_COMPLETE FRAMEBUFFER_BINDING RENDERBUFFER_BINDING " +
    "UNPACK_FLIP_Y_WEBGL UNPACK_PREMULTIPLY_ALPHA_WEBGL CONTEXT_LOST_WEBGL UNPACK_COLORSPACE_CONVERSION_WEBGL " +
    "BROWSER_DEFAULT_WEBGL UNPACK_ALIGNMENT PACK_ALIGNMENT FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE " +
    "FRAMEBUFFER_ATTACHMENT_OBJECT_NAME FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL " +
    "FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE FRAMEBUFFER_INCOMPLETE_ATTACHMENT " +
    "FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT FRAMEBUFFER_INCOMPLETE_DIMENSIONS FRAMEBUFFER_UNSUPPORTED " +
    "RENDERBUFFER_WIDTH RENDERBUFFER_HEIGHT RENDERBUFFER_INTERNAL_FORMAT STENCIL_INDEX MAX_CUBE_MAP_TEXTURE_SIZE " +
    "VERTEX_ATTRIB_ARRAY_SIZE VERTEX_ATTRIB_ARRAY_STRIDE VERTEX_ATTRIB_ARRAY_TYPE VERTEX_ATTRIB_ARRAY_NORMALIZED " +
    "VERTEX_ATTRIB_ARRAY_POINTER VERTEX_ATTRIB_ARRAY_BUFFER_BINDING IMPLEMENTATION_COLOR_READ_TYPE " +
    "IMPLEMENTATION_COLOR_READ_FORMAT SHADER_TYPE SAMPLES SAMPLE_BUFFERS DEPTH_BITS STENCIL_BITS RED_BITS " +
    "GREEN_BITS BLUE_BITS ALPHA_BITS"
  ).split(" ");
  for (let i = 0; i < 32; i++) GL_CONSTS.push("TEXTURE" + i);

  let glId = 1;
  const glObj = (kind) => ({ __kind: kind, __id: glId++ });

  function makeGL(canvas) {
    const gl = { canvas };
    GL_CONSTS.forEach((name, i) => { gl[name] = 0x1000 + i; });
    const params = {
      [gl.MAX_TEXTURE_IMAGE_UNITS]: 16,
      [gl.MAX_COMBINED_TEXTURE_IMAGE_UNITS]: 16,
      [gl.MAX_TEXTURE_SIZE]: 4096,
      [gl.MAX_RENDERBUFFER_SIZE]: 4096,
      [gl.MAX_VERTEX_ATTRIBS]: 16,
      [gl.MAX_VERTEX_UNIFORM_VECTORS]: 256,
      [gl.MAX_FRAGMENT_UNIFORM_VECTORS]: 256,
      [gl.MAX_VIEWPORT_DIMS]: new Int32Array([4096, 4096]),
      [gl.VIEWPORT]: new Int32Array([0, 0, 960, 544]),
      [gl.VERSION]: "WebGL 1.0 (stub)",
      [gl.SHADING_LANGUAGE_VERSION]: "WebGL GLSL ES 1.0 (stub)",
      [gl.VENDOR]: "stub",
      [gl.RENDERER]: "stub",
    };
    let calls = 0;
    gl.__calls = () => calls;
    const count = function () { calls++; };
    // Hot, side-effect-free calls
    for (const m of (
      "activeTexture attachShader bindAttribLocation bindBuffer bindFramebuffer bindRenderbuffer bindTexture " +
      "blendColor blendEquation blendEquationSeparate blendFunc blendFuncSeparate bufferData bufferSubData clear " +
      "clearColor clearDepth clearStencil colorMask compileShader compressedTexImage2D compressedTexSubImage2D " +
      "copyTexImage2D copyTexSubImage2D cullFace deleteBuffer deleteFramebuffer deleteProgram deleteRenderbuffer " +
      "deleteShader deleteTexture depthFunc depthMask depthRange detachShader disable disableVertexAttribArray " +
      "drawArrays drawElements enable enableVertexAttribArray finish flush framebufferRenderbuffer " +
      "framebufferTexture2D frontFace generateMipmap hint lineWidth linkProgram pixelStorei polygonOffset " +
      "renderbufferStorage sampleCoverage scissor shaderSource stencilFunc stencilFuncSeparate stencilMask " +
      "stencilMaskSeparate stencilOp stencilOpSeparate texImage2D texParameterf texParameteri texSubImage2D " +
      "uniform1f uniform1fv uniform1i uniform1iv uniform2f uniform2fv uniform2i uniform2iv uniform3f uniform3fv " +
      "uniform3i uniform3iv uniform4f uniform4fv uniform4i uniform4iv uniformMatrix2fv uniformMatrix3fv " +
      "uniformMatrix4fv useProgram validateProgram vertexAttrib1f vertexAttrib2f vertexAttrib3f vertexAttrib4f " +
      "vertexAttribPointer viewport readPixels"
    ).split(" ")) gl[m] = count;
    gl.createBuffer = () => glObj("buffer");
    gl.createFramebuffer = () => glObj("framebuffer");
    gl.createProgram = () => glObj("program");
    gl.createRenderbuffer = () => glObj("renderbuffer");
    gl.createShader = (type) => glObj("shader");
    gl.createTexture = () => glObj("texture");
    gl.getShaderParameter = () => true;
    gl.getProgramParameter = (p, pname) =>
      pname === gl.ACTIVE_UNIFORMS || pname === gl.ACTIVE_ATTRIBUTES ? 0 : true;
    gl.getShaderInfoLog = () => "";
    gl.getProgramInfoLog = () => "";
    gl.getAttribLocation = (() => { let n = 0; return () => (n++ % 8); })();
    gl.getUniformLocation = () => glObj("uniform");
    gl.getActiveUniform = () => null;
    gl.getActiveAttrib = () => null;
    gl.getParameter = (p) => (p in params ? params[p] : 0);
    gl.getError = () => 0;
    gl.getExtension = () => null;
    gl.getSupportedExtensions = () => [];
    gl.getContextAttributes = () => ({ alpha: true, antialias: false, premultipliedAlpha: true, stencil: true });
    gl.isContextLost = () => false;
    gl.checkFramebufferStatus = () => gl.FRAMEBUFFER_COMPLETE;
    gl.getShaderPrecisionFormat = () => ({ rangeMin: 127, rangeMax: 127, precision: 23 });
    gl.getFramebufferAttachmentParameter = () => null;
    gl.getBufferParameter = () => 0;
    gl.getRenderbufferParameter = () => 0;
    gl.getTexParameter = () => 0;
    gl.getVertexAttrib = () => null;
    gl.getVertexAttribOffset = () => 0;
    gl.getUniform = () => null;
    gl.getAttachedShaders = () => [];
    gl.getShaderSource = () => "";
    gl.isEnabled = () => false;
    gl.isFramebuffer = gl.isProgram = gl.isRenderbuffer = gl.isShader = () => true;
    gl.isTexture = () => true;
    gl.isBuffer = () => true;
    gl.drawingBufferWidth = canvas.width;
    gl.drawingBufferHeight = canvas.height;
    return tracking(gl, "gl");
  }

  // ---------------------------------------------------------------- Canvas 2D stub
  function makeCtx2D(canvas) {
    const ctx = {
      canvas,
      fillStyle: "#000", strokeStyle: "#000", globalAlpha: 1, globalCompositeOperation: "source-over",
      font: "10px sans-serif", textAlign: "start", textBaseline: "alphabetic", lineWidth: 1,
      imageSmoothingEnabled: true, shadowColor: "", shadowBlur: 0, shadowOffsetX: 0, shadowOffsetY: 0,
      miterLimit: 10, lineJoin: "miter", lineCap: "butt", direction: "inherit",
      letterSpacing: "0px",
    };
    for (const m of (
      "save restore scale rotate translate transform setTransform resetTransform clearRect fillRect strokeRect " +
      "beginPath closePath moveTo lineTo quadraticCurveTo bezierCurveTo arc arcTo rect ellipse fill stroke clip " +
      "fillText strokeText drawImage putImageData setLineDash"
    ).split(" ")) ctx[m] = noop;
    ctx.measureText = (s) => {
      const size = parseInt(/(\d+)px/.exec(ctx.font)?.[1] ?? "10", 10);
      const w = String(s).length * size * 0.5;
      return {
        width: w, actualBoundingBoxAscent: size * 0.8, actualBoundingBoxDescent: size * 0.2,
        actualBoundingBoxLeft: 0, actualBoundingBoxRight: w, fontBoundingBoxAscent: size * 0.8,
        fontBoundingBoxDescent: size * 0.2,
      };
    };
    ctx.getImageData = (x, y, w, h) => ({ width: w, height: h, data: new Uint8ClampedArray(w * h * 4) });
    ctx.createImageData = (w, h) => ({ width: w, height: h, data: new Uint8ClampedArray(w * h * 4) });
    ctx.createLinearGradient = () => ({ addColorStop: noop });
    ctx.createRadialGradient = () => ({ addColorStop: noop });
    ctx.createPattern = () => ({});
    ctx.getLineDash = () => [];
    ctx.isPointInPath = () => false;
    return tracking(ctx, "ctx2d");
  }

  // ---------------------------------------------------------------- DOM
  class Style { setProperty() {} removeProperty() {} getPropertyValue() { return ""; } }

  class Element extends EventTarget {
    constructor(tag) {
      super();
      this.tagName = tag.toUpperCase();
      this.nodeName = this.tagName;
      this.style = new Style();
      this.children = [];
      this.childNodes = this.children;
      this.parentNode = null;
      this.parentElement = null;
      this.attributes = {};
      this.dataset = {};
      this.classList = { add: noop, remove: noop, contains: () => false, toggle: noop };
      this.innerHTML = "";
      this.textContent = "";
      this.id = "";
    }
    appendChild(c) { this.children.push(c); c.parentNode = c.parentElement = this; return c; }
    removeChild(c) { const i = this.children.indexOf(c); if (i >= 0) this.children.splice(i, 1); c.parentNode = null; return c; }
    insertBefore(c) { return this.appendChild(c); }
    contains(c) { return this.children.includes(c); }
    setAttribute(k, v) { this.attributes[k] = String(v); }
    getAttribute(k) { return k in this.attributes ? this.attributes[k] : null; }
    removeAttribute(k) { delete this.attributes[k]; }
    getBoundingClientRect() {
      const w = this.width || 960, h = this.height || 544;
      return { x: 0, y: 0, left: 0, top: 0, right: w, bottom: h, width: w, height: h };
    }
    get clientWidth() { return this.width || 960; }
    get clientHeight() { return this.height || 544; }
    get offsetWidth() { return this.clientWidth; }
    get offsetHeight() { return this.clientHeight; }
    focus() {} blur() {} click() {}
    querySelector() { return null; }
    querySelectorAll() { return []; }
    getElementsByTagName() { return []; }
  }

  class HTMLCanvasElement extends Element {
    constructor() { super("canvas"); this.width = 300; this.height = 150; this.__ctx = {}; }
    getContext(type) {
      // Cached per type: Phaser's CanvasPool reuses canvases across context types.
      if (type === "2d") return (this.__ctx["2d"] ||= makeCtx2D(this));
      if (type === "webgl" || type === "experimental-webgl") return (this.__ctx.webgl ||= makeGL(this));
      return null;
    }
    toDataURL() { return "data:image/png;base64,"; }
  }

  class HTMLImageElement extends Element {
    constructor() { super("img"); this.width = 0; this.height = 0; this.complete = false; this.crossOrigin = null; }
    set src(v) {
      this.__src = v;
      // Benchmark images are "decoded" instantly; size comes from a #WxH suffix.
      const m = /#(\d+)x(\d+)$/.exec(v);
      this.width = this.naturalWidth = m ? +m[1] : 64;
      this.height = this.naturalHeight = m ? +m[2] : 64;
      this.complete = true;
      g.setTimeout(() => { this.onload?.(); this.dispatchEvent({ type: "load", target: this }); }, 0);
    }
    get src() { return this.__src; }
    decode() { return Promise.resolve(); }
  }

  class HTMLMediaElement extends Element {
    canPlayType(t) { return /mp3|mpeg|wav|ogg/.test(t) ? "probably" : ""; }
    load() {} play() { return Promise.resolve(); } pause() {}
  }

  const body = new Element("body");
  const documentElement = new Element("html");
  documentElement.appendChild(body);
  const document = tracking(Object.assign(new EventTarget(), {
    // tracking() replaces the prototype, so EventTarget methods must be own properties
    addEventListener: EventTarget.prototype.addEventListener,
    removeEventListener: EventTarget.prototype.removeEventListener,
    dispatchEvent: EventTarget.prototype.dispatchEvent,
    readyState: "complete",
    body,
    documentElement,
    head: new Element("head"),
    hidden: false,
    visibilityState: "visible",
    cookie: "",
    createElement(tag) {
      tag = String(tag).toLowerCase();
      if (tag === "canvas") return new HTMLCanvasElement();
      if (tag === "img") return new HTMLImageElement();
      if (tag === "audio" || tag === "video") return new HTMLMediaElement(tag);
      return new Element(tag);
    },
    createElementNS(_ns, tag) { return this.createElement(tag); },
    createTextNode(t) { return { textContent: t }; },
    getElementById() { return null; },
    querySelector() { return null; },
    querySelectorAll() { return []; },
    getElementsByTagName() { return []; },
  }), "document");

  // ---------------------------------------------------------------- timers / rAF
  let timerSeq = 1;
  const timers = new Map(); // id -> {at, fn, args, interval}
  let rafQueue = [];
  let nowMs = 0;

  g.setTimeout = (fn, ms = 0, ...args) => { const id = timerSeq++; timers.set(id, { at: nowMs + (+ms || 0), fn, args, interval: 0 }); return id; };
  g.setInterval = (fn, ms = 0, ...args) => { const id = timerSeq++; timers.set(id, { at: nowMs + (+ms || 0), fn, args, interval: Math.max(1, +ms || 0) }); return id; };
  g.clearTimeout = g.clearInterval = (id) => { timers.delete(id); };
  g.requestAnimationFrame = (fn) => { rafQueue.push(fn); return rafQueue.length; };
  g.cancelAnimationFrame = noop;
  g.queueMicrotask ||= (fn) => Promise.resolve().then(fn);

  g.__tick = function (t) {
    nowMs = t;
    for (const [id, tm] of [...timers]) {
      if (tm.at <= t && timers.has(id)) {
        if (tm.interval) tm.at = t + tm.interval; else timers.delete(id);
        tm.fn(...tm.args);
      }
    }
    const q = rafQueue;
    rafQueue = [];
    for (const fn of q) fn(t);
  };

  // ---------------------------------------------------------------- window
  const navigator = tracking({
    userAgent: "Mozilla/5.0 (PlayStation Vita 3.65) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0 Safari/537.36",
    platform: "PlayStation Vita",
    language: "en-US",
    languages: ["en-US", "en"],
    maxTouchPoints: 5,
    hardwareConcurrency: 1,
    onLine: true,
    getGamepads: () => [],
    vibrate: () => false,
  }, "navigator");

  const performance = { now: () => g.__now(), timeOrigin: 0, mark: noop, measure: noop };

  const storage = () => {
    const m = new Map();
    return {
      getItem: (k) => (m.has(k) ? m.get(k) : null),
      setItem: (k, v) => m.set(k, String(v)),
      removeItem: (k) => m.delete(k),
      clear: () => m.clear(),
      key: (i) => [...m.keys()][i] ?? null,
      get length() { return m.size; },
    };
  };

  Object.assign(g, {
    window: g, self: g, top: g, parent: g,
    document, navigator, performance,
    location: { href: "https://pokerogue.net/", protocol: "https:", host: "pokerogue.net", hostname: "pokerogue.net", origin: "https://pokerogue.net", pathname: "/", search: "", hash: "" },
    screen: { width: 960, height: 544, availWidth: 960, availHeight: 544, orientation: { type: "landscape-primary", angle: 0, addEventListener: noop } },
    innerWidth: 960, innerHeight: 544, outerWidth: 960, outerHeight: 544, devicePixelRatio: 1,
    scrollX: 0, scrollY: 0, pageXOffset: 0, pageYOffset: 0,
    localStorage: storage(), sessionStorage: storage(),
    WebGLRenderingContext: function WebGLRenderingContext() {},
    CanvasRenderingContext2D: function CanvasRenderingContext2D() {},
    EventTarget, Element, HTMLElement: Element, HTMLCanvasElement, HTMLImageElement, Image: HTMLImageElement,
    HTMLVideoElement: HTMLMediaElement, HTMLAudioElement: HTMLMediaElement,
    addEventListener: EventTarget.prototype.addEventListener,
    removeEventListener: EventTarget.prototype.removeEventListener,
    dispatchEvent: EventTarget.prototype.dispatchEvent,
    focus: noop, blur: noop, scrollTo: noop,
    getComputedStyle: () => new Style(),
    matchMedia: () => ({ matches: false, addListener: noop, removeListener: noop, addEventListener: noop }),
    console: g.console || { log: (...a) => g.__log(a.join(" ")), warn: (...a) => g.__log("WARN " + a.join(" ")), error: (...a) => g.__log("ERROR " + a.join(" ")), info: (...a) => g.__log(a.join(" ")) },
  });
})(globalThis);
