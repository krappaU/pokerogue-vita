// SPDX-License-Identifier: AGPL-3.0-only
// navigator, keyboard / pointer events from the native platform layer, and the Gamepad API.
(function (g) {
  "use strict";
  const N = g.__native, rt = g.__rt;

  // ---------------------------------------------------------------- gamepads
  class GamepadButton { constructor(v) { this.value = v; this.pressed = v > 0.5; this.touched = this.pressed; } }
  const known = new Map(); // index -> Gamepad
  function pollPads() {
    const raw = N.gamepads();
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
