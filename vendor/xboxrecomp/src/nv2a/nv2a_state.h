/*
 * NV2A GPU State - Standalone adaptation of xemu's nv2a_int.h
 *
 * Based on xemu (Copyright (c) 2012 espes, 2015 Jannik Vogel,
 * 2018-2025 Matt Borgerson) - LGPL v2+
 *
 * PGRAPH stores object-cache identities, DMA contexts and class state for
 * the connected guest-memory pixel executor.
 */

#ifndef BURNOUT3_NV2A_STATE_H
#define BURNOUT3_NV2A_STATE_H

#include "qemu_shim.h"
#include "nv2a_regs.h"

/* Debug macros from xemu's debug.h */
#ifndef DEBUG_NV2A
# define DEBUG_NV2A 0
#endif

#if DEBUG_NV2A
# define NV2A_DPRINTF(format, ...)  fprintf(stderr, "nv2a: " format, ## __VA_ARGS__)
#else
# define NV2A_DPRINTF(format, ...)  do { } while (0)
#endif

#define NV2A_XPRINTF(x, ...) do { \
    if (x) { fprintf(stderr, "nv2a: " __VA_ARGS__); } \
} while (0)

#define NV2A_UNCONFIRMED(...)  do {} while (0)
#define NV2A_UNIMPLEMENTED(...) do {} while (0)

/* NV2A_DEVICE: cast to NV2AState* */
#define NV2A_DEVICE(obj) ((NV2AState*)(obj))

/* ============================================================
 * FIFO Engine types
 * ============================================================ */

enum FIFOEngine {
    ENGINE_SOFTWARE = 0,
    ENGINE_GRAPHICS = 1,
    ENGINE_DVD = 2,
};

typedef struct DMAObject {
    unsigned int dma_class;
    unsigned int dma_target;
    hwaddr address;
    hwaddr limit;
} DMAObject;

/* ============================================================
 * PGRAPH state consumed by the class dispatcher and existing pixel executor.
 * ============================================================ */

struct PGRAPHState {
    uint32_t pending_interrupts;
    uint32_t enabled_interrupts;
    uint32_t regs[0x2000];
    struct {
        uint32_t dma_notifies, dma_in, dma_out;
    } memory_to_memory;
    struct {
        uint32_t object_instance, dma_image_source, dma_image_dest;
        uint32_t color_format, source_pitch, dest_pitch;
        uint32_t source_offset, dest_offset;
    } context_surfaces_2d;
    struct {
        uint32_t dma_notifies, color_key, clip, pattern, rop, beta, beta4;
        uint32_t context_surfaces, operation;
        uint32_t in_x, in_y, out_x, out_y, width, height;
    } image_blit;
    uint32_t dma_notifies, dma_a, dma_b, dma_state, dma_color, dma_zeta;
    uint32_t dma_vertex_a, dma_vertex_b, dma_semaphore, dma_report;
    uint32_t edge_flag;
    bool waiting_for_nop;
    bool texture_matrix_enable[4];
    float point_params[8];
    uint32_t ltctxa[NV2A_LTCTXA_COUNT][4];
    uint32_t ltctxb[NV2A_LTCTXB_COUNT][4];
    uint32_t ltc1[NV2A_LTC1_COUNT][4];
    float light_infinite_half_vector[8][4], light_infinite_direction[8][4];
    float light_local_position[8][4], light_local_attenuation[8][4];
    float material_alpha, specular_params[6];
};

/* ============================================================
 * NV2AState - Main GPU state
 * Adapted from xemu's nv2a_int.h
 * ============================================================ */

typedef struct NV2AState {
    /*< private >*/
    PCIDevice parent_obj;
    /*< public >*/

    qemu_irq irq;
    bool exiting;
    bool irq_in_service;

    VGACommonState vga;
    GraphicHwOps hw_ops;
    uint64_t vblank_frame;

    MemoryRegion *vram;
    MemoryRegion vram_pci;
    uint8_t *vram_ptr;
    MemoryRegion ramin;
    uint8_t *ramin_ptr;

    MemoryRegion mmio;
    MemoryRegion block_mmio[NV_NUM_BLOCKS];

    struct {
        uint32_t pending_interrupts;
        uint32_t enabled_interrupts;
    } pmc;

    struct {
        uint32_t pending_interrupts;
        uint32_t enabled_interrupts;
        uint32_t regs[0x2000];
        QemuMutex lock;
        QemuThread thread;
        QemuCond fifo_cond;
        QemuCond fifo_idle_cond;
        volatile LONG fifo_kick;
        bool halt;
        int (*method_consumer)(uint32_t subchannel, uint32_t method, uint32_t parameter, uint8_t *output);
    } pfifo;

    struct {
        uint32_t regs[0x1000];
    } pvideo;

    struct {
        uint32_t pending_interrupts;
        uint32_t enabled_interrupts;
        uint32_t numerator;
        uint32_t denominator;
        uint32_t alarm_time;
    } ptimer;

    struct {
        uint32_t regs[0x1000];
    } pfb;

    struct PGRAPHState pgraph;

    struct {
        uint32_t pending_interrupts;
        uint32_t enabled_interrupts;
        hwaddr start;
        uint32_t raster;
    } pcrtc;

    struct {
        uint32_t core_clock_coeff;
        uint64_t core_clock_freq;
        uint32_t memory_clock_coeff;
        uint32_t video_clock_coeff;
        uint32_t general_control;
        uint32_t fp_vdisplay_end;
        uint32_t fp_vcrtc;
        uint32_t fp_vsync_end;
        uint32_t fp_vvalid_end;
        uint32_t fp_hdisplay_end;
        uint32_t fp_hcrtc;
        uint32_t fp_hvalid_end;
    } pramdac;

    struct {
        uint16_t write_mode_address;
        uint8_t palette[256*3];
    } puserdac;

} NV2AState;

