/*
 * MCPX APU MMIO Hook - VEH instruction decoder for APU register access
 *
 * Reuses the same x86-64 instruction decoder pattern as nv2a_mmio_hook.c
 * but routes reads/writes through the MCPX APU register handlers.
 */

#include "apu.h"
#include "apu_debug.h"
#include "apu_regs.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

/* Global APU state pointer -- referenced from main.c regardless of which
 * platform's MMIO hook is active, so define it before the #if guard. */
MCPXAPUState *g_apu_state = NULL;

/* The MMIO hook is a Win32-VEH x86-64 instruction decoder. On Linux the
 * equivalent goes through sigaction + ucontext_t (Stage 2 / main.c). For
 * now the whole body is Windows-only so apu_emu links on Debian. */
#if defined(_WIN32)
#include "../nv2a/nv2a_mmio_hook.h"
#define APU_MMIO_BASE 0xFE800000u

uint64_t apu_read_adapter(void *device, uint32_t offset, unsigned size)
{
    uint64_t value = mcpx_apu_mmio_read(device, offset, size);
    if (g_dbg.trace_enabled) {
        static unsigned count;
        if (count++ < 400 || offset == NV_PAPU_FECTL ||
            offset == NV_PAPU_FEDECMETH || offset == NV_PAPU_FEDECPARAM)
            fprintf(stderr, "  [APUMMIO] read  0x%05X = %08X\n", offset, (uint32_t)value);
    }
    return value;
}

void apu_write_adapter(void *device, uint32_t offset, uint64_t value, unsigned size)
{
    if (g_dbg.ssl_trace_enabled &&
        (offset == NV_PAPU_VPSGEADDR || offset == NV_PAPU_GPSADDR ||
         offset == NV_PAPU_EPSADDR))
        fprintf(stderr, "  [APUMMIO] SGE base register=%05X value=%08X\n",
                offset, (uint32_t)value);
    if (g_dbg.trace_enabled) {
        static unsigned count;
        if (count++ < 400 || offset == NV_PAPU_FECTL ||
            offset == NV_PAPU_TVL2D || offset == NV_PAPU_TVL3D ||
            offset == NV_PAPU_TVLMP || offset == NV_PAPU_VPVADDR ||
            (offset >= 0x20100 && offset <= 0x20140))
            fprintf(stderr, "  [APUMMIO] write 0x%05X = %08X\n", offset, (uint32_t)value);
    }
    mcpx_apu_mmio_write(device, offset, value, size);
}

bool apu_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                          uint32_t fault_xbox_va, int is_write)
{
    uint32_t offset = fault_xbox_va - APU_MMIO_BASE;
    uint32_t method = offset - 0x20000u;
    if (is_write && g_dbg.ssl_trace_enabled &&
        offset == NV_PAPU_EPSADDR)
        fprintf(stderr, "  [APUMMIO] EP_SGE_BASE tid=%lu pc=%llX rva=%llX\n",
                (unsigned long)GetCurrentThreadId(), (unsigned long long)ctx->Rip,
                (unsigned long long)(ctx->Rip - (uintptr_t)GetModuleHandleW(NULL)));
    if (is_write && g_dbg.ssl_trace_enabled &&
        offset >= 0x50000u + NV_PAPU_EPPMEM + 0x5EBu * 4 &&
        offset <= 0x50000u + NV_PAPU_EPPMEM + 0x5F1u * 4)
        fprintf(stderr, "  [APUMMIO] EP_P_WRITE tid=%lu pc=%llX rva=%llX va=%08X\n",
                (unsigned long)GetCurrentThreadId(), (unsigned long long)ctx->Rip,
                (unsigned long long)(ctx->Rip - (uintptr_t)GetModuleHandleW(NULL)), fault_xbox_va);
    if (is_write && g_dbg.ssl_trace_enabled &&
        (method == NV1BA0_PIO_VOICE_PAUSE ||
         method == NV1BA0_PIO_SET_CURRENT_SSL ||
         method == NV1BA0_PIO_SET_VOICE_SSL_A || method == NV1BA0_PIO_SET_VOICE_SSL_B ||
         (method >= NV1BA0_PIO_SET_SSL_SEGMENT_OFFSET && method < NV1BA0_PIO_SET_SSL_SEGMENT_OFFSET + 512)))
        fprintf(stderr, "  [APUMMIO] SSL tid=%lu pc=%llX rva=%llX va=%08X\n",
                (unsigned long)GetCurrentThreadId(), (unsigned long long)ctx->Rip,
                (unsigned long long)(ctx->Rip - (uintptr_t)GetModuleHandleW(NULL)), fault_xbox_va);
    return xbox_mmio_decode(ctx, fault_xbox_va - APU_MMIO_BASE, is_write,
                            g_apu_state, apu_read_adapter, apu_write_adapter);
}

bool ac97_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                           uint32_t fault_xbox_va, int is_write)
{
    return xbox_mmio_decode(ctx, fault_xbox_va - 0xFEC00000u, is_write,
                            g_apu_state, mcpx_ac97_read, mcpx_ac97_write);
}
#endif
