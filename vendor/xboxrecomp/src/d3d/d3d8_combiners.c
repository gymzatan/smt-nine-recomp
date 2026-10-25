/**
 * NV2A Register Combiner to HLSL Pixel Shader Translator - Implementation
 *
 * Translates Xbox NV2A register combiner configurations into HLSL pixel
 * shaders compiled for D3D11. See d3d8_combiners.h for the full model
 * description.
 *
 * Implementation overview:
 *
 * 1. STATE TRACKING
 *    The game sets combiner configuration through either:
 *    (a) SetPixelShader(DWORD token) - a packed DWORD encoding combiner
 *        count and texture modes, with actual stage config in render states
 *    (b) Direct render state writes (D3DRS_PSALPHAINPUTS0..7, etc.)
 *    We parse either path into an NV2ACombinerState structure.
 *
 * 2. HLSL GENERATION
 *    From the combiner state, we emit a complete HLSL pixel shader that:
 *    - Samples textures based on tex_mode per stage
 *    - Walks each active general combiner stage performing AB*CD math
 *    - Executes the final combiner (lerp + add)
 *    - Handles alpha test and fog
 *
 * 3. SHADER CACHE
 *    We hash the full NV2ACombinerState and maintain a fixed-size cache
 *    (128 entries) of compiled ID3D11PixelShader objects. Most Xbox games
 *    use fewer than 20 unique combiner configurations, so this is ample.
 *
 * 4. DRAW INTEGRATION
 *    d3d8_combiners_prepare_draw() is called before each draw. It checks
 *    if state is dirty, rebuilds/looks up the shader, uploads constants,
 *    and binds everything to the D3D11 pipeline.
 */

#include "d3d8_internal.h"
#include "d3d8_combiners.h"
#include <d3dcompiler.h>
#include <string.h>
#include <stdio.h>

#pragma comment(lib, "d3dcompiler.lib")

/* ================================================================
 * Internal State
 * ================================================================ */

/** Current pixel shader token (0 = no combiner shader / fixed-function). */
static DWORD g_ps_token = 0;

/** Current parsed combiner state. */
static NV2ACombinerState g_combiner_state;

/** Dirty flag - set when any PS render state changes. */
static BOOL g_dirty = TRUE;

/** PS constant buffer and its last successfully uploaded complete contents. */
static ID3D11Buffer *g_combiner_cb = NULL;
static NV2APSConstants g_combiner_value;
static BOOL g_combiner_value_valid = FALSE;

/* ================================================================
 * Shader Cache
 *
 * Simple open-addressing hash table with linear probing.
 * 128 entries is generous - most games use <20 unique PS configs.
 * On a full table, the oldest entry is evicted (LRU approximation
 * via frame counter).
 * ================================================================ */

#define COMBINER_CACHE_SIZE 128

typedef struct CombinerCacheEntry {
    BOOL                in_use;
    uint32_t            hash;
    NV2ACombinerState   state;
    ID3D11PixelShader  *shader;
    NvkShader         *vulkan;
    uint32_t            last_used_frame;
} CombinerCacheEntry;

static CombinerCacheEntry g_cache[COMBINER_CACHE_SIZE];
static uint32_t g_frame_counter = 0;
static int g_last_lookup_slot = -1;

/* ================================================================
 * Hashing
 *
 * FNV-1a over the combiner state structure. This is fast enough
 * for our purposes and produces good distribution.
 * ================================================================ */

static uint32_t fnv1a_hash(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 0x811C9DC5u;
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x01000193u;
    }
    return h;
}

static uint32_t combiner_state_hash(const NV2ACombinerState *state)
{
    return fnv1a_hash(state, sizeof(NV2ACombinerState));
}

static BOOL combiner_state_equal(const NV2ACombinerState *a,
                                 const NV2ACombinerState *b)
{
    return memcmp(a, b, sizeof(NV2ACombinerState)) == 0;
}

/* ================================================================
 * Color Helpers
 *
 * Convert D3DCOLOR (ARGB packed DWORD) to float4 (RGBA).
 * D3DCOLOR byte layout in memory: BGRA (little-endian ARGB).
 * ================================================================ */

static void d3dcolor_to_float4(DWORD color, float out[4])
{
    out[0] = ((color >> 16) & 0xFF) / 255.0f; /* R */
    out[1] = ((color >>  8) & 0xFF) / 255.0f; /* G */
    out[2] = ((color >>  0) & 0xFF) / 255.0f; /* B */
    out[3] = ((color >> 24) & 0xFF) / 255.0f; /* A */
}

/* ================================================================
 * Combiner Input Parsing
 *
 * Each combiner input is packed as 8 bits in the render state DWORDs:
 *   [3:0] register select (NV2ACombinerRegister value)
 *   [4]   alpha channel replicate
 *   [7:5] input mapping mode (NV2AInputMapping value)
 * ================================================================ */

static void parse_combiner_input(DWORD packed, NV2ACombinerInput *input)
{
    input->reg       = (NV2ACombinerRegister)(packed & 0xF);
    input->alpha_rep = (packed >> 4) & 1;
    input->mapping   = (NV2AInputMapping)((packed >> 5) & 0x7);
}

/**
 * Parse a 32-bit input register DWORD containing 4 packed inputs.
 * Layout: [31:24]=D [23:16]=C [15:8]=B [7:0]=A
 */
static void parse_four_inputs(DWORD dword, NV2ACombinerInput inputs[4])
{
    parse_combiner_input((dword >>  0) & 0xFF, &inputs[0]); /* A */
    parse_combiner_input((dword >>  8) & 0xFF, &inputs[1]); /* B */
    parse_combiner_input((dword >> 16) & 0xFF, &inputs[2]); /* C */
    parse_combiner_input((dword >> 24) & 0xFF, &inputs[3]); /* D */
}

/**
 * Parse a 32-bit output configuration DWORD for one channel.
 *
 * Output DWORD layout:
 *   [3:0]   AB destination register
 *   [7:4]   CD destination register
 *   [11:8]  SUM destination register
 *   [12]    CD dot product flag
 *   [13]    AB dot product flag
 *   [14]    mux_sum flag (mux instead of sum)
 *   [17:15] output mapping (scale/bias)
 *   Bits 18-31 are reserved/unused.
 */
static void parse_output(DWORD dword, NV2ACombinerOutput *output)
{
    output->ab_dst     = (NV2ACombinerRegister)((dword >>  0) & 0xF);
    output->cd_dst     = (NV2ACombinerRegister)((dword >>  4) & 0xF);
    output->sum_dst    = (NV2ACombinerRegister)((dword >>  8) & 0xF);
    output->cd_dot     = (dword >> 12) & 1;
    output->ab_dot     = (dword >> 13) & 1;
    output->mux_sum    = (dword >> 14) & 1;
    output->output_map = (NV2AOutputMapping)((dword >> 15) & 0x7);
}

/* ================================================================
 * Token & Render State Parsing
 * ================================================================ */

