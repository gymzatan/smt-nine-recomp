/*
 * MCPX APU State Structures (adapted from xemu)
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2018-2019 Jannik Vogel
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "apu_shim.h"
#include "apu_regs.h"
#include "apu_debug.h"

#include "dsp/dsp.h"

/* ============================================================
 * SVF (State Variable Filter) - from vp/svf.h
 * ============================================================ */

#define flush_to_zero(x) (x)
#define F_LP 1
#define F_HP 2
#define F_BP 3
#define F_BR 4
#define F_AP 5

typedef struct {
    float f;
    float q;
    float qnrm;
    float h;
    float b;
    float l;
    float p;
    float n;
    float *op;
} sv_filter;

static inline void setup_svf(sv_filter *sv, float fc, float q, int t) {
    sv->f = fc;
    sv->q = q;
    sv->qnrm = (float)sqrt(sv->q / 2.0 + 0.01);
    switch (t) {
    case F_LP: sv->op = &(sv->l); break;
    case F_HP: sv->op = &(sv->h); break;
    case F_BP: sv->op = &(sv->b); break;
    case F_BR: sv->op = &(sv->n); break;
    default:   sv->op = &(sv->p); break;
    }
}

static inline float run_svf_lowpass(sv_filter *sv, float in) {
    in = sv->qnrm * in;
    sv->b = flush_to_zero(sv->b - sv->b * sv->b * sv->b * 0.001f);
    sv->h = flush_to_zero(in - sv->l - sv->q * sv->b);
    sv->b = sv->b + sv->f * sv->h;
    sv->l = flush_to_zero(sv->l + sv->f * sv->b);
    return sv->l;
}

/* ============================================================
 * HRTF Filter - from vp/hrtf.h
 * ============================================================ */

#define HRTF_SAMPLES_PER_FRAME  NUM_SAMPLES_PER_FRAME
#define HRTF_NUM_TAPS           31
#define HRTF_MAX_DELAY_SAMPLES  42
#define HRTF_BUFLEN             (HRTF_NUM_TAPS + HRTF_MAX_DELAY_SAMPLES)
#define HRTF_PARAM_SMOOTH_ALPHA 0.01f

typedef struct {
    int buf_pos;
    struct {
        float buf[HRTF_BUFLEN];
        float hrir_coeff_cur[HRTF_NUM_TAPS];
        float hrir_coeff_tar[HRTF_NUM_TAPS];
    } ch[2];
    float itd_cur;
    float itd_tar;
} HrtfFilter;

static inline void hrtf_filter_init(HrtfFilter *f) {
    memset(f, 0, sizeof(*f));
}

static inline void hrtf_filter_clear_history(HrtfFilter *f) {
    f->buf_pos = 0;
    memset(f->ch[0].buf, 0, sizeof(f->ch[0].buf));
    memset(f->ch[1].buf, 0, sizeof(f->ch[1].buf));
}

static inline void
hrtf_filter_set_target_params(HrtfFilter *f, float hrir_coeff[2][HRTF_NUM_TAPS],
                              float itd) {
    f->itd_tar = fmaxf(-(float)HRTF_MAX_DELAY_SAMPLES,
                        fminf(itd, (float)HRTF_MAX_DELAY_SAMPLES));
    for (int ch = 0; ch < 2; ch++) {
        float *coeff = f->ch[ch].hrir_coeff_tar;
        memcpy(coeff, hrir_coeff[ch], sizeof(f->ch[ch].hrir_coeff_tar));
        float s = 0.0f;
        for (int k = 0; k < HRTF_NUM_TAPS; k++) {
            s += fabsf(coeff[k]);
        }
        if (s == 0.0f || s == 1.0f) break;
        for (int k = 0; k < HRTF_NUM_TAPS; k++) {
            coeff[k] /= s;
        }
    }
}

static inline float hrtf_filter_smooth_param(float cur, float tar) {
    return cur + HRTF_PARAM_SMOOTH_ALPHA * (tar - cur);
}

static inline void hrtf_filter_step_parameters(HrtfFilter *f) {
    for (int ch = 0; ch < 2; ch++) {
        float *cc = f->ch[ch].hrir_coeff_cur;
        float *ct = f->ch[ch].hrir_coeff_tar;
        for (int k = 0; k < HRTF_NUM_TAPS; k++) {
            cc[k] = hrtf_filter_smooth_param(cc[k], ct[k]);
        }
    }
    f->itd_cur = hrtf_filter_smooth_param(f->itd_cur, f->itd_tar);
}

