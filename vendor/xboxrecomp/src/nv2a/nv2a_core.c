/*
 * NV2A GPU Core - Standalone register handlers
 *
 * Adapted from xemu (Copyright (c) 2012 espes, 2015 Jannik Vogel,
 * 2018-2025 Matt Borgerson) - LGPL v2+
 *
 * Contains: PMC, PBUS, PTIMER, PFB, PCRTC, PRAMDAC register handlers,
 * nv2a_update_irq, DMA helpers, block dispatch table, and standalone init.
 */

#include "nv2a_state.h"
#include "nv2a_color_logic.h"

/* ============================================================
 * Global state
 * ============================================================ */

static NV2AState *g_nv2a = NULL;
static MemoryRegion g_vram_region;
static MemoryRegion g_ramin_region;
static uint64_t g_ptimer_clock_bias;

NV2AState *nv2a_get_state(void) {
    return g_nv2a;
}

/* ============================================================
 * IRQ aggregation (from xemu nv2a.c)
 * ============================================================ */

void nv2a_update_irq(NV2AState *d)
{
    /* PFIFO */
    if (d->pfifo.pending_interrupts & d->pfifo.enabled_interrupts) {
        d->pmc.pending_interrupts |= NV_PMC_INTR_0_PFIFO;
    } else {
        d->pmc.pending_interrupts &= ~NV_PMC_INTR_0_PFIFO;
    }

    /* PCRTC */
    if (d->pcrtc.pending_interrupts & d->pcrtc.enabled_interrupts) {
        d->pmc.pending_interrupts |= NV_PMC_INTR_0_PCRTC;
    } else {
        d->pmc.pending_interrupts &= ~NV_PMC_INTR_0_PCRTC;
    }

    /* PGRAPH */
    if (d->pgraph.pending_interrupts & d->pgraph.enabled_interrupts) {
        d->pmc.pending_interrupts |= NV_PMC_INTR_0_PGRAPH;
    } else {
        d->pmc.pending_interrupts &= ~NV_PMC_INTR_0_PGRAPH;
    }

    if (d->pmc.pending_interrupts && d->pmc.enabled_interrupts) {
        pci_irq_assert(PCI_DEVICE(d));
    } else {
        pci_irq_deassert(PCI_DEVICE(d));
    }
}

/* ============================================================
 * DMA helpers (from xemu nv2a.c)
 * ============================================================ */

DMAObject nv_dma_load(NV2AState *d, hwaddr dma_obj_address)
{
    assert(dma_obj_address < memory_region_size(&d->ramin));

    uint32_t *dma_obj = (uint32_t *)(d->ramin_ptr + dma_obj_address);
    uint32_t flags = ldl_le_p(dma_obj);
    uint32_t limit = ldl_le_p(dma_obj + 1);
    uint32_t frame = ldl_le_p(dma_obj + 2);

    return (DMAObject){
        .dma_class  = GET_MASK(flags, NV_DMA_CLASS),
        .dma_target = GET_MASK(flags, NV_DMA_TARGET),
        .address    = (frame & NV_DMA_ADDRESS) | GET_MASK(flags, NV_DMA_ADJUST),
        .limit      = limit,
    };
}

void *nv_dma_map(NV2AState *d, hwaddr dma_obj_address, hwaddr *len)
{
    DMAObject dma = nv_dma_load(d, dma_obj_address);
    dma.address &= 0x07FFFFFF;

    if (dma.address >= memory_region_size(d->vram)) {
        fprintf(stderr, "[NV2A] DMA map address 0x%llx out of VRAM range\n",
                (unsigned long long)dma.address);
        *len = 0;
        return NULL;
    }

    *len = dma.limit;
    return d->vram_ptr + dma.address;
}

void *nv_dma_span(NV2AState *d, hwaddr instance, hwaddr offset, hwaddr bytes)
{
    hwaddr limit = 0;
    if (instance + 12 > memory_region_size(&d->ramin)) {
        fprintf(stderr, "[STOP] invalid DMA instance 0x%llX\n", (unsigned long long)instance);
        exit(5);
    }
    uint8_t *base = nv_dma_map(d, instance, &limit);
    hwaddr physical = base ? (hwaddr)(base - d->vram_ptr) : 0;
    if (!base || offset > limit || !bytes || bytes - 1 > limit - offset ||
        physical + offset > memory_region_size(d->vram) ||
        bytes > memory_region_size(d->vram) - physical - offset) {
        fprintf(stderr, "[STOP] DMA span instance=0x%llX offset=0x%llX bytes=0x%llX limit=0x%llX\n",
                (unsigned long long)instance, (unsigned long long)offset,
                (unsigned long long)bytes, (unsigned long long)limit);
        exit(5);
    }
    return base + offset;
}

/* ============================================================
 * PMC - card master control (from xemu pmc.c)
 * ============================================================ */

uint64_t pmc_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    uint64_t r = 0;
    switch (addr) {
    case NV_PMC_BOOT_0:
        /* NV2A, A03, Rev 0 */
        r = 0x02A000A3;
        break;
    case NV_PMC_INTR_0:
        r = d->pmc.pending_interrupts;
        break;
    case NV_PMC_INTR_EN_0:
        r = d->pmc.enabled_interrupts;
        break;
    default:
        break;
    }

    nv2a_reg_log_read(NV_PMC, addr, size, r);
    return r;
}

void pmc_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    nv2a_reg_log_write(NV_PMC, addr, size, val);

    switch (addr) {
    case NV_PMC_INTR_0:
        d->pmc.pending_interrupts &= ~val;
        nv2a_update_irq(d);
        break;
    case NV_PMC_INTR_EN_0:
        d->pmc.enabled_interrupts = val;
        nv2a_update_irq(d);
        break;
    default:
        break;
    }
}

/* ============================================================
 * PBUS - bus control (from xemu pbus.c)
 * ============================================================ */

uint64_t pbus_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *s = (NV2AState *)opaque;
    PCIDevice *d = PCI_DEVICE(s);

    uint64_t r = 0;
    switch (addr) {
    case NV_PBUS_PCI_NV_0:
        r = pci_get_long(d->config + PCI_VENDOR_ID);
        break;
    case NV_PBUS_PCI_NV_1:
        r = pci_get_long(d->config + PCI_COMMAND);
        break;
    case NV_PBUS_PCI_NV_2:
        r = pci_get_long(d->config + PCI_CLASS_REVISION);
        break;
    default:
        break;
    }

    nv2a_reg_log_read(NV_PBUS, addr, size, r);
    return r;
}

void pbus_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *s = (NV2AState *)opaque;
    PCIDevice *d = PCI_DEVICE(s);

    nv2a_reg_log_write(NV_PBUS, addr, size, val);

    switch (addr) {
    case NV_PBUS_PCI_NV_1:
        pci_set_long(d->config + PCI_COMMAND, val);
        break;
    default:
        break;
    }
}

/* ============================================================
 * PTIMER - time measurement (from xemu ptimer.c)
 * ============================================================ */

uint64_t ptimer_get_clock(NV2AState *d)
{
    if (d->ptimer.numerator == 0) return 0;
    return muldiv64(muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)+g_ptimer_clock_bias,
                             d->pramdac.core_clock_freq,
                             NANOSECONDS_PER_SECOND),
                    d->ptimer.denominator,
                    d->ptimer.numerator);
}

uint64_t ptimer_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    uint64_t r = 0;
    switch (addr) {
    case NV_PTIMER_INTR_0:
        r = d->ptimer.pending_interrupts;
        break;
    case NV_PTIMER_INTR_EN_0:
        r = d->ptimer.enabled_interrupts;
        break;
    case NV_PTIMER_NUMERATOR:
        r = d->ptimer.numerator;
        break;
    case NV_PTIMER_DENOMINATOR:
        r = d->ptimer.denominator;
        break;
    case NV_PTIMER_TIME_0:
        r = (ptimer_get_clock(d) & 0x7ffffff) << 5;
        break;
    case NV_PTIMER_TIME_1:
        r = (ptimer_get_clock(d) >> 27) & 0x1fffffff;
        break;
    default:
        break;
    }

    nv2a_reg_log_read(NV_PTIMER, addr, size, r);
    return r;
}

void ptimer_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    nv2a_reg_log_write(NV_PTIMER, addr, size, val);

    switch (addr) {
    case NV_PTIMER_INTR_0:
        d->ptimer.pending_interrupts &= ~val;
        nv2a_update_irq(d);
        break;
    case NV_PTIMER_INTR_EN_0:
        d->ptimer.enabled_interrupts = val;
        nv2a_update_irq(d);
        break;
    case NV_PTIMER_DENOMINATOR:
        d->ptimer.denominator = val;
        break;
    case NV_PTIMER_NUMERATOR:
        d->ptimer.numerator = val;
        break;
    case NV_PTIMER_ALARM_0:
        d->ptimer.alarm_time = val;
        break;
    default:
        break;
    }
}

/* ============================================================
 * PFB - framebuffer / memory control (from xemu pfb.c)
 * ============================================================ */

uint64_t pfb_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    uint64_t r = 0;
    switch (addr) {
    case NV_PFB_CSTATUS:
        r = memory_region_size(d->vram);
        break;
    case NV_PFB_WBC:
        r = 0; /* Flush not pending */
        break;
    default:
        r = d->pfb.regs[addr];
        break;
    }

    nv2a_reg_log_read(NV_PFB, addr, size, r);
    return r;
}

void pfb_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    nv2a_reg_log_write(NV_PFB, addr, size, val);

    switch (addr) {
    default:
        d->pfb.regs[addr] = val;
        break;
    }
}

/* ============================================================
 * PCRTC - CRT controller (from xemu pcrtc.c)
 * ============================================================ */

/* The original VGA refresh callback publishes PCRTC VBLANK independently of
 * submitted game frames. The standalone timer worker supplies that callback
 * from the existing paused/restored virtual clock, at the NTSC field rate. */
void nv2a_vblank_tick(NV2AState *d)
{
    uint64_t clock = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + g_ptimer_clock_bias;
    uint64_t frame = muldiv64(clock, 60, 1000000000);
    if (frame == d->vblank_frame) return;
    d->vblank_frame = frame;
    d->pcrtc.pending_interrupts |= NV_PCRTC_INTR_0_VBLANK;
    d->pcrtc.raster = 0;
    nv2a_update_irq(d);
}

uint64_t pcrtc_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    uint64_t r = 0;
    switch (addr) {
    case NV_PCRTC_INTR_0:
        r = d->pcrtc.pending_interrupts;
        break;
    case NV_PCRTC_INTR_EN_0:
        r = d->pcrtc.enabled_interrupts;
        break;
    case NV_PCRTC_START:
        r = d->pcrtc.start;
        break;
    case NV_PCRTC_RASTER:
        r = d->pcrtc.raster++;
        break;
    default:
        break;
    }

    nv2a_reg_log_read(NV_PCRTC, addr, size, r);
    return r;
}

void pcrtc_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    nv2a_reg_log_write(NV_PCRTC, addr, size, val);

    switch (addr) {
    case NV_PCRTC_INTR_0:
        d->pcrtc.pending_interrupts &= ~val;
        nv2a_update_irq(d);
        break;
    case NV_PCRTC_INTR_EN_0:
        d->pcrtc.enabled_interrupts = val;
        nv2a_update_irq(d);
        break;
    case NV_PCRTC_START:
        val &= 0x07FFFFFF;
        d->pcrtc.start = val;
        NV2A_DPRINTF("PCRTC_START - %x %x %x %x\n",
                d->vram_ptr[val+64], d->vram_ptr[val+64+1],
                d->vram_ptr[val+64+2], d->vram_ptr[val+64+3]);
        break;
    default:
        break;
    }
}

