/**
 * Existing guest-memory NV097 pixel consumer, connected to PGRAPH dispatch.
 * Colour surfaces, textures, palettes, vertex arrays and reports use the
 * original GPU's RAMIN DMA mappings. Vertex programs, combiners and swizzle
 * decoding retain their existing implementations. Unsupported methods stop.
 * Fixed-function 3D transforms remain incomplete; current screen-space
 * geometry support does not establish a complete game renderer.
 * RECOMP_FB_DUMP and the existing framebuffer window expose actual pixels.
 */
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "kernel.h"   /* XBOX_CONTIG_BASE / XBOX_CONTIG_SIZE */
#include "xbox_memory_layout.h"   /* xbox_Nv2aFrameCounterFlip */
/* The swizzle decoder the D3D8 layer already uses -- one implementation of
 * Morton order, not a second one that can disagree with it. */
#include "../d3d/d3d8_swizzle.h"
#include "nv2a_vsh_interp.h"
#include "nv2a_combiner.h"
#include "../video/video_player.h"
#include "../nv2a/nv2a_state.h"
#include "../nv2a/nv2a_color_logic.h"
#if defined(_WIN32)
#include <windows.h>
#include "../d3d/d3d8_internal.h"
#include "../d3d/d3d8_vsh.h"
#define NV_TLS __declspec(thread)
#else
#define NV_TLS __thread
#endif

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern ptrdiff_t g_xbox_mem_offset;
extern void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch);
extern void xbox_FramebufferWindowStart(void);
extern uint32_t g_xbox_image_lo, g_xbox_image_hi;

/* Additional image-overlap diagnostic for resolved guest addresses. */
static int surface_hits_image(uint32_t base, uint32_t bytes)
{
    if (!g_xbox_image_hi || !bytes)
        return 0;
    return base < g_xbox_image_hi && base + bytes > g_xbox_image_lo;
}

/* DMA offsets use the PGRAPH object's original RAMIN descriptor. */
static uint32_t dma_resolve(uint32_t instance, uint32_t offset, uint32_t bytes)
{
    NV2AState *gpu = nv2a_get_state();
    if (!gpu) {
        fprintf(stderr, "[STOP] pixel consumer has no connected GPU\n");
        exit(5);
    }
    uint8_t *data = nv_dma_span(gpu, instance, offset, bytes);
    return XBOX_CONTIG_BASE + (uint32_t)(data - gpu->vram_ptr);
}

static uint32_t color_dma_resolve(uint32_t offset, uint32_t bytes)
{
    return dma_resolve(nv2a_get_state()->pgraph.dma_color, offset, bytes);
}

static int surface_write_refused(uint32_t base, uint32_t bytes, const char *what)
{
    static int said;

    if (!surface_hits_image(base, bytes))
        return 0;
    if (!said) {
        said = 1;
        fprintf(stderr,
                "  [GPU] REFUSING to %s surface 0x%08X..0x%08X: that overlaps "
                "the loaded image (0x%08X..0x%08X).\n"
                "  [GPU]   The resolved DMA span overlaps the loaded image.\n",
                what, base, base + bytes, g_xbox_image_lo, g_xbox_image_hi);
        fflush(stderr);
    }
    return 1;
}

/* NV097 methods this executor acts on. */
/* Blending. The pair this title programs, read from its own pushbuffer
 * rather than guessed: BLEND_ENABLE written 1168 times and left on,
 * SFACTOR 0x0302 (SRC_ALPHA) and DFACTOR 0x0303 (ONE_MINUS_SRC_ALPHA).
 * ALPHA_TEST_ENABLE is written 390 times and left at zero, so this is
 * blending and not an alpha test. */
#define NV_BLEND_SRC_ALPHA                0x0302
#define NV_BLEND_ONE_MINUS_SRC_ALPHA      0x0303

/* The buffer flip. A title double-buffers by telling the GPU which buffer
 * the CRTC reads and which it draws into, advancing the write index and
 * then stalling until the flip has happened. Ignoring these means the
 * stall never clears: Half-Life 2's loader submits its initialisation,
 * asks for a flip, and waits for it in a loop that makes no kernel calls
 * and burns no dispatch, which reads as a hang with no cause.
 *
 * ponytail: the flip completes the moment it is asked for, because there is
 * no scanout to be in the middle of. That makes every frame land instantly
 * and a title that paces itself on the flip runs as fast as it can draw.
 * Pacing wants the vblank clock in the kernel, not a sleep in here. */
/* Draw a run of vertices straight out of the arrays, with no index list:
 * bits 0..23 are the first vertex, bits 24..31 the count minus one. It may
 * appear several times inside one BEGIN_END to draw a longer run. */
/* Immediate-mode vertices. SET_VERTEX3F/4F carry the position, and writing
 * its last component completes a vertex using whatever the SET_VERTEX_DATA*
 * registers currently hold for the other attributes. This is how Half-Life
 * 2's Xbox loader and the game's own 2D drawing submit every quad -- neither
 * uses INLINE_ARRAY -- so without these the executor saw SET_BEGIN_END pairs
 * with nothing attached and reported `draws 0` while a million and a half
 * textured quads a minute went past it. */

/* One immediate vertex, as this file packs it for the shared draw path:
 * position float4, diffuse D3DCOLOR, texcoord0 float2. */
/* An immediate-mode vertex: all 16 attributes as float4. */
#define IMM_VERTEX_DWORDS (NV_VERTEX_ATTRS * 4)

#define NV097_CLEAR_COLOR_MASK            0xF0   /* R,G,B,A bits */

/* One vertex attribute stream, as the title describes it. Attribute 0 is
 * position; the rest are colours, texture coordinates and so on. */
typedef struct {
    uint32_t offset;      /* guest address of element 0 */
    uint32_t dma_offset;  /* original method word, including A/B selection */
    int dma_bound;
    uint32_t type;        /* NV097 data type nibble */
    uint32_t size;        /* components per element */
    uint32_t stride;      /* bytes between elements */
} VertexAttr;

#define NV_VERTEX_ATTRS 16
/* Use the existing NV2A batch bound for every producer and its consumers.
 * A BEGIN/END may contain many method packets; 4096 is not a draw limit. */
#define NV_MAX_INDICES  NV2A_MAX_BATCH_LENGTH
#define NV_MAX_INLINE   NV2A_MAX_BATCH_LENGTH /* dwords of INLINE_ARRAY */

static void require_batch_space(const char *kind, uint32_t used,
                                uint32_t added, uint32_t capacity)
{
    if (used > capacity || added > capacity - used) {
        fprintf(stderr, "[STOP] NV097 %s batch exceeds NV2A capacity: used=%u added=%u limit=%u\n",
                kind, used, added, capacity);
        exit(5);
    }
}

/* Texture state decoded by the existing four-stage pixel consumer. */
typedef struct {
    uint32_t offset;                    /* resolved guest address */
    uint32_t dma_offset, dma_format;
    int offset_set;
    uint32_t width, height;             /* from IMAGE_RECT               */
    uint32_t pitch;                     /* bytes per row, from CONTROL1  */
    uint32_t color;                     /* NV097 colour-format code      */
    uint32_t addr_u, addr_v;            /* wrap mode per axis            */
    uint32_t border_color;
    uint32_t palette;                   /* guest address of the CLUT, P8 */
    uint32_t dma_palette;
    int palette_set;
    uint32_t levels;                    /* mip levels, from FORMAT       */
    uint32_t filter;                    /* SET_TEXTURE_FILTER            */
    int      cube;                      /* FORMAT: six faces, not one    */
    int      valid;
} Texture;

typedef struct PbState {
    VertexAttr attr[NV_VERTEX_ATTRS];
    uint32_t   prim;                    /* SET_BEGIN_END parameter, 0 = ended */
    uint32_t   idx[NV_MAX_INDICES];
    uint32_t   idx_count;
    uint8_t    idx_edge[NV_MAX_INDICES];
    /* INLINE_ARRAY payload: vertices written straight into the pushbuffer
     * instead of into a buffer the title points at. Same vertex format, a
     * different place to read them from. */
    uint32_t   inline_buf[NV_MAX_INLINE];
    uint32_t   inline_count;
    uint8_t    inline_edge[NV_MAX_INLINE];
    /* Current values of the immediate-mode attributes, and how many complete
     * vertices they have produced in this batch. */
    /* Immediate mode: every attribute's current value, as the SET_VERTEX
     * methods leave it. Writing attribute 0 (position) emits a vertex. */
    float      imm_attr[NV_VERTEX_ATTRS][4];
    uint16_t   imm_used;                /* attributes written since BEGIN */
    uint32_t   imm_count;
    int        inline_active;
    uint32_t   draws, verts, nonzero_draws;
    float      min_x, max_x, min_y, max_y;
    uint32_t color_offset, color_base, pitch, format;
    /* The surface the last batch actually drew into. A double-buffered title
     * has already pointed color_offset at the next buffer and cleared it by
     * the time the flip arrives, so dumping the current one dumps the frame
     * that has not been drawn yet -- which is how a correctly rendered
     * sequence came out as 12 black BMPs. */
    uint32_t drawn_offset, drawn_dma_color;
    /* ...and the shape of that surface. A title also draws into small
     * render targets (Burnout 3: 128x128 shadow/reflection maps, pitch 512),
     * often after the main scene, so "last surface drawn" alone picked one of
     * those and read it with the framebuffer's pitch: horizontal noise. The
     * biggest surface drawn since the last flip is the one being presented. */
    uint32_t drawn_pitch, drawn_x, drawn_y, drawn_w, drawn_h, drawn_bpp, drawn_format;
    int drawn_stale;      /* set at a flip: the next draw starts a new frame */
    uint64_t pixels;
    uint32_t pixel_max;   /* brightest value any pixel write carried */
    uint32_t clip_x, clip_w, clip_y, clip_h;
    uint32_t clear_color;
    uint32_t clears, unhandled_total;
    uint32_t flip_read, flip_write, flip_modulo, flips;
    uint32_t tris_drawn, tris_skipped_offscreen, batches_untransformed;
    /* Why a batch came out flat. "Untextured" has two causes that look
     * identical on screen and want opposite fixes: the batch carried no
     * texture coordinates, or it did and the stage was not usable. */
    uint32_t batches_textured, batches_no_uv, batches_no_tex;
    uint32_t blend_enable, blend_sfactor, blend_dfactor;
    uint32_t blend_equation, blend_color;
    uint32_t color_mask;        /* SET_COLOR_MASK: A<<24 R<<16 G<<8 B */
    /* Visibility tests (D3D's Begin/EndVisibilityTest): pixels that pass the
     * depth test while counting is on, reported by GET_REPORT. */
    uint32_t zpass_enable, zpass_count, reports;
    uint32_t blend_pairs[16];   /* sfactor<<16 | dfactor seen, for the report */
    int blend_npairs;
    Texture  texs[4];                   /* one per texture stage */
    /* Register combiners and the per-pixel state around them. rc_seen: the
     * title has programmed the combiners, so they decide every pixel's
     * colour; until then the old "stage 0 texel * diffuse" stands in. */
    Nv2aCombiner rc;
    int      rc_seen;
    uint32_t clip_plane_mode;           /* SET_SHADER_CLIP_PLANE_MODE */
    uint32_t alpha_test, alpha_func, alpha_ref;
    uint32_t fog_enable, fog_mode, fog_color;
    float    fog_param[3];              /* original FOG_K row; factor reads x/y */
    /* Vertex programs (nv2a_vsh_interp.c) and the depth buffer, which
     * together are what a 3D scene needs and a 2D one never used. */
    uint32_t xf_mode;                   /* TRANSFORM_EXECUTION_MODE: 2 = program */
    uint32_t batches_program, verts_program, tris_behind;
    /* Where program-path triangles go, so "nothing drew" has a reason. */
    uint32_t xf_degenerate, xf_offscreen, xf_drawn;
    uint64_t xf_depth_fail, xf_pixels;
    float xf_min[3], xf_max[3];
    uint32_t xf_fmt[16];  /* attribute formats seen: type | size<<4 | slot<<8 */
    int xf_nfmt, xf_seeded;
    uint32_t depth_test, depth_func, depth_mask;
    uint32_t zeta_offset, zstencil_clear, zeta_pitch;
    uint64_t timing_frequency, draw_ticks, program_raster_ticks, raster_wait_ticks;
    uint64_t vertex_ticks, hardware_surface_ticks, hardware_texture_ticks;
    uint64_t hardware_state_ticks, hardware_submit_ticks, hardware_publish_ticks;
    uint64_t vertices_transformed, vertices_reused;
    uint64_t hardware_color_uploads, hardware_texture_uploads, hardware_color_readbacks;
    uint64_t hardware_color_readback_bytes;
    uint64_t hardware_cpu_read_faults,hardware_cpu_write_faults,hardware_cpu_access_ticks;
    uint64_t hardware_gpu_completions,hardware_gpu_completion_ticks;
    uint64_t hardware_vertex_stream_uploads, hardware_vertex_stream_starts;
} PbState;
static PbState s_gpu;
static int s_pb_exec_verbose = -1;
#if defined(_WIN32)
static int hw_enabled(void);
static void hw_publish_all(void);
static void hw_complete_commands(void);
static void hw_cpu_write_begin(uint32_t address,size_t bytes,int overwrite);
static void hw_colour_cleared(uint32_t param);
static int hw_clear_full_color(void);
static void hw_clear_depth(uint32_t param);
static void hw_collect_queries(int wait);
static void hw_flush_batch(void);
static uint64_t gpu_timing_clock(void);
static void gpu_timing_finish(uint64_t start, uint64_t *counter);
#endif
void nv2a_pb_publish(void)
{
#if defined(_WIN32)
    hw_publish_all();
#endif
}
void nv2a_pb_checkpoint_publish(void)
{
#if defined(_WIN32)
    if (hw_enabled()) {
        hw_flush_batch();hw_complete_commands();hw_collect_queries(1);hw_publish_all();
    }
#endif
}

/* Unhandled methods, ranked. The interesting output is not that something was
 * skipped but which things dominate, because that is the order to implement
 * them in. */
#define PB_EXEC_MAX_UNHANDLED 2048
typedef struct { uint32_t method, count, last_param; } PbUnhandled;
static PbUnhandled s_unhandled[PB_EXEC_MAX_UNHANDLED];
static int s_unhandled_count;

/* Existing stage-zero diagnostic register snapshot. Rendering uses texs
 * and the original PGRAPH state, including each stage's ENABLE bit. */
#define NV_TEX_FIRST 0x1B00
#define NV_TEX_LAST  0x1BFC
static uint32_t s_tex_reg[(NV_TEX_LAST - NV_TEX_FIRST) / 4 + 1];
static uint8_t  s_tex_set[(NV_TEX_LAST - NV_TEX_FIRST) / 4 + 1];

/* Formats whose dimensions come from the format word and whose coordinates
 * arrive normalised, rather than from a pitch and SET_TEXTURE_IMAGE_RECT with
 * coordinates in texels. Swizzled and block-compressed are both in this group,
 * and every place that used to test only for swizzled needs the pair. */
static int tex_size_from_format(uint32_t fmt)
{
    return fmt==D3DFMT_D16 || fmt==D3DFMT_D24S8 || fmt==D3DFMT_F16 || fmt==D3DFMT_F24S8 || d3d8_format_is_swizzled(fmt) || d3d8_format_dxt_block_bytes(fmt);
}

static int texture_volume(const Texture *t) { return ((t->dma_format>>4)&15)==3; }
static unsigned texture_depth(const Texture *t) { return texture_volume(t)?1u<<((t->dma_format>>28)&15):1; }
static unsigned texture_address_w(unsigned stage) { return (nv2a_get_state()->pgraph.regs[NV_PGRAPH_TEXADDRESS0+stage*4]>>16)&15; }
static uint32_t texture_volume_bytes(const Texture *t)
{
    uint64_t bytes=(uint64_t)t->width*t->height*texture_depth(t)*(d3d8_format_bpp((D3DFORMAT)t->color)/8);
    if(t->cube || !d3d8_format_is_swizzled(t->color) || d3d8_format_dxt_block_bytes(t->color) || !bytes || bytes>UINT32_MAX) {
        fprintf(stderr,"[STOP] volume requires noncompressed Morton level-zero storage\n");exit(5);
    }
    return (uint32_t)bytes;
}

static void note_unhandled(uint32_t method, uint32_t param);

static void tex_update_valid(Texture *t)
{
    struct PGRAPHState *pg = &nv2a_get_state()->pgraph;
    unsigned stage = (unsigned)(t - s_gpu.texs);
    uint32_t control = pg->regs[NV_PGRAPH_TEXCTL0_0 + stage * 4];
    t->valid = (control & NV_PGRAPH_TEXCTL0_0_ENABLE)
            && t->offset_set && t->width && t->height
            && (tex_size_from_format(t->color) || t->pitch);
    if (!t->valid) return;
    if (t->valid) {
        t->offset = dma_resolve((t->dma_format & 3) == 2 ? pg->dma_b : pg->dma_a,
                                t->dma_offset, 1);
    }
    if (t->palette_set) {
        t->palette = dma_resolve((t->dma_palette & 1) ? pg->dma_b : pg->dma_a,
                                 t->dma_palette & ~0x3Fu,
                                 1024u >> ((t->dma_palette >> 2) & 3));
    }
}

static void record_tex_reg(uint32_t method, uint32_t param)
{
    s_tex_reg[(method - NV_TEX_FIRST) / 4] = param;
    s_tex_set[(method - NV_TEX_FIRST) / 4] = 1;
    /* A pitch is a linear texture's property. A swizzled one has no rows and
     * so no pitch, and requiring one here refused every swizzled texture --
     * which is nearly all of them, since swizzled is the Xbox default. That
     * left the title's own textures unsampled and every textured quad drawn in
     * flat vertex colour. */
    tex_update_valid(&s_gpu.texs[0]);
}

/* Every distinct texture a batch was drawn with, and how many batches used it.
 *
 * The per-draw verbose print shows the first few draws of the first frame,
 * which is enough to see that texturing works at all and not enough to answer
 * "is a font page ever bound". This is the same shape as the unhandled-method
 * table below it: a small set, ranked, printed with the rest of the report. */
#define PB_EXEC_MAX_TEXTURES 64
typedef struct {
    uint32_t offset, color, width, height, batches;
} PbTexUse;
static PbTexUse s_tex_use[PB_EXEC_MAX_TEXTURES];
static int s_tex_use_count;

/* Defined below, next to the sampler it goes through. */
static void dump_texture_bmp(uint32_t seq);

/* Repeat dumps are numbered from well past the first-use sequence, so a
 * listing sorts them after the textures they came from and no first-use file
 * is ever overwritten by one. */
#define TEX_DUMP_SEQ_BASE 1000u
#define TEX_DUMP_SEQ_MAX  40u

static void note_texture_use(void)
{
    int i;

    if (!s_gpu.texs[0].valid)
        return;
    for (i = 0; i < s_tex_use_count; i++) {
        if (s_tex_use[i].offset == s_gpu.texs[0].offset
         && s_tex_use[i].color  == s_gpu.texs[0].color) {
            s_tex_use[i].batches++;
            /* Dump a surface that is redrawn, every Nth time it is bound.
             *
             * First use alone cannot tell a decode error that is wrong in
             * every frame from one that accumulates across them. A block
             * transform that is wrong is wrong on its own, in the keyframe
             * as much as anywhere; motion compensation that is wrong starts
             * from a clean keyframe and smears further with each predicted
             * frame after it. In a single frame the two look identical, and
             * in a sequence they look nothing alike -- so the sequence is
             * what has to be captured.
             *
             * It belongs on this side of the return: a video surface keeps
             * one address for the whole film, so after the first frame it is
             * only ever found here, and the first-use dump below never fires
             * for it again. RECOMP_TEX_DUMP_EVERY=<n> sets the interval, and
             * RECOMP_TEX_DUMP still names the files. */
            {
                static int every = -1;
                static unsigned binds, seq;
                if (every < 0) {
                    const char *e = getenv("RECOMP_TEX_DUMP_EVERY");
                    every = e ? atoi(e) : 0;
                }
                if (every > 0 && ++binds % (unsigned)every == 0
                    && seq < TEX_DUMP_SEQ_MAX)
                    dump_texture_bmp(TEX_DUMP_SEQ_BASE + seq++);
            }
            return;
        }
    }
    if (s_tex_use_count < PB_EXEC_MAX_TEXTURES) {
        s_tex_use[s_tex_use_count].offset  = s_gpu.texs[0].offset;
        s_tex_use[s_tex_use_count].color   = s_gpu.texs[0].color;
        s_tex_use[s_tex_use_count].width   = s_gpu.texs[0].width;
        s_tex_use[s_tex_use_count].height  = s_gpu.texs[0].height;
        s_tex_use[s_tex_use_count].batches = 1;
        s_tex_use_count++;
        dump_texture_bmp((uint32_t)s_tex_use_count - 1);
    }
}

static void note_unhandled(uint32_t method, uint32_t param)
{
    fprintf(stderr, "[STOP] unsupported NV097 method 0x%04X parameter=0x%08X\n", method, param);
    NV2AState *gpu = nv2a_get_state();
    if (gpu)
        fprintf(stderr, "[STOP] PGRAPH class=0x%02X instance=0x%X PFIFO GET=0x%X PUT=0x%X state=0x%08X mode=%u lighting=%u\n",
                gpu->pgraph.regs[NV_PGRAPH_CTX_SWITCH1] & NV_PGRAPH_CTX_SWITCH1_GRCLASS,
                gpu->pgraph.regs[NV_PGRAPH_CTX_SWITCH5],
                gpu->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET],
                gpu->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT],
                gpu->pfifo.regs[NV_PFIFO_CACHE1_DMA_STATE],
                s_gpu.xf_mode & 3,
                !!(gpu->pgraph.regs[NV_PGRAPH_CSV0_C] & NV_PGRAPH_CSV0_C_LIGHTING));
    exit(5);
}

static float normalized_vertex_short(float value)
{
    return value / 32767.0f;
}

/* Read attribute `a` of vertex `index` as floats. Only the float and the
 * normalised-byte types appear in practice; anything else returns 0 so a
 * caller sees a degenerate vertex rather than reading past the array. */
/* Callers write local, transformed-vertex or upload data, not descriptors. */
static int fetch_attr(const VertexAttr * __restrict a, uint32_t index, float out[4])
{
    const uint8_t *p;
    uint32_t i;

    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    if (!a->size) {
        /* Disabled arrays consume the original current attribute register. */
        memcpy(out,s_gpu.imm_attr[a-s_gpu.attr],sizeof s_gpu.imm_attr[0]);
        return 1;
    }
    if (!a->stride)
        return 0;
    if (s_gpu.inline_active) {
        /* The batch arrived as INLINE_ARRAY, so `offset` is a byte offset into
         * the buffered payload rather than a guest address -- and 0 is a legal
         * one there, which is why the offset test is on the other side of this
         * branch. */
        size_t at = (size_t)a->offset + (size_t)index * a->stride;
        if (at + 4 > (size_t)s_gpu.inline_count * 4)
            return 0;
        p = (const uint8_t *)s_gpu.inline_buf + at;
    } else {
        if (!a->offset)
            return 0;
        p = (const uint8_t *)g_xbox_mem_offset + a->offset
            + (size_t)index * a->stride;
    }

    switch (a->type) {
    case 0:                                  /* D3DCOLOR */
        /* A DWORD 0xAARRGGBB, so little-endian bytes are B,G,R,A -- not the
         * component order of every other format here. Returned as R,G,B,A so
         * callers need not know which format the title chose. */
        out[0] = (float)p[2] / 255.0f;
        out[1] = (float)p[1] / 255.0f;
        out[2] = (float)p[0] / 255.0f;
        out[3] = (float)p[3] / 255.0f;
        return 1;
    case 2:                                  /* float */
        out[0] = ((const float *)p)[0];
        if (a->size > 1) out[1] = ((const float *)p)[1];
        if (a->size > 2) out[2] = ((const float *)p)[2];
        if (a->size > 3) out[3] = ((const float *)p)[3];
        return 1;
    case 4:                                  /* unsigned byte, normalised */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = (float)p[i] / 255.0f;
        return 1;
    /* The formats 3D geometry uses and screen-space quads never did. */
    case 1:                                  /* signed short, normalised */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = normalized_vertex_short((float)((const int16_t *)p)[i]);
        return 1;
    case 5:                                  /* signed short, as is */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = (float)((const int16_t *)p)[i];
        return 1;
    case 6: {                                /* packed 11:11:10 normal */
        uint32_t v = *(const uint32_t *)p;
        out[0] = (float)((int32_t)(v << 21) >> 21) / 1023.0f;
        out[1] = (float)((int32_t)(v << 10) >> 21) / 1023.0f;
        out[2] = (float)((int32_t)v >> 22) / 511.0f;
        return 1;
    }
    default:
        return 0;
    }
}

static uint32_t surface_bpp(void);

static void note_drawn(void)
{
    /* Swizzled render targets are textures, not linear scanout buffers. */
    if(GET_MASK(s_gpu.format,NV097_SET_SURFACE_FORMAT_TYPE)==NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE)
        return;
    if (!s_gpu.drawn_stale && s_gpu.drawn_offset
        && s_gpu.clip_w * s_gpu.clip_h < s_gpu.drawn_w * s_gpu.drawn_h)
        return;
    s_gpu.drawn_stale = 0;
    s_gpu.drawn_offset = s_gpu.color_offset;
    s_gpu.drawn_dma_color = nv2a_get_state()->pgraph.dma_color;
    s_gpu.drawn_pitch = s_gpu.pitch;
    s_gpu.drawn_bpp = surface_bpp();
    s_gpu.drawn_format = s_gpu.format;
    s_gpu.drawn_x = s_gpu.clip_x; s_gpu.drawn_y = s_gpu.clip_y;
    s_gpu.drawn_w = s_gpu.clip_w; s_gpu.drawn_h = s_gpu.clip_h;
}

static uint32_t surface_bpp(void)
{
    /* The surface format says it outright (NV097_SET_SURFACE_FORMAT_COLOR).
     * Deriving it from pitch / clip width -- the only way before -- is right
     * only while the clip spans the whole surface: Burnout 3 narrows the clip
     * to a 250-pixel window to draw its option values, 2560 / 250 came out as
     * 10 bytes a pixel, and every such text quad was refused. */
    switch (s_gpu.format & 0xF) {
    case 0x1: case 0x2: case 0x3: case 0xA:
        return 2;
    case 0x4: case 0x5: case 0x6: case 0x7: case 0x8:
        return 4;
    case 0x9:
        return 1;
    default:
        break;
    }
    /* The pitch and the clip width together give the pixel size, which is more
     * reliable than decoding the format field: the format's colour code is
     * only meaningful alongside a type the title also sets, while the pitch is
     * always exactly how many bytes a row occupies. */
    if (!s_gpu.clip_w)
        return 0;
    return s_gpu.pitch / s_gpu.clip_w;
}


static int color_swizzled(uint32_t format)
{
    return GET_MASK(format,NV097_SET_SURFACE_FORMAT_TYPE)==NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE;
}
static uint32_t color_width(uint32_t format,uint32_t pitch,uint32_t bpp)
{
    if(!bpp) note_unhandled(NV097_SET_SURFACE_FORMAT,format);
    if(color_swizzled(format)) {
        unsigned shift=GET_MASK(format,NV097_SET_SURFACE_FORMAT_WIDTH);
        if(shift>=32) note_unhandled(NV097_SET_SURFACE_FORMAT,format);
        return 1u<<shift;
    }
    return pitch/bpp;
}
static uint32_t color_height(uint32_t format,uint32_t clip_bottom)
{
    if(color_swizzled(format)) {
        unsigned shift=GET_MASK(format,NV097_SET_SURFACE_FORMAT_HEIGHT);
        if(shift>=32) note_unhandled(NV097_SET_SURFACE_FORMAT,format);
        return 1u<<shift;
    }
    return clip_bottom;
}
static uint32_t color_pitch(uint32_t format,uint32_t pitch,uint32_t bpp)
{
    if(!color_swizzled(format)) return pitch;
    uint64_t bytes=(uint64_t)color_width(format,pitch,bpp)*bpp;
    if(bytes>UINT32_MAX) note_unhandled(NV097_SET_SURFACE_FORMAT,format);
    return (uint32_t)bytes;
}
static uint32_t color_extent(uint32_t format,uint32_t pitch,uint32_t bpp,uint32_t clip_bottom)
{
    uint64_t bytes=(uint64_t)color_pitch(format,pitch,bpp)*color_height(format,clip_bottom);
    if(bytes>UINT32_MAX) note_unhandled(NV097_SET_SURFACE_FORMAT,format);
    return (uint32_t)bytes;
}
static size_t color_pixel_offset(uint32_t format,uint32_t pitch,uint32_t bpp,uint32_t x,uint32_t y)
{
    if(color_swizzled(format))
        return (size_t)swizzle_offset(x,y,color_width(format,pitch,bpp),color_height(format,0))*bpp;
    return (size_t)y*pitch+(size_t)x*bpp;
}

static void require_dither_surface(uint32_t bpp)
{
    /* The pixel consumer supplies byte-valued channels. An RGB8 target
     * retains those values; reducing them to fewer bits needs the hardware
     * dither conversion, which is not implemented here. Use the common
     * PGRAPH control bit so clear and both raster paths have one state. */
    if (bpp < 4 && (nv2a_get_state()->pgraph.regs[NV_PGRAPH_CONTROL_0] &
                    NV_PGRAPH_CONTROL_0_DITHERENABLE)) {
        fprintf(stderr, "[GPU] dither precision reduction unsupported format=%08X bpp=%u\n",
                s_gpu.format, bpp);
        note_unhandled(NV097_SET_DITHER_ENABLE, 1);
    }
}

/* Write the current surface out as a 24-bit BMP.
 *
 * A framebuffer window needs someone watching it. A file does not, which makes
 * this the only way to check what a title actually rendered on a machine you
 * are not sitting at -- and the only way to put a picture in a bug report.
 *
 * ponytail: bottom-up 24bpp BMP, no palette, no compression. That is the one
 * format every viewer reads and it is 30 lines; PNG would need a dependency.
 */
/* Set while RECOMP_FB_DUMP_FLIPS is dumping every flip: the report and
 * after-draw dumps stand down then, or they would put duplicate frames into
 * the sequence. */
static int s_flip_dumping;

static void dump_surface_bmp(void)
{
#if defined(_WIN32)
    hw_publish_all();
#endif
    const char *prefix = getenv("RECOMP_FB_DUMP");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    int drawn = s_gpu.drawn_offset != 0;
    uint32_t offset = drawn ? s_gpu.drawn_offset : s_gpu.color_offset;
    uint32_t pitch = drawn ? s_gpu.drawn_pitch : s_gpu.pitch;
    uint32_t cx = drawn ? s_gpu.drawn_x : s_gpu.clip_x;
    uint32_t cy = drawn ? s_gpu.drawn_y : s_gpu.clip_y;
    static int seq;
    char path[512];
    uint32_t w = drawn ? s_gpu.drawn_w : s_gpu.clip_w;
    uint32_t h = drawn ? s_gpu.drawn_h : s_gpu.clip_h, y, x;
    uint32_t bpp = drawn ? s_gpu.drawn_bpp : surface_bpp();
    uint32_t format=drawn ? s_gpu.drawn_format : s_gpu.format;
    uint32_t row_bytes, pad, filesz;
    uint8_t hdr[54];
    FILE *f;

    if (!prefix || !w || !h || (bpp != 2 && bpp != 4) || !offset)
        return;

    row_bytes = w * 3;
    pad = (4 - (row_bytes & 3)) & 3;
    filesz = 54 + (row_bytes + pad) * h;

    snprintf(path, sizeof path, "%s%05d.bmp", prefix, seq++);
    f = fopen(path, "wb");
    if (!f)
        return;

    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &filesz, 4);
    hdr[10] = 54;
    hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1;
    hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);

    /* BMP rows run bottom-up. */
    for (y = h; y-- > 0; ) {
        const uint8_t *surface = mem + dma_resolve(drawn ? s_gpu.drawn_dma_color : nv2a_get_state()->pgraph.dma_color, offset, color_extent(format,pitch,bpp,cy+h));
        for (x = 0; x < w; x++) {
            const uint8_t *pixel=surface+color_pixel_offset(format,pitch,bpp,cx+x,cy+y);
            uint8_t bgr[3];
            if (bpp == 4) {
                uint32_t v = *(const uint32_t *)pixel;
                bgr[0] = (uint8_t)(v);
                bgr[1] = (uint8_t)(v >> 8);
                bgr[2] = (uint8_t)(v >> 16);
            } else {
                uint16_t v = *(const uint16_t *)pixel;
                bgr[0] = (uint8_t)(( v        & 0x1F) << 3);
                bgr[1] = (uint8_t)(((v >>  5) & 0x3F) << 2);
                bgr[2] = (uint8_t)(((v >> 11) & 0x1F) << 3);
            }
            fwrite(bgr, 1, 3, f);
        }
        if (pad) {
            static const uint8_t zero[3] = {0, 0, 0};
            fwrite(zero, 1, pad, f);
        }
    }
    fclose(f);
    if (seq == 1)
        fprintf(stderr, "  [GPU] framebuffer dump: %s (%ux%u from 0x%08X %ubpp)\n",
                path, w, h, s_gpu.color_offset, bpp);
}

/* Defined below, next to the rest of the rasteriser; the clear path uses it
 * for RECOMP_RASTER_TEST. */
