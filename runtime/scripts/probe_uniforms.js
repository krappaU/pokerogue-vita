// SPDX-License-Identifier: AGPL-3.0-only
// Diagnostic probe: logs the shader uniforms Phaser sets during one frame (name = value) and the
// uniforms each shader could not find. Run `evalfile scripts/probe_uniforms.js`, then `eval __unp()`.
(() => {
  const S = Phaser.Renderer.WebGL.WebGLShader.prototype;
  const R = Phaser.Renderer.WebGL.WebGLRenderer.prototype;
  const rec = { frame: 0, sets: {}, missing: {} };
  globalThis.__unp = () => JSON.stringify(rec);
  let active = false;
  const o = R.preRender;
  R.preRender = function () { rec.frame++; active = rec.frame === 3; return o.apply(this, arguments); };
  const fmt = (v) => (typeof v === "number" ? Math.round(v * 1000) / 1000 : v && v.length ? "[" + Array.from(v).slice(0, 8).map((x) => Math.round(x * 1000) / 1000).join(",") + (v.length > 8 ? ",…" + v.length : "") + "]" : String(v));
  const SU = S.setUniform1;
  for (const k of ["setUniform1", "setUniform2", "setUniform3", "setUniform4"]) {
    const f = S[k];
    if (!f) continue;
    S[k] = function (setter, name, a, b, c, d) {
      if (active) {
        const key = this.name + "." + name;
        const u = this.uniforms[name];
        if (!u) rec.missing[key] = 1;
        rec.sets[key] = [a, b, c, d].filter((x) => x !== undefined).map(fmt).join(" ") + (u && !u.location ? " (no location)" : "");
      }
      return f.apply(this, arguments);
    };
  }
  return "uniform probe installed";
})();
