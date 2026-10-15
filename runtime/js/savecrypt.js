// SPDX-License-Identifier: AGPL-3.0-only
// The game keeps encrypted copies of its saves in localStorage: crypto-js's AES.encrypt(json,
// passphrase).toString() on every save, AES.decrypt(...).toString(Utf8) when loading. Pure-JS AES,
// base64 and UTF-8 conversion of a whole save took 7-16 s per save on the Vita. Here crypto-js's
// AES.encrypt/decrypt get native versions (src/savecrypt.c, OpenSSL) for that call pattern; their
// results are crypto-js objects whose fields are filled in only if something reads them.
//
// crypto-js lives inside the game's bundle, so it is found when it sets itself up: its first
// statements are `var C = {}; var C_lib = C.lib = {};`, and an Object.prototype setter for "lib"
// sees that object (then turns into a normal property). Before switching, the native versions are
// checked against crypto-js both ways; any difference keeps crypto-js. localStorage PKR_NATIVE_AES=0
// turns this off.
(function (g) {
  "use strict";
  const rt = g.__rt, N = g.__native;
  if (typeof N.aesEncrypt !== "function") return;
  try { if (g.localStorage.getItem("PKR_NATIVE_AES") === "0") return; } catch (e) { /* no storage yet */ }

  const candidates = [];
  let trapped = true;
  Object.defineProperty(Object.prototype, "lib", {
    configurable: true, enumerable: false,
    get() { return undefined; },
    set(v) {
      Object.defineProperty(this, "lib", { value: v, writable: true, enumerable: true, configurable: true });
      if (candidates.length < 16) candidates.push(this);
    },
  });
  function untrap() {
    if (trapped) { delete Object.prototype.lib; trapped = false; }
  }

  const stats = { encrypts: 0, decrypts: 0, bytes: 0, ms: 0, fallbacks: 0 };
  rt.saveCryptStats = () => stats;

  // Defines obj[name] to be computed on first access (or replaced by an assignment).
  function lazy(obj, name, make, onTouch) {
    const set = (v) => { Object.defineProperty(obj, name, { value: v, writable: true, enumerable: true, configurable: true }); onTouch(); };
    Object.defineProperty(obj, name, { configurable: true, enumerable: true, get() { const v = make(); set(v); return v; }, set });
  }

  function install(C) {
    const helper = C.AES, origEnc = helper.encrypt, origDec = helper.decrypt;
    const Utf8 = C.enc.Utf8, WordArray = C.lib.WordArray, CipherParams = C.lib.CipherParams;
    const sample = origEnc.call(helper, "x", "k");
    const shared = { algorithm: sample.algorithm, mode: sample.mode, padding: sample.padding, blockSize: sample.blockSize, formatter: sample.formatter };
    const words = (buf) => WordArray.create(N.bytesToWords(buf), buf.byteLength);

    function encrypt(message, key, cfg) {
      if (cfg === undefined && typeof message === "string" && typeof key === "string") {
        const t0 = N.now(), r = N.aesEncrypt(message, key);
        if (r) {
          const [b64, salt, k, iv, ct] = r;
          let touched = false;
          const touch = () => { touched = true; };
          const cp = CipherParams.create(Object.assign({}, shared));
          lazy(cp, "ciphertext", () => words(ct), touch);
          lazy(cp, "salt", () => words(salt), touch);
          lazy(cp, "key", () => words(k), touch);
          lazy(cp, "iv", () => words(iv), touch);
          cp.toString = function (fmt) {
            const f = fmt || this.formatter;
            return !touched && f === shared.formatter ? b64 : f.stringify(this);
          };
          stats.encrypts++; stats.bytes += message.length; stats.ms += N.now() - t0;
          return cp;
        }
        stats.fallbacks++;
      }
      return origEnc.apply(this, arguments);
    }

    function decrypt(ciphertext, key, cfg) {
      if (cfg === undefined && typeof ciphertext === "string" && typeof key === "string") {
        const t0 = N.now(), r = N.aesDecrypt(ciphertext, key);
        if (r) {
          const [str, pt] = r;
          let touched = false;
          const touch = () => { touched = true; };
          const wa = WordArray.create([], 0);
          lazy(wa, "words", () => N.bytesToWords(pt), touch);
          lazy(wa, "sigBytes", () => pt.byteLength, touch);
          wa.toString = function (enc) {
            if (enc === Utf8 && !touched && str !== null) return str;
            return WordArray.toString.call(this, enc);
          };
          stats.decrypts++; stats.bytes += ciphertext.length; stats.ms += N.now() - t0;
          return wa;
        }
        stats.fallbacks++;
      }
      return origDec.apply(this, arguments);
    }

    // Both directions against crypto-js, with multi-byte characters and the empty string.
    const pass = "pkr-check-é", msgs = ["", "x", "PokéRogue ✓ 🎉 " + JSON.stringify({ a: [1, 2, 3], b: "z".repeat(100) })];
    for (const m of msgs) {
      const mine = encrypt(m, pass), theirs = origEnc.call(helper, m, pass).toString();
      if (origDec.call(helper, mine.toString(), pass).toString(Utf8) !== m) throw new Error("native encrypt differs");
      const cp = encrypt(m, pass), s1 = cp.toString();
      if (shared.formatter.stringify(cp) !== s1) throw new Error("ciphertext fields differ");
      const d = decrypt(theirs, pass), ref = origDec.call(helper, theirs, pass);
      if (d.toString(Utf8) !== m) throw new Error("native decrypt differs");
      // (crypto-js leaves the padding bytes in its words past sigBytes; Hex reads up to sigBytes)
      if (d.sigBytes !== ref.sigBytes || C.enc.Hex.stringify(d) !== C.enc.Hex.stringify(ref)) throw new Error("decrypted bytes differ");
    }
    stats.encrypts = stats.decrypts = stats.bytes = stats.ms = stats.fallbacks = 0;
    helper.encrypt = encrypt;
    helper.decrypt = decrypt;
    rt.cryptoJS = C; // for scripts/probe_savecrypt.js
    rt.origAes = { encrypt: origEnc, decrypt: origDec };
  }

  const isCryptoJS = (c) => c && c.AES && typeof c.AES.encrypt === "function" && c.enc && c.enc.Utf8 && c.enc.Hex &&
    c.lib && c.lib.WordArray && c.lib.CipherParams && c.format && c.format.OpenSSL;
  function check() {
    const C = candidates.find(isCryptoJS);
    if (!C) return false;
    untrap();
    candidates.length = 0;
    try {
      install(C);
      N.log("[savecrypt] native AES for the game's save encryption");
    } catch (e) {
      N.log("[savecrypt] keeping crypto-js: " + e.message);
    }
    return true;
  }
  // crypto-js finishes setting up (AES is its last part) within the module that loads it; check
  // once per frame until then.
  const hook = () => {
    if (!check()) return;
    const i = rt.frameHooks.indexOf(hook);
    if (i >= 0) rt.frameHooks.splice(i, 1);
  };
  rt.frameHooks.push(hook);
})(globalThis);
