// SPDX-License-Identifier: AGPL-3.0-only
// A bot that plays PokeRogue for scripts/test_extended.sh: loaded with the control channel's
// `evalfile`, after `globalThis.__botConfig = {...}`. It looks at the game's UI mode a few times a
// second and presses the game's own buttons (ui.processInput, as the input controller does): fights
// with the first usable move, takes the first reward, sends out the next Pokemon when one faints,
// answers yes/no questions (no, except the few that only end with a yes), and goes through
// evolutions, hatches, mystery encounters and menus. It starts the run from the title (Continue, or
// New Game -> Classic -> the first starters on a fresh save).
// It logs "[bot] ..." lines: one per wave, and "[bot] done {...}" (JSON) when it stops: the target
// wave reached, back at the title (game over or the end of the run), or no new turn nor wave for
// stuckSeconds.
// Config: targetWave (absolute), or waves (more than the first wave seen); gameSpeed (the game's
// own setting, 1-5); stuckSeconds; starters (fresh save); heal (the party at full HP, PP and no
// status at each new wave).
(function (g) {
  "use strict";
  const N = g.__native, rt = g.__rt;
  const cfg = Object.assign({ waves: 10, gameSpeed: 5, stuckSeconds: 180, starters: 3, tickMs: 100, heal: false }, g.__botConfig || {});
  const BTN = { UP: 0, DOWN: 1, LEFT: 2, RIGHT: 3, SUBMIT: 4, ACTION: 5, CANCEL: 6 };
  const MODE = { MESSAGE: 0, TITLE: 1, COMMAND: 2, FIGHT: 3, BALL: 4, TARGET_SELECT: 5, MODIFIER_SELECT: 6, PARTY: 8, SUMMARY: 9,
    STARTER_SELECT: 10, EVOLUTION_SCENE: 11, EGG_HATCH_SCENE: 12, EGG_HATCH_SUMMARY: 13, CONFIRM: 14, OPTION_SELECT: 15, MENU: 16,
    MENU_OPTION_SELECT: 17, MYSTERY_ENCOUNTER: 45 };
  // yes/no questions answered yes (else no): the ones a no would only ask again
  const YES = [/begin with these/i, /stop trying to teach/i, /give up/i, /are you sure/i, /continue/i];
  const start = N.now();
  const st = { started: false, firstWave: 0, lastWave: 0, waveAt: start, lastTurn: -1, progressAt: start, turns: 0, lastMode: -1, sameMode: 0, presses: {}, confirmSeen: {},
    shopPresses: 0, partyPresses: 0, fightPresses: 0, done: false, waves: [] };
  let last = 0;
  const secs = () => ((N.now() - start) / 1000).toFixed(0);
  const log = (s) => N.log("[bot] " + s);

  function finish(reason, extra) {
    if (st.done) return;
    st.done = true;
    const r = Object.assign({ reason, firstWave: st.firstWave, lastWave: st.lastWave, seconds: +secs(), presses: st.presses, waves: st.waves }, extra || {});
    log("done " + JSON.stringify(r));
    rt.frameHooks.splice(rt.frameHooks.indexOf(hook), 1);
  }
  function press(ui, b, mode) {
    st.presses[mode] = (st.presses[mode] || 0) + 1;
    ui.processInput(b);
  }
  function messageText(ui) {
    try {
      const m = ui.getMessageHandler && ui.getMessageHandler();
      const t = m && m.message && m.message.text;
      return typeof t === "string" ? t.replace(/\s+/g, " ").trim() : "";
    } catch (e) { return ""; }
  }

  // heal: the party at full HP and PP, without status, at each new wave (a long run for the
  // runtime's sake shouldn't end on the bot's tactics; the game over path has its own scenario)
  function heal(s) {
    for (const p of typeof s.getPlayerParty === "function" ? s.getPlayerParty() : []) {
      try {
        p.hp = p.getMaxHp();
        if (typeof p.resetStatus === "function") p.resetStatus(true, false, false); else p.status = null;
        for (const m of p.getMoveset()) if (m) m.ppUsed = 0;
        if (typeof p.updateInfo === "function") p.updateInfo(true);
      } catch (e) { log("heal: " + e); }
    }
  }
  function tick() {
    const game = rt.renderer && rt.renderer.game, s = game && game.scene.getScene("battle");
    const ui = s && s.ui;
    if (!ui || typeof ui.processInput !== "function") return;
    const now = N.now();
    if (typeof s.gameSpeed === "number" && cfg.gameSpeed && s.gameSpeed !== cfg.gameSpeed) s.gameSpeed = cfg.gameSpeed;
    const b = s.currentBattle, wave = b ? b.waveIndex | 0 : 0, mode = ui.mode;
    if (wave && wave !== st.lastWave) {
      if (!st.firstWave) st.firstWave = wave;
      const took = ((now - st.waveAt) / 1000).toFixed(0);
      if (st.lastWave) st.waves.push([st.lastWave, +took, st.turns]);
      log("wave " + wave + " (" + (b.double ? "double" : "single") + (b.trainer ? ", trainer" : "") + ") at " + secs() + " s" +
        (st.lastWave ? "; wave " + st.lastWave + " took " + took + " s, " + st.turns + " turns" : ""));
      st.lastWave = wave, st.waveAt = now, st.progressAt = now, st.turns = 0, st.lastTurn = b.turn;
      if (cfg.heal) heal(s);
      const target = cfg.targetWave || (st.firstWave + cfg.waves);
      if (wave >= target) return finish("target");
    }
    if (b && b.turn !== st.lastTurn) st.lastTurn = b.turn, st.turns++, st.progressAt = now;
    if (st.firstWave && !wave && mode === MODE.TITLE) return finish("title"); // after a battle: game over or the run's end
    // stuck: no new wave nor turn for stuckSeconds
    if (now - st.progressAt > cfg.stuckSeconds * 1000) {
      const h = ui.getHandler && ui.getHandler();
      return finish("stuck", { mode, handler: h && h.constructor && h.constructor.name, message: messageText(ui).slice(0, 160) });
    }
    if (mode === st.lastMode) st.sameMode++; else st.sameMode = 0, st.lastMode = mode;
    if (mode !== MODE.MODIFIER_SELECT && mode !== MODE.PARTY) st.shopPresses = 0;
    if (mode !== MODE.PARTY) st.partyPresses = 0;
    if (mode !== MODE.FIGHT) st.fightPresses = 0;
    const h = ui.getHandler && ui.getHandler();
    switch (mode) {
      case MODE.TITLE:
        // Continue on a saved run, New Game on a fresh save: both are the first option
        if (!st.started) { st.started = true; log("from the title"); }
        if (h && typeof h.setCursor === "function" && h.cursor !== 0) h.setCursor(0);
        return press(ui, BTN.ACTION, mode);
      case MODE.STARTER_SELECT: {
        st.started = true;
        const n = h && Array.isArray(h.partyStarterIds) ? h.partyStarterIds.length : 0;
        if (n >= cfg.starters || (n && st.sameMode > 40)) {
          if (h && typeof h.tryStart === "function" && st.sameMode % 8 === 0) h.tryStart();
          return;
        }
        // the next starter: cursor right, then add it (the option list's first entry)
        if (st.sameMode % 4 === 0 && n && h && typeof h.setCursor === "function") h.setCursor(Math.min(n * 1, 80));
        return press(ui, BTN.ACTION, mode);
      }
      case MODE.COMMAND:
        st.started = true;
        if (h && typeof h.setCursor === "function" && h.cursor !== 0) h.setCursor(0);
        return press(ui, BTN.ACTION, mode);
      case MODE.FIGHT: {
        // a usable move (PP left, not disabled...), the next usable one after a press went nowhere
        const field = typeof s.getPlayerField === "function" ? s.getPlayerField() : [];
        const p = field[(h && h.fieldIndex) | 0];
        const moves = p && typeof p.getMoveset === "function" ? p.getMoveset() : [];
        const usable = moves.map((m, i) => { try { return m && m.isUsable(p)[0] ? i : -1; } catch (e) { return -1; } }).filter((i) => i >= 0);
        // strongest first, by the game's own simulated damage against the first enemy (the estimate
        // its AI uses); status moves last
        const foe = (typeof s.getEnemyField === "function" ? s.getEnemyField() : []).find((e) => e && e.hp > 0);
        const dmg = (i) => {
          try { return foe ? foe.getAttackDamage({ source: p, move: moves[i].getMove(), ignoreAbility: false, ignoreSourceAbility: false, isCritical: false, simulated: true }).damage | 0 : 0; } catch (e) { return 0; }
        };
        if (st.fightPresses === 0 && foe) {
          const d = new Map(usable.map((i) => [i, dmg(i)]));
          usable.sort((a, b) => d.get(b) - d.get(a));
          st.moveOrder = usable;
        }
        const order = st.fightPresses && st.moveOrder ? st.moveOrder : usable;
        if (order.length && h && typeof h.setCursor === "function") {
          const pick = order[Math.floor(st.fightPresses / 3) % order.length];
          if (h.cursor !== pick) h.setCursor(pick);
        }
        st.fightPresses++;
        return press(ui, BTN.ACTION, mode);
      }
      case MODE.MODIFIER_SELECT:
        // the first reward; if picking one goes nowhere, skip them (cancel, then yes)
        if (++st.shopPresses > 16) return press(ui, BTN.CANCEL, mode);
        return press(ui, BTN.ACTION, mode);
      case MODE.PARTY: {
        // a Pokemon that can go (not fainted; not already in battle), then its first option (send
        // out, apply...); the next one when that leads nowhere; back out after a while
        st.partyPresses++;
        if (st.partyPresses > 40) return press(ui, BTN.CANCEL, mode);
        if (h && h.optionsMode) return press(ui, BTN.ACTION, mode);
        const party = typeof s.getPlayerParty === "function" ? s.getPlayerParty() : [];
        const ok = party.map((p, i) => (p && p.hp > 0 && !(typeof p.isOnField === "function" && p.isOnField()) ? i : -1)).filter((i) => i >= 0);
        const pool = ok.length ? ok : party.map((p, i) => i);
        const pick = pool[Math.floor((st.partyPresses - 1) / 2) % pool.length];
        if (h && typeof h.setCursor === "function" && h.cursor !== pick) h.setCursor(pick);
        return press(ui, BTN.ACTION, mode);
      }
      case MODE.CONFIRM: {
        const text = messageText(ui), key = text.slice(0, 60);
        const seen = (st.confirmSeen[key] = (st.confirmSeen[key] || 0) + 1);
        const yes = YES.some((r) => r.test(text)) || seen > 3; // asked again and again: the other answer
        if (seen === 1) log("asked \"" + text.slice(0, 100) + "\": " + (yes ? "yes" : "no"));
        // the confirm window's cursor: 0 yes, 1 no
        if (h && typeof h.setCursor === "function") h.setCursor(yes ? 0 : 1);
        return press(ui, BTN.ACTION, mode);
      }
      case MODE.SUMMARY:
      case MODE.MENU:
      case MODE.BALL:
      case MODE.EGG_HATCH_SUMMARY:
        return press(ui, BTN.CANCEL, mode);
      case MODE.MESSAGE:
      case MODE.TARGET_SELECT:
      case MODE.OPTION_SELECT:
      case MODE.MENU_OPTION_SELECT:
      case MODE.EVOLUTION_SCENE:
      case MODE.EGG_HATCH_SCENE:
      case MODE.MYSTERY_ENCOUNTER:
        return press(ui, BTN.ACTION, mode);
      default:
        // anything else: confirm, and back out now and then
        return press(ui, st.sameMode % 6 === 5 ? BTN.CANCEL : BTN.ACTION, mode);
    }
  }
  function hook() {
    const now = N.now();
    if (now - last < cfg.tickMs || st.done) return;
    last = now;
    try { tick(); } catch (e) { finish("bot error", { error: String(e) + " " + (e && e.stack) }); }
  }
  log("started " + JSON.stringify(cfg));
  rt.frameHooks.push(hook);
})(globalThis);
