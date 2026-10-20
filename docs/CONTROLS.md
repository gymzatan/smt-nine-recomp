# Controls

Open `Config.exe` in the root directory for the full keyboard/controller mapping in Chinese, Japanese or English. The interface language can be selected independently from the game language. Double-click `smt9_recomp.exe` in the same directory to play. [中文操作说明](CONTROLS.zh-CN.md).

Window size offers 640×480, 960×720, 1280×720, 1280×960, 1600×900, 1920×1080, 2560×1440 and 3840×2160 presets. Select Custom size to enter another width and height. Fixed Aspect Ratio locks the window's client-area proportions to the chosen dimensions when dragging any edge or corner. It is off by default; `Display/FixedAspectRatio=1` enables it in `runtime/NINE.ini`. The game aspect ratio is independent and retains its existing letterboxing. Saved settings apply on the next launch.

- Double-click the game window to toggle borderless fullscreen and restore its previous size/position.
- F8 saves a BMP screenshot in `runtime/Screenshots`.
- F9 pauses/resumes guest execution, devices, game clock and audio.
- Closing the game window exits the process, including while paused.
- Left Shift+F1 through Left Shift+F5 save instant slots 1 through 5 respectively; F1 through F5 load those slots. Resume from F9 pause before using an instant state.
- Instant states are in `save/states/<language>` and must match their exact program and game image. Keep normal saves for moving between builds.
- Config → Save folder opens `save`. Ordinary saves in `save/UDATA` are shared by all languages; back up the whole folder. Old saves are copied on first use, with their originals retained. Archived instant states still require the original executable/image.

Pause and instant-save requests require the runtime's existing safe boundary. If a request is rejected, the window reports it and no valid instant state is created by that attempt. Keep ordinary saves.

Physical keyboard/controller input is accepted only while the game owns the foreground window. Configuration, saves, language packages and screenshots stay local and are excluded from the repository.

The table below shows the default profile. Use Config → Bindings to change keyboard and controller assignments independently, capture a key/button/stick direction, unbind an input or restore defaults. Apply the dialog and Save the main window; changes apply next launch and survive program updates. F1–F5, F8 and F9 are reserved.

The original Xbox control column is the button sent to the game; each screen determines its game function. Punctuation uses US keyboard labels. Stick directions and D-pad directions are separate inputs.

| Original Xbox control | Modern controller | Keyboard |
| --- | --- | --- |
| D-pad up | D-pad up | Up arrow |
| D-pad down | D-pad down | Down arrow |
| D-pad left | D-pad left | Left arrow |
| D-pad right | D-pad right | Right arrow |
| Left stick up | Left stick up | W |
| Left stick down | Left stick down | S |
| Left stick left | Left stick left | A |
| Left stick right | Left stick right | D |
| Right stick up | Right stick up | I |
| Right stick down | Right stick down | K |
| Right stick left | Right stick left | J |
| Right stick right | Right stick right | L |
| Left stick press (L3) | Press left stick | Number 1 |
| Right stick press (R3) | Press right stick | Number 2 |
| A | A | `.` (period) |
| B | B | `/` (slash) |
| X | X | `;` (semicolon) |
| Y | Y | `'` (apostrophe) |
| L trigger | LB | Q |
| R trigger | RB | E |
| White | LT | `[` (left bracket) |
| Black | RT | `]` (right bracket) |
| Start | Start / Menu | Right Shift |
| Back | Back / View | Backspace |

The shoulder buttons provide the game's L/R actions. LT/RT retain their pressure on the White/Black channels; keyboard buttons send full pressure. Keyboard input is enabled by default and merges with the first controller.
