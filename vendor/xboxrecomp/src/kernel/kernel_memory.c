/*
 * kernel_memory.c - Xbox Memory Management
 *
 * Implements Mm* and NtAllocateVirtualMemory/NtFreeVirtualMemory/NtQueryVirtualMemory
 * using Win32 VirtualAlloc/VirtualFree/VirtualQuery.
 *
 * Xbox contiguous memory (MmAllocateContiguousMemory) is used for GPU-accessible
 * buffers. On Windows, actual GPU resources are handled by our D3D11 layer;
 * these allocations just need to return valid CPU-accessible memory.
 */

#include "kernel.h"
#include "xbox_memory_layout.h"
#include <stdio.h>
#include <stdlib.h>
#if defined(_WIN32)
/* _aligned_malloc/_aligned_free; POSIX gets them from win32_compat.h */
#include <malloc.h>
#endif

/* ============================================================================
 * Helper: Xbox protect flags → Win32 protect flags
 * ============================================================================ */

static DWORD xbox_protect_to_win32(ULONG xbox_protect)
{
    /* Xbox uses the same PAGE_* constants as Windows NT */
    switch (xbox_protect & 0xFF) {
        case 0x01: return PAGE_NOACCESS;
        case 0x02: return PAGE_READONLY;
        case 0x04: return PAGE_READWRITE;
        case 0x08: return PAGE_WRITECOPY;
        case 0x10: return PAGE_EXECUTE;
        case 0x20: return PAGE_EXECUTE_READ;
        case 0x40: return PAGE_EXECUTE_READWRITE;
        default:   return PAGE_READWRITE;
    }
}

/* ============================================================================
 * Contiguous Memory (GPU-accessible on Xbox)
 * ============================================================================ */

PVOID __stdcall xbox_MmAllocateContiguousMemory(ULONG NumberOfBytes)
{
    PVOID p = VirtualAlloc(NULL, NumberOfBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    XBOX_TRACE(XBOX_LOG_MEM, "MmAllocateContiguousMemory(%u) = %p", NumberOfBytes, p);
    return p;
}

PVOID __stdcall xbox_MmAllocateContiguousMemoryEx(
    ULONG NumberOfBytes,
    ULONG_PTR LowestAcceptableAddress,
    ULONG_PTR HighestAcceptableAddress,
    ULONG Alignment,
    ULONG Protect)
{
    /*
     * Xbox requests physically contiguous, aligned memory for GPU use.
     * We can't guarantee physical contiguity on Windows, but the game's
     * CPU-side code just needs a valid pointer. GPU resources are handled
     * separately by our D3D11 layer.
     *
     * Use _aligned_malloc for alignment, then VirtualAlloc for a fallback.
     */
    PVOID p = NULL;

    if (Alignment > 0 && (Alignment & (Alignment - 1)) == 0) {
        /* Power-of-2 alignment: use _aligned_malloc */
        p = _aligned_malloc(NumberOfBytes, Alignment);
        if (p)
            memset(p, 0, NumberOfBytes);
    }

    if (!p) {
        /* Fallback: page-aligned VirtualAlloc */
        p = VirtualAlloc(NULL, NumberOfBytes, MEM_COMMIT | MEM_RESERVE,
                         xbox_protect_to_win32(Protect));
    }

    XBOX_TRACE(XBOX_LOG_MEM, "MmAllocateContiguousMemoryEx(%u, align=%u) = %p",
        NumberOfBytes, Alignment, p);
    return p;
}

VOID __stdcall xbox_MmFreeContiguousMemory(PVOID BaseAddress)
{
    XBOX_TRACE(XBOX_LOG_MEM, "MmFreeContiguousMemory(%p)", BaseAddress);
    if (!BaseAddress)
        return;

    /*
     * Determine if this was allocated with _aligned_malloc or VirtualAlloc.
     * We use VirtualQuery to check: if it's a VirtualAlloc'd region,
     * AllocationBase will equal the pointer (page-aligned).
     */
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(BaseAddress, &mbi, sizeof(mbi)) &&
        mbi.AllocationBase == BaseAddress &&
        mbi.State == MEM_COMMIT) {
        VirtualFree(BaseAddress, 0, MEM_RELEASE);
    } else {
        _aligned_free(BaseAddress);
    }
}

