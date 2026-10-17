# Menu graphics

The native host keeps the retail menu logic, icon atlases and geometry producers. Menu shapes use the existing centered horizontal UI mapping at widescreen aspects. The mapping leaves Y, depth, RHW, colors and texture coordinates intact for color geometry; 4:3 keeps the original coordinates.

## Scrollbar paths

| Original producer | Consumers and handling |
| --- | --- |
| `145A39`, direct draw return `146153` | Untextured four-vertex, 28-byte quads used by item, magic, party, demon, equipment and other menu lists. Shares the existing sprite UI mapping. |
| `F4E70`, draw return `F4F82`, original class table `331C64` | Four-vertex, 20-byte color buffers used by NAVI lists and other original rectangle consumers. The original rectangle writer rebuilds coordinates before drawing. Shares the color UI mapping; original full-screen field fades retain their 639×479 or 640×480 extent. |
| `1480BE`, draw return `148225` | The existing 28-byte color-quad renderer, including ordinary-load scroll and progress bars. Uses the established UI mapping. |

Track and arrow icons retain the original shared sprite/font paths. The reserved kana/symbol page is preserved from the retail font rather than replaced by Chinese glyphs. Its runtime font copies retain the same pixels at the selected scale.

The controller guide strips (`1353F2`, return `1357E7`) and menu polylines (`148240`, return `1482D5`) use the same color-geometry mapping. Every vertex in each strip is mapped, including the six-vertex formation connector.

## Portrait transitions

The original sprite renderer `145A39` switches to `1450CF` during the strip animation used to show and hide portraits. Its draw return `1452C5` submits 22 vertices rather than the settled four-vertex quad at `14617A`. Every animated vertex uses the same centered horizontal mapping as the settled portrait. Y, depth, RHW, colors and UVs retain the original animation values. This keeps the portrait's proportions consistent through entry, exit and speaker changes.

Displayed checks start a new game and record continuous frames of the mother and sister appearing, changing speakers and disappearing, followed by the red-haired communications portrait appearing and disappearing. The surrounding communications window and backdrop retain their existing handling. Targeted checks execute the actual helper with the original 22-vertex geometry at 4:3 and 16:9.

## Validation scope

Local displayed-surface checks cover title selection, name entry, Option/RGB extremes/default confirmation, controller Basic/RTS/Battle settings, ordinary-load devices/slots/confirmation/progress, item and magic selection, party/player/demon Status, demon sorting/skills/affinities/delete warnings, undiscovered analysis entries, equipment icons/lists, NAVI collection/view/change, AI and formation. Wide and original 4:3 layouts were compared for the demonstrated scrollbar and connector problems.

The graphics audit compared 1,818 retail graphics files (153,852,164 bytes) against the rebuilt Chinese resources, excluding declared translated font resources. Reserved symbol page8 matches retail, including the runtime font copies at 2×, 3× and 4×. These checks establish asset provenance, not the placement of every icon in every game state.

Existing accepted evidence also covers RTS/battle/communications UI and the field system menu/world-map round trip. Fusion list evidence exercises the shared list family; the corrected producer applies to that family. Every shop, fusion combination, appearance variant and progression-dependent analysis entry has not been visually revisited. A full playthrough remains outside this menu audit. See [VALIDATION.md](VALIDATION.md) and [STATUS.md](STATUS.md).

Targeted checks execute the actual mapping helpers, including both scrollbar formats, all strip vertices, original full-screen fades, neighboring unrelated callers and preservation of non-X bytes. Local temporary vertex logging is excluded from the delivered program and source.
