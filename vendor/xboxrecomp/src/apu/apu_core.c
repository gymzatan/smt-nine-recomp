/*
 * MCPX APU Core - Standalone extraction from xemu
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

#include "apu_state.h"
#include "apu.h"
#include "kernel/kernel.h"
#include "apu_xaudio2.h"
#include "fpconv.h"


/* ============================================================
 * Globals
 * ============================================================ */

uint8_t *g_apu_ram_ptr = NULL;

MCPXAPUState *g_state = NULL;

/* Forward declarations for software mixer */
static void mixer_init(void);
static void mixer_render(int16_t frame_buf[][2], int num_samples);
static APUMixerVoice g_mixer_voices[APU_MIXER_MAX_VOICES];
static volatile int g_mixer_active_count = 0;
static CRITICAL_SECTION g_mixer_cs;
static bool g_mixer_initialized = false;
struct McpxApuDebug g_dbg;
struct McpxApuDebug g_dbg_cache;
int g_dbg_voice_monitor = -1;
uint64_t g_dbg_muted_voices[4] = { 0 };

/* Global audio mute — disables all AWD/mixer sound playback */
volatile int g_audio_muted = 0;  /* 0 = audio enabled */
static int g_apu_checkpoint_restart;

/* ============================================================
 * Debug frame markers (minimal stubs)
 * ============================================================ */

void mcpx_debug_begin_frame(void) {}
void mcpx_debug_end_frame(void) {}

/* ============================================================
 * Physical RAM and interrupt levels
 * ============================================================ */

/* The caller supplies the same physical store used by the GPU. CPU guest
 * addresses are resolved separately by the kernel; they are not DMA offsets. */
uint8_t *mcpx_apu_phys(uint64_t addr, size_t length)
{
    if (!g_apu_ram_ptr || addr>0x04000000u || length>0x04000000u-addr) {
        fprintf(stderr,"[STOP] APU physical address outside connected RAM: 0x%llX\n",
                (unsigned long long)addr);
        exit(5);
    }
    return g_apu_ram_ptr+(size_t)addr;
}

/* The frame thread raises a level; the existing kernel worker delivers it. */
static volatile LONG s_irq_line;

static void update_irq(MCPXAPUState *d)
{
    if ((d->regs[NV_PAPU_IEN] & NV_PAPU_ISTS_GINTSTS) &&
        ((d->regs[NV_PAPU_ISTS] & ~NV_PAPU_ISTS_GINTSTS) &
         d->regs[NV_PAPU_IEN])) {
        qatomic_or(&d->regs[NV_PAPU_ISTS], NV_PAPU_ISTS_GINTSTS);
        InterlockedExchange(&s_irq_line, 1);
    } else {
        qatomic_and(&d->regs[NV_PAPU_ISTS], ~NV_PAPU_ISTS_GINTSTS);
        InterlockedExchange(&s_irq_line, 0);
    }
}

int mcpx_apu_irqs_pending(void)
{
    MCPXAPUState *d=g_state;
    if (!d) return 0;
    qemu_mutex_lock(&d->lock);
    int pending=InterlockedCompareExchange(&s_irq_line,0,0) || mcpx_ac97_irq_pending();
    qemu_mutex_unlock(&d->lock);
    return pending;
}

/* The kernel timer already owns a guest stack/TIB and calls original ISRs.
 * Both audio devices use that registry; the frame thread only raises levels. */
void mcpx_apu_service_irqs(int (*deliver)(uint32_t))
{
    MCPXAPUState *d=g_state;
    if (!d) return;
    qemu_mutex_lock(&d->lock);
    while (InterlockedCompareExchange(&s_irq_line,0,0)) {
        int claimed=deliver(5);
        if (claimed<0) {
            fprintf(stderr,"[STOP] APU IRQ has no callable connected ISR\n");
            exit(5);
        }
        /* A claimed level may stay pending after FE is resumed. Service that
         * same level before lower-priority guest work can retire its voice.
         * An unclaimed shared IRQ retains the existing deferred retry. */
        if (!claimed) break;
    }
    if (mcpx_ac97_irq_pending() && deliver(6)<0) {
        fprintf(stderr,"[STOP] AC97 IRQ has no callable connected ISR\n");
        exit(5);
    }
    qemu_mutex_unlock(&d->lock);
}

/* ============================================================
 * MMIO Read / Write
 * ============================================================ */

uint64_t mcpx_apu_read(void *opaque, hwaddr addr, unsigned int size)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;
    uint64_t r = 0;

    switch (addr) {
    case NV_PAPU_XGSCNT:
        r = (uint64_t)(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 100);
        break;
    default:
        if (addr < 0x20000) {
            r = qatomic_read(&d->regs[addr]);
        }
        break;
    }

    /* Uncomment for register tracing:
     * fprintf(stderr, "[APU] read  [0x%05llX] size=%u -> 0x%08llX\n",
     *         (unsigned long long)addr, size, (unsigned long long)r);
     */
    (void)size;
    return r;
}

