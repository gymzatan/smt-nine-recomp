#ifndef APU_RESAMPLER_H
#define APU_RESAMPLER_H
#include <stdio.h>
#include "../../../../reference/libsamplerate/include/samplerate.h"
int apu_resampler_save(FILE *out, SRC_STATE *state, const float *input, size_t input_floats);
SRC_STATE *apu_resampler_prepare(FILE *in, src_callback_t callback, void *opaque,
                                 float *input, size_t input_floats);
#endif
