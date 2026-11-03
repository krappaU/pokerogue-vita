// SPDX-License-Identifier: AGPL-3.0-only
// PartyUiHandler.processModifierTransferModeUpDownInput (src/ui/handlers/party-ui-handler.ts, Pagefault
// Games) as its bundler emits it, for the smoke test of src/gamefix.c. X (the party UI modes) is a
// parameter; updateOptions and moveOptionCursor are the test's.
export function makeParty(X){return class{processModifierTransferModeUpDownInput(e){let t=!1,n=this.options[this.optionsCursor];return this.partyUiMode===X.MODIFIER_TRANSFER&&(n!==4e3&&(this.transferQuantities[n]=this.transferQuantitiesMax[n]),this.updateOptions()),t=this.moveOptionCursor(e),t}}}