/* ============================================================
 * PRAMDAC - RAMDAC / PLL control (from xemu pramdac.c)
 * ============================================================ */

uint64_t pramdac_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    uint64_t r = 0;
    switch (addr & ~3) {
    case NV_PRAMDAC_NVPLL_COEFF:
        r = d->pramdac.core_clock_coeff;
        break;
    case NV_PRAMDAC_MPLL_COEFF:
        r = d->pramdac.memory_clock_coeff;
        break;
    case NV_PRAMDAC_VPLL_COEFF:
        r = d->pramdac.video_clock_coeff;
        break;
    case NV_PRAMDAC_PLL_TEST_COUNTER:
        /* emulated PLLs locked instantly */
        r = NV_PRAMDAC_PLL_TEST_COUNTER_VPLL2_LOCK
             | NV_PRAMDAC_PLL_TEST_COUNTER_NVPLL_LOCK
             | NV_PRAMDAC_PLL_TEST_COUNTER_MPLL_LOCK
             | NV_PRAMDAC_PLL_TEST_COUNTER_VPLL_LOCK;
        break;
    case NV_PRAMDAC_GENERAL_CONTROL:
        r = d->pramdac.general_control;
        break;
    case NV_PRAMDAC_FP_VDISPLAY_END:
        r = d->pramdac.fp_vdisplay_end;
        break;
    case NV_PRAMDAC_FP_VCRTC:
        r = d->pramdac.fp_vcrtc;
        break;
    case NV_PRAMDAC_FP_VSYNC_END:
        r = d->pramdac.fp_vsync_end;
        break;
    case NV_PRAMDAC_FP_VVALID_END:
        r = d->pramdac.fp_vvalid_end;
        break;
    case NV_PRAMDAC_FP_HDISPLAY_END:
        r = d->pramdac.fp_hdisplay_end;
        break;
    case NV_PRAMDAC_FP_HCRTC:
        r = d->pramdac.fp_hcrtc;
        break;
    case NV_PRAMDAC_FP_HVALID_END:
        r = d->pramdac.fp_hvalid_end;
        break;
    default:
        break;
    }

    /* Handle unaligned access */
    r >>= 32 - 8 * size - 8 * (addr & 3);

    nv2a_reg_log_read(NV_PRAMDAC, addr, size, r);
    return r;
}

void pramdac_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;
    uint32_t m, n, p;

    nv2a_reg_log_write(NV_PRAMDAC, addr, size, val);

    switch (addr) {
    case NV_PRAMDAC_NVPLL_COEFF:
        d->pramdac.core_clock_coeff = val;

        m = val & NV_PRAMDAC_NVPLL_COEFF_MDIV;
        n = (val & NV_PRAMDAC_NVPLL_COEFF_NDIV) >> 8;
        p = (val & NV_PRAMDAC_NVPLL_COEFF_PDIV) >> 16;

        if (m == 0) {
            d->pramdac.core_clock_freq = 0;
        } else {
            d->pramdac.core_clock_freq = (NV2A_CRYSTAL_FREQ * n)
                                          / (1 << p) / m;
        }
        break;
    case NV_PRAMDAC_MPLL_COEFF:
        d->pramdac.memory_clock_coeff = val;
        break;
    case NV_PRAMDAC_VPLL_COEFF:
        d->pramdac.video_clock_coeff = val;
        break;
    case NV_PRAMDAC_GENERAL_CONTROL:
        d->pramdac.general_control = val;
        break;
    case NV_PRAMDAC_FP_VDISPLAY_END:
        d->pramdac.fp_vdisplay_end = val;
        break;
    case NV_PRAMDAC_FP_VCRTC:
        d->pramdac.fp_vcrtc = val;
        break;
    case NV_PRAMDAC_FP_VSYNC_END:
        d->pramdac.fp_vsync_end = val;
        break;
    case NV_PRAMDAC_FP_VVALID_END:
        d->pramdac.fp_vvalid_end = val;
        break;
    case NV_PRAMDAC_FP_HDISPLAY_END:
        d->pramdac.fp_hdisplay_end = val;
        break;
    case NV_PRAMDAC_FP_HCRTC:
        d->pramdac.fp_hcrtc = val;
        break;
    case NV_PRAMDAC_FP_HVALID_END:
        d->pramdac.fp_hvalid_end = val;
        break;
    default:
        break;
    }
}

/* PRMDIO palette access from original xemu prmdio.c (Matt Borgerson, 2021).
 * The existing puserdac state is also the checkpoint's palette authority. */
static uint64_t prmdio_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = opaque;
    if (size == 1 && addr == NV_USER_DAC_WRITE_MODE_ADDRESS)
        return d->puserdac.write_mode_address / 3;
    fprintf(stderr, "[STOP] unsupported PRMDIO read: offset=0x%llX size=%u\n", (unsigned long long)addr, size);
    exit(5);
}
static void prmdio_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = opaque;
    if (size == 1) {
        switch (addr) {
        case NV_USER_DAC_WRITE_MODE_ADDRESS:
            d->puserdac.write_mode_address = (val & 0xff) * 3;
            return;
        case NV_USER_DAC_PALETTE_DATA:
            d->puserdac.palette[d->puserdac.write_mode_address++ % (256*3)] = (uint8_t)val;
            return;
        }
    }
    fprintf(stderr, "[STOP] unsupported PRMDIO write: offset=0x%llX size=%u value=0x%llX\n", (unsigned long long)addr, size, (unsigned long long)val);
    exit(5);
}

/* ============================================================
 * PVIDEO - video overlay (stub)
 * ============================================================ */

uint64_t pvideo_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;
    uint64_t r = d->pvideo.regs[addr];
    nv2a_reg_log_read(NV_PVIDEO, addr, size, r);
    return r;
}

void pvideo_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;
    nv2a_reg_log_write(NV_PVIDEO, addr, size, val);
    d->pvideo.regs[addr] = val;
}

/* ============================================================
 * PGRAPH register interface
 * ============================================================ */

uint64_t pgraph_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;
    uint64_t r;
    switch (addr) {
    case NV_PGRAPH_INTR: r = d->pgraph.pending_interrupts; break;
    case NV_PGRAPH_INTR_EN: r = d->pgraph.enabled_interrupts; break;
    default: r = d->pgraph.regs[addr]; break;
    }
    nv2a_reg_log_read(NV_PGRAPH, addr, size, r);
    return r;
}

void pgraph_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;
    nv2a_reg_log_write(NV_PGRAPH, addr, size, val);
    qemu_mutex_lock(&d->pfifo.lock);
    switch (addr) {
    case NV_PGRAPH_INTR:
        d->pgraph.pending_interrupts &= ~(uint32_t)val;
        if (!(d->pgraph.pending_interrupts & NV_PGRAPH_INTR_ERROR))
            d->pgraph.waiting_for_nop = false;
        nv2a_update_irq(d);
        break;
    case NV_PGRAPH_INTR_EN:
        d->pgraph.enabled_interrupts = (uint32_t)val;
        nv2a_update_irq(d);
        break;
    default:
        d->pgraph.regs[addr] = val;
        break;
    }
    nv2a_fifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
}

/* ============================================================
 * PGRAPH method dispatch
 * Called when push buffer commands are parsed.
 * Routes method calls into PGRAPH register writes.
 * ============================================================ */

static void pgraph_stop(uint32_t subchannel, uint32_t graphics_class,
                         uint32_t method, uint32_t parameter)
{
    fprintf(stderr, "[STOP] PGRAPH unsupported class=0x%02X subchannel=%u method=0x%04X parameter=0x%08X\n",
            graphics_class, subchannel, method, parameter);
    exit(5);
}

static uint32_t pgraph_context_class(NV2AState *d,uint32_t instance)
{
    if(!instance)return NV_NULL;
    if((hwaddr)instance+16>memory_region_size(&d->ramin)) {
        fprintf(stderr,"[STOP] PGRAPH context outside RAMIN: 0x%08X\n",instance);
        exit(5);
    }
    return ldl_le_p(d->ramin_ptr+instance)&NV_PGRAPH_CTX_SWITCH1_GRCLASS;
}

/* Existing xemu nv_clip_gpu_tile_blit contract: an enabled inclusive tile
 * limit clips a blit span crossing it. Consecutive-tile behavior remains
 * outside this consumer's established contract. */
static hwaddr nv_clip_gpu_tile_blit(NV2AState *d,hwaddr start,hwaddr bytes)
{
    for(unsigned i=0;i<NV_NUM_GPU_TILES;i++) {
        if(!(d->pfb.regs[NV_PFB_TILE_BASE_ADDRESS_AND_FLAGS(i)]&NV_PFB_TILE_FLAGS_VALID))continue;
        hwaddr limit=d->pfb.regs[NV_PFB_TILE_LIMIT(i)];
        if(start<limit && start+bytes>limit)return limit+1-start;
    }
    return bytes;
}

/* NV09F consumes the existing surfaces object and DMA spans. Guest
 * memory access retains the original physical aliases and graphics ownership
 * fault handler, including publication of pending render targets. */