static int polygon_stipple_pass(unsigned x,unsigned y);

static void raster_triangle(const float a[2], const float b[2],
                            const float c[2], uint32_t argb,
                            const float uv[3][2]);
static void raster_xf_clipped(const Nv2aVshOutput *, const Nv2aVshOutput *, const Nv2aVshOutput *, const Nv2aVshOutput *);

typedef struct { uint32_t x0, y0, x1, y1; } ClearBounds;

static ClearBounds clear_bounds(void)
{
    struct PGRAPHState *pg = &nv2a_get_state()->pgraph;
    uint32_t rx = pg->regs[NV_PGRAPH_CLEARRECTX], ry = pg->regs[NV_PGRAPH_CLEARRECTY];
    ClearBounds b;
    b.x0 = GET_MASK(rx, NV_PGRAPH_CLEARRECTX_XMIN);
    b.y0 = GET_MASK(ry, NV_PGRAPH_CLEARRECTY_YMIN);
    b.x1 = GET_MASK(rx, NV_PGRAPH_CLEARRECTX_XMAX) + 1;
    b.y1 = GET_MASK(ry, NV_PGRAPH_CLEARRECTY_YMAX) + 1;
    if (b.x0 < s_gpu.clip_x) b.x0 = s_gpu.clip_x;
    if (b.y0 < s_gpu.clip_y) b.y0 = s_gpu.clip_y;
    if (b.x1 > s_gpu.clip_x + s_gpu.clip_w) b.x1 = s_gpu.clip_x + s_gpu.clip_w;
    if (b.y1 > s_gpu.clip_y + s_gpu.clip_h) b.y1 = s_gpu.clip_y + s_gpu.clip_h;
    return b;
}

static void clear_surface(uint32_t param)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    uint32_t y, x;
    ClearBounds b = clear_bounds();
    int gpu_cleared=0;

    if (!(param & NV097_CLEAR_COLOR_MASK))
        return;                            /* depth/stencil only */
    if (!s_gpu.pitch || b.x0 >= b.x1 || b.y0 >= b.y1)
        return;
    require_dither_surface(bpp);
    uint32_t format = s_gpu.format & 0xF;
    if (format != NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8 &&
        format != NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8 &&
        format != NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5)
        note_unhandled(NV097_CLEAR_SURFACE, param);
    if ((uint64_t)b.x1 * bpp > color_pitch(s_gpu.format,s_gpu.pitch,bpp) ||
        (color_swizzled(s_gpu.format) && b.y1>color_height(s_gpu.format,0)))
        note_unhandled(NV097_CLEAR_SURFACE, param);
    {
        uint32_t bytes=color_extent(s_gpu.format,s_gpu.pitch,bpp,s_gpu.clip_y+s_gpu.clip_h);
        uint32_t base = color_dma_resolve(s_gpu.color_offset,bytes);
        if (surface_write_refused(base,
                                  bytes,
                                  "clear"))
            return;
        s_gpu.color_base = base;
#if defined(_WIN32)
        if(hw_enabled()) {
            int overwrite=bpp==4 && (param&NV097_CLEAR_COLOR_MASK)==NV097_CLEAR_COLOR_MASK &&
                !b.x0 && !b.y0 && b.x1==color_width(s_gpu.format,s_gpu.pitch,bpp) &&
                b.y1==color_height(s_gpu.format,s_gpu.clip_y+s_gpu.clip_h);
            if(overwrite)gpu_cleared=hw_clear_full_color();
            if(!gpu_cleared)hw_cpu_write_begin(base,bytes,overwrite);
        }
#endif
    }

    if(!gpu_cleared) {
    uint32_t mask = ((param & NV097_CLEAR_SURFACE_R) ? 0x00FF0000u : 0)
                  | ((param & NV097_CLEAR_SURFACE_G) ? 0x0000FF00u : 0)
                  | ((param & NV097_CLEAR_SURFACE_B) ? 0x000000FFu : 0)
                  | ((param & NV097_CLEAR_SURFACE_A) ? 0xFF000000u : 0);
    uint32_t clear_color=s_gpu.clear_color;
    if(format==NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8) clear_color|=0xFF000000u;
    if (!color_swizzled(s_gpu.format)) {
        if (bpp == 4) {
            for (y = b.y0; y < b.y1; y++) {
                uint32_t *row=(uint32_t *)(mem+s_gpu.color_base+color_pixel_offset(s_gpu.format,s_gpu.pitch,bpp,b.x0,y));
                for (x = 0; x < b.x1-b.x0; x++)
                    row[x]=(row[x]&~mask)|(clear_color&mask);
            }
        } else if (bpp == 2) {
            uint16_t v = (uint16_t)(((s_gpu.clear_color >> 8) & 0xF800)
                                  | ((s_gpu.clear_color >> 5) & 0x07E0)
                                  | ((s_gpu.clear_color >> 3) & 0x001F));
            uint16_t m = ((param & NV097_CLEAR_SURFACE_R) ? 0xF800 : 0)
                       | ((param & NV097_CLEAR_SURFACE_G) ? 0x07E0 : 0)
                       | ((param & NV097_CLEAR_SURFACE_B) ? 0x001F : 0);
            for (y = b.y0; y < b.y1; y++) {
                uint16_t *row=(uint16_t *)(mem+s_gpu.color_base+color_pixel_offset(s_gpu.format,s_gpu.pitch,bpp,b.x0,y));
                for (x = 0; x < b.x1-b.x0; x++)
                    row[x]=(row[x]&~m)|(v&m);
            }
        }
    } else for (y = b.y0; y < b.y1; y++) {
        if (bpp == 4) {
            for (x = b.x0; x < b.x1; x++) {
                uint32_t *p=(uint32_t *)(mem+s_gpu.color_base+color_pixel_offset(s_gpu.format,s_gpu.pitch,bpp,x,y));
                *p=(*p&~mask)|(clear_color&mask);
            }
        } else if (bpp == 2) {
            /* The clear value is always given as A8R8G8B8; a 16-bit surface
             * takes the same colour reduced to 5:6:5. */
            uint16_t v = (uint16_t)(((s_gpu.clear_color >> 8) & 0xF800)
                                  | ((s_gpu.clear_color >> 5) & 0x07E0)
                                  | ((s_gpu.clear_color >> 3) & 0x001F));
            uint16_t m = ((param & NV097_CLEAR_SURFACE_R) ? 0xF800 : 0)
                       | ((param & NV097_CLEAR_SURFACE_G) ? 0x07E0 : 0)
                       | ((param & NV097_CLEAR_SURFACE_B) ? 0x001F : 0);
            for (x = b.x0; x < b.x1; x++) {
                uint16_t *p=(uint16_t *)(mem+s_gpu.color_base+color_pixel_offset(s_gpu.format,s_gpu.pitch,bpp,x,y));
                *p=(*p&~m)|(v&m);
            }
        }
    }
    }
    s_gpu.clears++;
#if defined(_WIN32)
    if (hw_enabled() && !gpu_cleared) hw_colour_cleared(param);
#endif
    /* Progress markers, interleaved with everything else in the log. The
     * summary says drawing stopped; only a marker next to the surrounding
     * activity says what the title was doing when it stopped. */
    if ((s_gpu.clears % 100) == 0)
        fprintf(stderr, "  [GPU] clear #%u\n", s_gpu.clears);
    /* Distinct clear colours actually used. "Cleared to black" and "the clear
     * never ran" look identical in the framebuffer, and only one of them is a
     * bug -- so record what was asked for, not just how often. */
    {
        static uint32_t seen[8];
        static int n;
        int i;
        for (i = 0; i < n; i++)
            if (seen[i] == s_gpu.clear_color) break;
        if (i == n && n < 8) {
            seen[n++] = s_gpu.clear_color;
            fprintf(stderr, "  [GPU] clear colour 0x%08X -> surface 0x%08X"
                            " (%ubpp)\n",
                    s_gpu.clear_color, s_gpu.color_offset, surface_bpp());
        }
    }

    /* Prove the pixel path end to end, independent of whether the title has
     * given us any geometry yet.
     *
     * "Nothing on screen" has three very different causes -- the surface
     * address or pitch is wrong, the rasteriser is broken, or the title's
     * vertex buffers are empty -- and they are indistinguishable from a black
     * window. RECOMP_RASTER_TEST draws one known triangle into the surface
     * just cleared, so a visible triangle rules out the first two and leaves
     * only the third. On Wreckless it is the third: attribute 0 decodes
     * correctly (float, size 2, stride 16) and the buffer it points at stays
     * zero.
     *
     * ponytail: bring-up aid, not a feature. It costs one branch per clear. */
    static int raster_test = -1;
    if (raster_test < 0)
        raster_test = getenv("RECOMP_RASTER_TEST") != NULL;
    if (raster_test) {
        static int announced;
        /* Every clear, not once: the title clears each frame and double-buffers,
         * so a triangle drawn a single time is erased before anyone sees it. */
        if (s_gpu.clip_w && s_gpu.clip_h) {
            float a[2], b[2], c[2];
            a[0] = s_gpu.clip_w * 0.5f; a[1] = s_gpu.clip_h * 0.15f;
            b[0] = s_gpu.clip_w * 0.85f; b[1] = s_gpu.clip_h * 0.85f;
            c[0] = s_gpu.clip_w * 0.15f; c[1] = s_gpu.clip_h * 0.85f;
            raster_triangle(a, b, c, 0xFFFF00FFu, NULL);  /* magenta: never a clear colour */
            if (announced++ == 0)
            fprintf(stderr, "  [GPU] raster self-test: triangle (%.0f,%.0f)"
                            " (%.0f,%.0f) (%.0f,%.0f) into 0x%08X %ubpp\n",
                    a[0], a[1], b[0], b[1], c[0], c[1],
                    s_gpu.color_offset, surface_bpp());
        }
    }

    /* Show the surface actually being drawn into. A title that double-buffers
     * renders into the back buffer, so following AvSetDisplayMode's address
     * would show the one nothing is writing. */
    /* The window has to read where the pixels actually are, which is the
     * resolved address rather than the DMA-object offset. */
    /* Only until the title flips. Following the draw surface on every scan
     * shows the buffer being written right now, half a frame at a time; past
     * the first flip the window is repointed at the finished one instead. */
    if (s_gpu.flips == 0)
        xbox_FramebufferWindowSet(color_dma_resolve(s_gpu.color_offset, (s_gpu.clip_y + s_gpu.clip_h) * s_gpu.pitch), s_gpu.pitch);

    /* And open the window, rather than waiting for AvSetDisplayMode to do it.
     *
     * That was the only caller, so a title which draws before setting a display
     * mode -- or never sets one at all -- got no window however much it
     * rendered. The Xbox Dashboard clears a 1280x960 surface at 0x00088000 on
     * its first frame and had not called AvSetDisplayMode by then, so
     * RECOMP_FB_WINDOW=1 was set, the executor knew the address and the pitch,
     * and nothing appeared.
     *
     * Here is the better trigger anyway: this runs when a surface address is
     * known to be real, because a clear just used it. Idempotent and gated on
     * RECOMP_FB_WINDOW, so the cost is one interlocked compare per clear. */
    xbox_FramebufferWindowStart();
}


/* ── Rasteriser ──────────────────────────────────────────────────────────
 *
 * Fills triangles straight into the guest framebuffer, the same memory
 * clear_surface() writes and the framebuffer window already shows. That is the
 * whole reason it is done on the CPU rather than through D3D: nothing new has
 * to be plumbed for the result to be visible.
 *
 * ponytail: flat-shaded, no depth buffer, no texturing, no perspective
 * correction, and only batches whose attribute 0 is already in screen space.
 * A title running a vertex program hands over object-space positions that mean
 * nothing without executing the program, so those batches are counted and
 * skipped rather than drawn somewhere wrong. Upgrade path is the D3D11
 * translator in src/nv2a/nv2a_pgraph_d3d11.c once vertex programs are
 * translated; this exists to get the first geometry on screen for every title,
 * which in practice is UI, HUD and 2D overlays -- all pre-transformed.
 */

/* One texel, in the title's own format.
 *
 * The codes are the NV097 colour field, which is the Xbox D3DFMT_ enum --
 * src/d3d/d3d8_xbox.h is the table, and it is the table to check against
 * rather than recollection: 0x1E is LIN_X8R8G8B8 and not, as this first read
 * it, a byte-reversed BGRA. Getting that one wrong turned an opaque black
 * render target into a screen of pure blue, which is the kind of wrong that
 * looks like content.
 *
 * Only the linear (LIN_) formats are read. A swizzled texture stores its
 * texels in Morton order rather than in rows, so reading one as if it had a
 * pitch does not give a slightly wrong colour, it gives a different image --
 * and inventing that image is exactly what this is not for. An unsupported
 * format samples nothing and the caller keeps the vertex colour, which is
 * visibly wrong rather than quietly wrong.
 *
 * ponytail: nearest texel, no filtering, whatever SET_TEXTURE_FILTER asked
 * for. Bilinear when a title's output actually depends on it.
 */
/* Off the edge of the texture, the way the title asked for.
 *
 * Refusing to sample instead is not neutral: it hands the caller back the
 * vertex colour, so a pass whose coordinates reach the last texel by half a
 * texel gets a bright line down the edge of the screen. The dashboard's
 * resolve does exactly that -- its last column and last row, 1119 pixels of
 * white on a black frame, from a rounding step at the boundary.
 */
static uint32_t wrap_coord(uint32_t c, uint32_t size, uint32_t mode)
{
    /* Coordinates are signed: a bilinear tap one texel left of or above
     * texel 0 is -1, which must wrap to the far edge or clamp to 0 -- as an
     * unsigned value it clamped to the far edge instead. */
    int32_t sc = (int32_t)c, n = (int32_t)size;
    if (!size)
        return 0;
    if (mode == 1)                         /* wrap */
        return (uint32_t)(((sc % n) + n) % n);
    if (mode == 2) {                       /* mirrored repeat */
        int64_t period = (int64_t)n * 2;
        int64_t tap = ((int64_t)sc % period + period) % period;
        return (uint32_t)(tap < n ? tap : period - 1 - tap);
    }
    if (mode == 5 && sc < 0) {            /* mirror once, then clamp */
        int64_t tap = -(int64_t)sc - 1;
        return tap >= n ? size - 1 : (uint32_t)tap;
    }
    return sc < 0 ? 0u : (sc >= n ? size - 1 : c);
}

static uint32_t expand(uint32_t v, uint32_t bits)
{
    return d3d8_expand_channel(v, bits);
}

/* The linear format that decodes the same texels as a swizzled one.
 *
 * Swizzling changes where a texel lives, not what it says: A8R8G8B8 (0x06) and
 * LIN_A8R8G8B8 (0x12) are the same four bytes in the same order. So the whole
 * difference is the address calculation, and one of those lets every format
 * below serve both. Pairs read off the table in d3d8_xbox.h rather than
 * recalled -- the comment above this one is about getting exactly that wrong. */
static uint32_t linear_twin(uint32_t fmt)
{
    switch (fmt) {
    case 0x00: return 0x13;                /* L8        -> LIN_L8        */
    case 0x02: return 0x10;                /* A1R5G5B5  -> LIN_A1R5G5B5  */
    case 0x03: return 0x1C;                /* X1R5G5B5  -> LIN_X1R5G5B5  */
    case 0x04: return 0x1D;                /* A4R4G4B4  -> LIN_A4R4G4B4  */
    case 0x05: return 0x11;                /* R5G6B5    -> LIN_R5G6B5    */
    case 0x06: return 0x12;                /* A8R8G8B8  -> LIN_A8R8G8B8  */
    case 0x07: return 0x1E;                /* X8R8G8B8  -> LIN_X8R8G8B8  */
    case 0x19: return 0x1F;                /* A8        -> LIN_A8        */
    case D3DFMT_AL8: return D3DFMT_LIN_AL8;
    case D3DFMT_A8L8: return D3DFMT_LIN_A8L8;
    case D3DFMT_R5G5B5A1: return D3DFMT_LIN_R5G5B5A1;
    case D3DFMT_R4G4B4A4: return D3DFMT_LIN_R4G4B4A4;
    case D3DFMT_A8B8G8R8: return D3DFMT_LIN_A8B8G8R8;
    case D3DFMT_B8G8R8A8: return D3DFMT_LIN_B8G8R8A8;
    case D3DFMT_R8G8B8A8: return D3DFMT_LIN_R8G8B8A8;
    case D3DFMT_D16:return D3DFMT_LIN_D16;
    case D3DFMT_D24S8:return D3DFMT_LIN_D24S8;
    default:   return fmt;                 /* already linear, or unhandled */
    }
}

static uint32_t pack_color(const float c[4]);
static int texture_luma_format(const Texture *t) { return t->color==D3DFMT_LIN_L16; }
static unsigned texture_shadow_bits(const Texture *t)
{
    switch(t->color){case D3DFMT_D16:case D3DFMT_LIN_D16:return 16;
    case D3DFMT_D24S8:case D3DFMT_LIN_D24S8:return 24;default:return 0;}
}
static unsigned texture_shadow_mode(unsigned stage)
{
    const Texture *t=&s_gpu.texs[stage];unsigned bits=texture_shadow_bits(t);if(!bits)return 0;
    unsigned mode=(s_gpu.rc.stage_program>>(stage*5))&31;
    if(mode==0 || mode==4 || mode==5 || mode==17)return 0;
    if(mode!=1 && mode!=2){fprintf(stderr,"[STOP] integer shadow texture stage mode %u is not implemented\n",mode);exit(5);}
    if(t->cube || ((t->dma_format>>4)&15)!=2 || t->addr_u==4 || t->addr_v==4 ||
       nv2a_get_state()->pgraph.regs[NV_PGRAPH_SHADOWZSLOPETHRESHOLD]!=0x7f800000u) {
        fprintf(stderr,"[STOP] integer shadow requires 2D texture, established addressing and disabled slope threshold\n");exit(5);
    }
    return bits;
}
/* Sample texel (u, v) of stage texture `t`; `face` offsets a cube map. */
static int sample_tex_value(const Texture *t, uint32_t face_offset,
                      uint32_t u, uint32_t v, uint32_t z, unsigned addr_w, uint32_t *argb,float value[4])
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    const uint8_t *p;
    uint32_t fmt, base = t->offset + face_offset,local=0;
    if(!argb)argb=&local;

    if (!t->valid)
        return 0;
    if ((t->addr_u == 4 && ((int32_t)u < 0 || u >= t->width)) ||
        (t->addr_v == 4 && ((int32_t)v < 0 || v >= t->height)) ||
        (texture_volume(t) && addr_w == 4 && ((int32_t)z < 0 || z >= texture_depth(t)))) {
        *argb = t->border_color;
        goto decoded;
    }
    u = wrap_coord(u, t->width,  t->addr_u);
    v = wrap_coord(v, t->height, t->addr_v);
    if(texture_volume(t))z=wrap_coord(z,texture_depth(t),addr_w);

    fmt = t->color;
    if (d3d8_format_dxt_block_bytes(fmt)) {
        /* Decoded blocks, direct-mapped by address. Every texel of a DXT
         * texture decodes its whole 4x4 block, and bilinear reads four
         * neighbours that nearly always share one -- this was a tenth of
         * the executor's time. Keyed on the block's bytes as well as its
         * address, so a texture rewritten in place (a render target reused,
         * a streamed mip) never returns stale texels.
         * ponytail: single-threaded executor, so a plain static table. */
        static NV_TLS struct { uintptr_t key; uint32_t fmt; uint64_t raw[2];
                        uint32_t px[16]; } cache[4096];
        uint32_t bb = d3d8_format_dxt_block_bytes(fmt);
        uint32_t bx = u >> 2, by = v >> 2, bw = (t->width + 3) >> 2;
        const uint8_t *blk = mem + base + ((size_t)by * bw + bx) * bb;
        uintptr_t key = (uintptr_t)blk;
        size_t slot = ((key / bb) ^ (key >> 16)) & 4095;
        uint64_t raw[2] = {0, 0};
        memcpy(raw, blk, bb);
        if (cache[slot].key != key || cache[slot].fmt != fmt
            || cache[slot].raw[0] != raw[0] || cache[slot].raw[1] != raw[1]) {
            uint32_t i;
            for (i = 0; i < 16; i++)
                d3d8_dxt_decode_texel(mem + base, fmt, bx * 4 + (i & 3),
                                      by * 4 + (i >> 2), t->width,
                                      &cache[slot].px[i]);
            cache[slot].key = key;
            cache[slot].fmt = fmt;
            cache[slot].raw[0] = raw[0];
            cache[slot].raw[1] = raw[1];
        }
        *argb = cache[slot].px[(v & 3) * 4 + (u & 3)];
        goto decoded;
    }
    if (fmt==D3DFMT_D16 || fmt==D3DFMT_D24S8 || d3d8_format_is_swizzled(fmt)) {
        /* Morton order: a texel's index is interleaved from x and y instead of
         * v*pitch + u, so index from the base of the image. The switch below
         * casts to each format's own width, which makes that index a texel
         * index for every one of them. */
        fmt = linear_twin(fmt);
        p = mem + base;
        u = texture_volume(t)?xbox_swizzle_offset_3d(u,v,z,t->width,t->height,texture_depth(t)):swizzle_offset(u, v, t->width, t->height);
    } else {
        p = mem + base + (size_t)v * t->pitch;
    }

    /* These source layouts already have an authoritative conversion in the
     * D3D resource layer. Consume its upload pixels, then use the existing
     * corresponding linear sampler rather than reproduce its bit rules. */
    uint8_t converted[4];
    uint32_t converted_format = 0;
    if(fmt==D3DFMT_LIN_AL8) converted_format=D3DFMT_LIN_A8L8;
    else if(fmt==D3DFMT_LIN_R5G5B5A1) converted_format=D3DFMT_LIN_A1R5G5B5;
    else if(fmt==D3DFMT_LIN_R4G4B4A4) converted_format=D3DFMT_LIN_A4R4G4B4;
    if(converted_format) {
        d3d8_convert_linear_pixels((D3DFORMAT)fmt,1,1,p+(size_t)u*(d3d8_format_bpp((D3DFORMAT)fmt)/8),converted,0);
        fmt=converted_format;p=converted;u=0;
    }
    if(fmt==D3DFMT_LIN_D16 || fmt==D3DFMT_LIN_D24S8) {
        uint32_t raw=fmt==D3DFMT_LIN_D16?((const uint16_t *)p)[u]:((const uint32_t *)p)[u]>>8;
        float depth=(float)raw;
        if(value){value[0]=depth;value[1]=value[2]=0;value[3]=1;}
        *argb=raw;return 1;
    }
    if(fmt==D3DFMT_LIN_L16) {
        float luma=((const uint16_t *)p)[u]/65535.0f,rgba[4]={luma,luma,luma,1};
        if(value)memcpy(value,rgba,sizeof rgba);
        *argb=pack_color(rgba);return 1;
    }
    switch (fmt) {

    /* NV097 channel pairs use the original 16-bit resource bytes. The
     * published image swizzle also defines both otherwise-missing channels. */
    case D3DFMT_G8B8:case D3DFMT_LIN_G8B8:case D3DFMT_R8B8: {
        uint32_t word=((const uint16_t *)p)[u],lo=word&255,hi=word>>8;
        *argb=(hi<<24)|((fmt==D3DFMT_R8B8?hi:lo)<<16)|((fmt==D3DFMT_R8B8?lo:hi)<<8)|lo;
        goto decoded;
    }

    /* 32-bit, alpha-red-green-blue in the dword. */
    case 0x12:                                      /* LIN_A8R8G8B8 */
        *argb = ((const uint32_t *)p)[u];
        goto decoded;
    case 0x1E:                                      /* LIN_X8R8G8B8 */
        *argb = ((const uint32_t *)p)[u] | 0xFF000000u;
        goto decoded;

    /* 8-bit palette index, swizzled. Burnout 3 draws its logo and frontend
     * header art this way; without a case every such quad came out as the
     * unsupported-format fill, a white box where the logo should be. */
    case 0x0B:                                      /* SZ_I8_A8R8G8B8 */
        if (!t->palette)
            return 0;
        *argb = ((const uint32_t *)(mem + t->palette))[p[u]];
        goto decoded;

    /* 32-bit, other channel orders. The name gives the byte order from the
     * top of the dword down, so each is a permutation of the same four. */
    case 0x3F: {                                    /* LIN_A8B8G8R8 */
        uint32_t t = ((const uint32_t *)p)[u];
        *argb = (t & 0xFF00FF00u) | ((t & 0xFF) << 16) | ((t >> 16) & 0xFF);
        goto decoded;
    }
    case 0x40: {                                    /* LIN_B8G8R8A8 */
        uint32_t t = ((const uint32_t *)p)[u];
        *argb = ((t & 0xFFu) << 24)                 /* A, from the bottom */
              | (((t >>  8) & 0xFFu) << 16)         /* R */
              | (((t >> 16) & 0xFFu) <<  8)         /* G */
              |  ((t >> 24) & 0xFFu);               /* B, from the top */
        goto decoded;
    }
    case 0x41: {                                    /* LIN_R8G8B8A8 */
        uint32_t t = ((const uint32_t *)p)[u];
        *argb = ((t & 0xFFu) << 24) | (t >> 8);
        goto decoded;
    }

    /* 16-bit. */
    case 0x10: {                                    /* LIN_A1R5G5B5 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = ((t & 0x8000u) ? 0xFF000000u : 0u)
              | (expand((t >> 10) & 0x1F, 5) << 16)
              | (expand((t >>  5) & 0x1F, 5) <<  8)
              |  expand( t        & 0x1F, 5);
        goto decoded;
    }
    case 0x1C: {                                    /* LIN_X1R5G5B5 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = 0xFF000000u
              | (expand((t >> 10) & 0x1F, 5) << 16)
              | (expand((t >>  5) & 0x1F, 5) <<  8)
              |  expand( t        & 0x1F, 5);
        goto decoded;
    }
    case 0x11: {                                    /* LIN_R5G6B5 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = 0xFF000000u
              | (expand((t >> 11) & 0x1F, 5) << 16)
              | (expand((t >>  5) & 0x3F, 6) <<  8)
              |  expand( t        & 0x1F, 5);
        goto decoded;
    }
    case 0x1D: {                                    /* LIN_A4R4G4B4 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = (expand((t >> 12) & 0x0F, 4) << 24)
              | (expand((t >>  8) & 0x0F, 4) << 16)
              | (expand((t >>  4) & 0x0F, 4) <<  8)
              |  expand( t        & 0x0F, 4);
        goto decoded;
    }

    /* 8-bit. */
    case 0x13: {                                    /* LIN_L8 */
        uint32_t t = p[u];
        *argb = 0xFF000000u | (t << 16) | (t << 8) | t;
        goto decoded;
    }
    case 0x1F:                                      /* LIN_A8 */
        *argb = ((uint32_t)p[u] << 24) | 0x00FFFFFFu;
        goto decoded;
    case D3DFMT_LIN_A8L8: {
        uint32_t pixel=((const uint16_t *)p)[u],luma=pixel&255;
        *argb=((pixel&0xff00)<<16)|(luma<<16)|(luma<<8)|luma;
        goto decoded;
    }

    /* 4:2:2 packed YUV, two texels per four bytes.
     *
     * This is how a title hands over a decoded video frame, and without it
     * the frame falls through to `default` -- which returns 0, so the caller
     * paints the quad's vertex colour and the movie is a flat rectangle.
     *
     * The chroma pair is shared between an even texel and the one after it,
     * so the group is found by masking the bottom bit of the index. BT.601,
     * the same coefficients the D3D8 upload path converts with, so the two
     * paths agree rather than each having its own idea of the colour. */
    case 0x24:                                      /* LC_CR8YB8CB8YA8, YUY2 */
    case 0x25: {                                    /* LC_YB8CR8YA8CB8, UYVY */
        uint32_t yoff = (fmt == 0x24) ? 0u : 1u;
        const uint8_t *g = p + (size_t)(u & ~1u) * 2;
        int c  = (int)g[(u & 1u) ? 2 + yoff : yoff] - 16;
        int cu = (int)g[1 - yoff] - 128;
        int cv = (int)g[3 - yoff] - 128;
        int r = (298 * c + 409 * cv + 128) >> 8;
        int gg = (298 * c - 100 * cu - 208 * cv + 128) >> 8;
        int b = (298 * c + 516 * cu + 128) >> 8;
        if (r < 0) r = 0;
        if (r > 255) r = 255;
        if (gg < 0) gg = 0;
        if (gg > 255) gg = 255;
        if (b < 0) b = 0;
        if (b > 255) b = 255;
        *argb = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)gg << 8)
              | (uint32_t)b;
        goto decoded;
    }

    default:
        fprintf(stderr,"[STOP] texture format decoder %02X is not implemented\n",t->color);exit(5);
    }
decoded:
    if(value)nv2a_rc_unpack(*argb,value);
    return 1;
}

static int sample_tex(const Texture *t,uint32_t face,uint32_t u,uint32_t v,uint32_t *argb)
{ return sample_tex_value(t,face,u,v,0,0,argb,NULL); }

static int sample_texture(uint32_t u, uint32_t v, uint32_t *argb)
{
    return sample_tex(&s_gpu.texs[0], 0, u, v, argb);
}

/* Write a bound texture out as a BMP, through the sampler rather than around it.
 *
 * "Which texture is this" is not answerable from an address and a format, and
 * it is the question behind most of the ones that matter -- is that a font
 * page or an icon atlas, did the swizzle decode, is the alpha inverted. Going
 * through sample_texture means the file shows exactly what the rasteriser
 * sees, so a decode bug appears here rather than only as a wrong-looking
 * triangle.
 *
 * ponytail: RGB only, alpha dropped. A glyph page is alpha and would come out
 * black, so alpha is composited onto mid-grey to stay legible; that is a
 * viewing choice, not a decode. One file per distinct texture, first use only.
 */
static void dump_texture_bmp(uint32_t seq)
{
    const char *prefix = getenv("RECOMP_TEX_DUMP");
    uint32_t w = s_gpu.texs[0].width, h = s_gpu.texs[0].height, x, y;
    uint32_t row_bytes, pad, filesz;
    uint8_t hdr[54];
    char path[512];
    FILE *f;

    if (!prefix || !w || !h || w > 4096 || h > 4096)
        return;
    row_bytes = w * 3;
    pad = (4 - (row_bytes & 3)) & 3;
    filesz = 54 + (row_bytes + pad) * h;

    snprintf(path, sizeof path, "%s%02u_%08X_fmt%02X.bmp",
             prefix, seq, s_gpu.texs[0].offset, s_gpu.texs[0].color);
    f = fopen(path, "wb");
    if (!f)
        return;
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &filesz, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint32_t argb = 0, a;
            uint8_t px[3];
            if (!sample_texture(x, h - 1 - y, &argb))
                argb = 0;
            a = (argb >> 24) & 0xFFu;
            /* over mid-grey, so an alpha-only page is visible either way */
            px[0] = (uint8_t)(((argb & 0xFFu) * a + 128u * (255u - a)) / 255u);
            px[1] = (uint8_t)((((argb >> 8) & 0xFFu) * a + 128u * (255u - a)) / 255u);
            px[2] = (uint8_t)((((argb >> 16) & 0xFFu) * a + 128u * (255u - a)) / 255u);
            fwrite(px, 1, 3, f);
        }
        if (pad) {
            static const uint8_t zero[3] = {0, 0, 0};
            fwrite(zero, 1, pad, f);
        }
    }
    fclose(f);
    fprintf(stderr, "  [TEXDUMP] %s (%ux%u fmt 0x%02X)\n",
            path, w, h, s_gpu.texs[0].color);
    fflush(stderr);
}

/* The surface, resolved once per batch.
 *
 * The bound colour DMA descriptor and offset stay fixed during a batch.
 * Resolve and check the surface extent before rasterising its pixels.
 *
 * This is not a micro-optimisation for its own sake: the loader's video
 * paces on frames actually presented, so the rasteriser's throughput is the
 * playback rate. */
static uint8_t *s_surface;          /* host address of surface row 0 */

static int surface_begin_batch(const uint8_t *mem)
{
    require_dither_surface(surface_bpp());
    uint32_t bytes=color_extent(s_gpu.format,s_gpu.pitch,surface_bpp(),s_gpu.clip_y+s_gpu.clip_h);
    if(color_swizzled(s_gpu.format) &&
       ((uint64_t)s_gpu.clip_x+s_gpu.clip_w>color_width(s_gpu.format,s_gpu.pitch,surface_bpp()) ||
        (uint64_t)s_gpu.clip_y+s_gpu.clip_h>color_height(s_gpu.format,0)))
        note_unhandled(NV097_SET_SURFACE_FORMAT,s_gpu.format);
    uint32_t base = color_dma_resolve(s_gpu.color_offset,bytes);

    if (surface_hits_image(base,bytes))
        return 0;
    s_surface = (uint8_t *)mem + base;
    return 1;
}