void d3d8_combiners_parse_token(DWORD token, const DWORD *rs,
                                NV2ACombinerState *state)
{
    int i;
    memset(state, 0, sizeof(*state));

    /* Bits [3:0]: number of active combiner stages (1-8) */
    state->num_stages = token & 0xF;
    if (state->num_stages < 1) state->num_stages = 1;
    if (state->num_stages > NV2A_MAX_COMBINER_STAGES)
        state->num_stages = NV2A_MAX_COMBINER_STAGES;

    /* Bits [8:23]: texture mode per stage (4 bits each) */
    state->tex_mode[0] = (NV2ATextureMode)((token >>  8) & 0xF);
    state->tex_mode[1] = (NV2ATextureMode)((token >> 12) & 0xF);
    state->tex_mode[2] = (NV2ATextureMode)((token >> 16) & 0xF);
    state->tex_mode[3] = (NV2ATextureMode)((token >> 20) & 0xF);

    /* Bits [24:31]: dot mapping and other flags */
    state->flags = (token >> 24) & 0xFF;

    /* Parse per-stage inputs and outputs from render states */
    d3d8_combiners_from_render_states(rs, state);

    /* Preserve the token-derived fields (from_render_states may overwrite) */
    state->num_stages = token & 0xF;
    if (state->num_stages < 1) state->num_stages = 1;
    if (state->num_stages > NV2A_MAX_COMBINER_STAGES)
        state->num_stages = NV2A_MAX_COMBINER_STAGES;
    state->tex_mode[0] = (NV2ATextureMode)((token >>  8) & 0xF);
    state->tex_mode[1] = (NV2ATextureMode)((token >> 12) & 0xF);
    state->tex_mode[2] = (NV2ATextureMode)((token >> 16) & 0xF);
    state->tex_mode[3] = (NV2ATextureMode)((token >> 20) & 0xF);
    state->flags = (token >> 24) & 0xFF;
}

void d3d8_combiners_from_render_states(const DWORD *rs,
                                       NV2ACombinerState *state)
{
    int i;

    /*
     * If called standalone (not from parse_token), read combiner count
     * from D3DRS_PSCOMBINERCOUNT render state.
     *
     * D3DRS_PSCOMBINERCOUNT layout:
     *   [3:0] number of stages
     *   [8]   unique C0 per stage (1) vs shared (0)
     *   [9]   unique C1 per stage (1) vs shared (0)
     *   [16]  mux_MSB for final combiner (not commonly used)
     */
    if (state->num_stages == 0) {
        state->num_stages = rs[D3DRS_PSCOMBINERCOUNT] & 0xF;
        if (state->num_stages < 1) state->num_stages = 1;
        if (state->num_stages > NV2A_MAX_COMBINER_STAGES)
            state->num_stages = NV2A_MAX_COMBINER_STAGES;
    }

    /* Parse stage inputs */
    for (i = 0; i < state->num_stages && i < NV2A_MAX_COMBINER_STAGES; i++) {
        parse_four_inputs(rs[D3DRS_PSRGBINPUTS0 + i],
                          state->stages[i].rgb_input);
        parse_four_inputs(rs[D3DRS_PSALPHAINPUTS0 + i],
                          state->stages[i].alpha_input);
    }

    /* Parse stage outputs */
    for (i = 0; i < state->num_stages && i < NV2A_MAX_COMBINER_STAGES; i++) {
        parse_output(rs[D3DRS_PSRGBOUTPUTS0 + i],
                     &state->stages[i].rgb_output);
        parse_output(rs[D3DRS_PSALPHAOUTPUTS0 + i],
                     &state->stages[i].alpha_output);
    }

    /*
     * Parse final combiner inputs.
     *
     * D3DRS_PSFINALCOMBINERINPUTSABCD packs inputs A,B,C,D as 8 bits each:
     *   [7:0]=A  [15:8]=B  [23:16]=C  [31:24]=D
     *
     * D3DRS_PSFINALCOMBINERINPUTSEFG packs E,F,G:
     *   [7:0]=E  [15:8]=F  [23:16]=G  [31:24]=reserved
     */
    {
        DWORD abcd = rs[D3DRS_PSFINALCOMBINERINPUTSABCD];
        DWORD efg  = rs[D3DRS_PSFINALCOMBINERINPUTSEFG];

        parse_combiner_input((abcd >>  0) & 0xFF, &state->final_input[0]); /* A */
        parse_combiner_input((abcd >>  8) & 0xFF, &state->final_input[1]); /* B */
        parse_combiner_input((abcd >> 16) & 0xFF, &state->final_input[2]); /* C */
        parse_combiner_input((abcd >> 24) & 0xFF, &state->final_input[3]); /* D */
        parse_combiner_input((efg  >>  0) & 0xFF, &state->final_input[4]); /* E */
        parse_combiner_input((efg  >>  8) & 0xFF, &state->final_input[5]); /* F */
        parse_combiner_input((efg  >> 16) & 0xFF, &state->final_input[6]); /* G */
    }

    /* Per-stage constant colors */
    for (i = 0; i < NV2A_MAX_COMBINER_STAGES; i++) {
        state->c0[i] = rs[D3DRS_PSCONSTANT0_0 + i];
        state->c1[i] = rs[D3DRS_PSCONSTANT1_0 + i];
    }

    /* Final combiner uses the constants from the last active stage, or
     * can use its own - for now, store them separately. Games typically
     * share them with the last stage. */
    state->final_c0 = state->c0[state->num_stages > 0 ? state->num_stages - 1 : 0];
    state->final_c1 = state->c1[state->num_stages > 0 ? state->num_stages - 1 : 0];

    /* Read texture modes from render state if not already set by token */
    if (state->tex_mode[0] == 0 && state->tex_mode[1] == 0 &&
        state->tex_mode[2] == 0 && state->tex_mode[3] == 0) {
        DWORD tm = rs[D3DRS_PSTEXTUREMODES];
        state->tex_mode[0] = (NV2ATextureMode)((tm >>  0) & 0xF);
        state->tex_mode[1] = (NV2ATextureMode)((tm >>  4) & 0xF);
        state->tex_mode[2] = (NV2ATextureMode)((tm >>  8) & 0xF);
        state->tex_mode[3] = (NV2ATextureMode)((tm >> 12) & 0xF);
    }
}

/* ================================================================
 * HLSL Code Generation
 *
 * Strategy: build the shader string via snprintf into a large buffer.
 * Each section appends to a running offset. This is not the prettiest
 * approach but it's straightforward, debuggable, and has zero
 * external dependencies.
 *
 * Generated shader structure:
 *   1. Texture sampler declarations
 *   2. Constant buffer (matches NV2APSConstants layout)
 *   3. Input struct (SV_POSITION, COLOR0, COLOR1, TEXCOORD0-3)
 *   4. Input mapping helper function
 *   5. Output mapping helper function
 *   6. main():
 *      a. Initialize register file from inputs
 *      b. Execute each general combiner stage
 *      c. Execute final combiner
 *      d. Apply fog
 *      e. Apply alpha test
 *      f. Return result
 * ================================================================ */