/* ============================================================================
 * System Memory
 * ============================================================================ */

PVOID __stdcall xbox_MmAllocateSystemMemory(ULONG NumberOfBytes, ULONG Protect)
{
    PVOID p = VirtualAlloc(NULL, NumberOfBytes, MEM_COMMIT | MEM_RESERVE,
                           xbox_protect_to_win32(Protect));
    XBOX_TRACE(XBOX_LOG_MEM, "MmAllocateSystemMemory(%u) = %p", NumberOfBytes, p);
    return p;
}

VOID __stdcall xbox_MmFreeSystemMemory(PVOID BaseAddress, ULONG NumberOfBytes)
{
    XBOX_TRACE(XBOX_LOG_MEM, "MmFreeSystemMemory(%p, %u)", BaseAddress, NumberOfBytes);
    if (BaseAddress)
        VirtualFree(BaseAddress, 0, MEM_RELEASE);
}

/* ============================================================================
 * Memory Query & Protection
 * ============================================================================ */

NTSTATUS __stdcall xbox_MmQueryStatistics(PXBOX_MM_STATISTICS MemoryStatistics)
{
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);

    if (!MemoryStatistics)
        return STATUS_INVALID_PARAMETER;

    if (!GlobalMemoryStatusEx(&ms))
        return STATUS_UNSUCCESSFUL;

    memset(MemoryStatistics, 0, sizeof(XBOX_MM_STATISTICS));
    MemoryStatistics->Length = sizeof(XBOX_MM_STATISTICS);

    /* Xbox has 64MB RAM. Report plausible values. */
    ULONG page_size = 4096;
    MemoryStatistics->TotalPhysicalPages = 64 * 1024 * 1024 / page_size; /* 16384 pages */
    MemoryStatistics->AvailablePages = (ULONG)(ms.ullAvailPhys / page_size);
    if (MemoryStatistics->AvailablePages > MemoryStatistics->TotalPhysicalPages)
        MemoryStatistics->AvailablePages = MemoryStatistics->TotalPhysicalPages / 2;

    return STATUS_SUCCESS;
}