static void pgraph_image_blit_copy(NV2AState *d,uint32_t subchannel,uint32_t parameter)
{
    struct PGRAPHState *pg=&d->pgraph;
    int blend=pg->image_blit.operation==NV09F_SET_OPERATION_BLEND_AND;
    int rop=pg->image_blit.operation==NV09F_SET_OPERATION_ROP_AND;
    int copy_and=pg->image_blit.operation==NV09F_SET_OPERATION_SRCCOPY_AND;
    if((pg->image_blit.operation!=NV09F_SET_OPERATION_SRCCOPY && !blend && !rop && !copy_and) ||
       pg->image_blit.context_surfaces!=pg->context_surfaces_2d.object_instance ||
       pgraph_context_class(d,pg->image_blit.color_key)!=NV_NULL) {
        pgraph_stop(subchannel,NV_IMAGE_BLIT,NV09F_SIZE,parameter);return;
    }
    unsigned bpp;
    switch(pg->context_surfaces_2d.color_format) {
    case NV062_SET_COLOR_FORMAT_LE_Y8:bpp=1;break;
    case NV062_SET_COLOR_FORMAT_LE_R5G6B5:bpp=2;break;
    case NV062_SET_COLOR_FORMAT_LE_X8R8G8B8_Z8R8G8B8:
    case NV062_SET_COLOR_FORMAT_LE_X8R8G8B8:
    case NV062_SET_COLOR_FORMAT_LE_A8R8G8B8:
    case NV062_SET_COLOR_FORMAT_LE_Y32:bpp=4;break;
    default:pgraph_stop(subchannel,NV_IMAGE_BLIT,NV09F_SIZE,parameter);return;
    }
    if(blend && (bpp!=4 || pgraph_context_class(d,pg->image_blit.beta)!=NV_BETA ||
                  pg->image_blit.beta!=pg->beta.object_instance)) {
        pgraph_stop(subchannel,NV_IMAGE_BLIT,NV09F_SIZE,parameter);return;
    }
    /* SRCCOPY_AND discards zero-alpha source pixels. X8 conversion makes
     * alpha opaque, so this established subset uses the original copy path. */
    if(copy_and && (pg->context_surfaces_2d.color_format!=NV062_SET_COLOR_FORMAT_LE_X8R8G8B8 ||
                    pgraph_context_class(d,pg->image_blit.pattern)!=NV_NULL)) {
        pgraph_stop(subchannel,NV_IMAGE_BLIT,NV09F_SIZE,parameter);return;
    }
    unsigned rop3=pg->regs[NV_PGRAPH_ROP3]&0xff;
    if(rop && (pg->context_surfaces_2d.color_format!=NV062_SET_COLOR_FORMAT_LE_X8R8G8B8 ||
               pgraph_context_class(d,pg->image_blit.rop)!=NV_CONTEXT_ROP ||
               pgraph_context_class(d,pg->image_blit.pattern)!=NV_NULL ||
               (rop3>>4)!=(rop3&15))) {
        pgraph_stop(subchannel,NV_IMAGE_BLIT,NV09F_SIZE,parameter);return;
    }
    /* ROP3 indexes (P,S,D), while the existing GL logic enum numbers the
     * same sixteen P-independent functions in reverse truth-bit order. */
    unsigned nibble=rop3&15;
    unsigned logic=((nibble&1)<<3)|((nibble&2)<<1)|((nibble&4)>>1)|((nibble&8)>>3);
    unsigned width=pg->image_blit.width,height=pg->image_blit.height;
    unsigned in_x=pg->image_blit.in_x,in_y=pg->image_blit.in_y;
    int out_x=(int16_t)pg->image_blit.out_x,out_y=(int16_t)pg->image_blit.out_y;
    uint32_t clip_class=pgraph_context_class(d,pg->image_blit.clip);
    if(clip_class!=NV_NULL) {
        if(clip_class!=NV_CONTEXT_CLIP_RECTANGLE || pg->image_blit.clip!=pg->clip_rectangle.object_instance) {
            pgraph_stop(subchannel,NV_IMAGE_BLIT,NV09F_SIZE,parameter);return;
        }
        int left=(int16_t)pg->clip_rectangle.point,top=(int16_t)(pg->clip_rectangle.point>>16);
        int right=left+(int)(pg->clip_rectangle.size&0xffff),bottom=top+(int)(pg->clip_rectangle.size>>16);
        int x0=out_x>left?out_x:left,y0=out_y>top?out_y:top;
        int x1=out_x+(int)width<right?out_x+(int)width:right;
        int y1=out_y+(int)height<bottom?out_y+(int)height:bottom;
        if(x0>=x1 || y0>=y1)return;
        in_x+=(unsigned)(x0-out_x);in_y+=(unsigned)(y0-out_y);
        out_x=x0;out_y=y0;width=(unsigned)(x1-x0);height=(unsigned)(y1-y0);
    }
    if(out_x<0 || out_y<0) {pgraph_stop(subchannel,NV_IMAGE_BLIT,NV09F_SIZE,parameter);return;}
    unsigned row_pixels=pg->context_surfaces_2d.source_pitch<pg->context_surfaces_2d.dest_pitch?
        pg->context_surfaces_2d.source_pitch/bpp:pg->context_surfaces_2d.dest_pitch/bpp;
    if(width<row_pixels)row_pixels=width;
    if(!row_pixels)return;
    hwaddr source_offset=(hwaddr)pg->context_surfaces_2d.source_offset+
        (hwaddr)in_y*pg->context_surfaces_2d.source_pitch+(hwaddr)in_x*bpp;
    hwaddr destination_offset=(hwaddr)pg->context_surfaces_2d.dest_offset+
        (hwaddr)out_y*pg->context_surfaces_2d.dest_pitch+(hwaddr)out_x*bpp;
    size_t row_bytes=(size_t)row_pixels*bpp;
    hwaddr source_bytes=(hwaddr)(height-1)*pg->context_surfaces_2d.source_pitch+row_bytes;
    hwaddr destination_bytes=(hwaddr)(height-1)*pg->context_surfaces_2d.dest_pitch+row_bytes;
    uint8_t *destination=nv_dma_span(d,pg->context_surfaces_2d.dma_image_dest,destination_offset,1);
    hwaddr physical_destination=(hwaddr)(destination-d->vram_ptr);
    destination_bytes=nv_clip_gpu_tile_blit(d,physical_destination,destination_bytes);
    height=(unsigned)((destination_bytes-1)/pg->context_surfaces_2d.dest_pitch)+1;
    size_t last_row_bytes=(size_t)(destination_bytes-(hwaddr)(height-1)*pg->context_surfaces_2d.dest_pitch);
    if(last_row_bytes>row_bytes)last_row_bytes=row_bytes;
    source_bytes=(hwaddr)(height-1)*pg->context_surfaces_2d.source_pitch+last_row_bytes;
    uint8_t *source=nv_dma_span(d,pg->context_surfaces_2d.dma_image_source,source_offset,source_bytes);
    destination=nv_dma_span(d,pg->context_surfaces_2d.dma_image_dest,destination_offset,destination_bytes);
    /* Ordinary same-pitch linear copies choose their overlap direction in
     * hardware; compare the resolved RAM identities, not DMA handles. */
    int overlapping=source<destination+destination_bytes && destination<source+source_bytes;
    int linear_copy=overlapping && (!blend && !copy_and && (!rop || rop3==0xcc)) &&
        pg->context_surfaces_2d.source_pitch==pg->context_surfaces_2d.dest_pitch;
    hwaddr physical_source=(hwaddr)(source-d->vram_ptr);
    for(unsigned i=0;linear_copy && i<NV_NUM_GPU_TILES;i++) {
        uint32_t flags=d->pfb.regs[NV_PFB_TILE_BASE_ADDRESS_AND_FLAGS(i)];
        if(!(flags&NV_PFB_TILE_FLAGS_VALID))continue;
        hwaddr base=flags&NV_PFB_TILE_BASE_ADDRESS,limit=d->pfb.regs[NV_PFB_TILE_LIMIT(i)];
        if((physical_source<=limit && base<physical_source+source_bytes) ||
           (physical_destination<=limit && base<physical_destination+destination_bytes))linear_copy=0;
    }
    int reverse_rows=linear_copy && destination>source;
    /* Identical pixel mappings have no inter-pixel ordering dependency. */
    int exact_self=source==destination && pg->context_surfaces_2d.source_pitch==pg->context_surfaces_2d.dest_pitch;
    if((rop || copy_and) && ((last_row_bytes&3) ||
       (!exact_self && !linear_copy && overlapping))) {
        pgraph_stop(subchannel,NV_IMAGE_BLIT,NV09F_SIZE,parameter);return;
    }
    /* Keep original per-row memmove; retire lower destination rows first. */
    for(unsigned step=0;step<height;step++) {
        unsigned row=reverse_rows?height-1-step:step;
        uint8_t *src=source+(size_t)row*pg->context_surfaces_2d.source_pitch;
        uint8_t *dst=destination+(size_t)row*pg->context_surfaces_2d.dest_pitch;
        size_t copy_bytes=row+1==height?last_row_bytes:row_bytes;
        if(rop && !linear_copy) {
            for(size_t x=0;x<copy_bytes;x+=4)
                stl_le_p(dst+x,color_logic(logic,ldl_le_p(src+x),ldl_le_p(dst+x)));
        } else if(!blend)memmove(dst,src,copy_bytes);
        else {
            unsigned weight=pg->beta.value>>23;
            for(unsigned x=0;x<copy_bytes/bpp;x++)for(unsigned channel=0;channel<3;channel++)
                dst[x*4+channel]=(uint8_t)((src[x*4+channel]*weight+dst[x*4+channel]*(255-weight))/255);
        }
    }
    unsigned format=pg->context_surfaces_2d.color_format;
    if(format==NV062_SET_COLOR_FORMAT_LE_X8R8G8B8 || format==NV062_SET_COLOR_FORMAT_LE_X8R8G8B8_Z8R8G8B8)
        for(unsigned row=0;row<height;row++)for(unsigned x=0;x<(row+1==height?last_row_bytes:row_bytes)/bpp;x++)
            destination[(size_t)row*pg->context_surfaces_2d.dest_pitch+x*4+3]=(uint8_t)(format==NV062_SET_COLOR_FORMAT_LE_X8R8G8B8?255:0);
}

static void pgraph_m2mf_write_record(NV2AState *d,uint8_t *notification)
{
    uint64_t timestamp=ptimer_get_clock(d)<<5;
    stl_le_p(notification,(uint32_t)timestamp);
    stl_le_p(notification+4,(uint32_t)(timestamp>>32));
    stl_le_p(notification+8,0);
    /* The completion word publishes preceding RAM and record writes. */
    InterlockedExchange((volatile LONG *)(notification+12),0);
}

/* Class0039 format101 transfer, as submitted by the original NV04 M2MF
 * producer. Other increments, awaken and overlapping layouts need their own
 * established operational contract and retain an explicit stop. */
static void pgraph_m2mf_copy(NV2AState *d,uint32_t subchannel,uint32_t parameter)
{
    struct PGRAPHState *pg=&d->pgraph;
    if(parameter!=0 || pg->memory_to_memory.valid!=0x3ff ||
       pg->memory_to_memory.format!=0x101 ||
       !pg->memory_to_memory.line_count || !pg->memory_to_memory.line_length ||
       !pg->memory_to_memory.pitch_in || !pg->memory_to_memory.pitch_out ||
       pg->memory_to_memory.line_length>pg->memory_to_memory.pitch_in ||
       pg->memory_to_memory.line_length>pg->memory_to_memory.pitch_out)
        pgraph_stop(subchannel,NV_MEMORY_TO_MEMORY_FORMAT,0x328,parameter);
    hwaddr rows=pg->memory_to_memory.line_count,bytes=pg->memory_to_memory.line_length;
    hwaddr source_bytes=(rows-1)*pg->memory_to_memory.pitch_in+bytes;
    hwaddr destination_bytes=(rows-1)*pg->memory_to_memory.pitch_out+bytes;
    uint8_t *source=nv_dma_span(d,pg->memory_to_memory.dma_in,pg->memory_to_memory.offset_in,source_bytes);
    uint8_t *destination=nv_dma_span(d,pg->memory_to_memory.dma_out,pg->memory_to_memory.offset_out,destination_bytes);
    uint8_t *notification=nv_dma_span(d,pg->memory_to_memory.dma_notifies,0x10,16);
    if(((uintptr_t)notification&3) ||
       (source<destination+destination_bytes && destination<source+source_bytes))
        pgraph_stop(subchannel,NV_MEMORY_TO_MEMORY_FORMAT,0x328,parameter);
    for(hwaddr row=0;row<rows;row++)
        memcpy(destination+row*pg->memory_to_memory.pitch_out,source+row*pg->memory_to_memory.pitch_in,(size_t)bytes);
    pgraph_m2mf_write_record(d,notification);
}

static void pgraph_class_method(NV2AState *d, uint32_t subchannel,
                                uint32_t method, uint32_t param,
                                uint32_t graphics_class);

static uint32_t pgraph_publish_context(NV2AState *d, uint32_t subchannel)
{
    struct PGRAPHState *pg = &d->pgraph;
    const uint32_t *context = &pg->regs[NV_PGRAPH_CTX_CACHE1 + subchannel * 4];
    pg->regs[NV_PGRAPH_CTX_SWITCH1] = context[0x00];
    pg->regs[NV_PGRAPH_CTX_SWITCH2] = context[0x20];
    pg->regs[NV_PGRAPH_CTX_SWITCH3] = context[0x40];
    pg->regs[NV_PGRAPH_CTX_SWITCH4] = context[0x60];
    pg->regs[NV_PGRAPH_CTX_SWITCH5] = context[0x80];
    return pg->regs[NV_PGRAPH_CTX_SWITCH1] & NV_PGRAPH_CTX_SWITCH1_GRCLASS;
}

