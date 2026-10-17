#include "apu.h"
#include "apu_state.h"
#include "dsp56300.h"
#include "dsp/dsp_dma_regs.h"
#include "nv2a_mmio_hook.h"
#include <assert.h>

/* Device fixture: the same production registers, decoder and DSP/DMA engine,
 * with physical RAM but no host audio stream or background frame worker. */
extern MCPXAPUState *g_apu_state;
extern MCPXAPUState *g_state;
int xbox_CheckpointFrozen(void) { return 0; }
void xbox_RuntimePerformanceCounter(LARGE_INTEGER *counter) { QueryPerformanceCounter(counter); }
void xbox_CheckpointDeviceEnter(unsigned device) { (void)device;abort(); }
void xbox_CheckpointDeviceLeave(unsigned device) { (void)device;abort(); }
void xbox_CheckpointDeviceCreating(unsigned device) { (void)device;abort(); }
static unsigned trap_isr_calls;
static unsigned declined_isr_calls;
static void *fixture_physical_resolve(uint32_t address, uint32_t bytes)
{
    return mcpx_apu_phys(address, bytes);
}
typedef struct {
    MCPXAPUState *device;
    HANDLE entered, finished;
    float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
} VoiceFrameCheck;

static DWORD WINAPI run_voice_frame(void *arg)
{
    VoiceFrameCheck *check=arg;
    qemu_mutex_lock(&check->device->lock);
    SetEvent(check->entered);
    mcpx_apu_vp_frame(check->device,check->mixbins);
    qemu_mutex_unlock(&check->device->lock);
    SetEvent(check->finished);
    return 0;
}