/* One GL blend factor, per channel (r g b a). */
static void blend_factor(uint32_t f, const float s[4], const float d[4],
                         float out[4])
{
    int k;
    for (k = 0; k < 4; k++) {
        float c = (float)((s_gpu.blend_color >> (k == 3 ? 24 : 16 - 8 * k))
                          & 0xFF) / 255.0f;
        switch (f) {
        case 0x0000: out[k] = 0.0f;            break;   /* ZERO                */
        case 0x0001: out[k] = 1.0f;            break;   /* ONE                 */
        case 0x0300: out[k] = s[k];            break;   /* SRC_COLOR           */
        case 0x0301: out[k] = 1.0f - s[k];     break;
        case 0x0302: out[k] = s[3];            break;   /* SRC_ALPHA           */
        case 0x0303: out[k] = 1.0f - s[3];     break;
        case 0x0304: out[k] = d[3];            break;   /* DST_ALPHA           */
        case 0x0305: out[k] = 1.0f - d[3];     break;
        case 0x0306: out[k] = d[k];            break;   /* DST_COLOR           */
        case 0x0307: out[k] = 1.0f - d[k];     break;
        case 0x0308: out[k] = k == 3 ? 1.0f : fminf(s[3], 1.0f - d[3]); break;
        case 0x8001: out[k] = c;               break;   /* CONSTANT_COLOR      */
        case 0x8002: out[k] = 1.0f - c;        break;
        case 0x8003: out[k] = (float)(s_gpu.blend_color >> 24) / 255.0f; break;
        case 0x8004: out[k] = 1.0f - (float)(s_gpu.blend_color >> 24) / 255.0f; break;
        default:     out[k] = f ? 1.0f : 0.0f; break;
        }
    }
}

static void put_pixel(uint8_t *mem, uint32_t bpp, int x, int y, uint32_t argb)
{
    uint8_t *row;

    (void)mem;
    if (x < (int)s_gpu.clip_x || x >= (int)(s_gpu.clip_x + s_gpu.clip_w))
        return;
    if (y < (int)s_gpu.clip_y || y >= (int)(s_gpu.clip_y + s_gpu.clip_h))
        return;
    s_gpu.pixels++;
    if ((argb & 0x00FFFFFFu) > (s_gpu.pixel_max & 0x00FFFFFFu))
        s_gpu.pixel_max = argb;
    row = s_surface + color_pixel_offset(s_gpu.format,s_gpu.pitch,bpp,(uint32_t)x,(uint32_t)y);
    x = 0;

    /* Blending, with the GL factor set and equations the NV2A takes.
     *
     * Only SRC_ALPHA / ONE_MINUS_SRC_ALPHA used to be honoured and every other
     * pair was written opaque. Menus never noticed; a race does: Burnout 3
     * finishes its 3D frame with screen-space passes that modulate or add
     * over the scene, and written opaque they paint the whole view over. */
    uint32_t logic=nv2a_get_state()->pgraph.regs[NV_PGRAPH_BLEND];
    if(logic&NV_PGRAPH_BLEND_LOGICOP_ENABLE) {
        uint32_t destination;
        if(bpp==4)destination=*(const uint32_t *)row;
        else {
            uint32_t packed=*(const uint16_t *)row;
            destination=0xff000000u|((packed&0xf800u)<<8)|((packed&0x07e0u)<<5)|((packed&31u)<<3);
        }
        argb=color_logic(GET_MASK(logic,NV_PGRAPH_BLEND_LOGICOP),argb,destination);
    } else if (s_gpu.blend_enable
        && !(s_gpu.blend_sfactor == 1 && s_gpu.blend_dfactor == 0)) {
        uint32_t dst = 0xFF000000u;
        float sc[4], dc[4], sf[4], df[4], out[4];
        int k;
        if (bpp == 4) {
            dst = ((const uint32_t *)row)[x];
        } else if (bpp == 2) {
            uint32_t t = ((const uint16_t *)row)[x];
            dst = 0xFF000000u | ((t & 0xF800u) << 8) | ((t & 0x07E0u) << 5)
                | ((t & 0x001Fu) << 3);
        }
        for (k = 0; k < 4; k++) {           /* r g b a, 0..1 */
            int sh = k == 3 ? 24 : 16 - 8 * k;
            sc[k] = (float)((argb >> sh) & 0xFF) / 255.0f;
            dc[k] = (float)((dst  >> sh) & 0xFF) / 255.0f;
        }
        blend_factor(s_gpu.blend_sfactor, sc, dc, sf);
        blend_factor(s_gpu.blend_dfactor, sc, dc, df);
        for (k = 0; k < 4; k++) {
            float a = sc[k] * sf[k], b = dc[k] * df[k];
            switch (s_gpu.blend_equation) {
            case 0x800A: out[k] = a - b; break;               /* SUBTRACT     */
            case 0x800B: out[k] = b - a; break;               /* REV_SUBTRACT */
            case 0x8007: out[k] = fminf(sc[k], dc[k]); break; /* MIN          */
            case 0x8008: out[k] = fmaxf(sc[k], dc[k]); break; /* MAX          */
            default:     out[k] = a + b; break;               /* ADD          */
            }
        }
        argb = pack_color(out);
    }

    if (s_gpu.color_mask != 0x01010101u) {
        uint32_t keep = 0, old;
        if (!(s_gpu.color_mask & 0x01000000u)) keep |= 0xFF000000u;
        if (!(s_gpu.color_mask & 0x00010000u)) keep |= 0x00FF0000u;
        if (!(s_gpu.color_mask & 0x00000100u)) keep |= 0x0000FF00u;
        if (!(s_gpu.color_mask & 0x00000001u)) keep |= 0x000000FFu;
        if (keep == 0xFFFFFFFFu)
            return;
        if (bpp == 4) {
            old = ((const uint32_t *)row)[x];
        } else {
            uint32_t t = ((const uint16_t *)row)[x];
            old = ((t & 0xF800u) << 8) | ((t & 0x07E0u) << 5) | ((t & 0x001Fu) << 3);
        }
        argb = (argb & ~keep) | (old & keep);
    }
    if (bpp == 4) {
        ((uint32_t *)row)[x] = argb;
    } else if (bpp == 2) {
        ((uint16_t *)row)[x] = (uint16_t)(((argb >> 8) & 0xF800)
                                        | ((argb >> 5) & 0x07E0)
                                        | ((argb >> 3) & 0x001F));
    }
}

/* Half-space fill. Barycentric edge functions rather than scanline slopes:
 * the same test decides both windings, so a title that emits clockwise
 * triangles does not silently render nothing. */
static int polygon_stipple_pass(unsigned x,unsigned y)
{
    struct PGRAPHState *pg=&nv2a_get_state()->pgraph;
    if(s_gpu.prim<NV097_SET_BEGIN_END_OP_TRIANGLES || !(pg->regs[NV_PGRAPH_SETUPRASTER]&NV_PGRAPH_SETUPRASTER_STIPPLEENABLE))return 1;
    return (pg->polygon_stipple[31-(y&31)] & (1u<<((x&31)^7)))!=0;
}

static void raster_triangle(const float a[2], const float b[2],
                            const float c[2], uint32_t argb,
                            const float uv[3][2])
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    float area;
    int minx, maxx, miny, maxy, x, y;
    int textured = uv && s_gpu.texs[0].valid;

    if (bpp != 4 && bpp != 2)
        return;

    area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    if (area == 0.0f)
        return;                            /* degenerate */

    /* Where this batch writes. The same check the per-pixel path made, made
     * once: a surface address landing on the title's own image is no safer
     * one pixel at a time than 4.9 MB at once. */
    if (!surface_begin_batch(mem))
        return;

    minx = (int)floorf(fminf(a[0], fminf(b[0], c[0])));
    maxx = (int)ceilf (fmaxf(a[0], fmaxf(b[0], c[0])));
    miny = (int)floorf(fminf(a[1], fminf(b[1], c[1])));
    maxy = (int)ceilf (fmaxf(a[1], fmaxf(b[1], c[1])));

    if (minx < (int)s_gpu.clip_x) minx = (int)s_gpu.clip_x;
    if (miny < (int)s_gpu.clip_y) miny = (int)s_gpu.clip_y;
    if (maxx > (int)(s_gpu.clip_x + s_gpu.clip_w)) maxx = (int)(s_gpu.clip_x + s_gpu.clip_w);
    if (maxy > (int)(s_gpu.clip_y + s_gpu.clip_h)) maxy = (int)(s_gpu.clip_y + s_gpu.clip_h);
    if (minx >= maxx || miny >= maxy) {
        s_gpu.tris_skipped_offscreen++;
        return;
    }

    for (y = miny; y < maxy; y++) {
        for (x = minx; x < maxx; x++) {
            if(!polygon_stipple_pass(x,y))continue;
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
            float w1 = (c[0] - b[0]) * (py - b[1]) - (c[1] - b[1]) * (px - b[0]);
            float w2 = (a[0] - c[0]) * (py - c[1]) - (a[1] - c[1]) * (px - c[0]);
            if (!((w0 >= 0 && w1 >= 0 && w2 >= 0)
               || (w0 <= 0 && w1 <= 0 && w2 <= 0)))
                continue;
            if (textured) {
                /* Barycentric, straight from the edge functions already
                 * computed: w1 is the area opposite a, w2 opposite b, w0
                 * opposite c, and the three sum to the whole triangle.
                 *
                 * No perspective divide. These are screen-space vertices with
                 * no w to divide by -- which is exactly the case a full-screen
                 * pass is, and the only case that reaches here. */
                uint32_t texel;
                float su = (w1 * uv[0][0] + w2 * uv[1][0] + w0 * uv[2][0]) / area;
                float sv = (w1 * uv[0][1] + w2 * uv[1][1] + w0 * uv[2][1]) / area;
                if (su < 0.0f) su = 0.0f;
                if (sv < 0.0f) sv = 0.0f;
                if (sample_texture((uint32_t)su, (uint32_t)sv, &texel)) {
                    put_pixel(mem, bpp, x, y, texel);
                    continue;
                }
            }
            put_pixel(mem, bpp, x, y, argb);
        }
    }
    s_gpu.tris_drawn++;
    note_drawn();
}

/* Attribute 3 supplies diffuse colour. A disabled array consumes its
 * original current register through the same attribute reader used by
 * programmable vertices. */
/* Which attribute carries the colour.
 *
 * Slot 3 is diffuse by convention and titles that follow it are read straight
 * from there. Half-Life 2 does not: its vertex is position, colour, texcoord
 * at stride 24, and the colour arrives in slot 5. So fall back to the format
 * rather than the slot number -- D3DCOLOR is the one attribute type that is
 * only ever a colour, which makes it a stronger signal than the convention. */
static const VertexAttr *color_attr(void)
{
    uint32_t a;

    if (s_gpu.attr[3].offset && s_gpu.attr[3].stride)
        return &s_gpu.attr[3];
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (s_gpu.attr[a].type == 0 && s_gpu.attr[a].size == 4
                && s_gpu.attr[a].offset && s_gpu.attr[a].stride)
            return &s_gpu.attr[a];
    return &s_gpu.attr[3];
}

/* Attribute 9 is texture coordinate 0 in the NV2A vertex layout, the same way
 * 0 is position and 3 is diffuse -- for a title that follows the convention.
 *
 * Half-Life 2 does not, in either place. Its menu and HUD vertex is position,
 * colour, texcoord at stride 24, with the colour in slot 5 and the texcoords
 * in slot 7, so reading slot 9 found nothing and every batch drew untextured.
 * That is invisible rather than wrong-looking: the menu paints a full-screen
 * quad and then draws its text over it, and with no sampling both come out
 * white, so the screen is blank white and nothing suggests the text was ever
 * drawn.
 *
 * Falling back to the format works because the three attributes of such a
 * vertex are distinguishable: position is float3, colour is D3DCOLOR, and a
 * float2 is a texture coordinate and nothing else.
 *
 * ponytail: takes the first float2 it finds, so a title with two texcoord sets
 * gets stage 0's -- which is what this single-texture rasteriser samples
 * anyway. Multi-texture wants the D3D11 translator, not another heuristic. */
static const VertexAttr *texcoord_attr(void)
{
    uint32_t a;

    if (s_gpu.attr[9].offset && s_gpu.attr[9].stride)
        return &s_gpu.attr[9];
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (s_gpu.attr[a].type == 2 && s_gpu.attr[a].size == 2
                && s_gpu.attr[a].offset && s_gpu.attr[a].stride)
            return &s_gpu.attr[a];
    return &s_gpu.attr[9];
}

/* Texel coordinates, whichever convention the title used.
 *
 * The two are not interchangeable and the format decides which is in force: a
 * swizzled texture is addressed in [0,1], a linear one in texels. Both are
 * scaled to texels here so that everything downstream -- the barycentric
 * interpolation and the sampler -- works in one unit.
 *
 * This mattered the moment swizzled formats became samplable. Normalised
 * coordinates truncated to a texel index land on texel 0 for any coordinate
 * below 1.0, so a whole quad sampled a single texel and came out flat: the
 * background painted one near-black colour, which looks like a texture that
 * decoded wrong rather than one that was never indexed. */
static int fetch_texcoord(uint32_t index, float out[2])
{
    float t[4];

    if (!fetch_attr(texcoord_attr(), index, t))
        return 0;
    out[0] = t[0];
    out[1] = t[1];
    if (tex_size_from_format(s_gpu.texs[0].color)) {
        out[0] *= (float)s_gpu.texs[0].width;
        out[1] *= (float)s_gpu.texs[0].height;
    }
    return 1;
}

static uint32_t vertex_color(uint32_t index)
{
    float c[4];

    if (!fetch_attr(color_attr(), index, c))
        return 0xFFFFFFFFu;
    return ((uint32_t)(c[3] * 255.0f) << 24)
         | ((uint32_t)(c[0] * 255.0f) << 16)
         | ((uint32_t)(c[1] * 255.0f) <<  8)
         |  (uint32_t)(c[2] * 255.0f);
}

/* An untransformed batch drawn as if it were screen space smears a few pixels
 * into the corner, so the batch has to be classified before it is rasterised.
 *
 * This used to demand that every vertex land inside the surface, which is a
 * different question and the wrong one: geometry that extends past the
 * viewport is ordinary, and clipping it is raster_triangle's job (it clamps
 * its span to the clip rect). The dashboard is exactly the case that exposed
 * it -- a full-screen pass drawn as one oversized triangle, vertices at
 * (-0.5,-0.5), (2*w,-0.5), (-0.5,2*h), all correct and all rejected.
 *
 * What actually separates the two is scale. Object-space positions are model
 * units, a handful either side of the origin; screen-space ones are measured
 * in pixels of a surface hundreds of pixels wide. So: the batch has to be able
 * to touch the surface at all, and it has to be bigger than object space.
 *
 * ponytail: a genuinely tiny screen-space sprite reads as object space and is
 * skipped. It is counted as skipped rather than silently dropped, and the
 * unambiguous answer needs the vertex-program state, which is not tracked yet.
 */
#define OBJECT_SPACE_SPAN 8.0f

static int batch_is_screen_space(void)
{
    float p[4], lo_x, hi_x, lo_y, hi_y;
    uint32_t i;

    if (!s_gpu.clip_w || !s_gpu.clip_h || !s_gpu.idx_count)
        return 0;
    if (!fetch_attr(&s_gpu.attr[0], s_gpu.idx[0], p))
        return 0;
    lo_x = hi_x = p[0];
    lo_y = hi_y = p[1];
    for (i = 1; i < s_gpu.idx_count; i++) {
        if (!fetch_attr(&s_gpu.attr[0], s_gpu.idx[i], p))
            return 0;
        if (p[0] < lo_x) lo_x = p[0];
        if (p[0] > hi_x) hi_x = p[0];
        if (p[1] < lo_y) lo_y = p[1];
        if (p[1] > hi_y) hi_y = p[1];
    }

    /* Entirely off the surface: nothing to draw under either reading. */
    if (hi_x < (float)s_gpu.clip_x
     || lo_x > (float)(s_gpu.clip_x + s_gpu.clip_w)
     || hi_y < (float)s_gpu.clip_y
     || lo_y > (float)(s_gpu.clip_y + s_gpu.clip_h))
        return 0;

    /* Small enough to be model units rather than pixels. */
    if (hi_x - lo_x < OBJECT_SPACE_SPAN && hi_y - lo_y < OBJECT_SPACE_SPAN)
        return 0;

    return 1;
}

/* NV097 primitive types.
 *
 * These are the operand of SET_BEGIN_END, where 0 is END and the list starts
 * at 1. They were each one too low, so every title's geometry was decomposed
 * as the primitive below the one it asked for -- a strip as a fan, a fan as
 * quads, and TRIANGLES, the one case whose vertex count must be a multiple
 * of three, as a strip.
 *
 * The vertex order says which numbering is right without taking a table on
 * trust: a strip arrives in Z order and a fan in cyclic order, and they only
 * line up with the primitive under this one. */
#define NV_PRIM_POINTS         1
#define NV_PRIM_LINES          2
#define NV_PRIM_LINE_LOOP      3
#define NV_PRIM_LINE_STRIP     4
#define NV_PRIM_TRIANGLES      5
#define NV_PRIM_TRIANGLE_STRIP 6
#define NV_PRIM_TRIANGLE_FAN   7
#define NV_PRIM_QUADS          8
#define NV_PRIM_QUAD_STRIP     9
#define NV_PRIM_POLYGON        10

/* How many post-draw captures to keep: enough to see whether the geometry
 * is stable from frame to frame, few enough not to fill a directory. */
/* Primitive colour selection follows the original xemu geometry dispatcher.
 * Both indexed screen-space and program batches consume this same selection. */
static uint32_t provoking_slot(struct PGRAPHState *pg,uint32_t start)
{
    int first = (pg->regs[NV_PGRAPH_CONTROL_3] &
                 NV_PGRAPH_CONTROL_3_PROVOKING_VERTEX) != 0;
    switch (s_gpu.prim) {
    case NV_PRIM_LINES: case NV_PRIM_LINE_STRIP: return first ? start : start+1;
    case NV_PRIM_LINE_LOOP: return first ? start : (start+1)%s_gpu.idx_count;
    case NV_PRIM_TRIANGLES: return first ? start : start + 2;
    case NV_PRIM_TRIANGLE_STRIP: return first ? start : start + 2 - (start & 1);
    case NV_PRIM_TRIANGLE_FAN: return first ? 0 : start;
    case NV_PRIM_POLYGON: return start + 1;
    case NV_PRIM_QUADS: case NV_PRIM_QUAD_STRIP: return start + 3;
    default: note_unhandled(NV097_SET_BEGIN_END, s_gpu.prim); return 0;
    }
}

#define FB_DUMP_AFTER_DRAW 8
static int s_drawn_dumps;

static void dump_surface_bmp(void);

/* Screen-space arrays feed the same depth, combiner and colour consumer as
 * program outputs. The provoking vertex can lie outside a split triangle. */
static int fetch_screen_vertex(uint32_t index,Nv2aVshOutput *out)
{
    if (!fetch_attr(&s_gpu.attr[0], index, out->pos))
            return 0;
        nv2a_rc_unpack(vertex_color(index), out->d0);
        fetch_attr(&s_gpu.attr[4], index, out->d1);
        fetch_attr(&s_gpu.attr[5], index, out->fog);
        if (s_gpu.fog_enable) {
            uint32_t mode = GET_MASK(nv2a_get_state()->pgraph.regs[NV_PGRAPH_CSV0_D],
                                     NV_PGRAPH_CSV0_D_FOGGENMODE);
            if (mode == NV_PGRAPH_CSV0_D_FOGGENMODE_SPEC_ALPHA) {
                float alpha = out->d1[3];
                out->fog[0] = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
            } else if (mode != NV_PGRAPH_CSV0_D_FOGGENMODE_FOG_X) {
                note_unhandled(NV097_SET_FOG_GEN_MODE, mode);
            }
        }
        for (unsigned st = 0; st < 4; ++st)
            fetch_attr(&s_gpu.attr[9 + st], index, out->tex[st]);
    return 1;
}
static void raster_indexed(uint32_t i0,uint32_t i1,uint32_t i2,uint32_t provoking_index)
{
    const uint32_t indices[3]={i0,i1,i2};
    Nv2aVshOutput vertices[3]={0},provoking={0};
    for(unsigned i=0;i<3;i++) if(!fetch_screen_vertex(indices[i],&vertices[i])) return;
    nv2a_rc_unpack(vertex_color(provoking_index),provoking.d0);
    fetch_attr(&s_gpu.attr[4],provoking_index,provoking.d1);
    raster_xf_clipped(&vertices[0],&vertices[1],&vertices[2],&provoking);
}

/* ---- 3D: vertex programs and depth -------------------------------------
 *
 * A batch drawn while TRANSFORM_EXECUTION_MODE says "program" carries
 * model-space positions. Each vertex is run through the title's own vertex
 * program (nv2a_vsh_interp.c), whose oPos output on Xbox D3D is already
 * screen space -- the runtime appends the viewport transform and the divide
 * by w -- with the clip-space w left in oPos.w. So the rasteriser gets pixel
 * coordinates and a depth, and w for perspective-correct texturing.
 *
 * ponytail: no clipping. A triangle with any vertex behind the eye (w <= 0)
 * is dropped rather than clipped, which loses the slivers that cross the near
 * plane; road right under the camera is where that shows. Clip in
 * homogeneous space if it matters. */

/* Software pixels use the same DMA-resolved RAM as CPU and texture accesses.
 * No separate logical depth/stencil pixel storage is retained. */
#define NV_ZBUF_W 1024
#define NV_ZBUF_H 1024
typedef struct { uint8_t *memory;uint32_t address,width,height,pitch,format,layout; } ZetaSurface;
static int s_ftrace;            /* frame trace: 0 idle, 2 tracing, 3 done */

static float zclear_value(void)
{
    /* Zeta format in SURFACE_FORMAT bits 4-7: 1 is Z16, 2 is Z24S8. */
    uint32_t zf = (s_gpu.format >> 4) & 0xF;
    return zf == 1 ? (float)(s_gpu.zstencil_clear & 0xFFFF)
                   : (float)(s_gpu.zstencil_clear >> 8);
}

/* Keep encoded depth in the existing surface cache, including float formats.
 * Switching CONTROL0 therefore reinterprets the same guest bits instead of
 * replacing the surface. NV2A F16 is unsigned e4m12 with bias 7; F24 is
 * unsigned e8m16 with bias 127, distinct from host half/float depth. */
static float depth_encode(float depth, unsigned format)
{
    if (format==16 || format==24) {
        uint32_t bits;memcpy(&bits,&depth,4);
        if (!(depth>0)) return 0;
        if (format==16) {
            if (bits<=0x3c000000u) return 0;
            uint32_t value=(bits-0x3c000000u)>>11;
            return (float)(value>65535?65535:value);
        }
        uint32_t value=bits>>7;
        return (float)(value>0xfeffffu?0xfeffffu:value);
    }
    return floorf(fminf(format==1?65535.0f:16777215.0f,fmaxf(0,depth)));
}

static ZetaSurface zeta_surface(void)
{
    ZetaSurface surface={0};surface.format=(s_gpu.format>>4)&15;
    if(surface.format!=1 && surface.format!=2)note_unhandled(NV097_SET_SURFACE_FORMAT,s_gpu.format);
    unsigned bpp=surface.format==1?2:4;
    surface.width=color_width(s_gpu.format,s_gpu.pitch,surface_bpp());surface.height=color_height(s_gpu.format,s_gpu.clip_y+s_gpu.clip_h);
    surface.pitch=color_swizzled(s_gpu.format)?surface.width*bpp:s_gpu.zeta_pitch;surface.layout=s_gpu.format;
    if(!surface.width || !surface.height || surface.width>NV_ZBUF_W || surface.height>NV_ZBUF_H || surface.pitch<surface.width*bpp || (uint64_t)surface.pitch*surface.height>UINT32_MAX)
        note_unhandled(NV097_SET_SURFACE_PITCH,(s_gpu.zeta_pitch<<16)|s_gpu.pitch);
    surface.address=dma_resolve(nv2a_get_state()->pgraph.dma_zeta,s_gpu.zeta_offset,surface.pitch*surface.height);
    surface.memory=(uint8_t *)xbox_GetMemoryOffset()+surface.address;return surface;
}
static uint8_t *zeta_pixel(const ZetaSurface *surface,unsigned x,unsigned y)
{
    return surface->memory+color_pixel_offset(surface->layout,surface->pitch,surface->format==1?2:4,x,y);
}
static void zeta_write_depth(const ZetaSurface *surface,uint8_t *pixel,uint32_t encoded)
{
    if(surface->format==1){uint16_t value=(uint16_t)encoded;memcpy(pixel,&value,2);}
    else {pixel[1]=(uint8_t)encoded;pixel[2]=(uint8_t)(encoded>>8);pixel[3]=(uint8_t)(encoded>>16);}
}
static void zbuf_clear(uint32_t param)
{
    ClearBounds b=clear_bounds();if(b.x0>=b.x1 || b.y0>=b.y1)return;
    ZetaSurface surface=zeta_surface();
    if(b.x1>surface.width || b.y1>surface.height || ((param&NV097_CLEAR_SURFACE_STENCIL) && surface.format!=2))note_unhandled(NV097_CLEAR_SURFACE,param);
    uint32_t value=(uint32_t)zclear_value();
    for(unsigned y=b.y0;y<b.y1;y++)for(unsigned x=b.x0;x<b.x1;x++) {
        uint8_t *pixel=zeta_pixel(&surface,x,y);
        if(param&NV097_CLEAR_SURFACE_Z)zeta_write_depth(&surface,pixel,value);
        if(param&NV097_CLEAR_SURFACE_STENCIL)*pixel=(uint8_t)s_gpu.zstencil_clear;
    }
}

static int compare_pass(uint32_t func, float z, float stored)
{
    switch (func) {                              /* GL enums, as the NV2A */
    case 0x200: return 0;
    case 0x201: return z <  stored;
    case 0x202: return z == stored;
    case 0x203: return z <= stored;
    case 0x204: return z >  stored;
    case 0x205: return z != stored;
    case 0x206: return z >= stored;
    default:    return 1;
    }
}

static uint32_t pack_color(const float c[4])
{
    int i;
    uint32_t b[4];
    for (i = 0; i < 4; i++) {
        float f = c[i] < 0.0f ? 0.0f : (c[i] > 1.0f ? 1.0f : c[i]);
        b[i] = (uint32_t)(f * 255.0f + 0.5f);
    }
    return (b[3] << 24) | (b[0] << 16) | (b[1] << 8) | b[2];
}

static uint32_t modulate(uint32_t t, const float c[4])
{
    float f[4];
    f[0] = (float)((t >> 16) & 0xFF) / 255.0f * c[0];
    f[1] = (float)((t >> 8) & 0xFF) / 255.0f * c[1];
    f[2] = (float)(t & 0xFF) / 255.0f * c[2];
    f[3] = (float)(t >> 24) / 255.0f * c[3];
    return pack_color(f);
}

/* Bytes one texel of a (non-DXT) format takes, for cube-face strides. */
static uint32_t tex_texel_bytes(uint32_t fmt)
{
    switch (fmt) {
    case 0x00: case 0x01: case 0x0B: case 0x13: case 0x19: case 0x1F:
        return 1;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x10: case 0x11:
    case 0x1C: case 0x1D:case D3DFMT_D16:case D3DFMT_LIN_D16:
    case D3DFMT_G8B8:case D3DFMT_R8B8:case D3DFMT_LIN_G8B8:
        return 2;
    default:
        return 4;
    }
}

/* Bytes between cube faces: one face with all its mip levels, rounded up to
 * the NV2A's 128-byte face alignment (xemu texture.c). */
static uint32_t tex_face_stride(const Texture *t)
{
    uint32_t w = t->width, h = t->height, lv, total = 0;
    uint32_t block = d3d8_format_dxt_block_bytes(t->color);
    uint32_t levels = t->levels ? t->levels : 1;

    if (!tex_size_from_format(t->color))
        return (t->pitch * t->height + 127u) & ~127u;
    for (lv = 0; lv < levels; lv++) {
        uint32_t lw = (w >> lv) ? (w >> lv) : 1, lh = (h >> lv) ? (h >> lv) : 1;
        total += block ? ((lw + 3) / 4) * ((lh + 3) / 4) * block
                       : lw * lh * tex_texel_bytes(t->color);
    }
    return (total + 127u) & ~127u;
}

/* One texel at normalised (swizzled/DXT) or texel (linear) coordinates, as
 * floats. An unusable stage reads white, so a missing texture multiplies
 * through instead of blacking the pixel out. */
static void rc_texel(const Texture *t, uint32_t face, float u, float v, float w, unsigned addr_w,
                     float out[4])
{
    int volume=texture_volume(t);
    uint32_t texel, mag = (t->filter >> 24) & 0xF, min = (t->filter >> 16) & 0xFF;
    if (tex_size_from_format(t->color)) {
        u *= (float)t->width;
        v *= (float)t->height;
        if(volume)w*=(float)texture_depth(t);
    }
    /* TENT (bilinear) when the title asks for it, magnifying or minifying:
     * a 64x32 sky gradient stretched over the screen is blocks with nearest
     * texels and a gradient with four. MAG 2 is tent; MIN 2 is tent at LOD 0
     * and 4/6 the tent mip modes.
     * ponytail: level 0 only, no mip selection -- minified textures shimmer.
     * Add LOD from the screen-space derivative when that shows. */
    if (mag == 2 || min == 2 || min == 4 || min == 6) {
        float fu = u - 0.5f, fv = v - 0.5f, wu, wv, c[4][4];
        int32_t iu = (int32_t)floorf(fu), iv = (int32_t)floorf(fv), k, j;
        wu = fu - (float)iu;
        wv = fv - (float)iv;
        float ww=0;int32_t iw=0;
        if(volume){float fw=w-0.5f;iw=(int32_t)floorf(fw);ww=fw-(float)iw;}
        for(unsigned plane=0;plane<(volume?2u:1u);plane++) {
        for (k = 0; k < 4; k++) {
            if (sample_tex_value(t, face, (uint32_t)(iu + (k & 1)),
                           (uint32_t)(iv + (k >> 1)),volume?(uint32_t)(iw+plane):0,addr_w, &texel,c[k])) { }
            else {
                if(volume){fprintf(stderr,"[STOP] volume texel has no decoded source\n");exit(5);}
                c[k][0] = c[k][1] = c[k][2] = c[k][3] = 1.0f;
            }
        }
        for (j = 0; j < 4; j++) {
            float sampled = (c[0][j] * (1.0f - wu) + c[1][j] * wu) * (1.0f - wv)
                   + (c[2][j] * (1.0f - wu) + c[3][j] * wu) * wv;
            if(!plane)out[j]=sampled;else out[j]=out[j]*(1.0f-ww)+sampled*ww;
        }
        }
        return;
    }
    if (sample_tex_value(t, face, (uint32_t)(int32_t)floorf(u),
                   (uint32_t)(int32_t)floorf(v),volume?(uint32_t)(int32_t)floorf(w):0,addr_w, &texel,out)) { }
    else {
        if(volume){fprintf(stderr,"[STOP] volume texel has no decoded source\n");exit(5);}
        out[0] = out[1] = out[2] = out[3] = 1.0f;
    }
}

/* What texture stage `st` contributes, by its SHADER_STAGE_PROGRAM mode
 * (xemu psh.c). Returns 0 when a clip-plane stage kills the pixel. */
static int texture_alpha_kill_enabled(unsigned stage)
{
    return s_gpu.texs[stage].valid &&
        (nv2a_get_state()->pgraph.regs[NV_PGRAPH_TEXCTL0_0 + stage * 4] &
         NV_PGRAPH_TEXCTL0_0_ALPHAKILLEN) != 0;
}

static uint32_t texture_color_key_mask(unsigned stage)
{
    switch(s_gpu.texs[stage].color) {
    case D3DFMT_X1R5G5B5:case D3DFMT_LIN_X1R5G5B5:
    case D3DFMT_X8R8G8B8:case D3DFMT_LIN_X8R8G8B8:return 0x00ffffffu;
    default:return 0xffffffffu;
    }
}
static unsigned texture_color_key_mode(unsigned stage)
{
    unsigned mode=s_gpu.texs[stage].valid ? nv2a_get_state()->pgraph.regs[NV_PGRAPH_TEXCTL0_0+stage*4]&NV_PGRAPH_TEXCTL0_0_COLORKEYMODE : 0;
    unsigned format=s_gpu.texs[stage].color;
    if(mode && (format==D3DFMT_G8B8 || format==D3DFMT_R8B8 || format==D3DFMT_LIN_G8B8)) {
        fprintf(stderr,"[STOP] channel-pair texture color-key mask is not established\n");exit(5);
    }
    return mode;
}
static int texture_post_sample(unsigned stage,float color[4])
{
    if(texture_alpha_kill_enabled(stage) && color[3]==0) return 0;
    unsigned mode=texture_color_key_mode(stage);
    if(!mode)return 1;
    uint32_t mask=texture_color_key_mask(stage),key=nv2a_get_state()->pgraph.regs[NV_PGRAPH_COLORKEYCOLOR0+stage*4];
    if((pack_color(color)&mask)!=(key&mask))return 1;
    if(mode==3)return 0;
    color[3]=0;
    if(mode==2)color[0]=color[1]=color[2]=0;
    return 1;
}

static int texture_stage_samples(unsigned mode)
{
    return (mode>=1 && mode<=3) || mode==6 || mode==7 || mode==9 || mode==11 || mode==12 || mode==13 || mode==14 || mode==18 || mode==15 || mode==16;
}
static unsigned texture_dependent_source(unsigned stage)
{
    unsigned source=stage==1?0:(nv2a_get_state()->pgraph.regs[NV_PGRAPH_SHADERCTL]>>(stage==2?16:20))&15;
    if(stage==0 || source>=stage || !tex_size_from_format(s_gpu.texs[stage].color) || s_gpu.texs[stage].cube || ((s_gpu.texs[stage].dma_format>>4)&15)!=2) {
        fprintf(stderr,"[STOP] dependent texture stage requires an earlier input and swizzled 2D texture\n");exit(5);
    }
    return source;
}

