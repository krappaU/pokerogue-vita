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
    if (rt.shaderFix) {
      const r = rewriteShader(src);
      if (r.applied.length) {
        const name = (/#define SHADER_NAME (\w+)/.exec(src) || [])[1] ||
          (/spriteColors/.test(src) ? "PokeRogue sprite" : /terrainColor/.test(src) ? "PokeRogue field" : "a shader");
        const line = "[shaderfix] " + name + ": " + r.applied.join(", ");
        if (!shaderFixLogged.has(line)) shaderFixLogged.add(line), console.log(line);
      }
      src = r.src;
    }
    if (N.plainShaders) src = plainShader(src);
    return nativeShaderSource.call(this, shader, src);
  };

  // Rewrites that leave every pixel the same but give vitaGL's GLSL -> Cg -> SGX compiler cheaper
  // code. On the console (0.61, gpuprofile=1) one full-screen quad of PokeRogue's field shader took
  // ~19 ms of GPU time and one Pokémon of its sprite shader ~11 ms, against ~3 ms for a whole battle
  // with plain texture reads (plainshaders=1). The suspects: a condition that mixes a per-pixel test
  // with a setting seems to run its block for every pixel (both sides computed), the sprite shader's
  // 32-step colour loops, and Phaser's texture read inside an if-chain (no early fetch). Each rewrite
  // matches a pattern and leaves shaders without it alone; tests/shaders compares the pixels with and
  // without. config.txt shaderfix=0 (debug builds) or __rt.shaderFix = false keeps the originals.
  rt.shaderFix = N.shaderFix !== false;
  const shaderFixLogged = new Set();

  // The index just past the block that starts at src[open] === "{".
  function blockEnd(src, open) {
    for (let i = open, depth = 0; i < src.length; i++) {
      if (src[i] === "{") depth++;
      else if (src[i] === "}" && --depth === 0) return i + 1;
    }
    return -1;
  }
  // Effects a setting can switch off are wrapped in /*pkr+NAME*/ ... /*pkr-NAME*/: comments, so the
  // shader compiles as it is, but gl.c builds copies without the wrapped code and draws with the copy
  // whose effects the current settings switch off (the GPU runs all of a shader's code, effects
  // switched off included: 0.64's benchmark). Names gl.c knows: terrain, daynight, tera, variants,
  // fusion, shadow.
  const mark = (name, code) => (name ? "/*pkr+" + name + "*/ " + code + " /*pkr-" + name + "*/" : code);
  // `if (A && B) { ... }` -> `if (A) { if (B) { ... } }` for every match of re (A, B from its groups
  // through split), unless an else follows (it would change owner).
  function splitIf(src, re, split, name) {
    let out = "", from = 0, n = 0, m;
    re.lastIndex = 0;
    while ((m = re.exec(src))) {
      const open = m.index + m[0].length - 1, end = blockEnd(src, open);
      if (end < 0 || /^\s*else\b/.test(src.slice(end))) continue;
      const [outer, inner] = split(m);
      out += src.slice(from, m.index) + mark(name, "if (" + outer + ") { if (" + inner + ") " + src.slice(open, end) + " }");
      from = end;
      re.lastIndex = end;
      n++;
    }
    return [out + src.slice(from), n];
  }
  // `for (...) { if (C[i].a == 0.0) break; ... }` -> `if (C[0].a != 0.0) { if (texture.a > 0.0) { for ... } }`
  function guardLoops(src, skip) {
    const re = /for \(int i = 0; i < 32; i\+\+\) \{\s*if \((\w+)\[i\](\.a|\[3\]) == 0\.0\)\s*break;/g;
    let out = "", from = 0, n = 0, m;
    while ((m = re.exec(src))) {
      const end = blockEnd(src, src.indexOf("{", m.index));
      if (end < 0) continue;
      const name = { baseVariantColors: "variants", spriteColors: "fusion" }[m[1]];
      // and only for texels that aren't transparent: the loop changes nothing for them (its match
      // needs texture.a > 0), and the GPU ran its 32 steps for every pixel of a shiny sprite's quad,
      // most of it transparent (0.87, log131: ~100 ms of GPU per full screen for a shiny Pokémon)
      const tex = /\bpkr_texture\b/.test(src) ? "pkr_texture" : "texture";
      out += src.slice(from, m.index) + mark(name, skip.has("palette alpha")
        ? "if (" + m[1] + "[0]" + m[2] + " != 0.0) { " + src.slice(m.index, end) + " }"
        : "if (" + m[1] + "[0]" + m[2] + " != 0.0) { if (" + tex + ".a > 0.0) { " + src.slice(m.index, end) + " } }");
      from = end;
      re.lastIndex = end;
      n++;
    }
    return [out + src.slice(from), n];
  }
  // PokeRogue's sprite shadow block works out where the pixel sits on the sprite, then draws the
  // shadow's ellipse in the bottom rows (spriteY >= 0.9) where the sprite is transparent or cut off:
  //   if ((spriteY >= 0.9 && (color.a == 0.0 || yOverflow))) { A } else if (yOverflow) { discard; }
  // -> if (spriteY >= 0.9) { if (color.a == 0.0 || yOverflow) { A } } else if (yOverflow) { discard; }
  // (the same: with spriteY >= 0.9 and the inner test false, yOverflow is false too), with width and
  // spriteX, which only A reads, worked out inside A. Most pixels then skip it.
  function shadowTests(block) {
    const test = /if \(\(spriteY >= 0\.9 && \(color\.a == 0\.0 \|\| yOverflow\)\)\) \{/.exec(block);
    const moved = [/\s*float width = [^;]*;/, /\s*float spriteX = [^;]*;/].map((re) => re.exec(block));
    if (!test || moved.some((m) => !m || m.index > test.index)) return null;
    const open = test.index + test[0].length - 1, end = blockEnd(block, open);
    const tail = end > 0 && /^\s*else if \(yOverflow\) \{\s*discard;\s*\}/.exec(block.slice(end));
    if (!tail) return null;
    const body = block.slice(open + 1, end - 1), decl = moved.map((m) => m[0].trim()).join(" ");
    const out = block.slice(0, test.index) + "if (spriteY >= 0.9) { if (color.a == 0.0 || yOverflow) { " + decl + body + "} }" +
      block.slice(end);
    // The moved lines last: the indices above are in the original block.
    return moved[1].index > moved[0].index
      ? cut(cut(out, moved[1]), moved[0])
      : cut(cut(out, moved[0]), moved[1]);
  }
  const cut = (s, m) => s.slice(0, m.index) + s.slice(m.index + m[0].length);
  function rewriteShader(src) {
    const applied = [], skip = new Set(rt.shaderFixSkip || []); // skip: the shader benchmark's comparisons
    let n;
    // Phaser's texture pick (WebGL Utils.parseFragmentShaderMaxTextures, sampler arrays already split):
    // every unit read unconditionally, then the result chosen.
    src = src.replace(/if \(outTexId < 0\.5\)\s*\{\s*pkr_texture = texture2D\(\w+, outTexCoord\);\s*\}(?:\s*else\s*(?:if \(outTexId < \d+\.5\))?\s*\{\s*pkr_texture = texture2D\(\w+, outTexCoord\);\s*\})+/g, (chain) => {
      const s = [...chain.matchAll(/texture2D\((\w+), outTexCoord\)/g)].map((x) => x[1]);
      let pick = "pkr_t" + (s.length - 1);
      for (let i = s.length - 2; i >= 0; i--) pick = "(outTexId < " + i + ".5 ? pkr_t" + i + " : " + pick + ")";
      applied.push("texture pick (" + s.length + ")");
      return s.map((name, i) => "vec4 pkr_t" + i + " = texture2D(" + name + ", outTexCoord);").join(" ") + " pkr_texture = " + pick + ";";
    });
    // Phaser's tint effect (0 multiply, 1 fill, 2 solid colour) reaches the fragment shader as an
    // interpolated varying, which the Vita's GPU doesn't keep exactly at 2.0: the == test failed and
    // shapes drawn without a texture of their own (Graphics/Polygon fills: the summary's stats graph)
    // showed whatever texture was bound. Phaser only ever passes 0, 1 or 2, so rounding it is exact.
    const main = /void main\s*\(\s*(?:void)?\s*\)\s*\{/.exec(src);
    if (main && /outTintEffect == [12]\.0/.test(src)) {
      src = src.replace(/outTintEffect == ([12])\.0\b/g, "pkr_tintEffect == $1.0");
      const at = main.index + main[0].length;
      src = src.slice(0, at) + " float pkr_tintEffect = floor(outTintEffect + 0.5);" + src.slice(at);
      applied.push("tint effect test");
    }
    // PokeRogue's colour loops stop at the first entry with alpha 0: skip them when that's the first.
    [src, n] = guardLoops(src, skip);
    if (n) applied.push("colour loops (" + n + ")");
    // PokeRogue's day/night colour: an if-chain on settings only, so the same for every pixel of a
    // draw, but the GPU ran it for each (0.76's benchmark: day/night cost ~7 ms per full screen in
    // both shaders). Its blend with the colour, blendHardLight(color, tint), is per channel
    // tint < 0.5 ? 2 tint c : 1 - 2 (1 - tint)(1 - c), so c * mul + add with mul and add from the
    // tint alone (0.77's benchmark: still ~3 ms per full screen as a blend). gl.c's copies have
    // neither: they read pkr_tintMul and pkr_tintAdd, which gl.c works out once per draw.
    // /*pkr<NAME*/ CODE /*pkr|NEW*/: the copies have NEW instead of CODE.
    const dn = !skip.has("day/night colour") && /vec3 dayNightTint;\s*(if \(any\(lessThan\(vec3\(0\.0\), overrideTint\)\)\) \{)/.exec(src);
    const mainAt = /void main\s*\(/.exec(src);
    if (dn && mainAt && mainAt.index < dn.index) {
      const at = dn.index + dn[0].length - dn[1].length;
      let end = blockEnd(src, at + dn[1].length - 1);
      for (let e; end > 0 && (e = /^\s*else\b/.exec(src.slice(end)));) end = blockEnd(src, src.indexOf("{", end + e[0].length));
      const hl = end > 0 && !skip.has("hard light") &&
        /color(?:\.rgb = blendHardLight\(color\.rgb, dayNightTint\)| = vec4\(blendHardLight\(color\.rgb, dayNightTint\), color\.a\));/.exec(src.slice(end));
      if (end > 0) {
        const swap = (code, to) => "/*pkr<tint*/ " + code + " /*pkr|" + to + "*/";
        let tail = src.slice(end);
        if (hl) tail = tail.slice(0, hl.index) + swap(hl[0], "color.rgb = color.rgb * pkr_tintMul + pkr_tintAdd;") + tail.slice(hl.index + hl[0].length);
        src = src.slice(0, mainAt.index) + swap("", hl ? "uniform vec3 pkr_tintMul; uniform vec3 pkr_tintAdd;" : "uniform vec3 pkr_dayNightTint;") +
          "\n" + src.slice(mainAt.index, at) + swap(src.slice(at, end), hl ? "" : "dayNightTint = pkr_dayNightTint;") + tail;
        applied.push("day/night colour per draw" + (hl ? " (with its blend)" : ""));
      }
    }
    // PokeRogue's field shader draws the arena's bases (two 1920x792 images, 73% of the screen each,
    // mostly transparent) and the backdrop: for a transparent texel without a fill tint, everything
    // after `color = texture * texel` leaves color as it is (the tint effects need effect 1 or 2,
    // day/night and terrain color.a > 0), so it can end right there. Not the sprite shader: its
    // shadow is drawn on transparent pixels.
    if (!skip.has("transparent early") && /terrainColor/.test(src) && !/hasShadow/.test(src) && /pkr_tintEffect = floor/.test(src)) {
      const re = /vec4 color = pkr_texture \* texel;/;
      if (re.test(src)) {
        src = src.replace(re, "vec4 color = pkr_texture * texel; if (pkr_texture.a == 0.0 && pkr_tintEffect == 0.0) { gl_FragColor = color; return; }");
        applied.push("transparent early");
      }
    }
    // Per-pixel test && setting: the setting first, on its own.
    const splits = [
      [/if \(color\.a > 0\.0 && (all\(lessThan\(vec3\(0\.0\), teraColor\)\))\) \{/g, (m) => [m[1], "color.a > 0.0"], "tera", "tera"],
      [/if \(color\.a > 0\.0 && (!ignoreTimeTint)\) \{/g, (m) => [m[1], "color.a > 0.0"], "day/night", "daynight"],
      [/if \(\s*(terrainColorRatio > 0\.0)\s*&& (\(1\.0 - terrainColorRatio\) < outTexCoord\.y)\s*&& (color\.a > 0\.0)\s*&& (\(any\(lessThan\(vec3\(0\.0\), terrainColor\)\)\))\s*\) \{/g,
        (m) => [m[1] + " && " + m[4], m[2] + " && " + m[3]], "terrain", "terrain"],
    ];
    for (const [re, split, name, markName] of splits) {
      [src, n] = splitIf(src, re, split, markName);
      if (n) applied.push(name);
    }
    // PokeRogue's sprite shadow: only a setting switches it off.
    const sh = /if \(hasShadow\) \{/.exec(src), shEnd = sh ? blockEnd(src, sh.index + sh[0].length - 1) : -1;
    if (shEnd > 0 && !/^\s*else\b/.test(src.slice(shEnd))) {
      let block = src.slice(sh.index, shEnd);
      if (!skip.has("shadow tests")) {
        const r = shadowTests(block);
        if (r) block = r, applied.push("shadow tests");
      }
      src = src.slice(0, sh.index) + mark("shadow", block) + src.slice(shEnd);
    }
    const marks = [...new Set([...src.matchAll(/\/\*pkr\+(\w+)\*\//g)].map((x) => x[1]))];
    if (marks.length) applied.push("variants without " + marks.join("/"));
    return { src, applied };
  }
  rt.rewriteShader = rewriteShader; // tests/shaders

  // config.txt plainshaders=1 (debug builds, measuring only): a textured fragment shader becomes
  // "sample the texture, nothing else" (its first texture2D call, unit 0's sampler for Phaser's
  // batch), so the GPU time left over is what the shaders don't cost. Colours and effects are wrong.
  function plainShader(src) {
    if (!/gl_FragColor/.test(src) || /gl_Position/.test(src)) return src;
    const use = /texture2D\s*\(\s*(\w+)\s*,\s*(\w+)\s*\)/.exec(src);
    if (!use || !new RegExp("varying\\s+(?:\\w+\\s+)?vec2\\s+" + use[2] + "\\s*;").test(src)) return src;
    const precision = (/^\s*precision\s+\w+\s+float\s*;/m.exec(src) || ["precision mediump float;"])[0].trim();
    return precision + "\nuniform sampler2D " + use[1] + ";\nvarying vec2 " + use[2] + ";\n" +
      "void main() { gl_FragColor = texture2D(" + use[1] + ", " + use[2] + "); }\n";
  }

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

  // Framebuffer attachments as WebGL reports them: getFramebufferAttachmentParameter(...,
  // FRAMEBUFFER_ATTACHMENT_OBJECT_NAME) gives the attached WebGLTexture or WebGLRenderbuffer, or null.
  // The native call answers the GL's number (vitaGL: a pointer for the colour attachment), and
  // Phaser's WebGLFramebufferWrapper.destroy deletes what it gets, so a number deleted nothing: every
  // destroyed render target left its depth/stencil renderbuffer behind (review-2026-10.md, finding 3).
  // Here the attachments are remembered per framebuffer as they are made.
  const FB = proto.FRAMEBUFFER, OBJECT_NAME = proto.FRAMEBUFFER_ATTACHMENT_OBJECT_NAME;
  const OBJECT_TYPE = proto.FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE;
  const attached = new WeakMap(); // framebuffer -> {attachment: [object, TEXTURE | RENDERBUFFER]}
  let boundFb = null;
  const nativeBindFb = proto.bindFramebuffer, nativeFbTex = proto.framebufferTexture2D;
  const nativeFbRb = proto.framebufferRenderbuffer, nativeFbParam = proto.getFramebufferAttachmentParameter;
  const attach = (att, obj, type) => {
    if (!boundFb) return;
    let rec = attached.get(boundFb);
    if (!rec) attached.set(boundFb, (rec = {}));
    if (obj) rec[att] = [obj, type];
    else delete rec[att];
  };
  Object.assign(proto, {
    bindFramebuffer(target, fb) {
      if (target === FB) boundFb = fb || null;
      return nativeBindFb.call(this, target, fb);
    },
    framebufferTexture2D(target, att, textarget, tex, level) {
      if (target === FB) attach(att, tex, proto.TEXTURE);
      return nativeFbTex.call(this, target, att, textarget, tex, level);
    },
    framebufferRenderbuffer(target, att, rbtarget, rb) {
      if (target === FB) attach(att, rb, proto.RENDERBUFFER);
      return nativeFbRb.call(this, target, att, rbtarget, rb);
    },
    getFramebufferAttachmentParameter(target, att, pname) {
      if (target !== FB || (pname !== OBJECT_NAME && pname !== OBJECT_TYPE)) return nativeFbParam.call(this, target, att, pname);
      const rec = boundFb && attached.get(boundFb), a = rec && rec[att];
      if (pname === OBJECT_NAME) return a ? a[0] : null;
      return a ? a[1] : proto.NONE;
    },
  });
  rt.fbAttachments = (fb) => attached.get(fb) || null; // tests/phaser

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