void mcpx_apu_write(void *opaque, hwaddr addr, uint64_t val,
                     unsigned int size)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;

    /* Uncomment for register tracing:
     * fprintf(stderr, "[APU] write [0x%05llX] size=%u <- 0x%08llX\n",
     *         (unsigned long long)addr, size, (unsigned long long)val);
     */
    (void)size;

    switch (addr) {
    case NV_PAPU_ISTS:
        qatomic_and(&d->regs[NV_PAPU_ISTS], ~(uint32_t)val);
        update_irq(d);
        qemu_cond_broadcast(&d->cond);
        break;
    case NV_PAPU_FECTL:
    case NV_PAPU_SECTL:
        qatomic_set(&d->regs[addr], (uint32_t)val);
        /* Starting the APU has to start the frame thread.
         *
         * The thread idles on pause_requested, which init sets and only the
         * test tone ever cleared -- so a title that enabled the APU through
         * these registers got an APU that stayed asleep. Nothing then advanced
         * the front end, and a title waiting on a notify completion (the
         * FEMEMDATA magic write, which is how completion reaches guest memory)
         * waited forever. Wreckless hangs exactly there during DirectSound
         * init, and because it initialises its whole engine behind a
         * successful DirectSound create, that hang is not confined to audio.
         *
         * Resume whenever the write is not switching the block off; the thread
         * re-checks FECTL itself and idles again if it is halted or trapped. */
        {
            uint32_t sectl = qatomic_read(&d->regs[NV_PAPU_SECTL]);
            uint32_t fectl = qatomic_read(&d->regs[NV_PAPU_FECTL]);
            bool running =
                ((sectl & NV_PAPU_SECTL_XCNTMODE) != NV_PAPU_SECTL_XCNTMODE_OFF)
                && ((fectl & NV_PAPU_FECTL_FEMETHMODE)
                    != NV_PAPU_FECTL_FEMETHMODE_HALTED);
            if (running && d->pause_requested) {
                d->pause_requested = false;
                fprintf(stderr, "[APU] started by the title"
                                " (SECTL=%08X FECTL=%08X)\n", sectl, fectl);
            }
        }
        qemu_cond_broadcast(&d->cond);
        break;
    case NV_PAPU_FEMEMDATA:
        /* 'magic write' - value written to FEMEMADDR on notify completion */
        stl_le_phys(address_space_memory, d->regs[NV_PAPU_FEMEMADDR], (uint32_t)val);
        qatomic_set(&d->regs[addr], (uint32_t)val);
        break;
    default:
        if (addr < 0x20000) {
            qatomic_set(&d->regs[addr], (uint32_t)val);
        }
        break;
    }
}

/* ============================================================
 * Test tone state (used by monitor and test tone functions)
 * ============================================================ */

static struct {
    bool active;
    double phase;
    double phase_inc;
    int16_t amplitude;
} g_test_tone = { false, 0.0, 0.0, 0 };

/* ============================================================
 * Monitor - XAudio2 output
 * ============================================================ */

#if defined(_WIN32)
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif


#define MIXER_FRAME_SAMPLES 256

void mcpx_apu_monitor_init(MCPXAPUState *d, Error **errp)
{
    (void)errp;
    d->monitor.stream = NULL;
    d->monitor.queued_bytes_low = 8192;
    d->monitor.queued_bytes_high = 12288;

    if (!xa2_init()) {
        fprintf(stderr,"[STOP] XAudio2 output initialization failed\n");
        exit(5);
    }
    fprintf(stderr,"[APU] XAudio2 output connected\n");
}

void mcpx_apu_monitor_finalize(MCPXAPUState *d)
{
    (void)d;
    xa2_shutdown();
}

void mcpx_apu_monitor_frame(MCPXAPUState *d)
{
    if ((d->ep_frame_div + 1) % 8) {
        return;
    }

    /* XAudio2 path: render and submit a buffer */
    if (xa2_is_active()) {
        int buf_size = xa2_get_buffer_size();
        int16_t xa2_tmp[1024][2];  /* matches XA2_BUF_SAMPLES max */
        int remaining = buf_size;
        int out_offset = 0;

        while (remaining > 0) {
            int chunk = (remaining < MIXER_FRAME_SAMPLES) ? remaining : MIXER_FRAME_SAMPLES;
            memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));

            if (g_test_tone.active && !g_audio_muted) {
                for (int i = 0; i < chunk; i++) {
                    int16_t s = (int16_t)(sin(g_test_tone.phase) * g_test_tone.amplitude);
                    d->monitor.frame_buf[i][0] = s;
                    d->monitor.frame_buf[i][1] = s;
                    g_test_tone.phase += g_test_tone.phase_inc;
                    if (g_test_tone.phase >= 2.0 * M_PI)
                        g_test_tone.phase -= 2.0 * M_PI;
                }
            }

            if (!g_audio_muted)
                mixer_render(d->monitor.frame_buf, chunk);

            memcpy(xa2_tmp + out_offset, d->monitor.frame_buf, chunk * 2 * sizeof(int16_t));
            out_offset += chunk;
            remaining -= chunk;
        }

        xa2_submit_samples((const int16_t *)xa2_tmp, buf_size);
        return;
    }

    fprintf(stderr,"[STOP] audio output is not connected\n");
    exit(5);
}

/* ============================================================
 * Throttle (timing control for frame pacing)
 * ============================================================ */

static bool apu_pipeline_active(MCPXAPUState *d)
{
    return GET_MASK(qatomic_read(&d->regs[NV_PAPU_SECTL]),
                    NV_PAPU_SECTL_XCNTMODE) != NV_PAPU_SECTL_XCNTMODE_OFF &&
           !(qatomic_read(&d->regs[NV_PAPU_FECTL]) &
             (NV_PAPU_FECTL_FEMETHMODE_TRAPPED | NV_PAPU_FECTL_FEMETHMODE_HALTED));
}