static unsigned texture_bump_source(unsigned stage)
{
    const Texture *t=&s_gpu.texs[stage];
    unsigned source=stage==1?0:(nv2a_get_state()->pgraph.regs[NV_PGRAPH_SHADERCTL]>>(stage==2?16:20))&15;
    if(stage==0 || source>=stage || !t->valid || t->cube || ((t->dma_format>>4)&15)!=2) {
        fprintf(stderr,"[STOP] bump texture requires earlier input and valid unsigned 2D target\n");exit(5);
    }
    return source;
}
static void texture_bump_parameters(unsigned stage,float matrix[4],float lum_coord[4])
{
    const uint32_t *regs=nv2a_get_state()->pgraph.regs;
    const unsigned offsets[]={NV_PGRAPH_BUMPMAT00,NV_PGRAPH_BUMPMAT01,NV_PGRAPH_BUMPMAT10,NV_PGRAPH_BUMPMAT11};
    for(unsigned i=0;i<4;i++)memcpy(&matrix[i],&regs[offsets[i]+(stage-1)*4],4);
    memcpy(&lum_coord[0],&regs[NV_PGRAPH_BUMPSCALE1+(stage-1)*4],4);
    memcpy(&lum_coord[1],&regs[NV_PGRAPH_BUMPOFFSET1+(stage-1)*4],4);
    const Texture *t=&s_gpu.texs[stage];
    lum_coord[2]=tex_size_from_format(t->color)?1.0f:1.0f/t->width;
    lum_coord[3]=tex_size_from_format(t->color)?1.0f:1.0f/t->height;
}
static float texture_bump_signed(float value)
{
    value*=255.0f;return (value>=128.0f?value-256.0f:value)/127.0f;
}

typedef struct RcStageDots { float value[4],coordinate_w[4],next_coordinate[4]; } RcStageDots;

static unsigned texture_dot_source(unsigned stage,unsigned mode)
{
    const Texture *t=&s_gpu.texs[stage];unsigned program=s_gpu.rc.stage_program;
    unsigned source=stage==1?0:(nv2a_get_state()->pgraph.regs[NV_PGRAPH_SHADERCTL]>>(stage==2?16:20))&15;
    unsigned previous=stage?(program>>((stage-1)*5))&31:0;
    if(!stage || source>=stage || (mode==17 && stage>2) ||
       (mode==9 && (stage<2 || (previous!=17 && previous!=9) || !t->valid || t->cube || ((t->dma_format>>4)&15)!=2)) ||
       (mode==13 && (stage!=3 || ((program>>5)&31)!=17 || previous!=17 || source!=0 || ((nv2a_get_state()->pgraph.regs[NV_PGRAPH_SHADERCTL]>>16)&15)!=0 || !t->valid || t->cube || (((t->dma_format>>4)&15)!=2 && !texture_volume(t)))) ||
       (mode==11 && (stage!=2 || ((program>>5)&31)!=17 || (((program>>15)&31)!=12 && ((program>>15)&31)!=18) || ((nv2a_get_state()->pgraph.regs[NV_PGRAPH_SHADERCTL]>>20)&15)>=2 || !t->valid || ((t->dma_format>>4)&15)!=2)) ||
       ((mode==14 || mode==12 || mode==18) && (stage!=3 || ((program>>5)&31)!=17 || (previous!=17 && previous!=9 && !((mode==12 || mode==18) && previous==11)) || !t->valid || ((t->dma_format>>4)&15)!=2))) {
        fprintf(stderr,"[STOP] DOT stage requires established earlier input and DOT_ST 2D chain\n");exit(5);
    }
    return source;
}
static unsigned texture_dot_mapping(unsigned stage)
{
    unsigned mapping=(nv2a_get_state()->pgraph.regs[NV_PGRAPH_SHADERCTL]>>((stage-1)*4))&15;
    if(mapping>4){fprintf(stderr,"[STOP] DOT HILO hemisphere mapping is not implemented\n");exit(5);}
    return mapping;
}
static float texture_dot_component(unsigned mapping,float value)
{
    if(!mapping)return value;
    if(mapping==3)return texture_bump_signed(value);
    float encoded=value*255.0f;
    if(mapping==1)return (encoded-128.0f)/127.0f;
    return (encoded>=128.0f?encoded-255.5f:encoded+0.5f)/127.5f;
}

static float texture_dot_value(unsigned mapping,const float color[4],const float coordinate[4])
{
    if(mapping==4) {
        uint32_t hi=((uint32_t)(color[3]*255.0f)<<8)|(uint32_t)(color[0]*255.0f);
        uint32_t lo=((uint32_t)(color[1]*255.0f)<<8)|(uint32_t)(color[2]*255.0f);
        return coordinate[0]*((float)hi/65535.0f)+coordinate[1]*((float)lo/65535.0f)+coordinate[2];
    }
    float value=0;for(unsigned component=0;component<3;component++)value+=coordinate[component]*texture_dot_component(mapping,color[component]);return value;
}

static void texture_eye_vector(float eye[3])
{
    const uint32_t *regs=nv2a_get_state()->pgraph.regs;
    for(unsigned i=0;i<3;i++)memcpy(eye+i,regs+NV_PGRAPH_EYEVEC0+i*4,4);
}

static int texture_luma_stage(unsigned stage)
{
    const Texture *t=&s_gpu.texs[stage];if(!texture_luma_format(t))return 0;
    unsigned mode=(s_gpu.rc.stage_program>>(stage*5))&31;
    if(texture_stage_samples(mode) && (t->cube || texture_color_key_mode(stage))) {
        fprintf(stderr,"[STOP] pitched Y16 cube/color-key contract is not implemented\n");exit(5);
    }
    return 1;
}

static void rc_cube_texel(const Texture *t,const float c[3],float out[4])
{
    float x = c[0], y = c[1], z = c[2];
    float ax = fabsf(x), ay = fabsf(y), az = fabsf(z), ma, sc, tc;
    uint32_t face;
    if (ax >= ay && ax >= az) {
        face = x > 0 ? 0 : 1; ma = ax; sc = x > 0 ? -z : z; tc = -y;
    } else if (ay >= az) {
        face = y > 0 ? 2 : 3; ma = ay; sc = x; tc = y > 0 ? z : -z;
    } else {
        face = z > 0 ? 4 : 5; ma = az; sc = z > 0 ? x : -x; tc = -y;
    }
    if (ma == 0.0f)
        ma = 1.0f;
    rc_texel(t, face * tex_face_stride(t), (sc / ma + 1.0f) * 0.5f,
             (tc / ma + 1.0f) * 0.5f,0,0,out);
}

static void rc_dot_texel(const Texture *t,const float direction[3],float out[4])
{
    if(t->cube){rc_cube_texel(t,direction,out);return;}
    float x=direction[0],y=direction[1],z=direction[2],ax=fabsf(x),ay=fabsf(y),az=fabsf(z),u,v;
    if(ax>ay && ax>az){u=(x>0?-z:z)/ax;v=y/ax;}
    else if(ay>ax && ay>az){u=x/ay;v=(y>0?-z:z)/ay;}
    else {u=(z>0?x:-x)/az;v=y/az;}
    /* The primary DOT remap uses normalized sampler2D coordinates even
     * for pitched textures; rc_texel takes pixels for pitched storage. */
    if(!tex_size_from_format(t->color)){u*=t->width;v*=t->height;}
    rc_texel(t,0,u,v,0,0,out);
}

static int rc_stage_fetch(int st, const float c[4], const float previous[4][4],RcStageDots *dots,float out[4])
{
    const Texture *t = &s_gpu.texs[st];
    uint32_t mode = (s_gpu.rc.stage_program >> (st * 5)) & 0x1F, j;
    float q = c[3] != 0.0f ? c[3] : 1.0f;

    (void)texture_luma_stage((unsigned)st);
    unsigned shadow=texture_shadow_mode((unsigned)st);
    if(shadow && (mode==1 || mode==2)) {
        rc_texel(t,0,c[0]/q,c[1]/q,0,0,out);
        float reference=mode==2?fminf(shadow==16?65535.0f:16777215.0f,fmaxf(0,c[2]/q)):0;
        unsigned function=nv2a_get_state()->pgraph.regs[NV_PGRAPH_SHADOWCTL]&7;
        float pass=compare_pass(0x200|function,out[0],reference)?1.0f:0.0f;
        out[0]=out[1]=out[2]=out[3]=pass;
        return texture_post_sample((unsigned)st,out);
    }
    switch (mode) {
    case 0:                                       /* NONE */
        out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f;
        return 1;
    case 1: case 2:                               /* PROJECT2D / 3D */
        if(mode==2 && texture_volume(t))rc_texel(t,0,c[0]/c[3],c[1]/c[3],c[2]/c[3],texture_address_w(st),out);
        else rc_texel(t, 0, c[0] / q, c[1] / q,0,0,out);
        return texture_post_sample(st,out);
    case 3: {                                     /* CUBEMAP */
        if(t->cube)rc_cube_texel(t,c,out);else rc_texel(t,0,c[0],c[1],0,0,out);
        return texture_post_sample(st,out);
    }
    case 17: case 9: case 11: case 14: case 12: case 13: case 18: {                    /* DOTPRODUCT / DOT_ST / DOT_STR_CUBE */
        unsigned source=texture_dot_source(st,mode),mapping=texture_dot_mapping(st);
        dots->coordinate_w[st]=c[3];
        if(!((mode==12 || mode==18) && ((s_gpu.rc.stage_program>>10)&31)==11))dots->value[st]=texture_dot_value(mapping,previous[source],c);
        if(mode==11) {
            unsigned next_source=texture_dot_source(3,(s_gpu.rc.stage_program>>15)&31),next_mapping=texture_dot_mapping(3);
            dots->coordinate_w[3]=dots->next_coordinate[3];dots->value[3]=texture_dot_value(next_mapping,previous[next_source],dots->next_coordinate);
        }
        if(mode==17){out[0]=out[1]=out[2]=out[3]=0;return 1;}
        if(mode==13)rc_texel(t,0,dots->value[1],dots->value[2],dots->value[3],texture_address_w(st),out);
        else if(mode==12 || mode==18) {
            const float *normal=dots->value+1,*eye=dots->coordinate_w+1;
            float constant_eye[3];if(mode==18){texture_eye_vector(constant_eye);eye=constant_eye;}
            float scale=2.0f*(normal[0]*eye[0]+normal[1]*eye[1]+normal[2]*eye[2])/(normal[0]*normal[0]+normal[1]*normal[1]+normal[2]*normal[2]);
            float reflected[3];for(unsigned i=0;i<3;i++)reflected[i]=normal[i]*scale-eye[i];
            rc_dot_texel(t,reflected,out);
        } else if(mode==14 || mode==11)rc_dot_texel(t,dots->value+1,out);else rc_texel(t,0,dots->value[st-1],dots->value[st],0,0,out);
        return texture_post_sample(st,out);
    }
    case 6: case 7: {                             /* BUMPENVMAP / LUM */
        unsigned source=texture_bump_source(st);float matrix[4],params[4];texture_bump_parameters(st,matrix,params);
        float ds=texture_bump_signed(previous[source][2]),dt=texture_bump_signed(previous[source][1]);
        rc_texel(t,0,c[0]+matrix[0]*ds+matrix[2]*dt,c[1]+matrix[1]*ds+matrix[3]*dt,0,0,out);
        if(mode==7)for(unsigned component=0;component<4;component++)out[component]*=params[0]*previous[source][0]+params[1];
        return texture_post_sample(st,out);
    }
    case 15: case 16: {                           /* DEPENDENT AR / GB */
        unsigned source=texture_dependent_source(st);
        rc_texel(t,0,previous[source][mode==15?3:1],previous[source][mode==15?0:2],0,0,out);
        return texture_post_sample(st,out);
    }
    case 4:                                       /* PASSTHRU */
        for (j = 0; j < 4; j++)
            out[j] = c[j] < 0.0f ? 0.0f : (c[j] > 1.0f ? 1.0f : c[j]);
        return 1;
    case 5:                                       /* CLIPPLANE */
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        for (j = 0; j < 4; j++) {
            int ge = (s_gpu.clip_plane_mode >> (st * 4 + j)) & 1;
            if (ge ? c[j] >= 0.0f : c[j] < 0.0f)
                return 0;
        }
        return 1;
    default:
        note_unhandled(NV097_SET_SHADER_STAGE_PROGRAM, s_gpu.rc.stage_program);
        return 0;
    }
}

/* The fixed-function fog unit, per vertex, from the program's oFog.x
 * (xemu vsh.c). Fog disabled reads as factor 1: no fog. */
