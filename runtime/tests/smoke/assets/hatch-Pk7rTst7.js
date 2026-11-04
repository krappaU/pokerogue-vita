// SPDX-License-Identifier: AGPL-3.0-only
// Egg.generatePlayerPokemon (src/data/egg.ts) and Pokemon.init (src/field/pokemon.ts, Pagefault Games)
// as their bundler emits them, for the smoke test of src/gamefix.c. The scene, battle info class
// and log are parameters.
export function make(b,Info,log){class Pokemon{constructor(){this.battleInfo=void 0,this.init()}init(){this.fieldPosition=1,this.initBattleInfo(),b.fieldUI.addAt(this.battleInfo,0);let getSprite=()=>0;getSprite()}initBattleInfo(){this.battleInfo=new Info,log.push(`built`)}destroy(){this.battleInfo?.destroy()}}class Egg{generatePlayerPokemon(){let e,generatePlayerPokemonHelper=()=>{e=new Pokemon};return generatePlayerPokemonHelper(),e}}return{Egg,Pokemon}}