/**
 * Emit HLSL to append a string to the output buffer.
 * Returns new offset, or -1 if buffer overflow.
 */
#define EMIT(fmt, ...) do { \
    int _n = snprintf(buf + off, bufsize - off, fmt, ##__VA_ARGS__); \
    if (_n < 0 || off + _n >= bufsize) return -1; \
    off += _n; \
} while (0)

/**
 * Get the HLSL variable name for a register in the NV2A register file.
 *
 * The register file is represented as local float4 variables in the
 * generated shader. This returns the name used in the HLSL code.
 */
static const char *reg_name(NV2ACombinerRegister reg)
{
    switch (reg) {
    case NV2A_REG_ZERO:     return "r_zero";
    case NV2A_REG_C0:       return "r_c0";
    case NV2A_REG_C1:       return "r_c1";
    case NV2A_REG_FOG:      return "r_fog";
    case NV2A_REG_V0:       return "r_v0";
    case NV2A_REG_V1:       return "r_v1";
    case NV2A_REG_T0:       return "r_t0";
    case NV2A_REG_T1:       return "r_t1";
    case NV2A_REG_T2:       return "r_t2";
    case NV2A_REG_T3:       return "r_t3";
    case NV2A_REG_R0:       return "r_r0";
    case NV2A_REG_R1:       return "r_r1";
    case NV2A_REG_EF_PROD:  return "r_ef";
    case NV2A_REG_V1R0_SUM: return "r_v1r0sum";
    default:                return "r_zero";
    }
}

/**
 * Emit HLSL expression for reading a combiner input.
 *
 * An input consists of:
 *   1. Register selection (which variable to read)
 *   2. Channel selection (full RGBA or alpha-replicated)
 *   3. Mapping function (how to transform the value)
 *
 * For alpha-replicate: .aaaa swizzle
 * For normal RGB read in RGB path: .rgb (or .rgba for alpha path)
 *
 * The mapping function is applied inline as an arithmetic expression.
 *
 * @param suffix  ".rgb" for RGB path, ".a" for alpha path (determines swizzle)
 */
static void emit_mapped_input(char *buf, int bufsize, int *off,
                               const NV2ACombinerInput *input,
                               const char *suffix, int stage_idx)
{
    const char *rn;
    char swizzle[8];
    char base_expr[128];
    int n;

    rn = reg_name(input->reg);

    /* For per-stage C0/C1, use the stage-indexed constant */
    if (input->reg == NV2A_REG_C0) {
        snprintf(base_expr, sizeof(base_expr), stage_idx < 0 ? "fc0" : "c0[%d]", stage_idx);
    } else if (input->reg == NV2A_REG_C1) {
        snprintf(base_expr, sizeof(base_expr), stage_idx < 0 ? "fc1" : "c1[%d]", stage_idx);
    } else {
        snprintf(base_expr, sizeof(base_expr), "%s", rn);
    }

    /* Determine swizzle based on alpha replicate and target channel */
    if (input->alpha_rep) {
        /* Alpha replicate: use .aaaa for RGB, .a for alpha */
        if (strcmp(suffix, ".a") == 0)
            snprintf(swizzle, sizeof(swizzle), ".a");
        else
            snprintf(swizzle, sizeof(swizzle), ".aaa");
    } else {
        snprintf(swizzle, sizeof(swizzle), "%s", strcmp(suffix, ".a") == 0 ? ".b" : suffix);
    }

    /* Build the full variable reference */
    char var_ref[160];
    snprintf(var_ref, sizeof(var_ref), "%s%s", base_expr, swizzle);

    /* Apply input mapping */
    switch (input->mapping) {
    case NV2A_MAP_UNSIGNED_IDENTITY:
        /* x - passthrough */
        n = snprintf(buf + *off, bufsize - *off, "max(%s, 0.0)", var_ref);
        break;
    case NV2A_MAP_UNSIGNED_INVERT:
        /* 1 - x, clamped to [0,1] */
        n = snprintf(buf + *off, bufsize - *off,
                     "(1.0 - saturate(%s))", var_ref);
        break;
    case NV2A_MAP_EXPAND_NORMAL:
        /* 2x - 1 */
        n = snprintf(buf + *off, bufsize - *off,
                     "(2.0 * max(%s, 0.0) - 1.0)", var_ref);
        break;
    case NV2A_MAP_EXPAND_NEGATE:
        /* 1 - 2x */
        n = snprintf(buf + *off, bufsize - *off,
                     "(1.0 - 2.0 * max(%s, 0.0))", var_ref);
        break;
    case NV2A_MAP_HALFBIAS_NORMAL:
        /* x - 0.5 */
        n = snprintf(buf + *off, bufsize - *off,
                     "(max(%s, 0.0) - 0.5)", var_ref);
        break;
    case NV2A_MAP_HALFBIAS_NEGATE:
        /* 0.5 - x */
        n = snprintf(buf + *off, bufsize - *off,
                     "(0.5 - max(%s, 0.0))", var_ref);
        break;
    case NV2A_MAP_SIGNED_IDENTITY:
        /* x (allow negative values) */
        n = snprintf(buf + *off, bufsize - *off, "%s", var_ref);
        break;
    case NV2A_MAP_SIGNED_NEGATE:
        /* -x */
        n = snprintf(buf + *off, bufsize - *off, "(-%s)", var_ref);
        break;
    default:
        n = snprintf(buf + *off, bufsize - *off, "%s", var_ref);
        break;
    }
    if (n > 0) *off += n;
}

/**
 * Emit HLSL expression for the output mapping (scale/bias).
 */
static const char *output_map_prefix(NV2AOutputMapping map)
{
    switch (map) {
    case NV2A_OUT_IDENTITY:         return "";
    case NV2A_OUT_BIAS:             return "(";
    case NV2A_OUT_SHIFTLEFT_1:      return "(";
    case NV2A_OUT_SHIFTLEFT_1_BIAS: return "((";
    case NV2A_OUT_SHIFTLEFT_2:      return "(";
    case NV2A_OUT_SHIFTRIGHT_1:     return "(";
    default:                        return "";
    }
}

static const char *output_map_suffix(NV2AOutputMapping map)
{
    switch (map) {
    case NV2A_OUT_IDENTITY:         return "";
    case NV2A_OUT_BIAS:             return " - 0.5)";
    case NV2A_OUT_SHIFTLEFT_1:      return " * 2.0)";
    case NV2A_OUT_SHIFTLEFT_1_BIAS: return " - 0.5) * 2.0)";
    case NV2A_OUT_SHIFTLEFT_2:      return " * 4.0)";
    case NV2A_OUT_SHIFTRIGHT_1:     return " * 0.5)";
    default:                        return "";
    }
}