void pgraph_method(NV2AState *d, uint32_t subchannel,
                   uint32_t method, uint32_t param)
{
    if (subchannel >= 8)
        pgraph_stop(subchannel, 0, method, param);
    struct PGRAPHState *pg=&d->pgraph;
    int ordinary_notify=pg->memory_to_memory.notify_pending;
    if(ordinary_notify &&
       (subchannel!=pg->memory_to_memory.notify_subchannel || method==NV_SET_OBJECT ||
        method==0x104 || method==0x180 ||
        pg->regs[NV_PGRAPH_CTX_CACHE5+subchannel*4]!=pg->memory_to_memory.notify_instance))
        pgraph_stop(subchannel,NV_MEMORY_TO_MEMORY_FORMAT,method,param);
    if (method == NV_SET_OBJECT) {
        pgraph_class_method(d, subchannel, method, param, 0);
        return;
    }
    uint32_t graphics_class = pgraph_publish_context(d, subchannel);
    if (graphics_class == NV_KELVIN_PRIMITIVE &&
        ((method >= NV097_SET_TRANSFORM_PROGRAM && method < NV097_SET_TRANSFORM_CONSTANT + 128) ||
         (method >= NV097_ARRAY_ELEMENT16 && method < NV097_SET_TEXTURE_OFFSET + 256 &&
          !(method >= NV097_SET_EYE_VECTOR && method < NV097_SET_EYE_VECTOR+12)))) {
        if (d->pfifo.method_consumer)
            d->pfifo.method_consumer(subchannel, method, param, NULL);
        else
            pgraph_stop(subchannel, graphics_class, method, param);
        return;
    }
    pgraph_class_method(d, subchannel, method, param, graphics_class);
    if(ordinary_notify) {
        uint8_t *record=nv_dma_span(d,pg->memory_to_memory.dma_notifies,0,16);
        pgraph_m2mf_write_record(d,record);
        uint32_t awaken=pg->memory_to_memory.notify_type;
        pg->memory_to_memory.notify_pending=0;
        if(awaken) {
            pg->pending_interrupts|=NV_PGRAPH_INTR_NOTIFY;
            nv2a_update_irq(d);
        }
    }
}