static inline void hrtf_filter_process(HrtfFilter *f,
                                       float in[][2],
                                       float out[][2]) {
    for (int n = 0; n < HRTF_SAMPLES_PER_FRAME; n++) {
        hrtf_filter_step_parameters(f);
        for (int ch = 0; ch < 2; ch++) {
            float *buf = f->ch[ch].buf;
            float *coeff = f->ch[ch].hrir_coeff_cur;
            buf[f->buf_pos] = in[n][ch];
            float d = f->itd_cur * (ch == 0 ? +1.0f : -1.0f);
            if (d < 0.0f) d = 0.0f;
            int di = (int)d;
            float dfrac = d - di;
            float acc = 0.0f;
            for (int k = 0; k < HRTF_NUM_TAPS; k++) {
                int idx1 = (f->buf_pos - di - k + HRTF_BUFLEN) % HRTF_BUFLEN;
                float s = buf[idx1];
                if (dfrac > 0.0f) {
                    int idx2 = (idx1 - 1 + HRTF_BUFLEN) % HRTF_BUFLEN;
                    s = s * (1 - dfrac) + buf[idx2] * dfrac;
                }
                acc += coeff[k] * s;
            }
            out[n][ch] = acc;
        }
        f->buf_pos = (f->buf_pos + 1) % HRTF_BUFLEN;
    }
}

/* ============================================================
 * ADPCM Decoder - from vp/adpcm.h
 * ============================================================ */