static float fog_factor(float d)
{
    float f, px = s_gpu.fog_param[0], py = s_gpu.fog_param[1];
    if (!s_gpu.fog_enable)
        return 1.0f;
    switch (s_gpu.fog_mode) {
    case 0x800: case 0x802:                       /* EXP, EXP_ABS */
        f = px + exp2f(d * py * 16.0f) - 1.5f;
        break;
    case 0x801: case 0x803:                       /* EXP2, EXP2_ABS */
        f = px + exp2f(-d * d * py * py * 32.0f) - 1.5f;
        break;
    default:                                      /* LINEAR, LINEAR_ABS */
        f = px + d * py - 1.0f;
        break;
    }
    if (s_gpu.fog_mode == 0x802 || s_gpu.fog_mode == 0x803
        || s_gpu.fog_mode == 0x804)
        f = fabsf(f);
    if (f != f)
        f = 1.0f;
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

static int alpha_test_pass(float a)
{
    int v = (int)(a * 255.0f + 0.5f), r = (int)(s_gpu.alpha_ref & 0xFF);
    return compare_pass(s_gpu.alpha_func, (float)v, (float)r);
}

/* Everything a triangle's pixels need, computed once per triangle, so the
 * pixel loop can run over any subset of rows on any thread. */
typedef struct {
    uint8_t *mem;
    uint32_t bpp;
    const Nv2aVshOutput *va, *vb, *vc;
    const float *a, *b, *c;
    float iw[3], uv[3][2], stc[3][4][4], vfog[3], fogc[4], inv_area;
    ZetaSurface zeta;
    int has_zeta,stencil_test;
    uint32_t stencil_func, stencil_ref, stencil_mask;
    uint32_t stencil_write_mask, stencil_fail, stencil_zfail, stencil_zpass;
    float zmin, zmax, depth_offset_bias, depth_offset_slope;
    uint32_t clip_x[8], clip_y[8];
    int clip_exclusive, depth_clipping, w_depth;
    unsigned depth_format;
    unsigned edge_inclusive, raster_mode;
    int line;
    int minx, maxx, miny, maxy, use_rc, textured, alpha_kill;
} XfTri;

static void xf_stencil_write(const XfTri *triangle, uint8_t *pixel, unsigned operation,uint8_t *storage)
{
    if (!pixel || !triangle->stencil_write_mask) return;
    unsigned value=*pixel;
    switch (operation) {
    case NV_PGRAPH_CONTROL_2_STENCIL_OP_V_KEEP: return;
    case NV_PGRAPH_CONTROL_2_STENCIL_OP_V_ZERO: value=0; break;
    case NV_PGRAPH_CONTROL_2_STENCIL_OP_V_REPLACE: value=triangle->stencil_ref; break;
    case NV_PGRAPH_CONTROL_2_STENCIL_OP_V_INCRSAT: if (value<255) ++value; break;
    case NV_PGRAPH_CONTROL_2_STENCIL_OP_V_DECRSAT: if (value>0) --value; break;
    case NV_PGRAPH_CONTROL_2_STENCIL_OP_V_INVERT: value=~value; break;
    case NV_PGRAPH_CONTROL_2_STENCIL_OP_V_INCR: ++value; break;
    case NV_PGRAPH_CONTROL_2_STENCIL_OP_V_DECR: --value; break;
    default: note_unhandled(NV097_SET_STENCIL_OP_ZPASS,operation); return;
    }
    *pixel=(uint8_t)((*pixel & ~triangle->stencil_write_mask)|(value & triangle->stencil_write_mask));
    *storage=*pixel;
}

static uint32_t primitive_polygon_info(uint32_t a,uint32_t b,uint32_t c,uint32_t d,unsigned count)
{
    uint32_t slots[4]={a,b,c,d},edges=0,points=0;
    int connected=s_gpu.prim==NV_PRIM_TRIANGLE_STRIP || s_gpu.prim==NV_PRIM_TRIANGLE_FAN || s_gpu.prim==NV_PRIM_QUAD_STRIP;
    for(unsigned i=0;i<count;i++)if(connected || s_gpu.idx_edge[slots[i]])edges|=1u<<i;
    points=edges;
    if(s_gpu.prim==NV_PRIM_POLYGON) {
        /* Internal fan diagonals are not polygon boundaries. Each original
         * vertex is emitted once in point mode, including with blending. */
        edges&=2u|(b==1?1u:0u)|(c+1==s_gpu.idx_count?4u:0u);
        points&=4u|(b==1?3u:0u);
    }
    return count|(edges<<8)|(points<<16);
}

typedef struct { uint64_t depth_fail, pixels; uint32_t zpass; } XfCount;
#if defined(_WIN32)
#include "nv2a_d3d11_bridge.inc"
#endif

/* D3D11 aliased line diamond, in the existing Y-down window space.
 * Lower diamond edges own ties; the right corner also belongs to Y-major
 * lines. A fragment is generated only when the directed segment exits. */
static int xf_line_diamond(double u,double v,int ymajor)
{
    if(ymajor && u==.5 && v==.5)return 1;
    return u>-.5 && u<=.5 && v>=-.5 && v<.5;
}
static int xf_line_covers(const float *a,const float *b,float x,float y)
{
    double dx=(double)b[0]-a[0],dy=(double)b[1]-a[1];
    double p[2]={(double)a[0]-x+a[1]-y,(double)a[0]-x-a[1]+y};
    double d[2]={dx+dy,dx-dy},enter=0,leave=1;
    int ymajor=fabs(dy)>fabs(dx);
    for(unsigned i=0;i<2;i++) {
        if(d[i]==0) {if(p[i]<-.5 || p[i]>.5)return 0;}
        else {
            double lo=(-.5-p[i])/d[i],hi=(.5-p[i])/d[i];
            if(lo>hi){double swap=lo;lo=hi;hi=swap;}
            if(lo>enter)enter=lo;if(hi<leave)leave=hi;
        }
    }
    if(enter>leave || xf_line_diamond(p[0]+d[0],p[1]+d[1],ymajor))return 0;
    double mid=(enter+leave)*.5;
    return xf_line_diamond(p[0]+mid*d[0],p[1]+mid*d[1],ymajor) ||
        xf_line_diamond(p[0]+enter*d[0],p[1]+enter*d[1],ymajor) ||
        xf_line_diamond(p[0]+leave*d[0],p[1]+leave*d[1],ymajor);
}

/* Rows y0, y0+step, ... < maxy of triangle T. */
static void xf_stage_coordinate(const XfTri *T,unsigned stage,float l0,float l1,float l2,float pw,float coordinate[4])
{
    for(unsigned j=0;j<4;j++)coordinate[j]=(l0*T->stc[0][stage][j]+l1*T->stc[1][stage][j]+l2*T->stc[2][stage][j])/pw;
}

static void xf_rows(const XfTri *T, int y0, int step, XfCount *cnt)
{
    const float *a = T->a, *b = T->b, *c = T->c;
    const Nv2aVshOutput *va = T->va, *vb = T->vb, *vc = T->vc;
    int x, y, k;

    for (y = y0; y < T->maxy; y += step) {
        int row_min=T->minx,row_max=T->maxx;
        if(T->line && b[1]!=a[1]) {
            float lo=((float)y-a[1])/(b[1]-a[1]);
            float hi=((float)y+1-a[1])/(b[1]-a[1]);
            if(lo>hi){float swap=lo;lo=hi;hi=swap;}
            lo=fmaxf(0,lo);hi=fminf(1,hi);if(lo>hi)continue;
            float x0=a[0]+lo*(b[0]-a[0]),x1=a[0]+hi*(b[0]-a[0]);
            int left=(int)floorf(fminf(x0,x1)-.5f),right=(int)ceilf(fmaxf(x0,x1)+.5f);
            if(left>row_min)row_min=left;if(right<row_max)row_max=right;
        }
        for (x = row_min; x < row_max; x++) {
            if(T->raster_mode==0 && !polygon_stipple_pass(x,y))continue;
            int contained = 0;
            for (unsigned i = 0; i < 8; ++i)
                if (x >= (int)(T->clip_x[i] & 0xFFF) && x <= (int)((T->clip_x[i] >> 16) & 0xFFF) &&
                    y >= (int)(T->clip_y[i] & 0xFFF) && y <= (int)((T->clip_y[i] >> 16) & 0xFFF)) {
                    contained = 1;
                    break;
                }
            if (contained == T->clip_exclusive) continue;
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float l0, l1, l2, z, col[4], pw;
            uint32_t argb;
            if(T->line) {
                if(!xf_line_covers(a,b,px,py))continue;
                float dx=b[0]-a[0],dy=b[1]-a[1];
                l1=fabsf(dx)>=fabsf(dy)?(px-a[0])/dx:(py-a[1])/dy;
                l0=1-l1;l2=0;
            } else {
            float w0 = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
            float w1 = (c[0] - b[0]) * (py - b[1]) - (c[1] - b[1]) * (px - b[0]);
            float w2 = (a[0] - c[0]) * (py - c[1]) - (a[1] - c[1]) * (px - c[0]);
            float e0 = T->inv_area > 0.0f ? w0 : -w0;
            float e1 = T->inv_area > 0.0f ? w1 : -w1;
            float e2 = T->inv_area > 0.0f ? w2 : -w2;
            if (!(e0 > 0.0f || (e0 == 0.0f && (T->edge_inclusive & 1))) ||
                !(e1 > 0.0f || (e1 == 0.0f && (T->edge_inclusive & 2))) ||
                !(e2 > 0.0f || (e2 == 0.0f && (T->edge_inclusive & 4))))
                continue;
            /* w1 is opposite a, w2 opposite b, w0 opposite c. */
            l0 = w1 * T->inv_area; l1 = w2 * T->inv_area; l2 = w0 * T->inv_area;
            }
            /* Anchor the depth plane at a. A constant far-boundary depth
             * must remain exact: summing three weighted copies can round
             * above zmax and discard pixels from a full-screen clear. */
            z = a[2] + l1 * (b[2] - a[2]) + l2 * (c[2] - a[2]);
            if (T->w_depth) {
                float inverse_w = l0 * T->iw[0] + l1 * T->iw[1] + l2 * T->iw[2];
                z = inverse_w > 0.0f ? 1.0f / inverse_w : 3.402823466e+38f;
            }
            z+=T->depth_offset_bias;
            z+=T->depth_offset_slope;
            if (T->depth_clipping) {
                if (z < T->zmin || z > T->zmax) continue;
            } else {
                z = fminf(T->zmax, fmaxf(T->zmin, z));
            }
            z=depth_encode(z,T->depth_format);
            {
                uint8_t *zram=T->has_zeta?zeta_pixel(&T->zeta,(unsigned)x,(unsigned)y):NULL;
                uint32_t raw=0;if(zram){if(T->zeta.format==1){uint16_t value;memcpy(&value,zram,2);raw=value;}else memcpy(&raw,zram,4);}
                float stored=(float)(T->zeta.format==1?raw:raw>>8);uint8_t stencil_value=(uint8_t)raw;
                float *zp=T->has_zeta?&stored:NULL;
                int shade = (s_gpu.color_mask & 0x01010101u) || s_gpu.alpha_test || T->alpha_kill;
                uint8_t *sp=T->stencil_test?&stencil_value:NULL;
                int stencil_pass=!sp || compare_pass(T->stencil_func,
                    (float)(T->stencil_ref & T->stencil_mask),(float)(*sp & T->stencil_mask));
                int depth_pass=!zp || !s_gpu.depth_test || compare_pass(s_gpu.depth_func,z,*zp);
                unsigned stencil_op=!stencil_pass?T->stencil_fail:!depth_pass?T->stencil_zfail:T->stencil_zpass;
                /* Alpha rejection suppresses all stencil writes, including
                 * stencil/depth-fail operations. Keep early rejection when
                 * the fragment cannot have such a side effect. */
                int late_stencil=sp && T->stencil_write_mask && (s_gpu.alpha_test || T->alpha_kill);
                if ((!stencil_pass || !depth_pass) && !late_stencil) {
                    xf_stencil_write(T,sp,stencil_op,zram);
                    if (stencil_pass && !depth_pass) cnt->depth_fail++;
                    continue;
                }
                argb = 0;
                if (shade) {
                    for (k = 0; k < 4; k++)
                        col[k] = l0 * va->d0[k] + l1 * vb->d0[k] + l2 * vc->d0[k];
                    pw = l0 * T->iw[0] + l1 * T->iw[1] + l2 * T->iw[2];
                    if (T->use_rc) {
                        float d1[4], fog[4], t[4][4], out[4], tc[4];RcStageDots dots;
                        int st, keep = 1;
                        for (k = 0; k < 4; k++)
                            d1[k] = l0 * va->d1[k] + l1 * vb->d1[k] + l2 * vc->d1[k];
                        fog[0] = T->fogc[0]; fog[1] = T->fogc[1]; fog[2] = T->fogc[2];
                        fog[3] = l0 * T->vfog[0] + l1 * T->vfog[1] + l2 * T->vfog[2];
                        for (st = 0; st < 4 && keep; st++) {
                            if (!((s_gpu.rc.stage_program >> (st * 5)) & 0x1F)) {
                                t[st][0] = t[st][1] = t[st][2] = 0.0f;
                                t[st][3] = 1.0f;
                                continue;
                            }
                            if(st==3 && ((s_gpu.rc.stage_program>>10)&31)==11)memcpy(tc,dots.next_coordinate,sizeof(tc));
                            else xf_stage_coordinate(T,st,l0,l1,l2,pw,tc);
                            if(st==2 && ((s_gpu.rc.stage_program>>10)&31)==11)xf_stage_coordinate(T,3,l0,l1,l2,pw,dots.next_coordinate);
                            keep = rc_stage_fetch(st, tc, (const float (*)[4])t,&dots,t[st]);
                        }
                        if (!keep)
                            continue;
                        nv2a_rc_eval(&s_gpu.rc, col, d1, fog,
                                     (const float (*)[4])t, out);
                        if (s_gpu.alpha_test && !alpha_test_pass(out[3]))
                            continue;
                        argb = pack_color(out);
                    } else {
                        argb = pack_color(col);
                        if (T->textured) {
                            uint32_t texel;
                            float tu, tv;
                            tu = (l0 * T->uv[0][0] + l1 * T->uv[1][0] + l2 * T->uv[2][0]) / pw;
                            tv = (l0 * T->uv[0][1] + l1 * T->uv[1][1] + l2 * T->uv[2][1]) / pw;
                            if (sample_texture((uint32_t)(int32_t)floorf(tu),
                                               (uint32_t)(int32_t)floorf(tv), &texel)) {
                                float sampled[4];nv2a_rc_unpack(texel,sampled);
                                if(!texture_post_sample(0,sampled))continue;
                                argb = modulate(pack_color(sampled), col);
                            }
                        }
                        if (s_gpu.alpha_test
                            && !alpha_test_pass((float)(argb >> 24) / 255.0f))
                            continue;
                    }
                }
                xf_stencil_write(T,sp,stencil_op,zram);
                if (!stencil_pass || !depth_pass) {
                    if (stencil_pass && !depth_pass) cnt->depth_fail++;
                    continue;
                }
                if (zp && s_gpu.depth_test && s_gpu.depth_mask)
                    zeta_write_depth(&T->zeta,zram,(uint32_t)z);
                if (s_gpu.zpass_enable)
                    cnt->zpass++;
                if (!(s_gpu.color_mask & 0x01010101u))
                    continue;              /* colour writes off: depth only */
                cnt->pixels++;
            }
            put_pixel(T->mem, T->bpp, x, y, argb);
        }
    }
}

/* Worker threads for big triangles.
 *
 * The rasteriser is a software GPU, and a Burnout 3 frame is dominated by a
 * handful of full-screen passes -- composites, blurs, the menu backdrops --
 * each two triangles of 300,000 pixels through the register combiners. Those
 * split cleanly by row: every pixel reads only its own depth and colour, so
 * interleaved rows on N threads need no locking. Small triangles stay on the
 * executor thread, where waking workers would cost more than it saves.
 * RECOMP_RASTER_THREADS=<n> sets the count (1 = off); the default leaves a
 * few cores for the title and the host.
 * ponytail: s_gpu.pixels/pixel_max in put_pixel are unsynchronised stats and
 * may undercount; nothing depends on them. */
#define NV_RASTER_MAX_THREADS 16
#define NV_RASTER_MT_MIN_PIXELS 8192

#if defined(_WIN32)
static uint64_t gpu_timing_clock(void)
{
    static int enabled = -1;
    LARGE_INTEGER value;
    if (enabled < 0) {
        enabled = getenv("RECOMP_GPU_TIMING") != NULL;
        if (enabled) {
            if (!QueryPerformanceFrequency(&value) || value.QuadPart <= 0) {
                fprintf(stderr, "[STOP] GPU timing frequency unavailable\n");
                exit(5);
            }
            s_gpu.timing_frequency = (uint64_t)value.QuadPart;
        }
    }
    if (!enabled) return 0;
    if (!QueryPerformanceCounter(&value)) {
        fprintf(stderr, "[STOP] GPU timing counter unavailable\n");
        exit(5);
    }
    return (uint64_t)value.QuadPart;
}

static void gpu_timing_finish(uint64_t start, uint64_t *total)
{
    if (s_gpu.timing_frequency) *total += gpu_timing_clock() - start;
}

static struct {
    int n;                                  /* threads incl. the caller */
    HANDLE start[NV_RASTER_MAX_THREADS], done;
    volatile LONG pending;
    const XfTri *tri;
    XfCount cnt[NV_RASTER_MAX_THREADS];
} s_pool;

static DWORD WINAPI raster_worker(LPVOID arg)
{
    int k = (int)(intptr_t)arg;
    for (;;) {
        WaitForSingleObject(s_pool.start[k], INFINITE);
        memset(&s_pool.cnt[k], 0, sizeof s_pool.cnt[k]);
        xf_rows(s_pool.tri, s_pool.tri->miny + k, s_pool.n, &s_pool.cnt[k]);
        if (InterlockedDecrement(&s_pool.pending) == 0)
            SetEvent(s_pool.done);
    }
    return 0;
}

static int raster_pool_size(void)
{
    static int init;
    if (!init) {
        const char *e = getenv("RECOMP_RASTER_THREADS");
        SYSTEM_INFO si;
        int n, k;
        init = 1;
        GetSystemInfo(&si);
        n = e ? atoi(e) : (int)si.dwNumberOfProcessors - 4;
        if (n < 1) n = 1;
        if (n > NV_RASTER_MAX_THREADS) n = NV_RASTER_MAX_THREADS;
        s_pool.n = n;
        if (n > 1) {
            s_pool.done = CreateEventA(NULL, FALSE, FALSE, NULL);
            for (k = 0; k < n - 1; k++) {
                s_pool.start[k] = CreateEventA(NULL, FALSE, FALSE, NULL);
                CloseHandle(CreateThread(NULL, 0, raster_worker,
                                         (LPVOID)(intptr_t)k, 0, NULL));
            }
        }
    }
    return s_pool.n;
}

static void xf_rows_parallel(const XfTri *T, XfCount *total)
{
    int n = raster_pool_size(), k;
    long px = (long)(T->maxx - T->minx) * (T->maxy - T->miny);
    uint64_t timing_start = gpu_timing_clock();

    if (n <= 1 || px < NV_RASTER_MT_MIN_PIXELS || T->maxy - T->miny < n) {
        xf_rows(T, T->miny, 1, total);
        gpu_timing_finish(timing_start, &s_gpu.program_raster_ticks);
        return;
    }
    s_pool.tri = T;
    s_pool.pending = n - 1;
    for (k = 0; k < n - 1; k++)
        SetEvent(s_pool.start[k]);
    {
        XfCount mine = {0, 0, 0};
        xf_rows(T, T->miny + (n - 1), n, &mine);
        uint64_t wait_start = gpu_timing_clock();
        WaitForSingleObject(s_pool.done, INFINITE);
        gpu_timing_finish(wait_start, &s_gpu.raster_wait_ticks);
        total->depth_fail += mine.depth_fail;
        total->pixels += mine.pixels;
        total->zpass += mine.zpass;
    }
    for (k = 0; k < n - 1; k++) {
        total->depth_fail += s_pool.cnt[k].depth_fail;
        total->pixels += s_pool.cnt[k].pixels;
        total->zpass += s_pool.cnt[k].zpass;
    }
    gpu_timing_finish(timing_start, &s_gpu.program_raster_ticks);
}
#else
static void xf_rows_parallel(const XfTri *T, XfCount *total)
{
    xf_rows(T, T->miny, 1, total);
}
#endif

static float xf_polygon_depth_slope(const float *a,const float *b,const float *c,float inv_area,int w_depth)
{
    if(w_depth) {
        /* A finite constant W field has zero slope for every sampling grid. */
        if(isfinite(a[3]) && a[3]==b[3] && a[3]==c[3])return 0;
        fprintf(stderr,"[STOP] W-buffer polygon slope convention is not implemented\n");exit(5);
    }
    float db=b[2]-a[2],dc=c[2]-a[2];
    float dx=(db*(c[1]-a[1])-dc*(b[1]-a[1]))*inv_area;
    float dy=(dc*(b[0]-a[0])-db*(c[0]-a[0]))*inv_area;
    return fmaxf(fabsf(dx),fabsf(dy));
}

static void raster_xf_triangle_mode(const Nv2aVshOutput *va,const Nv2aVshOutput *vb,
                                    const Nv2aVshOutput *vc,unsigned raster_mode,int face_checked,float polygon_slope)
{
    XfTri T;
    Nv2aVshOutput back[3];
    XfCount cnt = {0, 0, 0};
    const Nv2aVshOutput *v[3];
    const float *a, *b, *c;
    float setup_pos[3][4];
    float area, su = 1.0f, sv = 1.0f;
    int k;
    static int no_rc = -1;

    if (no_rc < 0)
        no_rc = getenv("RECOMP_NO_COMBINERS") != NULL;
    memset(&T, 0, sizeof T);
    T.raster_mode=raster_mode;
    T.line=raster_mode>=4;
    T.mem = (uint8_t *)xbox_GetMemoryOffset();
    T.bpp = surface_bpp();
    T.textured = s_gpu.texs[0].valid;
    T.use_rc = s_gpu.rc_seen && !no_rc;
    /* xemu's texture-stage rule discards zero sampled alpha before the
     * combiner and any depth write, including a depth-only draw. */
    for (unsigned stage = 0; stage < 4; ++stage) {
        unsigned mode = (s_gpu.rc.stage_program >> (stage * 5)) & 0x1F;
        if ((texture_alpha_kill_enabled(stage) || texture_color_key_mode(stage)==3) &&
            (T.use_rc ? texture_stage_samples(mode) : stage == 0 && T.textured))
            T.alpha_kill = 1;
    }
    T.va = va; T.vb = vb; T.vc = vc;

    v[0] = va; v[1] = vb; v[2] = vc;
    /* NV2A setup has four fractional bits, truncated toward zero (the pinned
     * xemu roundScreenCoords rule). NINE's adjoining atlas panels otherwise
     * keep .53125 positions and expose transparent padding at their join. */
    for (unsigned i = 0; i < 3; ++i) {
        memcpy(setup_pos[i], v[i]->pos, sizeof(setup_pos[i]));
        setup_pos[i][0] = truncf(setup_pos[i][0] * 16.0f) / 16.0f;
        setup_pos[i][1] = truncf(setup_pos[i][1] * 16.0f) / 16.0f;
    }
    a = setup_pos[0]; b = setup_pos[1]; c = setup_pos[2];
    T.a = a; T.b = b; T.c = c;
    if (T.bpp != 4 && T.bpp != 2)
        return;
    if (a[3] <= 0.0f || b[3] <= 0.0f || c[3] <= 0.0f) {
        s_gpu.tris_behind++;
        return;
    }
    area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    if(T.line) {
        if(a[0]==b[0] && a[1]==b[1])return;
        area=1;
    }
    if (area == 0.0f || area != area) {
        s_gpu.xf_degenerate++;
        return;
    }
    /* Top/left edges own exact samples; the adjoining triangle excludes
     * the reversed edge, so alpha-blended quads shade their diagonal once. */
    for (unsigned i = 0; i < 3; ++i) {
        const float *p = setup_pos[i], *q = setup_pos[(i + 1) % 3];
        if (area < 0.0f) { const float *swap = p; p = q; q = swap; }
        if (q[1] < p[1] || (q[1] == p[1] && q[0] > p[0]))
            T.edge_inclusive |= 1u << i;
    }
    {
        struct PGRAPHState *pg = &nv2a_get_state()->pgraph;
        uint32_t raster = pg->regs[NV_PGRAPH_SETUPRASTER];
        int front = (area < 0) == ((raster & NV_PGRAPH_SETUPRASTER_FRONTFACE) != 0);
        if (!face_checked && s_gpu.prim>=NV_PRIM_TRIANGLES && (raster & NV_PGRAPH_SETUPRASTER_CULLENABLE)) {
            uint32_t face = GET_MASK(raster, NV_PGRAPH_SETUPRASTER_CULLCTRL);
            if (face == NV_PGRAPH_SETUPRASTER_CULLCTRL_FRONT_AND_BACK ||
                (front && face == NV_PGRAPH_SETUPRASTER_CULLCTRL_FRONT) ||
                (!front && face == NV_PGRAPH_SETUPRASTER_CULLCTRL_BACK)) return;
        }
        if(!face_checked && s_gpu.prim>=NV_PRIM_TRIANGLES && !front && (pg->regs[NV_PGRAPH_CSV0_C]&NV_PGRAPH_CSV0_C_LIGHT_TWO_SIDE_ENABLE)) {
            for(unsigned i=0;i<3;i++){back[i]=*v[i];memcpy(back[i].d0,v[i]->b0,16);memcpy(back[i].d1,v[i]->b1,16);v[i]=&back[i];}
            T.va=v[0];T.vb=v[1];T.vc=v[2];
        }
        T.clip_exclusive = (raster & NV_PGRAPH_SETUPRASTER_WINDOWCLIPTYPE) != 0;
        T.w_depth = (pg->regs[NV_PGRAPH_CONTROL_0] & NV_PGRAPH_CONTROL_0_Z_PERSPECTIVE_ENABLE) != 0;
        T.depth_format=(s_gpu.format>>4)&15;
        if (raster & NV_PGRAPH_SETUPRASTER_Z_FORMAT)
            T.depth_format=T.depth_format==1?16:24;
        T.depth_clipping = GET_MASK(pg->regs[NV_PGRAPH_ZCOMPRESSOCCLUDE],
                                   NV_PGRAPH_ZCOMPRESSOCCLUDE_ZCLAMP_EN) ==
                           NV_PGRAPH_ZCOMPRESSOCCLUDE_ZCLAMP_EN_CULL;
        memcpy(&T.zmin, &pg->regs[NV_PGRAPH_ZCLIPMIN], sizeof(T.zmin));
        memcpy(&T.zmax, &pg->regs[NV_PGRAPH_ZCLIPMAX], sizeof(T.zmax));
        for (unsigned i = 0; i < 8; ++i) {
            T.clip_x[i] = pg->regs[NV_PGRAPH_WINDOWCLIPX0 + i * 4];
            T.clip_y[i] = pg->regs[NV_PGRAPH_WINDOWCLIPY0 + i * 4];
        }
    }
    for (k = 0; k < 3; k++) {
        if (!s_gpu.xf_seeded)
            s_gpu.xf_min[k] = s_gpu.xf_max[k] = a[k];
        if (a[k] < s_gpu.xf_min[k]) s_gpu.xf_min[k] = a[k];
        if (a[k] > s_gpu.xf_max[k]) s_gpu.xf_max[k] = a[k];
    }
    s_gpu.xf_seeded = 1;
    if (!surface_begin_batch(T.mem))
        return;
    uint32_t stencil_control = nv2a_get_state()->pgraph.regs[NV_PGRAPH_CONTROL_1];
    int stencil_test = (stencil_control & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE) != 0;
    if (s_gpu.depth_test || stencil_test) {
        if (stencil_test && ((s_gpu.format >> 4) & 0xF) != 2)
            note_unhandled(NV097_SET_STENCIL_TEST_ENABLE, 1);
#if defined(_WIN32)
        if (!hw_enabled())
#endif
        {
            T.zeta=zeta_surface();T.has_zeta=1;
            if(T.maxx>(int)T.zeta.width || T.maxy>(int)T.zeta.height)note_unhandled(NV097_SET_SURFACE_CLIP_VERTICAL,s_gpu.clip_h);
        }
        if (stencil_test) {
            T.stencil_test=T.has_zeta;
            T.stencil_func = 0x200 | GET_MASK(stencil_control, NV_PGRAPH_CONTROL_1_STENCIL_FUNC);
            T.stencil_ref = GET_MASK(stencil_control, NV_PGRAPH_CONTROL_1_STENCIL_REF);
            T.stencil_mask = GET_MASK(stencil_control, NV_PGRAPH_CONTROL_1_STENCIL_MASK_READ);
            struct PGRAPHState *pg=&nv2a_get_state()->pgraph;
            if (pg->regs[NV_PGRAPH_CONTROL_0] & NV_PGRAPH_CONTROL_0_STENCIL_WRITE_ENABLE)
                T.stencil_write_mask=GET_MASK(stencil_control,NV_PGRAPH_CONTROL_1_STENCIL_MASK_WRITE);
            T.stencil_fail=GET_MASK(pg->regs[NV_PGRAPH_CONTROL_2],NV_PGRAPH_CONTROL_2_STENCIL_OP_FAIL);
            T.stencil_zfail=GET_MASK(pg->regs[NV_PGRAPH_CONTROL_2],NV_PGRAPH_CONTROL_2_STENCIL_OP_ZFAIL);
            T.stencil_zpass=GET_MASK(pg->regs[NV_PGRAPH_CONTROL_2],NV_PGRAPH_CONTROL_2_STENCIL_OP_ZPASS);
        }
    }
    if (tex_size_from_format(s_gpu.texs[0].color)) {
        su = (float)s_gpu.texs[0].width;
        sv = (float)s_gpu.texs[0].height;
    }
    for (k = 0; k < 3; k++) {
        T.iw[k] = 1.0f / v[k]->pos[3];
        T.uv[k][0] = v[k]->tex[0][0] * su * T.iw[k];
        T.uv[k][1] = v[k]->tex[0][1] * sv * T.iw[k];
    }
    if (T.use_rc) {
        int st, j;
        for (k = 0; k < 3; k++) {
            for (st = 0; st < 4; st++)
                for (j = 0; j < 4; j++)
                    T.stc[k][st][j] = v[k]->tex[st][j] * T.iw[k];
            T.vfog[k] = fog_factor(v[k]->fog[0]);
        }
        /* FOG_COLOR is R in bits 0-7, the reverse of a D3DCOLOR. */
        nv2a_rc_unpack(s_gpu.fog_color, T.fogc);
        { float r = T.fogc[2]; T.fogc[2] = T.fogc[0]; T.fogc[0] = r; }
    }

    T.minx = (int)floorf(fminf(a[0], fminf(b[0], c[0])));
    T.maxx = (int)ceilf (fmaxf(a[0], fmaxf(b[0], c[0])));
    T.miny = (int)floorf(fminf(a[1], fminf(b[1], c[1])));
    T.maxy = (int)ceilf (fmaxf(a[1], fmaxf(b[1], c[1])));
    if(T.line) {
        T.minx=(int)floorf(fminf(a[0],b[0])-.5f);T.maxx=(int)ceilf(fmaxf(a[0],b[0])+.5f);
        T.miny=(int)floorf(fminf(a[1],b[1])-.5f);T.maxy=(int)ceilf(fmaxf(a[1],b[1])+.5f);
    }
    if (T.minx < (int)s_gpu.clip_x) T.minx = (int)s_gpu.clip_x;
    if (T.miny < (int)s_gpu.clip_y) T.miny = (int)s_gpu.clip_y;
    if (T.maxx > (int)(s_gpu.clip_x + s_gpu.clip_w)) T.maxx = (int)(s_gpu.clip_x + s_gpu.clip_w);
    if (T.maxy > (int)(s_gpu.clip_y + s_gpu.clip_h)) T.maxy = (int)(s_gpu.clip_y + s_gpu.clip_h);
    if (T.maxx > NV_ZBUF_W) T.maxx = NV_ZBUF_W;
    if (T.maxy > NV_ZBUF_H) T.maxy = NV_ZBUF_H;
    if (T.minx >= T.maxx || T.miny >= T.maxy) {
        s_gpu.tris_skipped_offscreen++;
        s_gpu.xf_offscreen++;
        return;
    }
    s_gpu.xf_drawn++;
    T.inv_area = 1.0f / area;

    uint32_t offset_enable=raster_mode==1?NV_PGRAPH_SETUPRASTER_POFFSETPOINTENABLE:(raster_mode==2 || raster_mode==5)?NV_PGRAPH_SETUPRASTER_POFFSETLINEENABLE:NV_PGRAPH_SETUPRASTER_POFFSETFILLENABLE;
    if((raster_mode<3 || raster_mode==5) && s_gpu.prim>NV_PRIM_LINE_STRIP &&
       (nv2a_get_state()->pgraph.regs[NV_PGRAPH_SETUPRASTER]&offset_enable)) {
        struct PGRAPHState *pg=&nv2a_get_state()->pgraph;
        float factor;memcpy(&factor,&pg->regs[NV_PGRAPH_ZOFFSETFACTOR],4);
        memcpy(&T.depth_offset_bias,&pg->regs[NV_PGRAPH_ZOFFSETBIAS],4);
        if(factor!=0) {
            if(T.w_depth && !isfinite(factor)){fprintf(stderr,"[STOP] W-buffer polygon slope convention is not implemented\n");exit(5);}
            T.depth_offset_slope=factor*((raster_mode==1 || raster_mode==5)?polygon_slope:xf_polygon_depth_slope(a,b,c,T.inv_area,T.w_depth));
        }
    }

#if defined(_WIN32)
    if (hw_enabled()) hw_triangle(&T);
    else
#endif
    xf_rows_parallel(&T, &cnt);
    s_gpu.xf_depth_fail += cnt.depth_fail;
    s_gpu.xf_pixels += cnt.pixels;
    s_gpu.zpass_count += cnt.zpass;
    s_gpu.tris_drawn++;
    note_drawn();
}


static void raster_xf_triangle(const Nv2aVshOutput *a,const Nv2aVshOutput *b,const Nv2aVshOutput *c)
{
    raster_xf_triangle_mode(a,b,c,0,0,0);
}

/* Near-plane clipping.
 *
 * A triangle with a vertex behind the eye (w <= 0) used to be dropped whole.
 * That is invisible for distant geometry and ruinous close up: in a chase
 * view the road under the camera is exactly the set of triangles that reach
 * behind it, so the bottom of the frame showed a hole -- a quarter of every
 * race frame's triangles went this way.
 *
 * Clipping has to happen in clip space, but the D3D epilogue has already
 * divided x, y and z by w and applied the viewport (c[58] scale, c[59]
 * offset) while leaving w itself alone. So each vertex is taken back to clip
 * space first: clip = (screen - offset) / scale * w. Everything else the
 * vertex carries -- colours, fog, texture coordinates -- is linear in clip
 * space and is interpolated directly. The polygon is clipped against
 * w = NV_CLIP_W (Sutherland-Hodgman, one plane), re-projected, and fanned.
 * ponytail: the near plane only; x/y/far fall to the rasteriser's bounds
 * check, which is correct, just slower for huge off-screen triangles. */
#define NV_CLIP_W 1e-3f

static void xf_to_clip(const Nv2aVshOutput *v, const float k[3],
                       const float o[3], float c[3])
{
    int i;
    for (i = 0; i < 3; i++)
        c[i] = k[i] != 0.0f ? (v->pos[i] - o[i]) / k[i] * v->pos[3] : 0.0f;
}

static void xf_lerp(const Nv2aVshOutput *a, const Nv2aVshOutput *b, float t,
                    Nv2aVshOutput *out)
{
    const float *pa = (const float *)a, *pb = (const float *)b;
    float *po = (float *)out;
    size_t i, n = sizeof(Nv2aVshOutput) / sizeof(float);
    for (i = 0; i < n; i++)
        po[i] = pa[i] + (pb[i] - pa[i]) * t;
}

static unsigned xf_clip_polygon_w(const Nv2aVshOutput input[4],unsigned count,Nv2aVshOutput output[9],unsigned edges[9],unsigned points[9])
{
    int clipped=0;for(unsigned i=0;i<count;i++)if(input[i].pos[3]<=NV_CLIP_W)clipped=1;
    if(!clipped){for(unsigned i=0;i<count;i++)output[i]=input[i];return count;}
    const float *k=nv2a_vsh_constant(58),*o=nv2a_vsh_constant(59);unsigned n=0,oe[9]={0},op[9]={0};
    for(unsigned i=0;i<count;i++) {
        const Nv2aVshOutput *p=&input[i],*q=&input[(i+1)%count];int pin=p->pos[3]>NV_CLIP_W,qin=q->pos[3]>NV_CLIP_W;
        Nv2aVshOutput a=*p,b=*q;float ca[3],cb[3];xf_to_clip(p,k,o,ca);xf_to_clip(q,k,o,cb);
        for(unsigned j=0;j<3;j++){a.pos[j]=ca[j];b.pos[j]=cb[j];}
        if(pin){output[n]=a;oe[n]=edges[i];op[n]=points[i];n++;}
        if(pin!=qin){xf_lerp(&a,&b,(NV_CLIP_W-p->pos[3])/(q->pos[3]-p->pos[3]),&output[n]);oe[n]=pin?1:edges[i];op[n]=edges[i];n++;}
    }
    for(unsigned i=0;i<n;i++){for(unsigned j=0;j<3;j++)output[i].pos[j]=output[i].pos[j]/output[i].pos[3]*k[j]+o[j];edges[i]=oe[i];points[i]=op[i];}
    return n;
}

static unsigned xf_clip_polygon_xy(Nv2aVshOutput poly[9],unsigned edges[9],unsigned points[9],unsigned count,unsigned axis,float boundary,int lower)
{
    Nv2aVshOutput output[9];unsigned oe[9]={0},op[9]={0},n=0;
    for(unsigned i=0;i<count;i++) {
        Nv2aVshOutput a=poly[i],b=poly[(i+1)%count];float da=lower?a.pos[axis]-boundary:boundary-a.pos[axis],db=lower?b.pos[axis]-boundary:boundary-b.pos[axis];int ain=da>=0,bin=db>=0;
        if(ain){output[n]=a;oe[n]=edges[i];op[n]=points[i];n++;}
        if(ain && !bin && da==0)oe[n-1]=1;
        if(ain!=bin && da!=0 && db!=0) {
            float t=da/(da-db),clip_t=t*a.pos[3]/((1-t)*b.pos[3]+t*a.pos[3]);
            for(unsigned j=0;j<3;j++){a.pos[j]*=a.pos[3];b.pos[j]*=b.pos[3];}
            xf_lerp(&a,&b,clip_t,&output[n]);for(unsigned j=0;j<3;j++)output[n].pos[j]/=output[n].pos[3];output[n].pos[axis]=boundary;
            oe[n]=ain?1:edges[i];op[n]=edges[i];n++;
        }
    }
    for(unsigned i=0;i<n;i++){poly[i]=output[i];edges[i]=oe[i];points[i]=op[i];}return n;
}

static void raster_xf_clipped(const Nv2aVshOutput *a, const Nv2aVshOutput *b,
                              const Nv2aVshOutput *c, const Nv2aVshOutput *provoking)
{
    Nv2aVshOutput flat[3];
    if (!(nv2a_get_state()->pgraph.regs[NV_PGRAPH_CONTROL_3] & NV_PGRAPH_CONTROL_3_SHADEMODE)) {
        flat[0] = *a; flat[1] = *b; flat[2] = *c;
        for (unsigned i = 0; i < 3; ++i) {
            memcpy(flat[i].d0, provoking->d0, sizeof(flat[i].d0));
            memcpy(flat[i].d1, provoking->d1, sizeof(flat[i].d1));
            memcpy(flat[i].b0, provoking->b0, sizeof(flat[i].b0));
            memcpy(flat[i].b1, provoking->b1, sizeof(flat[i].b1));
        }
        a = &flat[0]; b = &flat[1]; c = &flat[2];
    }
    if(a->pos[3]>NV_CLIP_W && b->pos[3]>NV_CLIP_W && c->pos[3]>NV_CLIP_W){raster_xf_triangle(a,b,c);return;}
    if(a->pos[3]<=NV_CLIP_W && b->pos[3]<=NV_CLIP_W && c->pos[3]<=NV_CLIP_W){s_gpu.tris_behind++;return;}
    Nv2aVshOutput input[4]={*a,*b,*c},poly[9];unsigned edges[9]={1,1,1},points[9]={1,1,1};
    unsigned count=xf_clip_polygon_w(input,3,poly,edges,points);
    for(unsigned i=1;i+1<count;i++)raster_xf_triangle(&poly[0],&poly[i],&poly[i+1]);
}

static Nv2aVshOutput s_xf[NV_MAX_INDICES];
static uint32_t s_xf_positions[NV_MAX_INDICES];

static int transform_vertex(uint32_t index, uint32_t inputs, Nv2aVshOutput *out)
{
    float in[NV2A_VSH_INPUTS][4];
    uint32_t a;

    if (s_ftrace == 2) inputs |= (1u << 0) | (1u << 9);
    for (a = 0; a < NV2A_VSH_INPUTS; a++)
        if (inputs & (1u << a))
            fetch_attr(&s_gpu.attr[a], index, in[a]);   /* disabled arrays: current registers */
    int fixed=(s_gpu.xf_mode&3)==0;
    int result = fixed ? nv2a_vsh_fixed_run((const float (*)[4])in,&nv2a_get_state()->pgraph,out) : nv2a_vsh_run((const float (*)[4])in, out);
    if(fixed && !result) { fprintf(stderr,"[STOP] fixed software vertex mode or texgen requires consumer\n");exit(5); }
    if (s_ftrace == 2 && result && !isfinite(out->pos[3])) {
        static unsigned invalid_w_traces;
        if (invalid_w_traces++ < 16) {
            fprintf(stderr, "[FTRACE] nonfinite W input0=%g,%g,%g,%g input9=%g,%g,%g,%g attr0 type=%u size=%u stride=%u offset=%08X\n",
                    in[0][0], in[0][1], in[0][2], in[0][3],
                    in[9][0], in[9][1], in[9][2], in[9][3],
                    s_gpu.attr[0].type, s_gpu.attr[0].size,
                    s_gpu.attr[0].stride, s_gpu.attr[0].offset);
            for (unsigned ci = 0; ci < 2; ci++) {
                const float *cv = nv2a_vsh_constant(ci);
                fprintf(stderr, "[FTRACE] screen c[%u]=%g,%g,%g,%g\n",
                        ci, cv[0], cv[1], cv[2], cv[3]);
            }
        }
    }
    return result;
}

static void raster_vertex_point(const Nv2aVshOutput *input,unsigned mode,float polygon_slope)
{
    struct PGRAPHState *pg=&nv2a_get_state()->pgraph;
    float size=(pg->regs[NV_PGRAPH_CSV0_D]&NV_PGRAPH_CSV0_D_POINTPARAMSENABLE)?input->point_size:pg->regs[NV_PGRAPH_POINTSIZE]/8.0f;
    for(unsigned i=0;i<4;i++)if(!isfinite(input->pos[i]))return;
    if(!isfinite(size) || input->pos[3]<=0)return;
    float width=(float)color_width(s_gpu.format,s_gpu.pitch,surface_bpp()),height=(float)color_height(s_gpu.format,s_gpu.clip_y+s_gpu.clip_h);
    float x=truncf(input->pos[0]*16)/16,y=truncf(input->pos[1]*16)/16;
    if(x<0 || x>width || y<0 || y>height)return;
    size=fmaxf(1,floorf(size+.5f));
    if((unsigned)size&1){x=floorf(x)+.5f;y=floorf(y)+.5f;}else{x=floorf(x+.5f);y=floorf(y+.5f);}
    Nv2aVshOutput corner[4];
    for(unsigned i=0;i<4;i++) {
        corner[i]=*input;float u=(float)(i&1),v=(float)(i>>1);
        corner[i].pos[0]=x+(u-.5f)*size;corner[i].pos[1]=y+(v-.5f)*size;
        if(pg->regs[NV_PGRAPH_SETUPRASTER]&NV_PGRAPH_SETUPRASTER_POINTSMOOTHENABLE) {
            float su=1,sv=1;if(!tex_size_from_format(s_gpu.texs[3].color)){su=(float)s_gpu.texs[3].width;sv=(float)s_gpu.texs[3].height;}
            corner[i].tex[3][0]=u*su;corner[i].tex[3][1]=v*sv;corner[i].tex[3][2]=corner[i].tex[3][3]=1;
        }
    }
    raster_xf_triangle_mode(&corner[0],&corner[1],&corner[2],mode,1,polygon_slope);
    raster_xf_triangle_mode(&corner[2],&corner[1],&corner[3],mode,1,polygon_slope);
}

static void raster_vertex_line(const Nv2aVshOutput *va,const Nv2aVshOutput *vb,const Nv2aVshOutput *provoking,unsigned polygon,float polygon_slope);

static void raster_software_polygon(uint32_t a,uint32_t b,uint32_t c,uint32_t d,unsigned count,uint32_t provoking)
{
    struct PGRAPHState *pg=&nv2a_get_state()->pgraph;uint32_t info=primitive_polygon_info(a,b,c,d,count),raster=pg->regs[NV_PGRAPH_SETUPRASTER];
    uint32_t slots[4]={a,b,c,d};Nv2aVshOutput input[4],poly[9];unsigned edges[9]={0},points[9]={0};
    for(unsigned i=0;i<count;i++) {
        input[i]=s_xf[slots[i]];edges[i]=(info>>(8+i))&1;points[i]=(info>>(16+i))&1;
        if(!(pg->regs[NV_PGRAPH_CONTROL_3]&NV_PGRAPH_CONTROL_3_SHADEMODE)) {
            memcpy(input[i].d0,s_xf[provoking].d0,16);memcpy(input[i].d1,s_xf[provoking].d1,16);memcpy(input[i].b0,s_xf[provoking].b0,16);memcpy(input[i].b1,s_xf[provoking].b1,16);
        }
    }
    unsigned n=xf_clip_polygon_w(input,count,poly,edges,points);
    float width=(float)color_width(s_gpu.format,s_gpu.pitch,surface_bpp()),height=(float)color_height(s_gpu.format,s_gpu.clip_y+s_gpu.clip_h);
    n=xf_clip_polygon_xy(poly,edges,points,n,0,0,1);n=xf_clip_polygon_xy(poly,edges,points,n,0,width,0);
    n=xf_clip_polygon_xy(poly,edges,points,n,1,0,1);n=xf_clip_polygon_xy(poly,edges,points,n,1,height,0);
    if(n<3)return;
    for(unsigned i=0;i<n;i++){poly[i].pos[0]=truncf(poly[i].pos[0]*16)/16;poly[i].pos[1]=truncf(poly[i].pos[1]*16)/16;}
    float area=0;unsigned plane_vertex=1;for(unsigned i=1;i+1<n;i++){plane_vertex=i;area=(poly[i].pos[0]-poly[0].pos[0])*(poly[i+1].pos[1]-poly[0].pos[1])-(poly[i].pos[1]-poly[0].pos[1])*(poly[i+1].pos[0]-poly[0].pos[0]);if(area!=0)break;}
    if(area==0 || !isfinite(area))return;int front=(area<0)==((raster&NV_PGRAPH_SETUPRASTER_FRONTFACE)!=0);
    unsigned face=GET_MASK(raster,NV_PGRAPH_SETUPRASTER_CULLCTRL);
    if((raster&NV_PGRAPH_SETUPRASTER_CULLENABLE) && (face==3 || (front && face==1) || (!front && face==2)))return;
    unsigned mode=GET_MASK(raster,front?NV_PGRAPH_SETUPRASTER_FRONTFACEMODE:NV_PGRAPH_SETUPRASTER_BACKFACEMODE);
    if(!front && (pg->regs[NV_PGRAPH_CSV0_C]&NV_PGRAPH_CSV0_C_LIGHT_TWO_SIDE_ENABLE))for(unsigned i=0;i<n;i++){memcpy(poly[i].d0,poly[i].b0,16);memcpy(poly[i].d1,poly[i].b1,16);}
    if(mode==1 || mode==2){
        float slope=0;unsigned enable=mode==1?NV_PGRAPH_SETUPRASTER_POFFSETPOINTENABLE:NV_PGRAPH_SETUPRASTER_POFFSETLINEENABLE;
        if(raster&enable) {
            float factor;memcpy(&factor,&pg->regs[NV_PGRAPH_ZOFFSETFACTOR],4);
            if(factor!=0) {
                int w_depth=(pg->regs[NV_PGRAPH_CONTROL_0]&NV_PGRAPH_CONTROL_0_Z_PERSPECTIVE_ENABLE)!=0;
                if(w_depth)for(unsigned j=1;j+1<n;j++)slope=xf_polygon_depth_slope(poly[0].pos,poly[j].pos,poly[j+1].pos,0,1);
                else slope=xf_polygon_depth_slope(poly[0].pos,poly[plane_vertex].pos,poly[plane_vertex+1].pos,1/area,0);
            }
        }
        for(unsigned i=0;i<n;i++) {
            if(mode==1 && points[i])raster_vertex_point(&poly[i],1,slope);
            if(mode==2 && edges[i])raster_vertex_line(&poly[i],&poly[(i+1)%n],&poly[i],1,slope);
        }
        return;
    }
    for(unsigned i=1;i+1<n;i++)raster_xf_triangle_mode(&poly[0],&poly[i],&poly[i+1],0,1,0);
}

static void raster_program_triangle(uint32_t a,uint32_t b,uint32_t c,uint32_t p)
{
#if defined(_WIN32)
    if(s_hw_program_active) { hw_program_triangle(a,b,c,p);return; }
#endif
    if(nv2a_get_state()->pgraph.regs[NV_PGRAPH_SETUPRASTER]&(NV_PGRAPH_SETUPRASTER_FRONTFACEMODE|NV_PGRAPH_SETUPRASTER_BACKFACEMODE)){raster_software_polygon(a,b,c,0,3,p);return;}
    raster_xf_clipped(&s_xf[a],&s_xf[b],&s_xf[c],&s_xf[p]);
}

static void raster_vertex_line(const Nv2aVshOutput *va,const Nv2aVshOutput *vb,const Nv2aVshOutput *provoking,unsigned polygon,float polygon_slope)
{
    Nv2aVshOutput a=*va,b=*vb;
    if(!polygon && !(nv2a_get_state()->pgraph.regs[NV_PGRAPH_CONTROL_3]&NV_PGRAPH_CONTROL_3_SHADEMODE)) {
        memcpy(a.d0,provoking->d0,16);memcpy(b.d0,provoking->d0,16);
        memcpy(a.d1,provoking->d1,16);memcpy(b.d1,provoking->d1,16);
    }
    if(!polygon && a.pos[3]<=NV_CLIP_W && b.pos[3]<=NV_CLIP_W)return;
    if(!polygon && (a.pos[3]<=NV_CLIP_W || b.pos[3]<=NV_CLIP_W)) {
        const float *k=nv2a_vsh_constant(58),*o=nv2a_vsh_constant(59);float ca[3],cb[3];
        xf_to_clip(&a,k,o,ca);xf_to_clip(&b,k,o,cb);Nv2aVshOutput aa=a,bb=b,c;
        for(unsigned j=0;j<3;j++){aa.pos[j]=ca[j];bb.pos[j]=cb[j];}
        xf_lerp(&aa,&bb,(NV_CLIP_W-a.pos[3])/(b.pos[3]-a.pos[3]),&c);
        for(unsigned j=0;j<3;j++)c.pos[j]=c.pos[j]/c.pos[3]*k[j]+o[j];
        if(a.pos[3]<=NV_CLIP_W)a=c;else b=c;
    }
    a.pos[0]=truncf(a.pos[0]*16)/16;a.pos[1]=truncf(a.pos[1]*16)/16;
    b.pos[0]=truncf(b.pos[0]*16)/16;b.pos[1]=truncf(b.pos[1]*16)/16;
#if defined(_WIN32)
    if(hw_enabled()){hw_fixed_line(&a,&b);return;}
#endif
    unsigned width=(unsigned)floorf(GET_MASK(nv2a_get_state()->pgraph.regs[NV_PGRAPH_SETUPRASTER],NV_PGRAPH_SETUPRASTER_LINEWIDTH)/8.0f+.5f);
    if(!width)width=1;
    unsigned axis=fabsf(b.pos[0]-a.pos[0])>=fabsf(b.pos[1]-a.pos[1])?1:0;
    for(unsigned i=0;i<width;i++) {
        Nv2aVshOutput aa=a,bb=b;float offset=(float)i-((float)width-1)*.5f;
        aa.pos[axis]+=offset;bb.pos[axis]+=offset;
        raster_xf_triangle_mode(&aa,&bb,&bb,polygon?5:4,1,polygon_slope);
    }
}

static void raster_program_line(uint32_t a,uint32_t b,uint32_t p)
{
#if defined(_WIN32)
    if(s_hw_program_active) { hw_program_line(a,b,p);return; }
#endif
    raster_vertex_line(&s_xf[a],&s_xf[b],&s_xf[p],0,0);
}
static void raster_indexed_line(uint32_t i0,uint32_t i1,uint32_t p)
{
    Nv2aVshOutput a={0},b={0},provoking={0};
    if(!fetch_screen_vertex(i0,&a) || !fetch_screen_vertex(i1,&b))return;
    nv2a_rc_unpack(vertex_color(p),provoking.d0);fetch_attr(&s_gpu.attr[4],p,provoking.d1);
    raster_vertex_line(&a,&b,&provoking,0,0);
}

static void raster_batch_vertices(int fixed)
{
    uint32_t i, n = s_gpu.idx_count;
    /* Point batches do not select a provoking vertex. Other primitives use
     * this same device for the whole batch; the flag is still read per call. */
    struct PGRAPHState *pg=s_gpu.prim==NV_PRIM_POINTS?NULL:&nv2a_get_state()->pgraph;
#if defined(_WIN32)
    uint64_t transform_start = gpu_timing_clock();
    unsigned attributes[16],attribute_count=0;
    float (*packed)[4]=s_hw_program_upload.vertex;
#endif

    /* Attributes and the program are fixed during this synchronous batch.
     * Context writes make repeated indices stateful, so those execute every
     * vertex. No outputs are retained across command batches. */
    int reuse = fixed || !nv2a_vsh_cxt_write();
    uint32_t inputs = 0x1FFFu;
#if defined(_WIN32)
    if(hw_enabled()) {
        hw_initialize();
        if(!fixed) {
            s_hw_program=nv2a_vsh_program(&s_hw_program_length,&inputs);
            if(!s_hw_program) hw_check(E_NOTIMPL,"vertex program context writes or missing final");
        }
        s_hw_program_active=1;s_hw_output_active=fixed==2;s_hw_fixed_active=fixed==1;s_hw_program_vertex_count=0;
        if(s_hw_output_active) inputs=255;
        s_hw_program_inputs=inputs;
        /* The native input layout uses this same decoded input mask, in
         * ascending attribute order. Write its packed rows directly. */
        uint32_t remaining=inputs&0xFFFFu;
        while(remaining) {
            attributes[attribute_count++]=(unsigned)ctz32(remaining);
            remaining&=remaining-1;
        }
        s_hw_topology=s_gpu.prim==NV_PRIM_POINTS?D3D11_PRIMITIVE_TOPOLOGY_POINTLIST:s_gpu.prim<=NV_PRIM_LINE_STRIP?D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ:D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ;
        s_hw_bounds=(RECT){(LONG)s_gpu.clip_x,(LONG)s_gpu.clip_y,(LONG)(s_gpu.clip_x+s_gpu.clip_w),(LONG)(s_gpu.clip_y+s_gpu.clip_h)};
    } else
#endif
    if(!fixed) inputs=nv2a_vsh_inputs_used();
#if defined(_WIN32)
    int cache_hit=0,cache_locked=s_hw_program_active && !s_hw_output_active && !s_gpu.inline_active;
    HwVertexBatch *cache=NULL;
    if(cache_locked) {hw_enter();cache=hw_vertex_batch_begin(inputs,attribute_count,&cache_hit);}
    s_hw_vertex_batch=cache;
    if(!cache_hit) {
#endif
    /* Only slots read by this batch need resetting. */
    for (i = 0; i < n; i++)
        s_xf_positions[s_gpu.idx[i] % NV_MAX_INDICES] = 0;
    for (i = 0; i < n; i++) {
        uint32_t slot = s_gpu.idx[i] % NV_MAX_INDICES;
        uint32_t previous = s_xf_positions[slot];
        if (reuse && previous && s_gpu.idx[previous - 1] == s_gpu.idx[i]) {
#if defined(_WIN32)
            if(s_hw_program_active) s_hw_program_vertex_rows[i]=s_hw_program_vertex_rows[previous-1];
            else
#endif
            s_xf[i] = s_xf[previous - 1];
            ++s_gpu.vertices_reused;
        } else {
#if defined(_WIN32)
            if(s_hw_program_active) {
                uint32_t row=s_hw_program_vertex_count++;s_hw_program_vertex_rows[i]=row;
                if(s_hw_output_active) {
                    Nv2aVshOutput output={0};
                    if(!fetch_screen_vertex(s_gpu.idx[i],&output)) hw_check(E_FAIL,"screen vertex output");
                    hw_pack_output((HwVertexOutput *)packed,&output);
                    packed+=8;
                } else for(unsigned a=0;a<attribute_count;++a) {
                    unsigned attr=attributes[a];
                    fetch_attr(&s_gpu.attr[attr],s_gpu.idx[i],*packed++);
                }
            } else
#endif
            {
                if(fixed==2){memset(&s_xf[i],0,sizeof s_xf[i]);if(!fetch_screen_vertex(s_gpu.idx[i],&s_xf[i]))return;}
                else if (!transform_vertex(s_gpu.idx[i], inputs, &s_xf[i])) return;
                ++s_gpu.vertices_transformed;
            }
            s_xf_positions[slot] = i + 1;
        }
    }
#if defined(_WIN32)
    }
    if(cache_locked) {if(!cache_hit)hw_vertex_batch_end(cache,attribute_count);hw_leave();}
    gpu_timing_finish(transform_start, &s_gpu.vertex_ticks);
#endif
    if(!fixed) { s_gpu.batches_program++;s_gpu.verts_program += n; }
    if (s_gpu.blend_enable) {
        uint32_t pair = s_gpu.blend_sfactor << 16 | (s_gpu.blend_dfactor & 0xFFFF);
        int j;
        for (j = 0; j < s_gpu.blend_npairs && s_gpu.blend_pairs[j] != pair; j++)
            ;
        if (j == s_gpu.blend_npairs && j < 16)
            s_gpu.blend_pairs[s_gpu.blend_npairs++] = pair;
    }
    {
        /* RECOMP_VSH_TRACE=<n>: print n program batches -- attribute setup,
         * raw inputs, key constants, and what came out. */
        static int left = -1;
        if (left < 0) {
            const char *t = getenv("RECOMP_VSH_TRACE");
            left = t ? atoi(t) : 0;
        }
        /* Spread out: one every 20000 program batches, so a run that spends
         * its first half in menus still traces the 3D scene. */
        if (left > 0 && n >= 3 && s_gpu.batches_program % 20000 == 0
#if defined(_WIN32)
            && !s_hw_program_active
#endif
        ) {
            float in[4];
            left--;
            fprintf(stderr, "[VTRACE] prim %u n %u idx %u %u %u\n", s_gpu.prim,
                    n, s_gpu.idx[0], s_gpu.idx[1], s_gpu.idx[2]);
            for (i = 0; i < NV_VERTEX_ATTRS; i++) {
                const VertexAttr *at = &s_gpu.attr[i];
                if (!at->size)
                    continue;
                fetch_attr(at, s_gpu.idx[0], in);
                fprintf(stderr, "[VTRACE]   v%u off %08X type %u size %u"
                        " stride %u = %g %g %g %g\n", i, at->offset, at->type,
                        at->size, at->stride, in[0], in[1], in[2], in[3]);
            }
            {
                static const uint32_t cs[] = {58, 59, 96, 97, 112, 113, 114, 115};
                for (i = 0; i < 8; i++) {
                    const float *c = nv2a_vsh_constant(cs[i]);
                    fprintf(stderr, "[VTRACE]   c[%u] %g %g %g %g%c", cs[i],
                            c[0], c[1], c[2], c[3], 10);
                }
            }
            for (i = 0; i < 3; i++)
                fprintf(stderr, "[VTRACE]   out%u pos %g %g %g %g\n", i,
                        s_xf[i].pos[0], s_xf[i].pos[1], s_xf[i].pos[2],
                        s_xf[i].pos[3]);
        }
    }
    for (i = 0; i < NV2A_VSH_INPUTS; i++) {
        const VertexAttr *at = &s_gpu.attr[i];
        uint32_t f = at->type | (at->size << 4) | (i << 8), j;
        if (!at->size || !at->stride)
            continue;
        for (j = 0; j < (uint32_t)s_gpu.xf_nfmt && s_gpu.xf_fmt[j] != f; j++)
            ;
        if (j == (uint32_t)s_gpu.xf_nfmt && s_gpu.xf_nfmt < 16)
            s_gpu.xf_fmt[s_gpu.xf_nfmt++] = f;
    }
    if (s_gpu.texs[0].valid)
        note_texture_use();

    switch (s_gpu.prim) {
    case NV_PRIM_POINTS:
#if defined(_WIN32)
        if(s_hw_program_active) { for(i=0;i<n;i++) hw_program_point(i);break; }
#endif
        for(i=0;i<n;i++)raster_vertex_point(&s_xf[i],3,0);
        break;
    case NV_PRIM_LINES:
        for(i=0;i+1<n;i+=2) raster_program_line(i,i+1,provoking_slot(pg,i));
        break;
    case NV_PRIM_LINE_STRIP:
        for(i=0;i+1<n;i++) raster_program_line(i,i+1,provoking_slot(pg,i));
        break;
    case NV_PRIM_LINE_LOOP:
        for(i=0;i<n;i++) raster_program_line(i,(i+1)%n,provoking_slot(pg,i));
        break;
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < n; i += 3)
            raster_program_triangle(i,i+1,i+2,provoking_slot(pg,i));
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        for (i = 0; i + 2 < n; i++)
            raster_program_triangle(i+(i&1),i+1-(i&1),i+2,provoking_slot(pg,i));
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:
        for (i = 1; i + 1 < n; i++)
            raster_program_triangle(0,i,i+1,provoking_slot(pg,i));
        break;
    case NV_PRIM_QUADS:
        for (i = 0; i + 3 < n; i += 4) {
#if defined(_WIN32)
            if(s_hw_program_active && hw_polygon_modes_active()) {hw_program_quad(i,i+1,i+2,i+3,provoking_slot(pg,i));continue;}
#endif
            if(pg->regs[NV_PGRAPH_SETUPRASTER]&(NV_PGRAPH_SETUPRASTER_FRONTFACEMODE|NV_PGRAPH_SETUPRASTER_BACKFACEMODE)){raster_software_polygon(i,i+1,i+2,i+3,4,provoking_slot(pg,i));continue;}
            raster_program_triangle(i,i+1,i+2,provoking_slot(pg,i));
            raster_program_triangle(i,i+2,i+3,provoking_slot(pg,i));
        }
        break;
    case NV_PRIM_QUAD_STRIP:
        for (i = 0; i + 3 < n; i += 2) {
#if defined(_WIN32)
            if(s_hw_program_active && hw_polygon_modes_active()) {hw_program_quad(i,i+1,i+3,i+2,provoking_slot(pg,i));continue;}
#endif
            if(pg->regs[NV_PGRAPH_SETUPRASTER]&(NV_PGRAPH_SETUPRASTER_FRONTFACEMODE|NV_PGRAPH_SETUPRASTER_BACKFACEMODE)){raster_software_polygon(i,i+1,i+3,i+2,4,provoking_slot(pg,i));continue;}
            raster_program_triangle(i,i+1,i+3,provoking_slot(pg,i));
            raster_program_triangle(i,i+3,i+2,provoking_slot(pg,i));
        }
        break;
    default:
        break;
    }
}