static void pgraph_class_method(NV2AState *d, uint32_t subchannel,
                                uint32_t method, uint32_t param,
                                uint32_t graphics_class)
{
    struct PGRAPHState *pg = &d->pgraph;
    /* Binding fills the original cache before its class can be selected. */
    if (method == NV_SET_OBJECT) {
        if ((hwaddr)param + 16 > memory_region_size(&d->ramin))
            pgraph_stop(subchannel, 0, method, param);
        pg->regs[NV_PGRAPH_CTX_CACHE1 + subchannel * 4] = ldl_le_p(d->ramin_ptr + param);
        pg->regs[NV_PGRAPH_CTX_CACHE2 + subchannel * 4] = ldl_le_p(d->ramin_ptr + param + 4);
        pg->regs[NV_PGRAPH_CTX_CACHE3 + subchannel * 4] = ldl_le_p(d->ramin_ptr + param + 8);
        pg->regs[NV_PGRAPH_CTX_CACHE4 + subchannel * 4] = ldl_le_p(d->ramin_ptr + param + 12);
        pg->regs[NV_PGRAPH_CTX_CACHE5 + subchannel * 4] = param;
        uint32_t bound_class = pgraph_publish_context(d, subchannel);
        if (bound_class == NV_CONTEXT_SURFACES_2D)
            pg->context_surfaces_2d.object_instance = param;
        else if(bound_class == NV_CONTEXT_CLIP_RECTANGLE)pg->clip_rectangle.object_instance=param;
        else if(bound_class == NV_BETA)pg->beta.object_instance=param;
        return;
    }
    switch (graphics_class) {
    case NV_CONTEXT_ROP:
        if(method==NV043_SET_ROP5){pg->regs[NV_PGRAPH_ROP3]=param&0xff;return;}
        break;
    case NV_BETA:
        if(method==NV012_SET_BETA) {pg->beta.value=(param&0x80000000u)?0:param&0x7f800000u;return;}
        break;
    case NV_CONTEXT_CLIP_RECTANGLE:
        if(method==NV019_SET_POINT) {pg->clip_rectangle.point=param;return;}
        if(method==NV019_SET_SIZE) {pg->clip_rectangle.size=param;return;}
        break;
    case NV_MEMORY_TO_MEMORY_FORMAT:
        /* NVIDIA cl0039.h: contexts name real DMA instances. */
        switch (method) {
        case 0x100: return; /* NO_OPERATION is defined as ignored. */
        case 0x104: {
            if(param>1 || !(pg->memory_to_memory.valid&1))
                pgraph_stop(subchannel,graphics_class,method,param);
            uint8_t *record=nv_dma_span(d,pg->memory_to_memory.dma_notifies,0,16);
            if((uintptr_t)record&3)pgraph_stop(subchannel,graphics_class,method,param);
            pg->memory_to_memory.notify_pending=1;pg->memory_to_memory.notify_type=param;
            pg->memory_to_memory.notify_subchannel=subchannel;
            pg->memory_to_memory.notify_instance=pg->regs[NV_PGRAPH_CTX_CACHE5+subchannel*4];
            return;
        }
        case 0x180: pg->memory_to_memory.dma_notifies = param; pg->memory_to_memory.valid|=1; return;
        case 0x184: pg->memory_to_memory.dma_in = param; pg->memory_to_memory.valid|=2; return;
        case 0x188: pg->memory_to_memory.dma_out = param; pg->memory_to_memory.valid|=4; return;
        case 0x30c: pg->memory_to_memory.offset_in=param;pg->memory_to_memory.valid|=8;return;
        case 0x310: pg->memory_to_memory.offset_out=param;pg->memory_to_memory.valid|=16;return;
        case 0x314: case 0x318:
            if(param>0x7fff)pgraph_stop(subchannel,graphics_class,method,param);
            if(method==0x314){pg->memory_to_memory.pitch_in=param;pg->memory_to_memory.valid|=32;}
            else {pg->memory_to_memory.pitch_out=param;pg->memory_to_memory.valid|=64;}return;
        case 0x31c:
            if(param>=0x400000)pgraph_stop(subchannel,graphics_class,method,param);
            pg->memory_to_memory.line_length=param;pg->memory_to_memory.valid|=128;return;
        case 0x320:
            if(param>=0x800)pgraph_stop(subchannel,graphics_class,method,param);
            pg->memory_to_memory.line_count=param;pg->memory_to_memory.valid|=256;return;
        case 0x324: {
            uint32_t input=param&7,output=(param>>8)&7;
            if((param&~0x7ffu) || (param&0xf8) ||
               (input!=1 && input!=2 && input!=4) ||
               (output!=1 && output!=2 && output!=4))
                pgraph_stop(subchannel,graphics_class,method,param);
            pg->memory_to_memory.format=param;pg->memory_to_memory.valid|=512;return;
        }
        case 0x328:pgraph_m2mf_copy(d,subchannel,param);return;
        default: break;
        }
        break;
    case NV_CONTEXT_SURFACES_2D:
        /* Field handling from the original xemu class dispatcher. */
        switch (method) {
        case NV062_SET_CONTEXT_DMA_IMAGE_SOURCE: pg->context_surfaces_2d.dma_image_source = param; return;
        case NV062_SET_CONTEXT_DMA_IMAGE_DESTIN: pg->context_surfaces_2d.dma_image_dest = param; return;
        case NV062_SET_COLOR_FORMAT: pg->context_surfaces_2d.color_format = param; return;
        case NV062_SET_PITCH:
            pg->context_surfaces_2d.source_pitch = param & 0xFFFF;
            pg->context_surfaces_2d.dest_pitch = param >> 16;
            return;
        case NV062_SET_OFFSET_SOURCE: pg->context_surfaces_2d.source_offset = param & 0x07FFFFFF; return;
        case NV062_SET_OFFSET_DESTIN: pg->context_surfaces_2d.dest_offset = param & 0x07FFFFFF; return;
        default: break;
        }
        break;
    case NV_IMAGE_BLIT:
        /* Context slots follow envytools nv1_blit.xml; operational fields
         * follow the original xemu image-blit dispatcher. */
        switch (method) {
        case 0x180: pg->image_blit.dma_notifies = param; return;
        case 0x184: pg->image_blit.color_key = param; return;
        case 0x188: pg->image_blit.clip = param; return;
        case 0x18C: pg->image_blit.pattern = param; return;
        case 0x190: pg->image_blit.rop = param; return;
        case 0x194: pg->image_blit.beta = param; return;
        case 0x198: pg->image_blit.beta4 = param; return;
        case NV09F_SET_CONTEXT_SURFACES: pg->image_blit.context_surfaces = param; return;
        case NV09F_SET_OPERATION: pg->image_blit.operation = param; return;
        case NV09F_CONTROL_POINT_IN: pg->image_blit.in_x = param & 0xFFFF; pg->image_blit.in_y = param >> 16; return;
        case NV09F_CONTROL_POINT_OUT: pg->image_blit.out_x = param & 0xFFFF; pg->image_blit.out_y = param >> 16; return;
        case NV09F_SIZE:
            pg->image_blit.width = param & 0xFFFF; pg->image_blit.height = param >> 16;
            if (!pg->image_blit.width || !pg->image_blit.height) return;
            pgraph_image_blit_copy(d,subchannel,param);
            return;
        default: break;
        }
        break;
    case NV_KELVIN_PRIMITIVE:
        /* Original LT context products arrive independently of D3D8 API state. */
        if(method>=NV097_SET_POLYGON_STIPPLE_PATTERN && method<NV097_SET_POLYGON_STIPPLE_PATTERN+128) {
            pg->polygon_stipple[(method-NV097_SET_POLYGON_STIPPLE_PATTERN)/4]=param;return;
        }
        if (method >= NV097_SET_SCENE_AMBIENT_COLOR && method < NV097_SET_SCENE_AMBIENT_COLOR + 12) {
            pg->ltctxa[NV_IGRAPH_XF_LTCTXA_FR_AMB][(method-NV097_SET_SCENE_AMBIENT_COLOR)/4]=param; return;
        }
        if (method >= NV097_SET_MATERIAL_EMISSION && method < NV097_SET_MATERIAL_EMISSION + 12) {
            pg->ltctxa[NV_IGRAPH_XF_LTCTXA_CM_COL][(method-NV097_SET_MATERIAL_EMISSION)/4]=param; return;
        }
        if (method >= NV097_SET_BACK_SCENE_AMBIENT_COLOR && method < NV097_SET_BACK_SCENE_AMBIENT_COLOR + 12) {
            pg->ltctxa[NV_IGRAPH_XF_LTCTXA_BR_AMB][(method-NV097_SET_BACK_SCENE_AMBIENT_COLOR)/4]=param; return;
        }
        if (method >= NV097_SET_BACK_MATERIAL_EMISSION && method < NV097_SET_BACK_MATERIAL_EMISSION + 12) {
            pg->ltctxa[NV_IGRAPH_XF_LTCTXA_BCM_COL][(method-NV097_SET_BACK_MATERIAL_EMISSION)/4]=param; return;
        }
        if (method >= NV097_SET_SPECULAR_PARAMS_BACK && method < NV097_SET_SPECULAR_PARAMS_BACK + 24) {
            memcpy(&pg->back_specular_params[(method-NV097_SET_SPECULAR_PARAMS_BACK)/4],&param,4); return;
        }
        if (method >= NV097_SET_BACK_LIGHT_AMBIENT_COLOR && method < NV097_SET_BACK_LIGHT_AMBIENT_COLOR + 8*64) {
            unsigned light=(method-NV097_SET_BACK_LIGHT_AMBIENT_COLOR)/64;
            unsigned part=(method-NV097_SET_BACK_LIGHT_AMBIENT_COLOR)%64;
            if (part >= 36) pgraph_stop(subchannel,graphics_class,method,param);
            pg->ltctxb[NV_IGRAPH_XF_LTCTXB_L0_BAMB+light*6+part/12][(part%12)/4]=param; return;
        }
        if (method >= NV097_SET_SPECULAR_PARAMS && method < NV097_SET_SPECULAR_PARAMS + 24) {
            memcpy(&pg->specular_params[(method-NV097_SET_SPECULAR_PARAMS)/4],&param,4); return;
        }
        if (method >= NV097_SET_LIGHT_AMBIENT_COLOR && method < NV097_SET_LIGHT_AMBIENT_COLOR + 8*128) {
            unsigned light=(method-NV097_SET_LIGHT_AMBIENT_COLOR)/128;
            unsigned part=(method-NV097_SET_LIGHT_AMBIENT_COLOR)%128;
            if (part < 36) pg->ltctxb[NV_IGRAPH_XF_LTCTXB_L0_AMB+light*6+part/12][(part%12)/4]=param;
            else if (part == 36) pg->ltc1[NV_IGRAPH_XF_LTC1_r0+light][0]=param;
            else if (part < 48) memcpy(&pg->light_infinite_half_vector[light][(part-40)/4],&param,4);
            else if (part < 64) {
                if (part < 52) memcpy(&pg->light_infinite_half_vector[light][2],&param,4);
                else memcpy(&pg->light_infinite_direction[light][(part-52)/4],&param,4);
            }
            else if (part < 76) pg->ltctxa[NV_IGRAPH_XF_LTCTXA_L0_K+light*2][(part-64)/4]=param;
            else if (part < 92) pg->ltctxa[NV_IGRAPH_XF_LTCTXA_L0_SPT+light*2][(part-76)/4]=param;
            else if (part < 104) memcpy(&pg->light_local_position[light][(part-92)/4],&param,4);
            else if (part < 116) memcpy(&pg->light_local_attenuation[light][(part-104)/4],&param,4);
            else pgraph_stop(subchannel,graphics_class,method,param);
            return;
        }
        /* Consume the existing GPU's DMA identities, before pixels arrive. */
        switch (method) {
        case NV097_SET_CONTEXT_DMA_NOTIFIES: pg->dma_notifies = param; return;
        case NV097_SET_CONTEXT_DMA_A: pg->dma_a = param; return;
        case NV097_SET_CONTEXT_DMA_B: pg->dma_b = param; return;
        case NV097_SET_CONTEXT_DMA_STATE: pg->dma_state = param; return;
        case NV097_SET_CONTEXT_DMA_COLOR: pg->dma_color = param; return;
        case NV097_SET_CONTEXT_DMA_ZETA: pg->dma_zeta = param; return;
        case NV097_SET_CONTEXT_DMA_VERTEX_A: pg->dma_vertex_a = param; return;
        case NV097_SET_CONTEXT_DMA_VERTEX_B: pg->dma_vertex_b = param; return;
        case NV097_SET_CONTEXT_DMA_SEMAPHORE: pg->dma_semaphore = param; return;
        case NV097_SET_CONTEXT_DMA_REPORT: pg->dma_report = param; return;
        case NV097_NO_OPERATION:
            if (!param) return;
            if (pg->pending_interrupts & NV_PGRAPH_INTR_ERROR)
                pgraph_stop(subchannel, graphics_class, method, param);
            SET_MASK(pg->regs[NV_PGRAPH_TRAPPED_ADDR], NV_PGRAPH_TRAPPED_ADDR_CHID,
                     GET_MASK(pg->regs[NV_PGRAPH_CTX_USER], NV_PGRAPH_CTX_USER_CHID));
            SET_MASK(pg->regs[NV_PGRAPH_TRAPPED_ADDR], NV_PGRAPH_TRAPPED_ADDR_SUBCH, subchannel);
            SET_MASK(pg->regs[NV_PGRAPH_TRAPPED_ADDR], NV_PGRAPH_TRAPPED_ADDR_MTHD, method);
            pg->regs[NV_PGRAPH_TRAPPED_DATA_LOW] = param;
            pg->regs[NV_PGRAPH_NSOURCE] = NV_PGRAPH_NSOURCE_NOTIFICATION;
            pg->pending_interrupts |= NV_PGRAPH_INTR_ERROR;
            pg->waiting_for_nop = true;
            nv2a_update_irq(d);
            return;
        case NV097_SET_CLIP_MIN: pg->regs[NV_PGRAPH_ZCLIPMIN] = param; return;
        case NV097_SET_CLIP_MAX: pg->regs[NV_PGRAPH_ZCLIPMAX] = param; return;
        case NV097_SET_CONTROL0:
            SET_MASK(pg->regs[NV_PGRAPH_CONTROL_0], NV_PGRAPH_CONTROL_0_STENCIL_WRITE_ENABLE,
                     (param & NV097_SET_CONTROL0_STENCIL_WRITE_ENABLE) != 0);
            SET_MASK(pg->regs[NV_PGRAPH_SETUPRASTER], NV_PGRAPH_SETUPRASTER_Z_FORMAT,
                     (param & NV097_SET_CONTROL0_Z_FORMAT) != 0);
            SET_MASK(pg->regs[NV_PGRAPH_CONTROL_0], NV_PGRAPH_CONTROL_0_Z_PERSPECTIVE_ENABLE,
                     (param & NV097_SET_CONTROL0_Z_PERSPECTIVE_ENABLE) != 0);
            return;
        case NV097_SET_WINDOW_CLIP_TYPE:
            if (param > 1) pgraph_stop(subchannel, graphics_class, method, param);
            SET_MASK(pg->regs[NV_PGRAPH_SETUPRASTER], NV_PGRAPH_SETUPRASTER_WINDOWCLIPTYPE, param);
            return;
        case NV097_SET_ZMIN_MAX_CONTROL:
            if (GET_MASK(param, NV097_SET_ZMIN_MAX_CONTROL_ZCLAMP_EN) > 1)
                pgraph_stop(subchannel, graphics_class, method, param);
            SET_MASK(pg->regs[NV_PGRAPH_ZCOMPRESSOCCLUDE], NV_PGRAPH_ZCOMPRESSOCCLUDE_ZCLAMP_EN,
                     GET_MASK(param, NV097_SET_ZMIN_MAX_CONTROL_ZCLAMP_EN));
            return;
        case NV097_SET_CULL_FACE_ENABLE:
            if (param > 1) pgraph_stop(subchannel, graphics_class, method, param);
            SET_MASK(pg->regs[NV_PGRAPH_SETUPRASTER], NV_PGRAPH_SETUPRASTER_CULLENABLE, param);
            return;
        case NV097_SET_CULL_FACE:
            if (param != NV097_SET_CULL_FACE_V_FRONT && param != NV097_SET_CULL_FACE_V_BACK &&
                param != NV097_SET_CULL_FACE_V_FRONT_AND_BACK)
                pgraph_stop(subchannel, graphics_class, method, param);
            SET_MASK(pg->regs[NV_PGRAPH_SETUPRASTER], NV_PGRAPH_SETUPRASTER_CULLCTRL,
                     param == NV097_SET_CULL_FACE_V_FRONT ? NV_PGRAPH_SETUPRASTER_CULLCTRL_FRONT :
                     param == NV097_SET_CULL_FACE_V_BACK ? NV_PGRAPH_SETUPRASTER_CULLCTRL_BACK :
                     NV_PGRAPH_SETUPRASTER_CULLCTRL_FRONT_AND_BACK);
            return;
        case NV097_SET_FRONT_FACE:
            if (param != NV097_SET_FRONT_FACE_V_CW && param != NV097_SET_FRONT_FACE_V_CCW)
                pgraph_stop(subchannel, graphics_class, method, param);
            SET_MASK(pg->regs[NV_PGRAPH_SETUPRASTER], NV_PGRAPH_SETUPRASTER_FRONTFACE,
                     param == NV097_SET_FRONT_FACE_V_CCW);
            return;
        case NV097_SET_FRONT_POLYGON_MODE: case NV097_SET_BACK_POLYGON_MODE:
            if (param != NV097_SET_FRONT_POLYGON_MODE_V_FILL &&
                param != NV097_SET_FRONT_POLYGON_MODE_V_LINE &&
                param != NV097_SET_FRONT_POLYGON_MODE_V_POINT)
                pgraph_stop(subchannel, graphics_class, method, param);
            SET_MASK(pg->regs[NV_PGRAPH_SETUPRASTER],
                     method == NV097_SET_FRONT_POLYGON_MODE ? NV_PGRAPH_SETUPRASTER_FRONTFACEMODE :
                                                            NV_PGRAPH_SETUPRASTER_BACKFACEMODE,
                     param == NV097_SET_FRONT_POLYGON_MODE_V_LINE ?
                     NV_PGRAPH_SETUPRASTER_FRONTFACEMODE_LINE : param == NV097_SET_FRONT_POLYGON_MODE_V_POINT ?
                     NV_PGRAPH_SETUPRASTER_FRONTFACEMODE_POINT : NV_PGRAPH_SETUPRASTER_FRONTFACEMODE_FILL);
            return;
        case NV097_SET_DITHER_ENABLE:
            if (param > 1) pgraph_stop(subchannel, graphics_class, method, param);
            SET_MASK(pg->regs[NV_PGRAPH_CONTROL_0], NV_PGRAPH_CONTROL_0_DITHERENABLE, param); return;
        /* Disabled raster features have no pixel effect. Active modes remain
         * explicit support boundaries, rather than silently drawing fill. */
        case NV097_SET_STENCIL_TEST_ENABLE:
            if (param > 1) pgraph_stop(subchannel, graphics_class, method, param);
            SET_MASK(pg->regs[NV_PGRAPH_CONTROL_1], NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE, param); return;
        case NV097_SET_SKIN_MODE:
            if (param > NV097_SET_SKIN_MODE_4) pgraph_stop(subchannel, graphics_class, method, param);
            SET_MASK(pg->regs[NV_PGRAPH_CSV0_D], NV_PGRAPH_CSV0_D_SKIN, param); return;
        case NV097_SET_FOG_GEN_MODE:
            if (param > NV097_SET_FOG_GEN_MODE_V_ABS_PLANAR &&
                param != NV097_SET_FOG_GEN_MODE_V_FOG_X)
                pgraph_stop(subchannel, graphics_class, method, param);
            SET_MASK(pg->regs[NV_PGRAPH_CSV0_D], NV_PGRAPH_CSV0_D_FOGGENMODE,
                     param == NV097_SET_FOG_GEN_MODE_V_FOG_X ?
                     NV_PGRAPH_CSV0_D_FOGGENMODE_FOG_X : param);
            return;
        case NV097_SET_NORMALIZATION_ENABLE:
            if (param > 1) pgraph_stop(subchannel, graphics_class, method, param);
            SET_MASK(pg->regs[NV_PGRAPH_CSV0_C], NV_PGRAPH_CSV0_C_NORMALIZATION_ENABLE, param); return;
        case NV097_SET_BACK_MATERIAL_ALPHA:
            memcpy(&pg->back_material_alpha,&param,4); return;
        case NV097_SET_MATERIAL_ALPHA:
            memcpy(&pg->material_alpha,&param,4); return;
        case NV097_SET_LIGHT_ENABLE_MASK:
            if (param & ~0xFFFFu) pgraph_stop(subchannel,graphics_class,method,param);
            for(unsigned light=0;light<8;light++)
                SET_MASK(pg->regs[NV_PGRAPH_CSV0_D],NV_PGRAPH_CSV0_D_LIGHT0<<(light*2),(param>>(light*2))&3u);
            return;
        case NV097_SET_LIGHT_TWO_SIDE_ENABLE:
            if (param > 1) pgraph_stop(subchannel, graphics_class, method, param);
            SET_MASK(pg->regs[NV_PGRAPH_SETUPRASTER], 1u << 24, param);
            SET_MASK(pg->regs[NV_PGRAPH_CSV0_C], NV_PGRAPH_CSV0_C_LIGHT_TWO_SIDE_ENABLE, param);
            return;
        case NV097_SET_LIGHT_CONTROL:
            SET_MASK(pg->regs[NV_PGRAPH_CSV0_C], NV_PGRAPH_CSV0_C_SEPARATE_SPECULAR,
                     !!(param & NV097_SET_LIGHT_CONTROL_SEPARATE_SPECULAR));
            SET_MASK(pg->regs[NV_PGRAPH_CSV0_C], NV_PGRAPH_CSV0_C_LOCALEYE,
                     !!(param & NV097_SET_LIGHT_CONTROL_LOCALEYE));
            SET_MASK(pg->regs[NV_PGRAPH_CSV0_C], NV_PGRAPH_CSV0_C_ALPHA_FROM_MATERIAL_SPECULAR,
                     !!(param & NV097_SET_LIGHT_CONTROL_ALPHA_FROM_MATERIAL_SPECULAR));
            return;
        case NV097_SET_COLOR_MATERIAL:
            if ((param & ~0xFFFFu) || (param & 3u) > 2 || ((param >> 2) & 3u) > 2 ||
                    ((param >> 4) & 3u) > 2 || ((param >> 6) & 3u) > 2 ||
                    ((param >> 8) & 3u) > 2 || ((param >> 10) & 3u) > 2 ||
                    ((param >> 12) & 3u) > 2 || ((param >> 14) & 3u) > 2)
                pgraph_stop(subchannel, graphics_class, method, param);
            SET_MASK(pg->regs[NV_PGRAPH_CSV0_C], NV_PGRAPH_CSV0_C_EMISSION, (param >> 0) & 3u);
            SET_MASK(pg->regs[NV_PGRAPH_CSV0_C], NV_PGRAPH_CSV0_C_AMBIENT, (param >> 2) & 3u);
            SET_MASK(pg->regs[NV_PGRAPH_CSV0_C], NV_PGRAPH_CSV0_C_DIFFUSE, (param >> 4) & 3u);
            SET_MASK(pg->regs[NV_PGRAPH_CSV0_C], NV_PGRAPH_CSV0_C_SPECULAR, (param >> 6) & 3u);
            pg->back_material_sources=(param>>8)&0xffu;
            return;
        case NV097_SET_LIGHTING_ENABLE:
            if (param > 1) pgraph_stop(subchannel, graphics_class, method, param);
            SET_MASK(pg->regs[NV_PGRAPH_CSV0_C], NV_PGRAPH_CSV0_C_LIGHTING, param); return;
        case NV097_SET_SPECULAR_ENABLE:
            if (param > 1) pgraph_stop(subchannel, graphics_class, method, param);
            SET_MASK(pg->regs[NV_PGRAPH_CSV0_C], NV_PGRAPH_CSV0_C_SPECULAR_ENABLE, param); return;
        case NV097_SET_STENCIL_MASK:
            SET_MASK(pg->regs[NV_PGRAPH_CONTROL_1], NV_PGRAPH_CONTROL_1_STENCIL_MASK_WRITE, param); return;
        case NV097_SET_STENCIL_FUNC:
            if (param < 0x200 || param > 0x207) pgraph_stop(subchannel, graphics_class, method, param);
            SET_MASK(pg->regs[NV_PGRAPH_CONTROL_1], NV_PGRAPH_CONTROL_1_STENCIL_FUNC, param & 0xF); return;
        case NV097_SET_STENCIL_FUNC_REF:
            SET_MASK(pg->regs[NV_PGRAPH_CONTROL_1], NV_PGRAPH_CONTROL_1_STENCIL_REF, param); return;
        case NV097_SET_STENCIL_FUNC_MASK:
            SET_MASK(pg->regs[NV_PGRAPH_CONTROL_1], NV_PGRAPH_CONTROL_1_STENCIL_MASK_READ, param); return;
        case NV097_SET_STENCIL_OP_FAIL: case NV097_SET_STENCIL_OP_ZFAIL: case NV097_SET_STENCIL_OP_ZPASS: {
            unsigned operation;
            switch (param) {
            case NV097_SET_STENCIL_OP_V_KEEP: operation=NV_PGRAPH_CONTROL_2_STENCIL_OP_V_KEEP; break;
            case NV097_SET_STENCIL_OP_V_ZERO: operation=NV_PGRAPH_CONTROL_2_STENCIL_OP_V_ZERO; break;
            case NV097_SET_STENCIL_OP_V_REPLACE: operation=NV_PGRAPH_CONTROL_2_STENCIL_OP_V_REPLACE; break;
            case NV097_SET_STENCIL_OP_V_INCRSAT: operation=NV_PGRAPH_CONTROL_2_STENCIL_OP_V_INCRSAT; break;
            case NV097_SET_STENCIL_OP_V_DECRSAT: operation=NV_PGRAPH_CONTROL_2_STENCIL_OP_V_DECRSAT; break;
            case NV097_SET_STENCIL_OP_V_INVERT: operation=NV_PGRAPH_CONTROL_2_STENCIL_OP_V_INVERT; break;
            case NV097_SET_STENCIL_OP_V_INCR: operation=NV_PGRAPH_CONTROL_2_STENCIL_OP_V_INCR; break;
            case NV097_SET_STENCIL_OP_V_DECR: operation=NV_PGRAPH_CONTROL_2_STENCIL_OP_V_DECR; break;
            default: pgraph_stop(subchannel, graphics_class, method, param); return;
            }
            SET_MASK(pg->regs[NV_PGRAPH_CONTROL_2],
                     method == NV097_SET_STENCIL_OP_FAIL ? NV_PGRAPH_CONTROL_2_STENCIL_OP_FAIL :
                     method == NV097_SET_STENCIL_OP_ZFAIL ? NV_PGRAPH_CONTROL_2_STENCIL_OP_ZFAIL :
                                                          NV_PGRAPH_CONTROL_2_STENCIL_OP_ZPASS,
                     operation);
            return;
        }
        case NV097_SET_POLYGON_OFFSET_SCALE_FACTOR: case NV097_SET_POLYGON_OFFSET_BIAS:
            pg->regs[method == NV097_SET_POLYGON_OFFSET_SCALE_FACTOR ? NV_PGRAPH_ZOFFSETFACTOR :
                                                                       NV_PGRAPH_ZOFFSETBIAS] = param;
            return;
        case NV097_SET_POINT_SMOOTH_ENABLE:
            if (param > 1) pgraph_stop(subchannel, graphics_class, method, param);
            SET_MASK(pg->regs[NV_PGRAPH_SETUPRASTER], NV_PGRAPH_SETUPRASTER_POINTSMOOTHENABLE, param);
            return;
        case NV097_SET_POLY_OFFSET_POINT_ENABLE: case NV097_SET_POLY_OFFSET_LINE_ENABLE: case NV097_SET_POLY_OFFSET_FILL_ENABLE:
            if (param > 1) pgraph_stop(subchannel, graphics_class, method, param);
            SET_MASK(pg->regs[NV_PGRAPH_SETUPRASTER],
                     method == NV097_SET_POLY_OFFSET_POINT_ENABLE ? NV_PGRAPH_SETUPRASTER_POFFSETPOINTENABLE :
                     method == NV097_SET_POLY_OFFSET_LINE_ENABLE ? NV_PGRAPH_SETUPRASTER_POFFSETLINEENABLE :
                                                                NV_PGRAPH_SETUPRASTER_POFFSETFILLENABLE, param);
            return;
        case NV097_SET_LINE_SMOOTH_ENABLE: case NV097_SET_POLY_SMOOTH_ENABLE:
            if (param > 1) pgraph_stop(subchannel, graphics_class, method, param);
            SET_MASK(pg->regs[NV_PGRAPH_SETUPRASTER],
                     method == NV097_SET_LINE_SMOOTH_ENABLE ? NV_PGRAPH_SETUPRASTER_LINESMOOTHENABLE :
                                                           NV_PGRAPH_SETUPRASTER_POLYSMOOTHENABLE, param);
            return;
        case NV097_SET_POINT_SIZE:
            if (param <= NV097_SET_POINT_SIZE_V_MAX)
                pg->regs[NV_PGRAPH_POINTSIZE] = param;
            return;
        case NV097_SET_POINT_PARAMS_ENABLE:
            if (param > 1) pgraph_stop(subchannel, graphics_class, method, param);
            SET_MASK(pg->regs[NV_PGRAPH_CSV0_D], NV_PGRAPH_CSV0_D_POINTPARAMSENABLE, param);
            SET_MASK(pg->regs[NV_PGRAPH_CONTROL_3], NV_PGRAPH_CONTROL_3_POINTPARAMSENABLE, param);
            return;
        case NV097_SET_EDGE_FLAG:
            if (param > 1) pgraph_stop(subchannel, graphics_class, method, param);
            /* Vertex submission captures this standing boundary identity. */
            pg->edge_flag = param;
            return;
        case NV097_SET_LINE_WIDTH:
            if (param > NV097_SET_LINE_WIDTH_MAX_VALUE) pgraph_stop(subchannel, graphics_class, method, param);
            SET_MASK(pg->regs[NV_PGRAPH_SETUPRASTER], NV_PGRAPH_SETUPRASTER_LINEWIDTH, param);
            return;
        case NV097_SET_ANTI_ALIASING_CONTROL:
            /* With a single-sample surface, control1 suppresses analytic
             * LINE/POLY smoothing. Other masks/flags have no established consumer. */
            if (param != 0 && param != 0xFFFF0000 && param != 0xFFFF0001) pgraph_stop(subchannel, graphics_class, method, param);
            SET_MASK(pg->regs[NV_PGRAPH_ANTIALIASING], NV_PGRAPH_ANTIALIASING_ENABLE,
                     (param & NV097_SET_ANTI_ALIASING_CONTROL_ENABLE)!=0);
            return;
        case NV097_SET_SWATH_WIDTH:
            /* Envytools' class-97 hardware method test maps the low nibble
             * into raster bits 25..27. This is state, not a draw command. */
            if (param > 4 && param != 0xF) pgraph_stop(subchannel, graphics_class, method, param);
            SET_MASK(pg->regs[NV_PGRAPH_SETUPRASTER], NV_PGRAPH_SETUPRASTER_SWATHWIDTH, param);
            return;
        case NV097_SET_OCCLUDE_ZSTENCIL_EN:
            if (param > 3) pgraph_stop(subchannel, graphics_class, method, param);
            SET_MASK(pg->regs[NV_PGRAPH_ZCOMPRESSOCCLUDE], NV_PGRAPH_ZCOMPRESSOCCLUDE_OCCLUDE_ENABLE, param);
            return;
        case NV097_SET_POLYGON_STIPPLE_ENABLE:
            if (param > 1) pgraph_stop(subchannel, graphics_class, method, param);
            SET_MASK(pg->regs[NV_PGRAPH_SETUPRASTER], NV_PGRAPH_SETUPRASTER_STIPPLEENABLE, param);
            return;
        case NV097_SET_LOGIC_OP_ENABLE:
            if (param > 1) pgraph_stop(subchannel, graphics_class, method, param);
            SET_MASK(pg->regs[NV_PGRAPH_BLEND], NV_PGRAPH_BLEND_LOGICOP_ENABLE, param); return;
        case NV097_SET_LOGIC_OP:
            if (param < 0x1500 || param > 0x150F) pgraph_stop(subchannel, graphics_class, method, param);
            SET_MASK(pg->regs[NV_PGRAPH_BLEND], NV_PGRAPH_BLEND_LOGICOP, param & 0xF); return;
        case NV097_SET_COMPRESS_ZBUFFER_EN:
            if (param > 1) pgraph_stop(subchannel, graphics_class, method, param);
            /* The original pixel backend keeps logical depth decoded. Its
             * surface cache retains this storage mode with the DMA identity. */
            SET_MASK(pg->regs[NV_PGRAPH_ZCOMPRESSOCCLUDE], NV_PGRAPH_ZCOMPRESSOCCLUDE_COMPRESS_ENABLE, param);
            return;
        case NV097_SET_EYE_VECTOR:
        case NV097_SET_EYE_VECTOR+4:
        case NV097_SET_EYE_VECTOR+8:
            pg->regs[NV_PGRAPH_EYEVEC0+(method-NV097_SET_EYE_VECTOR)]=param;
            return;
        case NV097_SET_DOT_RGBMAPPING:
            SET_MASK(pg->regs[NV_PGRAPH_SHADERCTL],0xFFF,GET_MASK(param,0xFFF));
            return;
        case NV097_SET_SHADER_OTHER_STAGE_INPUT:
            SET_MASK(pg->regs[NV_PGRAPH_SHADERCTL], 0xFFFF000, GET_MASK(param, 0xFFFF000));
            return;
        case NV097_SET_SHADOW_ZSLOPE_THRESHOLD:
            /* The connected texture consumer has no active shadow modes.
             * Infinity disables this threshold in the original dispatcher. */
            if (param != 0x7F800000) pgraph_stop(subchannel, graphics_class, method, param);
            pg->regs[NV_PGRAPH_SHADOWZSLOPETHRESHOLD] = param;
            return;
        case NV097_SET_SHADOW_DEPTH_FUNC:
            if (param > NV097_SET_SHADOW_DEPTH_FUNC_ALWAYS) pgraph_stop(subchannel, graphics_class, method, param);
            SET_MASK(pg->regs[NV_PGRAPH_SHADOWCTL], NV_PGRAPH_SHADOWCTL_SHADOW_ZFUNC, param);
            return;
        case NV097_SET_PROVOKING_VERTEX:
            if (param > 1) pgraph_stop(subchannel, graphics_class, method, param);
            SET_MASK(pg->regs[NV_PGRAPH_CONTROL_3], NV_PGRAPH_CONTROL_3_PROVOKING_VERTEX, param);
            return;
        case NV097_SET_SHADE_MODE:
            if (param != NV097_SET_SHADE_MODE_V_FLAT && param != NV097_SET_SHADE_MODE_V_SMOOTH)
                pgraph_stop(subchannel, graphics_class, method, param);
            SET_MASK(pg->regs[NV_PGRAPH_CONTROL_3], NV_PGRAPH_CONTROL_3_SHADEMODE,
                     param == NV097_SET_SHADE_MODE_V_SMOOTH);
            return;
        case NV097_SET_SEMAPHORE_OFFSET:
            pg->regs[NV_PGRAPH_SEMAPHOREOFFSET] = param;
            return;
        case NV097_SET_CLEAR_RECT_HORIZONTAL:
            pg->regs[NV_PGRAPH_CLEARRECTX] = param;
            return;
        case NV097_SET_CLEAR_RECT_VERTICAL:
            pg->regs[NV_PGRAPH_CLEARRECTY] = param;
            return;
        case NV097_BACK_END_WRITE_SEMAPHORE_RELEASE: {
            uint8_t *output=nv_dma_span(d,pg->dma_semaphore,pg->regs[NV_PGRAPH_SEMAPHOREOFFSET],4);
            if (!d->pfifo.method_consumer || !d->pfifo.method_consumer(subchannel,method,param,output))
                stl_le_p(output,param);
            return;
        }
        default: break;
        }
        if ((method >= NV097_SET_WINDOW_CLIP_HORIZONTAL && method < NV097_SET_WINDOW_CLIP_HORIZONTAL + 32) ||
            (method >= NV097_SET_WINDOW_CLIP_VERTICAL && method < NV097_SET_WINDOW_CLIP_VERTICAL + 32)) {
            uint32_t horizontal = method < NV097_SET_WINDOW_CLIP_VERTICAL;
            uint32_t first = horizontal ? NV097_SET_WINDOW_CLIP_HORIZONTAL : NV097_SET_WINDOW_CLIP_VERTICAL;
            uint32_t reg = horizontal ? NV_PGRAPH_WINDOWCLIPX0 : NV_PGRAPH_WINDOWCLIPY0;
            /* A write to slot i propagates to slots i..7 in the original GPU. */
            for (uint32_t slot = (method - first) / 4; slot < 8; ++slot)
                pg->regs[reg + slot * 4] = param;
            return;
        }
        if (method >= NV097_SET_COLOR_KEY_COLOR && method < NV097_SET_COLOR_KEY_COLOR + 16) {
            pg->regs[NV_PGRAPH_COLORKEYCOLOR0 + method - NV097_SET_COLOR_KEY_COLOR] = param;
            return;
        }
        if (method == NV097_SET_TEXGEN_VIEW_MODEL) {
            if (param > NV097_SET_TEXGEN_VIEW_MODEL_INFINITE_VIEWER) {
                pgraph_stop(subchannel,graphics_class,method,param);
            }
            SET_MASK(pg->regs[NV_PGRAPH_CSV0_D],NV_PGRAPH_CSV0_D_TEXGEN_REF,param);
            return;
        }
        if (method >= NV097_SET_TEXGEN_S && method < NV097_SET_TEXGEN_S + 64) {
            uint32_t slot = (method - NV097_SET_TEXGEN_S) / 16;
            uint32_t component = ((method - NV097_SET_TEXGEN_S) % 16) / 4;
            uint32_t mode;
            /* Original Kelvin method values map to the existing CSV1 fields.
             * The selected vertex consumer decides whether to generate them. */
            switch(param) {
            case NV097_SET_TEXGEN_S_DISABLE: mode=NV_PGRAPH_CSV1_A_T0_S_DISABLE;break;
            case NV097_SET_TEXGEN_S_EYE_LINEAR: mode=NV_PGRAPH_CSV1_A_T0_S_EYE_LINEAR;break;
            case NV097_SET_TEXGEN_S_OBJECT_LINEAR: mode=NV_PGRAPH_CSV1_A_T0_S_OBJECT_LINEAR;break;
            case NV097_SET_TEXGEN_S_SPHERE_MAP:
                if(component>=2) pgraph_stop(subchannel,graphics_class,method,param);
                mode=NV_PGRAPH_CSV1_A_T0_S_SPHERE_MAP;break;
            case NV097_SET_TEXGEN_S_REFLECTION_MAP:
                if(component>=3) pgraph_stop(subchannel,graphics_class,method,param);
                mode=NV_PGRAPH_CSV1_A_T0_S_REFLECTION_MAP;break;
            case NV097_SET_TEXGEN_S_NORMAL_MAP:
                if(component>=3) pgraph_stop(subchannel,graphics_class,method,param);
                mode=NV_PGRAPH_CSV1_A_T0_S_NORMAL_MAP;break;
            default:pgraph_stop(subchannel,graphics_class,method,param);return;
            }
            uint32_t mask = NV_PGRAPH_CSV1_A_T0_S << (component * 3 + (slot % 2) * 16);
            SET_MASK(pg->regs[slot < 2 ? NV_PGRAPH_CSV1_A : NV_PGRAPH_CSV1_B], mask, mode);
            return;
        }
        if (method >= NV097_SET_TEXTURE_MATRIX_ENABLE &&
            method < NV097_SET_TEXTURE_MATRIX_ENABLE + 16) {
            if (param>1) pgraph_stop(subchannel, graphics_class, method, param);
            pg->texture_matrix_enable[(method - NV097_SET_TEXTURE_MATRIX_ENABLE) / 4] = param!=0;
            return;
        }
        if (method >= NV097_SET_POINT_PARAMS &&
            method < NV097_SET_POINT_PARAMS + 8 * 4) {
            /* Original Kelvin attenuation/range parameters. Enabling their
             * unsupported point consumer remains a strict boundary. */
            memcpy(&pg->point_params[(method - NV097_SET_POINT_PARAMS) / 4],
                   &param, sizeof(param));
            return;
        }
        if (d->pfifo.method_consumer) {
            d->pfifo.method_consumer(subchannel, method, param, NULL);
            return;
        }
        break;
    default: break;
    }
    pgraph_stop(subchannel, graphics_class, method, param);
}