/* ============================================================
 * NV2ABlockInfo - block dispatch table entry
 * ============================================================ */

typedef struct NV2ABlockInfo {
    const char *name;
    hwaddr offset;
    uint64_t size;
    MemoryRegionOps ops;
} NV2ABlockInfo;

extern const NV2ABlockInfo blocktable[NV_NUM_BLOCKS];

/* ============================================================
 * Function prototypes
 * ============================================================ */

void nv2a_update_irq(NV2AState *d);

/* Register block log helpers (no-op stubs since trace is disabled) */
static inline
void nv2a_reg_log_read(int block, hwaddr addr, unsigned int size, uint64_t val)
{
    (void)block; (void)addr; (void)size; (void)val;
}

static inline
void nv2a_reg_log_write(int block, hwaddr addr, unsigned int size, uint64_t val)
{
    (void)block; (void)addr; (void)size; (void)val;
}

/* Register block read/write prototypes */
#define DEFINE_PROTO(n) \
    uint64_t n##_read(void *opaque, hwaddr addr, unsigned int size); \
    void n##_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size);

DEFINE_PROTO(pmc)
DEFINE_PROTO(pbus)
DEFINE_PROTO(pfifo)
DEFINE_PROTO(user)
DEFINE_PROTO(pvideo)
DEFINE_PROTO(ptimer)
DEFINE_PROTO(pfb)
DEFINE_PROTO(pgraph)
DEFINE_PROTO(pcrtc)
DEFINE_PROTO(pramdac)
#undef DEFINE_PROTO

/* Stub read/write for blocks we haven't implemented yet */
uint64_t nv2a_stub_read(void *opaque, hwaddr addr, unsigned int size);
void nv2a_stub_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size);

/* DMA helpers */
DMAObject nv_dma_load(NV2AState *d, hwaddr dma_obj_address);
void *nv_dma_map(NV2AState *d, hwaddr dma_obj_address, hwaddr *len);
void *nv_dma_span(NV2AState *d, hwaddr instance, hwaddr offset, hwaddr bytes);
uint64_t ptimer_get_clock(NV2AState *d);
void nv2a_vblank_tick(NV2AState *d);

/* PGRAPH method dispatch (from push buffer commands) */
void pgraph_method(NV2AState *d, uint32_t subchannel,
                   uint32_t method, uint32_t param);

/* ============================================================
 * Public API
 * ============================================================ */

/* Initialize the NV2A GPU state (standalone, no QEMU PCI bus) */
NV2AState *nv2a_init_standalone(uint8_t *vram_ptr, uint32_t vram_size,
                                 uint8_t *ramin_ptr, uint32_t ramin_size);

/* Process an MMIO read/write from the VEH handler */
uint64_t nv2a_mmio_read(NV2AState *d, hwaddr addr, unsigned int size);
void nv2a_mmio_write(NV2AState *d, hwaddr addr, uint64_t val, unsigned int size);

/* Get the global NV2A state instance */
NV2AState *nv2a_get_state(void);
int nv2a_save_checkpoint(FILE *);
void *nv2a_prepare_checkpoint(FILE *);
void nv2a_apply_checkpoint(const void *);
/* Runs the device DMA state machine under pfifo.lock. */
/* Native constant range: tag, starting row, word count (4 or 16), raw words.
 * It occupies the same storage as the replaced producer's two GPU packets. */
#define NV2A_NATIVE_CONSTANT_RANGE 0x60000003u
/* Packed index range in the same ordered storage: count in bits 18..28,
 * followed by that many pairs of 16-bit indices. */
#define NV2A_NATIVE_INDEX_RANGE 0x60000007u
#define NV2A_NATIVE_INDEX_COUNT_MASK 0x1FFC0000u
void nv2a_fifo_run(NV2AState *d);
/* Kick requires pfifo.lock; start/stop/idle must be called without it. */
void nv2a_fifo_kick(NV2AState *d);
int nv2a_fifo_start(NV2AState *d, int threaded);
void nv2a_fifo_wait_idle(NV2AState *d);
void nv2a_fifo_stop(NV2AState *d);

#endif /* BURNOUT3_NV2A_STATE_H */