static void throttle(MCPXAPUState *d)
{
    if (d->monitor.ac97_frame_div % 8) {
        return;
    }

    /* Use the original monitor watermarks against the actual DAC cursor.
     * An inactive analog producer must retain its continuous wall clock. */
    int queued_bytes = -1;
    if (apu_pipeline_active(d) || g_test_tone.active || d->monitor.ac97_output_channel == 7) {
        queued_bytes = xa2_queued_bytes();
        while (!d->pause_requested && queued_bytes >= d->monitor.queued_bytes_high) {
            qemu_cond_timedwait(&d->cond, &d->lock, EP_FRAME_US / 1000);
            if (d->pause_requested) break;
            queued_bytes = xa2_queued_bytes();
        }
    }
    if (queued_bytes >= 0 && queued_bytes <= d->monitor.queued_bytes_low) return;

    int64_t now_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);

    if (d->next_frame_time_us == 0 ||
        now_us - d->next_frame_time_us > EP_FRAME_US) {
        d->next_frame_time_us = now_us;
    }

    while (!d->pause_requested) {
        now_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
        int64_t remaining_ms = (d->next_frame_time_us - now_us) / 1000;
        if (remaining_ms > 0) {
            qemu_cond_timedwait(&d->cond, &d->lock, (int)remaining_ms);
        } else {
            break;
        }
    }
    d->next_frame_time_us += EP_FRAME_US;
    if (queued_bytes >= 0) {
        int mid = (d->monitor.queued_bytes_low + d->monitor.queued_bytes_high) / 2;
        d->next_frame_time_us += (queued_bytes > mid) - (queued_bytes < mid);
    }

    d->sleep_acc_us += (int)(qemu_clock_get_us(QEMU_CLOCK_REALTIME) - now_us);
}

/* ============================================================
 * se_frame - Process one audio frame (VP -> GP -> EP pipeline)
 * ============================================================ */

static void se_frame(MCPXAPUState *d)
{
    mcpx_apu_update_dsp_preference(d);
    mcpx_debug_begin_frame();
    g_dbg.gp_realtime = d->gp.realtime;
    g_dbg.ep_realtime = d->ep.realtime;

    int64_t now_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    int64_t elapsed_ms = now_ms - d->frame_count_time_ms;
    if (elapsed_ms >= 1000) {
        g_dbg.utilization = 1.0f - d->sleep_acc_us / (elapsed_ms * 1000.0f);
        g_dbg.frames_processed = (int)(d->frame_count * 1000.0 / elapsed_ms + 0.5);
        d->frame_count_time_ms = now_ms;
        d->frame_count = 0;
        d->sleep_acc_us = 0;
    }
    d->frame_count++;

    /* Buffer for all mixbins for this frame */
    float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    memset(mixbins, 0, sizeof(mixbins));

    mcpx_apu_vp_frame(d, mixbins);
    mcpx_apu_dsp_frame(d, mixbins);
    /* The analog AC97 descriptor consumer sends the DSP output to the DAC. */

    mcpx_debug_end_frame();
}

/* ============================================================
 * APU frame thread (background processing)
 * ============================================================ */

static void *mcpx_apu_frame_thread(void *arg)
{
    MCPXAPUState *d = MCPX_APU_DEVICE(arg);
    xbox_CheckpointDeviceEnter(XBOX_CHECKPOINT_APU);
    xbox_CheckpointDeviceLeave(XBOX_CHECKPOINT_APU);
    qemu_mutex_lock(&d->lock);

    while (!qatomic_read(&d->exiting)) {
        if (d->pause_requested && !g_test_tone.active && !g_mixer_active_count) {
            d->is_idle = true;
            qemu_cond_signal(&d->idle_cond);
            qemu_cond_wait(&d->cond, &d->lock);
            d->is_idle = false;
            continue;
        }

        qemu_mutex_unlock(&d->lock);
        xbox_CheckpointDeviceEnter(XBOX_CHECKPOINT_APU);
        qemu_mutex_lock(&d->lock);
        if (g_apu_checkpoint_restart) {
            g_apu_checkpoint_restart=0;qemu_mutex_unlock(&d->lock);
            xbox_CheckpointDeviceLeave(XBOX_CHECKPOINT_APU);qemu_mutex_lock(&d->lock);continue;
        }
        /* Always run the audio output loop — the software mixer and test tone
         * need continuous frame delivery regardless of APU register state.
         * The VP/DSP pipeline (se_frame) only runs when registers allow it. */
        throttle(d);

        bool apu_active = apu_pipeline_active(d);

        if (apu_active && !g_test_tone.active) {
            /* Original VP/DSP publishes its PCM and physical FIFO output. */
            se_frame(d);
        } else {
            /* Lightweight: just monitor frame (test tone + software mixer) */
            if (g_test_tone.active) mcpx_apu_monitor_frame(d);
        }

        /* Turn actual VP or trapped-method notification into an IRQ level. */
        if (d->set_irq) {
            update_irq(d);
            d->set_irq = false;
        }
        unsigned off=(d->monitor.ac97_frame_div%8)*NUM_SAMPLES_PER_FRAME;
        if (d->monitor.point==MCPX_APU_DEBUG_MON_AC97)
            memset(d->monitor.frame_buf[off],0,NUM_SAMPLES_PER_FRAME*2*sizeof(int16_t));
        memset(d->monitor.spdif_buf[off],0,NUM_SAMPLES_PER_FRAME*2*sizeof(int16_t));
        mcpx_ac97_frame();
        bool output_ready=d->monitor.ac97_output_channel==7
            ? (d->monitor.ac97_frame_div%8)==7
            : apu_active && (d->ep_frame_div%8)==7;
        if (output_ready && !g_test_tone.active) {
            const int16_t *output=d->monitor.ac97_output_channel==7
                ? (const int16_t *)d->monitor.spdif_buf : (const int16_t *)d->monitor.frame_buf;
            /* The DAC queue may wait for its existing buffer callback. Keep
             * the completed PCM block stable, and allow original FE/IRQ
             * methods to acquire the device while the host consumes it. */
            int16_t completed_output[256][2];
            memcpy(completed_output,output,sizeof(completed_output));
            memset(d->monitor.frame_buf,0,sizeof(d->monitor.frame_buf));
            qemu_mutex_unlock(&d->lock);
            int accepted=xa2_submit_samples((const int16_t *)completed_output,256);
            qemu_mutex_lock(&d->lock);
            if (!accepted) {
                fprintf(stderr,"[STOP] XAudio2 did not accept the actual AC97 frame\n");
                exit(5);
            }
        }
        /* DSP banks advance only when their actual producer runs. AC97 has
         * its own continuous sample clock while FE is trapped or halted. */
        if(apu_active || g_test_tone.active)d->ep_frame_div++;
        d->monitor.ac97_frame_div++;

        /* Let the guest in once per frame.
         *
         * The thread holds d->lock for its whole loop and only drops it inside
         * throttle()'s wait. Once voices really play, processing can run
         * behind real time, throttle never waits, and the lock is never
         * released -- while every VOICE_ON/OFF/RELEASE the title writes needs
         * it (voice_lock). A critical section is not fair, so the title's
         * thread starved there indefinitely: Burnout 3 froze on its vehicle
         * select, blocked in voice_lock at raised IRQL, which in turn held
         * off every USB interrupt. */
        qemu_mutex_unlock(&d->lock);
        xbox_CheckpointDeviceLeave(XBOX_CHECKPOINT_APU);
        SwitchToThread();
        qemu_mutex_lock(&d->lock);
    }

    qemu_mutex_unlock(&d->lock);
    return NULL;
}