static void check_voice_lock(MCPXAPUState *d,uint8_t *ram)
{
    VoiceFrameCheck check={0};
    check.device=d;
    check.entered=CreateEventA(NULL,TRUE,FALSE,NULL);
    check.finished=CreateEventA(NULL,TRUE,FALSE,NULL);
    assert(check.entered && check.finished);
    mcpx_apu_mmio_write(d,NV_PAPU_VPVADDR,0x6000,4);
    mcpx_apu_mmio_write(d,NV_PAPU_VPSSLADDR,0x9000,4);
    mcpx_apu_mmio_write(d,NV_PAPU_TVL2D,64,4);
    mcpx_apu_mmio_write(d,NV_PAPU_TVL3D,0xffff,4);
    mcpx_apu_mmio_write(d,NV_PAPU_TVLMP,0xffff,4);
    uint32_t *voice=(uint32_t *)(ram+0x6000+64*0x80);
    voice[NV_PAVS_VOICE_CFG_FMT/4]=NV_PAVS_VOICE_CFG_FMT_PERSIST |
        NV_PAVS_VOICE_CFG_FMT_DATA_TYPE | (1u<<28) | (1u<<30);
    voice[NV_PAVS_VOICE_PAR_STATE/4]=NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE |
        (5u<<24) | (5u<<28);
    voice[NV_PAVS_VOICE_PAR_OFFSET/4]=0xff000000u;
    voice[NV_PAVS_VOICE_PAR_NEXT/4]=0xff000000u;
    voice[NV_PAVS_VOICE_TAR_PITCH_LINK/4]=0xffff;
    mcpx_apu_mmio_write(d,0x20000+NV1BA0_PIO_SET_CURRENT_VOICE,64,4);
    mcpx_apu_mmio_write(d,0x20000+NV1BA0_PIO_VOICE_LOCK,1,4);
    mcpx_apu_mmio_write(d,0x20000+NV1BA0_PIO_SET_VOICE_SSL_A,1,4);
    HANDLE worker=CreateThread(NULL,0,run_voice_frame,&check,0,NULL);
    assert(worker);
    assert(WaitForSingleObject(check.entered,1000)==WAIT_OBJECT_0);
    assert(WaitForSingleObject(check.finished,30)==WAIT_TIMEOUT);
    /* Populate the original descriptor while the voice is locked. */
    mcpx_apu_mmio_write(d,0x20000+NV1BA0_PIO_SET_CURRENT_SSL,0,4);
    mcpx_apu_mmio_write(d,0x20000+NV1BA0_PIO_SET_SSL_SEGMENT_OFFSET,0xa000,4);
    mcpx_apu_mmio_write(d,0x20000+NV1BA0_PIO_SET_SSL_SEGMENT_LENGTH,0x00010200,4);
    for (unsigned i=0;i<512;++i) ((int16_t *)(ram+0xa000))[i]=1234;
    assert((voice[NV_PAVS_VOICE_PAR_OFFSET/4]&0xffffff)==0);
    mcpx_apu_mmio_write(d,0x20000+NV1BA0_PIO_VOICE_LOCK,0,4);
    assert(WaitForSingleObject(check.finished,1000)==WAIT_OBJECT_0);
    assert(WaitForSingleObject(worker,1000)==WAIT_OBJECT_0);
    /* The sinc converter may prefetch; ownership must still block all input
     * consumption until unlock, then advance within the actual descriptor. */
    unsigned consumed=voice[NV_PAVS_VOICE_PAR_OFFSET/4]&0xffffff;
    assert(consumed>0 && consumed<512);
    CloseHandle(worker); CloseHandle(check.entered); CloseHandle(check.finished);
}
typedef struct {
    MCPXAPUState *d;
    HANDLE entered, finished;
} SslWriteCheck;
static DWORD WINAPI run_ssl_write(void *opaque)
{
    SslWriteCheck *check=opaque;
    SetEvent(check->entered);
    mcpx_apu_mmio_write(check->d,0x20000+NV1BA0_PIO_SET_VOICE_SSL_B,0x00020002,4);
    SetEvent(check->finished);
    return 0;
}
static void check_fe_transaction(MCPXAPUState *d)
{
    /* The real frame consumer holds this device lock while reading SSL.
     * A public FE method must not update its base/count during that frame. */
    SslWriteCheck check={d,CreateEvent(NULL,TRUE,FALSE,NULL),CreateEvent(NULL,TRUE,FALSE,NULL)};
    assert(check.entered && check.finished);
    mcpx_apu_mmio_write(d,0x20000+NV1BA0_PIO_SET_CURRENT_VOICE,254,4);
    assert(d->vp.ssl[254].base[1]==0 && d->vp.ssl[254].count[1]==0);
    qemu_mutex_lock(&d->lock);
    HANDLE worker=CreateThread(NULL,0,run_ssl_write,&check,0,NULL);
    assert(worker && WaitForSingleObject(check.entered,1000)==WAIT_OBJECT_0);
    DWORD completed=WaitForSingleObject(check.finished,30);
    if(completed!=WAIT_TIMEOUT)
        fprintf(stderr,"[APUFE] method completed inside active frame: base=%u count=%u\n",
                d->vp.ssl[254].base[1],d->vp.ssl[254].count[1]);
    assert(completed==WAIT_TIMEOUT);
    assert(d->vp.ssl[254].base[1]==0 && d->vp.ssl[254].count[1]==0);
    qemu_mutex_unlock(&d->lock);
    assert(WaitForSingleObject(check.finished,1000)==WAIT_OBJECT_0);
    assert(WaitForSingleObject(worker,1000)==WAIT_OBJECT_0);
    assert(d->vp.ssl[254].base[1]==512 && d->vp.ssl[254].count[1]==2);
    CloseHandle(worker); CloseHandle(check.entered); CloseHandle(check.finished);
}
static int decline_irq(uint32_t vector)
{
    assert(vector == 5);
    ++declined_isr_calls;
    return 0;
}
static int consume_trap_irq(uint32_t vector)
{
    assert(vector == 5);
    ++trap_isr_calls;
    /* The real NINE ISR handles the method on every FE interrupt.
     * A second delivery without a new idle event reuses a retired voice. */
    assert(trap_isr_calls == 1);
    assert(mcpx_apu_mmio_read(g_state, NV_PAPU_FECTL, 4) == 0x1FEF);
    mcpx_apu_mmio_write(g_state, NV_PAPU_ISTS, 0x11, 4);
    mcpx_apu_mmio_write(g_state, NV_PAPU_TVL3D, 0xffff, 4);
    mcpx_apu_mmio_write(g_state, NV_PAPU_FECTL, 0x1F8F, 4);
    mcpx_apu_mmio_write(g_state, NV_PAPU_FECTL, 0x1F0F, 4);
    return 1;
}
static void publish_idle_event(MCPXAPUState *d,uint8_t *ram)
{
    mcpx_apu_mmio_write(d,NV_PAPU_FECTL,0x1F0F,4);
    mcpx_apu_mmio_write(d,NV_PAPU_VPVADDR,0x6000,4);
    mcpx_apu_mmio_write(d,NV_PAPU_FETFORCE1,NV_PAPU_FETFORCE1_SE2FE_IDLE_VOICE,4);
    mcpx_apu_mmio_write(d,NV_PAPU_TVL2D,0xffff,4);
    mcpx_apu_mmio_write(d,NV_PAPU_TVL3D,0,4);
    mcpx_apu_mmio_write(d,NV_PAPU_TVLMP,0xffff,4);
    uint32_t *voice=(uint32_t *)(ram+0x6000);
    memset(voice,0,0x80);
    voice[NV_PAVS_VOICE_TAR_PITCH_LINK/4]=0xffff;
    float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME]={0};
    qemu_mutex_lock(&d->lock);
    mcpx_apu_vp_frame(d,mixbins);
    qemu_mutex_unlock(&d->lock);
    assert(mcpx_apu_mmio_read(d,NV_PAPU_FEDECMETH,4)==SE2FE_IDLE_VOICE);
    assert(mcpx_apu_mmio_read(d,NV_PAPU_FEDECPARAM,4)==0);
    /* A zero status acknowledgement applies the original IRQ calculator. */
    mcpx_apu_mmio_write(d,NV_PAPU_ISTS,0,4);
}
static uint64_t read_ac97(void *d, uint32_t a, unsigned n) { return mcpx_ac97_read(d,a,n); }
static void write_ac97(void *d, uint32_t a, uint64_t v, unsigned n) { mcpx_ac97_write(d,a,v,n); }