static int emit_texture_dot(char *buf,int bufsize,int off,unsigned identifier,unsigned coordinate_stage,unsigned source,unsigned mapping,const char *result)
{
    EMIT("float3 dot_color%d=r_t%d.rgb;\n",identifier,source);
    if(mapping==1)EMIT("dot_color%d=(dot_color%d*255.0-128.0)/127.0;\n",identifier,identifier);
    else if(mapping==2 || mapping==3) {
        EMIT("float3 dot_raw%d=dot_color%d*255.0;\n",identifier,identifier);
        if(mapping==2)EMIT("dot_color%d=float3(dot_raw%d.x>=128?dot_raw%d.x-255.5:dot_raw%d.x+0.5,dot_raw%d.y>=128?dot_raw%d.y-255.5:dot_raw%d.y+0.5,dot_raw%d.z>=128?dot_raw%d.z-255.5:dot_raw%d.z+0.5)/127.5;\n",identifier,identifier,identifier,identifier,identifier,identifier,identifier,identifier,identifier,identifier);
        else EMIT("dot_color%d=float3(dot_raw%d.x>=128?dot_raw%d.x-256:dot_raw%d.x,dot_raw%d.y>=128?dot_raw%d.y-256:dot_raw%d.y,dot_raw%d.z>=128?dot_raw%d.z-256:dot_raw%d.z)/127.0;\n",identifier,identifier,identifier,identifier,identifier,identifier,identifier,identifier,identifier,identifier);
    }
    else if(mapping==4) {
        EMIT("uint4 hilo_raw%u=uint4(r_t%u*255.0); dot_color%u=float3(float((hilo_raw%u.a<<8)|hilo_raw%u.r)/65535.0,float((hilo_raw%u.g<<8)|hilo_raw%u.b)/65535.0,1);\n",identifier,source,identifier,identifier,identifier,identifier,identifier);
    }
    EMIT("float %s=dot(input.tc%u.xyz,dot_color%u);\n",result,coordinate_stage,identifier);
    return off;
}