static inline int adpcm_decode_block(int16_t *outbuf, const uint8_t *inbuf,
                                      size_t inbufsize, int channels) {
    #define ADPCM_CLIP(data, mn, mx) \
        if ((data) > (mx)) data = mx; \
        else if ((data) < (mn)) data = mn;

    #define ADPCM_MAG(s, n) (((s) >> 3) + (((n) & 1) ? ((s) >> 2) : 0) + \
                             (((n) & 2) ? ((s) >> 1) : 0) + (((n) & 4) ? (s) : 0))
    #define ADPCM_DELTAS(s) {ADPCM_MAG(s, 0), ADPCM_MAG(s, 1), ADPCM_MAG(s, 2), ADPCM_MAG(s, 3), \
                            ADPCM_MAG(s, 4), ADPCM_MAG(s, 5), ADPCM_MAG(s, 6), ADPCM_MAG(s, 7)}
    static const uint16_t delta_table[89][8] = {
        ADPCM_DELTAS(7), ADPCM_DELTAS(8), ADPCM_DELTAS(9), ADPCM_DELTAS(10),
        ADPCM_DELTAS(11), ADPCM_DELTAS(12), ADPCM_DELTAS(13), ADPCM_DELTAS(14),
        ADPCM_DELTAS(16), ADPCM_DELTAS(17), ADPCM_DELTAS(19), ADPCM_DELTAS(21),
        ADPCM_DELTAS(23), ADPCM_DELTAS(25), ADPCM_DELTAS(28), ADPCM_DELTAS(31),
        ADPCM_DELTAS(34), ADPCM_DELTAS(37), ADPCM_DELTAS(41), ADPCM_DELTAS(45),
        ADPCM_DELTAS(50), ADPCM_DELTAS(55), ADPCM_DELTAS(60), ADPCM_DELTAS(66),
        ADPCM_DELTAS(73), ADPCM_DELTAS(80), ADPCM_DELTAS(88), ADPCM_DELTAS(97),
        ADPCM_DELTAS(107), ADPCM_DELTAS(118), ADPCM_DELTAS(130), ADPCM_DELTAS(143),
        ADPCM_DELTAS(157), ADPCM_DELTAS(173), ADPCM_DELTAS(190), ADPCM_DELTAS(209),
        ADPCM_DELTAS(230), ADPCM_DELTAS(253), ADPCM_DELTAS(279), ADPCM_DELTAS(307),
        ADPCM_DELTAS(337), ADPCM_DELTAS(371), ADPCM_DELTAS(408), ADPCM_DELTAS(449),
        ADPCM_DELTAS(494), ADPCM_DELTAS(544), ADPCM_DELTAS(598), ADPCM_DELTAS(658),
        ADPCM_DELTAS(724), ADPCM_DELTAS(796), ADPCM_DELTAS(876), ADPCM_DELTAS(963),
        ADPCM_DELTAS(1060), ADPCM_DELTAS(1166), ADPCM_DELTAS(1282), ADPCM_DELTAS(1411),
        ADPCM_DELTAS(1552), ADPCM_DELTAS(1707), ADPCM_DELTAS(1878), ADPCM_DELTAS(2066),
        ADPCM_DELTAS(2272), ADPCM_DELTAS(2499), ADPCM_DELTAS(2749), ADPCM_DELTAS(3024),
        ADPCM_DELTAS(3327), ADPCM_DELTAS(3660), ADPCM_DELTAS(4026), ADPCM_DELTAS(4428),
        ADPCM_DELTAS(4871), ADPCM_DELTAS(5358), ADPCM_DELTAS(5894), ADPCM_DELTAS(6484),
        ADPCM_DELTAS(7132), ADPCM_DELTAS(7845), ADPCM_DELTAS(8630), ADPCM_DELTAS(9493),
        ADPCM_DELTAS(10442), ADPCM_DELTAS(11487), ADPCM_DELTAS(12635), ADPCM_DELTAS(13899),
        ADPCM_DELTAS(15289), ADPCM_DELTAS(16818), ADPCM_DELTAS(18500), ADPCM_DELTAS(20350),
        ADPCM_DELTAS(22385), ADPCM_DELTAS(24623), ADPCM_DELTAS(27086), ADPCM_DELTAS(29794),
        ADPCM_DELTAS(32767),
    };
    #undef ADPCM_DELTAS
    #undef ADPCM_MAG
    static const int index_table[] = { -1, -1, -1, -1, 2, 4, 6, 8 };

    int samples = 1, chunks;
    int32_t pcmdata[2];
    int8_t index[2];

    if (inbufsize < (uint32_t)channels * 4) return 0;

    for (int ch = 0; ch < channels; ch++) {
        *outbuf++ = pcmdata[ch] = (int16_t)(inbuf[0] | (inbuf[1] << 8));
        /* The fourth header byte is RESERVED, not validated: the MCPX does not
         * refuse a block over it, and titles ship blocks where it is non-zero
         * (Jet Set Radio Future ends every ADPCM buffer in a 0x08 pad, and
         * 3.5-4.4% of its blocks were silenced by this test). The step index
         * is clamped for the same reason -- and it must be clamped, not
         * trusted, because delta_table has 89 entries. Refusing either returned
         * 0 AFTER the first sample of each channel had been written. */
        index[ch] = (int8_t)inbuf[2];
        ADPCM_CLIP(index[ch], 0, 88);
        inbufsize -= 4;
        inbuf += 4;
    }

    chunks = (int)(inbufsize / (channels * 4));
    samples += chunks * 8;

    while (chunks--) {
        for (int ch = 0; ch < channels; ++ch) {
            for (int i = 0; i < 4; ++i) {
                int code = *inbuf & 0xf;
                int delta = delta_table[index[ch]][code & 7];
                if (code & 8) delta = -delta;
                pcmdata[ch] += delta;
                index[ch] += index_table[code & 7];
                ADPCM_CLIP(index[ch], 0, 88);
                ADPCM_CLIP(pcmdata[ch], -32768, 32767);
                outbuf[i * 2 * channels] = (int16_t)pcmdata[ch];

                code = *inbuf >> 4;
                delta = delta_table[index[ch]][code & 7];
                if (code & 8) delta = -delta;
                pcmdata[ch] += delta;
                index[ch] += index_table[code & 7];
                ADPCM_CLIP(index[ch], 0, 88);
                ADPCM_CLIP(pcmdata[ch], -32768, 32767);
                outbuf[(i * 2 + 1) * channels] = (int16_t)pcmdata[ch];
                inbuf++;
            }
            outbuf++;
        }
        outbuf += channels * 7;
    }
    #undef ADPCM_CLIP
    return samples;
}

/* ============================================================
 * VP State (from vp/vp.h)
 * ============================================================ */

typedef struct MCPXAPUState MCPXAPUState;

typedef struct MCPXAPUVPSSLData {
    uint32_t base[MCPX_HW_SSLS_PER_VOICE];
    uint8_t count[MCPX_HW_SSLS_PER_VOICE];
    int ssl_index;
    int ssl_seg;
} MCPXAPUVPSSLData;

typedef struct MCPXAPUVoiceFilter {
    uint16_t voice;
    float resample_buf[NUM_SAMPLES_PER_FRAME * 2];
    SRC_STATE *resampler;
    sv_filter svf[2];
    HrtfFilter hrtf;
} MCPXAPUVoiceFilter;

typedef struct VoiceWorkItem {
    int voice;
    int list;
} VoiceWorkItem;

typedef struct VoiceWorker {
    QemuThread thread;
    float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    float sample_buf[NUM_SAMPLES_PER_FRAME][2];
    VoiceWorkItem queue[MCPX_HW_MAX_VOICES];
    int queue_len;
} VoiceWorker;

