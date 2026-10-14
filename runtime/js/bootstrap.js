// SPDX-License-Identifier: AGPL-3.0-only
// Entry point: install the browser shim, then load the live page like a browser would.
//   1. fetch index.html, parse it into document
//   2. register @font-face rules from <style> blocks with document.fonts
//   3. run inline classic scripts, fire DOMContentLoaded
//   4. import the module entry script(s); fire window "load"
(function (g) {
  "use strict";
  const N = g.__native;
  for (const f of ["core.js", "dom.js", "canvas2d.js", "webgl.js", "net.js", "storage.js", "input.js", "audio.js"]) {
    N.loadScript(f);
  }
  const rt = g.__rt, doc = g.document;

  // Native render fast paths (js/fastpath.js) attach to Phaser once the game has loaded it.
  // PKR_FASTPATH=0 in localStorage turns them off for A/B measurements.
  const fpHook = () => {
    if (!g.Phaser || !g.Phaser.Renderer) return;
    rt.frameHooks.splice(rt.frameHooks.indexOf(fpHook), 1);
    N.loadScript("fastpath.js");
    const on = g.localStorage.getItem("PKR_FASTPATH") !== "0";
    rt.fastpath.set(on);
    N.log("[fastpath] " + (on ? "enabled" : "disabled"));
  };
  rt.frameHooks.push(fpHook);

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

  boot().catch((e) => N.log("[boot] failed: " + e + "\n" + (e && e.stack || "")));
})(globalThis);
