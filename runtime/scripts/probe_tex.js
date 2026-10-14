// SPDX-License-Identifier: AGPL-3.0-only
// Diagnostic probe: draws test textures with a known pattern (vertical colour bands) on top of the
// game through raw WebGL calls, to check texture upload and sampling. `evalfile` it; the quads
// appear in the top-left corner on every frame.
(() => {
  const R = Phaser.Renderer.WebGL.WebGLRenderer.prototype;
  if (R.__texProbe) return "already installed";
  let prog, buf, texs, gl;
  const cols = [[255, 0, 0], [0, 255, 0], [0, 0, 255], [255, 255, 255], [255, 255, 0], [0, 255, 255]];
  function makeTex(w, h, useImage) {
    const px = new Uint8Array(w * h * 4);
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
      const c = cols[Math.floor(x * 4 / w)], i = (y * w + x) * 4;
      px[i] = c[0], px[i + 1] = c[1], px[i + 2] = c[2], px[i + 3] = 255;
    }
    const t = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, t);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    if (useImage) {
      // Same path as the game's images and text: a canvas as the source.
      const c = document.createElement("canvas");
      c.width = w, c.height = h;
      const cx = c.getContext("2d");
      const id = cx.createImageData(w, h);
      id.data.set(px);
      cx.putImageData(id, 0, 0);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, c);
    } else {
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, px);
    }
    return t;
  }
  function setup(r) {
    gl = r.gl;
    const vs = gl.createShader(gl.VERTEX_SHADER);
    gl.shaderSource(vs, "attribute vec2 aPos;\nattribute vec2 aUv;\nvarying vec2 vUv;\nvoid main() { vUv = aUv; gl_Position = vec4(aPos, 0.0, 1.0); }");
    gl.compileShader(vs);
    const fs = gl.createShader(gl.FRAGMENT_SHADER);
    gl.shaderSource(fs, "precision mediump float;\nuniform sampler2D uTex;\nvarying vec2 vUv;\nvoid main() { gl_FragColor = texture2D(uTex, vUv); }");
    gl.compileShader(fs);
    prog = gl.createProgram();
    gl.attachShader(prog, vs), gl.attachShader(prog, fs);
    gl.linkProgram(prog);
    buf = gl.createBuffer();
    texs = [makeTex(4, 4, false), makeTex(6, 4, false), makeTex(300, 20, false), makeTex(300, 20, true)];
  }
  const o = R.postRender;
  R.postRender = function () {
    if (!gl) setup(this);
    this.flush && this.flush();
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, this.width, this.height);
    gl.disable(gl.BLEND);
    gl.useProgram(prog);
    gl.bindBuffer(gl.ARRAY_BUFFER, buf);
    const aPos = gl.getAttribLocation(prog, "aPos"), aUv = gl.getAttribLocation(prog, "aUv");
    gl.activeTexture(gl.TEXTURE0);
    gl.uniform1i(gl.getUniformLocation(prog, "uTex"), 0);
    texs.forEach((t, i) => {
      const x0 = -0.95 + i * 0.45, x1 = x0 + 0.4, y0 = 0.9, y1 = 0.5;
      gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([x0, y0, 0, 0, x1, y0, 1, 0, x0, y1, 0, 1, x1, y0, 1, 0, x1, y1, 1, 1, x0, y1, 0, 1]), gl.STREAM_DRAW);
      gl.enableVertexAttribArray(aPos), gl.enableVertexAttribArray(aUv);
      gl.vertexAttribPointer(aPos, 2, gl.FLOAT, false, 16, 0);
      gl.vertexAttribPointer(aUv, 2, gl.FLOAT, false, 16, 8);
      gl.bindTexture(gl.TEXTURE_2D, t);
      gl.drawArrays(gl.TRIANGLES, 0, 6);
    });
    const r = o.apply(this, arguments);
    this.pipelines.rebind && this.pipelines.rebind();
    return r;
  };
  R.__texProbe = true;
  return "texture probe installed";
})();