int d3d8_combiners_generate_hlsl(const NV2ACombinerState *state,
                                 char *buf, int bufsize)
{
    int off = 0, i, j;
    for (i = 0; i < 4; ++i) {
        if (state->tex_mode[i] >= NV2A_TEXMODE_NONE && state->tex_mode[i] != NV2A_TEXMODE_DEPENDENT_AR && state->tex_mode[i] != NV2A_TEXMODE_DEPENDENT_GB && state->tex_mode[i] != NV2A_TEXMODE_BUMP && state->tex_mode[i] != NV2A_TEXMODE_BUMP_LUM && state->tex_mode[i] != NV2A_TEXMODE_DOT_ST && state->tex_mode[i] != NV2A_TEXMODE_DOT_STR_CUBE && state->tex_mode[i] != NV2A_TEXMODE_DOT_RFLCT_SPEC && state->tex_mode[i] != NV2A_TEXMODE_DOT_RFLCT_DIFF && state->tex_mode[i] != NV2A_TEXMODE_DOT_RFLCT_SPEC_CONST && state->tex_mode[i] != NV2A_TEXMODE_DOT_STR) continue;
        EMIT("%s tex%d : register(t%d);\nSamplerState samp%d : register(s%d);\n",
             (state->volume_mask&(1u<<i)) ? "Texture3D" :
             ((state->tex_mode[i] == NV2A_TEXMODE_CUBEMAP || state->tex_mode[i] == NV2A_TEXMODE_DOT_STR_CUBE || state->tex_mode[i] == NV2A_TEXMODE_DOT_RFLCT_SPEC || state->tex_mode[i] == NV2A_TEXMODE_DOT_RFLCT_DIFF || state->tex_mode[i] == NV2A_TEXMODE_DOT_RFLCT_SPEC_CONST) && !(state->dot_2d_mask&(1u<<i))) ? "TextureCube" :
             state->tex_mode[i] == NV2A_TEXMODE_3D ? "Texture3D" : "Texture2D", i, i, i, i);
    }
    if(state->dot_2d_mask)EMIT("float2 dot_remap_2d(float3 d) { float3 a=abs(d); if(a.x>a.y && a.x>a.z)return float2(d.x>0?-d.z:d.z,d.y)/a.x; if(a.y>a.x && a.y>a.z)return float2(d.x,d.y>0?-d.z:d.z)/a.y; return float2(d.z>0?d.x:-d.x,d.y)/a.z; }\n");
    EMIT("cbuffer CombinerCB : register(b0) { float4 c0[8]; float4 c1[8]; float4 fc0; float4 fc1; float4 fog_color; float alpha_ref; uint alpha_func; uint alpha_test_enable; uint fog_enable; uint4 alpha_only; uint4 color_key, color_key_mask; float4 bump_matrix[4], bump_lum_coord[4]; };\n");
    if (state->native_coordinates) {
        EMIT("cbuffer NativeCB : register(b1) { uint4 clip_x[2]; uint4 clip_y[2]; float4 depth_params; uint4 native_flags; uint4 surface_bounds; float4 depth_offset; uint4 stipple_flags; uint4 stipple_pattern[8]; };\n");
        EMIT("struct PS_IN { float4 pos:SV_POSITION; noperspective float4 color0:COLOR0; noperspective float4 color1:COLOR1; float4 tc0:TEXCOORD0; float4 tc1:TEXCOORD1; float4 tc2:TEXCOORD2; float4 tc3:TEXCOORD3; noperspective float fog:TEXCOORD4; noperspective float2 z:TEXCOORD5; };\n");
        EMIT("struct PS_OUT { float4 color:SV_TARGET; float depth:SV_DEPTH; };\nPS_OUT main(PS_IN input) {\n");
        /* Derivatives precede per-pixel clipping. The slope is measured in
         * guest pixels, and offset is applied before the original depth clip. */
        /* HLSL SV_Position.w is W, unlike GLSL's reciprocal W. The Vulkan
         * compiler already requests DX position-W semantics for this shader. */
        EMIT("float nv_depth = native_flags.z != 0 ? input.pos.w : input.z.x; float slope_offset=0; if(depth_offset.x != 0) { float slope=depth_offset.z!=0 ? input.z.y : max(abs(ddx(input.z.x)),abs(ddy(input.z.x)))*float(native_flags.w); slope_offset=depth_offset.x*slope; } nv_depth+=depth_offset.y; nv_depth+=slope_offset;\n");
        EMIT("uint2 pixel = uint2(input.pos.xy / float(native_flags.w)); if(any(pixel < surface_bounds.xy) || any(pixel >= surface_bounds.zw)) discard; bool contained = false; [unroll] for(uint k=0;k<8;k++) { uint cx=clip_x[k/4][k%%4], cy=clip_y[k/4][k%%4]; contained = contained || (pixel.x >= (cx&4095) && pixel.x <= ((cx>>16)&4095) && pixel.y >= (cy&4095) && pixel.y <= ((cy>>16)&4095)); } if (contained == (native_flags.x != 0)) discard;\n");
        EMIT("if(stipple_flags.x != 0) { uint row=31-(pixel.y&31); if((stipple_pattern[row/4][row%%4] & (1u<<((pixel.x&31)^7)))==0) discard; }\n");
        EMIT("if(native_flags.y != 0 && (nv_depth < depth_params.x || nv_depth > depth_params.y)) discard; nv_depth = clamp(nv_depth, depth_params.x, depth_params.y);\n");
        EMIT("if(depth_params.w==16) { uint bits=asuint(nv_depth); nv_depth=nv_depth>0 && bits>0x3c000000 ? float(min((bits-0x3c000000)>>11,65535u)) : 0; } else if(depth_params.w==24) { nv_depth=nv_depth>0 ? float(min(asuint(nv_depth)>>7,0xfeffffu)) : 0; } else nv_depth=floor(max(0,nv_depth));\n");
    } else {
        EMIT("struct PS_IN { float4 pos:SV_POSITION; float4 color0:COLOR0; float4 color1:COLOR1; float3 tc0:TEXCOORD0; float3 tc1:TEXCOORD1; float3 tc2:TEXCOORD2; float3 tc3:TEXCOORD3; };\nfloat4 main(PS_IN input):SV_TARGET {\n");
    }
    if(state->native_coordinates && state->tex_mode[0]==NV2A_TEXMODE_2D) {
    }
    EMIT("float4 r_zero=0, r_c0=c0[0], r_c1=c1[0], r_fog=fog_color, r_v0=input.color0, r_v1=input.color1;\n");
    if (state->native_coordinates) EMIT("r_fog.a = input.fog;\n");
    for (i = 0; i < 4; ++i) {
        if(state->tex_mode[i] == NV2A_TEXMODE_DOTPRODUCT || state->tex_mode[i] == NV2A_TEXMODE_DOT_ST || state->tex_mode[i] == NV2A_TEXMODE_DOT_STR_CUBE || state->tex_mode[i] == NV2A_TEXMODE_DOT_RFLCT_SPEC || state->tex_mode[i] == NV2A_TEXMODE_DOT_RFLCT_DIFF || state->tex_mode[i] == NV2A_TEXMODE_DOT_RFLCT_SPEC_CONST || state->tex_mode[i] == NV2A_TEXMODE_DOT_STR) {
            if(i==3 && state->tex_mode[2]==NV2A_TEXMODE_DOT_RFLCT_DIFF)EMIT("float dot_stage3=dot_stage2_next;\n");
            else { char result[24];snprintf(result,sizeof(result),"dot_stage%d",i);off=emit_texture_dot(buf,bufsize,off,i,i,state->dependent_source[i],state->dot_mapping[i],result);if(off<0)return -1; }
            if(state->tex_mode[i]==NV2A_TEXMODE_DOT_RFLCT_DIFF) { off=emit_texture_dot(buf,bufsize,off,4,3,state->dependent_source[3],state->dot_mapping[3],"dot_stage2_next");if(off<0)return -1; }
        }
        if(state->tex_mode[i] == NV2A_TEXMODE_DOTPRODUCT) {
            EMIT("float4 r_t%d=0;\n",i);
        } else if(state->tex_mode[i] == NV2A_TEXMODE_DOT_RFLCT_DIFF) {
            EMIT("float4 r_t%d=tex%d.SampleLevel(samp%d,%sfloat3(dot_stage1,dot_stage2,dot_stage2_next)%s,0);\n",i,i,i,state->dot_2d_mask&(1u<<i)?"dot_remap_2d(":"",state->dot_2d_mask&(1u<<i)?")":"");
        } else if(state->tex_mode[i] == NV2A_TEXMODE_DOT_RFLCT_SPEC || state->tex_mode[i] == NV2A_TEXMODE_DOT_RFLCT_SPEC_CONST) {
            EMIT("float3 reflection_normal=float3(dot_stage1,dot_stage2,dot_stage3),reflection_eye=%s; float3 reflection_direction=2*reflection_normal*dot(reflection_normal,reflection_eye)/dot(reflection_normal,reflection_normal)-reflection_eye; float4 r_t%d=tex%d.SampleLevel(samp%d,%sreflection_direction%s,0);\n",state->tex_mode[i]==NV2A_TEXMODE_DOT_RFLCT_SPEC_CONST?"bump_matrix[3].xyz":"float3(input.tc1.w,input.tc2.w,input.tc3.w)",i,i,i,state->dot_2d_mask&(1u<<i)?"dot_remap_2d(":"",state->dot_2d_mask&(1u<<i)?")":"");
        } else if(state->tex_mode[i] == NV2A_TEXMODE_DOT_STR_CUBE) {
            EMIT("float4 r_t%d=tex%d.SampleLevel(samp%d,%sfloat3(dot_stage1,dot_stage2,dot_stage3)%s,0);\n",i,i,i,state->dot_2d_mask&(1u<<i)?"dot_remap_2d(":"",state->dot_2d_mask&(1u<<i)?")":"");
        } else if(state->tex_mode[i] == NV2A_TEXMODE_DOT_STR) {
            if(state->volume_mask&(1u<<i))EMIT("float4 r_t%d=tex%d.SampleLevel(samp%d,float3(dot_stage1,dot_stage2,dot_stage3),0);\n",i,i,i);
            else EMIT("float4 r_t%d=tex%d.SampleLevel(samp%d,float2(dot_stage1,dot_stage2)*bump_lum_coord[%d].zw,0);\n",i,i,i,i);
        } else if(state->tex_mode[i] == NV2A_TEXMODE_DOT_ST) {
            EMIT("float4 r_t%d=tex%d.SampleLevel(samp%d,float2(dot_stage%d,dot_stage%d)*bump_lum_coord[%d].zw,0);\n",i,i,i,i-1,i,i);
        } else if (state->tex_mode[i] == NV2A_TEXMODE_PASSTHRU) {
            EMIT("float4 r_t%d=saturate(input.tc%d);\n",i,i);
        } else if (state->tex_mode[i] == NV2A_TEXMODE_CLIPPLANE) {
            for (j=0;j<4;j++) EMIT("if(input.tc%d[%d] %s 0) discard;\n",i,j,(state->clip_plane_mode>>(i*4+j))&1 ? ">=" : "<");
            EMIT("float4 r_t%d=0;\n",i);
        } else if (state->tex_mode[i] == NV2A_TEXMODE_BUMP || state->tex_mode[i] == NV2A_TEXMODE_BUMP_LUM) {
            EMIT("float2 bump_input%d=r_t%d.bg*255.0; bump_input%d=float2(bump_input%d.x>=128?bump_input%d.x-256:bump_input%d.x,bump_input%d.y>=128?bump_input%d.y-256:bump_input%d.y)/127.0;\n",i,state->dependent_source[i],i,i,i,i,i,i,i);
            EMIT("float2 bump_delta%d=float2(dot(bump_matrix[%d].xz,bump_input%d),dot(bump_matrix[%d].yw,bump_input%d)); float4 r_t%d=tex%d.SampleLevel(samp%d,(input.tc%d.xy+bump_delta%d)*bump_lum_coord[%d].zw,0);\n",i,i,i,i,i,i,i,i,i,i,i);
        } else if (state->tex_mode[i] == NV2A_TEXMODE_DEPENDENT_AR || state->tex_mode[i] == NV2A_TEXMODE_DEPENDENT_GB) {
            EMIT("float4 r_t%d=tex%d.SampleLevel(samp%d,r_t%d.%s,0);\n",i,i,i,state->dependent_source[i],state->tex_mode[i]==NV2A_TEXMODE_DEPENDENT_AR?"ar":"gb");
        } else if (state->tex_mode[i] == NV2A_TEXMODE_NONE) {
            EMIT("float4 r_t%d=float4(0,0,0,1);\n", i);
        } else if (state->tex_mode[i] == NV2A_TEXMODE_3D && (state->projected_mask & (1u << i))) {
            EMIT("float4 r_t%d=tex%d.SampleLevel(samp%d,input.tc%d.xyz/input.tc%d.w,0);\n",i,i,i,i,i);
        } else if (state->tex_mode[i] == NV2A_TEXMODE_CUBEMAP || state->tex_mode[i] == NV2A_TEXMODE_3D) {
            EMIT("float4 r_t%d=tex%d.SampleLevel(samp%d,input.tc%d.xyz,0);\n", i,i,i,i);
        } else if (state->projected_mask & (1u << i)) {
            EMIT("float4 r_t%d=tex%d.SampleLevel(samp%d,input.tc%d.xy/(input.tc%d.w!=0?input.tc%d.w:1),0);\n",i,i,i,i,i,i);
        } else {
            EMIT("float4 r_t%d=tex%d.SampleLevel(samp%d,input.tc%d.xy,0);\n",i,i,i,i);
        }
        if(state->luma_mask&(1u<<i))EMIT("r_t%d=float4(r_t%d.rrr,1);\n",i,i);
        if(state->tex_mode[i] == NV2A_TEXMODE_BUMP_LUM)EMIT("r_t%d*=bump_lum_coord[%d].x*r_t%d.r+bump_lum_coord[%d].y;\n",i,i,state->dependent_source[i],i);
        if(state->shadow_mask&(1u<<i)) {
            if(state->shadow_function==0)EMIT("r_t%d=0;\n",i);
            else if(state->shadow_function==7)EMIT("r_t%d=1;\n",i);
            else {
                static const char *comparison[]={"", "<", "==", "<=", ">", "!=", ">="};
                if(state->shadow_function>7)return -1;
                if(state->shadow_project_mask&(1u<<i))EMIT("float shadow_ref%d=clamp(input.tc%d.z/(input.tc%d.w!=0?input.tc%d.w:1),0,%u.0);\n",i,i,i,i,state->shadow_bits[i]==16?65535u:16777215u);
                else EMIT("float shadow_ref%d=0;\n",i);
                EMIT("r_t%d=(r_t%d.r %s shadow_ref%d)?float4(1,1,1,1):float4(0,0,0,0);\n",i,i,comparison[state->shadow_function],i);
            }
        }
        if (state->alpha_kill_mask & (1u << i)) EMIT("if(r_t%d.a == 0) discard;\n",i);
        if (state->tex_mode[i] != NV2A_TEXMODE_NONE) EMIT("if(alpha_only[%d]) r_t%d.rgb=1;\n",i,i);
        if(state->color_key_mode[i]) {
            EMIT("uint4 key_bytes%d=uint4(saturate(r_t%d)*255+0.5);uint key_pixel%d=(key_bytes%d.a<<24)|(key_bytes%d.r<<16)|(key_bytes%d.g<<8)|key_bytes%d.b; if((key_pixel%d&color_key_mask[%d])==(color_key[%d]&color_key_mask[%d])) {\n",i,i,i,i,i,i,i,i,i,i,i);
            if(state->color_key_mode[i]==3) EMIT("discard;\n");
            else if(state->color_key_mode[i]==2) EMIT("r_t%d=0;\n",i);
            else EMIT("r_t%d.a=0;\n",i);
            EMIT("}\n");
        }
    }
    EMIT("float4 r_r0=float4(0,0,0,r_t0.a), r_r1=0, r_ef=0, r_v1r0sum=0;\n");
    for (i = 0; i < state->num_stages; ++i) {
        const NV2ACombinerOutput *rgb = &state->stages[i].rgb_output;
        const NV2ACombinerOutput *alpha = &state->stages[i].alpha_output;
        EMIT("{\n");
        for (j = 0; j < 4; ++j) {
            EMIT("float3 v%d=",j);
            emit_mapped_input(buf,bufsize,&off,&state->stages[i].rgb_input[j],".rgb",i); EMIT(";\n");
            EMIT("float a%d=",j);
            emit_mapped_input(buf,bufsize,&off,&state->stages[i].alpha_input[j],".a",i); EMIT(";\n");
        }
        EMIT("float3 ab_rgb=%s, cd_rgb=%s; float ab_a=a0*a1, cd_a=a2*a3;\n",
             rgb->ab_dot ? "dot(v0,v1).xxx" : "v0*v1",
             rgb->cd_dot ? "dot(v2,v3).xxx" : "v2*v3");
        EMIT("bool mux_cd=%s;\n", state->flags & 1 ? "r_r0.a>=0.5" : "((int(r_r0.a*255)&1)!=0)");
        EMIT("float3 sum_rgb=%s; float sum_a=%s;\n",
             rgb->mux_sum ? "(mux_cd?cd_rgb:ab_rgb)" : "ab_rgb+cd_rgb",
             alpha->mux_sum ? "(mux_cd?cd_a:ab_a)" : "ab_a+cd_a");
        const char *names[] = {"ab", "cd", "sum"};
        const NV2ACombinerRegister dst_rgb[] = {rgb->ab_dst,rgb->cd_dst,rgb->sum_dst};
        const NV2ACombinerRegister dst_a[] = {alpha->ab_dst,alpha->cd_dst,alpha->sum_dst};
        for(j=0;j<3;j++) {
            EMIT("%s_rgb=clamp(%s%s_rgb%s,-1.0,1.0);\n",names[j],output_map_prefix(rgb->output_map),names[j],output_map_suffix(rgb->output_map));
            EMIT("%s_a=clamp(%s%s_a%s,-1.0,1.0);\n",names[j],output_map_prefix(alpha->output_map),names[j],output_map_suffix(alpha->output_map));
        }
        for(j=0;j<3;j++) if(dst_rgb[j]) EMIT("%s.rgb=%s_rgb;\n",reg_name(dst_rgb[j]),names[j]);
        if(rgb->ab_dst && rgb->ab_blue_alpha) EMIT("%s.a=ab_rgb.b;\n",reg_name(rgb->ab_dst));
        if(rgb->cd_dst && rgb->cd_blue_alpha) EMIT("%s.a=cd_rgb.b;\n",reg_name(rgb->cd_dst));
        for(j=0;j<3;j++) if(dst_a[j]) EMIT("%s.a=%s_a;\n",reg_name(dst_a[j]),names[j]);
        EMIT("}\n");
    }
    EMIT("float4 result;\n");
    if (!state->native_coordinates || state->final_enabled) {
        EMIT("r_v1r0sum.rgb=%s + %s; r_v1r0sum.a=0;\n",
             state->final_flags & 0x40 ? "(1-r_v1.rgb)" : "r_v1.rgb",
             state->final_flags & 0x20 ? "(1-r_r0.rgb)" : "r_r0.rgb");
        if(state->final_flags & 0x80) EMIT("r_v1r0sum.rgb=saturate(r_v1r0sum.rgb);\n");
        EMIT("float3 e_val="); emit_mapped_input(buf,bufsize,&off,&state->final_input[4],".rgb",-1); EMIT(";\nfloat3 f_val="); emit_mapped_input(buf,bufsize,&off,&state->final_input[5],".rgb",-1); EMIT(";\nr_ef.rgb=e_val*f_val;\n");
        for(j=0;j<4;j++) { EMIT("float3 fc_%d=",j); emit_mapped_input(buf,bufsize,&off,&state->final_input[j],".rgb",-1); EMIT(";\n"); }
        EMIT("result.rgb=fc_3 + fc_2*(1-fc_0)+fc_1*fc_0; result.a="); emit_mapped_input(buf,bufsize,&off,&state->final_input[6],".a",-1); EMIT(";\n");
    } else EMIT("result=r_r0;\n");
    EMIT("result=saturate(result);\n");
    EMIT("if(alpha_test_enable) { float a=floor(result.a*255+0.5), r=floor(alpha_ref*255+0.5); bool alpha_pass=alpha_func==8 || (alpha_func==2 && a<r) || (alpha_func==3 && a==r) || (alpha_func==4 && a<=r) || (alpha_func==5 && a>r) || (alpha_func==6 && a!=r) || (alpha_func==7 && a>=r); if(!alpha_pass) discard; }\n");
    if(state->native_coordinates) EMIT("PS_OUT output; output.color=result; output.depth=nv_depth*depth_params.z; return output;\n}\n");
    else EMIT("return result;\n}\n");
    return off;
}