/* ============================================================
 * Wait for idle / resume helpers
 * ============================================================ */

static void mcpx_apu_wait_for_idle(MCPXAPUState *d)
{
    d->pause_requested = true;
    qemu_cond_signal(&d->cond);
    while (!d->is_idle) {
        qemu_cond_wait(&d->idle_cond, &d->lock);
    }
}

static void mcpx_apu_resume(MCPXAPUState *d)
{
    d->pause_requested = false;
    qemu_cond_signal(&d->cond);
}
typedef struct ApuCheckpoint {
    uint32_t state_bytes,filter_ops[MCPX_HW_MAX_VOICES][2];
    uint8_t resampler_active[MCPX_HW_MAX_VOICES];
    int32_t audio_muted,voice_monitor;
    uint64_t muted_voices[4];
    unsigned char tone[sizeof g_test_tone];
    MCPXAPUState state;
} ApuCheckpoint;
static unsigned apu_filter_output(const sv_filter *filter) {
    const float *fields[]={NULL,&filter->l,&filter->h,&filter->b,&filter->n,&filter->p};
    for(unsigned i=0;i<6;i++) if(filter->op==fields[i]) return i;
    return 6;
}
int mcpx_apu_save_checkpoint(MCPXAPUState *d,FILE *out) {
    if (!d || !out || !xbox_CheckpointFrozen()) return 0;
    for(unsigned i=0;i<APU_MIXER_MAX_VOICES;i++) if(g_mixer_voices[i].pcm_data || g_mixer_voices[i].active) return 0;
    if (d->vp.voice_work_dispatch.workers || d->vp.voice_work_dispatch.num_workers || d->exiting) return 0;
    ApuCheckpoint *saved=calloc(1,sizeof *saved);if(!saved) return 0;
    qemu_mutex_lock(&d->lock);saved->state_bytes=sizeof saved->state;memcpy(&saved->state,d,sizeof *d);
    saved->audio_muted=g_audio_muted;saved->voice_monitor=g_dbg_voice_monitor;
    memcpy(saved->muted_voices,g_dbg_muted_voices,sizeof saved->muted_voices);memcpy(saved->tone,&g_test_tone,sizeof g_test_tone);
    MCPXAPUState *state=&saved->state;state->parent_obj.irq_notify=NULL;state->parent_obj.irq_opaque=NULL;
    memset(&state->apu_thread,0,sizeof state->apu_thread);memset(&state->lock,0,sizeof state->lock);
    memset(&state->cond,0,sizeof state->cond);memset(&state->idle_cond,0,sizeof state->idle_cond);
    state->is_idle=false;state->ram=NULL;state->ram_ptr=NULL;state->physical_resolve=NULL;state->monitor.stream=NULL;
    state->mmio.opaque=state->vp.mmio.opaque=state->gp.mmio.opaque=state->ep.mmio.opaque=NULL;
    memset(&state->vp.voice_work_dispatch,0,sizeof state->vp.voice_work_dispatch);
    state->gp.dsp=state->ep.dsp=NULL;
    int ok=1;
    for(unsigned v=0;ok && v<MCPX_HW_MAX_VOICES;v++) {
        saved->resampler_active[v]=d->vp.filters[v].resampler!=NULL;
        if(saved->resampler_active[v] && d->vp.filters[v].voice!=v) {ok=0;break;}
        state->vp.filters[v].resampler=NULL;
        for(unsigned ch=0;ch<2;ch++) {
            saved->filter_ops[v][ch]=apu_filter_output(&d->vp.filters[v].svf[ch]);
            ok=ok && saved->filter_ops[v][ch]<6;state->vp.filters[v].svf[ch].op=NULL;
        }
    }
    int64_t now=qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    if(state->next_frame_time_us) state->next_frame_time_us-=now;
    state->frame_count_time_ms-=now/1000;
    if(state->throttle.deviation.last_us) state->throttle.deviation.last_us-=now;
    ok=ok && fwrite("XBAPUS",6,1,out)==1 && fwrite(saved,sizeof *saved,1,out)==1;
    for(unsigned v=0;ok && v<MCPX_HW_MAX_VOICES;v++) if(saved->resampler_active[v])
        ok=apu_resampler_save(out,d->vp.filters[v].resampler,d->vp.filters[v].resample_buf,
                             ARRAY_SIZE(d->vp.filters[v].resample_buf));
    ok=ok && dsp_save_checkpoint(d->gp.dsp,out) && dsp_save_checkpoint(d->ep.dsp,out) &&
       mcpx_ac97_save_checkpoint(out) && xa2_save_checkpoint(out);
    qemu_mutex_unlock(&d->lock);free(saved);return ok;
}
int mcpx_apu_load_checkpoint(MCPXAPUState *d,FILE *in) {
    if (!d || !in || !xbox_CheckpointFrozen()) return 0;
    char magic[6];ApuCheckpoint *saved=malloc(sizeof *saved);if(!saved) return 0;
    int ok=fread(magic,6,1,in)==1 && !memcmp(magic,"XBAPUS",6) && fread(saved,sizeof *saved,1,in)==1 &&
        saved->state_bytes==sizeof saved->state && saved->state.monitor.ac97_output_channel==d->monitor.ac97_output_channel;
    MCPXAPUState *state=&saved->state;
    if (ok) {
        const unsigned char *bytes=(const unsigned char *)state;
        ok=bytes[offsetof(MCPXAPUState,set_irq)]<=1 && bytes[offsetof(MCPXAPUState,pause_requested)]<=1 &&
           bytes[offsetof(MCPXAPUState,gp)+offsetof(MCPXAPUGPState,realtime)]<=1 &&
           bytes[offsetof(MCPXAPUState,ep)+offsetof(MCPXAPUEPState,realtime)]<=1 && saved->tone[0]<=1 &&
           !state->exiting && !state->gp.dsp && !state->ep.dsp && !state->ram && !state->ram_ptr &&
           !state->physical_resolve && !state->monitor.stream && !state->vp.voice_work_dispatch.workers;
    }
    for(unsigned v=0;ok && v<MCPX_HW_MAX_VOICES;v++)
        ok=!state->vp.filters[v].resampler && saved->resampler_active[v]<=1 &&
            saved->filter_ops[v][0]<6 && saved->filter_ops[v][1]<6 &&
            (!saved->resampler_active[v] || state->vp.filters[v].voice==v);
    SRC_STATE *resamplers[MCPX_HW_MAX_VOICES]={0};
    for(unsigned v=0;ok && v<MCPX_HW_MAX_VOICES;v++) if(saved->resampler_active[v])
        ok=(resamplers[v]=mcpx_apu_vp_prepare_resampler(&d->vp.filters[v],in))!=NULL;
    void *gp=NULL,*ep=NULL,*ac97=NULL,*dac=NULL;
    if (ok) ok=(gp=dsp_prepare_checkpoint(d->gp.dsp,in))!=NULL && (ep=dsp_prepare_checkpoint(d->ep.dsp,in))!=NULL &&
                (ac97=mcpx_ac97_prepare_checkpoint(in))!=NULL && (dac=xa2_prepare_checkpoint(in))!=NULL;
    if (ok) {
        qemu_mutex_lock(&d->lock);ok=xa2_apply_checkpoint(dac);
        if (ok) {
            dsp_apply_checkpoint(d->gp.dsp,gp);dsp_apply_checkpoint(d->ep.dsp,ep);mcpx_ac97_apply_checkpoint(ac97);
            d->parent_obj.devfn=state->parent_obj.devfn;d->parent_obj.irq_level=state->parent_obj.irq_level;
            memcpy(d->parent_obj.config,state->parent_obj.config,sizeof d->parent_obj.config);
            d->set_irq=state->set_irq;d->pause_requested=state->pause_requested;d->mmio.size=state->mmio.size;
            d->vp.mmio.size=state->vp.mmio.size;
            for(unsigned v=0;v<MCPX_HW_MAX_VOICES;v++)src_delete(d->vp.filters[v].resampler);
            memcpy(d->vp.filters,state->vp.filters,sizeof d->vp.filters);
            for(unsigned v=0;v<MCPX_HW_MAX_VOICES;v++) {
                d->vp.filters[v].resampler=resamplers[v];resamplers[v]=NULL;
            }
            for(unsigned v=0;v<MCPX_HW_MAX_VOICES;v++) for(unsigned ch=0;ch<2;ch++) {
                sv_filter *filter=&d->vp.filters[v].svf[ch];
                float *fields[]={NULL,&filter->l,&filter->h,&filter->b,&filter->n,&filter->p};
                filter->op=fields[saved->filter_ops[v][ch]];
            }
            size_t offset=offsetof(MCPXAPUVPState,ssl_base_page);
            memcpy((char *)&d->vp+offset,(const char *)&state->vp+offset,sizeof d->vp-offset);
            d->gp.realtime=state->gp.realtime;d->ep.realtime=state->ep.realtime;
            d->gp.mmio.size=state->gp.mmio.size;d->ep.mmio.size=state->ep.mmio.size;
            memcpy(d->gp.regs,state->gp.regs,sizeof d->gp.regs);memcpy(d->ep.regs,state->ep.regs,sizeof d->ep.regs);
            offset=offsetof(MCPXAPUState,regs);
            memcpy((char *)d+offset,(const char *)state+offset,offsetof(MCPXAPUState,monitor.stream)-offset);
            d->monitor.queued_bytes_low=state->monitor.queued_bytes_low;d->monitor.queued_bytes_high=state->monitor.queued_bytes_high;
            int64_t now=qemu_clock_get_us(QEMU_CLOCK_REALTIME);
            if(d->next_frame_time_us) d->next_frame_time_us+=now;
            d->frame_count_time_ms+=now/1000;
            if(d->throttle.deviation.last_us) d->throttle.deviation.last_us+=now;
            g_audio_muted=saved->audio_muted;g_dbg_voice_monitor=saved->voice_monitor;
            memcpy(g_dbg_muted_voices,saved->muted_voices,sizeof g_dbg_muted_voices);memcpy(&g_test_tone,saved->tone,sizeof g_test_tone);
            g_apu_checkpoint_restart=1;qemu_cond_signal(&d->cond);
        }
        qemu_mutex_unlock(&d->lock);
    }
    for(unsigned v=0;v<MCPX_HW_MAX_VOICES;v++)src_delete(resamplers[v]);
    dsp_free_checkpoint(gp);dsp_free_checkpoint(ep);free(ac97);free(dac);free(saved);return ok;
}

