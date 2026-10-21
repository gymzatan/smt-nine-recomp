#ifndef SMT9_CONFIG_H
#define SMT9_CONFIG_H
#include <windows.h>
#include "language.h"
#include "../../vendor/xboxrecomp/src/input/input_bindings.h"

typedef struct NineConfig {
    unsigned render_scale, window_width, window_height;
    unsigned aspect_width, aspect_height;
    wchar_t backend[16];
    wchar_t language[NINE_LANGUAGE_ID];
    unsigned refresh_hz;
    wchar_t interface_language[16];
    unsigned fixed_aspect;
    unsigned multithreaded_graphics;
    unsigned vsync;
    XboxInputBindings controls;
} NineConfig;
int nine_config_read(NineConfig *config, wchar_t *error, size_t count);
int nine_config_write(const NineConfig *config, wchar_t *error, size_t count);
int nine_config_validate(const NineConfig *config, wchar_t *error, size_t count);
#endif
