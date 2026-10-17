/**
 * XAudio2 Audio Output Backend
 *
 * Provides low-latency audio output via XAudio2 (Win7+).
 * Called from the APU monitor frame to submit mixed samples.
 * Reports initialization failure to the caller.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "apu_xaudio2.h"
#include "apu_debug.h"
#include "kernel/kernel.h"

/* The XAudio2 backend is Windows-only. On Linux all xa2_* functions are
 * stubbed to report inactive; real audio output via SDL2 comes later. */
#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>
#include <xaudio2.h>

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

#define XA2_SAMPLE_RATE   48000
#define XA2_CHANNELS      2
#define XA2_BUF_SAMPLES   1024   /* Per-slot capacity; DSP submits 256 frames. */
#define XA2_NUM_BUFS      16

static IXAudio2               *g_xa2 = NULL;
static IXAudio2MasteringVoice *g_xa2_master = NULL;
static IXAudio2SourceVoice    *g_xa2_source = NULL;
static int16_t                 g_xa2_bufs[XA2_NUM_BUFS][XA2_BUF_SAMPLES][2];
static int                     g_xa2_next_buf = 0;
static int                     g_xa2_initialized = 0;
static int                     g_xa2_frames_written = 0;
static HANDLE                  g_xa2_buffer_end = NULL;
static volatile LONG           g_xa2_voice_error;
typedef struct Xa2Queued { uint64_t start;uint32_t samples,pending; } Xa2Queued;
static Xa2Queued g_xa2_queued[XA2_NUM_BUFS];
static SRWLOCK g_xa2_queue_lock=SRWLOCK_INIT;
static uint64_t g_xa2_submitted_samples,g_xa2_played_bias;
static int g_xa2_checkpoint_paused;
static uintptr_t g_xa2_generation=1;

static void STDMETHODCALLTYPE cb_pass_start(IXAudio2VoiceCallback *self, UINT32 bytes) { (void)self; (void)bytes; }
static void STDMETHODCALLTYPE cb_pass_end(IXAudio2VoiceCallback *self) { (void)self; }
static void STDMETHODCALLTYPE cb_stream_end(IXAudio2VoiceCallback *self) { (void)self; }
static void STDMETHODCALLTYPE cb_buffer_start(IXAudio2VoiceCallback *self, void *context) { (void)self; (void)context; }
static void STDMETHODCALLTYPE cb_buffer_end(IXAudio2VoiceCallback *self, void *context)
{
    (void)self;
    unsigned index=(unsigned)((uintptr_t)context&255);
    if (index && index<=XA2_NUM_BUFS && ((uintptr_t)context>>8)==g_xa2_generation) {
        AcquireSRWLockExclusive(&g_xa2_queue_lock);g_xa2_queued[index-1].pending=0;
        ReleaseSRWLockExclusive(&g_xa2_queue_lock);
    }
    SetEvent(g_xa2_buffer_end);
}
static void STDMETHODCALLTYPE cb_loop_end(IXAudio2VoiceCallback *self, void *context) { (void)self; (void)context; }
static void STDMETHODCALLTYPE cb_voice_error(IXAudio2VoiceCallback *self, void *context, HRESULT error)
{
    (void)self; (void)context;
    InterlockedExchange(&g_xa2_voice_error,(LONG)error);
    SetEvent(g_xa2_buffer_end);
}
static IXAudio2VoiceCallbackVtbl g_xa2_callback_vtable = {
    cb_pass_start,cb_pass_end,cb_stream_end,cb_buffer_start,
    cb_buffer_end,cb_loop_end,cb_voice_error
};
static IXAudio2VoiceCallback g_xa2_callback = { &g_xa2_callback_vtable };
static HRESULT xa2_create_source(IXAudio2SourceVoice **voice) {
    WAVEFORMATEX wfx={0};wfx.wFormatTag=WAVE_FORMAT_PCM;wfx.nChannels=XA2_CHANNELS;
    wfx.nSamplesPerSec=XA2_SAMPLE_RATE;wfx.wBitsPerSample=16;
    wfx.nBlockAlign=XA2_CHANNELS*2;wfx.nAvgBytesPerSec=wfx.nSamplesPerSec*wfx.nBlockAlign;
    HRESULT result=IXAudio2_CreateSourceVoice(g_xa2,voice,&wfx,0,XAUDIO2_DEFAULT_FREQ_RATIO,&g_xa2_callback,NULL,NULL);
    return FAILED(result)?result:IXAudio2SourceVoice_Start(*voice,0,XAUDIO2_COMMIT_NOW);
}