/* ============================================================
 * Reset
 * ============================================================ */

static void mcpx_apu_reset_locked(MCPXAPUState *d)
{
    memset(d->regs, 0, sizeof(d->regs));
    mcpx_apu_vp_reset(d);

    if (d->gp.dsp) {
        dsp_invalidate_opcache(d->gp.dsp);
    }
    if (d->ep.dsp) {
        dsp_invalidate_opcache(d->ep.dsp);
    }
    d->set_irq = false;
}

/* ============================================================
 * Public API: Init / Shutdown
 * ============================================================ */

MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr,
    void *(*physical_resolve)(uint32_t, uint32_t))
{
    if (!physical_resolve) {
        fprintf(stderr, "[STOP] APU physical bus is not connected\n");
        exit(5);
    }
    MCPXAPUState *d = (MCPXAPUState *)calloc(1, sizeof(MCPXAPUState));
    if (!d) {
        fprintf(stderr, "[APU] Failed to allocate MCPXAPUState\n");
        return NULL;
    }

    g_apu_ram_ptr = ram_ptr;
    g_state = d;
    d->ram_ptr = ram_ptr;
    d->physical_resolve = physical_resolve;
    const char *output=getenv("RECOMP_AUDIO_OUTPUT");
    if (!output || !strcmp(output,"analog")) d->monitor.ac97_output_channel=1;
    else if (!strcmp(output,"spdif")) d->monitor.ac97_output_channel=7;
    else {
        fprintf(stderr,"[STOP] invalid RECOMP_AUDIO_OUTPUT: %s\n",output);
        exit(5);
    }
    fprintf(stderr,"[APU] fixed PC output: %s\n",d->monitor.ac97_output_channel==7 ? "AC97 spdif" : "original DSP PCM");

    d->set_irq = false;
    d->exiting = false;
    d->is_idle = false;
    d->pause_requested = true;

    qemu_mutex_init(&d->lock);
    qemu_mutex_lock(&d->lock);
    qemu_cond_init(&d->cond);
    qemu_cond_init(&d->idle_cond);

    /* Init VP (voice processor) */
    mcpx_apu_vp_init(d);

    /* Connect the original GP/EP interpreter and DMA. */
    mcpx_apu_dsp_init(d);

    /* Init software mixer for DirectSound bridge */
    mixer_init();

    /* Connect the host DAC endpoint. */
    Error *local_err = NULL;
    mcpx_apu_monitor_init(d, &local_err);
    if (local_err) {
        warn_reportf_err(local_err, "mcpx_apu_monitor_init failed: ");
    }

    /* Start background frame thread */
    xbox_CheckpointDeviceCreating(XBOX_CHECKPOINT_APU);
    qemu_thread_create(&d->apu_thread, "mcpx.apu_thread",
                       mcpx_apu_frame_thread, d, QEMU_THREAD_JOINABLE);
    if (!d->apu_thread.thread) {
        fprintf(stderr,"[STOP] APU frame worker creation failed\n");
        exit(5);
    }
    mcpx_apu_wait_for_idle(d);
    qemu_mutex_unlock(&d->lock);

    fprintf(stderr, "[APU] MCPX APU initialized (standalone)\n");
    fprintf(stderr, "[APU]   RAM pointer: %p\n", (void *)ram_ptr);
    fprintf(stderr, "[APU]   MMIO base: 0xFE800000 (512KB)\n");
    fprintf(stderr, "[APU]   VP: %d max voices, %d samples/frame\n",
            MCPX_HW_MAX_VOICES, NUM_SAMPLES_PER_FRAME);
    return d;
}