#undef EMIT

/* ================================================================
 * Shader Compilation & Cache
 * ================================================================ */

static int compile_combiner_shader(const NV2ACombinerState *state,CombinerCacheEntry *output)
{
    /* 16KB should be more than enough for any combiner shader */
    char hlsl[16384];
    ID3DBlob *code = NULL;
    ID3DBlob *errors = NULL;
    ID3D11PixelShader *ps = NULL;
    HRESULT hr;
    int len;

    len = d3d8_combiners_generate_hlsl(state, hlsl, sizeof(hlsl));
    if (len < 0) {
        fprintf(stderr, "NV2A combiners: HLSL generation failed (buffer overflow)\n");
        return 0;
    }

    if(d3d8_IsNativeVulkan()) {
        output->vulkan=nvk_shader_create(hlsl,"main",VK_SHADER_STAGE_FRAGMENT_BIT);
        return output->vulkan!=NULL;
    }
    hr = D3DCompile(hlsl, (SIZE_T)len, "ps_combiner",
                    NULL, NULL, "main", "ps_5_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                    &code, &errors);
    if (FAILED(hr)) {
        fprintf(stderr, "NV2A combiners: HLSL compile failed: %s\n",
                errors ? (char *)ID3D10Blob_GetBufferPointer(errors)
                       : "unknown error");
        /* Dump the generated source for debugging */
        fprintf(stderr, "--- Generated HLSL ---\n%s\n--- End HLSL ---\n", hlsl);
        if (errors) ID3D10Blob_Release(errors);
        return 0;
    }
    if (errors) ID3D10Blob_Release(errors);

    hr = ID3D11Device_CreatePixelShader(
        d3d8_GetD3D11Device(),
        ID3D10Blob_GetBufferPointer(code),
        ID3D10Blob_GetBufferSize(code),
        NULL, &ps);
    ID3D10Blob_Release(code);

    if (FAILED(hr)) {
        fprintf(stderr, "NV2A combiners: CreatePixelShader failed: 0x%08lX\n", hr);
        return 0;
    }

    output->shader=ps;return 1;
}

static CombinerCacheEntry *get_combiner_shader(const NV2ACombinerState *state)
{
    NV2ACombinerState key = *state;
    memset(key.c0, 0, sizeof key.c0);
    memset(key.c1, 0, sizeof key.c1);
    key.final_c0 = key.final_c1 = 0;
    for(int i=0;i<NV2A_MAX_COMBINER_STAGES;i++)
        if(i>=key.num_stages)memset(&key.stages[i],0,sizeof key.stages[i]);
    if(key.native_coordinates && !key.final_enabled) {
        memset(key.final_input,0,sizeof key.final_input);
        key.final_flags=0;
    }
    state = &key;
    /* Adjacent unchanged keys reuse the existing cache entry directly.
     * Compare the original full key, including all nonconstant fields. */
    if(g_last_lookup_slot>=0){
        CombinerCacheEntry *entry=&g_cache[g_last_lookup_slot];
        if(entry->in_use && combiner_state_equal(&entry->state,state)){
            entry->last_used_frame=g_frame_counter;
            return entry;
        }
    }
    uint32_t hash = combiner_state_hash(state);
    uint32_t idx = hash & (COMBINER_CACHE_SIZE - 1);
    int probe;

    /* Linear probe lookup */
    for (probe = 0; probe < COMBINER_CACHE_SIZE; probe++) {
        uint32_t slot = (idx + probe) & (COMBINER_CACHE_SIZE - 1);
        CombinerCacheEntry *entry = &g_cache[slot];

        if (!entry->in_use) {
            /* Cache miss - compile and insert */
            CombinerCacheEntry compiled={0};
            int compiled_ok=compile_combiner_shader(state,&compiled);
            if (!compiled_ok) return NULL;

            entry->in_use = TRUE;
            entry->hash = hash;
            memcpy(&entry->state, state, sizeof(NV2ACombinerState));
            entry->shader=compiled.shader;entry->vulkan=compiled.vulkan;
            entry->last_used_frame = g_frame_counter;
            g_last_lookup_slot=(int)slot;
            return entry;
        }

        if (entry->hash == hash && combiner_state_equal(&entry->state, state)) {
            /* Cache hit */
            entry->last_used_frame = g_frame_counter;
            g_last_lookup_slot=(int)slot;
            return entry;
        }
    }

    /*
     * Table is full - evict the least recently used entry.
     * This is rare in practice (most games use <20 configs).
     */
    {
        uint32_t lru_slot = idx;
        uint32_t lru_frame = UINT32_MAX;
        CombinerCacheEntry compiled={0};
        CombinerCacheEntry *entry;

        for (probe = 0; probe < COMBINER_CACHE_SIZE; probe++) {
            if (g_cache[probe].last_used_frame < lru_frame) {
                lru_frame = g_cache[probe].last_used_frame;
                lru_slot = probe;
            }
        }

        entry = &g_cache[lru_slot];
        if(entry->vulkan)nvk_shader_destroy(entry->vulkan);
        if (entry->shader) {
            ID3D11PixelShader_Release(entry->shader);
        }

        int compiled_ok=compile_combiner_shader(state,&compiled);
        if (!compiled_ok) return NULL;

        entry->hash = hash;
        memcpy(&entry->state, state, sizeof(NV2ACombinerState));
        entry->shader=compiled.shader;entry->vulkan=compiled.vulkan;
        entry->last_used_frame = g_frame_counter;
        g_last_lookup_slot=(int)lru_slot;
        return entry;
    }
}


ID3D11PixelShader *d3d8_combiners_get_shader(const NV2ACombinerState *state)
{ CombinerCacheEntry *entry=get_combiner_shader(state);return entry?entry->shader:NULL; }
NvkShader *d3d8_combiners_vulkan_shader(const NV2ACombinerState *state)
{ CombinerCacheEntry *entry=get_combiner_shader(state);return entry?entry->vulkan:NULL; }

/* ================================================================
 * Initialization / Shutdown
 * ================================================================ */

HRESULT d3d8_combiners_init(void)
{
    D3D11_BUFFER_DESC cbd;
    HRESULT hr;

    memset(g_cache, 0, sizeof(g_cache));
    g_last_lookup_slot=-1;
    memset(&g_combiner_state, 0, sizeof(g_combiner_state));
    g_ps_token = 0;
    g_dirty = TRUE;
    g_frame_counter = 0;
    g_combiner_value_valid = FALSE;

    /* Create the PS constant buffer for combiner shaders.
     * Size must match NV2APSConstants, rounded up to 16-byte alignment. */
    memset(&cbd, 0, sizeof(cbd));
    cbd.ByteWidth = (sizeof(NV2APSConstants) + 15) & ~15;
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = ID3D11Device_CreateBuffer(d3d8_GetD3D11Device(), &cbd, NULL,
                                   &g_combiner_cb);
    if (FAILED(hr)) {
        fprintf(stderr, "NV2A combiners: Failed to create constant buffer: "
                "0x%08lX\n", hr);
        return hr;
    }

    fprintf(stderr, "NV2A combiners: Initialized (cache size=%d)\n",
            COMBINER_CACHE_SIZE);
    return S_OK;
}

void d3d8_combiners_shutdown(void)
{
    int i;
    g_combiner_value_valid = FALSE;

    /* Release all cached shaders */
    for (i = 0; i < COMBINER_CACHE_SIZE; i++) {
        if (g_cache[i].in_use && g_cache[i].shader) {
            ID3D11PixelShader_Release(g_cache[i].shader);
        }
    }
    for(unsigned i=0;i<COMBINER_CACHE_SIZE;i++)if(g_cache[i].vulkan)nvk_shader_destroy(g_cache[i].vulkan);
    memset(g_cache, 0, sizeof(g_cache));
    g_last_lookup_slot=-1;

    if (g_combiner_cb) {
        ID3D11Buffer_Release(g_combiner_cb);
        g_combiner_cb = NULL;
    }

    fprintf(stderr, "NV2A combiners: Shut down\n");
}

/* ================================================================
 * Draw Integration
 * ================================================================ */

void d3d8_combiners_set_pixel_shader(DWORD token)
{
    if (token != g_ps_token) {
        g_ps_token = token;
        g_dirty = TRUE;
    }
}

BOOL d3d8_combiners_active(void)
{
    return g_ps_token != 0;
}

void d3d8_combiners_mark_dirty(void)
{
    g_dirty = TRUE;
}

static HRESULT combiner_upload_constants(ID3D11DeviceContext *ctx, const NV2APSConstants *constants)
{
    if (g_combiner_value_valid && !memcmp(&g_combiner_value, constants, sizeof(*constants)))
        return S_OK;
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g_combiner_cb, 0,
                                       D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return hr;
    memcpy(mapped.pData, constants, sizeof(*constants));
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_combiner_cb, 0);
    memcpy(&g_combiner_value, constants, sizeof(*constants));
    g_combiner_value_valid = TRUE;
    return S_OK;
}