PVOID __stdcall xbox_MmMapIoSpace(ULONG_PTR PhysicalAddress, ULONG NumberOfBytes, ULONG Protect)
{
    /* GPU register access - handled by our D3D11 layer. Return a dummy buffer. */
    PVOID p = VirtualAlloc(NULL, NumberOfBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    XBOX_TRACE(XBOX_LOG_MEM, "MmMapIoSpace(0x%08X, %u) = %p (stub)", (ULONG)PhysicalAddress, NumberOfBytes, p);
    return p;
}

VOID __stdcall xbox_MmUnmapIoSpace(PVOID BaseAddress, ULONG NumberOfBytes)
{
    XBOX_TRACE(XBOX_LOG_MEM, "MmUnmapIoSpace(%p, %u)", BaseAddress, NumberOfBytes);
    if (BaseAddress)
        VirtualFree(BaseAddress, 0, MEM_RELEASE);
}

/* Physical identities for CPU pages outside the contiguous window. Reserve
 * their physical pages through the existing allocator, retain the actual
 * CPU backing, and expose that same backing to DMA consumers. A known heap
 * allocation reserves its remaining pages together: NINE uses unaligned
 * four-kilobyte SGEs whose tails reach the next physical page. */
static SRWLOCK g_dma_page_lock = SRWLOCK_INIT;
typedef struct DmaPageIdentity { uint32_t guest, physical; } DmaPageIdentity;
static DmaPageIdentity g_dma_pages[16384];
static unsigned g_dma_page_count;
typedef struct DmaPageCheckpoint { uint32_t count;DmaPageIdentity pages[16384]; } DmaPageCheckpoint;
int xbox_SaveDmaPageIdentities(FILE *out) {
    if(!out || !xbox_CheckpointFrozen())return 0;
    AcquireSRWLockShared(&g_dma_page_lock);
    int ok=fwrite("XBDMA",5,1,out)==1 && fwrite(&g_dma_page_count,4,1,out)==1 &&
        fwrite(g_dma_pages,sizeof g_dma_pages[0],g_dma_page_count,out)==g_dma_page_count;
    ReleaseSRWLockShared(&g_dma_page_lock);return ok;
}
void *xbox_PrepareDmaPageIdentities(FILE *in) {
    if(!in || !xbox_CheckpointFrozen())return NULL;
    DmaPageCheckpoint *saved=malloc(sizeof *saved);if(!saved)return NULL;
    char magic[5];int ok=fread(magic,5,1,in)==1 && !memcmp(magic,"XBDMA",5) &&
        fread(&saved->count,4,1,in)==1 && saved->count<=16384 &&
        fread(saved->pages,sizeof saved->pages[0],saved->count,in)==saved->count;
    for(unsigned i=0;ok && i<saved->count;i++) {
        DmaPageIdentity page=saved->pages[i];
        ok=!(page.guest&4095) && !(page.physical&4095) && page.guest &&
            (uint64_t)page.guest+4096<=xbox_GetMappedSize() && (uint64_t)page.physical+4096<=XBOX_CONTIG_SIZE;
        for(unsigned j=0;ok && j<i;j++)ok=page.guest!=saved->pages[j].guest && page.physical!=saved->pages[j].physical;
    }
    if(!ok){free(saved);return NULL;}return saved;
}
void xbox_ApplyDmaPageIdentities(void *opaque) {
    DmaPageCheckpoint *saved=opaque;
    AcquireSRWLockExclusive(&g_dma_page_lock);
    memcpy(g_dma_pages,saved->pages,saved->count*sizeof g_dma_pages[0]);g_dma_page_count=saved->count;
    ReleaseSRWLockExclusive(&g_dma_page_lock);
}

ULONG_PTR __stdcall xbox_MmGetPhysicalAddress(PVOID BaseAddress)
{
    uint32_t va = (uint32_t)(uintptr_t)BaseAddress;
    uint32_t page = va & ~4095u, physical;
    uint32_t remaining, pages;
    unsigned i;
    if (va >= XBOX_CONTIG_BASE &&
        (uint64_t)va < (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE)
        return va - XBOX_CONTIG_BASE;
    if (!va || (uint64_t)page + 4096 > xbox_GetMappedSize()) {
        fprintf(stderr, "[STOP] MmGetPhysicalAddress has no CPU backing: 0x%08X\n", va);
        exit(5);
    }
    remaining = xbox_HeapBlockSize(va);
    pages = remaining ? (uint32_t)(((uint64_t)(va & 4095u) + remaining + 4095) / 4096) : 1;
    AcquireSRWLockExclusive(&g_dma_page_lock);
    for (i = 0; i < g_dma_page_count; ++i)
        if (g_dma_pages[i].guest == page) {
            physical = g_dma_pages[i].physical;
            ReleaseSRWLockExclusive(&g_dma_page_lock);
            return physical + (va & 4095u);
        }
    for (i = 0; i < g_dma_page_count; ++i)
        if (g_dma_pages[i].guest >= page &&
            (uint64_t)g_dma_pages[i].guest < (uint64_t)page + pages * 4096u) {
            fprintf(stderr, "[STOP] CPU DMA allocation overlaps an established page identity\n");
            exit(5);
        }
    if (pages > 16384 - g_dma_page_count ||
        !(physical = xbox_ContiguousAlloc(pages * 4096u, 4096))) {
        fprintf(stderr, "[STOP] CPU DMA physical-page allocation failed\n");
        exit(5);
    }
    physical -= XBOX_CONTIG_BASE;
    for (i = 0; i < pages; ++i) {
        g_dma_pages[g_dma_page_count].guest = page + i * 4096u;
        g_dma_pages[g_dma_page_count++].physical = physical + i * 4096u;
    }
    ReleaseSRWLockExclusive(&g_dma_page_lock);
    return physical + (va & 4095u);
}

void *xbox_ResolvePhysicalBuffer(uint32_t physical, uint32_t bytes)
{
    uint32_t guest = XBOX_CONTIG_BASE + physical;
    unsigned i;
    if ((uint64_t)physical + bytes > XBOX_CONTIG_SIZE) {
        fprintf(stderr, "[STOP] DMA physical span 0x%08X + 0x%X\n", physical, bytes);
        exit(5);
    }
    AcquireSRWLockShared(&g_dma_page_lock);
    for (i = 0; i < g_dma_page_count; ++i)
        if (physical >= g_dma_pages[i].physical &&
            physical - g_dma_pages[i].physical < 4096) {
            if ((physical & 4095u) + (uint64_t)bytes > 4096) {
                fprintf(stderr, "[STOP] CPU DMA alias crosses an unverified page boundary\n");
                exit(5);
            }
            guest = g_dma_pages[i].guest + (physical & 4095u);
            break;
        }
    ReleaseSRWLockShared(&g_dma_page_lock);
    return (void *)((uintptr_t)xbox_GetMemoryOffset() + guest);
}

VOID __stdcall xbox_MmPersistContiguousMemory(PVOID BaseAddress, ULONG NumberOfBytes, BOOLEAN Persist)
{
    /* Xbox: mark memory to survive soft-reboot. No-op on Windows. */
    XBOX_TRACE(XBOX_LOG_MEM, "MmPersistContiguousMemory(%p, %u, %d) - stub", BaseAddress, NumberOfBytes, Persist);
}

ULONG __stdcall xbox_MmQueryAddressProtect(PVOID VirtualAddress)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(VirtualAddress, &mbi, sizeof(mbi))) {
        /* Return the Xbox-equivalent protection */
        return mbi.Protect;
    }
    return PAGE_NOACCESS;
}

