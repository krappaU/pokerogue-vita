// SPDX-License-Identifier: AGPL-3.0-only
// WebGLRenderingContext: the methods and constants are native (src/gl.c) and live on the prototype.
// There is one real GL context. Extra contexts (Phaser's feature probe) share it.
(function (g) {
  "use strict";
  const N = g.__native, rt = g.__rt;
  const proto = N.createWebGLPrototype();

  function WebGLRenderingContext() { throw new TypeError("Illegal constructor"); }
  WebGLRenderingContext.prototype = proto;
  Object.defineProperty(proto, "constructor", { value: WebGLRenderingContext });
  // Constants are also static properties in browsers (WebGLRenderingContext.TEXTURE_2D).
  for (const k of Object.keys(proto)) if (/^[A-Z0-9_]+$/.test(k)) WebGLRenderingContext[k] = proto[k];

  // vitaGL translates GLSL to Sony's Cg dialect, where some words GLSL allows as identifiers are
  // reserved (Phaser's main shader names a variable "texture"; Cg then fails to parse it). Rename
  // those identifiers. Whole words only: texture2D and friends are untouched.
  const CG_RESERVED = /\b(texture|half|fixed|string|sampler|pass|technique|compile|decl|emit|get|packed|matrix|vector|register|shared|dword|row_major|column_major|interface|template|typedef|union|this|sampler_state|pixelshader|vertexshader|pixelfragment|vertexfragment)\b/g;
  const nativeShaderSource = proto.shaderSource;
  proto.shaderSource = function (shader, source) {
    let src = String(source).replace(CG_RESERVED, "pkr_$1");
    const arrays = {};
    src = splitSamplerArrays(src, arrays);
    if (shader) shader.pkrSamplerArrays = arrays;
    if (g.__shaderHook) src = g.__shaderHook(src); // set by a debug script (see bootstrap.js)
    return nativeShaderSource.call(this, shader, src);
  };

  // vitaGL compiles a sampler array (uniform sampler2D s[16]) as one sampler: only s[0] ever gets a
  // texture, and the rest sample garbage. Phaser's batching shader is built around one (a sprite's
  // texture can sit on any of 16 units). So each array becomes separate samplers s_pkr0..s_pkr15,
  // constant indices s[k] become s_pkrk (GLSL ES only allows constant sampler indices anyway), and
  // the location/uniform calls below map the array back onto them.
  const SAMPLER_ARRAY = /uniform\s+(?:(?:lowp|mediump|highp)\s+)?(sampler2D|samplerCube)\s+(\w+)\s*\[\s*(\d+)\s*\]\s*;/g;
  function splitSamplerArrays(src, arrays) {
    src = src.replace(SAMPLER_ARRAY, (m, type, name, n) => {
      arrays[name] = +n;
      const out = [];
      for (let i = 0; i < +n; i++) out.push("uniform " + type + " " + name + "_pkr" + i + ";");
      return out.join(" ");
    });
    for (const name of Object.keys(arrays))
      src = src.replace(new RegExp("\\b" + name + "\\s*\\[\\s*(\\d+)\\s*\\]", "g"), (m, i) => name + "_pkr" + i);
    return src;
  }
  const nativeAttach = proto.attachShader, nativeLink = proto.linkProgram;
  const nativeGetUniformLocation = proto.getUniformLocation, nativeGetActiveUniform = proto.getActiveUniform;
  const nativeUniform1i = proto.uniform1i, nativeUniform1iv = proto.uniform1iv;
  Object.assign(proto, {
    attachShader(program, shader) {
      if (program && shader) (program.pkrShaders || (program.pkrShaders = [])).push(shader);
      return nativeAttach.call(this, program, shader);
    },
    linkProgram(program) {
      const arrays = {};
      for (const sh of (program && program.pkrShaders) || []) Object.assign(arrays, sh.pkrSamplerArrays);
      if (program) program.pkrSamplerArrays = arrays;
      return nativeLink.call(this, program);
    },
    getUniformLocation(program, name) {
      const arrays = program && program.pkrSamplerArrays, m = /^(\w+)(?:\[(\d+)\])?$/.exec(name);
      if (!arrays || !m || !(m[1] in arrays)) return nativeGetUniformLocation.call(this, program, name);
      const locs = [];
      for (let i = m[2] ? +m[2] : 0; i < arrays[m[1]]; i++) locs.push(nativeGetUniformLocation.call(this, program, m[1] + "_pkr" + i));
      if (!locs.some((l) => l)) return null;
      const loc = Object.create(g.WebGLUniformLocation.prototype);
      loc.pkrSamplers = locs;
      return loc;
    },
    getActiveUniform(program, index) {
      const info = nativeGetActiveUniform.call(this, program, index);
      const arrays = program && program.pkrSamplerArrays, m = info && /^(\w+)_pkr0$/.exec(info.name);
      if (!m || !arrays || !(m[1] in arrays)) return info;
      return { name: m[1] + "[0]", size: arrays[m[1]], type: info.type };
    },
    uniform1i(loc, v) {
      if (loc && loc.pkrSamplers) loc = loc.pkrSamplers[0];
      return nativeUniform1i.call(this, loc, v);
    },
    uniform1iv(loc, v) {
      if (!loc || !loc.pkrSamplers) return nativeUniform1iv.call(this, loc, v);
      const locs = loc.pkrSamplers;
      for (let i = 0; i < locs.length && i < v.length; i++) if (locs[i]) nativeUniform1i.call(this, locs[i], v[i]);
    },
  });

  let mainCanvas = null;
  const lose = { loseContext() {}, restoreContext() {} };
  // Extensions Phaser can use when present; none are needed for PokeRogue.
  const extensions = { WEBGL_lose_context: lose };

  Object.assign(proto, {
    getContextAttributes() {
      return { alpha: true, antialias: false, depth: false, failIfMajorPerformanceCaveat: false, desynchronized: false,
        powerPreference: "default", premultipliedAlpha: true, preserveDrawingBuffer: false, stencil: true };
    },
    getSupportedExtensions() { return Object.keys(extensions); },
    getExtension(name) { return extensions[name] || null; },
    isContextLost() { return false; },
    makeXRCompatible() { return Promise.resolve(); },
  });
  Object.defineProperty(proto, "drawingBufferWidth", { get() { return this.canvas.width; } });
  Object.defineProperty(proto, "drawingBufferHeight", { get() { return this.canvas.height; } });
  Object.defineProperty(proto, "drawingBufferColorSpace", { get() { return "srgb"; } });

  rt.createWebGL = function (canvas, attrs) {
    const ctx = Object.create(proto);
    ctx.canvas = canvas;
    // The last canvas to ask for WebGL is the one the game renders to; its size drives the
    // screen scaling in gl.c.
    mainCanvas = canvas;
    ctx.setCanvas(canvas, attrs !== undefined && attrs !== null);
    return ctx;
  };
  rt.mainCanvas = () => mainCanvas;
  g.WebGLRenderingContext = WebGLRenderingContext;
  for (const n of ["WebGLBuffer", "WebGLTexture", "WebGLProgram", "WebGLShader", "WebGLFramebuffer",
    "WebGLRenderbuffer", "WebGLUniformLocation", "WebGLActiveInfo", "WebGLShaderPrecisionFormat"]) g[n] = function () {};
  g.WebGL2RenderingContext = undefined;
})(globalThis);
