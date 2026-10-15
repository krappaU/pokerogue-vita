// SPDX-License-Identifier: AGPL-3.0-only
// Checks js/lazy_ui.js filter-reset coalescing on the Pokedex (open it first): runs resetFilters +
// updateStarters with and without it and compares the resulting screen state. Run with
// scripts/drive.sh "evalfile scripts/probe_filter_coalesce.js" (slow: ~25 s of JS on desktop).
(() => {
  const s = __rt.renderer.game.scene.getScene('battle');
  const h = s.ui.handlers.find((x) => x && x.constructor.name === 'PokedexUiHandler');
  const sig = () => (h.filteredPokemonData || []).map((d) => d.species.speciesId).join(',') + '#' +
    (h.pokemonContainers || []).map((c) => (c.visible ? 1 : 0) + ':' + c.x + ',' + c.y + ':' + (c.species && c.species.speciesId)).join(';') + '#' +
    h.filterBar.dropDowns.map((d) => JSON.stringify(d.getSettings()) + d.cursor).join('|') + '#' + h.cursor + '#' + h.scrollCursor;
  if (!h.gameData) h.gameData = s.gameData;
  const run = (on) => { __rt.coalesceFilters = on; const t = performance.now(); h.resetFilters(); h.updateStarters(); return [performance.now() - t, sig()]; };
  const a = run(false), b = run(true), c = run(false), d = run(true);
  __rt.coalesceFilters = true;
  return 'original ' + a[0].toFixed(0) + '/' + c[0].toFixed(0) + ' ms, coalesced ' + b[0].toFixed(0) + '/' + d[0].toFixed(0) + ' ms, same: ' + (a[1] === b[1]) + ' ' + (c[1] === d[1]) + ' ' + (a[1] === c[1]) + ' len ' + a[1].length;
})()
