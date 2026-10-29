"""Whole original APU state and stopped XAudio2 PCM queue resume together."""
from pathlib import Path
import shutil
import subprocess

import pytest


@pytest.fixture(scope='module')
def native_apu(tmp_path_factory):
    root=Path(__file__).resolve().parents[2];work=tmp_path_factory.mktemp('checkpoint_apu')
    compiler=shutil.which('cl');assert compiler, 'requires existing MSVC environment'
    source=r'''
#include "kernel/kernel_bridge.c"
#include "apu/apu_core.c"
#include "apu/apu_ac97.c"
#include "apu/apu_xaudio2.c"
static unsigned long attenuation_test_pow_calls;
static float attenuation_test_pow(float x,float y) {++attenuation_test_pow_calls;return powf(x,y);}
#define powf attenuation_test_pow
#include "apu/apu_vp.c"
#undef powf
#include <assert.h>
recomp_func_t recomp_lookup(uint32_t va) { (void)va;return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va;return NULL; }
static void *physical(uint32_t address,uint32_t bytes) {
    return address<0x04000000 && bytes<=0x04000000-address?(void *)(address+xbox_GetMemoryOffset()):NULL;
}
static void seed(MCPXAPUState *d) {
    d->regs[0x100]=0x12345678;d->gp.regs[0x100]=0xabcdef12;d->ep.regs[0x100]=0x98765432;
    d->ep_frame_div=321;d->monitor.ac97_frame_div=456;d->vp.ssl[3].base[0]=0x12000;d->vp.ssl[3].count[0]=4;d->vp.ssl[3].ssl_seg=2;
    d->vp.voice_locked[2]=0xabcdef1234567890ULL;d->vp.sample_buf[4][1]=0.375f;
    d->vp.hrtf.entries[7].hrir[1][4]=0.625f;d->vp.filters[3].hrtf.ch[0].buf[5]=0.25f;
    setup_svf(&d->vp.filters[3].svf[0],0.125f,1.0f,F_LP);d->vp.filters[3].svf[0].l=0.75f;
    d->monitor.frame_buf[7][1]=1234;d->monitor.spdif_buf[7][0]=-2345;
    dsp_write_memory(d->gp.dsp,'X',0xc00,0x112233);dsp_write_memory(d->ep.dsp,'Y',0x10,0x445566);
    s_ac97.bm_regs[1].bdbar=0x20000;s_ac97.bm_regs[1].bd_valid=1;s_ac97.bm_regs[1].bd_addr=0x30000;
    s_ac97.bm_regs[1].picb=16;s_ac97.analog_last_sample[0]=77;s_ac97.analog_bup_last=true;
    int16_t pcm[256][2];for(unsigned i=0;i<256;i++){pcm[i][0]=1000+i;pcm[i][1]=-1000-(int)i;}
    assert(xa2_submit_samples((const int16_t *)pcm,256));
    for(unsigned i=0;i<256;i++){pcm[i][0]=2000+i;pcm[i][1]=-2000-(int)i;}
    assert(xa2_submit_samples((const int16_t *)pcm,256));
}
static void verify(MCPXAPUState *d) {
    assert(d->regs[0x100]==0x12345678 && d->gp.regs[0x100]==0xabcdef12 && d->ep.regs[0x100]==0x98765432);
    assert(d->ep_frame_div==321 && d->vp.ssl[3].base[0]==0x12000 && d->vp.ssl[3].count[0]==4 && d->vp.ssl[3].ssl_seg==2);
    assert(d->monitor.ac97_frame_div==456);
    assert(d->vp.voice_locked[2]==0xabcdef1234567890ULL && d->vp.sample_buf[4][1]==0.375f);
    assert(d->vp.hrtf.entries[7].hrir[1][4]==0.625f && d->vp.filters[3].hrtf.ch[0].buf[5]==0.25f);
    assert(d->vp.filters[3].svf[0].op==&d->vp.filters[3].svf[0].l && *d->vp.filters[3].svf[0].op==0.75f);
    assert(d->monitor.frame_buf[7][1]==1234 && d->monitor.spdif_buf[7][0]==-2345);
    assert(dsp_read_memory(d->gp.dsp,'X',0x1400)==0x112233 && dsp_read_memory(d->ep.dsp,'Y',0x10)==0x445566);
    assert(s_ac97.bm_regs[1].bd_valid==1 && s_ac97.bm_regs[1].bd_addr==0x30000 && s_ac97.bm_regs[1].picb==16);
    assert(s_ac97.analog_last_sample[0]==77 && s_ac97.analog_bup_last);
    XAUDIO2_VOICE_STATE voice;IXAudio2SourceVoice_GetState(g_xa2_source,&voice,0);
    assert(voice.BuffersQueued==2 && voice.SamplesPlayed+g_xa2_played_bias==0);
    assert(g_xa2_bufs[0][100][0]==1100 && g_xa2_bufs[1][100][1]==-2100);
}
static void verify_partial_cursor(void) {
    assert(xbox_CheckpointBootstrapBegin());int16_t pcm[1024][2];memset(pcm,0,sizeof pcm);
    for(unsigned i=0;i<3;i++) assert(xa2_submit_samples((const int16_t *)pcm,1024));
    XAUDIO2_VOICE_STATE voice;IXAudio2SourceVoice_GetState(g_xa2_source,&voice,0);
    uint64_t before=voice.SamplesPlayed+g_xa2_played_bias;
    xbox_CheckpointEnd();ULONGLONG deadline=GetTickCount64()+3000;
    do { IXAudio2SourceVoice_GetState(g_xa2_source,&voice,0);assert(GetTickCount64()<deadline);Sleep(1); }
    while(voice.SamplesPlayed+g_xa2_played_bias==before);
    assert(xbox_CheckpointBootstrapBegin());IXAudio2SourceVoice_GetState(g_xa2_source,&voice,0);
    assert(voice.BuffersQueued && voice.SamplesPlayed+g_xa2_played_bias<g_xa2_submitted_samples);
    uint64_t played=voice.SamplesPlayed+g_xa2_played_bias,total=g_xa2_submitted_samples;
    FILE *file=tmpfile();assert(file && xa2_save_checkpoint(file));rewind(file);
    void *prepared=xa2_prepare_checkpoint(file);fclose(file);assert(prepared);
    xbox_CheckpointEnd();Sleep(100);assert(xbox_CheckpointBootstrapBegin());
    assert(xa2_apply_checkpoint(prepared));free(prepared);
    IXAudio2SourceVoice_GetState(g_xa2_source,&voice,0);assert(voice.SamplesPlayed+g_xa2_played_bias==played);
    xbox_CheckpointEnd();Sleep(100);IXAudio2SourceVoice_GetState(g_xa2_source,&voice,0);
    assert(!voice.BuffersQueued && voice.SamplesPlayed+g_xa2_played_bias==total);
}
static uint32_t *seed_tone(MCPXAPUState *d) {
    uint8_t *ram=d->ram_ptr;d->regs[NV_PAPU_VPVADDR]=0x6000;d->regs[NV_PAPU_VPSGEADDR]=0x3000;
    ((uint32_t *)(ram+0x3000))[0]=0x20000;
    ((uint32_t *)(ram+0x3000))[2]=0x21000;
    ((uint32_t *)(ram+0x3000))[4]=0x22000;
    for(unsigned i=0;i<4608;i++)((int16_t *)(ram+0x20000))[i]=(int16_t)lrint(12000*sin(i*2*3.141592653589793/24));
    uint32_t *voice=(uint32_t *)(ram+0x6000+64*0x80);memset(voice,0,0x80);
    voice[NV_PAVS_VOICE_CFG_FMT/4]=(1u<<28)|(1u<<30)|NV_PAVS_VOICE_CFG_FMT_LOOP;
    voice[NV_PAVS_VOICE_PAR_STATE/4]=NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE;
    voice[NV_PAVS_VOICE_PAR_NEXT/4]=4607;
    return voice;
}
static void resampler_case(MCPXAPUState *d,const char *mode,const char *path) {
    uint32_t *voice=seed_tone(d);float pcm[32][2];FILE *file;
    if(!strcmp(mode,"src-tone2") || !strcmp(mode,"src-tonehalf")) {
        double ratio=!strcmp(mode,"src-tone2")?2.0:0.5;
        for(unsigned i=0;i<100;i++)assert(voice_resample(d,64,pcm,32,(float)ratio)==32);
        file=fopen(path,"wb");assert(file);
        for(unsigned i=0;i<1500;i++){assert(voice_resample(d,64,pcm,32,(float)ratio)==32);assert(fwrite(pcm,sizeof pcm,1,file)==1);}
        fclose(file);return;
    }
    if(!strcmp(mode,"src-save")) {
        for(unsigned i=0;i<97;i++)assert(voice_resample(d,64,pcm,32,1.37f)==32);
        file=fopen(path,"wb");assert(file && fwrite(voice,128,1,file)==1 && mcpx_apu_save_checkpoint(d,file));fclose(file);
    } else {
        file=fopen(path,"rb");uint32_t restored_voice[32];assert(file && fread(restored_voice,128,1,file)==1);
        if(!strcmp(mode,"src-reject")){assert(!mcpx_apu_load_checkpoint(d,file));fclose(file);return;}
        assert(mcpx_apu_load_checkpoint(d,file));fclose(file);memcpy(voice,restored_voice,128);
    }
    char output[1024];snprintf(output,sizeof output,"%s.%s.f32",path,mode);file=fopen(output,"wb");assert(file);
    for(unsigned i=0;i<128;i++) {
        float ratio=i<32?0.63f:i<64?1.75f:i<96?2.0f:1.37f;
        assert(voice_resample(d,64,pcm,32,ratio)==32 && fwrite(pcm,sizeof pcm,1,file)==1);
    }
    fclose(file);
}
static int attenuation_case(void) {
    mcpx_apu_vp_init(NULL);
    unsigned long initialized_calls=attenuation_test_pow_calls;
    for(unsigned v=0;v<=UINT16_MAX;v++) {
        uint16_t masked=(uint16_t)(v&0xfff);
        float expected=masked==0xfff?0.0f:powf(10.0f,masked/(64.0f*-20.0f));
        float actual=attenuate((uint16_t)v);
        assert(!memcmp(&actual,&expected,sizeof actual));
    }
    LARGE_INTEGER frequency,begin,end;QueryPerformanceFrequency(&frequency);
    float total=0;const unsigned iterations=8000000;
    QueryPerformanceCounter(&begin);
    for(unsigned i=0;i<iterations;i++)total+=attenuate((uint16_t)i);
    QueryPerformanceCounter(&end);
    unsigned long hot_calls=attenuation_test_pow_calls-initialized_calls;
    printf("{\"values_checked\":65536,\"iterations\":%u,\"hot_pow_calls\":%lu,\"nanoseconds_per_call\":%.6f,\"checksum\":%.9g}\n",iterations,hot_calls,1e9*(end.QuadPart-begin.QuadPart)/frequency.QuadPart/iterations,total);
    fflush(stdout);
    if(hot_calls){fprintf(stderr,"[ATTENUATION] repeated hot-path powf calls: %lu\n",hot_calls);return 5;}
    return 0;
}

static void sge_voice_boundaries(MCPXAPUState *d) {
    uint8_t *ram=d->ram_ptr;
    for(unsigned adpcm=0;adpcm<2;adpcm++)for(unsigned channels=1;channels<=2;channels++)
    for(unsigned unaligned=0;unaligned<2;unaligned++)for(unsigned width=0;width<(adpcm?1:4);width++) {
        uint32_t *voice=seed_tone(d);
        d->regs[NV_PAPU_VPSSLADDR]=0x4000;d->regs[NV_PAPU_FENADDR]=0x50000;
        d->regs[NV_PAPU_ISTS]=0;d->set_irq=false;
        memset(ram+0x50000,0,0x10000);memset(&d->vp.ssl[64],0,sizeof d->vp.ssl[64]);
        unsigned cs=adpcm?NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_ADPCM:width==0?0:width==1?1:3;
        voice_set_mask(d,64,NV_PAVS_VOICE_CFG_FMT,NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE,cs);
        voice_set_mask(d,64,NV_PAVS_VOICE_CFG_FMT,NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE,width);
        voice_set_mask(d,64,NV_PAVS_VOICE_CFG_FMT,NV_PAVS_VOICE_CFG_FMT_SAMPLES_PER_BLOCK,channels-1);
        voice_set_mask(d,64,NV_PAVS_VOICE_CFG_FMT,NV_PAVS_VOICE_CFG_FMT_STEREO,channels==2);
        voice_set_mask(d,64,NV_PAVS_VOICE_CFG_FMT,NV_PAVS_VOICE_CFG_FMT_DATA_TYPE,0);
        voice_set_mask(d,64,NV_PAVS_VOICE_CFG_FMT,NV_PAVS_VOICE_CFG_FMT_PERSIST,1);
        voice_set_mask(d,64,NV_PAVS_VOICE_CFG_FMT,NV_PAVS_VOICE_CFG_FMT_LOOP,1);
        voice_set_mask(d,64,NV_PAVS_VOICE_CUR_PSL_START,NV_PAVS_VOICE_CUR_PSL_START_BA,4090);
        voice_set_mask(d,64,NV_PAVS_VOICE_PAR_NEXT,NV_PAVS_VOICE_PAR_NEXT_EBO,adpcm?191:31);
        for(unsigned i=0;i<0x3000;i++){ram[0x20000+i]=(uint8_t)(i*73+19);ram[0x28000+i]=(uint8_t)(i*31+47);}
        float observed[3][32][2];memset(observed,0x5a,sizeof observed);
        for(unsigned call=0;call<3;call++) {
            ((uint32_t*)(ram+0x3000))[0]=(call==1?0x28000:0x20000)+(unaligned?1:0);
            ((uint32_t*)(ram+0x3000))[2]=(call==1?0x29000:0x21000)+(unaligned?3:0);
            voice_set_mask(d,64,NV_PAVS_VOICE_PAR_OFFSET,NV_PAVS_VOICE_PAR_OFFSET_CBO,0);
            uint32_t start_voice[32];memcpy(start_voice,voice,sizeof start_voice);
            assert(voice_get_samples(d,64,observed[call],32)==32);
            uint32_t end_voice[32];memcpy(end_voice,voice,sizeof end_voice);
            float individual[32][2];memset(individual,0x5a,sizeof individual);
            memcpy(voice,start_voice,sizeof start_voice);
            for(unsigned sample=0;sample<32;sample++) {
                voice_set_mask(d,64,NV_PAVS_VOICE_PAR_OFFSET,NV_PAVS_VOICE_PAR_OFFSET_CBO,sample);
                assert(voice_get_samples(d,64,&individual[sample],1)==1);
            }
            assert(!memcmp(observed[call],individual,sizeof individual));
            assert(!memcmp(voice,end_voice,sizeof end_voice));
        }
        assert(memcmp(observed[0],observed[1],sizeof observed[0]));
        assert(!memcmp(observed[0],observed[2],sizeof observed[0]));
    }
    puts("SGE voice boundaries: 20 cases passed");
}

/* Exercise the original methods through the original decoder and adapters. */
struct MCPXAPUState;
extern struct MCPXAPUState *g_apu_state;
uint64_t apu_read_adapter(void *device,uint32_t offset,unsigned size);
void apu_write_adapter(void *device,uint32_t offset,uint64_t value,unsigned size);

typedef struct DirectApuOp { uint32_t offset,value; unsigned write; } DirectApuOp;
static void direct_apu_ops(DirectApuOp op[16],unsigned voice,uint32_t pattern,
                           uint32_t bin,uint32_t format) {
    DirectApuOp authored[16]={
        {0x20010,0,0},{0x202f8,voice,1},{0x202fc,1,1},
        {0x20300,bin,1},{0x20304,format,1},
        {0x20360,pattern,1},{0x20364,~pattern,1},{0x20368,pattern^0x5a5a5a5a,1},
        {0x202fc,0,1},
        {0x20010,0,0},{0x202f8,voice,1},{0x202fc,1,1},
        {0x20360,~pattern,1},{0x20364,pattern^0xa5a5a5a5,1},{0x20368,pattern,1},
        {0x202fc,0,1}
    };
    memcpy(op,authored,sizeof authored);
}
static uint32_t direct_apu_step(MCPXAPUState *d,const DirectApuOp *op,int direct) {
    if(direct) {
        if(op->write) {apu_write_adapter(d,op->offset,op->value,4);return op->value;}
        return (uint32_t)apu_read_adapter(d,op->offset,4);
    }
    const unsigned char load[]={0x8b,0x10},store[]={0x89,0x10};
    CONTEXT context={0};const unsigned char *code=op->write?store:load;
    context.Rip=(DWORD64)code;context.Rax=0xfe800000u+op->offset;
    context.Rdx=0xfedcba9800000000ull|op->value;context.EFlags=0xad7;
    assert(apu_hook_handle_mmio(&context,0,0xfe800000u+op->offset,op->write));
    assert(context.Rip==(DWORD64)code+2 && context.EFlags==0xad7);
    if(op->write)assert(context.Rdx==(0xfedcba9800000000ull|op->value));
    else assert(context.Rdx<=UINT32_MAX);
    return (uint32_t)context.Rdx;
}
static void direct_apu_save(MCPXAPUState *d,FILE *file,uint32_t result) {
    assert(mcpx_apu_save_checkpoint(d,file));
    assert(fwrite(mcpx_apu_phys(d->regs[NV_PAPU_VPVADDR],256*128),256*128,1,file)==1);
    XboxGuestRegisters guest;xbox_CaptureGuestRegisters(&guest);
    assert(fwrite(&guest,sizeof guest,1,file)==1);
    assert(fwrite(&result,sizeof result,1,file)==1);
}
static void direct_apu_equal_files(FILE *a,FILE *b) {
    assert(fflush(a)==0 && fflush(b)==0);long bytes=ftell(a);assert(bytes>=0 && ftell(b)==bytes);
    rewind(a);rewind(b);unsigned char left[16384],right[16384];
    while(bytes) {size_t count=bytes>sizeof left?sizeof left:(size_t)bytes;
        assert(fread(left,count,1,a)==1 && fread(right,count,1,b)==1);
        assert(!memcmp(left,right,count));bytes-=(long)count;
    }
}
static void direct_apu_restore(MCPXAPUState *d,FILE *saved,const uint8_t *ram,
                               const XboxGuestRegisters *guest) {
    memcpy(d->ram_ptr,ram,0x4000000);rewind(saved);
    assert(mcpx_apu_load_checkpoint(d,saved));assert(xbox_RestoreGuestRegisters(guest));
}
static void direct_apu_sequence(MCPXAPUState *d,const DirectApuOp op[16],int direct,FILE *observed) {
    for(unsigned i=0;i<16;i++) {
        XboxGuestRegisters before,after;xbox_CaptureGuestRegisters(&before);
        uint32_t value=direct_apu_step(d,&op[i],direct);
        xbox_CaptureGuestRegisters(&after);assert(!memcmp(&before,&after,sizeof before));
        if(observed)direct_apu_save(d,observed,value);
    }
}
static void direct_apu_compare(MCPXAPUState *d) {
    g_dbg.trace_enabled=false;
    mcpx_apu_mmio_write(d,NV_PAPU_VPVADDR,0x2000000,4);
    uint8_t *start=malloc(0x4000000),*reference=malloc(0x4000000);assert(start&&reference);
    for(unsigned i=0;i<256*128;i++)d->ram_ptr[0x2000000+i]=(uint8_t)(i*73+19);
    memcpy(start,d->ram_ptr,0x4000000);
    XboxGuestRegisters guest;xbox_CaptureGuestRegisters(&guest);
    guest.eax=0x12345678;guest.ecx=0x89abcdef;guest.edx=0x76543210;
    guest.ebx=0x13579bdf;guest.esi=0x2468ace0;guest.edi=0xfedcba98;
    guest.eflags=0x8d7;guest.fp_control_word=0x37f;guest.fp_status_word=0x45;
    guest.mxcsr=0x3f80;for(unsigned i=0;i<sizeof guest.xmm;i++)((uint8_t *)guest.xmm)[i]=(uint8_t)(i*31+7);
    assert(xbox_RestoreGuestRegisters(&guest));xbox_CaptureGuestRegisters(&guest);
    FILE *saved=tmpfile();assert(saved&&mcpx_apu_save_checkpoint(d,saved));
    const unsigned voices[]={0,31,32,63,64,127,128,255};
    const uint32_t patterns[]={0,0xffffffff,0x80000000,0x5aa5c33c};
    LARGE_INTEGER frozen,ending;xbox_RuntimePerformanceCounter(&frozen);
    unsigned cases=0;
    for(unsigned v=0;v<8;v++)for(unsigned p=0;p<4;p++) {
        DirectApuOp op[16];direct_apu_ops(op,voices[v],patterns[p],patterns[p]^0x13579bdf,~patterns[p]);
        FILE *a=tmpfile(),*b=tmpfile();assert(a&&b);
        direct_apu_restore(d,saved,start,&guest);direct_apu_sequence(d,op,0,a);memcpy(reference,d->ram_ptr,0x4000000);
        direct_apu_restore(d,saved,start,&guest);direct_apu_sequence(d,op,1,b);
        direct_apu_equal_files(a,b);assert(!memcmp(reference,d->ram_ptr,0x4000000));
        fclose(a);fclose(b);cases++;
    }
    fclose(saved);
    /* Original valid tone and full voice consumer, after the same two blocks. */
    uint32_t *voice=seed_tone(d);DirectApuOp tone[16];
    direct_apu_ops(tone,64,0,voice[NV_PAVS_VOICE_CFG_VBIN/4],voice[NV_PAVS_VOICE_CFG_FMT/4]);
    /* Zero attenuation in all original target volume fields. */
    for(unsigned i=0;i<16;i++)if(tone[i].offset>=0x20360 && tone[i].offset<=0x20368)tone[i].value=0;
    memcpy(start,d->ram_ptr,0x4000000);xbox_CaptureGuestRegisters(&guest);
    saved=tmpfile();assert(saved&&mcpx_apu_save_checkpoint(d,saved));
    FILE *a=tmpfile(),*b=tmpfile();assert(a&&b);
    unsigned pcm_nonzero[2]={0};
    for(int direct=0;direct<2;direct++) {
        FILE *out=direct?b:a;direct_apu_restore(d,saved,start,&guest);direct_apu_sequence(d,tone,direct,out);
        for(unsigned frame=0;frame<32;frame++) {
            float bins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME]={0},monitor[NUM_SAMPLES_PER_FRAME][2]={0};
            voice_process(d,bins,monitor,64,0);
            for(unsigned bin=0;bin<NUM_MIXBINS;bin++)for(unsigned sample=0;sample<NUM_SAMPLES_PER_FRAME;sample++) {
                assert(isfinite(bins[bin][sample]));if(bins[bin][sample]!=0)pcm_nonzero[direct]++;
            }
            for(unsigned sample=0;sample<NUM_SAMPLES_PER_FRAME;sample++)for(unsigned ch=0;ch<2;ch++) {
                assert(isfinite(monitor[sample][ch]));if(monitor[sample][ch]!=0)pcm_nonzero[direct]++;
            }
            assert(fwrite(bins,sizeof bins,1,out)==1&&fwrite(monitor,sizeof monitor,1,out)==1);
        }
        direct_apu_save(d,out,0);if(!direct)memcpy(reference,d->ram_ptr,0x4000000);
    }
    assert(pcm_nonzero[0] && pcm_nonzero[0]==pcm_nonzero[1]);
    direct_apu_equal_files(a,b);assert(!memcmp(reference,d->ram_ptr,0x4000000));fclose(a);fclose(b);
    xbox_RuntimePerformanceCounter(&ending);assert(frozen.QuadPart==ending.QuadPart);
    /* One fixed ABBA trial of the original complete sixteen-method block. */
    double seconds[4];const int order[]={0,1,1,0};LARGE_INTEGER frequency,t0,t1;QueryPerformanceFrequency(&frequency);
    for(unsigned pass=0;pass<4;pass++) {
        direct_apu_restore(d,saved,start,&guest);
        for(unsigned warm=0;warm<64;warm++)for(unsigned i=0;i<16;i++)direct_apu_step(d,&tone[i],order[pass]);
        QueryPerformanceCounter(&t0);
        for(unsigned loop=0;loop<65536;loop++)for(unsigned i=0;i<16;i++)direct_apu_step(d,&tone[i],order[pass]);
        QueryPerformanceCounter(&t1);seconds[pass]=(double)(t1.QuadPart-t0.QuadPart)/frequency.QuadPart;
    }
    fclose(saved);free(start);free(reference);
    printf("DIRECT_APU_RESULT {\"cases\":%u,\"operations_per_route\":%u,\"whole_ram_bytes_per_case\":67108864,\"guest_state_bytes\":%zu,\"pcm_frames_per_route\":32,\"pcm_nonzero_values_per_route\":%u,\"clock_exclusions\":0,\"abba_method_seconds\":[%.9f,%.9f,%.9f,%.9f]}\n",cases,cases*16,sizeof guest,pcm_nonzero[0],seconds[0],seconds[1],seconds[2],seconds[3]);
}
static void direct_apu_trace(MCPXAPUState *d,int direct) {
    mcpx_apu_mmio_write(d,NV_PAPU_VPVADDR,0x2000000,4);g_dbg.trace_enabled=true;
    DirectApuOp op[16];direct_apu_ops(op,64,0x5aa5c33c,0x12345678,0x87654321);
    for(unsigned cycle=0;cycle<320;cycle++)for(unsigned i=0;i<16;i++)direct_apu_step(d,&op[i],direct);
    g_dbg.trace_enabled=false;puts("DIRECT_APU_TRACE complete");
}

int main(int argc,char **argv) {
    if(argc==4 && !strcmp(argv[1],"attenuation"))return attenuation_case();
    assert(argc==4);FILE *file=fopen(argv[2],"rb");assert(file);fseek(file,0,SEEK_END);long bytes=ftell(file);rewind(file);
    void *xbe=malloc(bytes);assert(xbe && fread(xbe,bytes,1,file)==1);fclose(file);
    assert(xbox_MemoryLayoutInit(xbe,bytes));free(xbe);_putenv_s("RECOMP_GUEST_LOCK","1");assert(guest_cpu_enabled());
    MCPXAPUState *d=mcpx_apu_init_standalone((void *)xbox_GetMemoryOffset(),physical);assert(d);
    extern MCPXAPUState *g_apu_state;g_apu_state=d;mcpx_ac97_init();assert(xbox_CheckpointBootstrapBegin());
    if(!strcmp(argv[1],"sge-boundaries")){sge_voice_boundaries(d);fflush(NULL);ExitProcess(0);}
    if(!strcmp(argv[1],"direct-apu-compare")){direct_apu_compare(d);fflush(NULL);ExitProcess(0);}
    if(!strcmp(argv[1],"direct-apu-trace-decoder")){direct_apu_trace(d,0);fflush(NULL);ExitProcess(0);}
    if(!strcmp(argv[1],"direct-apu-trace-direct")){direct_apu_trace(d,1);fflush(NULL);ExitProcess(0);}
    if(!strncmp(argv[1],"src-",4)){resampler_case(d,argv[1],argv[3]);fflush(NULL);ExitProcess(0);}
    if(!strcmp(argv[1],"save")) {
        seed(d);file=fopen(argv[3],"wb");assert(file && mcpx_apu_save_checkpoint(d,file));fclose(file);verify(d);
        d->regs[0x100]=0xdeadbeef;dsp_write_memory(d->gp.dsp,'X',0xc00,0x778899);
        file=fopen(argv[3],"rb");assert(file && mcpx_apu_load_checkpoint(d,file));fclose(file);verify(d);
    } else if(!strcmp(argv[1],"reject")) {
        d->regs[0x100]=0xdeadbeef;dsp_write_memory(d->gp.dsp,'X',0xc00,0x778899);
        file=fopen(argv[3],"rb");assert(file && !mcpx_apu_load_checkpoint(d,file));fclose(file);
        assert(d->regs[0x100]==0xdeadbeef && dsp_read_memory(d->gp.dsp,'X',0xc00)==0x778899);
        return 0;
    } else {
        file=fopen(argv[3],"rb");assert(file && mcpx_apu_load_checkpoint(d,file));fclose(file);verify(d);
    }
    Sleep(20);verify(d);xbox_CheckpointEnd();Sleep(60);
    XAUDIO2_VOICE_STATE voice;IXAudio2SourceVoice_GetState(g_xa2_source,&voice,0);
    assert(!voice.BuffersQueued && voice.SamplesPlayed+g_xa2_played_bias==512);
    verify_partial_cursor();
    assert(xbox_CheckpointBootstrapBegin());
    qemu_mutex_lock(&d->lock);d->regs[NV_PAPU_SECTL]=0;d->ep_frame_div=3;d->monitor.ac97_frame_div=0;
    d->pause_requested=false;qemu_cond_signal(&d->cond);qemu_mutex_unlock(&d->lock);
    xbox_CheckpointEnd();Sleep(40);assert(xbox_CheckpointBootstrapBegin());
    assert(d->ep_frame_div==3 && d->monitor.ac97_frame_div>=16);
    d->pause_requested=true;xbox_CheckpointEnd();
    mcpx_apu_shutdown(d);xbox_MemoryLayoutShutdown();return 0;
}
'''
    (work/'apu.c').write_text(source,encoding='utf-8')
    libraries=list((root.parents[1]/'build/xboxrecomp/src').glob('*/*.lib'));assert len(libraries)==8
    libraries.append(root.parents[1]/'build_dsp_jit/x86_64-pc-windows-msvc/release/dsp56300_emu_ffi.lib')
    result=subprocess.run([compiler,'/nologo','/O2','/Gy','/MD','/utf-8',
        '/DWIN32_LEAN_AND_MEAN','/DNOMINMAX','/D_CRT_SECURE_NO_WARNINGS','/D_USE_MATH_DEFINES','/I'+str(root/'src'),
        str(work/'apu.c'),'/Fe:'+str(work/'apu.exe'),'/link','/OPT:REF',*[str(p) for p in libraries],
        'kernel32.lib','user32.lib','gdi32.lib','advapi32.lib','ole32.lib','shell32.lib',
        'd3d11.lib','d3d12.lib','dxgi.lib','dxguid.lib','d3dcompiler.lib','winmm.lib','xaudio2_8.lib',
        'xinput.lib','bcrypt.lib','ntdll.lib','userenv.lib','ws2_32.lib'],
        cwd=work,capture_output=True,text=True,errors='replace')
    assert result.returncode==0,result.stdout+result.stderr
    return work/'apu.exe',root/'tools/conformance/test.xbe'


