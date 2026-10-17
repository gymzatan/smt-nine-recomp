# Controls

Open `Config.exe` in the root directory for the full keyboard/controller mapping in Chinese, Japanese or English. The interface defaults to Chinese and can be selected independently from the game language. Double-click `smt9_recomp.exe` in the same directory to play.

- Double-click the game window to toggle borderless fullscreen and restore its previous size/position.
- F8 saves a BMP screenshot in `runtime/Screenshots`.
- F9 pauses/resumes guest execution, devices, game clock and audio.
- Shift+F1 through Shift+F5 save instant slots; F1 through F5 load those slots. Resume from F9 pause before using an instant state.
- Instant states must match their exact program and game image. Keep normal saves for moving between builds.

Pause and instant-save requests require the runtime's existing safe boundary. If a request is rejected, the window reports it and no valid instant state is created by that attempt. Keep ordinary saves.

Physical keyboard/controller input is accepted only while the game owns the foreground window. Configuration, saves, language packages and screenshots stay local and are excluded from the repository.