/* ============================================================
 * PFIFO - register interface to the connected DMA state machine
 * ============================================================ */

uint64_t pfifo_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    uint64_t r = 0;
    switch (addr) {
    case NV_PFIFO_INTR_0:
        r = d->pfifo.pending_interrupts;
        break;
    case NV_PFIFO_INTR_EN_0:
        r = d->pfifo.enabled_interrupts;
        break;
    case NV_PFIFO_RUNOUT_STATUS:
        r = NV_PFIFO_RUNOUT_STATUS_LOW_MARK;
        break;
    default:
        r = d->pfifo.regs[addr];
        break;
    }

    nv2a_reg_log_read(NV_PFIFO, addr, size, r);
    return r;
}

void pfifo_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    nv2a_reg_log_write(NV_PFIFO, addr, size, val);
    qemu_mutex_lock(&d->pfifo.lock);

    switch (addr) {
    case NV_PFIFO_INTR_0:
        d->pfifo.pending_interrupts &= ~val;
        nv2a_update_irq(d);
        break;
    case NV_PFIFO_INTR_EN_0:
        d->pfifo.enabled_interrupts = val;
        nv2a_update_irq(d);
        break;
    default:
        d->pfifo.regs[addr] = val;
        break;
    }
    nv2a_fifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
}