void mcpx_apu_shutdown(MCPXAPUState *d)
{
    if (!d) return;

    fprintf(stderr, "[APU] Shutting down MCPX APU...\n");

    qemu_mutex_lock(&d->lock);
    mcpx_apu_wait_for_idle(d);
    qatomic_set(&d->exiting, true);
    qemu_cond_signal(&d->cond);
    qemu_mutex_unlock(&d->lock);

    qemu_thread_join(&d->apu_thread);
    mcpx_apu_vp_finalize(d);
    dsp_destroy(d->gp.dsp);
    dsp_destroy(d->ep.dsp);
    mcpx_apu_monitor_finalize(d);

    free(d);
    g_state = NULL;
    fprintf(stderr, "[APU] Shutdown complete\n");
}

/* ============================================================
 * VP MMIO handlers (sub-region at +0x20000)
 *
 * These are called when the game writes to the VP PIO registers
 * to configure voices, SSL, etc.
 * ============================================================ */

uint64_t mcpx_apu_vp_read(void *opaque, hwaddr addr, unsigned int size);
void mcpx_apu_vp_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size);

/* Dispatch a VP-region access (offset 0x20000-0x2FFFF from APU base) */
void mcpx_apu_dispatch_mmio(MCPXAPUState *d, hwaddr addr, uint64_t val,
                             unsigned int size, bool is_write)
{
    if (addr >= 0x20000 && addr < 0x30000) {
        /* VP region */
        hwaddr vp_addr = addr - 0x20000;
        if (is_write) {
            mcpx_apu_vp_write(d, vp_addr, val, size);
        }
        /* VP reads handled by caller if needed */
    } else if (addr < 0x20000) {
        /* Main APU registers */
        if (is_write) {
            mcpx_apu_write(d, addr, val, size);
        }
    }
    /* GP (0x30000) and EP (0x50000) regions ignored for now */
}