int xa2_init(void)
{
    HRESULT hr;
    int com_initialized;

    if (g_xa2_initialized) return 1;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "[XA2] CoInitializeEx failed: 0x%08lX\n", hr);
        return 0;
    }
    com_initialized = SUCCEEDED(hr);
    g_xa2_buffer_end=CreateEventA(NULL,FALSE,FALSE,NULL);
    if (!g_xa2_buffer_end) goto fail;
    if (g_dbg.trace_enabled) fprintf(stderr,"  [XA2] buffer-completion event=%p\n",g_xa2_buffer_end);
    InterlockedExchange(&g_xa2_voice_error,S_OK);

    hr = XAudio2Create(&g_xa2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !g_xa2) {
        fprintf(stderr, "[XA2] XAudio2Create failed: 0x%08lX\n", hr);
        goto fail;
    }

    hr = IXAudio2_CreateMasteringVoice(g_xa2, &g_xa2_master,
        XA2_CHANNELS, XA2_SAMPLE_RATE, 0, NULL, NULL, 0);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateMasteringVoice failed: 0x%08lX\n", hr);
        goto fail;
    }

    hr = xa2_create_source(&g_xa2_source);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] Start failed: 0x%08lX\n", hr);
        goto fail;
    }

    g_xa2_next_buf = 0;
    g_xa2_initialized = 1;
    g_xa2_frames_written = 0;
    memset(g_xa2_queued,0,sizeof g_xa2_queued);g_xa2_submitted_samples=g_xa2_played_bias=0;

    fprintf(stderr, "[XA2] XAudio2 initialized (%d Hz stereo 16-bit, %d x %d-sample buffers)\n",
            XA2_SAMPLE_RATE, XA2_NUM_BUFS, XA2_BUF_SAMPLES);
    return 1;

fail:
    xa2_shutdown();
    /* Failed initialization still runs on the COM-initializing thread. */
    if (com_initialized) CoUninitialize();
    return 0;
}