static unsigned primitive_min_vertices(void)
{
    return s_gpu.prim==NV_PRIM_POINTS ? 1u : s_gpu.prim<=NV_PRIM_LINE_STRIP ? 2u : 3u;
}

static void raster_batch(void)
{
    uint32_t i;
    uint32_t before = s_gpu.tris_drawn;

    if (s_gpu.idx_count < primitive_min_vertices())
        return;
    if(s_gpu.format&0xF000u)note_unhandled(NV097_SET_SURFACE_FORMAT,s_gpu.format);
    struct PGRAPHState *smooth_pg=&nv2a_get_state()->pgraph;
    if(!(smooth_pg->regs[NV_PGRAPH_ANTIALIASING]&NV_PGRAPH_ANTIALIASING_ENABLE)) {
        uint32_t raster=smooth_pg->regs[NV_PGRAPH_SETUPRASTER];
        int lines=s_gpu.prim>=NV_PRIM_LINES && s_gpu.prim<=NV_PRIM_LINE_STRIP,fill=0;
        if(s_gpu.prim>=NV_PRIM_TRIANGLES) {
            unsigned front=GET_MASK(raster,NV_PGRAPH_SETUPRASTER_FRONTFACEMODE),back=GET_MASK(raster,NV_PGRAPH_SETUPRASTER_BACKFACEMODE);
            lines=front==2 || back==2;fill=front==0 || back==0;
        }
        if(lines && (raster&NV_PGRAPH_SETUPRASTER_LINESMOOTHENABLE))note_unhandled(NV097_SET_LINE_SMOOTH_ENABLE,1);
        if(fill && (raster&NV_PGRAPH_SETUPRASTER_POLYSMOOTHENABLE))note_unhandled(NV097_SET_POLY_SMOOTH_ENABLE,1);
    }
#if defined(_WIN32)
    if(hw_enabled()) s_hw_topology=s_gpu.prim==NV_PRIM_POINTS?D3D11_PRIMITIVE_TOPOLOGY_POINTLIST:s_gpu.prim<=NV_PRIM_LINE_STRIP?D3D11_PRIMITIVE_TOPOLOGY_LINELIST:D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
#endif
    static int no_vsh = -1;
    if (no_vsh < 0)
        no_vsh = getenv("RECOMP_NO_VSH") != NULL;
    if ((s_gpu.xf_mode & 3) == 2 && !no_vsh) {
        raster_batch_vertices(0);
        return;
    }
#if defined(_WIN32)
    if(!hw_enabled())
#endif
    if((s_gpu.xf_mode&3)==0) {
        struct PGRAPHState *fixed=&nv2a_get_state()->pgraph;
        if((fixed->regs[NV_PGRAPH_CSV0_C]&(NV_PGRAPH_CSV0_C_LIGHTING|NV_PGRAPH_CSV0_C_LIGHT_TWO_SIDE_ENABLE)) ||
           GET_MASK(fixed->regs[NV_PGRAPH_CSV0_D],NV_PGRAPH_CSV0_D_SKIN) ||
           ((fixed->regs[NV_PGRAPH_CSV0_D]&NV_PGRAPH_CSV0_D_POINTPARAMSENABLE) &&
            (GET_MASK(fixed->regs[NV_PGRAPH_SETUPRASTER],NV_PGRAPH_SETUPRASTER_FRONTFACEMODE)==1 || GET_MASK(fixed->regs[NV_PGRAPH_SETUPRASTER],NV_PGRAPH_SETUPRASTER_BACKFACEMODE)==1)) || s_gpu.prim==NV_PRIM_POINTS || s_gpu.attr[0].size<4 || !batch_is_screen_space()) {
            raster_batch_vertices(1);return;
        }
    }
    /* CSV1 texgen and texture matrices belong to the original fixed vertex
     * path. Program output coordinates must not be generated a second time. */
    struct PGRAPHState *pg=&nv2a_get_state()->pgraph;
    int fixed_texture=0;
    for(unsigned stage=0;stage<4;stage++) {
        for(unsigned component=0;component<4;component++) {
            uint32_t mask=NV_PGRAPH_CSV1_A_T0_S<<(component*3+(stage%2)*16);
            uint32_t mode=GET_MASK(pg->regs[stage<2?NV_PGRAPH_CSV1_A:NV_PGRAPH_CSV1_B],mask);
            if(mode) {
                if(mode<=NV_PGRAPH_CSV1_A_T0_S_OBJECT_LINEAR || ((mode==4 || mode==5) && component<3) || (mode==3 && component<2)) {fixed_texture=1;continue;}
                fprintf(stderr,"[STOP] fixed vertex texgen requires consumer: stage=%u component=%u mode=%u\n",stage,component,mode);
                exit(5);
            }
        }
        if(pg->texture_matrix_enable[stage]) {
            fixed_texture=1;
        }
    }
    if((s_gpu.xf_mode&3)==0 && fixed_texture
#if defined(_WIN32)
       && !hw_enabled()
#endif
    ) {raster_batch_vertices(1);return;}
#if defined(_WIN32)
    uint32_t raster=pg->regs[NV_PGRAPH_SETUPRASTER];
    int polygon_point_params=(pg->regs[NV_PGRAPH_CSV0_D]&NV_PGRAPH_CSV0_D_POINTPARAMSENABLE) &&
        (GET_MASK(raster,NV_PGRAPH_SETUPRASTER_FRONTFACEMODE)==1 || GET_MASK(raster,NV_PGRAPH_SETUPRASTER_BACKFACEMODE)==1);
    if(hw_enabled() && (polygon_point_params || fixed_texture || (pg->regs[NV_PGRAPH_CSV0_C]&NV_PGRAPH_CSV0_C_LIGHT_TWO_SIDE_ENABLE) || GET_MASK(pg->regs[NV_PGRAPH_CSV0_D],NV_PGRAPH_CSV0_D_SKIN)!=0 || s_gpu.attr[0].size<4 || (pg->regs[NV_PGRAPH_CSV0_C]&NV_PGRAPH_CSV0_C_LIGHTING) || s_gpu.prim==NV_PRIM_POINTS || !batch_is_screen_space())) {
        if((s_gpu.xf_mode&3)!=0)
            hw_check(E_NOTIMPL,"fixed vertex mode");
        if(GET_MASK(pg->regs[NV_PGRAPH_CSV0_D],NV_PGRAPH_CSV0_D_FOGGENMODE)>NV_PGRAPH_CSV0_D_FOGGENMODE_FOG_X)
            hw_check(E_NOTIMPL,"fixed vertex fog generation");
        raster_batch_vertices(1);return;
    }
#endif
#if defined(_WIN32)
    if(hw_enabled() && (hw_polygon_modes_active() || (s_gpu.prim>=NV_PRIM_LINES && s_gpu.prim<=NV_PRIM_LINE_STRIP))) {
        raster_batch_vertices(2);return;
    }
#endif
    if(s_gpu.prim>NV_PRIM_LINE_STRIP && (pg->regs[NV_PGRAPH_SETUPRASTER]&(NV_PGRAPH_SETUPRASTER_FRONTFACEMODE|NV_PGRAPH_SETUPRASTER_BACKFACEMODE))) {raster_batch_vertices(2);return;}
    if(GET_MASK(pg->regs[NV_PGRAPH_CSV0_D],NV_PGRAPH_CSV0_D_SKIN)!=0)
        note_unhandled(NV097_SET_SKIN_MODE,GET_MASK(pg->regs[NV_PGRAPH_CSV0_D],NV_PGRAPH_CSV0_D_SKIN));
    if (pg->regs[NV_PGRAPH_CSV0_C] & NV_PGRAPH_CSV0_C_LIGHTING) {
        fprintf(stderr,"[STOP] fixed vertex lighting requires consumer: mode=%u primitive=%u\n",
                s_gpu.xf_mode & 3, s_gpu.prim);
        exit(5);
    }
    if(s_gpu.prim==NV_PRIM_POINTS) note_unhandled(NV097_SET_BEGIN_END,s_gpu.prim);
    if (!batch_is_screen_space()) {
        s_gpu.batches_untransformed++;
        return;
    }

    /* Count why, once per batch: the texture stage cannot change inside one. */
    {
        const VertexAttr *tc = texcoord_attr();

        if (!(tc->offset && tc->stride))
            s_gpu.batches_no_uv++;
        else if (!s_gpu.texs[0].valid)
            s_gpu.batches_no_tex++;
        else {
            s_gpu.batches_textured++;
            note_texture_use();
        }
    }

    switch (s_gpu.prim) {
    case NV_PRIM_LINES:
        for(i=0;i+1<s_gpu.idx_count;i+=2) raster_indexed_line(s_gpu.idx[i],s_gpu.idx[i+1],s_gpu.idx[provoking_slot(pg,i)]);
        break;
    case NV_PRIM_LINE_STRIP:
        for(i=0;i+1<s_gpu.idx_count;i++) raster_indexed_line(s_gpu.idx[i],s_gpu.idx[i+1],s_gpu.idx[provoking_slot(pg,i)]);
        break;
    case NV_PRIM_LINE_LOOP:
        for(i=0;i<s_gpu.idx_count;i++) raster_indexed_line(s_gpu.idx[i],s_gpu.idx[(i+1)%s_gpu.idx_count],s_gpu.idx[provoking_slot(pg,i)]);
        break;
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < s_gpu.idx_count; i += 3)
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+2],
                           s_gpu.idx[provoking_slot(pg,i)]);
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        for (i = 0; i + 2 < s_gpu.idx_count; i++)
            raster_indexed(s_gpu.idx[i + (i & 1)], s_gpu.idx[i + 1 - (i & 1)], s_gpu.idx[i+2],
                           s_gpu.idx[provoking_slot(pg,i)]);
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:
        for (i = 1; i + 1 < s_gpu.idx_count; i++)
            raster_indexed(s_gpu.idx[0], s_gpu.idx[i], s_gpu.idx[i+1],
                           s_gpu.idx[provoking_slot(pg,i)]);
        break;
    case NV_PRIM_QUADS:
        /* Independent quads, four vertices each. A batch of eight is two
         * quads, not one six-triangle fan around the first vertex; with
         * exactly four the two agreed, which is why sharing the fan arm
         * looked right. */
        for (i = 0; i + 3 < s_gpu.idx_count; i += 4) {
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+2],
                           s_gpu.idx[provoking_slot(pg,i)]);
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+2], s_gpu.idx[i+3],
                           s_gpu.idx[provoking_slot(pg,i)]);
        }
        break;
    case NV_PRIM_QUAD_STRIP:
        /* Each vertex pair past the first closes another quad against the
         * pair before it. */
        for (i = 0; i + 3 < s_gpu.idx_count; i += 2) {
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+3],
                           s_gpu.idx[provoking_slot(pg,i)]);
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+3], s_gpu.idx[i+2],
                           s_gpu.idx[provoking_slot(pg,i)]);
        }
        break;
    default:
        break;
    }

    if (s_gpu.tris_drawn && (s_gpu.tris_drawn % 500) == 0)
        fprintf(stderr, "  [GPU] %u triangles rasterised\n", s_gpu.tris_drawn);

    /* Capture the surface while the geometry is still on it.
     *
     * The periodic report dumps too, but a title clears every frame and draws
     * in only some of them, so a report almost always lands on a surface that
     * was wiped a moment ago -- which reads as "nothing was drawn" when the
     * triangles went down correctly just before it. A few frames that actually
     * contain geometry are worth more than any number of clears. */
    if (s_gpu.tris_drawn != before && s_drawn_dumps < FB_DUMP_AFTER_DRAW
        && !s_flip_dumping) {
        s_drawn_dumps++;
        dump_surface_bmp();
    }
}

/* What a batch actually contains. Before anything can be rasterised, the
 * question is what space attribute 0 arrives in: a title running a vertex
 * program hands over object-space positions that mean nothing without running
 * it, while pre-transformed screen-space coordinates can be drawn directly. */
/* RECOMP_FRAME_TRACE=<flag file>: once the file exists, log every batch of
 * the next whole frame (flip to flip) -- where it drew, with what texture,
 * blend and depth state, and how many pixels it actually wrote. "The frame
 * is black" has many causes and this is what tells them apart. */

static void frame_trace_flip(void)
{
    static const char *flag = (const char *)-1;
    if (flag == (const char *)-1)
        flag = getenv("RECOMP_FRAME_TRACE");
    if (!flag)
        return;
    if (s_ftrace == 3) {
        /* Deleting the flag file re-arms the trace for the next time it
         * appears, so one run can trace several moments. */
        FILE *f = fopen(flag, "rb");
        if (f)
            fclose(f);
        else
            s_ftrace = 0;
        return;
    }
    if (s_ftrace == 2) {
        s_ftrace = 3;
        fprintf(stderr, "[FTRACE] end of frame\n");
        return;
    }
    if (s_ftrace == 0) {
        FILE *f = fopen(flag, "rb");
        if (!f)
            return;
        fclose(f);
        s_ftrace = 2;
        fprintf(stderr, "[FTRACE] frame begins (flip %u)\n", s_gpu.flips);
    }
}

static void draw_primitive(void)
{
    float v[4];
    uint32_t i;

    if (!s_gpu.prim || !s_gpu.idx_count)
        return;
    for(unsigned stage=0;stage<4;stage++)if(texture_volume(&s_gpu.texs[stage]) && texture_stage_samples((s_gpu.rc.stage_program>>(stage*5))&31)) {
        Texture *t=&s_gpu.texs[stage];unsigned mode=(s_gpu.rc.stage_program>>(stage*5))&31,w=texture_address_w(stage);
        if((mode!=13 && mode!=2) || w<1 || w>5){fprintf(stderr,"[STOP] volume requires DOT_STR/PROJECT3D and established W addressing\n");exit(5);}
        if(mode==13)texture_dot_source(stage,mode);
        uint32_t bytes=texture_volume_bytes(t);struct PGRAPHState *pg=&nv2a_get_state()->pgraph;
        t->offset=dma_resolve((t->dma_format&3)==2?pg->dma_b:pg->dma_a,t->dma_offset,bytes);
    }
    s_gpu.draws++;
    if (s_ftrace == 2 && (s_gpu.draws % 200) == 0)
        fprintf(stderr, "  [GPU] draw #%u\n", s_gpu.draws);
    s_gpu.verts += s_gpu.idx_count;

    /* How many batches carry coordinates at all, and what range they span.
     * A pipeline that decodes perfectly and draws nothing is indistinguishable
     * from one that never ran, unless the vertices themselves are measured. */
    {
        float p[4];
        if (fetch_attr(&s_gpu.attr[0], s_gpu.idx[0], p)) {
            if (p[0] != 0.0f || p[1] != 0.0f || p[2] != 0.0f) {
                s_gpu.nonzero_draws++;
                if (p[0] < s_gpu.min_x) s_gpu.min_x = p[0];
                if (p[0] > s_gpu.max_x) s_gpu.max_x = p[0];
                if (p[1] < s_gpu.min_y) s_gpu.min_y = p[1];
                if (p[1] > s_gpu.max_y) s_gpu.max_y = p[1];
            }
        }
    }

    {
        uint32_t t0 = s_gpu.tris_drawn;
        uint64_t p0 = s_gpu.pixels;
#if defined(_WIN32)
        uint64_t timing_start = gpu_timing_clock();
#endif
        raster_batch();
#if defined(_WIN32)
        if (hw_enabled()) hw_flush_batch();
#endif
#if defined(_WIN32)
        gpu_timing_finish(timing_start, &s_gpu.draw_ticks);
#endif
        if (s_ftrace == 2) {
            if(getenv("RECOMP_FRAME_DRAWS_DUMP")){fprintf(stderr,"[FTRACE] draw surface dump draw=%u\n",s_gpu.draws);dump_surface_bmp();}
            float input_position[4];fetch_attr(&s_gpu.attr[0],s_gpu.idx[0],input_position);
            if(s_gpu.prim==NV_PRIM_QUADS){float uv[4];fetch_attr(&s_gpu.attr[9],s_gpu.idx[0],uv);fprintf(stderr,"[FTRACE] original quad uv=%g,%g,%g,%g\n",uv[0],uv[1],uv[2],uv[3]);dump_texture_bmp(s_gpu.draws);}
            fprintf(stderr,"[FTRACE] original input type=%u size=%u stride=%u offset=%08X screen=%d xyzw=%g,%g,%g,%g\n",s_gpu.attr[0].type,s_gpu.attr[0].size,s_gpu.attr[0].stride,s_gpu.attr[0].offset,batch_is_screen_space(),input_position[0],input_position[1],input_position[2],input_position[3]);
            if((s_gpu.xf_mode&3)==0)for(unsigned row=0;row<4;row++){const float *cv=nv2a_vsh_constant(row);fprintf(stderr,"[FTRACE] composite row=%u %g,%g,%g,%g\n",row,cv[0],cv[1],cv[2],cv[3]);}
            fprintf(stderr, "[FTRACE] %s prim %u n %u surf %08X %ux%u+%u+%u"
                    " pitch %u | tex %s %08X %ux%u fmt %02X | blend %u %X/%X"
                    " eq %X | z test %u func %X mask %u | tris %u px %llu\n",
                    (s_gpu.xf_mode & 3) == 2 ? "PRG" : "FIX", s_gpu.prim,
                    s_gpu.idx_count, s_gpu.color_offset, s_gpu.clip_w,
                    s_gpu.clip_h, s_gpu.clip_x, s_gpu.clip_y, s_gpu.pitch,
                    s_gpu.texs[0].valid ? "on" : "off", s_gpu.texs[0].offset,
                    s_gpu.texs[0].width, s_gpu.texs[0].height, s_gpu.texs[0].color,
                    s_gpu.blend_enable, s_gpu.blend_sfactor,
                    s_gpu.blend_dfactor, s_gpu.blend_equation,
                    s_gpu.depth_test, s_gpu.depth_func, s_gpu.depth_mask,
                    s_gpu.tris_drawn - t0,
                    (unsigned long long)(s_gpu.pixels - p0));
            if ((s_gpu.xf_mode & 3) == 2)
                fprintf(stderr, "[FTRACE]     v0 %g %g %g %g  v1 %g %g  v2 %g %g%c",
                        s_xf[0].pos[0], s_xf[0].pos[1], s_xf[0].pos[2],
                        s_xf[0].pos[3], s_xf[1].pos[0], s_xf[1].pos[1],
                        s_xf[2].pos[0], s_xf[2].pos[1], 10);
            if (s_gpu.rc_seen) {
                int st;
                fprintf(stderr, "[FTRACE]     rc stages %u ctl %08X prog %05X fin %08X %08X",
                        s_gpu.rc.control & 0xFF, s_gpu.rc.control,
                        s_gpu.rc.stage_program, s_gpu.rc.final0, s_gpu.rc.final1);
                for (st = 0; st < 4; st++)
                    if ((s_gpu.rc.stage_program >> (st * 5)) & 0x1F)
                        fprintf(stderr, " | t%d %08X %ux%u fmt %02X%s", st,
                                s_gpu.texs[st].offset, s_gpu.texs[st].width,
                                s_gpu.texs[st].height, s_gpu.texs[st].color,
                                s_gpu.texs[st].valid ? "" : " (invalid)");
                fputc(10, stderr);
                for (st = 0; st < (int)(s_gpu.rc.control & 0xFF) && st < 8; st++)
                    fprintf(stderr, "[FTRACE]       s%d icw %08X %08X ocw %08X %08X c %08X %08X%c",
                            st, s_gpu.rc.color_icw[st], s_gpu.rc.alpha_icw[st],
                            s_gpu.rc.color_ocw[st], s_gpu.rc.alpha_ocw[st],
                            s_gpu.rc.factor0[st], s_gpu.rc.factor1[st], 10);
            }
            if ((s_gpu.xf_mode & 3) == 2) {
                uint32_t r;
                for (r = 112; r < 116; r++) {
                    const float *c = nv2a_vsh_constant(r);
                    fprintf(stderr, "[FTRACE]     c[%u] %g %g %g %g%c", r,
                            c[0], c[1], c[2], c[3], 10);
                }
            }
        }
    }

    if (s_pb_exec_verbose > 0) {
        static int shown;
        if (shown++ < 6) {
            fprintf(stderr, "  [GPU] prim %u, %u indices, pos attr:"
                            " off 0x%08X type %u size %u stride %u\n",
                    s_gpu.prim, s_gpu.idx_count, s_gpu.attr[0].offset,
                    s_gpu.attr[0].type, s_gpu.attr[0].size, s_gpu.attr[0].stride);
            /* The texture stage, for either kind of batch. This used to print
             * only for inline batches, which meant a title drawing through
             * vertex arrays -- Half-Life 2's menu, for one -- showed no
             * texture state at all, and the reason a quad sampled flat was
             * invisible. */
            fprintf(stderr, "  [GPU]   tex: off 0x%08X %ux%u pitch %u"
                            " colour 0x%02X swizzled %d valid %d\n",
                    s_gpu.texs[0].offset, s_gpu.texs[0].width, s_gpu.texs[0].height,
                    s_gpu.texs[0].pitch, s_gpu.texs[0].color,
                    d3d8_format_is_swizzled(s_gpu.texs[0].color), s_gpu.texs[0].valid);
            {
                uint32_t k;
                for (k = 0; k < s_gpu.idx_count && k < 3; k++) {
                    float t[2];
                    if (fetch_texcoord(s_gpu.idx[k], t))
                        fprintf(stderr, "  [GPU]   uv[%u] = %.3f %.3f\n",
                                k, t[0], t[1]);
                }
            }
            /* An inline batch has no guest buffer to go and look at -- the
             * vertices are the payload -- so print the payload too. */
            if (s_gpu.inline_active) {
                uint32_t k;
                fprintf(stderr, "  [GPU]   inline %u dwords:", s_gpu.inline_count);
                for (k = 0; k < s_gpu.inline_count && k < 16; k++)
                    fprintf(stderr, " %08X", s_gpu.inline_buf[k]);
                fprintf(stderr, "\n");
                for (k = 0; k < s_gpu.idx_count && k < 4; k++) {
                    float t[2];
                    if (fetch_texcoord(s_gpu.idx[k], t))
                        fprintf(stderr, "  [GPU]   uv[%u] = %.3f %.3f\n",
                                k, t[0], t[1]);
                }
                fprintf(stderr, "  [GPU]   tex: off 0x%08X %ux%u pitch %u"
                                " colour 0x%02X valid %d\n",
                        s_gpu.texs[0].offset, s_gpu.texs[0].width, s_gpu.texs[0].height,
                        s_gpu.texs[0].pitch, s_gpu.texs[0].color, s_gpu.texs[0].valid);
            }
            {
                /* Every attribute the batch has, not just position. If the
                 * other streams carry data and position does not, the problem
                 * is one buffer rather than the whole vertex path. */
                const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
                uint32_t a, k;
                for (a = 0; a < NV_VERTEX_ATTRS; a++) {
                    const VertexAttr *at = &s_gpu.attr[a];
                    uint32_t nz = 0;
                    if (!at->offset || !at->size)
                        continue;
                    for (k = 0; k < 64; k++)
                        if (mem[at->offset + k]) nz++;
                    fprintf(stderr, "  [GPU]   attr%-2u off 0x%08X type %u"
                                    " size %u stride %-3u  %u/64 bytes set\n",
                            a, at->offset, at->type, at->size, at->stride, nz);
                }
            }
            {
                /* Raw bytes at the array, in case the values read as zero:
                 * that looks the same whether the offset is wrong or the
                 * buffer genuinely has not been filled yet. */
                const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
                uint32_t k;
                fprintf(stderr, "  [GPU]   bytes @0x%08X:", s_gpu.attr[0].offset);
                for (k = 0; k < 32; k++)
                    fprintf(stderr, " %02X", mem[s_gpu.attr[0].offset + k]);
                fprintf(stderr, "\n");
                fprintf(stderr, "  [GPU]   indices:");
                for (k = 0; k < s_gpu.idx_count && k < 8; k++)
                    fprintf(stderr, " %u", s_gpu.idx[k]);
                fprintf(stderr, "\n");
            }
            for (i = 0; i < s_gpu.idx_count && i < 3; i++) {
                if (fetch_attr(&s_gpu.attr[0], s_gpu.idx[i], v))
                    fprintf(stderr, "  [GPU]   v[%u] = %.3f %.3f %.3f %.3f\n",
                            s_gpu.idx[i], v[0], v[1], v[2], v[3]);
            }
        }
    }
}

