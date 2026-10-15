// SPDX-License-Identifier: AGPL-3.0-only
// Times the game's save encryption with the native AES (js/savecrypt.js) against crypto-js on a
// save-sized JSON string, and checks each side decrypts the other's output.
// scripts/drive.sh "evalfile scripts/probe_savecrypt.js"
(() => {
  const C = __rt.cryptoJS, o = __rt.origAes, N = __native;
  if (!C) return "native AES not installed";
  const data = { dexData: {}, starterData: {}, gameStats: { playTime: 12345 }, name: "Pokémon ★" };
  for (let i = 0; i < 1100; i++) {
    data.dexData[i] = { seenAttr: "1234567890123", caughtAttr: "987654321", natureAttr: 4194303, seenCount: i, caughtCount: i >> 1, hatchedCount: 0, ivs: [31, 31, 30, 29, 31, 20] };
    data.starterData[i] = { moveset: null, eggMoves: 15, candyCount: i % 50, friendship: 120, abilityAttr: 7, passiveAttr: 3, valueReduction: 2, classicWinCount: 1 };
  }
  const json = JSON.stringify(data), key = "x0i2O7WRiANTqPmZ";
  let t = N.now();
  const mine = C.AES.encrypt(json, key).toString();
  const tMine = N.now() - t;
  t = N.now();
  const theirs = o.encrypt.call(C.AES, json, key).toString();
  const tTheirs = N.now() - t;
  t = N.now();
  const back = C.AES.decrypt(theirs, key).toString(C.enc.Utf8);
  const tDec = N.now() - t;
  t = N.now();
  const back2 = o.decrypt.call(C.AES, mine, key).toString(C.enc.Utf8);
  const tDec2 = N.now() - t;
  return `${(json.length / 1024).toFixed(0)} KB JSON | encrypt native ${tMine.toFixed(1)} ms, crypto-js ${tTheirs.toFixed(1)} ms | ` +
    `decrypt native ${tDec.toFixed(1)} ms, crypto-js ${tDec2.toFixed(1)} ms | round trips ok: ${back === json} ${back2 === json}`;
})()