void xa2_shutdown(void)
{
    if (g_xa2_source) {
        IXAudio2SourceVoice_Stop(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
        IXAudio2SourceVoice_FlushSourceBuffers(g_xa2_source);
        g_xa2_source->lpVtbl->DestroyVoice(g_xa2_source);
        g_xa2_source = NULL;
    }
    if (g_xa2_master) {
        g_xa2_master->lpVtbl->DestroyVoice(g_xa2_master);
        g_xa2_master = NULL;
    }
    if (g_xa2) {
        IXAudio2_Release(g_xa2);
        g_xa2 = NULL;
    }
    if (g_xa2_buffer_end) {
        CloseHandle(g_xa2_buffer_end);
        g_xa2_buffer_end = NULL;
    }

    if (g_xa2_initialized)
        fprintf(stderr, "[XA2] Shut down (%d frames written)\n", g_xa2_frames_written);
    g_xa2_initialized = 0;
    g_xa2_checkpoint_paused=0;
}

int xa2_is_active(void)
{
    return g_xa2_initialized;
}

/* Submit a buffer of mixed samples to XAudio2.
 * Called from APU frame thread. Returns 1 if buffer was submitted. */
int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    XAUDIO2_VOICE_STATE state;
    XAUDIO2_BUFFER xbuf;
    int idx;
    int copy_samples;
    HRESULT hr;

    if (!g_xa2_initialized || !g_xa2_source) {
        fprintf(stderr,"[XA2] source voice is not connected\n");
        return 0;
    }
    for (;;) {
        HRESULT voice_error=(HRESULT)InterlockedCompareExchange(&g_xa2_voice_error,0,0);
        if (FAILED(voice_error)) {
            fprintf(stderr,"[XA2] voice callback failed: 0x%08lX\n",voice_error);
            return 0;
        }
        IXAudio2SourceVoice_GetState(g_xa2_source,&state,XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if ((int)state.BuffersQueued<XA2_NUM_BUFS) break;
        /* Buffer ownership changes only when XAudio2 consumes its last byte.
         * This callback does not take the APU lock or depend on guest work. */
        DWORD wait_result=WaitForSingleObject(g_xa2_buffer_end,1000);
        if (wait_result!=WAIT_OBJECT_0) {
            fprintf(stderr,"[XA2] buffer completion wait failed: result=0x%08lX error=%lu event=%p\n",wait_result,GetLastError(),g_xa2_buffer_end);
            return 0;
        }
        if (g_dbg.trace_enabled) {
            static unsigned shown;
            if (shown++ < 4) {
                IXAudio2SourceVoice_GetState(g_xa2_source,&state,0);
                fprintf(stderr,"  [XA2] completion observed: queued=%u actual_played=%llu\n",
                        state.BuffersQueued,(unsigned long long)state.SamplesPlayed);
            }
        }
    }

    idx = g_xa2_next_buf;
    copy_samples = (num_samples > XA2_BUF_SAMPLES) ? XA2_BUF_SAMPLES : num_samples;
    memcpy(g_xa2_bufs[idx], samples, copy_samples * XA2_CHANNELS * sizeof(int16_t));

    memset(&xbuf, 0, sizeof(xbuf));
    xbuf.AudioBytes = copy_samples * XA2_CHANNELS * sizeof(int16_t);
    xbuf.pAudioData = (const BYTE *)g_xa2_bufs[idx];
    xbuf.pContext=(void *)((g_xa2_generation<<8)|(uintptr_t)(idx+1));
    AcquireSRWLockExclusive(&g_xa2_queue_lock);
    g_xa2_queued[idx]=(Xa2Queued){g_xa2_submitted_samples,(uint32_t)copy_samples,1};
    ReleaseSRWLockExclusive(&g_xa2_queue_lock);

    hr = IXAudio2SourceVoice_SubmitSourceBuffer(g_xa2_source, &xbuf, NULL);
    if (FAILED(hr)) {
        AcquireSRWLockExclusive(&g_xa2_queue_lock);g_xa2_queued[idx].pending=0;ReleaseSRWLockExclusive(&g_xa2_queue_lock);
        fprintf(stderr,"[XA2] SubmitSourceBuffer failed: 0x%08lX\n",hr);
        return 0;
    }

    g_xa2_next_buf = (idx + 1) % XA2_NUM_BUFS;
    g_xa2_frames_written++;
    g_xa2_submitted_samples+=(uint32_t)copy_samples;
    if (g_dbg.trace_enabled && g_xa2_frames_written<=4) {
        IXAudio2SourceVoice_GetState(g_xa2_source,&state,0);
        fprintf(stderr,"  [XA2] submitted=%d frames queued=%u actual_played=%llu first=%04X,%04X\n",
                copy_samples,state.BuffersQueued,(unsigned long long)state.SamplesPlayed,
                (uint16_t)samples[0],(uint16_t)samples[1]);
    }
    return 1;
}

int xa2_queued_bytes(void)
{
    if(!g_xa2_initialized || !g_xa2_source) { fprintf(stderr,"[STOP] audio queue is not connected\n");exit(5); }
    XAUDIO2_VOICE_STATE state;IXAudio2SourceVoice_GetState(g_xa2_source,&state,0);
    uint64_t played=state.SamplesPlayed+g_xa2_played_bias;
    if(played>g_xa2_submitted_samples) { fprintf(stderr,"[STOP] audio sample cursor exceeds submitted PCM\n");exit(5); }
    int bytes=(int)((g_xa2_submitted_samples-played)*XA2_CHANNELS*sizeof(int16_t));
    static int last=-1;
    if(g_dbg.queue_trace_enabled && g_xa2_frames_written/200!=last) {
        last=g_xa2_frames_written/200;
        XAUDIO2_PERFORMANCE_DATA performance;IXAudio2_GetPerformanceData(g_xa2,&performance);
        fprintf(stderr,"[AUDIO QUEUE] blocks=%d bytes=%d buffers=%u played=%llu glitches=%u latency=%u\n",g_xa2_frames_written,bytes,state.BuffersQueued,(unsigned long long)played,performance.GlitchesSinceEngineStarted,performance.CurrentLatencyInSamples);
    }
    return bytes;
}

int xa2_get_buffer_size(void)
{
    return XA2_BUF_SAMPLES;
}

void xa2_checkpoint_pause(void) {
    if (g_xa2_initialized && !g_xa2_checkpoint_paused) { IXAudio2_StopEngine(g_xa2);g_xa2_checkpoint_paused=1; }
}
void xa2_checkpoint_resume(void) {
    if (g_xa2_checkpoint_paused) {
        HRESULT result=IXAudio2_StartEngine(g_xa2);
        if (FAILED(result)) { fprintf(stderr,"[STOP] XAudio2 resume failed: %08lX\n",result);exit(5); }
        g_xa2_checkpoint_paused=0;
    }
}
typedef struct Xa2Checkpoint {
    uint32_t active,next,frames,count;
    uint64_t submitted,played;
    Xa2Queued queued[XA2_NUM_BUFS];
    int16_t buffers[XA2_NUM_BUFS][XA2_BUF_SAMPLES][XA2_CHANNELS];
} Xa2Checkpoint;
int xa2_save_checkpoint(FILE *out) {
    if (!out || !xbox_CheckpointFrozen() || (g_xa2_initialized && !g_xa2_checkpoint_paused)) return 0;
    Xa2Checkpoint saved={0};saved.active=g_xa2_initialized;
    if (saved.active) {
        XAUDIO2_VOICE_STATE state;IXAudio2SourceVoice_GetState(g_xa2_source,&state,0);
        saved.played=state.SamplesPlayed+g_xa2_played_bias;saved.submitted=g_xa2_submitted_samples;
        saved.next=g_xa2_next_buf;saved.frames=g_xa2_frames_written;
        memcpy(saved.queued,g_xa2_queued,sizeof saved.queued);memcpy(saved.buffers,g_xa2_bufs,sizeof saved.buffers);
        for (unsigned i=0;i<XA2_NUM_BUFS;i++) saved.count+=saved.queued[i].pending!=0;
        if (saved.count!=state.BuffersQueued) return 0;
    }
    return fwrite("XBDACQ",6,1,out)==1 && fwrite(&saved,sizeof saved,1,out)==1;
}
void *xa2_prepare_checkpoint(FILE *in) {
    if (!in || !xbox_CheckpointFrozen() || (g_xa2_initialized && !g_xa2_checkpoint_paused)) return NULL;
    char magic[6];Xa2Checkpoint *saved=malloc(sizeof *saved);if (!saved) return NULL;
    int ok=fread(magic,6,1,in)==1 && !memcmp(magic,"XBDACQ",6) && fread(saved,sizeof *saved,1,in)==1 &&
           saved->active==(uint32_t)g_xa2_initialized && saved->next<XA2_NUM_BUFS && saved->count<=XA2_NUM_BUFS &&
           saved->played<=saved->submitted;
    unsigned count=0;
    for (unsigned i=0;ok && i<XA2_NUM_BUFS;i++) {
        Xa2Queued *queued=&saved->queued[i];ok=queued->pending<=1;
        if (queued->pending) { count++;ok=ok && queued->samples && queued->samples<=XA2_BUF_SAMPLES &&
            queued->start<=saved->submitted && queued->samples<=saved->submitted-queued->start &&
            saved->played<queued->start+queued->samples;
            for (unsigned j=0;ok && j<i;j++) if(saved->queued[j].pending) ok=queued->start!=saved->queued[j].start;
        }
    }
    ok=ok && count==saved->count;
    if (!ok) { free(saved);return NULL; }return saved;
}
int xa2_apply_checkpoint(const void *prepared) {
    const Xa2Checkpoint *saved=prepared;if (!saved->active) return 1;
    IXAudio2SourceVoice *next=NULL;HRESULT result=xa2_create_source(&next);
    if (FAILED(result)) { if(next)next->lpVtbl->DestroyVoice(next);return 0; }
    unsigned used=0;uintptr_t generation=(g_xa2_generation+1)&(UINTPTR_MAX>>8);
    for (unsigned n=0;n<saved->count;n++) {
        unsigned index=XA2_NUM_BUFS;
        for (unsigned i=0;i<XA2_NUM_BUFS;i++) if(saved->queued[i].pending && !(used&(1u<<i)) &&
            (index==XA2_NUM_BUFS || saved->queued[i].start<saved->queued[index].start)) index=i;
        used|=1u<<index;
        uint32_t begin=saved->played>saved->queued[index].start?(uint32_t)(saved->played-saved->queued[index].start):0;
        XAUDIO2_BUFFER buffer={0};buffer.AudioBytes=saved->queued[index].samples*XA2_CHANNELS*sizeof(int16_t);
        buffer.pAudioData=(const BYTE *)g_xa2_bufs[index];buffer.PlayBegin=begin;
        buffer.PlayLength=saved->queued[index].samples-begin;
        buffer.pContext=(void *)((generation<<8)|(uintptr_t)(index+1));
        result=IXAudio2SourceVoice_SubmitSourceBuffer(next,&buffer,NULL);
        if (FAILED(result)) { next->lpVtbl->DestroyVoice(next);return 0; }
    }
    /* DestroyVoice finishes old callbacks before the ring is repopulated. */
    g_xa2_source->lpVtbl->DestroyVoice(g_xa2_source);g_xa2_source=next;
    g_xa2_generation=generation;
    memcpy(g_xa2_bufs,saved->buffers,sizeof g_xa2_bufs);memcpy(g_xa2_queued,saved->queued,sizeof g_xa2_queued);
    g_xa2_next_buf=(int)saved->next;g_xa2_frames_written=(int)saved->frames;
    g_xa2_submitted_samples=saved->submitted;g_xa2_played_bias=saved->played;
    InterlockedExchange(&g_xa2_voice_error,S_OK);ResetEvent(g_xa2_buffer_end);return 1;
}

#else /* !_WIN32 -- POSIX stubs (no audio output yet) */

int  xa2_init(void)                                   { return 0; }
void xa2_shutdown(void)                               {}
int  xa2_is_active(void)                              { return 0; }
int xa2_queued_bytes(void) { fprintf(stderr,"[STOP] XAudio2 queue unavailable on this platform\n");exit(5); }
int  xa2_submit_samples(const int16_t *s, int n)      { (void)s; (void)n; return 0; }
int  xa2_get_buffer_size(void)                        { return 0; }
void xa2_checkpoint_pause(void) {}
void xa2_checkpoint_resume(void) {}
int xa2_save_checkpoint(FILE *out) { (void)out;return 0; }
void *xa2_prepare_checkpoint(FILE *in) { (void)in;return NULL; }
int xa2_apply_checkpoint(const void *state) { (void)state;return 0; }

#endif /* _WIN32 */
