/*
 * Copyright (C) 2006 InnoTek Systemberatung GmbH
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * in version 2 as it comes in the "COPYING" file of the VirtualBox OSE
 * distribution. VirtualBox OSE is distributed in the hope that it will
 * be useful, but WITHOUT ANY WARRANTY of any kind.
 *
 * If you received this file as part of a commercial VirtualBox
 * distribution, then only the terms of your commercial VirtualBox
 * license agreement apply instead of the previous paragraph.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

/* Standalone register and DMA consumer of the existing AC97 link.
 * Analog and S/PDIF retain independent descriptors and output payloads. */
#include "apu.h"
#include "ac97_regs.h"
#include "apu_state.h"
#include "apu_xaudio2.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "kernel/kernel.h"

typedef struct AC97BusMasterRegs {
    uint32_t bdbar;
    uint8_t civ, lvi;
    uint16_t sr, picb;
    uint8_t piv, cr;
    unsigned bd_valid;
    uint32_t bd_addr, bd_ctl_len;
} AC97BusMasterRegs;
typedef struct AC97State {
    uint32_t glob_cnt, glob_sta, cas;
    AC97BusMasterRegs bm_regs[8];
    uint16_t mixer_data[128];
    int attached;
    uint32_t spdif_status;
    int16_t analog_last_sample[2];
    bool analog_bup_last;
} AC97State;
static AC97State s_ac97;
int mcpx_ac97_save_checkpoint(FILE *out) {
    return xbox_CheckpointFrozen() && out && fwrite("XBAC97",6,1,out)==1 && fwrite(&s_ac97,sizeof s_ac97,1,out)==1;
}
void *mcpx_ac97_prepare_checkpoint(FILE *in) {
    if (!in || !xbox_CheckpointFrozen()) return NULL;
    char magic[6];AC97State *state=malloc(sizeof *state);if (!state) return NULL;
    int ok=fread(magic,6,1,in)==1 && !memcmp(magic,"XBAC97",6) && fread(state,sizeof *state,1,in)==1 &&
           state->attached==s_ac97.attached && ((unsigned char *)state)[offsetof(AC97State,analog_bup_last)]<=1;
    for (unsigned i=0;ok && i<8;i++) ok=state->bm_regs[i].bd_valid<=1;
    if (!ok) { free(state);return NULL; }return state;
}
void mcpx_ac97_apply_checkpoint(const void *state) { memcpy(&s_ac97,state,sizeof s_ac97); }

