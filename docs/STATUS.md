# Current scope and validation

The initial source release uses the existing accepted native port. It is an early playable release with defined remaining coverage; it does not claim the complete game has been finished or that every scene maintains normal-speed 60 FPS.

Existing runtime verification includes startup, Japanese/Chinese/English selection, ordinary saves and loads, movies, field navigation and shops, story battles, RTS tutorials, chapter two entry, display/aspect options, pause/screenshots and exact-executable instant-state recovery. The reported post-hacking second story battle proceeds through its following street dialogue and Central Administration Bureau scene with the delivered executable. The ordinary Status demon view has been verified with Sphinx and Cait Sith; its model projection uses the same widescreen mapping as its surrounding panel.

Current scene handling keeps the projected speaker endpoint at its world-screen position while the speech window and connector text end share the existing UI mapping. The per-frame popup update retains measured Chinese line width. CLOVER preserves the original appearance scale on both shader axes before applying widescreen X mapping. On the movie EOF frame, teardown returns through the existing epilogue so the following scene can render before dialogue contexts advance. Targeted helper and compiled-function checks cover these changes; the exact reported Legion movie boundary and all CLOVER appearance variants have not been recaptured in game.

Window entry effects reuse the visible UI rectangle for both the original transition mesh and its captured-texture sampling. The two-body Manual fusion entry was replayed through all eleven transition draws at 16:9; its source and screen left edges agree with the settled list. This shared window path also serves shop lists. A separate buy/sell replay remains outside the current verification.

Config supports independently saved keyboard and XInput bindings for 24 game actions, input capture, unbound inputs and restoring defaults. It reuses the existing window-key and controller polling paths; analog pressure, foreground gating and the keyboard/controller merge remain in place.

The original callback catalog includes long exception-handled callbacks, forwarding entries and conditionally selected pointers, with their original out-of-line branch ranges. The supplied Shibuya story battle state runs through a 30-minute supervised replay without the reported unresolved-call exit; displayed verification includes later battle turns and skill effects. Full-game computed-target and battle coverage remains incomplete.

The resource callback used after the Sophia dialogue retains its complete initialization call and both return paths. Replaying the supplied corridor state reaches the following room, dialogue and battle without the reported black screen. Battle item and reward icons use the same horizontal UI mapping as their surrounding panels and text, preserving their original placement at 4:3.

The random-encounter gauge frame uses the same horizontal mapping as its fill and nearby command panels. Native Vulkan readback buffers prefer compatible host-cached memory. Battle effect initialization checks completed callback outputs instead of uninitialized object fields; focused regressions and a ten-minute diagnostic replay cover the reproduced failure. Heavy battle scenes and wider long-session coverage still need performance work.

Known coverage limits:

- Horizontal character gaps and incorrect occlusion have been reported on some player configurations. Local four-backend street samples and an AMD-integrated D3D11 sample did not reproduce them; the exact female-player and restaurant scenes, affected builds and drivers remain unverified. A graphics-memory cause has not been established, and 512 MiB support is not certified. See the README for hardware requirements and memory-report fields.
- Full playthrough, later chapters, all battles, complete audio coverage and long-session stability remain incomplete.
- Performance in heavy scenes still needs work. Selecting 60 Hz is not a promise that every scene maintains it.
- The previously unresolved fusion callback is integrated and the original two-demon selector has been reached. Final fusion confirmation/result coverage remains pending.
- CLOVER viewport/projection corrections are integrated; complete appearance-route visual coverage remains pending.
- Later hardware/render states can still expose explicit unsupported boundaries. The runtime does not silently turn them into successful operations.
- Newly edited Chinese text has structural/reference verification, but every scene has not been replayed in game.

The [source-package validation](VALIDATION.md) records the fresh disc-to-native build, startup, ordinary loading, external-language import/update and same-executable checkpoint scope separately from this existing gameplay coverage. No old checkpoint is treated as proof that a new executable can restore it.