VOID __stdcall xbox_MmSetAddressProtect(PVOID BaseAddress, ULONG NumberOfBytes, ULONG NewProtect)
{
    DWORD old_protect;
    VirtualProtect(BaseAddress, NumberOfBytes, xbox_protect_to_win32(NewProtect), &old_protect);
    XBOX_TRACE(XBOX_LOG_MEM, "MmSetAddressProtect(%p, %u, 0x%X)", BaseAddress, NumberOfBytes, NewProtect);
}

ULONG __stdcall xbox_MmQueryAllocationSize(PVOID BaseAddress)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(BaseAddress, &mbi, sizeof(mbi))) {
        return (ULONG)mbi.RegionSize;
    }
    return 0;
}

PVOID __stdcall xbox_MmClaimGpuInstanceMemory(ULONG NumberOfBytes, PULONG NumberOfPaddingBytes)
{
    /* GPU instance memory - handled by D3D11 layer */
    if (NumberOfPaddingBytes)
        *NumberOfPaddingBytes = 0;
    PVOID p = VirtualAlloc(NULL, NumberOfBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    XBOX_TRACE(XBOX_LOG_MEM, "MmClaimGpuInstanceMemory(%u) = %p (stub)", NumberOfBytes, p);
    return p;
}

VOID __stdcall xbox_MmLockUnlockBufferPages(PVOID BaseAddress, ULONG NumberOfBytes, BOOLEAN UnlockPages)
{
    /* Page locking is not meaningful in user mode. No-op. */
    XBOX_TRACE(XBOX_LOG_MEM, "MmLockUnlockBufferPages(%p, %u, %d) - stub", BaseAddress, NumberOfBytes, UnlockPages);
}

VOID __stdcall xbox_MmLockUnlockPhysicalPage(ULONG_PTR PhysicalAddress, BOOLEAN UnlockPage)
{
    XBOX_TRACE(XBOX_LOG_MEM, "MmLockUnlockPhysicalPage(0x%08X, %d) - stub", (ULONG)PhysicalAddress, UnlockPage);
}

/* ============================================================================
 * Kernel Stack
 * ============================================================================ */

PVOID __stdcall xbox_MmCreateKernelStack(ULONG NumberOfBytes, BOOLEAN DebuggerThread)
{
    /* Allocate a stack-like region. Return the TOP of the stack (high address). */
    PVOID base = VirtualAlloc(NULL, NumberOfBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!base)
        return NULL;

    /* Xbox convention: return pointer to top of stack */
    PVOID stack_top = (PUCHAR)base + NumberOfBytes;
    XBOX_TRACE(XBOX_LOG_MEM, "MmCreateKernelStack(%u) = %p (base=%p)", NumberOfBytes, stack_top, base);
    return stack_top;
}

VOID __stdcall xbox_MmDeleteKernelStack(PVOID StackBase, PVOID StackLimit)
{
    /* StackLimit is the low address (base of VirtualAlloc), StackBase is the high address */
    XBOX_TRACE(XBOX_LOG_MEM, "MmDeleteKernelStack(base=%p, limit=%p)", StackBase, StackLimit);
    if (StackLimit)
        VirtualFree(StackLimit, 0, MEM_RELEASE);
}

/* ============================================================================
 * Virtual Memory (Nt API)
 * ============================================================================ */

NTSTATUS __stdcall xbox_NtAllocateVirtualMemory(
    PVOID* BaseAddress,
    ULONG_PTR ZeroBits,
    PSIZE_T RegionSize,
    ULONG AllocationType,
    ULONG Protect)
{
    if (!BaseAddress || !RegionSize)
        return STATUS_INVALID_PARAMETER;

    PVOID result = VirtualAlloc(*BaseAddress, *RegionSize,
                                AllocationType, xbox_protect_to_win32(Protect));
    if (!result) {
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_MEM,
            "NtAllocateVirtualMemory failed: base=%p size=%u type=0x%X err=%u",
            *BaseAddress, (ULONG)*RegionSize, AllocationType, GetLastError());
        return STATUS_NO_MEMORY;
    }

    *BaseAddress = result;
    XBOX_TRACE(XBOX_LOG_MEM, "NtAllocateVirtualMemory(%p, %u) = %p",
        *BaseAddress, (ULONG)*RegionSize, result);
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtFreeVirtualMemory(
    PVOID* BaseAddress,
    PSIZE_T RegionSize,
    ULONG FreeType)
{
    if (!BaseAddress || !*BaseAddress)
        return STATUS_INVALID_PARAMETER;

    SIZE_T size = (FreeType & MEM_RELEASE) ? 0 : (RegionSize ? *RegionSize : 0);

    if (!VirtualFree(*BaseAddress, size, FreeType)) {
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_MEM,
            "NtFreeVirtualMemory failed: base=%p type=0x%X err=%u",
            *BaseAddress, FreeType, GetLastError());
        return STATUS_UNSUCCESSFUL;
    }

    XBOX_TRACE(XBOX_LOG_MEM, "NtFreeVirtualMemory(%p, 0x%X)", *BaseAddress, FreeType);

    if (FreeType & MEM_RELEASE)
        *BaseAddress = NULL;

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtQueryVirtualMemory(
    PVOID BaseAddress,
    PVOID MemoryInformation,
    ULONG MemoryInformationLength,
    PULONG ReturnLength)
{
    MEMORY_BASIC_INFORMATION mbi;

    if (!VirtualQuery(BaseAddress, &mbi, sizeof(mbi)))
        return STATUS_INVALID_PARAMETER;

    /*
     * Xbox NtQueryVirtualMemory returns a MEMORY_BASIC_INFORMATION-like struct.
     * Copy what fits into the caller's buffer.
     */
    ULONG copy_size = (MemoryInformationLength < sizeof(mbi)) ? MemoryInformationLength : (ULONG)sizeof(mbi);
    memcpy(MemoryInformation, &mbi, copy_size);

    if (ReturnLength)
        *ReturnLength = copy_size;

    return STATUS_SUCCESS;
}