/* Draw the vertices the title wrote straight into the pushbuffer.
 *
 * INLINE_ARRAY carries no offsets and no indices: the dwords between BEGIN and
 * END *are* the vertex buffer, packed in attribute order using the same
 * SET_VERTEX_DATA_ARRAY_FORMAT registers an ordinary array would use. So the
 * whole batch is describable as a vertex array whose base happens to be that
 * payload, which means synthesising the layout and handing it to the existing
 * path -- rather than a second copy of the topology and rasterisation code.
 *
 * The title's own attribute table is saved and put back: these offsets and
 * strides are ours, and it has not stopped using its.
 *
 * ponytail: each attribute is padded to a whole dword. That is exact for the
 * float and D3DCOLOR formats every inline batch actually uses; a packed
 * sub-dword attribute would need the unpadded layout.
 */
static void draw_inline_array(void)
{
    VertexAttr saved[NV_VERTEX_ATTRS];
    uint32_t off = 0, a, i, vsize, count;

    memcpy(saved, s_gpu.attr, sizeof saved);

    for (a = 0; a < NV_VERTEX_ATTRS; a++) {
        uint32_t bytes;
        if (!s_gpu.attr[a].size)
            continue;
        switch (s_gpu.attr[a].type) {
        case 0:  bytes = 4;                        break;  /* D3DCOLOR   */
        case 2:  bytes = 4 * s_gpu.attr[a].size;   break;  /* float      */
        case 4:  bytes = s_gpu.attr[a].size;       break;  /* ubyte norm */
        default: bytes = 4 * s_gpu.attr[a].size;   break;
        }
        s_gpu.attr[a].offset = off;
        off += (bytes + 3u) & ~3u;
    }
    vsize = off;
    if (!vsize)
        goto out;

    count = (s_gpu.inline_count * 4) / vsize;
    if (count < primitive_min_vertices() || count > NV_MAX_INDICES)
        goto out;
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (s_gpu.attr[a].size)
            s_gpu.attr[a].stride = vsize;

    for (i = 0; i < count; i++)
        { s_gpu.idx[i] = i; s_gpu.idx_edge[i]=s_gpu.inline_edge[(i+1)*vsize/4-1]; }
    s_gpu.idx_count = count;

    s_gpu.inline_active = 1;
    draw_primitive();
    s_gpu.inline_active = 0;

out:
    memcpy(s_gpu.attr, saved, sizeof saved);
    s_gpu.idx_count = 0;
}

/* Draw the vertices SET_VERTEX3F/4F completed.
 *
 * Same trick as draw_inline_array: rather than a second copy of the topology
 * and rasterisation code, describe what was accumulated as an ordinary vertex
 * array and hand it to the existing path. The layout is ours and fixed, so
 * the attribute table is written out here rather than derived from the
 * title's format registers.
 *
 * The title's own table is saved and put back -- it has not stopped using it.
 *
 * ponytail: position, diffuse and texcoord0 only. That is what a 2D quad
 * carries and what this rasteriser samples; a second texcoord set or a normal
 * would need the D3D11 translator, not more slots here.
 */
static void draw_immediate(void)
{
    VertexAttr saved[NV_VERTEX_ATTRS];
    uint32_t i;

    if (s_gpu.imm_count < primitive_min_vertices())
        return;

    memcpy(saved, s_gpu.attr, sizeof saved);
    memset(s_gpu.attr, 0, sizeof s_gpu.attr);
    /* Offsets are byte offsets into inline_buf here, not guest addresses --
     * fetch_attr reads them that way while inline_active is set, which is
     * also why 0 is a legal offset for position. Every attribute is a float4;
     * the ones the batch never set carry their standing values, as on the
     * GPU. */
    for (i = 0; i < NV_VERTEX_ATTRS; i++) {
        s_gpu.attr[i].type = 2;
        s_gpu.attr[i].size = 4;
        s_gpu.attr[i].offset = i * 16;
        s_gpu.attr[i].stride = IMM_VERTEX_DWORDS * 4;
    }

    for (i = 0; i < s_gpu.imm_count && i < NV_MAX_INDICES; i++)
        { s_gpu.idx[i] = i; s_gpu.idx_edge[i]=s_gpu.inline_edge[i*IMM_VERTEX_DWORDS]; }
    s_gpu.idx_count = i;

    /* fetch_attr bounds-checks against inline_count dwords. */
    s_gpu.inline_count = s_gpu.imm_count * IMM_VERTEX_DWORDS;
    s_gpu.inline_active = 1;
    draw_primitive();
    s_gpu.inline_active = 0;

    memcpy(s_gpu.attr, saved, sizeof saved);
    s_gpu.idx_count = 0;
    s_gpu.inline_count = 0;
}

/* A vertex is complete: append all 16 attributes in the layout
 * draw_immediate describes. */
static void imm_emit_vertex(void)
{
    uint32_t at = s_gpu.imm_count * IMM_VERTEX_DWORDS;

    if (!s_gpu.prim)
        return;
    require_batch_space("immediate", at, IMM_VERTEX_DWORDS, NV_MAX_INLINE);
    memcpy(&s_gpu.inline_buf[at], s_gpu.imm_attr, sizeof s_gpu.imm_attr);
    s_gpu.inline_edge[at]=(uint8_t)nv2a_get_state()->pgraph.edge_flag;
    s_gpu.imm_count++;
}

/* The immediate-mode writes. Returns 1 if `method` was one of them.
 *
 * Split out because it is a range test against five separate bases, and that
 * reads better than five more cases in an already long switch.
 */
/* The immediate-mode attribute methods (xemu pgraph.c SET_VERTEX_DATA*).
 * Each sets an attribute's current value; completing attribute 0 -- the
 * position -- emits a vertex carrying every attribute as it stands. This used
 * to keep position, diffuse and texcoord 0 only, which dropped any quad with
 * a second texture coordinate: Burnout 3 composites its whole 3D scene into
 * the frame with exactly such a quad, and the frame never got the scene. */
static int imm_vertex_method(uint32_t method, uint32_t param)
{
    union { uint32_t u; float f; } v;
    uint32_t attr, c;
    float *a;
    v.u = param;

    /* Legacy texture bases are explicit: stage 3 has a different 4F gap. */
    if (method >= NV097_SET_TEXCOORD0_2F && method < NV097_SET_TEXCOORD3_4S + 8) {
        static const uint32_t bases[4][4] = {
            {NV097_SET_TEXCOORD0_2F,NV097_SET_TEXCOORD0_4F,NV097_SET_TEXCOORD0_2S,NV097_SET_TEXCOORD0_4S},
            {NV097_SET_TEXCOORD1_2F,NV097_SET_TEXCOORD1_4F,NV097_SET_TEXCOORD1_2S,NV097_SET_TEXCOORD1_4S},
            {NV097_SET_TEXCOORD2_2F,NV097_SET_TEXCOORD2_4F,NV097_SET_TEXCOORD2_2S,NV097_SET_TEXCOORD2_4S},
            {NV097_SET_TEXCOORD3_2F,NV097_SET_TEXCOORD3_4F,NV097_SET_TEXCOORD3_2S,NV097_SET_TEXCOORD3_4S}
        };
        for (unsigned stage=0;stage<4;stage++) {
            uint32_t attribute=9+stage;
            if (method>=bases[stage][0] && method<bases[stage][0]+8) {
                s_gpu.imm_attr[attribute][2]=0;s_gpu.imm_attr[attribute][3]=1;
                method=NV097_SET_VERTEX_DATA2F_M+attribute*8+method-bases[stage][0];break;
            }
            if (method>=bases[stage][1] && method<bases[stage][1]+16) {
                method=NV097_SET_VERTEX_DATA4F_M+attribute*16+method-bases[stage][1];break;
            }
            if (method==bases[stage][2]) {
                method=NV097_SET_VERTEX_DATA2S+attribute*4;break;
            }
            if (method>=bases[stage][3] && method<bases[stage][3]+8) {
                method=NV097_SET_VERTEX_DATA4S_M+attribute*8+method-bases[stage][3];break;
            }
        }
    } else if (method == NV097_SET_FOG_COORD) {
        s_gpu.imm_attr[5][1]=s_gpu.imm_attr[5][2]=s_gpu.imm_attr[5][3]=v.f;
        method=NV097_SET_VERTEX_DATA4F_M+5*16;
    }

    /* Legacy setters share the existing generic standing-attribute consumer.
     * Three-component colours and weights supply the NV097 missing-component
     * defaults on each write, including partial writes before vertex emission. */
    int normalized_normal = 0;
    if (method >= NV097_SET_NORMAL3F && method < NV097_SET_WEIGHT4F + 16) {
        uint32_t first = 0, components = 0, attribute = 0;
        if (method >= NV097_SET_NORMAL3F && method < NV097_SET_NORMAL3F + 12) {
            first=NV097_SET_NORMAL3F;components=3;attribute=2;
        } else if (method >= NV097_SET_NORMAL3S && method < NV097_SET_NORMAL3S + 8) {
            normalized_normal=1;
            method=NV097_SET_VERTEX_DATA4S_M+2*8+method-NV097_SET_NORMAL3S;
        } else if (method >= NV097_SET_DIFFUSE_COLOR4F && method < NV097_SET_DIFFUSE_COLOR4F + 16) {
            first=NV097_SET_DIFFUSE_COLOR4F;components=4;attribute=3;
        } else if (method >= NV097_SET_DIFFUSE_COLOR3F && method < NV097_SET_DIFFUSE_COLOR3F + 12) {
            first=NV097_SET_DIFFUSE_COLOR3F;components=3;attribute=3;
            s_gpu.imm_attr[3][3]=1;
        } else if (method == NV097_SET_DIFFUSE_COLOR4UB) {
            method=NV097_SET_VERTEX_DATA4UB+3*4;
        } else if (method >= NV097_SET_SPECULAR_COLOR4F && method < NV097_SET_SPECULAR_COLOR4F + 16) {
            first=NV097_SET_SPECULAR_COLOR4F;components=4;attribute=4;
        } else if (method >= NV097_SET_SPECULAR_COLOR3F && method < NV097_SET_SPECULAR_COLOR3F + 12) {
            first=NV097_SET_SPECULAR_COLOR3F;components=3;attribute=4;
            s_gpu.imm_attr[4][3]=1;
        } else if (method == NV097_SET_SPECULAR_COLOR4UB) {
            method=NV097_SET_VERTEX_DATA4UB+4*4;
        } else if (method == NV097_SET_WEIGHT1F) {
            first=NV097_SET_WEIGHT1F;components=1;attribute=1;
            s_gpu.imm_attr[1][1]=s_gpu.imm_attr[1][2]=0;s_gpu.imm_attr[1][3]=1;
        } else if (method >= NV097_SET_WEIGHT2F && method < NV097_SET_WEIGHT2F + 8) {
            first=NV097_SET_WEIGHT2F;components=2;attribute=1;
            s_gpu.imm_attr[1][2]=0;s_gpu.imm_attr[1][3]=1;
        } else if (method >= NV097_SET_WEIGHT3F && method < NV097_SET_WEIGHT3F + 12) {
            first=NV097_SET_WEIGHT3F;components=3;attribute=1;
            s_gpu.imm_attr[1][3]=1;
        } else if (method >= NV097_SET_WEIGHT4F && method < NV097_SET_WEIGHT4F + 16) {
            first=NV097_SET_WEIGHT4F;components=4;attribute=1;
        }
        if (components)method=NV097_SET_VERTEX_DATA4F_M+attribute*16+method-first;
    }

    if (method >= NV097_SET_VERTEX4F && method < NV097_SET_VERTEX4F + 16) {
        c = (method - NV097_SET_VERTEX4F) / 4;
        s_gpu.imm_attr[0][c] = v.f;
        s_gpu.imm_used |= 1;
        if (c == 3)
            imm_emit_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX3F && method < NV097_SET_VERTEX3F + 12) {
        c = (method - NV097_SET_VERTEX3F) / 4;
        s_gpu.imm_attr[0][c] = v.f;
        s_gpu.imm_used |= 1;
        if (c == 2) {
            s_gpu.imm_attr[0][3] = 1.0f;
            imm_emit_vertex();
        }
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA2F_M
            && method < NV097_SET_VERTEX_DATA2F_M + NV_VERTEX_ATTRS * 8) {
        attr = (method - NV097_SET_VERTEX_DATA2F_M) / 8;
        c = ((method - NV097_SET_VERTEX_DATA2F_M) % 8) / 4;
        a = s_gpu.imm_attr[attr];
        a[c] = v.f;
        if (c == 1) {
            a[2] = 0.0f; a[3] = 1.0f;
        }
        s_gpu.imm_used |= (uint16_t)(1u << attr);
        if (attr == 0 && c == 1)
            imm_emit_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA4F_M
            && method < NV097_SET_VERTEX_DATA4F_M + NV_VERTEX_ATTRS * 16) {
        attr = (method - NV097_SET_VERTEX_DATA4F_M) / 16;
        c = ((method - NV097_SET_VERTEX_DATA4F_M) % 16) / 4;
        s_gpu.imm_attr[attr][c] = v.f;
        s_gpu.imm_used |= (uint16_t)(1u << attr);
        if (attr == 0 && c == 3)
            imm_emit_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA2S
            && method < NV097_SET_VERTEX_DATA2S + NV_VERTEX_ATTRS * 4) {
        attr = (method - NV097_SET_VERTEX_DATA2S) / 4;
        a = s_gpu.imm_attr[attr];
        a[0] = (float)(int16_t)(param & 0xFFFF);
        a[1] = (float)(int16_t)(param >> 16);
        a[2] = 0.0f; a[3] = 1.0f;
        s_gpu.imm_used |= (uint16_t)(1u << attr);
        if (attr == 0)
            imm_emit_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA4UB
            && method < NV097_SET_VERTEX_DATA4UB + NV_VERTEX_ATTRS * 4) {
        /* Bytes in register order, x from the low byte (xemu). */
        attr = (method - NV097_SET_VERTEX_DATA4UB) / 4;
        a = s_gpu.imm_attr[attr];
        a[0] = (float)( param        & 0xFF) / 255.0f;
        a[1] = (float)((param >>  8) & 0xFF) / 255.0f;
        a[2] = (float)((param >> 16) & 0xFF) / 255.0f;
        a[3] = (float)( param >> 24        ) / 255.0f;
        s_gpu.imm_used |= (uint16_t)(1u << attr);
        if (attr == 0)
            imm_emit_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA4S_M
            && method < NV097_SET_VERTEX_DATA4S_M + NV_VERTEX_ATTRS * 8) {
        attr = (method - NV097_SET_VERTEX_DATA4S_M) / 8;
        c = ((method - NV097_SET_VERTEX_DATA4S_M) % 8) / 4;
        a = s_gpu.imm_attr[attr];
        a[c * 2]     = (float)(int16_t)(param & 0xFFFF);
        a[c * 2 + 1] = (float)(int16_t)(param >> 16);
        if (normalized_normal) {
            a[c * 2] = fmaxf(-1.0f, normalized_vertex_short(a[c * 2]));
            a[c * 2 + 1] = fmaxf(-1.0f, normalized_vertex_short(a[c * 2 + 1]));
        }
        s_gpu.imm_used |= (uint16_t)(1u << attr);
        if (attr == 0 && c == 1)
            imm_emit_vertex();
        return 1;
    }
    return 0;
}
/* The texture stage registers, all four stages. Stage i's registers are
 * 0x1B00 + 0x40*i onwards; only stage 0 used to be decoded, which was
 * enough for menus drawn one texture at a time and not for a 3D frame that
 * composites its scene through a second stage. */
#if defined(_MSC_VER)
#define NV_NOINLINE __declspec(noinline)
#else
#define NV_NOINLINE __attribute__((noinline))
#endif
static NV_NOINLINE void tex_stage_method(uint32_t method, uint32_t param)
{
    uint32_t stage = (method - NV_TEX_FIRST) / 0x40;
    uint32_t reg = NV_TEX_FIRST + ((method - NV_TEX_FIRST) & 0x3F);
    Texture *t = &s_gpu.texs[stage];

    switch (reg) {
    case NV097_SET_TEXTURE_OFFSET:
        /* Keep the raw offset until FORMAT supplies its DMA A/B selection. */
        t->dma_offset = param;
        t->offset_set = 1;
        break;
    case NV097_SET_TEXTURE_FORMAT:
        t->dma_format = param;
        t->color = (param >> 8) & 0xFF;
        t->cube = (param >> 2) & 1;
        t->levels = (param >> 16) & 0xF;
        /* A swizzled texture carries its own dimensions here, as log2 in
         * BASE_SIZE_U/V. It has to: IMAGE_RECT describes a linear image, and
         * a title that only uses swizzled textures never sends one. */
        if (tex_size_from_format(t->color)) {
            t->width  = 1u << ((param >> 20) & 0xF);
            t->height = 1u << ((param >> 24) & 0xF);
        }
        break;
    case NV097_SET_TEXTURE_PALETTE:
        /* The low six bits carry the DMA context and the entry count. */
        t->dma_palette = param;
        t->palette_set = 1;
        break;
    case NV097_SET_TEXTURE_ADDRESS:
        /* The sampler consumes the same per-axis addressing enums on both
         * software and native paths. Border colour is standing stage state. */
        t->addr_u =  param        & 0xF;
        t->addr_v = (param >>  8) & 0xF;
        nv2a_get_state()->pgraph.regs[NV_PGRAPH_TEXADDRESS0+stage*4]=param;
        break;
    case NV097_SET_TEXTURE_BORDER_COLOR:
        t->border_color = param;
        nv2a_get_state()->pgraph.regs[NV_PGRAPH_BORDERCOLOR0 + stage * 4] = param;
        break;
    case NV097_SET_TEXTURE_CONTROL0:
        nv2a_get_state()->pgraph.regs[NV_PGRAPH_TEXCTL0_0 + stage * 4] = param;
        break;
    case NV097_SET_TEXTURE_SET_BUMP_ENV_MAT:
    case NV097_SET_TEXTURE_SET_BUMP_ENV_MAT+4:
    case NV097_SET_TEXTURE_SET_BUMP_ENV_MAT+8:
    case NV097_SET_TEXTURE_SET_BUMP_ENV_MAT+12:
        if(stage) {
            static const unsigned swizzle[]={NV_PGRAPH_BUMPMAT00,NV_PGRAPH_BUMPMAT01,NV_PGRAPH_BUMPMAT11,NV_PGRAPH_BUMPMAT10};
            nv2a_get_state()->pgraph.regs[swizzle[(reg-NV097_SET_TEXTURE_SET_BUMP_ENV_MAT)/4]+(stage-1)*4]=param;
        }
        break;
    case NV097_SET_TEXTURE_SET_BUMP_ENV_SCALE:
    case NV097_SET_TEXTURE_SET_BUMP_ENV_OFFSET:
        if(stage)nv2a_get_state()->pgraph.regs[(reg==NV097_SET_TEXTURE_SET_BUMP_ENV_SCALE?NV_PGRAPH_BUMPSCALE1:NV_PGRAPH_BUMPOFFSET1)+(stage-1)*4]=param;
        break;
    case NV097_SET_TEXTURE_CONTROL1:
        t->pitch = param >> 16;          /* linear formats only */
        break;
    case NV097_SET_TEXTURE_IMAGE_RECT:
        t->width  = param >> 16;
        t->height = param & 0xFFFF;
        break;
    case 0x1B14:                                  /* SET_TEXTURE_FILTER */
        t->filter = param;
        break;
    default:
        break;
    }
    tex_update_valid(t);
    if (stage == 0)
        record_tex_reg(method, param);
}

/* Scalar commands and native ranges append to the same primitive storage. */
void nv2a_pb_index_words(const uint32_t *words,uint32_t count)
{
    if (!s_gpu.prim || !count) return;
    require_batch_space("index",s_gpu.idx_count,count*2,NV_MAX_INDICES);
    for (uint32_t i=0;i<count;++i) {
        uint32_t value=words[i];
        s_gpu.idx_edge[s_gpu.idx_count]=(uint8_t)nv2a_get_state()->pgraph.edge_flag;
        s_gpu.idx[s_gpu.idx_count++]=(uint16_t)value;
        s_gpu.idx_edge[s_gpu.idx_count]=(uint8_t)nv2a_get_state()->pgraph.edge_flag;
        s_gpu.idx[s_gpu.idx_count++]=(uint16_t)(value>>16);
    }
}

