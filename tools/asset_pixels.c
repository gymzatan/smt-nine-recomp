/* Disc asset preparation calls the same texture decoder as the runtime. */
#include "../vendor/xboxrecomp/src/d3d/d3d8_swizzle.h"
__declspec(dllexport) void nine_unswizzle(void *destination, const void *source,
                                        unsigned width, unsigned height)
{
    xbox_unswizzle_rect(destination, source, width, height, 4);
}
