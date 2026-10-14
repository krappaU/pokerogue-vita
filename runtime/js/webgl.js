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
