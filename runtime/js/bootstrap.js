// SPDX-License-Identifier: AGPL-3.0-only
// Entry point: install the browser shim, then load the live page like a browser would.
//   1. fetch index.html, parse it into document
//   2. register @font-face rules from <style> blocks with document.fonts
//   3. run inline classic scripts, fire DOMContentLoaded
//   4. import the module entry script(s); fire window "load"
(function (g) {
  "use strict";
  const N = g.__native;
  for (const f of ["core.js", "i18n_cache.js", "dom.js", "canvas2d.js", "webgl.js", "net.js", "storage.js", "input.js", "audio.js", "savecrypt.js"]) {
    N.loadScript(f);
  }
  const rt = g.__rt, doc = g.document;
  rt.onPhaserLoaded = rt.onPhaserLoaded || []; // shim code that needs Phaser registers here

  // Debugging aid: <data>/debug.js, if present, runs before the game loads (e.g. to install hooks).
  // g.__shaderHook(source), if a debug script defines it, can rewrite shader sources.
  const debugScript = N.readFile("debug.js");
  if (debugScript) {
    N.log("[debug] running debug.js from the data folder");
    try { (0, eval)(debugScript); } catch (e) { N.log("[debug] debug.js failed: " + e + "\n" + (e && e.stack)); }
  }

  // Runtime adjustments to Phaser, applied the moment the game's bundle assigns window.Phaser
  // (before any Phaser.Game exists):
  //  - pre-FX off: Phaser otherwise allocates 99 square render targets plus 3 full-screen ones up
  //    front (~180 MB of GPU memory at 1920x1080) for per-object effects PokeRogue never uses.
  //  - tweens with an onUpdate callback advance in steps of ~17 ms on slow frames (see below).
  //  - native render fast paths (js/fastpath.js); PKR_FASTPATH=0 in localStorage turns them off.
  let phaserValue;
  function onPhaser(P) {
    if (!P || !P.Renderer || !P.Renderer.WebGL || rt.phaserPatched) return;
    rt.phaserPatched = true;
    const PM = P.Renderer.WebGL.PipelineManager.prototype;
    const boot = PM.boot;
    PM.boot = function () {
      if (this.game && this.game.config) this.game.config.disablePreFX = true;
      return boot.apply(this, arguments);
    };
    // The renderer, so the host can tell it when GL state changed behind its back (debug line).
    const WR = P.Renderer.WebGL.WebGLRenderer.prototype, init = WR.init;
    WR.init = function () {
      rt.renderer = this;
      return init.apply(this, arguments);
    };
    // Game code written for 60 fps sometimes acts at most once per tween update: the shop reveals
    // one item per onUpdate of a 1250 ms counter (divided by the game speed), so at Vita frame
    // rates the last items never appeared. On frames longer than 25 ms, tweens with an onUpdate
    // callback take extra steps of the frame's time split up (at most 8); the others advance once
    // by the whole frame as before. Phaser 3.90's TweenManager.step otherwise.
    const TM = P.Tweens && P.Tweens.TweenManager && P.Tweens.TweenManager.prototype;
    if (TM && typeof TM.step === "function" && typeof TM.getDelta === "function" && g.localStorage.getItem("PKR_TWEEN_STEPS") !== "0") {
      const hasUpdateCb = (t) => !!(t.callbacks && t.callbacks.onUpdate);
      TM.step = function tweenStep(tick) {
        const e = this.getDelta(tick === true);
        if (e <= 0) return;
        this.processing = true;
        const list = this.tweens, done = [];
        const n = e > 25 ? Math.min(Math.ceil(e / 16.7), 8) : 1, sub = e / n;
        let finished = null;
        for (let k = 0; k < n - 1; k++) {
          for (let i = 0; i < list.length; i++) {
            const t = list[i];
            if (!hasUpdateCb(t) || (finished && finished.has(t))) continue;
            if (t.update(sub)) { (finished || (finished = new Set())).add(t); done.push(t); }
          }
        }
        for (let i = 0; i < list.length; i++) {
          const t = list[i];
          if (finished && finished.has(t)) continue;
          if (t.update(n > 1 && hasUpdateCb(t) ? sub : e)) done.push(t);
        }
        for (let i = 0; i < done.length && list.length; i++) {
          const t = done[i], j = list.indexOf(t);
          if (j > -1 && (t.isPendingRemove() || t.isDestroyed())) { list.splice(j, 1); t.destroy(); }
        }
        this.processing = false;
      };
    }
    for (const f of rt.onPhaserLoaded) f(P);
    N.loadScript("fastpath.js");
    N.loadScript("lazy_ui.js");
    const on = g.localStorage.getItem("PKR_FASTPATH") !== "0";
    rt.fastpath.set(on);
    N.log("[phaser] patched: pre-FX disabled, fast path " + (on ? "enabled" : "disabled"));
  }
  // Phaser caches GL state (bound program, textures, buffers, blending). Around native drawing over
  // its frame, flush and suspend it, then have it set everything up again.
  rt.beforeExternalGL = () => { const r = rt.renderer; if (r && r.pipelines) r.pipelines.clear(); };
  rt.afterExternalGL = () => { const r = rt.renderer; if (r && r.pipelines) r.pipelines.rebind(); };
  Object.defineProperty(g, "Phaser", {
    configurable: true,
    enumerable: true,
    get() { return phaserValue; },
    set(v) { phaserValue = v; onPhaser(v); },
  });

  function fontFacesFromCSS(css) {
    const out = [];
    for (const m of css.matchAll(/@font-face\s*{([^}]*)}/g)) {
      const body = m[1];
      const fam = /font-family\s*:\s*['"]?([^'";]+)['"]?/.exec(body);
      const src = /src\s*:\s*(url\([^)]*\))/.exec(body);
      if (!fam || !src) continue;
      const weight = (/font-weight\s*:\s*([^;]+)/.exec(body) || [])[1];
      const style = (/font-style\s*:\s*([^;]+)/.exec(body) || [])[1];
      out.push(new FontFace(fam[1].trim(), src[1], { weight: weight && weight.trim(), style: style && style.trim() }));
    }
    return out;
  }

  async function boot() {
    const t0 = N.now();
    const res = await g.fetch(g.location.href, { cache: "no-store" });
    if (!res.ok) throw new Error("page fetch failed: HTTP " + res.status);
    const html = await res.text();
    N.log("[boot] fetched page (" + html.length + " bytes) in " + (N.now() - t0).toFixed(0) + " ms");

    // Parse into a scratch root, then move <head>/<body> content into the real document.
    const scratch = new DocumentFragment();
    rt.parseInto(scratch, html, true);
    const find = (tag) => scratch.querySelector(tag);
    const head = find("head"), body = find("body"), htmlEl = find("html");
    if (htmlEl) for (const [k, v] of Object.entries(htmlEl._attrs)) doc.documentElement.setAttribute(k, v);
    if (head) for (const n of head.childNodes.slice()) doc.head.appendChild(n);
    if (body) for (const n of body.childNodes.slice()) doc.body.appendChild(n);
    if (body) for (const [k, v] of Object.entries(body._attrs)) doc.body.setAttribute(k, v);
    const title = doc.querySelector("title");
    if (title) doc.title = title.textContent;

    for (const style of doc.querySelectorAll("style")) {
      for (const face of fontFacesFromCSS(style.textContent)) doc.fonts.add(face);
    }

    doc.readyState = "interactive";
    const scripts = doc.querySelectorAll("script");
    for (const s of scripts) {
      const type = (s.getAttribute("type") || "").toLowerCase();
      if (type === "module") continue;
      if (type && type !== "text/javascript" && type !== "application/javascript") continue;
      if (s.getAttribute("src")) { N.log("[boot] external classic script skipped: " + s.getAttribute("src")); continue; }
      try { (0, eval)(s.textContent); } catch (e) { N.log("[boot] inline script failed: " + e); }
    }
    doc.dispatchEvent(new Event("DOMContentLoaded", { bubbles: true }));

    // Module scripts present in the HTML; scripts added later by JS run via HTMLScriptElement.
    for (const s of scripts) {
      if ((s.getAttribute("type") || "").toLowerCase() === "module" && s.getAttribute("src")) {
        s._parsed = false;
        s._connected();
      }
    }
    g.setTimeout(() => {
      doc.readyState = "complete";
      g.dispatchEvent(new Event("load"));
      N.log("[boot] load event after " + (N.now() - t0).toFixed(0) + " ms");
    }, 0);
  }

  boot().catch((e) => {
    N.log("[boot] failed: " + e + "\n" + (e && e.stack || ""));
    const net = /fetch|resolve|connect|SSL|timed? ?out/i.test(String(e));
    N.showMessage("PokeRogue could not start\n\n" + String(e).replace(/^TypeError: /, "") + "\n\n" +
      (net ? "Check that Wi-Fi is connected, then restart the app.\n\n" : "") +
      "Log: ux0:data/pokerogue/log.txt\nPress START + SELECT to quit.");
  });
})(globalThis);
