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

The small speech-window connector is rebuilt by `1059A0` and submitted by `105C60` as two four-vertex strips at `105EEC` and `105F08`. Each submitted range shares the existing color UI mapping once. The projected world-speaker point is converted into UI coordinates before the original interpolation, so the final draw preserves the world endpoint while mapping the text end with its background. The per-frame update at `F7DA3` calls the existing `6EC24C` popup-layout wrapper, including its measured Chinese width and screen clamp. This keeps the underline inside the measured window instead of resetting it to the original fixed width. The captured eight-vertex payload verifies both ranges and preserves the original gradient, Y, depth and RHW.

## Appearance and Status previews

The appearance producers preserve the original ratio-times-scale on both physical shader axes, then apply the shared widescreen factor to X. Their viewport and screen origin use the existing UI mapping. The Status producer maps both its body projection at `1427AE` and the fixed-function effect viewport at `1427E9`. The latter is recreated at 60,84,168,184 before each draw; its X and width share the body mapping while Y, height and depth remain original. This keeps animated effects, such as Tsukuyomi's rotating galaxy, attached to the model. The subsequent full-screen viewport restore is unchanged. Tsukuyomi was compared at 4:3 and 16:9, with corrected widescreen output checked on native Vulkan and OpenGL. Production-helper checks exercise both aspects; complete appearance-route visual coverage remains pending.

## Fusion preview and scene

The sword preview at `10DFBD` uses the same centered horizontal mapping for its projection and its original 112×112 viewport. The static viewport table is mapped from its original dimensions on every draw, preventing repeated scaling. Its full-screen entry and depth range remain unchanged.

The original scene camera `31389` supplies the shader's screen scale. With independent native shader axes, its scale is 240 times the original zoom instead of 320, preserving the original scalar used by both axes. The original solid/wireframe animation and depth tests remain intact. The screen-space backdrop stretches horizontally, so `368F3` maps each authored disc anchor's X to match that backdrop before the original model and effect consumers read the shared object root. Only the root position moves; local model dimensions retain their normal proportions. The original anchor table and 4:3 coordinates remain unchanged.

The supplied sword-fusion route was captured through confirmation, the full animation and result at 4:3 and 16:9 on native Vulkan, and at 16:9 on OpenGL. These are 20-second displayed sequences sampled at 12 Hz. The preview remains square inside its menu; the demon's feet stand on the platform before its intended wireframe dissolution. Ordinary two-body fusion (Amaterasu + Fire Elemental → Tsukuyomi) and three-body fusion (Vishnu + Freya + Dominion → Amaterasu) were also captured through their complete animations and results on native Vulkan at 16:9. All three material roots and the result root align with their corresponding discs. This does not establish coverage of every fusion combination.

## Random encounter gauge

The selection countdown's textured frame and its plain branch are rebuilt by `110106` and submitted through `1102DC` and `110305`. Both use the same centered horizontal mapping as the existing `110102` green fill and the adjacent command panels. The original stack quad is rebuilt each draw, so the adjustment does not accumulate. The four-vertex payload retains Y, depth, RHW, color and UV values, and 4:3 retains every original byte. This screen-space frame is separate from the projected HP/MP bars above battle actors.

## Portrait transitions

The original sprite renderer `145A39` switches to `1450CF` during the strip animation used to show and hide portraits. Its draw return `1452C5` submits 22 vertices rather than the settled four-vertex quad at `14617A`. Every animated vertex uses the same centered horizontal mapping as the settled portrait. Y, depth, RHW, colors and UVs retain the original animation values. This keeps the portrait's proportions consistent through entry, exit and speaker changes.

For the settled textured quad, whole-screen artwork retains its framing. A standing portrait can reach or exceed the screen height without covering its width; it retains the centered horizontal mapping and the existing atlas endpoint correction.

The original `112B74` initializer registers the background sprite list at `3B2480`, separately from the character list at `3B21E8`. The two background sprites cover 512 and 128 screen pixels and together fill the selected aspect. The existing draw helper reads that original manager identity from the settled renderer's saved stack slot, or from the strip renderer's saved frame slot. Both background tiles retain their original X extent and UVs, including when the queue uses an inline sprite copy. Character subregions remain proportional even when they share a texture with background artwork; no whole-atlas role or image-shape heuristic is used.

The title initializer `215180` registers list `530408` for both initial entry and later returns. Its texture 0 and 1 records with source height 480 are the original 512+128 background tiles. The shared draw helper preserves their X extent and UVs using the saved manager and original record fields, including inline queue copies. The same list's Press Start and menu sprites retain their proportional mapping and shared title anchor from `font_assets.h`; the separate logo layer and original 4:3 asset handling remain in place. Production-helper checks exercise both background tiles, original and copied records, and the Press Start anchor at 4:3 and 16:9.

Production-helper checks cover both background tiles with 128- and 512-pixel source textures, original and queued-copy records, both strip-animation tiles, and the original `endchar` character rectangle at source X128, width360, height512. They exercise both display aspects, preserve non-X payloads and verify portrait texture edges. The reported Miranda dialogue and ending scene have not been replayed.

Displayed checks start a new game and record continuous frames of the mother and sister appearing, changing speakers and disappearing, followed by the red-haired communications portrait appearing and disappearing. The surrounding communications window and backdrop retain their existing handling. Targeted checks execute the actual helper with the original 22-vertex geometry at 4:3 and 16:9.

## Window entry effects

After copying a window descriptor, the original `10D6C8` initializer maps its X origin and width with the same centered UI factor as the settled window. The original animation engine then generates screen vertices and captured-texture coordinates from that rectangle. This keeps the animated fragments aligned with the captured UI. Original 4:3 coordinates and full-screen rectangles are preserved.

The two-body Manual fusion list was replayed through all eleven entry draws at 16:9. Its screen and texture-source left edges both match the settled list at X218, instead of the original X184. Production-helper checks cover this captured rectangle, 4:3 and full-screen preservation. Shop lists share the initializer; their buy/sell transitions have not been replayed separately.

## Validation scope

Local displayed-surface checks cover title selection, name entry, Option/RGB extremes/default confirmation, controller Basic/RTS/Battle settings, ordinary-load devices/slots/confirmation/progress, item and magic selection, party/player/demon Status, demon sorting/skills/affinities/delete warnings, undiscovered analysis entries, equipment icons/lists, NAVI collection/view/change, AI and formation. Wide and original 4:3 layouts were compared for the demonstrated scrollbar and connector problems.

The graphics audit compared 1,818 retail graphics files (153,852,164 bytes) against the rebuilt Chinese resources, excluding declared translated font resources. Reserved symbol page8 matches retail, including the runtime font copies at 2×, 3× and 4×. These checks establish asset provenance, not the placement of every icon in every game state.

Existing accepted evidence also covers RTS/battle/communications UI and the field system menu/world-map round trip. Fusion list evidence exercises the shared list family; the corrected producer applies to that family. Every shop, fusion combination, appearance variant and progression-dependent analysis entry has not been visually revisited. A full playthrough remains outside this menu audit. See [VALIDATION.md](VALIDATION.md) and [STATUS.md](STATUS.md).

Targeted checks execute the actual mapping helpers, including both scrollbar formats, all strip vertices, original full-screen fades, neighboring unrelated callers and preservation of non-X bytes. Local temporary vertex logging is excluded from the delivered program and source.
