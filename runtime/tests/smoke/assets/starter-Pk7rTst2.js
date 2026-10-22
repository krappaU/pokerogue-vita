// SPDX-License-Identifier: AGPL-3.0-only
// Two pieces of PokeRogue's starter select screen (src/ui/handlers/starter-select-ui-handler.ts,
// Pagefault Games) as its bundler emits them, for the smoke test of src/gamefix.c: the setup loop
// that creates the 81 starter slots, and filterStarters (its filter shortened to two filters). The
// bundle's species registry (z) is set with useRegistry.
let z;
export function useRegistry(r){z=r}
export function makeSlots(calcStarterContainerPosition,StarterContainer){const C=[];for(let e=0;e<81;e++){let t=calcStarterContainerPosition(e),m=new StarterContainer(z.getAllStarters()[e]);C.push([t,m])}return C}
export class StarterSelect{constructor(e){this.filterBar=e}filterStarters(){this.filteredStarterIds=z.getAllStarters().filter(e=>{let t=z.getSpecies(e),S=this.filterBar.getVals(0).includes(t.generation),C=this.filterBar.getVals(1).some(e=>t.isOfType(e-1)),ue=this.filterBar.getVals(5).some(e=>e.val===`FAVORITE`&&e.state===0?t.favorite:e.val===`FAVORITE`&&e.state===2?!t.favorite:e.val===`FAVORITE`&&e.state===1);return!!(S&&C&&ue)})}}