static int nv2a_pb_exec_state(uint32_t method, uint32_t param, uint8_t *output)
{
    switch (method) {
    case NV097_SET_SURFACE_CLIP_HORIZONTAL:
        s_gpu.clip_x = param & 0xFFFF;
        s_gpu.clip_w = (param >> 16) & 0xFFFF;
        break;
    case NV097_SET_SURFACE_CLIP_VERTICAL:
        s_gpu.clip_y = param & 0xFFFF;
        s_gpu.clip_h = (param >> 16) & 0xFFFF;
        break;
    case NV097_SET_SURFACE_FORMAT:
        s_gpu.format = param;
        break;
    case NV097_NO_OPERATION:
        if (param) note_unhandled(method, param);
        break;
    case NV097_WAIT_FOR_IDLE:
#if defined(_WIN32)
        if(hw_enabled() && hw_pending_output(NULL,0,0)) hw_collect_queries(1);
#endif
        break;
    case NV097_BREAK_VERTEX_BUFFER_CACHE:
        if (param) note_unhandled(method, param);
        /* fetch_attr reads the mapped vertex buffer on every draw. There is
         * no persistent vertex cache to invalidate; retain array descriptors
         * and publish memory before the next synchronous fetch. */
#if defined(_WIN32)
        MemoryBarrier();
#else
        __sync_synchronize();
#endif
        break;
    case NV097_SET_SURFACE_PITCH:
        s_gpu.pitch = param & 0xFFFF;
        s_gpu.zeta_pitch = param >> 16;
        break;
    case NV097_SET_SURFACE_COLOR_OFFSET:
        if (s_ftrace == 2)
            fprintf(stderr, "[FTRACE] color offset -> %08X%c", param, 10);
        s_gpu.color_offset = param;
        break;
    case NV097_SET_COLOR_CLEAR_VALUE:
        s_gpu.clear_color = param;
        break;
    case NV097_SET_BLEND_ENABLE:
        s_gpu.blend_enable = param;
        break;
    case NV097_SET_BLEND_EQUATION:
        s_gpu.blend_equation = param;
        break;
    case 0x0358:                                  /* SET_COLOR_MASK */
        s_gpu.color_mask = param;
        break;

    /* Register combiners. */
    case 0x0288: s_gpu.rc.final0 = param; break;  /* SPECULAR_FOG_CW0 */
    case 0x028C: s_gpu.rc.final1 = param; break;  /* SPECULAR_FOG_CW1 */
    case 0x1E20: s_gpu.rc.final_c0 = param; break;/* SPECULAR_FOG_FACTOR */
    case 0x1E24: s_gpu.rc.final_c1 = param; break;
    case 0x1E60:                                  /* COMBINER_CONTROL */
        s_gpu.rc.control = param;
        s_gpu.rc_seen = 1;
        break;
    case 0x1E70: s_gpu.rc.stage_program = param; break;
    case 0x17F8: s_gpu.clip_plane_mode = param; break;

    /* Alpha test and fog. */
    case 0x0300: s_gpu.alpha_test = param; break;
    case 0x033C: s_gpu.alpha_func = param; break;
    case 0x0340: s_gpu.alpha_ref = param; break;
    case 0x02A4: s_gpu.fog_enable = param; break;
    case 0x02A8: s_gpu.fog_color = param; break;  /* R in bits 0-7 */
    case 0x029C: s_gpu.fog_mode = param; break;
    case 0x09C0: memcpy(&s_gpu.fog_param[0], &param, 4); break;
    case 0x09C4: memcpy(&s_gpu.fog_param[1], &param, 4); break;
    case 0x09C8: memcpy(&s_gpu.fog_param[2], &param, 4); break;
    case 0x17C8:                                  /* CLEAR_REPORT_VALUE */
#if defined(_WIN32)
        if (hw_enabled()) hw_collect_queries(1);
#endif
        s_gpu.zpass_count = 0;
        break;
    case NV097_BACK_END_WRITE_SEMAPHORE_RELEASE:
#if defined(_WIN32)
        if(hw_enabled()) {
            if(output) {hw_defer_output(output,param);return 1;}
            /* The original survey executor has no DMA output contract. */
            hw_complete_commands();
        }
#endif
        break;
    case 0x17CC:                                  /* SET_ZPASS_PIXEL_COUNT_ENABLE */
        s_gpu.zpass_enable = param;
        break;
    case 0x17D0: {                                /* GET_REPORT */
#if defined(_WIN32)
        if (hw_enabled()) { hw_collect_queries(1); hw_publish_all(); }
#endif
        /* The GPU's answer to a visibility test: 16 bytes at the report
         * DMA offset -- a timestamp, the pixel count, and 0 for "done".
         * Never written, a title polling it (D3D's GetVisibilityTestResult
         * starts the slot at 0xFFFFFFFF) sees every test incomplete. Burnout
         * 3 culls its world with these, so the race's main view drew only
         * the sky while the cube-map passes, which do not test, drew it all.
         * The bound report object's base and limit come from RAMIN. */
        uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
        NV2AState *gpu = nv2a_get_state();
        if ((param >> 24) != NV097_GET_REPORT_TYPE_ZPASS_PIXEL_CNT)
            note_unhandled(method, param);
        uint32_t at = dma_resolve(gpu->pgraph.dma_report, param & 0x00FFFFFFu, 16);
        uint64_t stamp = ptimer_get_clock(gpu);
        s_gpu.reports++;
        memcpy(mem + at, &stamp, 8);
        memcpy(mem + at + 8, &s_gpu.zpass_count, 4);
        memset(mem + at + 12, 0, 4);
        break;
    }
    case NV097_SET_BLEND_COLOR:
        s_gpu.blend_color = param;
        break;
    case NV097_SET_BLEND_FUNC_SFACTOR:
        s_gpu.blend_sfactor = param;
        break;
    case NV097_SET_BLEND_FUNC_DFACTOR:
        s_gpu.blend_dfactor = param;
        break;
    case NV097_CLEAR_SURFACE:
        if (param & ~(NV097_CLEAR_SURFACE_COLOR | NV097_CLEAR_SURFACE_Z | NV097_CLEAR_SURFACE_STENCIL))
            note_unhandled(method, param);
        if (s_ftrace == 2)
            fprintf(stderr, "[FTRACE] clear %X surf %08X %ux%u colour %08X%c",
                    param, s_gpu.color_offset, s_gpu.clip_w, s_gpu.clip_h,
                    s_gpu.clear_color, 10);
        clear_surface(param);
        if (param & (NV097_CLEAR_SURFACE_Z | NV097_CLEAR_SURFACE_STENCIL)) {
#if defined(_WIN32)
            if (hw_enabled()) hw_clear_depth(param);
            else
#endif
            zbuf_clear(param);
        }
        break;

    case NV097_SET_BEGIN_END:
        if (param) {
            static unsigned point_states;
            if (param == NV_PRIM_POINTS && point_states++ < 4) {
                struct PGRAPHState *point_pg = &nv2a_get_state()->pgraph;
                fprintf(stderr, "[POINT STATE] mode=%u size_eighths=%u raster=%08X csv0_d=%08X control3=%08X inputs=%04X\n",
                        s_gpu.xf_mode & 3, point_pg->regs[NV_PGRAPH_POINTSIZE],
                        point_pg->regs[NV_PGRAPH_SETUPRASTER], point_pg->regs[NV_PGRAPH_CSV0_D],
                        point_pg->regs[NV_PGRAPH_CONTROL_3], nv2a_vsh_inputs_used());
                fprintf(stderr, "[POINT STATE] params");
                for (unsigned point_i = 0; point_i < 8; ++point_i)
                    fprintf(stderr, " %g", point_pg->point_params[point_i]);
                fprintf(stderr, "\n");
            }
            if (param < NV_PRIM_POINTS || param > NV_PRIM_POLYGON)
                note_unhandled(method, param);
            /* Optional hardware early-cull enables stay in the original
             * ZCOMPRESSOCCLUDE register. This uncached pixel path performs the
             * exact depth/stencil comparisons for every fragment; no second
             * hierarchical cache or visibility result is synthesized. */
            s_gpu.prim = param;
            s_gpu.idx_count = 0;
            s_gpu.inline_count = 0;
            s_gpu.imm_count = 0;
        } else {
            /* DMA bindings can change without another offset method. */
            for (unsigned i = 0; i < 4; ++i) tex_update_valid(&s_gpu.texs[i]);
            if (!s_gpu.imm_count && !s_gpu.inline_count) {
                struct PGRAPHState *pg = &nv2a_get_state()->pgraph;
                for (unsigned i = 0; i < NV_VERTEX_ATTRS; ++i) {
                    VertexAttr *a = &s_gpu.attr[i];
                    if (a->dma_bound)
                        a->offset = dma_resolve((a->dma_offset & 0x80000000u) ? pg->dma_vertex_b : pg->dma_vertex_a,
                                                a->dma_offset & 0x7FFFFFFFu, 1);
                }
            }
            /* Three ways a batch can have arrived, and only one is in use at
             * a time: vertices completed by SET_VERTEX4F, a payload written
             * with INLINE_ARRAY, or indices into the title's own arrays. */
            if (s_gpu.imm_count)
                draw_immediate();
            else if (s_gpu.inline_count)
                draw_inline_array();
            else
                draw_primitive();
            s_gpu.prim = 0;
            s_gpu.inline_count = 0;
            s_gpu.imm_count = 0;
        }
        break;

    case NV097_INLINE_ARRAY:
        /* Vertex data, not a pointer to it. Buffered rather than decoded here
         * because the format is only fully known at END. */
        if (s_gpu.prim) {
            require_batch_space("inline", s_gpu.inline_count, 1, NV_MAX_INLINE);
            s_gpu.inline_edge[s_gpu.inline_count]=(uint8_t)nv2a_get_state()->pgraph.edge_flag;
            s_gpu.inline_buf[s_gpu.inline_count++] = param;
        }
        break;

    case NV097_DRAW_ARRAYS: {
        /* The method this title actually draws with, and the reason the
         * executor reported zero draws while geometry was being submitted the
         * whole time: BEGIN_END arrived, END arrived, and in between came a
         * run description rather than the index list the draw path wanted, so
         * every batch ended with idx_count == 0 and was dropped in silence.
         *
         * Expanded into indices because that is what the rasteriser consumes,
         * and an implicit run is just the indices start..start+count-1. */
        uint32_t start = param & 0x00FFFFFFu;
        uint32_t count = ((param >> 24) & 0xFFu) + 1u;
        uint32_t i;

        if (!s_gpu.prim)
            break;
        require_batch_space("index", s_gpu.idx_count, count, NV_MAX_INDICES);
        for (i = 0; i < count; i++) {
            s_gpu.idx_edge[s_gpu.idx_count]=(uint8_t)nv2a_get_state()->pgraph.edge_flag;
            s_gpu.idx[s_gpu.idx_count++] = start + i;
        }
        break;
    }

    case NV097_ARRAY_ELEMENT16:
        /* Two 16-bit indices per parameter word. */
        nv2a_pb_index_words(&param,1);
        break;

    case NV097_ARRAY_ELEMENT32:
        if (s_gpu.prim) {
            require_batch_space("index", s_gpu.idx_count, 1, NV_MAX_INDICES);
            s_gpu.idx_edge[s_gpu.idx_count]=(uint8_t)nv2a_get_state()->pgraph.edge_flag;
            s_gpu.idx[s_gpu.idx_count++] = param;
        }
        break;

    case NV097_SET_FLIP_READ:
        if (s_ftrace == 2)
            fprintf(stderr, "[FTRACE] P_READ %u%c", param, 10);
        s_gpu.flip_read = param;
        return 0;

    case NV097_SET_FLIP_WRITE:
        if (s_ftrace == 2)
            fprintf(stderr, "[FTRACE] P_WRITE %u%c", param, 10);
        s_gpu.flip_write = param;
        return 0;

    case NV097_SET_FLIP_MODULO:
        s_gpu.flip_modulo = param;
        return 0;

    case NV097_FLIP_INCREMENT_WRITE:
        s_gpu.flip_write = s_gpu.flip_modulo
                         ? (s_gpu.flip_write + 1) % s_gpu.flip_modulo
                         : s_gpu.flip_write + 1;
        s_gpu.flips++;
        frame_trace_flip();
        return 0;

    case NV097_FLIP_STALL: {
        uint32_t done=s_gpu.drawn_offset?s_gpu.drawn_offset:s_gpu.color_offset;
        uint32_t pitch=s_gpu.drawn_offset?s_gpu.drawn_pitch:s_gpu.pitch;
        uint32_t displayed=0;
        if(s_gpu.pitch && done)
            displayed=dma_resolve(s_gpu.drawn_offset?s_gpu.drawn_dma_color:nv2a_get_state()->pgraph.dma_color,
                done,(s_gpu.drawn_offset?s_gpu.drawn_y+s_gpu.drawn_h:s_gpu.clip_y+s_gpu.clip_h)*pitch);
#if defined(_WIN32)
        /* PFIFO keeps the completed colour owner alive. Native display keeps
         * its GPU image; explicit RAM display consumes the original publication. */
        D3D8Surface *host_readback=NULL;
        if(s_hw_native_present) {
            if(displayed)hw_finish_native_frame(displayed,pitch);
            else hw_complete_commands();
        } else {
            host_readback=hw_enabled() && displayed?hw_prepare_scaled_color(displayed,pitch):NULL;
            hw_publish_all();
        }
#endif
        if (s_ftrace == 2)
            fprintf(stderr, "[FTRACE] FLIP_STALL presenting %08X (color now %08X)%c",
                    s_gpu.drawn_offset, s_gpu.color_offset, 10);
        /* The stall ends when the buffer being read is the one just finished.
         * There is no scanout here to wait for, so that is now. */
        s_gpu.flip_read = s_gpu.flip_write;
        /* And this is a completed swap, which is what a title's own swap
         * counter counts -- see xbox_Nv2aFrameCounterFlip. */
        xbox_Nv2aFrameCounterFlip();
        /* Hand the window a copy of the frame just finished.
         *
         * The buffer the title has finished is the one the last batch drew
         * into, which is what drawn_offset holds and why it exists: by the
         * flip, color_offset has already moved to the next buffer. Copying
         * here, rather than letting the window read guest memory on its own
         * clock, is what stops it showing a surface the rasteriser is still
         * writing. */
        {
            extern void xbox_FramebufferWindowFrameStats(uint32_t);
            static uint32_t draws_at_flip;
#if defined(_WIN32)
            static int load_timing=-1;
            static ULONGLONG previous_flip_tick,last_logged_tick;
            if (load_timing<0) load_timing=getenv("RECOMP_LOAD_TIMING")!=NULL;
            if (load_timing) {
                ULONGLONG now=GetTickCount64();
                if (!previous_flip_tick || now-previous_flip_tick>=100 || now-last_logged_tick>=1000) {
                    fprintf(stderr,"  [FRAME TIME] t_ms=%llu gap_ms=%llu flip=%u draws=%u\n",
                            (unsigned long long)now,
                            (unsigned long long)(previous_flip_tick ? now-previous_flip_tick : 0),
                            s_gpu.flips,s_gpu.draws-draws_at_flip);
                    last_logged_tick=now;
                }
                previous_flip_tick=now;
            }
#endif
            xbox_FramebufferWindowFrameStats(s_gpu.draws - draws_at_flip);
            draws_at_flip = s_gpu.draws;
        }
        if (s_gpu.pitch && done) {
            extern void xbox_FramebufferWindowPresent(uint32_t, uint32_t);
            xbox_FramebufferWindowSet(displayed,pitch);
#if defined(_WIN32)
            if(!s_hw_native_present)
#endif
                xbox_FramebufferWindowPresent(displayed,pitch);
        }
#if defined(_WIN32)
        if(!s_hw_native_present)hw_present_scaled_color(host_readback);
        else d3d8_LogVideoMemory();
#endif
        {
            /* RECOMP_FB_DUMP_FLIPS: dump every presented frame, not one per
             * report -- a consecutive sequence, which is what a headless
             * recording (frames -> ffmpeg) needs. "1" dumps from boot; any
             * other value names a file, and frames are dumped while it
             * exists, so a capture can start mid-game without writing
             * gigabytes of menus first. */
            static const char *spec = (const char *)-1;
            if (spec == (const char *)-1)
                spec = getenv("RECOMP_FB_DUMP_FLIPS");
            if (spec) {
                int on = strcmp(spec, "1") == 0;
                if (!on) {
                    FILE *f = fopen(spec, "rb");
                    if (f) { fclose(f); on = 1; }
                }
                s_flip_dumping = on;
                if (on)
                    dump_surface_bmp();
            }
        }
        s_gpu.drawn_stale = 1;

        if (s_pb_exec_verbose > 0) {
            static unsigned n;
            if (n++ < 8) {
                fprintf(stderr, "  [GPU] flip %u: read=%u write=%u\n",
                        s_gpu.flips, s_gpu.flip_read, s_gpu.flip_write);
                fflush(stderr);
            }
        }
        return 0;
    }

    /* Vertex programs and their constants: forwarded as they arrive. */
    case 0x1E94:                                  /* TRANSFORM_EXECUTION_MODE */
        s_gpu.xf_mode = param;
        break;
    case 0x1E98:                                  /* _PROGRAM_CXT_WRITE_EN */
        nv2a_vsh_set_cxt_write(param);
        break;
    case 0x1E9C:                                  /* _PROGRAM_LOAD */
        nv2a_vsh_set_load_slot(param);
        break;
    case 0x1EA0:                                  /* _PROGRAM_START */
        nv2a_vsh_set_start_slot(param);
        break;
    case 0x1EA4:                                  /* _CONSTANT_LOAD */
        nv2a_vsh_set_constant_load(param);
        break;

    /* Depth. */
    case 0x030C: s_gpu.depth_test = param; break; /* DEPTH_TEST_ENABLE */
    case 0x0354: s_gpu.depth_func = param; break; /* DEPTH_FUNC */
    case 0x035C: s_gpu.depth_mask = param; break; /* DEPTH_MASK */
    case 0x0214: s_gpu.zeta_offset = param; break;/* SURFACE_ZETA_OFFSET */
    case 0x1D8C: s_gpu.zstencil_clear = param; break; /* ZSTENCIL_CLEAR_VALUE */

    default:
        if (method >= NV097_SET_PROJECTION_MATRIX && method < NV097_SET_PROJECTION_MATRIX + 64) {
            unsigned slot = (method - NV097_SET_PROJECTION_MATRIX) / 4;
            nv2a_vsh_constant_component(NV_IGRAPH_XF_XFCTX_PMAT0 + slot / 4, slot % 4, param);
            break;
        }
        if (method >= NV097_SET_MODEL_VIEW_MATRIX && method < NV097_SET_MODEL_VIEW_MATRIX + 256) {
            unsigned slot = (method - NV097_SET_MODEL_VIEW_MATRIX) / 4;
            nv2a_vsh_constant_component(NV_IGRAPH_XF_XFCTX_MMAT0 + (slot / 16) * 8 + (slot % 16) / 4,
                                        slot % 4, param);
            break;
        }
        if (method >= NV097_SET_INVERSE_MODEL_VIEW_MATRIX && method < NV097_SET_INVERSE_MODEL_VIEW_MATRIX + 256) {
            unsigned slot = (method - NV097_SET_INVERSE_MODEL_VIEW_MATRIX) / 4;
            nv2a_vsh_constant_component(NV_IGRAPH_XF_XFCTX_IMMAT0 + (slot / 16) * 8 + (slot % 16) / 4,
                                        slot % 4, param);
            break;
        }
        if (method >= NV097_SET_COMPOSITE_MATRIX && method < NV097_SET_COMPOSITE_MATRIX + 64) {
            unsigned slot = (method - NV097_SET_COMPOSITE_MATRIX) / 4;
            nv2a_vsh_constant_component(NV_IGRAPH_XF_XFCTX_CMAT0 + slot / 4, slot % 4, param);
            break;
        }
        if (method >= NV097_SET_TEXTURE_MATRIX && method < NV097_SET_TEXTURE_MATRIX + 256) {
            unsigned slot = (method - NV097_SET_TEXTURE_MATRIX) / 4;
            nv2a_vsh_constant_component(NV_IGRAPH_XF_XFCTX_T0MAT + (slot / 16) * 8 + (slot % 16) / 4,
                                        slot % 4, param);
            break;
        }
        if (method >= NV097_SET_EYE_POSITION && method < NV097_SET_EYE_POSITION + 16) {
            nv2a_vsh_constant_component(NV_IGRAPH_XF_XFCTX_EYEP,
                                        (method - NV097_SET_EYE_POSITION) / 4, param);
            break;
        }
        if (method >= NV097_SET_FOG_PLANE && method < NV097_SET_FOG_PLANE + 16) {
            nv2a_vsh_constant_component(NV_IGRAPH_XF_XFCTX_FOG,
                                        (method - NV097_SET_FOG_PLANE) / 4, param);
            break;
        }
        if (method >= NV097_SET_TEXGEN_PLANE_S && method < NV097_SET_TEXGEN_PLANE_S + 256) {
            unsigned slot = (method - NV097_SET_TEXGEN_PLANE_S) / 4;
            nv2a_vsh_constant_component(NV_IGRAPH_XF_XFCTX_TG0MAT + (slot / 16) * 8 + (slot % 16) / 4,
                                        slot % 4, param);
            break;
        }
        if (method >= 0x0260 && method < 0x0280) {         /* ALPHA_ICW(i) */
            s_gpu.rc.alpha_icw[(method - 0x0260) / 4] = param;
            break;
        }
        if (method >= 0x0AC0 && method < 0x0AE0) {         /* COLOR_ICW(i) */
            s_gpu.rc.color_icw[(method - 0x0AC0) / 4] = param;
            break;
        }
        if (method >= 0x1E40 && method < 0x1E60) {         /* COLOR_OCW(i) */
            s_gpu.rc.color_ocw[(method - 0x1E40) / 4] = param;
            break;
        }
        if (method >= 0x0AA0 && method < 0x0AC0) {         /* ALPHA_OCW(i) */
            s_gpu.rc.alpha_ocw[(method - 0x0AA0) / 4] = param;
            break;
        }
        if (method >= 0x0A60 && method < 0x0A80) {         /* FACTOR0(i) */
            s_gpu.rc.factor0[(method - 0x0A60) / 4] = param;
            break;
        }
        if (method >= 0x0A80 && method < 0x0AA0) {         /* FACTOR1(i) */
            s_gpu.rc.factor1[(method - 0x0A80) / 4] = param;
            break;
        }
        /* SET_VIEWPORT_OFFSET / _SCALE. The GPU keeps these in the constant
         * file, at c[59] and c[58] -- exactly where the D3D epilogue reads
         * them -- so they are constants that happen to arrive by method.
         * Dropped, every vertex program put its whole batch at the origin:
         * no 3D anywhere, and a black car on the garage screen. */
        if (method >= 0x0A20 && method < 0x0A30) {
            nv2a_vsh_constant_component(59, (method - 0x0A20) / 4, param);
            break;
        }
        if (method >= 0x0AF0 && method < 0x0B00) {
            nv2a_vsh_constant_component(58, (method - 0x0AF0) / 4, param);
            break;
        }
        if (method >= NV_TEX_FIRST && method <= NV_TEX_LAST)
            record_tex_reg(method, param);
        if (method >= NV097_SET_VERTEX_DATA_ARRAY_OFFSET
                && method < NV097_SET_VERTEX_DATA_ARRAY_OFFSET + NV_VERTEX_ATTRS * 4) {
            /* Preserve the hardware offset and its vertex DMA selection;
             * refresh the mapped address when an indexed batch is consumed. */
            VertexAttr *a = &s_gpu.attr[(method - NV097_SET_VERTEX_DATA_ARRAY_OFFSET) / 4];
            a->dma_offset = param;
            a->dma_bound = 1;
            a->offset = dma_resolve((param & 0x80000000u) ? nv2a_get_state()->pgraph.dma_vertex_b : nv2a_get_state()->pgraph.dma_vertex_a,
                                    param & 0x7FFFFFFFu, 1);
        } else if (method >= NV097_SET_VERTEX_DATA_ARRAY_FORMAT
                && method < NV097_SET_VERTEX_DATA_ARRAY_FORMAT + NV_VERTEX_ATTRS * 4) {
            VertexAttr *a = &s_gpu.attr[(method - NV097_SET_VERTEX_DATA_ARRAY_FORMAT) / 4];
            a->type   =  param        & 0x0F;
            a->size   = (param >> 4)  & 0x0F;
            a->stride = (param >> 8)  & 0xFF;
        } else if (!imm_vertex_method(method, param)) {
            note_unhandled(method, param);
        }
        break;
    }
    return 0;
}

/* Keep initialization and tracing out of the per-word dispatch body. The
 * same consumer handles both diagnostic and ordinary calls. */
static int nv2a_pb_exec_active(uint32_t subch, uint32_t method, uint32_t param, uint8_t *output)
{
    if (method >= NV_TEX_FIRST && method <= NV_TEX_LAST) {
        tex_stage_method(method, param);
        return 0;
    }
    /* PGRAPH has already selected the bound class and published this
     * subchannel's context. Kelvin methods share the same consumers on all
     * eight subchannels; the channel number is not a class identity. */
    (void)subch;
    if (method >= 0x0B00 && method < 0x0B80) {    /* TRANSFORM_PROGRAM(i) */
        nv2a_vsh_program_word(param);
        return 0;
    }
    if (method >= 0x0B80 && method < 0x0C00) {    /* TRANSFORM_CONSTANT(i) */
        nv2a_vsh_constant_word(param);
        return 0;
    }
    return nv2a_pb_exec_state(method, param, output);
}

static NV_NOINLINE int nv2a_pb_exec_diagnostic(uint32_t subch, uint32_t method, uint32_t param, uint8_t *output)
{
    static int inited;
    if (!inited) {
        inited = 1;
        s_gpu.color_mask = 0x01010101u;           /* all channels, as reset */
        {
            int i;
            for (i = 0; i < NV_VERTEX_ATTRS; i++)
                s_gpu.imm_attr[i][3] = 1.0f;
        }
        s_gpu.min_x = s_gpu.min_y = 1e30f;
        s_gpu.max_x = s_gpu.max_y = -1e30f;
    }
    /* Bring-up: the first parameters each surface method carries. A wrong
     * pitch or clip is indistinguishable from a method never arriving unless
     * the values are visible. Cached: this runs for every method, and an
     * uncached getenv here was a quarter of the executor's time. */
    if (s_pb_exec_verbose < 0)
        s_pb_exec_verbose = getenv("RECOMP_PB_EXEC_VERBOSE") != NULL;
    if (s_pb_exec_verbose) {
        static int shown[8];
        int slot = -1;
        switch (method) {
        case NV097_SET_SURFACE_CLIP_HORIZONTAL: slot = 0; break;
        case NV097_SET_SURFACE_CLIP_VERTICAL:   slot = 1; break;
        case NV097_SET_SURFACE_FORMAT:          slot = 2; break;
        case NV097_SET_SURFACE_PITCH:           slot = 3; break;
        case NV097_SET_SURFACE_COLOR_OFFSET:    slot = 4; break;
        case NV097_SET_COLOR_CLEAR_VALUE:       slot = 5; break;
        case NV097_CLEAR_SURFACE:               slot = 6; break;
        default: break;
        }
        if (slot >= 0 && shown[slot]++ < 4)
            fprintf(stderr, "  [GPU] subch %u method 0x%04X param 0x%08X\n",
                    subch, method, param);
    }

    if (s_ftrace == 2) {
        /* RECOMP_FRAME_TRACE_METHODS: every method of the traced frame,
         * less the bulk (vertex data, program and constant uploads). */
        static int all = -1;
        if (all < 0)
            all = getenv("RECOMP_FRAME_TRACE_METHODS") != NULL;
        if (all && !(method >= 0x1800 && method < 0x1A00)
            && !(method >= 0x0B00 && method < 0x0C00))
            fprintf(stderr, "[FTRACE]   m %u:%04X = %08X%c", subch, method,
                    param, 10);
    }
    return nv2a_pb_exec_active(subch,method,param,output);
}

int nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param, uint8_t *output)
{
    if(s_pb_exec_verbose || s_ftrace==2)
        return nv2a_pb_exec_diagnostic(subch,method,param,output);
    return nv2a_pb_exec_active(subch,method,param,output);
}

/* Both modes collect the same ordered completion queries. */
void nv2a_pb_request_progress(void)
{
#if defined(_WIN32)
    if (ReadAcquire64(&s_hw_query_count)) {
        NV2AState *d=nv2a_get_state();
        if (d && d->pfifo.thread.thread) {
            /* Progress publishes no commands. A posted request already owns
             * the next collection pass; running alone still needs a request.
             * Keep PUT publication on the unchanged generic kick path. */
            if (!(ReadAcquire(&d->pfifo.fifo_kick) & 1)) nv2a_fifo_kick(d);
        } else if (hw_enabled()) hw_collect_queries(0);
    }
#endif
}
void nv2a_pb_progress(void)
{
#if defined(_WIN32)
    if(hw_enabled() && ReadAcquire64(&s_hw_query_count)) hw_collect_queries(0);
#endif
}

typedef struct PbCheckpoint {
    uint32_t state_bytes,hardware;
    PbState state;
    uint32_t texture_registers[sizeof s_tex_reg/sizeof s_tex_reg[0]];
    uint8_t texture_set[sizeof s_tex_set];
} PbCheckpoint;
int nv2a_gpu_save_checkpoint(FILE *out) {
    if(!out || !xbox_CheckpointFrozen()) return 0;
    nv2a_pb_checkpoint_publish();PbCheckpoint *saved=calloc(1,sizeof *saved);if(!saved) return 0;
    saved->state_bytes=sizeof saved->state;saved->state=s_gpu;
    memcpy(saved->texture_registers,s_tex_reg,sizeof s_tex_reg);memcpy(saved->texture_set,s_tex_set,sizeof s_tex_set);
#if defined(_WIN32)
    saved->hardware=(uint32_t)hw_enabled();
#endif
    int ok=fwrite("XBGPUR",6,1,out)==1 && fwrite(saved,sizeof *saved,1,out)==1 &&
        nv2a_save_checkpoint(out) && nv2a_vsh_save_checkpoint(out);
#if defined(_WIN32)
    if(ok && saved->hardware) ok=hw_save_depths(out);
#endif
    free(saved);return ok;
}
int nv2a_gpu_load_checkpoint(FILE *in) {
    if(!in || !xbox_CheckpointFrozen()) return 0;
    char magic[6];PbCheckpoint *saved=malloc(sizeof *saved);if(!saved) return 0;
    int ok=fread(magic,6,1,in)==1 && !memcmp(magic,"XBGPUR",6) && fread(saved,sizeof *saved,1,in)==1 &&
        saved->state_bytes==sizeof saved->state && saved->state.idx_count<=NV_MAX_INDICES &&
        saved->state.inline_count<=NV_MAX_INLINE && saved->state.imm_count<=NV_MAX_INLINE/IMM_VERTEX_DWORDS;
#if defined(_WIN32)
    ok=ok && saved->hardware==(uint32_t)hw_enabled();
#else
    ok=ok && !saved->hardware;
#endif
    void *registers=NULL,*vsh=NULL;
#if defined(_WIN32)
    HwDepth *depths=NULL;HwColor *colors=NULL;
#endif
    if(ok) ok=(registers=nv2a_prepare_checkpoint(in))!=NULL && (vsh=nv2a_vsh_prepare_checkpoint(in))!=NULL;
#if defined(_WIN32)
    if(ok && saved->hardware) ok=hw_prepare_depths(in,&depths,&colors);
#endif
    if(ok) {
#if defined(_WIN32)
        if(saved->hardware) { hw_enter();hw_apply_depths(depths,colors);depths=NULL;colors=NULL;hw_leave(); }
#endif
        nv2a_apply_checkpoint(registers);nv2a_vsh_apply_checkpoint(vsh);s_gpu=saved->state;
        memcpy(s_tex_reg,saved->texture_registers,sizeof s_tex_reg);memcpy(s_tex_set,saved->texture_set,sizeof s_tex_set);
        s_surface=NULL;
    }
#if defined(_WIN32)
    hw_free_depths(depths);hw_free_colors(colors);
#endif
    free(registers);free(vsh);free(saved);return ok;
}

/* Find where the title actually wrote its quad.
 *
 * The GPU is pointed at a buffer that stays zero, which says the data went
 * somewhere else -- and the only way to find somewhere else is to look for the
 * data. A screen-space quad for a 640x480 target contains 640.0f and 480.0f as
 * floats, which is a distinctive enough pair to search guest RAM for. Whatever
 * address that turns up is where the title's writes are landing, and the
 * difference from the programmed offset is the bug. */
static void find_quad_vertices(void)
{
    const uint32_t W = 0x44200000u;   /* 640.0f */
    const uint32_t H = 0x43F00000u;   /* 480.0f */
    const uint32_t *ram = (const uint32_t *)xbox_GetMemoryOffset();
    uint32_t i, hits = 0;

    fprintf(stderr, "[GPU] searching guest RAM for 640.0f/480.0f pairs...\n");
    for (i = 0x1000 / 4; i < (0x04000000u / 4) - 8 && hits < 12; i++) {
        if (ram[i] != W && ram[i] != H)
            continue;
        /* Both values within a few words of each other: a lone 640.0f is
         * common, the pair much less so. */
        {
            int has_w = 0, has_h = 0;
            uint32_t k;
            for (k = 0; k < 8; k++) {
                if (ram[i + k] == W) has_w = 1;
                if (ram[i + k] == H) has_h = 1;
            }
            if (!has_w || !has_h)
                continue;
        }
        hits++;
        fprintf(stderr, "  [GPU]   0x%08X:", i * 4);
        {
            uint32_t k;
            for (k = 0; k < 8; k++)
                fprintf(stderr, " %08X", ram[i + k]);
        }
        fprintf(stderr, "\n");
        i += 8;
    }
    if (!hits)
        fprintf(stderr, "  [GPU]   none found -- the quad is not in RAM in"
                        " that form\n");
    fflush(stderr);
}

/* Locate NaN-filled transform matrices in guest RAM.
 *
 * A matrix arriving as NaN says the maths went wrong somewhere upstream, and
 * the only way to find where is to find the matrix and watch who writes it.
 * Three consecutive real-indefinite values is a distinctive enough signature:
 * ordinary data does not contain runs of 0xFFC00000. */
static void find_nan_matrices(void)
{
    const uint32_t NAN_NEG = 0xFFC00000u;
    const uint32_t *ram = (const uint32_t *)xbox_GetMemoryOffset();
    uint32_t i, hits = 0;

    fprintf(stderr, "[GPU] searching guest RAM for NaN matrices...\n");
    for (i = 0x1000 / 4; i < (0x04000000u / 4) - 20 && hits < 10; i++) {
        if (ram[i] != NAN_NEG || ram[i + 1] != NAN_NEG || ram[i + 2] != NAN_NEG)
            continue;
        hits++;
        fprintf(stderr, "  [GPU]   0x%08X:", i * 4);
        {
            uint32_t k;
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %08X", ram[i + k]);
        }
        fprintf(stderr, "\n");
        i += 16;
    }
    if (!hits)
        fprintf(stderr, "  [GPU]   none in RAM -- the NaNs are computed into"
                        " registers, not stored\n");
    fflush(stderr);
}

/* Print guest dwords named by RECOMP_PEEK, as hex and as float.
 *
 * Chasing a value backwards means reading it, and a value that is only wrong
 * for one frame in a thousand cannot be caught by stopping. Both
 * interpretations are printed because the question is usually "is this a
 * pointer or a number", and guessing wrong costs a run. */
static void peek_addresses(void)
{
    const char *spec = getenv("RECOMP_PEEK");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    char buf[256];
    char *tok, *ctx = NULL;

    if (!spec)
        return;
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
        uint32_t va = (uint32_t)strtoul(tok, NULL, 0);
        uint32_t v;
        float f;
        if (va < 0x1000u || va >= 0x04000000u)
            continue;
        v = *(const uint32_t *)(mem + va);
        memcpy(&f, &v, 4);
        fprintf(stderr, "  [PEEK] 0x%08X = %08X  (%g)\n", va, v, f);
    }
    fflush(stderr);
}

/* Walk a pointer chain and print every step.
 *
 * RECOMP_PEEK_CHAIN="0x1315A8,8,0x10,0" starts at that address, and for each
 * offset dereferences the current pointer and adds it. Following a chain by
 * hand costs one run per level; this costs one run for the whole chain, and
 * prints where it goes wrong when a level is null.
 */
static void peek_chain(void)
{
    const char *spec = getenv("RECOMP_PEEK_CHAIN");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    char buf[256], *tok, *ctx = NULL;
    uint32_t cur = 0;
    int step = 0;

    if (!spec)
        return;
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;

    for (tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
        uint32_t off = (uint32_t)strtoul(tok, NULL, 0);
        if (step == 0) {
            cur = off;
            fprintf(stderr, "  [CHAIN] start 0x%08X\n", cur);
        } else {
            if (cur < 0x1000u || cur + 4 >= 0x04000000u) {
                fprintf(stderr, "  [CHAIN] step %d: 0x%08X is not a guest"
                                " pointer -- chain ends\n", step, cur);
                return;
            }
            cur = *(const uint32_t *)(mem + cur) + off;
            fprintf(stderr, "  [CHAIN] step %d: deref +0x%X -> 0x%08X\n",
                    step, off, cur);
        }
        step++;
    }
    if (cur >= 0x1000u && cur + 4 < 0x04000000u)
        fprintf(stderr, "  [CHAIN] final value at 0x%08X = 0x%08X\n",
                cur, *(const uint32_t *)(mem + cur));
    fflush(stderr);
}

void nv2a_pb_exec_report(void)
{
    peek_addresses();
    peek_chain();
    if (getenv("RECOMP_FIND_NAN")) {
        /* Every report, not once: the matrix is fine early on and only turns
         * to NaN later, so a single scan at startup finds nothing and says
         * nothing. */
        find_nan_matrices();
    }
    if (getenv("RECOMP_FIND_QUAD")) {
        static int done;
        if (!done) { done = 1; find_quad_vertices(); }
    }
    int i, j;

    {
        /* Frames per second of guest time, from flips between reports. */
        static uint32_t last_flips;
        static unsigned long last_ms;
        unsigned long now = (unsigned long)(clock() * 1000.0 / CLOCKS_PER_SEC);
        if (last_ms && now > last_ms)
            fprintf(stderr, "[GPU] %.2f fps (%u flips)%c",
                    (s_gpu.flips - last_flips) * 1000.0 / (now - last_ms),
                    s_gpu.flips, 10);
        last_flips = s_gpu.flips;
        last_ms = now;
    }
    fprintf(stderr, "[GPU] surface 0x%08X pitch %u clip %ux%u+%u+%u"
                    " clears %u | %u unhandled methods (%d distinct)\n",
            s_gpu.color_offset, s_gpu.pitch, s_gpu.clip_w, s_gpu.clip_h,
            s_gpu.clip_x, s_gpu.clip_y, s_gpu.clears,
            s_gpu.unhandled_total, s_unhandled_count);
    fprintf(stderr, "[GPU] draws %u (%u with coordinates), %u indices;"
                    " x %.1f..%.1f  y %.1f..%.1f\n",
            s_gpu.draws, s_gpu.nonzero_draws, s_gpu.verts,
            s_gpu.min_x, s_gpu.max_x, s_gpu.min_y, s_gpu.max_y);
    /* One picture per report rather than per clear: a title clears hundreds of
     * times a second and nobody wants that many files. */
    if (!s_flip_dumping)
        dump_surface_bmp();

    /* Drawn and skipped separately: "nothing appeared" and "every batch needed
     * a vertex program we do not run" look identical on screen, and only one
     * of them means the rasteriser is broken. */
    fprintf(stderr, "[GPU] brightest pixel written 0x%08X\n", s_gpu.pixel_max);
    fprintf(stderr, "[GPU] %llu pixels written; draw surface 0x%08X"
                    " -> 0x%08X, clear surface 0x%08X -> 0x%08X\n",
            (unsigned long long)s_gpu.pixels, s_gpu.drawn_offset,
            (s_gpu.drawn_offset ? dma_resolve(s_gpu.drawn_dma_color, s_gpu.drawn_offset, 1) : 0), s_gpu.color_offset,
            color_dma_resolve(s_gpu.color_offset, (s_gpu.clip_y + s_gpu.clip_h) * s_gpu.pitch));
    fprintf(stderr, "[GPU] rasterised %u triangles; %u batches skipped as not"
                    " screen-space, %u triangles fully off-surface\n",
            s_gpu.tris_drawn, s_gpu.batches_untransformed,
            s_gpu.tris_skipped_offscreen);
    fprintf(stderr, "[GPU] vertex programs: %u batches, %u vertices;"
                    " %u triangles dropped behind the eye\n",
            s_gpu.batches_program, s_gpu.verts_program, s_gpu.tris_behind);
    fprintf(stderr, "[GPU]   of the rest: %u degenerate/NaN, %u off-surface,"
                    " %u drawn; pixels %llu written, %llu depth-failed;"
                    " x %.0f..%.0f y %.0f..%.0f z %g..%g\n",
            s_gpu.xf_degenerate, s_gpu.xf_offscreen, s_gpu.xf_drawn,
            (unsigned long long)s_gpu.xf_pixels,
            (unsigned long long)s_gpu.xf_depth_fail,
            s_gpu.xf_min[0], s_gpu.xf_max[0], s_gpu.xf_min[1], s_gpu.xf_max[1],
            s_gpu.xf_min[2], s_gpu.xf_max[2]);
    {
        fprintf(stderr, "[GPU]   blend pairs (src/dst):");
        for (j = 0; j < s_gpu.blend_npairs; j++)
            fprintf(stderr, " %X/%X", s_gpu.blend_pairs[j] >> 16,
                    s_gpu.blend_pairs[j] & 0xFFFF);
        fputc(10, stderr);
        fprintf(stderr, "[GPU]   program attribute formats (slot:type/size):");
        for (j = 0; j < s_gpu.xf_nfmt; j++)
            fprintf(stderr, " v%u:%u/%u", s_gpu.xf_fmt[j] >> 8,
                    s_gpu.xf_fmt[j] & 15, (s_gpu.xf_fmt[j] >> 4) & 15);
        fprintf(stderr, "\n");
    }

    /* And of the batches that did rasterise, how many sampled anything. A menu
     * that draws its background from one texture and its text from another
     * shows both as flat colour if either half is missing, so the split is
     * what says which half. */
    fprintf(stderr, "[GPU] batches: %u textured, %u with no texcoords,"
                    " %u with texcoords but no usable stage\n",
            s_gpu.batches_textured, s_gpu.batches_no_uv, s_gpu.batches_no_tex);
    for (i = 0; i < s_tex_use_count; i++)
        fprintf(stderr, "  [TEXUSE] 0x%08X %ux%u fmt 0x%02X%s: %u batches\n",
                s_tex_use[i].offset, s_tex_use[i].width, s_tex_use[i].height,
                s_tex_use[i].color,
                d3d8_format_dxt_block_bytes(s_tex_use[i].color) ? " dxt"
                    : d3d8_format_is_swizzled(s_tex_use[i].color) ? " swz" : " lin",
                s_tex_use[i].batches);

    if (getenv("RECOMP_TEX_STATE")) {
        uint32_t k;
        for (k = 0; k < sizeof s_tex_set / sizeof s_tex_set[0]; k++)
            if (s_tex_set[k])
                fprintf(stderr, "  [TEX] 0x%04X = 0x%08X\n",
                        (unsigned)(NV_TEX_FIRST + k * 4), s_tex_reg[k]);
    }

    /* Top ten by frequency: selection sort over a small table, once every few
     * seconds, is not worth a better algorithm.
     *
     * RECOMP_PB_UNHANDLED_ALL lists every one instead. Ten is the right
     * default -- the tail is a long list of state registers nobody needs to
     * read -- but when a title stops and the question is which method it
     * stopped on, the answer is as likely to be the one seen twice as the
     * one seen a thousand times, and ten hides it. */
    {
        int shown = getenv("RECOMP_PB_UNHANDLED_ALL") ? s_unhandled_count : 10;
    for (i = 0; i < shown && i < s_unhandled_count; i++) {
        int best = i;
        for (j = i + 1; j < s_unhandled_count; j++)
            if (s_unhandled[j].count > s_unhandled[best].count)
                best = j;
        if (best != i) {
            PbUnhandled t = s_unhandled[i];
            s_unhandled[i] = s_unhandled[best];
            s_unhandled[best] = t;
        }
        {
            /* The value as well as the count. A method nobody decoded is
             * a guess until you see what it carried: screen coordinates,
             * a 0..1 texcoord and a packed colour are told apart at a
             * glance, and that is what says which vertex encoding a title
             * is using. */
            union { uint32_t u; float f; } v;
            v.u = s_unhandled[i].last_param;
            fprintf(stderr, "  [GPU]   0x%04X x%-8u last=0x%08X (%.4f)\n",
                    s_unhandled[i].method, s_unhandled[i].count,
                    v.u, v.f);
        }
    }
    }    fflush(stderr);
}