def test_original_apu_voice_dsp_dma_and_pcm_queue_restore_in_fresh_process(native_apu,tmp_path):
    executable,xbe=native_apu;snapshot=tmp_path/'apu.state'
    def run(mode,path=snapshot):
        result=subprocess.run([str(executable),mode,str(xbe),str(path)],
                              capture_output=True,text=True,errors='replace',timeout=20)
        assert result.returncode==0,result.stdout+result.stderr
    run('save');run('load');bad=tmp_path/'bad.state';bad.write_bytes(snapshot.read_bytes()[:-1]);run('reject',bad)


def test_original_voice_rate_and_fractional_filter_history_survive_fresh_process(native_apu,tmp_path):
    import numpy as np
    executable,xbe=native_apu
    def run(mode,path):
        result=subprocess.run([str(executable),mode,str(xbe),str(path)],capture_output=True,text=True,errors='replace',timeout=20)
        assert result.returncode==0,result.stdout+result.stderr
    for mode,frequency in [('src-tone2',1000),('src-tonehalf',4000)]:
        output=tmp_path/(mode+'.f32');run(mode,output)
        signal=np.frombuffer(output.read_bytes(),dtype='<f4').reshape(-1,2)
        assert len(signal)==48000 and np.array_equal(signal[:,0],signal[:,1])
        spectrum=np.abs(np.fft.rfft(signal[:,0]));assert abs(int(np.argmax(spectrum))-frequency)<=1
    snapshot=tmp_path/'resampler.state';run('src-save',snapshot);run('src-load',snapshot)
    expected=Path(str(snapshot)+'.src-save.f32').read_bytes()
    assert Path(str(snapshot)+'.src-load.f32').read_bytes()==expected
    bad=tmp_path/'resampler-truncated.state';bad.write_bytes(snapshot.read_bytes()[:-1]);run('src-reject',bad)


