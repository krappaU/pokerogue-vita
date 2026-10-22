// SPDX-License-Identifier: AGPL-3.0-only
// navigator, keyboard / pointer events from the native platform layer, and the Gamepad API.
(function (g) {
  "use strict";
  const N = g.__native, rt = g.__rt;

  // ---------------------------------------------------------------- gamepads
  class GamepadButton { constructor(v) { this.value = v; this.pressed = v > 0.5; this.touched = this.pressed; } }
  const known = new Map(); // index -> Gamepad
  // N.gamepads() returns the same array as last time while no button or stick changed: the pads
  // (and their button objects) stay as they are, only their timestamp moves on. _pkrSeq counts the
  // changes, for Phaser's Gamepad.update below.
  let lastRaw = null, padSeq = 0;
  function pollPads() {
    const raw = N.gamepads();
    if (raw === lastRaw) {
      const now = g.performance.now();
      for (const p of padSnapshot) if (p) p.timestamp = now;
      return padSnapshot;
    }
    lastRaw = raw;
    padSeq++;
    const out = [null, null, null, null];
    raw.forEach((p, i) => {
      let pad = known.get(i);
      const isNew = !pad || pad.id !== p.id;
      if (isNew) {
        pad = { id: p.id, index: i, connected: true, mapping: "standard", timestamp: 0, axes: [], buttons: [], vibrationActuator: null, hapticActuators: [] };
        known.set(i, pad);
      }
      pad.axes = p.axes.slice();
      pad.buttons = p.buttons.map((v) => new GamepadButton(v));
      pad.timestamp = g.performance.now();
      pad._pkrSeq = padSeq;
      out[i] = pad;
      if (isNew) g.dispatchEvent(new GamepadEvent("gamepadconnected", { gamepad: pad }));
    });
    for (const [i, pad] of known) {
      if (i >= raw.length) { known.delete(i); pad.connected = false; g.dispatchEvent(new GamepadEvent("gamepaddisconnected", { gamepad: pad })); }
    }
    return out;
  }
  let padSnapshot = [null, null, null, null];
  rt.frameHooks.push(() => { padSnapshot = pollPads(); });

  // ---------------------------------------------------------------- navigator
  // A desktop Chrome identity: the game shows on-screen touch controls for mobile user agents,
  // and the Vita has real buttons.
  g.navigator = rt.tracked({
    userAgent: "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0 Safari/537.36 PokeRogueVita/0.1",
    appVersion: "5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0 Safari/537.36",
    appName: "Netscape", appCodeName: "Mozilla", product: "Gecko", productSub: "20030107",
    vendor: "Google Inc.", vendorSub: "",
    platform: "Linux x86_64",
    language: "en-US",
    languages: ["en-US", "en"],
    hardwareConcurrency: 1,
    deviceMemory: 0.5,
    maxTouchPoints: 0,
    onLine: true,
    cookieEnabled: true,
    doNotTrack: null,
    pdfViewerEnabled: false,
    webdriver: false,
    userActivation: { isActive: true, hasBeenActive: true },
    getGamepads: () => padSnapshot,
    vibrate: () => false,
    sendBeacon: () => true,
    clipboard: { writeText: () => Promise.resolve(), readText: () => Promise.resolve("") },
    storage: { estimate: () => Promise.resolve({ quota: 0, usage: 0 }), persist: () => Promise.resolve(false) },
    locks: undefined,
  }, "navigator");

  // ---------------------------------------------------------------- text entry
  // When an enabled text <input> has focus, open the system keyboard (on Linux, a control-channel
  // stand-in) once per focus. The result goes into the field with the same events a browser fires.
  // Focus then moves on if the next field is a password field (username -> password -> confirm),
  // which is how PokeRogue's login and register forms are laid out. The game may focus a field
  // while it is still disabled (form animating in), so this is checked every frame rather than
  // at focus time. The fields are HTML elements the runtime doesn't draw; the keyboard dialog is
  // where the user sees the text.
  let imeTarget = null, focusSeq = 0, handledSeq = 0;
  const TEXT_TYPES = new Set(["text", "password", "email", "search", "tel", "url", "number", ""]);
  rt.onInputFocus = () => { focusSeq++; };
  rt.frameHooks.push(() => {
    if (imeTarget) {
      const r = N.imePoll();
      if (!r) return;
      const el = imeTarget;
      imeTarget = null;
      if (!r.ok) return;
      el.value = r.text;
      el.dispatchEvent(new Event("input", { bubbles: true }));
      el.dispatchEvent(new Event("change", { bubbles: true }));
      const all = g.document.querySelectorAll("input");
      const next = all[all.indexOf(el) + 1];
      if (next && next.type === "password" && !next.readOnly) g.setTimeout(() => next.focus(), 0);
      return;
    }
    const el = g.document.activeElement;
    if (!el || handledSeq === focusSeq || !(el instanceof HTMLInputElement)) return;
    if (el.readOnly || el.disabled || !TEXT_TYPES.has(el.type)) return;
    handledSeq = focusSeq;
    const password = el.type === "password";
    // The game's fields have no labels: name them from the form's layout.
    const all = g.document.querySelectorAll("input"), next = all[all.indexOf(el) + 1];
    const title = el.placeholder || (password ? "Password" : next && next.type === "password" ? "Username" : "Text");
    if (N.imeOpen(title, el.value || "", el.maxLength > 0 ? el.maxLength : 64, password)) imeTarget = el;
  });

  // ---------------------------------------------------------------- drawing the text fields
  // The game's text fields are <input> elements placed over the canvas (Phaser DOMElements); a
  // browser draws them, this runtime doesn't. So each visible field's text is drawn here with the
  // field's own font and colour (passwords as asterisks) and handed to the native layer, which
  // draws it over the game. A field counts as visible if Phaser rendered its element this frame.
  let frameNo = 0, lastSig = "", inputs = [];
  const fieldCanvases = [];
  (rt.onPhaserLoaded || (rt.onPhaserLoaded = [])).push((P) => {
    const DE = P.GameObjects.DOMElement && P.GameObjects.DOMElement.prototype;
    if (!DE || !DE.renderWebGL) return;
    const render = DE.renderWebGL;
    DE.renderWebGL = function (renderer, src) { if (src) src._pkrSeen = frameNo; return render.apply(this, arguments); };
    // Phaser 3.90's DOMElement.preRender hides the element of a field whose container won't render,
    // every frame: the game's ten login and registration fields, hidden all game long, wrote
    // display = "none" through the style object (a Proxy) each frame. Writing it again when it is
    // already "none" changes nothing, so that write is left out (the container check still runs).
    if (/^function\s*\w*\(\)\s*\{\s*var (\w+)\s*=\s*this\.parentContainer,\s*(\w+)\s*=\s*this\.node;?\s*\2\s*&&\s*\1\s*&&\s*!\1\.willRender\(\)\s*&&\s*\(\2\.style\.display\s*=\s*[`"']none[`"']\);?\s*\}$/.test(String(DE.preRender))) {
      const orig = DE.preRender;
      DE.preRender = function () {
        const c = this.parentContainer, node = this.node;
        if (node && c && !c.willRender() && !(node._styleProps && node._styleProps.display === "none")) node.style.display = "none";
      };
      rt.domPreRenderSwap = [orig, DE.preRender]; // tests/phaser
    } else N.log("[input] DOMElement.preRender is not Phaser 3.90's: left as it is");
  });
  // Phaser's Gamepad.update(pad) passes every button and axis value to its Button and Axis objects,
  // every frame. With the same values as last time that changes nothing (a value is stored again;
  // presses and releases are only emitted on a change), so a pad that hasn't changed since this
  // Gamepad's last update is skipped.
  rt.onPhaserLoaded.push((P) => {
    const GP = P.Input && P.Input.Gamepad && P.Input.Gamepad.Gamepad && P.Input.Gamepad.Gamepad.prototype;
    if (!GP || typeof GP.update !== "function") return;
    const update = GP.update;
    GP.update = function (pad) {
      if (pad && pad === this._pkrPad && pad._pkrSeq === this._pkrSeq) return;
      this._pkrPad = pad;
      this._pkrSeq = pad && pad._pkrSeq;
      return update.call(this, pad);
    };
    rt.gamepadSwap = [update, GP.update]; // tests/phaser
  });
  function drawField(fc, text, font, color, w, h) {
    const cv = fc.canvas;
    cv.width = Math.ceil(w);
    cv.height = Math.ceil(h);
    const ctx = cv.getContext("2d");
    ctx.font = font;
    ctx.fillStyle = color;
    ctx.textBaseline = "middle";
    const tw = ctx.measureText(text).width;
    ctx.fillText(text, tw > w - 4 ? w - 2 - tw : 2, h / 2); // long text: show its end, like a browser
    return ctx.getImageData(0, 0, cv.width, cv.height);
  }
  let fieldShown = false;
  function updateFields() {
    if (frameNo % 30 === 1) inputs = Array.from(g.document.querySelectorAll("input"));
    const out = [];
    let sig = "", shown = false;
    for (let k = 0; k < inputs.length; k++) {
      const el = inputs[k], go = el.phaser;
      // drawn last frame (cheap checks first: this runs every frame for every input)
      if (!go || go._pkrSeen === undefined || frameNo - go._pkrSeen > 1) continue;
      shown = true;
      if (!el.value || out.length >= 8) continue;
      const st = el.style;
      if (st.display === "none" || st.visibility === "hidden" || parseFloat(st.opacity) === 0) continue;
      const m = /matrix\(([^)]*)\)/.exec(st.transform || "");
      const w = parseFloat(st.width) || 0, h = parseFloat(st.height) || 0;
      if (!m || !w || !h) continue;
      const [a, , , d, e, f] = m[1].split(",").map(Number);
      const text = el.type === "password" ? "*".repeat(el.value.length) : el.value;
      // The game sets a unitless font-size ("64"), which browsers read as pixels here.
      const fs = parseFloat(st.fontSize) || parseFloat((/font-size:\s*([0-9.]+)/.exec(st.cssText || "") || [])[1]) || h * 0.55;
      const font = fs + "px " + (st.fontFamily || "sans-serif");
      const color = st.color || "#000";
      const key = text + "|" + font + "|" + color + "|" + w + "x" + h;
      const i = out.length;
      const fc = fieldCanvases[i] || (fieldCanvases[i] = { key: null, canvas: g.document.createElement("canvas") });
      const item = { x: e, y: f, w: w * a, h: h * d };
      if (fc.key !== key) {
        fc.key = key;
        const img = drawField(fc, text, font, color, w, h);
        item.pix = img.data.buffer;
        item.pw = img.width;
        item.ph = img.height;
      }
      sig += key + "@" + item.x + "," + item.y + "," + item.w + ";";
      out.push(item);
    }
    if (sig !== lastSig) {
      lastSig = sig;
      N.setFieldOverlays(out);
    }
    if (shown !== fieldShown && N.setTextFieldShown) N.setTextFieldShown((fieldShown = shown));
  }
  rt.frameHooks.push(() => { frameNo++; updateFields(); });

  // ---------------------------------------------------------------- keyboard and pointer
  const down = new Set();
  rt.onInput = function (type, key, code, keyCode, x, y, id) {
    if (type === "keydown" || type === "keyup") {
      const repeat = type === "keydown" && down.has(code);
      if (type === "keydown") down.add(code); else down.delete(code);
      const target = g.document.activeElement || g.document.body;
      const ev = new KeyboardEvent(type, { key, code, keyCode, repeat, bubbles: true, cancelable: true });
      ev.isTrusted = true;
      target.dispatchEvent(ev);
      if (rt.onKeyForInput) rt.onKeyForInput(type, key, ev);
      return;
    }
    const canvas = rt.mainCanvas && rt.mainCanvas();
    const target = canvas || g.document.body;
    const map = { pointerdown: ["pointerdown", "mousedown"], pointermove: ["pointermove", "mousemove"], pointerup: ["pointerup", "mouseup"] }[type];
    const buttons = type === "pointerup" ? 0 : 1;
    for (const t of map) {
      const Ctor = t.startsWith("pointer") ? PointerEvent : MouseEvent;
      const ev = new Ctor(t, { clientX: x, clientY: y, button: 0, buttons, pointerId: id + 1, bubbles: true, cancelable: true });
      ev.isTrusted = true;
      target.dispatchEvent(ev);
    }
  };
})(globalThis);
