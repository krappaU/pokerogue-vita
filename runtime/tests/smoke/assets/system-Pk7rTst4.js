// SPDX-License-Identifier: AGPL-3.0-only
// GameData.parseSystemData (src/system/game-data.ts, Pagefault Games) as its bundler emits it, for the
// smoke test of src/gamefix.c. The classes it builds are parameters.
export function makeGameData(GameStats,Egg,Ribbons){return class GameData{static parseSystemData(e){let t=JSON.parse(e,(e,t)=>{if(e===`gameStats`)return new GameStats(t);if(e===`eggs`){let e=[];t===null&&(t=[]);for(let m of t)e.push(new Egg(m));return e}return e===`ribbons`?Ribbons.fromJSON(t):e.endsWith(`Attr`)&&![`natureAttr`,`abilityAttr`,`passiveAttr`].includes(e)?BigInt(t??0):t});return t.appliedMigrators??={},t}}}
