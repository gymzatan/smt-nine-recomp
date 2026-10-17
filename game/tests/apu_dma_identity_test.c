#include "kernel/kernel.h"
#include "kernel/xbox_memory_layout.h"
#include "apu/apu_state.h"
#include "apu/dsp/dsp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t address) { (void)address; abort(); }
recomp_func_t recomp_lookup_manual(uint32_t address) { (void)address; abort(); }

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    FILE *file = fopen(argv[1], "rb");
    if (!file) return 2;
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    rewind(file);
    unsigned char *image = malloc(length);
    if (!image || fread(image, 1, length, file) != (size_t)length) return 2;
    fclose(file);
    if (!xbox_MemoryLayoutInit(image, length)) return 2;
    MCPXAPUState *d = calloc(1, sizeof(*d));
    qemu_mutex_init(&d->lock);
    d->ram_ptr = (uint8_t *)((uintptr_t)xbox_GetMemoryOffset() + XBOX_CONTIG_BASE);
    g_apu_ram_ptr = d->ram_ptr;
    d->physical_resolve = xbox_ResolvePhysicalBuffer;
    mcpx_apu_dsp_init(d);
    uint32_t table = xbox_ContiguousAlloc(4096, 4096);
    uint32_t cpu = xbox_HeapAlloc(12288, 4096);
    uint8_t *bytes = (uint8_t *)((uintptr_t)xbox_GetMemoryOffset() + cpu);
    uint32_t physical[3];
    physical[0] = (uint32_t)xbox_MmGetPhysicalAddress((void *)(uintptr_t)cpu);
    uint32_t unrelated = xbox_ContiguousAlloc(4096, 4096);
    uint8_t *unrelated_bytes = (uint8_t *)((uintptr_t)xbox_GetMemoryOffset() + unrelated);
    memset(unrelated_bytes, 0x3c, 4096);
    for (unsigned i = 1; i < 3; ++i)
        physical[i] = (uint32_t)xbox_MmGetPhysicalAddress((void *)(uintptr_t)(cpu + i * 4096));
    uint32_t *sge = (uint32_t *)((uintptr_t)xbox_GetMemoryOffset() + table);
    sge[0] = physical[0] + 0xa0; sge[1] = 0;
    sge[2] = physical[1] + 0xa0; sge[3] = 0;
    *(uint32_t *)(bytes + 0xa0) = 0x123456;
    *(uint32_t *)(bytes + 0x10a0) = 0x654321;
    mcpx_apu_mmio_write(d, NV_PAPU_GPSADDR, table - XBOX_CONTIG_BASE, 4);
    mcpx_apu_mmio_write(d, NV_PAPU_GPSMAXSGE, 1, 4);
    mcpx_apu_mmio_write(d, 0x30000 + NV_PAPU_GPRST, 3, 4);
    uint32_t first = dsp_read_memory(d->gp.dsp, 'P', 0);
    uint32_t second = dsp_read_memory(d->gp.dsp, 'P', 1024);
    printf("Original CPU identity -> GP bootstrap: %06X %06X\n", first, second);
    if (first != 0x123456 || second != 0x654321) return 1;
    unsigned char payload[512];
    memset(payload, 0x5a, sizeof(payload));
    d->gp.dsp->dma.scratch_rw(d->gp.dsp->dma.rw_opaque, payload, 0xf00, sizeof(payload), true);
    int cpu_matches = memcmp(bytes + 0xfa0, payload, sizeof(payload)) == 0;
    int unrelated_unchanged = 1;
    for (unsigned i = 0; i < 4096; ++i)
        if (unrelated_bytes[i] != 0x3c) unrelated_unchanged = 0;
    printf("Interleaved physical identities: CPU0=%08X CPU1=%08X other=%08X; CPU payload=%d unrelated preserved=%d first=%02X\n",
           physical[0], physical[1], unrelated - XBOX_CONTIG_BASE,
           cpu_matches, unrelated_unchanged, unrelated_bytes[0]);
    if (!cpu_matches || !unrelated_unchanged) return 1;
    puts("PASS original CPU alias bootstrap and cross-page DSP writeback");
    return 0;
}