typedef struct VoiceWorkDispatch {
    QemuMutex lock;
    int num_workers;
    VoiceWorker *workers;
    bool workers_should_exit;
    QemuCond work_pending;
    uint64_t workers_pending;
    QemuCond work_finished;
    float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    VoiceWorkItem queue[MCPX_HW_MAX_VOICES];
    int queue_len;
} VoiceWorkDispatch;

typedef struct MCPXAPUVPState {
    MemoryRegion mmio;
    VoiceWorkDispatch voice_work_dispatch;
    MCPXAPUVoiceFilter filters[MCPX_HW_MAX_VOICES];

    int ssl_base_page;
    MCPXAPUVPSSLData ssl[MCPX_HW_MAX_VOICES];
    uint8_t hrtf_headroom;
    uint8_t hrtf_submix[4];
    uint8_t submix_headroom[NUM_MIXBINS];
    float sample_buf[NUM_SAMPLES_PER_FRAME][2];
    uint64_t voice_locked[4];

    struct {
        int current_entry;
        struct {
            float hrir[2][HRTF_NUM_TAPS];
            float itd;
        } entries[HRTF_ENTRY_COUNT];
    } hrtf;

    uint32_t inbuf_sge_handle;
    uint32_t outbuf_sge_handle;
} MCPXAPUVPState;

/* ============================================================
 * GP/EP State (from dsp/gp_ep.h)
 * ============================================================ */

typedef struct MCPXAPUGPState {
    bool realtime;
    MemoryRegion mmio;
    DSPState *dsp;
    uint32_t regs[0x10000];
} MCPXAPUGPState;

typedef struct MCPXAPUEPState {
    bool realtime;
    MemoryRegion mmio;
    DSPState *dsp;
    uint32_t regs[0x10000];
} MCPXAPUEPState;

/* ============================================================
 * Main APU State (from apu_int.h)
 * ============================================================ */

struct MCPXAPUState {
    /*< private >*/
    PCIDevice parent_obj;
    /*< public >*/

    bool exiting;
    bool set_irq;

    QemuThread apu_thread;
    QemuMutex lock;
    QemuCond cond;
    QemuCond idle_cond;
    bool pause_requested;
    bool is_idle;

    MemoryRegion *ram;
    uint8_t *ram_ptr;
    MemoryRegion mmio;

    MCPXAPUVPState vp;
    MCPXAPUGPState gp;
    MCPXAPUEPState ep;

    uint32_t regs[0x20000];

    int ep_frame_div;
    int sleep_acc_us;
    int frame_count;
    int64_t frame_count_time_ms;
    int64_t next_frame_time_us;

    struct {
        struct {
            int backoff, ok, speedup;
        } pacing;
        struct {
            int64_t last_us;
            int64_t min_us, max_us, sum_us;
            int count;
        } deviation;
        int queued_bytes_min, queued_bytes_max;
        int64_t queued_bytes_sum;
        int queued_bytes_count;
    } throttle;

    struct {
        McpxApuDebugMonitorPoint point;
        int16_t frame_buf[256][2];
        int16_t spdif_buf[256][2];
        unsigned ac97_output_channel;
        unsigned ac97_frame_div;
        uint32_t spdif_channel_status;
        void *stream; /* SDL_AudioStream* - stubbed */
        int queued_bytes_low, queued_bytes_high;
    } monitor;
    void *(*physical_resolve)(uint32_t, uint32_t);
};

/* ============================================================
 * Forward declarations for APU sub-module functions
 * ============================================================ */

/* VP functions */
void mcpx_apu_vp_init(MCPXAPUState *d);
void mcpx_apu_vp_finalize(MCPXAPUState *d);
void mcpx_apu_vp_frame(MCPXAPUState *d, float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME]);
void mcpx_apu_vp_reset(MCPXAPUState *d);
SRC_STATE *mcpx_apu_vp_prepare_resampler(MCPXAPUVoiceFilter *filter, FILE *in);

/* Original GP/EP DSP integration. */
void mcpx_apu_dsp_init(MCPXAPUState *d);
void mcpx_apu_update_dsp_preference(MCPXAPUState *d);
void mcpx_apu_dsp_frame(MCPXAPUState *d, float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME]);
extern const MemoryRegionOps gp_ops;
extern const MemoryRegionOps ep_ops;

/* Debug globals */
extern MCPXAPUState *g_state;
extern int g_dbg_voice_monitor;
extern uint64_t g_dbg_muted_voices[4];