/* ============================================================
 * Public MMIO API (called from VEH or MMIO hook)
 * addr is offset from APU base (0xFE800000)
 * ============================================================ */

uint64_t mcpx_apu_mmio_read(MCPXAPUState *d, uint64_t addr, unsigned int size)
{
    if (!d || size!=4 || (addr&3)) {
        fprintf(stderr,"[STOP] unsupported APU read: offset=%llX size=%u\n",(unsigned long long)addr,size);
        exit(5);
    }
    if (addr >= 0x20000 && addr < 0x30000) {
        return mcpx_apu_vp_read(d, addr - 0x20000, size);
    } else if (addr < 0x20000) {
        return mcpx_apu_read(d, (hwaddr)addr, size);
    }
    if (addr>=0x30000 && addr<0x40000) return gp_ops.read(d,addr-0x30000,size);
    if (addr>=0x50000 && addr<0x60000) return ep_ops.read(d,addr-0x50000,size);
    fprintf(stderr,"[STOP] APU unmapped read: offset=%llX\n",(unsigned long long)addr);
    exit(5);
}

void mcpx_apu_mmio_write(MCPXAPUState *d, uint64_t addr, uint64_t val, unsigned int size)
{
    if (!d || size!=4 || (addr&3) || addr>=0x60000 || (addr>=0x40000 && addr<0x50000)) {
        fprintf(stderr,"[STOP] APU DSP-window or unsupported write: offset=%llX value=%08llX size=%u\n",
                (unsigned long long)addr,(unsigned long long)val,size);
        exit(5);
    }
    if (addr>=0x30000 && addr<0x40000) gp_ops.write(d,addr-0x30000,val,size);
    else if (addr>=0x50000 && addr<0x60000) ep_ops.write(d,addr-0x50000,val,size);
    else {
        /* The direct frame consumer already owns this lock. A single FE
         * method must commit its state together before another frame sees it. */
        qemu_mutex_lock(&d->lock);
        mcpx_apu_dispatch_mmio(d, (hwaddr)addr, val, size, true);
        qemu_mutex_unlock(&d->lock);
    }
}

/* ============================================================
 * APU Test Tone - Direct XAudio2 sine generator
 *
 * Bypasses the VP pipeline entirely and writes a 440Hz sine wave
 * directly to the monitor frame_buf. This verifies that XAudio2
 * output works correctly.
 * ============================================================ */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void mcpx_apu_play_test_tone(MCPXAPUState *d)
{
    if (!d) {
        fprintf(stderr, "[APU-TEST] No APU state\n");
        return;
    }

    if (g_test_tone.active) {
        /* Toggle off */
        g_test_tone.active = false;
        fprintf(stderr, "[APU-TEST] Test tone OFF\n");
        return;
    }

    /* 440Hz at 48kHz sample rate */
    g_test_tone.phase = 0.0;
    g_test_tone.phase_inc = 2.0 * M_PI * 440.0 / 48000.0;
    g_test_tone.amplitude = 6000;  /* ~18% of full scale */
    g_test_tone.active = true;

    /* Enable SECTL and resume the existing APU thread. */
    qemu_mutex_lock(&d->lock);
    d->regs[NV_PAPU_SECTL] = NV_PAPU_SECTL_XCNTMODE & ~NV_PAPU_SECTL_XCNTMODE_OFF;
    d->regs[NV_PAPU_FECTL] = NV_PAPU_FECTL_FEMETHMODE_FREE_RUNNING;
    /* Initialize empty voice lists so VP frame doesn't crash */
    d->regs[NV_PAPU_TVL2D] = 0xFFFF;
    d->regs[NV_PAPU_TVL3D] = 0xFFFF;
    d->regs[NV_PAPU_TVLMP] = 0xFFFF;
    mcpx_apu_resume(d);
    qemu_mutex_unlock(&d->lock);

    fprintf(stderr, "[APU-TEST] Test tone ON - 440Hz sine, amplitude=%d\n",
            g_test_tone.amplitude);
}

/* ============================================================
 * Software mixer - mixes explicit HLE buffers to XAudio2
 *
 * This bypasses the VP hardware voice pipeline entirely.
 * DirectSound buffers register PCM data here, and the APU
 * frame thread mixes them into the monitor frame_buf.
 * ============================================================ */

static void mixer_init(void)
{
    if (g_mixer_initialized) return;
    InitializeCriticalSection(&g_mixer_cs);
    memset(g_mixer_voices, 0, sizeof(g_mixer_voices));
    g_mixer_initialized = true;
}

int apu_mixer_alloc_voice(void)
{
    if (!g_mixer_initialized) mixer_init();
    EnterCriticalSection(&g_mixer_cs);
    for (int i = 0; i < APU_MIXER_MAX_VOICES; i++) {
        if (!g_mixer_voices[i].active && !g_mixer_voices[i].pcm_data) {
            g_mixer_voices[i].volume = 1.0f;
            g_mixer_voices[i].sample_rate = 44100;
            g_mixer_voices[i].num_channels = 2;
            LeaveCriticalSection(&g_mixer_cs);
            return i;
        }
    }
    LeaveCriticalSection(&g_mixer_cs);
    return -1;
}

void apu_mixer_free_voice(int slot)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    EnterCriticalSection(&g_mixer_cs);
    apu_mixer_stop(slot);
    g_mixer_voices[slot].pcm_data = NULL;
    g_mixer_voices[slot].pcm_bytes = 0;
    g_mixer_voices[slot].play_offset = 0;
    LeaveCriticalSection(&g_mixer_cs);
}

APUMixerVoice *apu_mixer_get_voice(int slot)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return NULL;
    return &g_mixer_voices[slot];
}