int main(int argc, char **argv)
{
    MCPXAPUState *d=calloc(1,sizeof(*d));
    assert(d);
    mcpx_apu_vp_init(d);
    uint8_t *ram=calloc(1,0x04000000);
    assert(d && ram);
    qemu_mutex_init(&d->lock);
    qemu_cond_init(&d->cond);
    d->ram_ptr=ram;
    d->physical_resolve=fixture_physical_resolve;
    g_apu_ram_ptr=ram;
    g_apu_state=d;
    mcpx_apu_dsp_init(d);
    mcpx_ac97_init();
    g_state=d;
    /* Consume the existing DSP output callback, preserving its real DMA bytes. */
    d->monitor.ac97_output_channel=1;
    mcpx_apu_update_dsp_preference(d);
    assert(d->monitor.point==MCPX_APU_DEBUG_MON_GP_OR_EP);
    uint32_t *ep_sge=(uint32_t *)(ram+0x8000);ep_sge[0]=0x9000;
    d->regs[NV_PAPU_EPFADDR]=0x8000;d->regs[NV_PAPU_EPFMAXSGE]=0;
    d->regs[NV_PAPU_EPOFBASE0]=0;d->regs[NV_PAPU_EPOFEND0]=4096;d->regs[NV_PAPU_EPOFCUR0]=0;
    int16_t ep_pcm[256][2];
    for(unsigned i=0;i<256;i++){ep_pcm[i][0]=(int16_t)(i*37-4000);ep_pcm[i][1]=(int16_t)(9000-i*53);}
    d->ep.dsp->dma.fifo_rw(d->ep.dsp->dma.rw_opaque,(uint8_t *)ep_pcm,0,sizeof ep_pcm,true);
    assert(!memcmp(d->monitor.frame_buf,ep_pcm,sizeof ep_pcm));
    assert(!memcmp(ram+0x9000,ep_pcm,sizeof ep_pcm));
    assert(d->regs[NV_PAPU_EPOFCUR0]==sizeof ep_pcm);
    d->monitor.ac97_output_channel=7;mcpx_apu_update_dsp_preference(d);assert(d->monitor.point==MCPX_APU_DEBUG_MON_AC97);
    d->monitor.ac97_output_channel=1;d->monitor.point=MCPX_APU_DEBUG_MON_AC97;
    memset(d->monitor.frame_buf,0,sizeof d->monitor.frame_buf);
    printf("Original DSP PCM monitor and physical FIFO bytes match; fixed digital selection preserves AC97.\n");
    assert(!mcpx_apu_irqs_pending());
    if (argc==2) {
        if (!strcmp(argv[1],"--reject-physical")) mcpx_apu_phys(0x03ffffff,2);
        if (!strcmp(argv[1],"--reject-adpcm-block")) {
            mcpx_apu_mmio_write(d,NV_PAPU_VPVADDR,0x6000,4);
            mcpx_apu_mmio_write(d,NV_PAPU_VPSSLADDR,0x9000,4);
            mcpx_apu_mmio_write(d,NV_PAPU_TVL2D,32,4);
            mcpx_apu_mmio_write(d,NV_PAPU_TVL3D,0xffff,4);
            mcpx_apu_mmio_write(d,NV_PAPU_TVLMP,0xffff,4);
            uint32_t *voice=(uint32_t *)(ram+0x6000+32*0x80);
            voice[NV_PAVS_VOICE_CFG_FMT/4]=NV_PAVS_VOICE_CFG_FMT_PERSIST |
                NV_PAVS_VOICE_CFG_FMT_DATA_TYPE | NV_PAVS_VOICE_CFG_FMT_STEREO |
                (3u<<16) | (2u<<28) | (2u<<30);
            voice[NV_PAVS_VOICE_PAR_STATE/4]=NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE |
                (5u<<24) | (5u<<28);
            voice[NV_PAVS_VOICE_PAR_OFFSET/4]=0xff000000u;
            voice[NV_PAVS_VOICE_PAR_NEXT/4]=0xff000000u;
            voice[NV_PAVS_VOICE_TAR_PITCH_LINK/4]=0xffff;
            mcpx_apu_mmio_write(d,0x20000+NV1BA0_PIO_SET_CURRENT_VOICE,32,4);
            mcpx_apu_mmio_write(d,0x20000+NV1BA0_PIO_SET_VOICE_SSL_A,1,4);
            mcpx_apu_mmio_write(d,0x20000+NV1BA0_PIO_SET_SSL_SEGMENT_OFFSET,0xa000,4);
            mcpx_apu_mmio_write(d,0x20000+NV1BA0_PIO_SET_SSL_SEGMENT_LENGTH,0x00860040,4);
            float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME]={0};
            mcpx_apu_vp_frame(d,mixbins);
        }
        if (!strcmp(argv[1],"--reject-descriptor")) {
            mcpx_ac97_write(d,0x110,0x03fffffc,4);
            mcpx_ac97_write(d,0x11b,1,1);
        }
        if (!strcmp(argv[1],"--reject-spdif-format")) {
            uint32_t *bd=(uint32_t *)(ram+0x1000);
            bd[0]=0x2000; bd[1]=64;
            mcpx_ac97_write(d,0x170,0x1000,4);
            mcpx_ac97_write(d,0x17c,0x02000002,4);
            mcpx_ac97_write(d,0x17b,1,1);
            mcpx_ac97_frame();
        }
        return 1;
    }
    assert(mcpx_ac97_read(d,0x130,4)==0x100);
    mcpx_ac97_write(d,0x130,0x100,4);
    assert(mcpx_ac97_read(d,0x130,4)==0x100);
    assert(mcpx_ac97_read(d,0x134,1)==0);
    assert(mcpx_ac97_read(d,0x134,1)==1);
    assert(mcpx_ac97_read(d,0x7c,2)==0x8384);
    assert(mcpx_ac97_read(d,0x134,1)==0);
    mcpx_ac97_write(d,0x7c,0,2);
    assert(mcpx_ac97_read(d,0x7c,2)==0x8384);
    mcpx_ac97_write(d,0x11b,0x1c,1);
    mcpx_ac97_write(d,0x110,0x1003,4);
    mcpx_ac97_write(d,0x115,31,1);
    mcpx_ac97_write(d,0x11b,2,1);
    assert(mcpx_ac97_read(d,0x11b,1)==0x1c);
    assert(mcpx_ac97_read(d,0x110,4)==0);
    assert(mcpx_ac97_read(d,0x114,4)==0x00010000);
    uint8_t mov_word[]={0x66,0xc7,0x00,0x08,0x08};
    CONTEXT ctx={0}; ctx.Rip=(DWORD64)mov_word;
    assert(xbox_mmio_decode(&ctx,0x18,1,d,read_ac97,write_ac97));
    assert(ctx.Rip==(DWORD64)mov_word+sizeof(mov_word));
    assert(mcpx_ac97_read(d,0x18,2)==0x0808);
    uint8_t mov_cl[]={0x8a,0x08};
    ctx.Rip=(DWORD64)mov_cl; ctx.Rcx=0x1234567890abcdefULL;
    assert(xbox_mmio_decode(&ctx,0x11b,0,d,read_ac97,write_ac97));
    assert(ctx.Rcx==0x1234567890abcd1cULL);

    uint32_t *bd=(uint32_t *)(ram+0x1000);
    int16_t *pcm=(int16_t *)(ram+0x2000);
    pcm[0]=10000; pcm[1]=-10000; pcm[2]=1234; pcm[3]=-4321;
    bd[0]=0x2000; bd[1]=0x80000004;
    bd[2]=0x2040; bd[3]=0x80000040;
    mcpx_ac97_write(d,0x2,0,2);
    mcpx_ac97_write(d,0x18,0,2);
    mcpx_ac97_write(d,0x110,0x1000,4);
    mcpx_ac97_write(d,0x11b,0x1d,1);
    mcpx_ac97_frame();
    assert(d->monitor.frame_buf[0][0]==10000 && d->monitor.frame_buf[0][1]==-10000);
    assert(d->monitor.frame_buf[1][0]==1234 && d->monitor.frame_buf[1][1]==-4321);
    assert(mcpx_ac97_read(d,0x118,2)==0);
    assert((mcpx_ac97_read(d,0x116,1)&31)==15 && mcpx_ac97_irq_pending());
    assert(mcpx_apu_irqs_pending());
    mcpx_ac97_write(d,0x116,0xff,1);
    assert((mcpx_ac97_read(d,0x116,1)&31)==3 && !mcpx_ac97_irq_pending());
    assert(!mcpx_apu_irqs_pending());
    mcpx_ac97_write(d,0x115,1,1);
    assert(mcpx_ac97_read(d,0x114,1)==1);
    assert(mcpx_ac97_read(d,0x118,2)==64);
    mcpx_ac97_frame();
    assert((mcpx_ac97_read(d,0x116,1)&31)==15);
    mcpx_ac97_write(d,0x11b,2,1);
    assert(!mcpx_ac97_irq_pending());

    /* Actual NINE descriptor flags select the original analog BUP last sample. */
    bd[0]=0x2000; bd[1]=0xc0001000;
    for (unsigned i=0;i<4096;++i) pcm[i]=(int16_t)(i&255);
    pcm[4094]=1234; pcm[4095]=-4321;
    mcpx_ac97_write(d,0x110,0x1000,4);
    mcpx_ac97_write(d,0x11b,0x1d,1);
    for (unsigned i=0;i<64;++i) mcpx_ac97_frame();
    assert((mcpx_ac97_read(d,0x116,1)&31)==15);
    assert(mcpx_ac97_read(d,0x118,2)==0);
    mcpx_ac97_write(d,0x116,0xff,1);
    pcm[4094]=999; pcm[4095]=888;
    memset(d->monitor.frame_buf,0,sizeof(d->monitor.frame_buf));
    mcpx_ac97_frame();
    for (unsigned i=0;i<32;++i) {
        assert(d->monitor.frame_buf[i][0]==1234);
        assert(d->monitor.frame_buf[i][1]==-4321);
    }
    assert((mcpx_ac97_read(d,0x116,1)&31)==3);
    assert(mcpx_ac97_read(d,0x114,1)==0 && mcpx_ac97_read(d,0x118,2)==0);
    assert(!mcpx_ac97_irq_pending());
    mcpx_ac97_write(d,0x2,0x8000,2);
    mcpx_ac97_frame();
    assert(d->monitor.frame_buf[0][0]==0 && d->monitor.frame_buf[0][1]==0);
    mcpx_ac97_write(d,0x2,0,2);
    mcpx_ac97_frame();
    assert(d->monitor.frame_buf[0][0]==1234 && d->monitor.frame_buf[0][1]==-4321);
    mcpx_ac97_write(d,0x11b,0x1c,1);
    memset(d->monitor.frame_buf,0,sizeof(d->monitor.frame_buf));
    mcpx_ac97_frame();
    assert(d->monitor.frame_buf[0][0]==0 && d->monitor.frame_buf[0][1]==0);
    mcpx_ac97_write(d,0x11b,2,1);
    memset(d->monitor.frame_buf,0,sizeof(d->monitor.frame_buf));
    mcpx_ac97_frame();
    assert(d->monitor.frame_buf[0][0]==0 && d->monitor.frame_buf[0][1]==0);

    /* BUP clear preserves the existing zero output after the last descriptor. */
    pcm[0]=10000; pcm[1]=-10000; pcm[2]=1234; pcm[3]=-4321;
    bd[1]=0x80000004;
    mcpx_ac97_write(d,0x110,0x1000,4);
    mcpx_ac97_write(d,0x11b,0x1d,1);
    mcpx_ac97_frame();
    memset(d->monitor.frame_buf,0,sizeof(d->monitor.frame_buf));
    mcpx_ac97_frame();
    assert(d->monitor.frame_buf[0][0]==0 && d->monitor.frame_buf[0][1]==0);
    mcpx_ac97_write(d,0x11b,2,1);

    /* Independent digital payload/IRQ, unaffected by analog codec mute. */
    int16_t previous_analog=d->monitor.frame_buf[0][0];
    mcpx_ac97_write(d,0x2,0x8000,2);
    mcpx_ac97_write(d,0x18,0x8000,2);
    mcpx_ac97_write(d,0x170,0x1000,4);
    mcpx_ac97_write(d,0x17c,0x02000000,4);
    mcpx_ac97_write(d,0x17b,0x1d,1);
    mcpx_ac97_frame();
    assert(d->monitor.spdif_buf[0][0]==10000 && d->monitor.spdif_buf[0][1]==-10000);
    assert(d->monitor.spdif_buf[1][0]==1234 && d->monitor.spdif_buf[1][1]==-4321);
    assert(d->monitor.frame_buf[0][0]==previous_analog);
    assert(d->monitor.spdif_channel_status==0x02000000);
    assert((mcpx_ac97_read(d,0x176,1)&31)==15);
    assert((mcpx_ac97_read(d,0x130,4)&0x10) && mcpx_ac97_irq_pending());
    assert(mcpx_apu_irqs_pending());
    mcpx_ac97_write(d,0x176,0xff,1);
    assert(!mcpx_ac97_irq_pending());
    ((int16_t *)(ram+0x2040))[0]=4000;
    ((int16_t *)(ram+0x2040))[1]=-4000;
    mcpx_ac97_write(d,0x175,1,1);
    assert(mcpx_ac97_read(d,0x174,1)==1 && mcpx_ac97_read(d,0x178,2)==64);
    mcpx_ac97_frame();
    assert(d->monitor.spdif_buf[0][0]==4000 && d->monitor.spdif_buf[0][1]==-4000);
    assert((mcpx_ac97_read(d,0x176,1)&31)==15);
    mcpx_ac97_write(d,0x17b,2,1);
    assert(!mcpx_ac97_irq_pending());

    /* Original scatter/gather bootstrap loads a real three-instruction program. */
    ((uint32_t *)(ram+0x3000))[0]=0x4000;
    ((uint32_t *)(ram+0x3000))[2]=0x5000;
    uint32_t *program=(uint32_t *)(ram+0x4000);
    program[0]=0;
    program[1]=0x052aa0; /* MOVEC #0x2a,M0 */
    program[2]=0x0c0000; /* JMP 0 */
    mcpx_apu_mmio_write(d,NV_PAPU_GPSADDR,0x3000,4);
    mcpx_apu_mmio_write(d,NV_PAPU_GPSMAXSGE,1,4);
    mcpx_apu_mmio_write(d,0x30000+NV_PAPU_GPRST,1,4);
    mcpx_apu_mmio_write(d,0x30000+NV_PAPU_GPRST,3,4);
    assert(dsp_read_memory(d->gp.dsp,'P',1)==0x052aa0);
    Dsp56300Jit *jit;
    Dsp56300State core;
    /* The original JitBackend's first member is its library handle. */
    memcpy(&jit,d->gp.dsp->backend,sizeof(jit));
    dsp_step(d->gp.dsp);
    dsp56300_get_state(jit,&core);
    assert(core.pc==1);
    dsp_step(d->gp.dsp);
    dsp56300_get_state(jit,&core);
    assert(core.pc==2 && core.registers[DSP56300_REG_M0]==0x2a);
    dsp_step(d->gp.dsp);
    dsp56300_get_state(jit,&core);
    assert(core.pc==0 && core.cycle_count>0);
    /* DMA copies computed DSP words through the original SGE into physical RAM. */
    uint32_t node[7]={NODE_POINTER_EOL, (15<<5)|(6<<10)|NODE_CONTROL_DIRECTION,2,0x40,0x100,0,0xfff};
    for (unsigned i=0;i<7;++i) dsp_write_memory(d->gp.dsp,'X',0x80+i,node[i]);
    dsp_write_memory(d->gp.dsp,'X',0x40,0x123456);
    dsp_write_memory(d->gp.dsp,'X',0x41,0x654321);
    dsp_dma_write(&d->gp.dsp->dma,DMA_NEXT_BLOCK,0x80);
    dsp_dma_write(&d->gp.dsp->dma,DMA_CONTROL,DMA_CONTROL_ACTION_START);
    assert(((uint32_t *)(ram+0x4100))[0]==0x123456);
    assert(((uint32_t *)(ram+0x4100))[1]==0x654321);
    assert(d->gp.dsp->dma.eol);
    mcpx_apu_mmio_write(d,NV_PAPU_IEN,0x11,4);
    publish_idle_event(d,ram);
    assert(mcpx_apu_irqs_pending());
    mcpx_apu_service_irqs(consume_trap_irq);
    assert(trap_isr_calls==1); /* One service per actual idle event. */
    assert(!mcpx_apu_irqs_pending());
    mcpx_apu_service_irqs(consume_trap_irq);
    assert(trap_isr_calls==1); /* Cleared levels produce no extra interrupt. */
    publish_idle_event(d,ram);
    mcpx_apu_service_irqs(decline_irq);
    assert(declined_isr_calls==1);
    assert(mcpx_apu_irqs_pending());
    assert(mcpx_apu_mmio_read(d,NV_PAPU_ISTS,4)&0x11); /* Deferred level is retained. */
    trap_isr_calls=0;
    mcpx_apu_service_irqs(consume_trap_irq);
    assert(trap_isr_calls==1);
    g_state=NULL;
    check_fe_transaction(d);
    check_voice_lock(d,ram);
    puts("APU/AC97 checks passed: shared MMIO, independent analog/digital PCM/IRQ, GP bootstrap/instructions, DSP DMA and pending trap-level service");
    dsp_destroy(d->gp.dsp); dsp_destroy(d->ep.dsp);
    free(d); free(ram);
    return 0;
}
