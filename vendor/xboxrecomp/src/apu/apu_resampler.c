/* The original converter is compiled once here so its existing private state
 * can join the APU checkpoint without copying its algorithm or pointer layout. */
#include "../../../../reference/libsamplerate/src/src_sinc.c"
#include "apu_resampler.h"

typedef struct ApuResamplerCheckpoint {
    uint32_t state_bytes, filter_bytes;
    int32_t input_offset;
    SRC_STATE state;
    SINC_FILTER filter;
} ApuResamplerCheckpoint;

int apu_resampler_save(FILE *out, SRC_STATE *state, const float *input, size_t input_floats)
{
    if (!out || !state || state->vt != &sinc_stereo_state_vt ||
        state->mode != SRC_MODE_CALLBACK || state->channels != 2) return 0;
    SINC_FILTER *filter = state->private_data;
    if (!filter || filter->coeffs != fastest_coeffs.coeffs) return 0;
    ApuResamplerCheckpoint saved = {0};
    saved.state_bytes = sizeof saved.state; saved.filter_bytes = sizeof saved.filter;
    saved.state = *state; saved.filter = *filter; saved.input_offset = -1;
    if (state->saved_data) {
        uintptr_t begin=(uintptr_t)input, pointer=(uintptr_t)state->saved_data;
        if (pointer<begin || pointer-begin>input_floats*sizeof(float) ||
            (pointer-begin)%sizeof(float)) return 0;
        saved.input_offset=(int32_t)((pointer-begin)/sizeof(float));
    }
    if (state->saved_frames<0 || state->saved_frames>(long)(input_floats/2) ||
        (state->saved_frames && saved.input_offset<0) ||
        (saved.input_offset>=0 && (size_t)saved.input_offset+state->saved_frames*2>input_floats)) return 0;
    saved.state.vt=NULL; saved.state.callback_func=NULL; saved.state.user_callback_data=NULL;
    saved.state.saved_data=NULL; saved.state.private_data=NULL;
    saved.filter.coeffs=NULL; saved.filter.buffer=NULL;
    return fwrite("XBAPUSRC",8,1,out)==1 && fwrite(&saved,sizeof saved,1,out)==1 &&
        fwrite(filter->buffer,sizeof(float),(size_t)filter->b_len+2,out)==(size_t)filter->b_len+2;
}

SRC_STATE *apu_resampler_prepare(FILE *in, src_callback_t callback, void *opaque,
                                 float *input, size_t input_floats)
{
    char magic[8]; ApuResamplerCheckpoint saved;
    if (!in || fread(magic,8,1,in)!=1 || memcmp(magic,"XBAPUSRC",8) ||
        fread(&saved,sizeof saved,1,in)!=1 || saved.state_bytes!=sizeof saved.state ||
        saved.filter_bytes!=sizeof saved.filter || saved.state.channels!=2 ||
        saved.state.mode!=SRC_MODE_CALLBACK || saved.state.error!=SRC_ERR_NO_ERROR ||
        saved.state.vt || saved.state.callback_func || saved.state.user_callback_data ||
        saved.state.saved_data || saved.state.private_data || saved.filter.coeffs || saved.filter.buffer ||
        saved.state.saved_frames<0 || saved.state.saved_frames>(long)(input_floats/2) ||
        saved.input_offset < -1 || (saved.state.saved_frames && saved.input_offset<0) ||
        (saved.input_offset>=0 && (size_t)saved.input_offset+saved.state.saved_frames*2>input_floats) ||
        !isfinite(saved.state.last_ratio) || !isfinite(saved.state.last_position) ||
        !isfinite(saved.filter.src_ratio) || !isfinite(saved.filter.input_index)) return NULL;
    int error; SRC_STATE *next=src_callback_new(callback,SRC_SINC_FASTEST,2,&error,opaque);
    if (!next) return NULL;
    SINC_FILTER *filter=next->private_data;
    if (saved.filter.sinc_magic_marker!=filter->sinc_magic_marker ||
        saved.filter.b_len!=filter->b_len || saved.filter.coeff_half_len!=filter->coeff_half_len ||
        saved.filter.index_inc!=filter->index_inc || saved.filter.b_current<0 ||
        saved.filter.b_current>=filter->b_len || saved.filter.b_end<0 ||
        saved.filter.b_end>filter->b_len || saved.filter.b_real_end < -1 ||
        saved.filter.b_real_end>filter->b_len ||
        fread(filter->buffer,sizeof(float),(size_t)filter->b_len+2,in)!=(size_t)filter->b_len+2) {
        src_delete(next); return NULL;
    }
    SRC_STATE_VT *vt=next->vt; const coeff_t *coeffs=filter->coeffs; float *buffer=filter->buffer;
    *next=saved.state; *filter=saved.filter;
    next->vt=vt; next->private_data=filter; next->callback_func=callback; next->user_callback_data=opaque;
    next->saved_data=saved.input_offset<0?NULL:input+saved.input_offset;
    filter->coeffs=coeffs; filter->buffer=buffer;
    return next;
}
