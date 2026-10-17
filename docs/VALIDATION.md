# Source release validation

The source package was built in a separate Windows directory from the supported Japanese retail ISO, repository inputs and the dependencies listed in BUILDING.md. No developer game executable, extracted Media, generated game C, prepared XBE, font container or compiled DSP library was used as a build input. A fresh Python environment was installed from the requirements file. Windows SimSun/SimHei supplied the font inputs.

The ISO SHA-256 was `eafcd00e7fbc53bb10a0ac5743faf2d5ea3e1be9f7351ddb3e97b3c8d916d446`; all 3,515 extracted files were checked against the retail manifest. The original XBE SHA-256 was `6c249401369ed41c2a319762f0b56b80aefde3efe766ac0d8731889a4150deac`.

The existing writers rebuilt the Chinese scripts, fixed fields, fonts, title, staff list and five subtitled movies. The rebuilt native image's sections match the accepted development image byte for byte; only the original certificate title differs. All 22,325 generated function bodies passed the recorded generation hashes. The title bundle matches the accepted bundle. The subtitle writer retained all 24 original audio tracks across 7,631 movie frames and checked the original audio and packet timing. These are rebuild checks, not claims that every translated scene has been played.

The native host, Config and DSP library compiled from source. The native configuration is Release with machine checkpoints enabled. Runtime checks covered:

- Japanese startup/title and an ordinary LOAD01 read.
- Chinese startup, ordinary LOAD10, chapter-two dialogue, a city transition and Status → Demons with Sphinx correctly inside the model panel at 16:9.
- Chinese F9 pause/resume, a new instant save and strict same-executable cold restore back into the Sphinx Status view. F8 captured the restored displayed surface. Captures were taken at different running frames and were not an exact-frame pixel comparison.
- First English import through Config and the ISO picker, a rebuilt package update, language selection, English title/load menu, ordinary LOAD01 into the street, F8 and F9 pause. The English patch and ISO remain external user inputs.
- An intentionally invalid update failed while preserving the existing ready English registration and releasing the import lock.
- The native executables were copied to D:, renamed into a directory containing Chinese characters and spaces, and launched directly with a different working directory. Config displayed its Chinese interface and saved settings in that moved folder. The game reached the title menu and F8 wrote into that folder. Both executables use the Windows GUI subsystem, so direct launch requires no command script or console window. The delivered play folder excludes the test-created saves and caches.
- The Option screen's RGB message-background fills align with their slider frames at 16:9, verified on the displayed surface. Their original direct color-quad entry uses the existing UI coordinate mapping. Targeted checks cover empty, fractional and populated fills, preserve all non-X vertex bytes and leave 4:3 unchanged.

The menu graphics audit checks the original shared scrollbar, icon and connector paths. See [MENU_UI.md](MENU_UI.md) for producer identities, displayed coverage and progression-dependent limits.

New-game displayed recordings also cover the mother/sister and red-haired communications portrait transitions, including entry and exit. The original 22-vertex strip renderer shares the existing UI mapping throughout its animation. The actual helper checks preserve 4:3 coordinates and all non-X vertex bytes.

Closing the framebuffer window ends the game process. A hidden test reproduced the previous lifecycle race: clearing the running flag on close allowed the next guest clear to start another window thread before the old thread finished. The close handler now uses the runtime's existing process-exit behavior. Closing the rebuilt game, including while F9-paused, was verified to terminate the process without creating another framebuffer window.

The ordinary save fixture was copied into the private verification directory only after the disc-to-native build completed. No old instant state was used to establish fresh startup or ordinary-load success. Test processes were closed after verification.

Source-only CI runs the content audit and the targeted build/integration checks without requiring a game disc. It does not run the game or substitute for the clean local build. See [STATUS.md](STATUS.md) for remaining gameplay and performance coverage.