/* CRTC index/data behavior adapted from xemu hw/display/vga.c.
 * Copyright (c) 2003 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * The existing VGA cr array is authoritative. Unsupported addresses stop. */
uint64_t prmcio_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;
    if (size == 1 && addr == 0x3D4)
        return d->vga.cr_index;
    if (size == 1 && addr == 0x3D5)
        return d->vga.cr[d->vga.cr_index];
    fprintf(stderr, "[STOP] unsupported PRMCIO read: offset=0x%llX size=%u\n",
            (unsigned long long)addr, size);
    exit(5);
}

void prmcio_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;
    if (size == 1 && addr == 0x3D4) {
        d->vga.cr_index = (uint8_t)val;
        return;
    }
    if (size == 1 && addr == 0x3D5) {
        /* CR11 bit 7 protects CR0..7, except CR7 bit 4. */
        if ((d->vga.cr[0x11] & 0x80) && d->vga.cr_index <= 7) {
            if (d->vga.cr_index == 7)
                d->vga.cr[7] = (d->vga.cr[7] & ~0x10) | (val & 0x10);
            return;
        }
        d->vga.cr[d->vga.cr_index] = (uint8_t)val;
        return;
    }
    fprintf(stderr, "[STOP] unsupported PRMCIO write: offset=0x%llX size=%u value=0x%llX\n",
            (unsigned long long)addr, size, (unsigned long long)val);
    exit(5);
}

/* ============================================================
 * Stub handler for unimplemented blocks
 * ============================================================ */

uint64_t nv2a_stub_read(void *opaque, hwaddr addr, unsigned int size)
{
    (void)opaque; (void)size;
    NV2A_DPRINTF("stub read: addr=0x%llx size=%d\n",
                 (unsigned long long)addr, size);
    return 0;
}

void nv2a_stub_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    (void)opaque; (void)size;
    NV2A_DPRINTF("stub write: addr=0x%llx val=0x%llx size=%d\n",
                 (unsigned long long)addr, (unsigned long long)val, size);
}

/* ============================================================
 * Block dispatch table (from xemu nv2a.c)
 * ============================================================ */