int apu_mixer_set_position(int slot, uint32_t byte_offset)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return 0;
    EnterCriticalSection(&g_mixer_cs);
    APUMixerVoice *v = &g_mixer_voices[slot];
    uint32_t frame_bytes = v->num_channels * sizeof(int16_t);
    int valid = (v->num_channels == 1 || v->num_channels == 2) &&
                byte_offset < v->pcm_bytes &&
                byte_offset / frame_bytes < v->pcm_bytes / frame_bytes;
    if (valid) v->play_offset = ((uint64_t)(byte_offset / frame_bytes)) << 16;
    LeaveCriticalSection(&g_mixer_cs);
    return valid;
}

void apu_mixer_get_state(int slot, uint32_t *byte_offset, int *active, int *looping)
{
    if (byte_offset) *byte_offset = 0;
    if (active) *active = 0;
    if (looping) *looping = 0;
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    EnterCriticalSection(&g_mixer_cs);
    APUMixerVoice *v = &g_mixer_voices[slot];
    if (byte_offset) *byte_offset = (uint32_t)(v->play_offset >> 16) *
                                    v->num_channels * sizeof(int16_t);
    if (active) *active = v->active;
    if (looping) *looping = v->active && v->looping;
    LeaveCriticalSection(&g_mixer_cs);
}

void apu_mixer_play(int slot, int looping)
{
    if (g_audio_muted) return;
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    EnterCriticalSection(&g_mixer_cs);
    APUMixerVoice *v = &g_mixer_voices[slot];
    if (!v->pcm_data || (v->num_channels != 1 && v->num_channels != 2) ||
        v->pcm_bytes < v->num_channels * sizeof(int16_t)) {
        LeaveCriticalSection(&g_mixer_cs);
        return;
    }
    v->looping = looping;
    if (!v->active) InterlockedIncrement((volatile LONG *)&g_mixer_active_count);
    v->active = 1;

    static int play_log_count = 0;
    if (play_log_count < 20) {
        fprintf(stderr, "[APU-MIX] Play voice %d: %u bytes, %u ch, %u Hz, vol=%.2f, loop=%d\n",
                slot, v->pcm_bytes, v->num_channels, v->sample_rate, v->volume, looping);
        play_log_count++;
    }
    LeaveCriticalSection(&g_mixer_cs);

    /* The frame thread takes the APU lock before the mixer lock. */
    extern MCPXAPUState *g_state;
    if (g_state) {
        qemu_mutex_lock(&g_state->lock);
        g_state->pause_requested = false;
        qemu_cond_signal(&g_state->cond);
        qemu_mutex_unlock(&g_state->lock);
    }
}

void apu_mixer_stop(int slot)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    EnterCriticalSection(&g_mixer_cs);
    if (g_mixer_voices[slot].active) {
        g_mixer_voices[slot].active = 0;
        InterlockedDecrement((volatile LONG *)&g_mixer_active_count);
    }
    LeaveCriticalSection(&g_mixer_cs);
}

/* Mix all active voices into frame_buf. Called from mcpx_apu_monitor_frame.
 * Keep 16 fractional bits in a wide offset so buffers can exceed 65536 frames. */
static void mixer_render(int16_t frame_buf[][2], int num_samples)
{
    if (!g_mixer_initialized) return;
    EnterCriticalSection(&g_mixer_cs);

    for (int v = 0; v < APU_MIXER_MAX_VOICES; v++) {
        APUMixerVoice *voice = &g_mixer_voices[v];
        if (!voice->active || !voice->pcm_data || voice->pcm_bytes == 0)
            continue;

        uint32_t total_frames = voice->pcm_bytes / sizeof(int16_t);
        if (voice->num_channels == 2) total_frames /= 2;
        if (total_frames == 0) continue;

        /* Fixed-point 16.16 increment per output sample */
        uint64_t inc = ((uint64_t)voice->sample_rate << 16) / 48000;
        uint64_t pos = voice->play_offset;
        uint64_t end = (uint64_t)total_frames << 16;
        float vol = voice->volume;

        for (int i = 0; i < num_samples; i++) {
            uint32_t src_frame = pos >> 16;

            if (src_frame >= total_frames) {
                if (voice->looping) {
                    pos %= end;
                    src_frame = (uint32_t)(pos >> 16);
                } else {
                    pos = 0;
                    voice->active = 0;
                    InterlockedDecrement((volatile LONG *)&g_mixer_active_count);
                    break;
                }
            }

            int32_t left, right;
            if (voice->num_channels >= 2) {
                left  = (int32_t)(voice->pcm_data[src_frame * 2] * vol);
                right = (int32_t)(voice->pcm_data[src_frame * 2 + 1] * vol);
            } else {
                left = right = (int32_t)(voice->pcm_data[src_frame] * vol);
            }

            /* Accumulate (mix) into frame_buf with clamping */
            int32_t mixed_l = frame_buf[i][0] + left;
            int32_t mixed_r = frame_buf[i][1] + right;
            if (mixed_l > 32767) mixed_l = 32767;
            if (mixed_l < -32768) mixed_l = -32768;
            if (mixed_r > 32767) mixed_r = 32767;
            if (mixed_r < -32768) mixed_r = -32768;
            frame_buf[i][0] = (int16_t)mixed_l;
            frame_buf[i][1] = (int16_t)mixed_r;

            pos += inc;
        }

        voice->play_offset = pos;
        uint32_t end_frame = pos >> 16;
        if (end_frame >= total_frames) {
            if (voice->looping) {
                voice->play_offset = pos % end;
            } else if (voice->active) {
                voice->play_offset = 0;
                voice->active = 0;
                InterlockedDecrement((volatile LONG *)&g_mixer_active_count);
            }
        }
    }
    LeaveCriticalSection(&g_mixer_cs);
}