def test_original_voice_attenuation_values_reuse_initialized_results(native_apu,tmp_path):
    import json
    executable,xbe=native_apu
    result=subprocess.run([str(executable),'attenuation',str(xbe),str(tmp_path/'unused')],capture_output=True,text=True,errors='replace',timeout=20)
    assert result.returncode==0,result.stdout+result.stderr
    (tmp_path/'attenuation_measurement.json').write_text(result.stdout,encoding='utf-8')
    measured=json.loads(result.stdout)
    assert measured['values_checked']==65536 and measured['hot_pow_calls']==0



def test_original_voice_sge_pages_unaligned_base_and_next_call_visibility(native_apu,tmp_path):
    executable,xbe=native_apu
    result=subprocess.run([str(executable),'sge-boundaries',str(xbe),str(tmp_path/'unused')],
                          capture_output=True,text=True,errors='replace',timeout=30)
    assert result.returncode==0,result.stdout+result.stderr
    assert 'SGE voice boundaries: 20 cases passed' in result.stdout


def test_original_static_apu_methods_match_decoder_state_voice_and_trace(native_apu,tmp_path):
    import json
    executable,xbe=native_apu
    completed={}
    for label,mode in [('semantic','direct-apu-compare'),('trace-decoder','direct-apu-trace-decoder'),('trace-direct','direct-apu-trace-direct')]:
        result=subprocess.run([str(executable),mode,str(xbe),str(tmp_path/'unused')],capture_output=True,text=True,errors='replace')
        (tmp_path/(label+'.stdout')).write_text(result.stdout,encoding='utf-8')
        (tmp_path/(label+'.stderr')).write_text(result.stderr,encoding='utf-8')
        assert result.returncode==0,result.stdout+result.stderr
        completed[label]=result
    line=next(line for line in completed['semantic'].stdout.splitlines() if line.startswith('DIRECT_APU_RESULT '))
    measured=json.loads(line.split(' ',1)[1])
    assert measured['cases']==32 and measured['operations_per_route']==512
    assert measured['whole_ram_bytes_per_case']==0x4000000 and measured['clock_exclusions']==0
    assert measured['pcm_frames_per_route']==32 and measured['pcm_nonzero_values_per_route']>0
    (tmp_path/'direct_method_measurement.json').write_text(json.dumps(measured,indent=2),encoding='utf-8')
    traces=[[line for line in completed[label].stderr.splitlines() if '[APUMMIO]' in line] for label in ['trace-decoder','trace-direct']]
    assert traces[0]==traces[1]
    assert sum(' read ' in line for line in traces[0])==400
    assert sum(' write ' in line for line in traces[0])==400