static void ac97_stop(const char *reason, uint32_t offset, uint64_t value, unsigned size)
{
    fprintf(stderr, "[STOP] AC97 %s: offset=0x%03X value=0x%08llX size=%u\n",
            reason, offset, (unsigned long long)value, size);
    fflush(stderr);
    exit(5);
}
static void reset_bm_regs(AC97BusMasterRegs *r)
{
    uint8_t control = r->cr & 0x1c;
    memset(r, 0, sizeof(*r));
    r->cr = control;
    r->sr = 1; /* DMA controller halted */
    if (r==&s_ac97.bm_regs[1]) {
        memset(s_ac97.analog_last_sample,0,sizeof(s_ac97.analog_last_sample));
        s_ac97.analog_bup_last=false;
    }
}
static void mixer_reset(void)
{
    memset(s_ac97.mixer_data, 0, sizeof(s_ac97.mixer_data));
#define STORE(reg,value) s_ac97.mixer_data[(reg)/2]=(value)
    STORE(AC97_Powerdown_Ctrl_Stat, 0x000f);
    STORE(AC97_Vendor_ID1, 0x8384);
    STORE(AC97_Vendor_ID2, 0x7600);
    STORE(AC97_Extended_Audio_ID, 0x0809);
    STORE(AC97_Extended_Audio_Ctrl_Stat, 0x0009);
    STORE(AC97_PCM_Front_DAC_Rate, 0xbb80);
    STORE(AC97_PCM_Surround_DAC_Rate, 0xbb80);
    STORE(AC97_PCM_LFE_DAC_Rate, 0xbb80);
    STORE(AC97_PCM_LR_ADC_Rate, 0xbb80);
    STORE(AC97_MIC_ADC_Rate, 0xbb80);
    STORE(AC97_Master_Volume_Mute, 0x8000);
    STORE(AC97_PCM_Out_Volume_Mute, 0x8808);
    STORE(AC97_Record_Gain_Mute, 0x8808);
#undef STORE
}
static void fetch_bd(AC97BusMasterRegs *r)
{
    uint64_t descriptor=(uint64_t)r->bdbar+r->civ*8;
    if (descriptor>0x04000000u-8) ac97_stop("descriptor outside physical RAM",0,descriptor,8);
    const uint32_t *bd=(const uint32_t *)mcpx_apu_phys(descriptor,8);
    r->bd_addr=bd[0]&~3u;
    r->bd_ctl_len=bd[1];
    r->picb=(uint16_t)bd[1];
    if (r->bd_addr>0x04000000u || (uint32_t)r->picb*2>0x04000000u-r->bd_addr)
        ac97_stop("PCM buffer outside physical RAM",0,r->bd_addr,(unsigned)r->picb*2);
    r->bd_valid=1;
}
static void update_sr(AC97BusMasterRegs *r, uint16_t status)
{
    r->sr=status;
    unsigned index=(unsigned)(r-s_ac97.bm_regs);
    static const uint32_t irq_mask[8]={0x20,0x40,0x80,0,0,0,0,0x10};
    int raised=((status&8)&&(r->cr&16)) || ((status&4)&&(r->cr&4)) || ((status&16)&&(r->cr&8));
    if (raised) s_ac97.glob_sta|=irq_mask[index];
    else s_ac97.glob_sta&=~irq_mask[index];
}
int mcpx_ac97_irq_pending(void)
{
    return !!(s_ac97.glob_sta&0xf0);
}
void mcpx_ac97_frame(void)
{
    for (unsigned index=1; index<=7; index+=6) {
        AC97BusMasterRegs *r=&s_ac97.bm_regs[index];
        unsigned elapsed=NUM_SAMPLES_PER_FRAME*4;
        bool holding=index==1 && (r->sr&1) && s_ac97.analog_bup_last;
        if (!(r->cr&1) || ((r->sr&1) && !holding)) continue;
        while (elapsed && (!(r->sr&1) || holding)) {
            if (!holding && !r->bd_valid) fetch_bd(r);
            if (!holding && !r->picb) {
                if (r->civ==r->lvi) {
                    if (index==1) s_ac97.analog_bup_last=false;
                    update_sr(r,r->sr|1);
                    break;
                }
                r->civ=r->piv;
                r->piv=(r->piv+1)%32;
                fetch_bd(r);
                continue;
            }
            unsigned bytes=holding ? elapsed : (unsigned)r->picb*2;
            if (bytes>elapsed) bytes=elapsed;
            if (bytes&3) ac97_stop("output is not a whole stereo frame",index*16+0x100,bytes,4);
            const int16_t *pcm=holding ? s_ac97.analog_last_sample
                                      : (const int16_t *)mcpx_apu_phys(r->bd_addr,bytes);
            extern MCPXAPUState *g_apu_state;
            unsigned off=(g_apu_state->monitor.ac97_frame_div%8)*NUM_SAMPLES_PER_FRAME;
            unsigned used=(NUM_SAMPLES_PER_FRAME*4-elapsed)/4;
            if (index==1) {
                if (g_apu_state->monitor.point==MCPX_APU_DEBUG_MON_AC97) {
                if (s_ac97.mixer_data[AC97_PCM_Front_DAC_Rate/2]!=48000)
                    ac97_stop("non-48kHz DAC is not connected",0x2c,s_ac97.mixer_data[AC97_PCM_Front_DAC_Rate/2],2);
                /* Accumulate the actual analog bus samples into the existing EP-sized output. */
                uint16_t master=s_ac97.mixer_data[AC97_Master_Volume_Mute/2];
                uint16_t volume=s_ac97.mixer_data[AC97_PCM_Out_Volume_Mute/2];
                for (unsigned frame=0;frame<bytes/4;++frame) for (unsigned ch=0;ch<2;++ch) {
                    unsigned shift=ch==0 ? 8 : 0;
                    unsigned gain=255-255*((master>>shift)&63)/63;
                    unsigned pgain=255-255*((volume>>shift)&31)/31;
                    gain=gain*pgain/255;
                    if ((master|volume)&0x8000) gain=0;
                    g_apu_state->monitor.frame_buf[off+used+frame][ch]=(int16_t)((int32_t)pcm[(holding ? 0 : frame)*2+ch]*(int32_t)gain/255);
                }
                }
            } else {
                /* Preserve the actual transmitted words independently of the
                 * analog codec volume. The selected PC output is fixed. */
                if (s_ac97.spdif_status!=0x02000000u)
                    ac97_stop("S/PDIF channel-status configuration is not verified",0x17c,s_ac97.spdif_status,4);
                memcpy(g_apu_state->monitor.spdif_buf[off+used],pcm,bytes);
                g_apu_state->monitor.spdif_channel_status=s_ac97.spdif_status;
                if (g_dbg.trace_enabled) {
                    static unsigned trace_count;
                    if (trace_count++<4) fprintf(stderr,"  [AC97DMA] S/PDIF addr=0x%08X bytes=%u first=%04X,%04X\n",r->bd_addr,bytes,(uint16_t)pcm[0],(uint16_t)pcm[1]);
                }
            }
            /* Original analog BUP holds the consumed sample, without DMA or IRQ progress. */
            if (holding) break;
            if (index==1) memcpy(s_ac97.analog_last_sample,pcm+bytes/2-2,4);
            r->bd_addr+=bytes;
            r->picb-=(uint16_t)(bytes/2);
            elapsed-=bytes;
            if (!r->picb) {
                uint16_t status=r->sr&~2;
                if (r->bd_ctl_len&0x80000000u) status|=8;
                if (r->civ==r->lvi) {
                    status|=1|2|4;
                    if (index==1) s_ac97.analog_bup_last=!!(r->bd_ctl_len&0x40000000u);
                }
                else { r->civ=r->piv; r->piv=(r->piv+1)%32; fetch_bd(r); }
                update_sr(r,status);
            }
        }
    }
}
void mcpx_ac97_init(void)
{
    memset(&s_ac97, 0, sizeof(s_ac97));
    for (unsigned i=0; i<8; ++i) reset_bm_regs(&s_ac97.bm_regs[i]);
    mixer_reset();
    s_ac97.attached = 1;
}
static AC97BusMasterRegs *channel(uint32_t offset, unsigned size)
{
    unsigned index=(offset-0x100)>>4;
    if (offset<0x100 || offset>=0x180 || (index!=0 && index!=1 && index!=2 && index!=7))
        ac97_stop("unsupported bus-master channel",offset,0,size);
    return &s_ac97.bm_regs[index];
}
static uint64_t ac97_read_locked(void *device, uint32_t offset, unsigned size)
{
    uint32_t value=0;
    if (!s_ac97.attached) ac97_stop("device absent",offset,0,size);
    if (offset<0x100) {
        if (size!=2 || (offset&1) || offset>0xfe) ac97_stop("unsupported codec access",offset,0,size);
        s_ac97.cas=0;
        value=s_ac97.mixer_data[offset/2];
    } else if (offset==0x12c && size==4) value=s_ac97.glob_cnt;
    else if (offset==0x130 && size==4) value=s_ac97.glob_sta|0x100;
    else if (offset==0x17c && size==4) value=s_ac97.spdif_status;
    else if (offset==0x134 && size==1) { value=s_ac97.cas; s_ac97.cas=1; }
    else {
        AC97BusMasterRegs *r=channel(offset,size);
        unsigned reg=offset&15;
        if (reg==0 && size==4) value=r->bdbar;
        else if (reg==4 && size==4) value=r->civ|(r->lvi<<8)|(r->sr<<16);
        else if (reg==8 && size==4) value=r->picb|(r->piv<<16)|(r->cr<<24);
        else if (reg==4 && size==1) value=r->civ;
        else if (reg==5 && size==1) value=r->lvi;
        else if (reg==6 && (size==1 || size==2)) value=r->sr;
        else if (reg==8 && size==2) value=r->picb;
        else if (reg==10 && size==1) value=r->piv;
        else if (reg==11 && size==1) value=r->cr;
        else ac97_stop("unsupported read",offset,0,size);
    }
    if (g_dbg.trace_enabled) {
        static unsigned count;
        if (count++<160) fprintf(stderr,"  [AC97MMIO] read  0x%03X = %08X size=%u\n",offset,value,size);
    }
    return value;
}
static void codec_write(uint32_t offset, uint16_t value)
{
    uint16_t *reg=&s_ac97.mixer_data[offset/2];
    s_ac97.cas=0;
    switch (offset) {
    case AC97_Reset: mixer_reset(); break;
    case AC97_Powerdown_Ctrl_Stat: *reg=(value&~0x800f)|(*reg&15); break;
    case AC97_Master_Volume_Mute: *reg=value&0xbf3f; break;
    case AC97_PCM_Out_Volume_Mute: *reg=value&0x9f1f; break;
    case AC97_Record_Gain_Mute: *reg=value&0x8f0f; break;
    case AC97_Record_Select: *reg=value&0x0707; break;
    case AC97_Extended_Audio_Ctrl_Stat:
        if (!(value&EACS_VRA)) {
            s_ac97.mixer_data[AC97_PCM_Front_DAC_Rate/2]=48000;
            s_ac97.mixer_data[AC97_PCM_LR_ADC_Rate/2]=48000;
        }
        if (!(value&EACS_VRM)) s_ac97.mixer_data[AC97_MIC_ADC_Rate/2]=48000;
        *reg=value;
        break;
    case AC97_PCM_Front_DAC_Rate:
    case AC97_PCM_LR_ADC_Rate:
        if (s_ac97.mixer_data[AC97_Extended_Audio_Ctrl_Stat/2]&EACS_VRA) *reg=value;
        break;
    case AC97_MIC_ADC_Rate:
        if (s_ac97.mixer_data[AC97_Extended_Audio_Ctrl_Stat/2]&EACS_VRM) *reg=value;
        break;
    case AC97_Vendor_ID1: case AC97_Vendor_ID2: case AC97_Extended_Audio_ID:
    case AC97_Headphone_Volume_Mute: case AC97_Master_Volume_Mono_Mute:
    case AC97_Master_Tone_RL: case AC97_PC_BEEP_Volume_Mute:
    case AC97_Phone_Volume_Mute: case AC97_Mic_Volume_Mute:
    case AC97_Line_In_Volume_Mute: case AC97_CD_Volume_Mute:
    case AC97_Video_Volume_Mute: case AC97_Aux_Volume_Mute:
    case AC97_Record_Gain_Mic_Mute: case AC97_General_Purpose:
    case AC97_3D_Control: case AC97_Sigmatel_Analog: case AC97_Sigmatel_Dac2Invert:
        break; /* Read-only capabilities of this codec, as in the original link. */
    default: ac97_stop("unsupported codec write",offset,value,2);
    }
}
static void ac97_write_locked(void *device, uint32_t offset, uint64_t value, unsigned size)
{
    if (!s_ac97.attached) ac97_stop("device absent",offset,value,size);
    if (g_dbg.trace_enabled) {
        static unsigned count;
        if (count++<160) fprintf(stderr,"  [AC97MMIO] write 0x%03X = %08X size=%u\n",offset,(uint32_t)value,size);
    }
    if (offset<0x100) {
        if (size!=2 || (offset&1) || offset>0xfe) ac97_stop("unsupported codec access",offset,value,size);
        codec_write(offset,(uint16_t)value);
        return;
    }
    if (offset==0x12c && size==4) {
        if (value&4) ac97_stop("warm reset not implemented",offset,value,size);
        if (!(value&2)) {
            for (unsigned i=0;i<8;++i) reset_bm_regs(&s_ac97.bm_regs[i]);
            mixer_reset();
        }
        s_ac97.glob_cnt=(uint32_t)value&0x3f;
        return;
    }
    if (offset==0x130 && size==4) {
        s_ac97.glob_sta&=~((uint32_t)value&0x8c01);
        s_ac97.glob_sta|=((uint32_t)value&~(0x8c01|0x73fe))&0x3ffff;
        return;
    }
    if (offset==0x17c && size==4) { s_ac97.spdif_status=(uint32_t)value; return; }
    AC97BusMasterRegs *r=channel(offset,size);
    unsigned reg=offset&15;
    if ((reg==4 && size==1) || (reg==8 && size==2) || (reg==10 && size==1)) return;
    if (reg==0 && size==4) r->bdbar=(uint32_t)value&~3u;
    else if (reg==5 && size==1) {
        if ((r->cr&1) && (r->sr&1)) {
            r->sr&=~3;
            r->civ=r->piv;
            r->piv=(r->piv+1)%32;
            fetch_bd(r);
        }
        r->lvi=(uint8_t)value&31;
    }
    else if (reg==6 && (size==1 || size==2)) {
        r->sr|=(uint16_t)value&~(3|0x1c);
        update_sr(r,r->sr&~((uint16_t)value&0x1c));
    } else if (reg==11 && size==1) {
        if (value&2) { reset_bm_regs(r); update_sr(r,r->sr); }
        else {
            r->cr=(uint8_t)value&31;
            if (value&1) {
                if (r!=&s_ac97.bm_regs[1] && r!=&s_ac97.bm_regs[7]) ac97_stop("capture DMA is not connected",offset,value,size);
                r->civ=r->piv;
                r->piv=(r->piv+1)%32;
                fetch_bd(r);
                r->sr&=~1;
                MCPXAPUState *apu=device;
                apu->pause_requested=false;
                qemu_cond_broadcast(&apu->cond);
            } else r->sr|=1;
            update_sr(r,r->sr);
        }
    } else ac97_stop("unsupported write",offset,value,size);
}

uint64_t mcpx_ac97_read(void *device, uint32_t offset, unsigned size)
{
    MCPXAPUState *d=device;
    qemu_mutex_lock(&d->lock);
    uint64_t value=ac97_read_locked(device,offset,size);
    qemu_mutex_unlock(&d->lock);
    return value;
}
void mcpx_ac97_write(void *device, uint32_t offset, uint64_t value, unsigned size)
{
    MCPXAPUState *d=device;
    qemu_mutex_lock(&d->lock);
    ac97_write_locked(device,offset,value,size);
    qemu_mutex_unlock(&d->lock);
}
