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
#include "apu/apu_vp.c"
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
int main(int argc,char **argv) {
    assert(argc==4);FILE *file=fopen(argv[2],"rb");assert(file);fseek(file,0,SEEK_END);long bytes=ftell(file);rewind(file);
    void *xbe=malloc(bytes);assert(xbe && fread(xbe,bytes,1,file)==1);fclose(file);
    assert(xbox_MemoryLayoutInit(xbe,bytes));free(xbe);_putenv_s("RECOMP_GUEST_LOCK","1");assert(guest_cpu_enabled());
    MCPXAPUState *d=mcpx_apu_init_standalone((void *)xbox_GetMemoryOffset(),physical);assert(d);
    extern MCPXAPUState *g_apu_state;g_apu_state=d;mcpx_ac97_init();assert(xbox_CheckpointBootstrapBegin());
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
        'd3d11.lib','dxgi.lib','dxguid.lib','d3dcompiler.lib','winmm.lib','xaudio2_8.lib',
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