BOOL d3d8_combiners_prepare_draw(void)
{
    ID3D11DeviceContext *ctx;
    ID3D11PixelShader *ps;
    NV2APSConstants constants={0};
    const DWORD *rs;
    int i;

    /* Not using combiner shaders - fall back to fixed-function */
    if (g_ps_token == 0)
        return FALSE;

    ctx = d3d8_GetD3D11Context();
    if (!ctx || !g_combiner_cb)
        return FALSE;

    rs = d3d8_GetRenderStates();

    /* Rebuild combiner state from token + render states if dirty */
    if (g_dirty) {
        d3d8_combiners_parse_token(g_ps_token, rs, &g_combiner_state);
        g_dirty = FALSE;
    }

    /* Get or compile the pixel shader for this combiner state */
    ps = d3d8_combiners_get_shader(&g_combiner_state);
    if (!ps) {
        fprintf(stderr, "NV2A combiners: Failed to get shader, "
                "falling back to FFP\n");
        return FALSE;
    }

    /* Bind the combiner pixel shader */
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);

    /* Construct every original parameter independently of shader dirtiness. */
    {
        NV2APSConstants *cb = &constants;

        /* Per-stage constants */
        for (i = 0; i < NV2A_MAX_COMBINER_STAGES; i++) {
            d3dcolor_to_float4(g_combiner_state.c0[i], cb->c0[i]);
            d3dcolor_to_float4(g_combiner_state.c1[i], cb->c1[i]);
        }

        /* Final combiner constants */
        d3dcolor_to_float4(g_combiner_state.final_c0, cb->final_c0);
        d3dcolor_to_float4(g_combiner_state.final_c1, cb->final_c1);

        /* Fog color from render state */
        d3dcolor_to_float4(rs[D3DRS_FOGCOLOR], cb->fog_color);

        /* Alpha test parameters */
        cb->alpha_ref = rs[D3DRS_ALPHAREF] / 255.0f;
        cb->alpha_func = rs[D3DRS_ALPHAFUNC];
        cb->alpha_test_enable = rs[D3DRS_ALPHATESTENABLE] ? 1 : 0;
        cb->fog_enable = rs[D3DRS_FOGENABLE] ? 1 : 0;
        for (i = 0; i < NV2A_MAX_TEXTURES; i++) {
            D3DFORMAT format = d3d8_base_format(d3d8_GetStageTexture(i));
            cb->alpha_only[i] = format == D3DFMT_A8 || format == D3DFMT_LIN_A8;
            cb->color_key[i] = cb->color_key_mask[i] = 0;
        }

    }
    (void)combiner_upload_constants(ctx, &constants);

    /* Bind the constant buffer to PS slot 0 */
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &g_combiner_cb);

    /* Advance frame counter for LRU tracking */
    g_frame_counter++;

    return TRUE;
}

HRESULT d3d8_combiners_bind(const NV2ACombinerState *state, const NV2APSConstants *constants)
{
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
    ID3D11PixelShader *ps = d3d8_combiners_get_shader(state);
    HRESULT hr;
    if (!ps || !ctx || !g_combiner_cb) return E_FAIL;
    hr = combiner_upload_constants(ctx, constants);
    if (FAILED(hr)) return hr;
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &g_combiner_cb);
    ++g_frame_counter;
    return S_OK;
}
