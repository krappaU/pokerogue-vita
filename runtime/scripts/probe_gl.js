// SPDX-License-Identifier: AGPL-3.0-only
// Diagnostic probe for "nothing on screen": records one frame's GL calls (draws, framebuffer binds,
// errors) and reads back the screen centre. Run `evalfile scripts/probe_gl.js`, then `eval __glp()`.
(() => {
  const R = Phaser.Renderer.WebGL.WebGLRenderer.prototype;
  const rec = { frames: 0, log: [], errors: {}, done: false };
  globalThis.__glp = () => JSON.stringify(rec);
  const origPre = R.preRender, origPost = R.postRender;
  let gl = null, wrapped = false, active = false;
  function wrapGL() {
    for (const k of ["drawArrays", "drawElements", "bindFramebuffer", "viewport", "clear", "useProgram", "framebufferTexture2D", "texImage2D", "scissor"]) {
      const f = gl[k];
      gl[k] = function (...a) {
        if (active && rec.log.length === 0) rec.pending = gl.getError();
        const r = f.apply(this, a);
        if (active) {
          const e = gl.getError();
          let px = "";
          if (k === "clear" || k.startsWith("draw")) {
            const b = new Uint8Array(4);
            gl.readPixels(480, 270, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, b);
            px = " px=" + Array.from(b).join("/");
            gl.getError();
          }
          const desc = a.map((x) => (x && typeof x === "object" ? (x.constructor && x.constructor.name) || "obj" : x)).join(",");
          if (rec.log.length < 120) rec.log.push(k + "(" + desc + ")" + (e ? " ERR " + e : "") + px);
          if (e) rec.errors[k] = (rec.errors[k] || 0) + 1;
        }
        return r;
      };
    }
    wrapped = true;
  }
  R.preRender = function () {
    gl = this.gl;
    if (!wrapped) wrapGL();
    rec.frames++;
    active = rec.frames === 3;
    return origPre.apply(this, arguments);
  };
  R.postRender = function () {
    const r = origPost.apply(this, arguments);
    if (active) {
      const px = new Uint8Array(4);
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.readPixels(480, 270, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);
      rec.centre = Array.from(px);
      rec.size = [this.width, this.height];
      active = false;
      rec.done = true;
    }
    return r;
  };
  return "gl probe installed";
})();