#define ENTRY(NAME, LNAME, OFFSET, SIZE) [NV_##NAME] = { \
    .name   = #NAME,                                      \
    .offset = OFFSET,                                     \
    .size   = SIZE,                                       \
    .ops    = { .read = LNAME##_read, .write = LNAME##_write }, \
}
#define STUB_ENTRY(NAME, OFFSET, SIZE) [NV_##NAME] = { \
    .name   = #NAME,                                    \
    .offset = OFFSET,                                   \
    .size   = SIZE,                                     \
    .ops    = { .read = nv2a_stub_read, .write = nv2a_stub_write }, \
}

const NV2ABlockInfo blocktable[NV_NUM_BLOCKS] = {
    ENTRY(PMC,      pmc,      0x000000, 0x001000),
    ENTRY(PBUS,     pbus,     0x001000, 0x001000),
    ENTRY(PFIFO,    pfifo,    0x002000, 0x002000),
    STUB_ENTRY(PFIFO_CACHE,   0x003000, 0x001000),
    STUB_ENTRY(PRMA,          0x007000, 0x001000),
    ENTRY(PVIDEO,   pvideo,   0x008000, 0x001000),
    ENTRY(PTIMER,   ptimer,   0x009000, 0x001000),
    STUB_ENTRY(PCOUNTER,      0x00a000, 0x001000),
    STUB_ENTRY(PVPE,          0x00b000, 0x001000),
    STUB_ENTRY(PTV,           0x00d000, 0x001000),
    STUB_ENTRY(PRMFB,         0x0a0000, 0x020000),
    STUB_ENTRY(PRMVIO,        0x0c0000, 0x001000),
    ENTRY(PFB,      pfb,      0x100000, 0x001000),
    STUB_ENTRY(PSTRAPS,       0x101000, 0x001000),
    ENTRY(PGRAPH,   pgraph,   0x400000, 0x002000),
    ENTRY(PCRTC,    pcrtc,    0x600000, 0x001000),
    ENTRY(PRMCIO,   prmcio,   0x601000, 0x001000),
    ENTRY(PRAMDAC,  pramdac,  0x680000, 0x001000),
    ENTRY(PRMDIO,   prmdio,   0x681000, 0x001000),
    /* NV_PRAMIN = 19 */
    { .name = NULL },
    /* NV_USER = 20 */
    ENTRY(USER,     user,     0x800000, 0x800000),
};

#undef ENTRY
#undef STUB_ENTRY

/* ============================================================
 * MMIO dispatch (for VEH handler integration)
 * ============================================================ */

/* PFB register state is guest-CPU-owned; neither the parser nor renderer
 * reads it. Its write-combine flush does not wait for GPU completion.
 * USER owns synchronization for the shared FIFO producer/consumer indices. */

uint64_t nv2a_mmio_read(NV2AState *d, hwaddr addr, unsigned int size)
{
    /* Find which block handles this address */
    for (int i = 0; i < NV_NUM_BLOCKS; i++) {
        if (!blocktable[i].name) continue;
        if (addr >= blocktable[i].offset &&
            addr < blocktable[i].offset + blocktable[i].size) {
            hwaddr block_addr = addr - blocktable[i].offset;
            if (blocktable[i].ops.read == nv2a_stub_read) {
                fprintf(stderr, "[STOP] unsupported NV2A read: block=%s offset=0x%llX size=%u\n",
                        blocktable[i].name, (unsigned long long)addr, size);
                exit(5);
            }
            if (i != NV_USER && i != NV_PFB) qemu_mutex_lock(&d->pfifo.lock);
            uint64_t value = blocktable[i].ops.read(d, block_addr, size);
            if (i != NV_USER && i != NV_PFB) qemu_mutex_unlock(&d->pfifo.lock);
            return value;
        }
    }
    fprintf(stderr, "[STOP] unmapped NV2A read: offset=0x%llX size=%u\n",
            (unsigned long long)addr, size);
    exit(5);
}

void nv2a_mmio_write(NV2AState *d, hwaddr addr, uint64_t val, unsigned int size)
{
    for (int i = 0; i < NV_NUM_BLOCKS; i++) {
        if (!blocktable[i].name) continue;
        if (addr >= blocktable[i].offset &&
            addr < blocktable[i].offset + blocktable[i].size) {
            hwaddr block_addr = addr - blocktable[i].offset;
            if (blocktable[i].ops.write == nv2a_stub_write) {
                fprintf(stderr, "[STOP] unsupported NV2A write: block=%s offset=0x%llX size=%u value=0x%llX\n",
                        blocktable[i].name, (unsigned long long)addr, size,
                        (unsigned long long)val);
                exit(5);
            }
            if (i != NV_USER && i != NV_PFB) qemu_mutex_lock(&d->pfifo.lock);
            blocktable[i].ops.write(d, block_addr, val, size);
            if (i != NV_USER && i != NV_PFB) qemu_mutex_unlock(&d->pfifo.lock);
            return;
        }
    }
    fprintf(stderr, "[STOP] unmapped NV2A write: offset=0x%llX size=%u value=0x%llX\n",
            (unsigned long long)addr, size, (unsigned long long)val);
    exit(5);
}

/* ============================================================
 * Standalone initialization
 * ============================================================ */

NV2AState *nv2a_init_standalone(uint8_t *vram_ptr, uint32_t vram_size,
                                 uint8_t *ramin_ptr, uint32_t ramin_size)
{
    if (g_nv2a) return g_nv2a;

    NV2AState *d = (NV2AState *)calloc(1, sizeof(NV2AState));
    if (!d) return NULL;

    /* Set up VRAM */
    g_vram_region.size = vram_size;
    d->vram = &g_vram_region;
    d->vram_ptr = vram_ptr;
    d->vram_pci.size = vram_size;

    /* Set up RAMIN */
    g_ramin_region.size = ramin_size;
    d->ramin.size = ramin_size;
    d->ramin_ptr = ramin_ptr;

    /* PCI config space: NV2A vendor/device */
    pci_set_long(d->parent_obj.config + PCI_VENDOR_ID, 0x02A010DE); /* NVIDIA NV2A */
    pci_set_long(d->parent_obj.config + PCI_CLASS_REVISION, 0x030000A1);

    /* Default PLL: 233 MHz core clock (Xbox default) */
    d->pramdac.core_clock_coeff = 0x00011C01; /* n=0x1C, m=1, p=0 */
    d->pramdac.core_clock_freq = NV2A_CRYSTAL_FREQ * 0x1C; /* ~233 MHz */

    d->pgraph.edge_flag = 1;

    /* Default timer divisors */
    d->ptimer.numerator = 1;
    d->ptimer.denominator = 1;

    /* xemu initializes the method cache empty before the first DMA word. */
    d->pfifo.regs[NV_PFIFO_CACHE1_STATUS] = NV_PFIFO_CACHE1_STATUS_LOW_MARK;

    /* Initialize PFIFO mutex */
    qemu_mutex_init(&d->pfifo.lock);
    qemu_cond_init(&d->pfifo.fifo_cond);
    qemu_cond_init(&d->pfifo.fifo_idle_cond);

    d->vblank_frame = muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), 60, 1000000000);
    g_nv2a = d;

    fprintf(stderr, "[NV2A] Standalone GPU initialized: VRAM=%uMB RAMIN=%uKB\n",
            vram_size / (1024*1024), ramin_size / 1024);

    return d;
}
typedef struct Nv2aCheckpoint { uint32_t state_bytes;uint64_t clock;NV2AState state; } Nv2aCheckpoint;
extern int xbox_CheckpointFrozen(void);
int nv2a_save_checkpoint(FILE *out) {
    if(!out || !g_nv2a || !xbox_CheckpointFrozen()) return 0;
    Nv2aCheckpoint *saved=calloc(1,sizeof *saved);if(!saved) return 0;
    NV2AState *d=g_nv2a;qemu_mutex_lock(&d->pfifo.lock);
    saved->state_bytes=sizeof saved->state;saved->clock=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)+g_ptimer_clock_bias;memcpy(&saved->state,d,sizeof *d);
    NV2AState *s=&saved->state;s->parent_obj.irq_notify=NULL;s->parent_obj.irq_opaque=NULL;
    s->vga.vram_ptr=NULL;memset(&s->hw_ops,0,sizeof s->hw_ops);
    s->vram=NULL;s->vram_ptr=NULL;s->ramin_ptr=NULL;s->vram_pci.opaque=s->ramin.opaque=s->mmio.opaque=NULL;
    for(unsigned i=0;i<NV_NUM_BLOCKS;i++) s->block_mmio[i].opaque=NULL;
    memset(&s->pfifo.lock,0,sizeof s->pfifo.lock);memset(&s->pfifo.thread,0,sizeof s->pfifo.thread);
    memset(&s->pfifo.fifo_cond,0,sizeof s->pfifo.fifo_cond);memset(&s->pfifo.fifo_idle_cond,0,sizeof s->pfifo.fifo_idle_cond);
    s->pfifo.method_consumer=NULL;s->pfifo.fifo_kick=0;
    int ok=fwrite("XBNV2A",6,1,out)==1 && fwrite(saved,sizeof *saved,1,out)==1;
    qemu_mutex_unlock(&d->pfifo.lock);free(saved);return ok;
}
void *nv2a_prepare_checkpoint(FILE *in) {
    if(!in || !g_nv2a || !xbox_CheckpointFrozen()) return NULL;
    char magic[6];Nv2aCheckpoint *saved=malloc(sizeof *saved);if(!saved) return NULL;
    int ok=fread(magic,6,1,in)==1 && !memcmp(magic,"XBNV2A",6) && fread(saved,sizeof *saved,1,in)==1 &&
        saved->state_bytes==sizeof saved->state && saved->state.vram_pci.size==g_nv2a->vram_pci.size &&
        saved->state.ramin.size==g_nv2a->ramin.size && !saved->state.vram && !saved->state.vram_ptr && !saved->state.ramin_ptr;
    if(!ok) {free(saved);return NULL;}return saved;
}
void nv2a_apply_checkpoint(const void *prepared) {
    const Nv2aCheckpoint *saved=prepared;const NV2AState *s=&saved->state;NV2AState *d=g_nv2a;
    qemu_mutex_lock(&d->pfifo.lock);
    d->parent_obj.devfn=s->parent_obj.devfn;d->parent_obj.irq_level=s->parent_obj.irq_level;
    memcpy(d->parent_obj.config,s->parent_obj.config,sizeof d->parent_obj.config);
    d->vblank_frame=s->vblank_frame;
    d->irq=s->irq;d->irq_in_service=s->irq_in_service;d->exiting=s->exiting;
    d->vga.vram_size=s->vga.vram_size;d->vga.cr_index=s->vga.cr_index;
    memcpy(d->vga.cr,s->vga.cr,sizeof d->vga.cr);memcpy(d->vga.sr,s->vga.sr,sizeof d->vga.sr);
    d->pmc=s->pmc;d->pfifo.pending_interrupts=s->pfifo.pending_interrupts;d->pfifo.enabled_interrupts=s->pfifo.enabled_interrupts;
    memcpy(d->pfifo.regs,s->pfifo.regs,sizeof d->pfifo.regs);InterlockedExchange(&d->pfifo.fifo_kick,0);d->pfifo.halt=s->pfifo.halt;
    d->pvideo=s->pvideo;d->ptimer=s->ptimer;d->pfb=s->pfb;d->pgraph=s->pgraph;
    d->pcrtc=s->pcrtc;d->pramdac=s->pramdac;d->puserdac=s->puserdac;
    g_ptimer_clock_bias=saved->clock-qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (d->pfifo.thread.thread) nv2a_fifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
}
