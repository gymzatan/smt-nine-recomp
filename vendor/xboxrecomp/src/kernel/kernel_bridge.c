/**
 * kernel_bridge.c - Bridge between translated game code and kernel functions
 *
 * Problem:
 *   Translated game code calls kernel functions via indirect calls through
 *   the kernel thunk table at VA 0x0036B7C0. In the XBE file, these entries
 *   contain unresolved ordinals (0x80000000 | ordinal). On real Xbox hardware,
 *   the kernel loader replaces these with actual function pointers before the
 *   game runs.
 *
 * Solution:
 *   1. After xbox_MemoryLayoutInit copies .rdata, call xbox_kernel_bridge_init()
 *   2. Replace each ordinal entry in Xbox memory with a synthetic VA
 *   3. When RECOMP_ICALL encounters a synthetic VA, route it to a per-ordinal
 *      bridge function that reads args from the simulated Xbox stack, translates
 *      pointer arguments from Xbox VA→native, and calls the kernel function.
 *
 * Synthetic VA scheme:
 *   Each thunk slot i gets VA 0xFE000000 + i*4
 *   The lookup function checks this range and dispatches appropriately.
 *
 * Why per-ordinal bridges instead of a generic trampoline:
 *   Kernel functions receive Xbox pointers (32-bit VAs) that must be translated
 *   to native pointers by adding g_xbox_mem_offset. Different functions have
 *   different parameter layouts (pointer vs value), so each needs its own bridge.
 */

#include "kernel.h"
#include "guest_vmem.h"
#include "xbox_memory_layout.h"
#include "recomp_icall_feedback.h"
#include "recomp_continuation.h"
#include "recomp_fpu_state.h"
#include "../nv2a/nv2a_state.h"
#include <stdio.h>
/* stdlib.h is load-bearing, not tidiness. Without it C89 implicit declaration
 * makes malloc return `int`, so bridge_spawn_thread truncated its heap pointer
 * to 32 bits and sign-extended it into a `struct bridge_thread_start *`. Every
 * subsequent s->field wrote to an address that had nothing to do with the
 * allocation. MSVC says so (C4013 + C4047 "differs in levels of indirection")
 * but only as warnings, and this file is compiled with /W4 /WX-. */
#include <stdlib.h>
#include <float.h>
/* Section B string helpers: wcslen helper, case folding.
 * ctype.h/wctype.h are not pulled in by the platform headers on either host. */
#include <string.h>
#include <ctype.h>
#include <wctype.h>
#include <setjmp.h>
#include "apu/apu_xaudio2.h"

/* Access to recompiled code registers. Per-thread: RECOMP_TLS comes from
 * xbox_memory_layout.h and must match the definitions there -- a plain extern
 * here binds to the TLS template rather than the calling thread's copy, which
 * reads as every register being zero. */
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi, g_ebp;
extern uint32_t g_xbox_code_lo, g_xbox_code_hi;
extern RECOMP_TLS uint32_t g_seh_ebp;
extern ptrdiff_t g_xbox_mem_offset;
static RECOMP_TLS int t_dispatch_depth;
static RECOMP_TLS int t_guest_thread;     /* this host thread runs a guest thread */
static RECOMP_TLS int t_guest_terminated;

/* Dispatch table lookup (for function pointer args) */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
/* Timer DPCs run on a host thread, which needs a guest stack under it. */
int  xbox_worker_stack_alloc(void);

recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);

/* Memory access - same as recomp_types.h MEM32 but without the #define guard */
#define BRIDGE_MEM32(addr) (*(volatile uint32_t *)((uintptr_t)(addr) + g_xbox_mem_offset))

/* Translate Xbox VA to native pointer (NULL-safe: 0 → NULL) */
#define XBOX_TO_NATIVE(va) ((va) ? (void*)((uintptr_t)(va) + g_xbox_mem_offset) : NULL)

/* ── Guest buffers the host is about to touch ───────────
 *
 * A bridge turns a guest VA into a host pointer by adding an offset, so a VA
 * the guest got wrong does not fail the call -- it faults inside the kernel
 * implementation, on a host stack with no recompiled frame in it and a fault
 * address that means nothing on its own.
 *
 * The dangerous shape is an address AND a length that both come from the
 * guest. NtReadFile is the clearest case: the host WRITES `length` bytes
 * through the pointer, so a buffer near the top of the mapping, or a length
 * that does not match the buffer it names, walks the host past the end of
 * guest memory writing file contents into whatever follows. Nothing above this
 * layer can catch it, because xbox_NtReadFile receives a host pointer and a
 * count and cannot know where the mapping ends.
 */
static int bridge_va_mapped(uint32_t va, uint32_t bytes)
{
    uint64_t end = (uint64_t)va + bytes;
    uint64_t mapped = g_xbox_map_size ? g_xbox_map_size : g_xbox_total_ram;

    if (va < 0x1000)            /* page zero is deliberately unmapped */
        return 0;
    if (end <= mapped)
        return 1;
    return va >= XBOX_CONTIG_BASE
        && end <= (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE;
}

/* STATUS_ACCESS_VIOLATION is what NT answers for a user buffer it cannot
 * touch, and it is far more useful to a title than a host crash: the call
 * fails, the guest gets a status it has a branch for, and the log names the
 * export, the buffer and the length. Warned once per export so a title that
 * does this in a loop does not bury the rest of the log. */
static int bridge_buf_ok(uint32_t va, uint32_t bytes, const char *export_name)
{
    static const char *seen[16];
    static int distinct;
    int i;

    if (!bytes)                                   /* nothing is accessed */
        return 1;
    if (va && bridge_va_mapped(va, bytes))
        return 1;

    for (i = 0; i < distinct; ++i)
        if (seen[i] == export_name)
            return 0;
    if (distinct < (int)(sizeof(seen) / sizeof(seen[0])))
        seen[distinct++] = export_name;

    fprintf(stderr,
            "  [KERNEL] %s: buffer 0x%08X length %u is not mapped guest "
            "memory; returning STATUS_ACCESS_VIOLATION\n",
            export_name, va, (unsigned)bytes);
    fflush(stderr);
    return 0;
}

/* ── Synthetic VA range (for function exports) ─────────── */

#define KERNEL_VA_BASE  0xFE000000u
#define KERNEL_VA_END   (KERNEL_VA_BASE + XBOX_KERNEL_THUNK_TABLE_SIZE * 4)
/* Internal continuation identities, outside the imported-thunk range. */
#define KERNEL_CONT_CALLBACK (KERNEL_VA_END + 0x100)
#define KERNEL_CONT_APC      (KERNEL_VA_END + 0x104)
#define KERNEL_CONT_DRIVER   (KERNEL_VA_END + 0x108)
#define KERNEL_CONT_COMPLETE (KERNEL_VA_END + 0x10c)
#define KERNEL_CONT_SYNC_API (KERNEL_VA_END + 0x110)
#define KERNEL_CONT_SYNC_CALL (KERNEL_VA_END + 0x114)
#define KERNEL_CONT_NATIVE_WAIT (KERNEL_VA_END + 0x118)
#define KERNEL_CONT_QUEUE (KERNEL_VA_END + 0x11c)
#define KERNEL_CONT_COMPLETION (KERNEL_VA_END + 0x120)
#define KERNEL_CONT_RUNDOWN (KERNEL_VA_END + 0x124)
#define KERNEL_CONT_TERMINATE (KERNEL_VA_END + 0x128)
#define KERNEL_CONT_THREAD_EXIT (KERNEL_VA_END + 0x12c)
#define KERNEL_CONT_INVALID_REQUEST (KERNEL_VA_END + 0x130)
#define KERNEL_CONT_DEVICE_DELETE (KERNEL_VA_END + 0x134)
#define KERNEL_CONT_NATIVE_DEVICE (KERNEL_VA_END + 0x138)
#define KERNEL_CONT_CONTROL_FILE (KERNEL_VA_END + 0x13c)
#define KERNEL_CONT_FILE_DEVICE_RELEASE (KERNEL_VA_END + 0x140)
#define KERNEL_CONT_CLOSE_HANDLE (KERNEL_VA_END + 0x144)
#define KERNEL_CONT_OBJECT_RELEASE (KERNEL_VA_END + 0x148)
#define KERNEL_CONT_DEREFERENCE (KERNEL_VA_END + 0x14c)
#define KERNEL_CONT_REQUEST_RELEASE (KERNEL_VA_END + 0x150)
#define KERNEL_CONT_COMPLETE_APC (KERNEL_VA_END + 0x154)
#define KERNEL_CONT_CONTROL_RELEASE (KERNEL_VA_END + 0x158)
#define KERNEL_CONT_FILE_REQUEST (KERNEL_VA_END + 0x15c)
#define KERNEL_CONT_FILE_DELETE (KERNEL_VA_END + 0x160)
#define KERNEL_CONT_EXCEPTION_DISPATCH (KERNEL_VA_END + 0x168)
#define KERNEL_CONT_EXCEPTION_RECORD (KERNEL_VA_END + 0x16c)
#define KERNEL_CONT_EXCEPTION_ENTRY (KERNEL_VA_END + 0x170)
#define KERNEL_CONT_EXCEPTION_TRANSFER (KERNEL_VA_END + 0x174)
#define KERNEL_CONT_CPU_EXCEPTION (KERNEL_VA_END + 0x178)
#define KERNEL_EXCEPTION_NESTED_GUARD (KERNEL_VA_END + 0x17c)
#define KERNEL_CONT_CREATE_FILE (KERNEL_VA_END + 0x180)
#define KERNEL_CONT_CREATE_IMPORT (KERNEL_VA_END + 0x184)
#define KERNEL_CONT_CREATE_RELEASE (KERNEL_VA_END + 0x188)
#define KERNEL_CONT_UNWIND (KERNEL_VA_END + 0x18c)
#define KERNEL_CONT_DPC (KERNEL_VA_END + 0x194)
#define KERNEL_CONT_SHUTDOWN (KERNEL_VA_END + 0x198)
#define KERNEL_UNWIND_COLLISION_GUARD (KERNEL_VA_END + 0x190)

/* ── Kernel data exports ──────────────────────────────────
 *
 * Some kernel ordinals are DATA exports (structs/variables), not functions.
 * The game reads their thunk entries and dereferences the result to access
 * the data. These cannot use synthetic VAs — they must point to real,
 * dereferenceable addresses in the Xbox VA space.
 *
 * We allocate a "kernel data area" at XBOX_KERNEL_DATA_BASE and populate
 * it with the expected structures.
 */

#define BRIDGE_MEM16(addr) (*(volatile uint16_t *)((uintptr_t)(addr) + g_xbox_mem_offset))
#define BRIDGE_MEM8(addr)  (*(volatile uint8_t  *)((uintptr_t)(addr) + g_xbox_mem_offset))

/**
 * Get the Xbox VA of data for a kernel DATA export ordinal.
 * Returns 0 if the ordinal is not a data export (i.e., it's a function).
 */
static uint32_t kernel_data_va_for_ordinal(ULONG ordinal)
{
    /* Ordinals here are checked BEFORE function routing (see the thunk build
     * loop), so an ordinal listed by mistake turns a real kernel function into
     * a data address -- the title then calls it and jumps into kernel data.
     *
     * This table had a whole block shifted. 17 (ExFreePool), 65
     * (IoCreateDevice), 327 (XeLoadSection) and 328 (XeUnloadSection) are all
     * functions and were all being handed data addresses; Crimson Skies imports
     * every one of them. In the other direction, the genuine exports at 16, 353,
     * 354, 355, 356 and 357 got no thunk at all, so a title reading
     * XboxLANKey or KeTimeIncrement read whatever the function fallback left.
     *
     * test_bridge_ordinals.py now checks every entry below against the export
     * table, which is why the block cannot drift again unnoticed. */
    switch (ordinal) {
    case  16: return XBOX_KERNEL_DATA_BASE + KDATA_EVENT_OBJ_TYPE;
    case  22: return XBOX_KERNEL_DATA_BASE + KDATA_MUTANT_OBJ_TYPE;
    case  30: return XBOX_KERNEL_DATA_BASE + KDATA_SEMAPHORE_OBJ_TYPE;
    case  31: return XBOX_KERNEL_DATA_BASE + KDATA_TIMER_OBJ_TYPE;
    case  40: return XBOX_KERNEL_DATA_BASE + KDATA_DISK_CACHE_PARTS;
    case  41: return XBOX_KERNEL_DATA_BASE + KDATA_DISK_MODEL_STR;
    case  42: return XBOX_KERNEL_DATA_BASE + KDATA_DISK_SERIAL_STR;
    case  64: return XBOX_KERNEL_DATA_BASE + KDATA_IO_COMPLETION_TYPE;
    case  70: return XBOX_KERNEL_DATA_BASE + KDATA_IO_DEVICE_TYPE;
    case  71: return XBOX_KERNEL_DATA_BASE + KDATA_FILE_OBJ_TYPE;
    case 156: return XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT;
    case 157: return XBOX_KERNEL_DATA_BASE + KDATA_TIME_INCREMENT;
    case 164: return XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE;
    case 259: return XBOX_KERNEL_DATA_BASE + KDATA_THREAD_OBJ_TYPE;
    case 322: return XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO;
    case 323: return XBOX_KERNEL_DATA_BASE + KDATA_HD_KEY;
    case 324: return XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION;
    case 325: return XBOX_KERNEL_DATA_BASE + KDATA_SIGNATURE_KEY;
    case 326: return XBOX_KERNEL_DATA_BASE + KDATA_XE_IMAGE_FILENAME;
    case 353: return XBOX_KERNEL_DATA_BASE + KDATA_LAN_KEY;
    case 354: return XBOX_KERNEL_DATA_BASE + KDATA_ALT_SIGNATURE_KEYS;
    case 355: return XBOX_KERNEL_DATA_BASE + KDATA_XE_PUBLIC_KEY;
    case 356: return XBOX_KERNEL_DATA_BASE + KDATA_BOOT_SMC_VIDEO;
    case 357: return XBOX_KERNEL_DATA_BASE + KDATA_IDEX_CHANNEL;
    default:  return 0;  /* Not a data export */
    }
}

/**
 * Initialize kernel data export values at the kernel data area.
 * Called during bridge init, after Xbox memory is mapped.
 */
static void kernel_data_init(void)
{
    uint32_t shutdown_head=XBOX_KERNEL_DATA_BASE+KDATA_SHUTDOWN_HEAD;
    BRIDGE_MEM32(shutdown_head)=BRIDGE_MEM32(shutdown_head+4)=shutdown_head;
    /* XboxHardwareInfo (ordinal 322) - XBOX_HARDWARE_INFO
     *   +0: ULONG Flags (0 = retail, 0x20 = devkit)
     *   +4: UCHAR GpuRevision
     *   +5: UCHAR McpRevision
     */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO + 0) = 0;   /* Retail */
    BRIDGE_MEM8(XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO + 4) = 0xA1; /* NV2A A1 */
    BRIDGE_MEM8(XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO + 5) = 0xB1; /* MCPX B1 */

    /* XboxKrnlVersion (ordinal 324) - XBOX_KRNL_VERSION
     *   +0: USHORT Major (1)
     *   +2: USHORT Minor (0)
     *   +4: USHORT Build (5849 = XDK version)
     *   +6: USHORT Qfe (0)
     */
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 0) = 1;
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 2) = 0;
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 4) = 5849;
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 6) = 0;

    /* KeTickCount (ordinal 156), in host milliseconds. The existing device
     * worker advances the exported data independently of guest reads. */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT) = (uint32_t)xbox_RuntimeTickCount64();

    /* LaunchDataPage (ordinal 164).
     *
     * This is how a title receives a command line. XGetLaunchInfo reads the
     * page, takes LaunchDataType from its first dword, and copies 3072 bytes
     * from +0x400; a type of 3 means those bytes *are* the command line.
     * Half-Life 2 does exactly that in sub_00596710, and falls back to the
     * empty string at 0x00772EA7 when the call fails -- which is what a NULL
     * page produces, so the engine started with no arguments and no map.
     *
     * On hardware the launcher fills this in before rebooting into the title.
     * RECOMP_CMDLINE does the same thing here, so a title can be told to load
     * a level the way the console would tell it: "+map intro".
     */
    {
        const char *cmdline = getenv("RECOMP_CMDLINE");

        if (cmdline && *cmdline) {
            uint32_t page = xbox_HeapAlloc(0x1000 + 0x0C00, 4096);
            if (page) {
                size_t n = strlen(cmdline);
                size_t i;

                if (n > 0x0BFF)
                    n = 0x0BFF;
                BRIDGE_MEM32(page + 0) = 3;          /* LaunchDataType */
                BRIDGE_MEM32(page + 4) = 0x45410091; /* title id */
                for (i = 0; i < n; i++)
                    BRIDGE_MEM8(page + 0x400 + (uint32_t)i) =
                        (uint8_t)cmdline[i];
                BRIDGE_MEM8(page + 0x400 + (uint32_t)n) = 0;

                BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE) =
                    page;
                fprintf(stderr, "  Launch data: type 3 at 0x%08X, "
                                "command line %s\n", page, cmdline);
            } else {
                BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE) = 0;
            }
        } else {
            BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE) = 0;
        }
    }

    /* Object-type exports. Each gets a DISTINCT non-zero value rather than 0.
     *
     * They were all zero, which is wrong twice over: a title that null-checks
     * one sees "no such type", and a title that distinguishes two of them --
     * ObReferenceObjectByHandle takes an expected type and compares it -- sees
     * every type as equal, so a mutant handle passes a check meant for events.
     * The values are opaque to the game; only identity and non-nullness matter,
     * so they are the export ordinal offset into the kernel data area, which
     * also makes a stray one recognisable in a crash dump.
     */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_THREAD_OBJ_TYPE)    = XBOX_KERNEL_DATA_BASE + KDATA_THREAD_OBJ_TYPE;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_EVENT_OBJ_TYPE)     = XBOX_KERNEL_DATA_BASE + KDATA_EVENT_OBJ_TYPE;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_MUTANT_OBJ_TYPE)    = XBOX_KERNEL_DATA_BASE + KDATA_MUTANT_OBJ_TYPE;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_SEMAPHORE_OBJ_TYPE) = XBOX_KERNEL_DATA_BASE + KDATA_SEMAPHORE_OBJ_TYPE;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_TIMER_OBJ_TYPE)     = XBOX_KERNEL_DATA_BASE + KDATA_TIMER_OBJ_TYPE;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_FILE_OBJ_TYPE)      = XBOX_KERNEL_DATA_BASE + KDATA_FILE_OBJ_TYPE;

    /* KeTimeIncrement (ordinal 157) - 100ns units per clock tick. 0x2710 is
     * 1 ms, which is what KeTickCount above is updated at. A title dividing by
     * this to convert ticks to time gets a division by zero if it is left 0. */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_TIME_INCREMENT) = 0x2710;

    /* HalBootSMCVideoMode (ordinal 356) - SMC video mode word from boot. 0 is
     * "no video mode reported", which titles treat as auto-detect. */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_BOOT_SMC_VIDEO) = 0;

    /* IdexChannelObject is a structure, not an opaque pointer. Guest file-close
     * code walks DeviceQueue.DeviceListHead at +0x28. Host-backed synchronous
     * I/O does not enqueue guest IRPs, so this must be an empty circular list.
     * Reserve separate storage: the old 16-byte slot overlapped the key exports. */
    {
        uint32_t channel=XBOX_KERNEL_DATA_BASE + KDATA_IDEX_CHANNEL;
        memset(XBOX_TO_NATIVE(channel),0,0x200);
        BRIDGE_MEM32(channel+0x28)=channel+0x28;
        BRIDGE_MEM32(channel+0x2C)=channel+0x28;
    }

    /* HalDiskCachePartitionCount (ordinal 40) - number of cache partitions.
     * Retail consoles report 3 (X, Y, Z). Titles size a partition array from
     * this, so 0 gives a zero-length array and 1 hides two drives. */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_DISK_CACHE_PARTS) = 3;

    /* IoCompletionObjectType (ordinal 64) - type object */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_IO_COMPLETION_TYPE) = 0;

    /* IoDeviceObjectType (ordinal 71) - type object (stub: 0) */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_IO_DEVICE_TYPE) = 0;

    /* XboxHDKey (ordinal 323) - 16 bytes of zeros (no key) */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_HD_KEY) + g_xbox_mem_offset), 0, 16);

    /* XboxSignatureKey (ordinal 325) - 16 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_SIGNATURE_KEY) + g_xbox_mem_offset), 0, 16);

    /* XboxLANKey (ordinals 326, 355) - 16 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_LAN_KEY) + g_xbox_mem_offset), 0, 16);

    /* XboxAlternateSignatureKeys (ordinals 327, 356) - 256 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_ALT_SIGNATURE_KEYS) + g_xbox_mem_offset), 0, 256);

    /* XePublicKeyData (ordinal 357) - 284 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_XE_PUBLIC_KEY) + g_xbox_mem_offset), 0, 284);

    /* HAL disk identity strings (ordinals 41/42). The exported symbol is an
     * XBOX_ANSI_STRING whose Buffer must be an Xbox VA the title can deref --
     * HalRandGather reads the bytes for entropy. Build the struct and its text
     * inside the kernel data area so both are addressable. */
    {
        struct { uint32_t str_off, buf_off; const char *text; } d[] = {
            { KDATA_DISK_MODEL_STR,  KDATA_DISK_MODEL_BUF,  "XBOXRECOMP VIRTUAL HDD" },
            { KDATA_DISK_SERIAL_STR, KDATA_DISK_SERIAL_BUF, "XR0000000000" },
            /* XeImageFileName (ordinal 326). Declared but never filled in, so
             * its Buffer held whatever was in the page -- Half-Life 2's CRT
             * reads it while working out the running image's path, took the
             * uninitialised bytes as a char*, and dereferenced 0x68737572
             * (the ASCII "rush"). A disc-booted title's value looks like this. */
            { KDATA_XE_IMAGE_FILENAME, KDATA_XE_IMAGE_BUF,
              "\\Device\\CdRom0\\default.xbe" },
        };
        for (int k = 0; k < (int)(sizeof(d) / sizeof(d[0])); k++) {
            uint32_t str_va = XBOX_KERNEL_DATA_BASE + d[k].str_off;
            uint32_t buf_va = XBOX_KERNEL_DATA_BASE + d[k].buf_off;
            size_t len = strlen(d[k].text);
            memcpy(XBOX_TO_NATIVE(buf_va), d[k].text, len + 1);
            BRIDGE_MEM16(str_va + 0) = (uint16_t)len;        /* Length */
            BRIDGE_MEM16(str_va + 2) = (uint16_t)(len + 1);  /* MaximumLength */
            BRIDGE_MEM32(str_va + 4) = buf_va;               /* Buffer (Xbox VA) */
        }
    }

    fprintf(stderr, "  Kernel data exports: initialized at Xbox VA 0x%08X\n",
            XBOX_KERNEL_DATA_BASE);
}

/* ── Per-slot ordinal and bridge function ────────────────── */

/* Ordinal for each slot (read from Xbox memory during init) */
static ULONG g_slot_ordinals[XBOX_KERNEL_THUNK_TABLE_SIZE];

/* Log counter - limit output to avoid flooding */
/* Calls per ordinal, for the ranking in the periodic summary. 378 counters
 * is smaller than one of the strings this file prints. */
static unsigned long long g_ordinal_calls[XBOX_KERNEL_THUNK_TABLE_SIZE];
/* 64-bit: a title that polls the clock through the kernel passes 2^31 calls
 * within minutes (X-Men Legends does). As a 32-bit int it wrapped negative,
 * "count <= log budget" turned true, and every kernel call then wrote two log
 * lines through the shared stderr lock -- the frame rate fell to ~1 FPS. */
static long long g_kernel_call_count = 0;

/* How many kernel calls get logged before the log goes quiet.
 *
 * The cap keeps a title that makes thousands of calls from burying the
 * console, but a bring-up that gets past early init then has no visibility at
 * exactly the point it stops being obvious. Override with
 * RECOMP_KERNEL_LOG_BUDGET, the same way RECOMP_TRACE_BUDGET works for the
 * function tracer. 0 silences the log entirely.
 */
static long kernel_log_budget(void)
{
    static long budget = -1;

    if (budget < 0) {
        const char *env = getenv("RECOMP_KERNEL_LOG_BUDGET");
        budget = env ? strtol(env, NULL, 0) : 200;
        if (budget < 0)
            budget = 0;
    }
    return budget;
}

#define KERNEL_LOG_ON()      (g_kernel_call_count <= kernel_log_budget())
/* Some sites logged at a tighter cap than the rest; keep them proportional. */
#define KERNEL_LOG_ON_HALF() (g_kernel_call_count <= kernel_log_budget() / 2)

/* Read Xbox stack arg as uint32_t.
 * After kernel_thunk_dispatch pops the dummy return address (g_esp += 4),
 * arg0 is at g_esp+0, arg1 at g_esp+4, etc. */
#define STACK_ARG(n) ((uint32_t)BRIDGE_MEM32(g_esp + (n) * 4))

/* Guest return address of the call currently in a bridge. */
RECOMP_TLS uint32_t g_xbox_kernel_caller;

/* ── Per-ordinal bridge functions ─────────────────────────
 *
 * Each bridge reads args from the Xbox stack, translates pointer
 * args from Xbox VA→native, calls the kernel function, and stores
 * the result in g_eax.
 *
 * Xbox cdecl: args pushed right-to-left, caller cleans stack.
 * Xbox stdcall: args pushed right-to-left, callee cleans stack.
 * In our case the caller (translated code) does "PUSH32" for each arg
 * before calling, and the kernel function's ret-N is handled by the
 * translated code's own stack adjustment.
 */

/* ── PsCreateSystemThreadEx (ordinal 255) ────────────────
 * NTSTATUS PsCreateSystemThreadEx(
 *   PHANDLE ThreadHandle,      // arg0: Xbox VA → pointer
 *   ULONG ThreadExtraSize,     // arg1: value
 *   ULONG KernelStackSize,     // arg2: value
 *   ULONG TlsDataSize,         // arg3: value
 *   PULONG ThreadId,           // arg4: Xbox VA → pointer (can be NULL)
 *   PVOID StartContext1,       // arg5: Xbox VA → opaque
 *   PVOID StartContext2,       // arg6: Xbox VA → opaque
 *   BOOLEAN CreateSuspended,   // arg7: value
 *   BOOLEAN DebugStack,        // arg8: value
 *   PXBOX_SYSTEM_ROUTINE StartRoutine  // arg9: Xbox function pointer
 * )
 *
 * For static recompilation, we don't create a real thread.
 * Instead we call the StartRoutine synchronously via RECOMP_ICALL.
 * This is correct because on Xbox, the entry point creates a system
 * thread and returns, and the thread runs the actual game.
 */
static int g_thread_call_count = 0;

/* Thread entry shim. Sets up the new thread's own simulated stack, pushes the
 * two Xbox start-context arguments plus the dummy return address the callee's
 * `ret` consumes, and runs. */
/* Set on threads this bridge spawned; see PsTerminateSystemThread. */
static RECOMP_TLS int g_is_spawned_thread = 0;

/* This thread's simulated stack, so both exits can give it back. Thread-local
 * for the obvious reason, and needed at all because the common exit is
 * ExitThread from PsTerminateSystemThread -- bridge_thread_main's own return
 * path is the rare one. */
static RECOMP_TLS uint32_t g_thread_stack_top = 0;

typedef struct CheckpointRestoreBatch {
    HANDLE start;
    int commit;
} CheckpointRestoreBatch;
struct bridge_thread_start {
    recomp_func_t fn;
    uint32_t ctx1, ctx2, stack_top, tib;
    void *checkpoint_record;
    FILE *restore_packet;
    CheckpointRestoreBatch *restore_batch;
};
static void *checkpoint_new_thread(void);
static void checkpoint_bind_thread(void *);
static void checkpoint_set_thread_pcr(void *,uint32_t,DWORD);
static uint32_t ke_thread_for_handle(HANDLE);
static uint32_t ke_object_for_handle(HANDLE);
static uint32_t ke_current_thread(void);
static void ke_release_object_if_unused(uint32_t);
static int ke_register_thread_object(HANDLE,uint32_t,uint32_t);
static NTSTATUS ke_reference_handle(HANDLE,uint32_t,uint32_t *);
static int io_is_guest_file(uint32_t);
static NTSTATUS ke_reference_pointer(uint32_t,uint32_t);
static void ke_dereference_object(uint32_t);
static NTSTATUS ke_create_timer_object(PHANDLE,uint32_t);
static void bridge_create_dispatcher_object(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);

static NTSTATUS ke_open_named_object(PXBOX_OBJECT_ATTRIBUTES,uint32_t,PHANDLE);
static NTSTATUS bridge_read_object_attributes(uint32_t,XBOX_OBJECT_ATTRIBUTES *,XBOX_ANSI_STRING *);
static void ke_unlink_unused_name(uint32_t);
static void ke_cancel_timer_apc(uint32_t);
static void ke_timer_apc_kernel(uint32_t,uint32_t);
static void ke_rundown_thread_timers(uint32_t);
static void ke_rundown_current_thread(void);
static void bridge_finish_thread(void);
static recomp_func_t checkpoint_lookup_root(uint32_t);
static DWORD kernel_wait_native(ULONG,const HANDLE *,KPROCESSOR_MODE,BOOLEAN,DWORD);
static void checkpoint_terminate_thread(void);
static void checkpoint_drop_thread(void);
static void checkpoint_discard_thread(void *);
static void checkpoint_poll(void);
static int checkpoint_restore_thread(struct bridge_thread_start *);
static int checkpoint_root_cancelled(void);
static int guest_cpu_enabled(void);

static void bridge_write_handle(uint32_t handle_va, HANDLE h);
static uint32_t ke_completion_for_handle(HANDLE handle);
static void ke_maybe_release_queue(uint32_t queue);
static HANDLE bridge_resolve_handle(uint32_t token);
static void bridge_run_main_inline(recomp_func_t,uint32_t,uint32_t,uint32_t);
static int bridge_restore_main_inline(void);

static void bridge_run_thread_inline(recomp_func_t fn, uint32_t ctx1,
                                     uint32_t ctx2)
{
    g_esp -= 4; BRIDGE_MEM32(g_esp) = ctx2;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = ctx1;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
    g_seh_ebp = g_esp;
    xbox_RunGuestRoot(fn);
    g_esp += 12;
}

void guest_cpu_join(void);
void guest_cpu_part(void);
void guest_cpu_yield(void);

static DWORD WINAPI bridge_thread_main(LPVOID param)
{
    struct bridge_thread_start *s = (struct bridge_thread_start *)param;
    recomp_func_t fn = s->fn;
    uint32_t ctx1 = s->ctx1, ctx2 = s->ctx2;

    int restoring=s->restore_packet!=NULL;
    if (restoring) {
        if (!checkpoint_restore_thread(s)) { free(s);return 0; }
    } else {
    /* Own register set (RECOMP_TLS), own simulated stack -- and own TIB.
     *
     * The TIB carries the SEH chain head and, through fs:[4], the CRT's
     * per-thread data. Sharing one made "which thread am I" a single answer
     * for every thread, which is how two of them ended up inside _lock() each
     * holding the lock the other wanted. */
    g_is_spawned_thread = 1;
    g_esp = s->stack_top;
    g_thread_stack_top = s->stack_top;
    void *checkpoint_record=s->checkpoint_record;
    {
        xbox_IrqlBindThreadPcr(s->tib);
    }
    checkpoint_bind_thread(checkpoint_record);
    }
    free(s);

    guest_cpu_join();
    if (restoring) {
        xbox_RunGuestRoot(fn);
        if (g_is_spawned_thread && fn!=bridge_finish_thread) g_esp+=12;
    } else bridge_run_thread_inline(fn, ctx1, ctx2);
    uint32_t exit_status=g_eax;
    if(!checkpoint_root_cancelled() && !t_guest_terminated) {
        xbox_RunGuestRoot(bridge_finish_thread);exit_status=g_eax;
    }
    if(!checkpoint_root_cancelled() && g_is_spawned_thread)xbox_FreeThreadStack(g_thread_stack_top);
    g_thread_stack_top=0;g_eax=exit_status;
    checkpoint_drop_thread();
    if(!checkpoint_root_cancelled())ke_release_object_if_unused(ke_current_thread());
    guest_cpu_part();

    fprintf(stderr, "  [KERNEL] worker thread returned (eax=0x%08X)\n", g_eax);
    fflush(stderr);
    return g_eax;
}

static HANDLE bridge_spawn_thread(recomp_func_t fn, uint32_t ctx1,
                                  uint32_t ctx2, uint32_t stack_top)
{
    struct bridge_thread_start *s = malloc(sizeof(*s));
    HANDLE th;

    if (!s) return NULL;
    s->fn = fn; s->ctx1 = ctx1; s->ctx2 = ctx2; s->stack_top = stack_top;
    s->tib=xbox_AllocThreadTib();
    if(!s->tib){free(s);return NULL;}
    s->checkpoint_record=checkpoint_new_thread();
    checkpoint_set_thread_pcr(s->checkpoint_record,s->tib,0);
    s->restore_packet=NULL;s->restore_batch=NULL;

    DWORD host_id=0;
    th = CreateThread(NULL, 0, bridge_thread_main, s, CREATE_SUSPENDED, &host_id);
    if (!th) { xbox_HeapFree(s->tib);checkpoint_discard_thread(s->checkpoint_record);free(s); }
    else {
        checkpoint_set_thread_pcr(s->checkpoint_record,s->tib,host_id);
        if(!ke_register_thread_object(th,BRIDGE_MEM32(s->tib+0x28),s->tib))recomp_cont_invalid();
        ResumeThread(th);
    }
    /* Record the game thread so a host-tick-driven title's watchdog can sample
     * it via xbox_thread_debug_handle. Harmless for default-model titles: they
     * spawn workers too, but never read it back. See kernel_thread.c. */
    if (th) xbox_set_game_thread(th);
    return th;
}

/* Two ways a title expects its first PsCreateSystemThreadEx to behave.
 *
 * INLINE (default): the first call IS the game starting -- run the routine
 * inline, inheriting register state, and it drives its own main loop forever.
 * This is what Halo and Crimson Skies need and the historical behavior.
 *
 * SPAWN: the title's entry spawns an init thread and RETURNS, expecting the host
 * to drive the per-frame tick afterwards (Burnout 3 is tick-driven). Here the
 * first call must spawn a real thread and return, so control comes back to the
 * host. Opt in with xbox_SetThreadMode before the game starts. See
 * docs/technical/burnout3-reunification.md. */
/* XBOX_THREAD_MODE_* and xbox_SetThreadMode are declared in xbox_memory_layout.h. */
static int g_thread_mode = XBOX_THREAD_MODE_INLINE;
void xbox_SetThreadMode(int mode) { g_thread_mode = mode; }
void xbox_CaptureThreadCreationState(XboxThreadCreationState *state) {
    state->call_count=g_thread_call_count;state->mode=g_thread_mode;
}
int xbox_RestoreThreadCreationState(const XboxThreadCreationState *state) {
    if (!state || state->call_count<0 ||
        (state->mode!=XBOX_THREAD_MODE_INLINE && state->mode!=XBOX_THREAD_MODE_SPAWN)) return 0;
    g_thread_call_count=state->call_count;g_thread_mode=state->mode;return 1;
}

static void bridge_PsCreateSystemThreadEx(void)
{
    if (bridge_restore_main_inline()) goto creation_completed;
    uint32_t xbox_handle_ptr = STACK_ARG(0);
    uint32_t start_context1  = STACK_ARG(5);
    uint32_t start_context2  = STACK_ARG(6);
    uint32_t start_routine   = STACK_ARG(9);
    /* In SPAWN mode there is no privileged "first call": every thread is real,
     * so the entry can return. In INLINE mode the first call runs the game. */
    int is_first_call = (g_thread_mode == XBOX_THREAD_MODE_INLINE)
                        && (g_thread_call_count == 0);
    g_thread_call_count++;

    fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx #%d: routine=0x%08X ctx1=0x%08X ctx2=0x%08X\n",
            g_thread_call_count, start_routine, start_context1, start_context2);
    fflush(stderr);

    /* Write a fake handle to the output pointer */
    if (xbox_handle_ptr) {
        BRIDGE_MEM32(xbox_handle_ptr) = 0xBEEF0001;  /* fake handle */
    }

    /* Call the start routine synchronously through the recomp dispatch.
     * Xbox thread start routines receive two parameters:
     *   void ThreadRoutine(PVOID StartContext1, PVOID StartContext2)
     * We push both onto the simulated stack (right-to-left).
     *
     * First call: the game's main thread entry point. Must run synchronously
     * and inherit the current register state (this IS the game starting).
     *
     * Subsequent calls: worker threads. Must save/restore ALL global registers
     * because on real Xbox each thread has its own register set. Without this,
     * the worker clobbers the caller's g_esi, g_ebx, etc. */
    if (start_routine) {
        recomp_func_t fn = recomp_lookup(start_routine);
        if (!fn) fn = recomp_lookup_manual(start_routine);
        if (fn) {
            if (is_first_call) {
                /* Main game thread: run directly, inheriting register state */
                bridge_run_main_inline(fn,start_routine,start_context1,start_context2);
            } else {
                /* Worker thread: a real one.
                 *
                 * This used to run the routine synchronously and restore the
                 * caller's registers afterwards, which is fine only for a
                 * worker that finishes. Halo's cache/file worker does not -- it
                 * blocks on an event waiting for requests, so CreateThread
                 * never returned and startup deadlocked before the main loop.
                 *
                 * Now that the register set is thread-local (RECOMP_TLS), a
                 * spawned thread gets its own, and the caller's is untouched by
                 * construction rather than by save/restore. */
                /* RECOMP_WORKERS=inline runs a title's worker routines on
                 * the calling thread instead of spawning one.
                 *
                 * Not a mode to ship a title in -- a worker that blocks
                 * waiting for requests never returns, and the caller never
                 * gets control back. It is a bisecting tool: when something
                 * only goes wrong with two guest threads running, this says so
                 * in one run, and the answer separates a concurrency bug from
                 * everything else it might have been. */
                const char *inline_workers = getenv("RECOMP_WORKERS");
                uint32_t stack_top;

                if (inline_workers && !strcmp(inline_workers, "inline")) {
                    fprintf(stderr, "  [KERNEL] RECOMP_WORKERS=inline: running "
                            "worker 0x%08X on this thread\n", start_routine);
                    fflush(stderr);
                    bridge_run_thread_inline(fn, start_context1, start_context2);
                    return;
                }

                stack_top = xbox_AllocThreadStack();

                if (!stack_top) {
                    fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: out of "
                            "thread stacks, running worker 0x%08X inline\n",
                            start_routine);
                    fflush(stderr);
                    bridge_run_thread_inline(fn, start_context1, start_context2);
                } else {
                    HANDLE th = bridge_spawn_thread(fn, start_context1,
                                                    start_context2, stack_top);
                    fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: spawned "
                            "worker 0x%08X (ctx=0x%08X, stack top 0x%08X)\n",
                            start_routine, start_context1, stack_top);
                    fflush(stderr);
                    if (xbox_handle_ptr && th) {
                        bridge_write_handle(xbox_handle_ptr, th);
                    }
                }
            }
        } else {
            fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: start routine 0x%08X not found in dispatch!\n",
                    start_routine);
        }
    }

creation_completed: ;
    g_eax = 0; /* STATUS_SUCCESS */
}

/* ── NtClose (ordinal 187) ───────────────────────────────
 * NTSTATUS NtClose(HANDLE Handle)
 * Handle is a value (not a pointer), so safe for generic call.
 */

/* Asynchronous-handle bookkeeping, defined with the file bridges below. */
static void bridge_note_async_handle(uint32_t token);
static void bridge_forget_async_handle(uint32_t token);
static int  bridge_handle_is_async(uint32_t token);

/* Handle-table helpers; defined further below. Xbox memory slots are 32-bit
 * but native HANDLEs are 64-bit pointers, so handles are kept in a table and
 * referenced by tagged 32-bit tokens. */
static void   bridge_write_handle(uint32_t handle_va, HANDLE h);
static HANDLE bridge_take_handle(uint32_t token);

static void bridge_NtClose(void)
{
    uint32_t raw_handle = STACK_ARG(0);

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] NtClose: handle=0x%08X\n", raw_handle);
        fflush(stderr);
    }

    HANDLE h=bridge_resolve_handle(raw_handle);
    g_eax=(uint32_t)xbox_NtClose(h);
#ifndef _WIN32
    if((NTSTATUS)g_eax>=0) {
        bridge_take_handle(raw_handle);bridge_forget_async_handle(raw_handle);
    }
#endif
}

/* ── MmAllocateContiguousMemory (ordinal 165) ─────────────
 * PVOID MmAllocateContiguousMemory(ULONG NumberOfBytes)
 */
static void bridge_MmAllocateContiguousMemory(void)
{
    uint32_t size = STACK_ARG(0);

    /* From the contiguous window, not the general heap: the caller is
     * entitled to assume (VA & 0x0FFFFFFF) | 0x80000000 == VA, because that
     * is how a driver converts between the address it holds and the one it
     * hands the hardware. See xbox_ContiguousAlloc. */
    uint32_t xbox_va = xbox_ContiguousAlloc(size, 4096);

    if (KERNEL_LOG_ON_HALF()) {
        fprintf(stderr, "  [KERNEL] MmAllocateContiguousMemory: size=%u → Xbox VA 0x%08X\n",
                size, xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

/* ── MmAllocateSystemMemory (ordinal 167) ─────────────────
 * PVOID MmAllocateSystemMemory(ULONG NumberOfBytes, ULONG Protect)
 *
 * The kernel's own page allocator: whole pages, not the title's heap, and not
 * required to be physically contiguous. Titles use it for large buffers they
 * intend to keep -- Half-Life 2 takes one while loading a level, right after
 * it opens the .bsp.
 *
 * Unbridged this returned 0, and a NULL from an allocator is not a failure the
 * caller checks for here; it carried the pointer into graphics setup and hung
 * touching the NV2A aperture with it. Page-aligned out of the ordinary heap is
 * the right answer: the distinction the console draws between system memory
 * and the title heap is about which pool the pages come from, and there is one
 * pool here.
 *
 * Zeroed, because the console hands out zeroed pages and callers assume it.
 */
static void bridge_MmAllocateSystemMemory(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t prot = STACK_ARG(1);
    uint32_t xbox_va;

    (void)prot;
    if (!size) {
        g_eax = 0;
        return;
    }
    xbox_va = xbox_HeapAlloc(size, 4096);
    if (xbox_va)
        memset((void *)((uintptr_t)xbox_va + g_xbox_mem_offset), 0, size);
    else
        fprintf(stderr, "  [KERNEL] MmAllocateSystemMemory: %u bytes REFUSED\n",
                size);
    g_eax = xbox_va;
}

/* ── MmAllocateContiguousMemoryEx (ordinal 166) ───────────
 * PVOID MmAllocateContiguousMemoryEx(SIZE_T size, ULONG_PTR low, ULONG_PTR high,
 *                                     ULONG alignment, ULONG protect)
 */
/* Contiguous memory is addressed through the physical-memory mirror: physical
 * page P is visible at 0x80000000 + P. Titles that pin buffers at fixed
 * physical addresses check the returned pointer against that, so the address
 * has to be honoured rather than satisfied from the general heap. */
#define XBOX_PHYSICAL_MIRROR_BASE 0x80000000u

static void bridge_MmAllocateContiguousMemoryEx(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t low = STACK_ARG(1);
    uint32_t high = STACK_ARG(2);
    uint32_t align = STACK_ARG(3);
    uint32_t prot = STACK_ARG(4);
    uint32_t xbox_va;

    (void)prot;

    /*
     * A caller that constrains the range to exactly one allocation's worth is
     * demanding a specific physical address, not expressing a preference.
     * Halo does this for its two big pools and asserts on the result
     * (physical_memory_map.c:46) - XPhysicalAlloc passes lowest = the address
     * it wants and highest = lowest + size - 1, then requires
     * 0x80000000 | lowest back. Satisfying that from the heap fails the assert
     * and leaves its whole memory map wrong.
     */
    if (low && high >= low && (high - low + 1) <= size + 0x1000) {
        xbox_va = XBOX_PHYSICAL_MIRROR_BASE + low;

        /* The console hands out zeroed pages here, and titles rely on it:
         * pool headers and free-list roots are assumed clear, so whatever the
         * backing view happened to contain shows up later as structures that
         * are "allocated" but full of garbage. */
        memset((void *)((uintptr_t)xbox_va + g_xbox_mem_offset), 0, size);

        if (KERNEL_LOG_ON_HALF()) {
            fprintf(stderr, "  [KERNEL] MmAllocateContiguousMemoryEx: size=%u "
                    "pinned phys 0x%08X -> Xbox VA 0x%08X (zeroed)\n",
                    size, low, xbox_va);
            fflush(stderr);
        }
        g_eax = xbox_va;
        return;
    }

    if (align < 4096) align = 4096;
    xbox_va = xbox_ContiguousAlloc(size, align);

    if (KERNEL_LOG_ON_HALF()) {
        fprintf(stderr, "  [KERNEL] MmAllocateContiguousMemoryEx: size=%u align=%u → Xbox VA 0x%08X\n",
                size, align, xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

/* ── MmFreeContiguousMemory (ordinal 171) ─────────────────
 * VOID MmFreeContiguousMemory(PVOID BaseAddress)
 */
static void bridge_MmFreeContiguousMemory(void)
{
    extern int xbox_ContiguousFree(uint32_t addr);
    uint32_t addr = STACK_ARG(0);
    /* Contiguous blocks come from their own arena, not the heap: passing
     * them to xbox_HeapFree found nothing, so none ever came back. */
    if (!xbox_ContiguousFree(addr))
        xbox_HeapFree(addr);
    g_eax = 0;
}

/* ── NtAllocateVirtualMemory (ordinal 184) ────────────────
 * NTSTATUS NtAllocateVirtualMemory(PVOID *BaseAddress, ULONG ZeroBits,
 *     PULONG AllocationSize, ULONG AllocationType, ULONG Protect)
 */
static void bridge_NtAllocateVirtualMemory(void)
{
    uint32_t base_ptr = STACK_ARG(0);  /* PVOID* in Xbox VA */
    uint32_t zero_bits = STACK_ARG(1);
    uint32_t size_ptr = STACK_ARG(2);  /* PULONG in Xbox VA */
    uint32_t alloc_type = STACK_ARG(3);
    uint32_t protect = STACK_ARG(4);

    /* Read the requested size from Xbox memory */
    uint32_t size = size_ptr ? BRIDGE_MEM32(size_ptr) : 0;
    /* Read the base address hint (0 = let kernel choose) */
    uint32_t base_hint = base_ptr ? BRIDGE_MEM32(base_ptr) : 0;

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] NtAllocateVirtualMemory: base=0x%08X size=%u type=0x%X prot=0x%X\n",
                base_hint, size, alloc_type, protect);
        fflush(stderr);
    }

    /* RECOMP_EXT_VMA: a request for a specific address above the RAM mirrors
     * gets exactly that address, or a failure the caller can act on. The heap
     * path below never honours a base; it substitutes whatever its cursor is
     * at, which breaks any title that checks what it got against what it asked
     * for. Everything else (base = 0, or an address in mapped RAM) carries on
     * below. */
    if (base_ptr && size_ptr) {
        uint32_t vm_base = base_hint;
        uint32_t vm_size = size;
        uint32_t vm_status;

        if (guest_vmem_allocate(&vm_base, &vm_size, alloc_type, protect, &vm_status)) {
            if (KERNEL_LOG_ON()) {
                fprintf(stderr, "  [KERNEL] NtAllocateVirtualMemory (extended VMA): "
                                "base=0x%08X size=%u status=0x%08X\n",
                        vm_base, vm_size, vm_status);
                fflush(stderr);
            }
            if (vm_status == 0) {
                BRIDGE_MEM32(base_ptr) = vm_base;
                BRIDGE_MEM32(size_ptr) = vm_size;
            }
            g_eax = vm_status;
            return;
        }
    }

    if (size == 0) {
        g_eax = 0xC0000045u; /* STATUS_INVALID_PAGE_PROTECTION */
        return;
    }

    /*
     * Xbox NtAllocateVirtualMemory supports two modes:
     * - MEM_RESERVE (0x2000): Reserve virtual address space
     * - MEM_COMMIT  (0x1000): Commit pages within a reserved region
     * - MEM_RESERVE|MEM_COMMIT (0x3000): Both in one call
     *
     * Our Xbox heap (bump allocator) always commits memory immediately,
     * so MEM_COMMIT on an already-reserved region is a no-op.
     * Only allocate new memory when MEM_RESERVE is requested.
     */
    /* An address above physical RAM is not free memory -- it aliases.
     *
     * The runtime maps 64 MB and then mirrors it at 64 MB intervals,
     * because real Xbox RAM wraps on a 26-bit address bus. So a guest that
     * sub-allocates past the top of RAM does not get fresh pages, it gets
     * low memory that something else already owns, and the two quietly
     * share storage. Half-Life 2 put a CUtlRBTree element array at
     * 0x0CB80000, which aliases 0x00B80000; other regions it took land
     * inside the live heap (0x05B80000 -> 0x01B80000).
     *
     * Real hardware wraps *physical* addresses while translating virtual
     * ones, so this never happens there. Modelling every guest address as
     * physical is the gap, and that is a bigger change than a bridge fix.
     * Until then, say so: silent aliasing surfaces as corrupted data
     * structures far from here, which is the worst way to find it.
     */
    if (base_hint >= (g_xbox_map_size ? g_xbox_map_size : g_xbox_total_ram)) {
        static unsigned warned;
        if (warned++ < 8)
            fprintf(stderr,
                    "  [KERNEL] WARNING: allocation at 0x%08X is above "
                    "%u MB mapped; it aliases 0x%08X\n",
                    base_hint,
                    (unsigned)((g_xbox_map_size ? g_xbox_map_size
                                                : g_xbox_total_ram)
                               / (1024 * 1024)),
                    (uint32_t)(base_hint % (g_xbox_map_size
                                            ? g_xbox_map_size
                                            : g_xbox_total_ram)));
        fflush(stderr);
    }

    if (base_hint != 0 && (alloc_type & 0x2000) == 0) {
        /* MEM_COMMIT only, on an already-reserved region.
         * The memory is already committed by our bump allocator.
         * Don't change the base address - just return success. */
        if (KERNEL_LOG_ON()) {
            fprintf(stderr, "  [KERNEL] → MEM_COMMIT on existing region 0x%08X, no-op\n", base_hint);
            fflush(stderr);
        }
        g_eax = 0; /* STATUS_SUCCESS */
        return;
    }

    /* Allocate from Xbox heap (MEM_RESERVE or MEM_RESERVE|MEM_COMMIT).
     *
     * A pure MEM_RESERVE costs no RAM on real hardware -- it takes address
     * space out of a 4 GB range, not pages out of the 64 MB of memory -- so
     * titles reserve far more than the console physically has and commit a
     * fraction of it. Our heap is a bump allocator that commits everything it
     * hands out, so a large reserve asks for RAM that does not exist.
     *
     * Half-Life 2's XBE header sets PeHeapReserve to 128 MB, and its CRT
     * reserves exactly that during RtlCreateHeap. Failing it returned
     * STATUS_NO_MEMORY, RtlCreateHeap returned 0, and CRT init aborted before
     * main -- on a console with 64 MB, asking for 128 MB is normal, not an
     * error.
     *
     * So a reserve that does not fit is clamped to what the heap can actually
     * back, and the caller is told the real size through the IN/OUT RegionSize
     * parameter, which is where the API already reports the rounded figure.
     *
     * ponytail: the honest fix is a reserve that costs nothing and a commit
     * that backs pages on demand, which needs the allocator to separate the
     * two. This clamp is enough for a title that reserves generously and
     * commits little, and it fails loudly and later rather than silently and
     * at startup if one does not. */
    uint32_t xbox_va = xbox_HeapAlloc(size, 4096);
    if (!xbox_va && (alloc_type & 0x2000) && !(alloc_type & 0x1000)) {
        /* A pure reservation too big for the heap. Take it from the mapped
         * space above RAM, where it costs no heap and the pages are distinct.
         *
         * Clamping instead -- handing back a fraction of what was asked for --
         * is what broke Half-Life 2. It reserves 128 MB and then 200 MB, got
         * 32 MB and 12.5 MB, and then sub-allocated across the range it
         * believed it owned. That walks past the top of RAM, where the mirrors
         * alias low memory, so its containers quietly shared storage with the
         * live heap. Granting the full range is both more honest and less
         * damaging. Returns 0 unless the title asked for a mapping larger than
         * RAM, so nothing changes for titles that did not. */
        xbox_va = xbox_ReserveAlloc(size, 4096);
        if (xbox_va) {
            fprintf(stderr, "  [KERNEL] NtAllocateVirtualMemory: reserve of %u"
                            " granted at 0x%08X above RAM\n", size, xbox_va);
            fflush(stderr);
        }
    }
    if (!xbox_va && (alloc_type & 0x2000) && !(alloc_type & 0x1000)) {
        uint32_t want = size;
        while (want > 0x10000 && !xbox_va) {
            want /= 2;
            xbox_va = xbox_HeapAlloc(want, 4096);
        }
        if (xbox_va) {
            fprintf(stderr, "  [KERNEL] NtAllocateVirtualMemory: reserve of %u "
                            "clamped to %u (heap cannot back the full range)\n",
                    size, want);
            fflush(stderr);
            size = want;
        }
    }
    if (!xbox_va) {
        g_eax = 0xC0000017u; /* STATUS_NO_MEMORY */
        return;
    }

    /* Write back the allocated address and actual size */
    if (base_ptr) BRIDGE_MEM32(base_ptr) = xbox_va;
    if (size_ptr) BRIDGE_MEM32(size_ptr) = size;

    g_eax = 0; /* STATUS_SUCCESS */
}

/* ── NtFreeVirtualMemory (ordinal 199) ────────────────────
 * NTSTATUS NtFreeVirtualMemory(PVOID *BaseAddress, PULONG FreeSize,
 *     ULONG FreeType)
 */
/* -- NtQueryVirtualMemory (ordinal 217, 2 args = 8 bytes) --------------
 *
 * Xbox takes two arguments, not NT's four:
 *
 *     NTSTATUS NtQueryVirtualMemory(PVOID BaseAddress,
 *                                   PMEMORY_BASIC_INFORMATION Info);
 *
 * This has to be a guest-side answer. The existing xbox_NtQueryVirtualMemory
 * in kernel_memory.c calls the host VirtualQuery and memcpy's a host
 * MEMORY_BASIC_INFORMATION into guest memory, whose pointer fields are 64-bit
 * on an x64 build -- so every field after BaseAddress lands in the wrong place.
 *
 * It also has to exist at all. Without a bridge entry the thunk is left
 * unbridged, and a title's CRT heap creation calls this to probe its heap
 * region: RtlCreateHeap does
 *
 *     call NtQueryVirtualMemory ; test eax,eax ; jl fail
 *     cmp  mbi.BaseAddress, requested ; jne fail
 *     cmp  mbi.State, MEM_FREE        ; je  fail
 *
 * and returns 0 on any of those. On Half-Life 2 that null heap propagated
 * silently through the rest of CRT init.
 *
 * Guest MEMORY_BASIC_INFORMATION, 32-bit, 28 bytes:
 *     +0x00 BaseAddress   +0x04 AllocationBase  +0x08 AllocationProtect
 *     +0x0C RegionSize    +0x10 State           +0x14 Protect
 *     +0x18 Type
 *
 * The guest is one flat committed mapping, so that is what we report: any
 * address inside it is MEM_COMMIT / PAGE_READWRITE / MEM_PRIVATE, and anything
 * outside is MEM_FREE rather than an error, which is the honest answer and the
 * one that lets a caller distinguish the two.
 */
static void bridge_NtQueryVirtualMemory(void)
{
    uint32_t base_va = STACK_ARG(0);
    uint32_t info_va = STACK_ARG(1);
    uint32_t page_base = base_va & ~0xFFFu;

    if (!info_va) {
        g_eax = 0xC000000Du;               /* STATUS_INVALID_PARAMETER */
        return;
    }

    /* Anything the extended-VMA tracker owns is answered from its own records.
     * The generic answer below calls everything above RAM free, so a caller
     * walking its address space for a free range would be handed one that is
     * live. */
    {
        uint32_t vm_info[7];

        if (guest_vmem_query(base_va, vm_info)) {
            BRIDGE_MEM32(info_va + 0x00) = vm_info[0]; /* BaseAddress */
            BRIDGE_MEM32(info_va + 0x04) = vm_info[1]; /* AllocationBase */
            BRIDGE_MEM32(info_va + 0x08) = vm_info[2]; /* AllocationProtect */
            BRIDGE_MEM32(info_va + 0x0C) = vm_info[3]; /* RegionSize */
            BRIDGE_MEM32(info_va + 0x10) = vm_info[4]; /* State */
            BRIDGE_MEM32(info_va + 0x14) = vm_info[5]; /* Protect */
            BRIDGE_MEM32(info_va + 0x18) = vm_info[6]; /* Type */
            g_eax = 0;
            return;
        }
    }

    BRIDGE_MEM32(info_va + 0x00) = page_base;          /* BaseAddress */
    BRIDGE_MEM32(info_va + 0x04) = page_base;          /* AllocationBase */
    BRIDGE_MEM32(info_va + 0x08) = 0x04;               /* PAGE_READWRITE */
    BRIDGE_MEM32(info_va + 0x14) = 0x04;               /* Protect */
    BRIDGE_MEM32(info_va + 0x18) = 0x20000;            /* MEM_PRIVATE */

    if (page_base >= g_xbox_code_lo && page_base < XBOX_TOTAL_RAM) {
        BRIDGE_MEM32(info_va + 0x0C) = XBOX_TOTAL_RAM - page_base; /* RegionSize */
        BRIDGE_MEM32(info_va + 0x10) = 0x1000;         /* MEM_COMMIT */
    } else {
        /* Free to the next boundary, not one page. A caller enumerating free
         * memory then steps across the range instead of crawling it 4 KB at a
         * time; past the user range that is 0x80000 queries per lap, and on the
         * title this came from the walk wrapped at 0xFFFFFFFF and never ended.
         * Only with RECOMP_EXT_VMA, since it changes what every query above
         * the image answers. */
        uint32_t region = 0x1000;

        if (guest_vmem_active()) {
            /* Only where nothing else owns the answer: below the image, and
             * above the tracker's range. */
            uint32_t end = 0;

            if (page_base < g_xbox_code_lo)
                end = g_xbox_code_lo;
            else if (page_base >= GUEST_VMEM_TOP)
                end = 0xFFFFFFFFu;
            if (end > page_base)
                region = end - page_base;
        }
        BRIDGE_MEM32(info_va + 0x0C) = region;
        BRIDGE_MEM32(info_va + 0x10) = 0x10000;        /* MEM_FREE */
        BRIDGE_MEM32(info_va + 0x08) = 0;
        BRIDGE_MEM32(info_va + 0x18) = 0;
    }

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] NtQueryVirtualMemory: base=0x%08X -> "
                        "state=0x%X size=%u\n", base_va,
                        BRIDGE_MEM32(info_va + 0x10),
                        BRIDGE_MEM32(info_va + 0x0C));
        fflush(stderr);
    }
    g_eax = 0;                                          /* STATUS_SUCCESS */
}

static void bridge_NtFreeVirtualMemory(void)
{
    uint32_t base_ptr = STACK_ARG(0);
    uint32_t size_ptr = STACK_ARG(1);
    uint32_t free_type = STACK_ARG(2);

    if (base_ptr && size_ptr) {
        uint32_t vm_base = BRIDGE_MEM32(base_ptr);
        uint32_t vm_size = BRIDGE_MEM32(size_ptr);
        uint32_t vm_status;

        if (guest_vmem_free(&vm_base, &vm_size, free_type, &vm_status)) {
            if (vm_status == 0) {
                BRIDGE_MEM32(base_ptr) = vm_base;
                BRIDGE_MEM32(size_ptr) = vm_size;
            }
            g_eax = vm_status;
            return;
        }
    }

    /* Memory NtAllocateVirtualMemory took from the guest heap. The call below
     * reads the 32-bit guest slots as host pointers and hands them to
     * VirtualFree, which fails, so none of it ever came back. Under
     * RECOMP_HEAP_RECLAIM a release returns the block to the heap and a
     * decommit zeroes the pages it names, since heap memory stays committed. */
    if (xbox_HeapReclaimEnabled() && base_ptr && size_ptr) {
        uint32_t vm_base = BRIDGE_MEM32(base_ptr);
        uint32_t left = xbox_HeapBlockSize(vm_base);

        if (left) {
            if (free_type & 0x8000) {              /* MEM_RELEASE */
                xbox_HeapFree(vm_base);
                BRIDGE_MEM32(size_ptr) = 0;
            } else {
                /* MEM_DECOMMIT keeps the block, but on the console the pages
                 * are gone and a later MEM_COMMIT (a no-op here) brings them
                 * back zeroed. Zero them now so it does. */
                uint32_t vm_size = BRIDGE_MEM32(size_ptr);
                uint32_t n = (vm_size && vm_size < left) ? vm_size : left;
                memset(XBOX_TO_NATIVE(vm_base), 0, n);
            }
            g_eax = 0;
            return;
        }
    }

    g_eax = (uint32_t)xbox_NtFreeVirtualMemory(
        XBOX_TO_NATIVE(base_ptr), XBOX_TO_NATIVE(size_ptr), free_type);
}

/* ── ExAllocatePool / ExAllocatePoolWithTag (ordinals 15, 16) ─
 * Must allocate from Xbox heap so the returned pointer is an Xbox VA
 * that can be accessed via MEM32(). Native HeapAlloc returns 64-bit
 * pointers that get truncated and produce garbage Xbox VAs.
 */
/* ExQueryNonVolatileSetting(ValueIndex, Type, Value, ValueLength, ResultLength)
 *
 * Titles read region, language and AV settings from EEPROM through this very
 * early in boot. Ordinal 24 was previously routed to bridge_ExQueryPoolBlockSize,
 * so the call returned a pool size where the game expected a settings blob. */
/* ── FscGetCacheSize (35) / FscSetCacheSize (37) ───────────
 *
 * The Xbox filesystem cache, sized in 4 KB pages. A title that streams from a
 * pack file resizes it around the work: Half-Life 2's pack scanner
 * (sub_0041E650) reads the current size, sets 1 MB for the scan, and puts the
 * old value back when it is done.
 *
 * There is no cache here -- reads go to the host filesystem, which has its
 * own -- so the size is only ever a number the title stores and restores.
 * Keeping it is still worth doing: unbridged, the getter returned 0, and a
 * title that saves that and restores it later is restoring a cache size of
 * zero pages. 64 KB is the console's own default.
 */
#define XBOX_FSCACHE_DEFAULT_PAGES 16u      /* 64 KB in 4 KB pages */

static uint32_t g_fscache_pages = XBOX_FSCACHE_DEFAULT_PAGES;

static void bridge_FscGetCacheSize(void)
{
    g_eax = g_fscache_pages;
}

static void bridge_FscSetCacheSize(void)
{
    uint32_t pages = STACK_ARG(0);

    /* The kernel rejects a request it cannot satisfy and leaves the current
     * size alone; the caller checks for a negative status. Nothing here can
     * fail, so accept it and remember what was asked for. */
    g_fscache_pages = pages;
    g_eax = STATUS_SUCCESS;
}

/* ── RtlCompareMemory (268) / RtlCompareMemoryUlong (269) ──
 *
 * Both answer "how far do these match", counted in bytes from the start, and
 * both are used to decide whether a buffer needs work rather than to do it.
 * Unbridged they returned 0, which reads as "differs at the first byte" -- the
 * safe-looking answer that is wrong whenever the caller is checking for a
 * region it can skip.
 *
 * RtlCompareMemoryUlong compares against a repeating ULONG and only ever
 * examines whole ULONGs, so a length that is not a multiple of four leaves the
 * remainder uncompared; the count it returns is still in bytes.
 */
static void bridge_RtlCompareMemory(void)
{
    uint32_t a_va   = STACK_ARG(0);
    uint32_t b_va   = STACK_ARG(1);
    uint32_t length = STACK_ARG(2);
    const uint8_t *a, *b;
    uint32_t i;

    if (!a_va || !b_va || !length) {
        g_eax = 0;
        return;
    }
    a = (const uint8_t *)XBOX_TO_NATIVE(a_va);
    b = (const uint8_t *)XBOX_TO_NATIVE(b_va);
    for (i = 0; i < length; i++) {
        if (a[i] != b[i])
            break;
    }
    g_eax = i;
}

static void bridge_RtlCompareMemoryUlong(void)
{
    uint32_t base_va = STACK_ARG(0);
    uint32_t length  = STACK_ARG(1);
    uint32_t pattern = STACK_ARG(2);
    uint32_t i;

    if (!base_va) {
        g_eax = 0;
        return;
    }
    length &= ~3u;                      /* whole ULONGs only */
    for (i = 0; i < length; i += 4) {
        if (BRIDGE_MEM32(base_va + i) != pattern)
            break;
    }
    g_eax = i;
}

static void bridge_ExQueryNonVolatileSetting(void)
{
    uint32_t value_index  = STACK_ARG(0);
    uint32_t type_va      = STACK_ARG(1);
    uint32_t value_va     = STACK_ARG(2);
    uint32_t value_length = STACK_ARG(3);
    uint32_t result_va    = STACK_ARG(4);

    NTSTATUS st = xbox_ExQueryNonVolatileSetting(
        value_index,
        type_va   ? (PULONG)&BRIDGE_MEM32(type_va)   : NULL,
        value_va  ? (PVOID)((uintptr_t)value_va + g_xbox_mem_offset) : NULL,
        value_length,
        result_va ? (PULONG)&BRIDGE_MEM32(result_va) : NULL);

    g_eax = (uint32_t)st;
}

/* HalReturnToFirmware(Routine) - the title asking to reboot or quit.
 *
 * It never returns on hardware. Returning here would let the game run on past
 * a decision to quit, which reads as a hang rather than an exit. */
static void bridge_HalReturnToFirmware(void)
{
    if(recomp_cont_recovering(KERNEL_CONT_SHUTDOWN)){xbox_HalReturnToFirmware(2);return;}
    uint32_t routine = STACK_ARG(0);

    /* Routine 2 is a quick reboot, which on Xbox is how a title hands off to
     * another image: XLaunchNewImage fills the launch data page and reboots.
     * So "the title is exiting" and "the title is launching something" look
     * identical here, and the launch page is what tells them apart. */
    {
        uint32_t page = BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE);

        if (page) {
            char path[64];
            uint32_t i;

            for (i = 0; i < sizeof(path) - 1; i++) {
                uint8_t c = BRIDGE_MEM8(page + 8 + i);
                if (!c) break;
                path[i] = (char)c;
            }
            path[i] = 0;
            fprintf(stderr, "  [KERNEL] launch data page 0x%08X:"
                            " type=%u titleid=0x%08X path='%s'\n",
                    page, BRIDGE_MEM32(page), BRIDGE_MEM32(page + 4), path);
            /* XapiBootToDash packs its reason and two parameters into the
             * front of the launch data, so this says why the title asked to
             * leave rather than merely that it did. */
            fprintf(stderr, "  [KERNEL]   launch data:");
            for (i = 0; i < 8; i++)
                fprintf(stderr, " %08X", BRIDGE_MEM32(page + 1024 + i * 4));
            fprintf(stderr, "\n");
        } else {
            fprintf(stderr, "  [KERNEL] no launch data page set\n");
        }
    }

    /* Who asked to quit.
     *
     * A title exiting looks identical whether it finished cleanly, hit an
     * error path, or was told to reboot -- and the routine number does not say
     * which. The guest call chain does. Same GS format tools/stackwalk.py
     * reads. */
    {
        const uint8_t *mem = (const uint8_t *)g_xbox_mem_offset;
        uint32_t i;

        fprintf(stderr, "  [KERNEL] exit requested, guest esp=0x%08X:\n", g_esp);
        for (i = 0; i < 1024; i++) {
            uint32_t a = g_esp + i * 4;
            if (a < 0x00010000u || a >= 0x04000000u) break;
            fprintf(stderr, "    GS %08X %08X\n", a,
                    *(const uint32_t *)(mem + a));
        }
        fflush(stderr);
    }

    xbox_PeekSample("exit peek");
    fprintf(stderr, "  [KERNEL] HalReturnToFirmware: routine=%u - title is exiting\n",
            routine);
    fflush(stderr);

    /* Write the indirect-branch targets before the process goes away. This
     * path ends in ExitProcess, which does not run atexit handlers, so the
     * host's registered dump never fires -- and a title that gives up during
     * boot is exactly the one whose targets are worth having. No-op unless
     * RECOMP_ICALL_FEEDBACK is on. */
    RECOMP_ICALL_FEEDBACK_DUMP();

    /* Let a host-played FMV finish before the process goes away.
     *
     * The title is not the one presenting it, so it has no reason to wait --
     * it opens the file, carries on, and quits, which would kill the video
     * thread part-way through a five-second clip. Waiting here is what makes
     * the clip actually watchable, and it costs nothing when no video is
     * playing. Bounded, so a stuck player cannot stop the process exiting. */
    {
        extern int xbox_VideoIsPlaying(void);
        int waited = 0;

        while (xbox_VideoIsPlaying() && waited < 60000) {
            Sleep(50);
            waited += 50;
        }
        if (waited)
            fprintf(stderr, "  [KERNEL] waited %dms for the video to finish\n",
                    waited);
    }

    xbox_HalReturnToFirmware(routine);
}

static void bridge_ExAllocatePool(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t xbox_va = xbox_HeapAlloc(size, 16);

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] ExAllocatePool: size=%u → Xbox VA 0x%08X\n",
                size, xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

static void bridge_ExAllocatePoolWithTag(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t tag = STACK_ARG(1);
    uint32_t xbox_va = xbox_HeapAlloc(size, 16);

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] ExAllocatePoolWithTag: size=%u tag='%c%c%c%c' → Xbox VA 0x%08X\n",
                size,
                (char)(tag & 0xFF), (char)((tag >> 8) & 0xFF),
                (char)((tag >> 16) & 0xFF), (char)((tag >> 24) & 0xFF),
                xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

/* ── KfRaiseIrql / KfLowerIrql (ordinals 160, 161) ────── */
static void bridge_KfRaiseIrql(void)
{
    uint32_t new_irql = g_ecx; /* fastcall: KIRQL is passed in CL */
    g_eax = (uint32_t)xbox_KfRaiseIrql((UCHAR)new_irql);
}

static void bridge_KfLowerIrql(void)
{
    uint32_t new_irql = g_ecx; /* fastcall: KIRQL is passed in CL */
    xbox_KfLowerIrql((UCHAR)new_irql);
    g_eax = 0;
}

/* ── KeRaiseIrqlToDpcLevel (ordinal 129) ─────────────────── */
static void bridge_KeRaiseIrqlToDpcLevel(void)
{
    g_eax = (uint32_t)xbox_KeRaiseIrqlToDpcLevel();
}

/* ── RtlInitializeCriticalSection / Enter / Leave (ordinals 291, 277, 294) ─ */
static void bridge_RtlInitializeCriticalSection(void)
{
    uint32_t cs_va = STACK_ARG(0);
    xbox_RtlInitializeCriticalSection(XBOX_TO_NATIVE(cs_va));
    g_eax = 0;
}

static void bridge_RtlEnterCriticalSection(void)
{
    uint32_t cs_va = STACK_ARG(0);
    xbox_RtlEnterCriticalSection(XBOX_TO_NATIVE(cs_va));
    g_eax = 0;
}

static void bridge_RtlLeaveCriticalSection(void)
{
    uint32_t cs_va = STACK_ARG(0);
    xbox_RtlLeaveCriticalSection(XBOX_TO_NATIVE(cs_va));
    g_eax = 0;
}

/* ── KeQueryPerformanceCounter / Frequency (ordinals 126, 127) ─ */
static void bridge_KeQueryPerformanceCounter(void)
{
    LARGE_INTEGER li = xbox_KeQueryPerformanceCounter();
    g_eax = (uint32_t)li.LowPart;
    g_edx = (uint32_t)li.HighPart;
}

static void bridge_KeQueryPerformanceFrequency(void)
{
    LARGE_INTEGER li = xbox_KeQueryPerformanceFrequency();
    g_eax = (uint32_t)li.LowPart;
    g_edx = (uint32_t)li.HighPart;
}

/* ── KeQuerySystemTime (ordinal 128) ─────────────────────── */
static void bridge_KeQuerySystemTime(void)
{
    uint32_t time_ptr = STACK_ARG(0);
    xbox_KeQuerySystemTime(XBOX_TO_NATIVE(time_ptr));
    g_eax = 0;
}

/* ── MmQueryStatistics (ordinal 181) ─────────────────────── */
static void bridge_MmQueryStatistics(void)
{
    uint32_t stats_ptr = STACK_ARG(0);
    g_eax = (uint32_t)xbox_MmQueryStatistics(XBOX_TO_NATIVE(stats_ptr));
}

/* ── NtCreateEvent (ordinal 189) ─────────────────────────── */
static void bridge_NtCreateEvent(void)
{
    bridge_create_dispatcher_object(189,STACK_ARG(0),STACK_ARG(1),STACK_ARG(2),STACK_ARG(3));
}

static HANDLE ke_shadow_lookup(uint32_t guest_va);
static uint32_t ke_completion_for_handle(HANDLE handle);
static void ke_mark_owned_queue(uint32_t queue);
static void ke_maybe_release_queue(uint32_t queue);
static uint32_t ke_completion_for_handle(HANDLE handle);
static void ke_maybe_release_queue(uint32_t queue);
static HANDLE bridge_resolve_handle(uint32_t token);
static int ke_shadow_insert(uint32_t guest_va, HANDLE host);
static HANDLE bridge_resolve_handle(uint32_t token);
static HANDLE ke_guest_event(uint32_t guest_va, int *type);
static void ke_shadow_remove(uint32_t guest_va);
enum { KE_EVENT_TITLE_HEADER = 2 };
static uint32_t ke_native_event_state(HANDLE event);
static void ke_prepare_event_wait(uint32_t object, HANDLE event, int type);
static void ke_finish_event_wait(uint32_t object, HANDLE event, int type);
static uint32_t ke_change_event(uint32_t object, HANDLE event, unsigned operation);


/* Xbox dispatcher Size is in 32-bit words; list links are guest addresses. */
static void ke_initialize_header(uint32_t va, uint8_t type, uint8_t words,
                                 uint32_t signal_state)
{
    BRIDGE_MEM8(va) = type;
    BRIDGE_MEM8(va + 1) = 0;
    BRIDGE_MEM8(va + 2) = words;
    BRIDGE_MEM8(va + 3) = 0;
    BRIDGE_MEM32(va + 4) = signal_state;
    BRIDGE_MEM32(va + 8) = va + 8;
    BRIDGE_MEM32(va + 12) = va + 8;
}

static int ke_initialize_event(uint32_t va, uint32_t type, uint32_t state)
{
    HANDLE event = CreateEventW(NULL, type == 0, state != 0, NULL);
    if (!event)
        return 0;
    if (!ke_shadow_insert(va, event))
        return 0;
    ke_initialize_header(va, (uint8_t)type, 4, state != 0);
    return 1;
}

static void ke_initialize_device_queue(uint32_t va)
{
    BRIDGE_MEM16(va) = 0x14; /* DeviceQueueObject */
    BRIDGE_MEM8(va + 2) = 12;
    BRIDGE_MEM8(va + 3) = 0;
    BRIDGE_MEM32(va + 4) = va + 4;
    BRIDGE_MEM32(va + 8) = va + 4;
}

/* The queue and entries live in guest RAM. Keep their original links as the
 * only queue state; the guest CPU lock serializes every import that mutates it. */
static int ke_insert_device_queue(uint32_t queue, uint32_t entry, int keyed, uint32_t key)
{
    if (keyed) BRIDGE_MEM32(entry + 8) = key;
    if (!BRIDGE_MEM8(queue + 3)) {
        BRIDGE_MEM8(queue + 3) = 1;
        BRIDGE_MEM8(entry + 12) = 0;
        return 0;
    }
    uint32_t head = queue + 4, next = head;
    if (keyed) {
        next = BRIDGE_MEM32(head);
        while (next != head && BRIDGE_MEM32(next + 8) <= key)
            next = BRIDGE_MEM32(next);
    }
    uint32_t previous = BRIDGE_MEM32(next + 4);
    BRIDGE_MEM32(entry) = next;
    BRIDGE_MEM32(entry + 4) = previous;
    BRIDGE_MEM32(previous) = entry;
    BRIDGE_MEM32(next + 4) = entry;
    BRIDGE_MEM8(entry + 12) = 1;
    return 1;
}

static int ke_remove_device_queue_entry(uint32_t entry)
{
    if (!BRIDGE_MEM8(entry + 12)) return 0;
    uint32_t next = BRIDGE_MEM32(entry), previous = BRIDGE_MEM32(entry + 4);
    BRIDGE_MEM32(previous) = next;
    BRIDGE_MEM32(next + 4) = previous;
    BRIDGE_MEM8(entry + 12) = 0;
    return 1;
}

static uint32_t ke_remove_device_queue(uint32_t queue, int keyed, uint32_t key)
{
    uint32_t head = queue + 4, entry = BRIDGE_MEM32(head);
    if (entry == head) {
        BRIDGE_MEM8(queue + 3) = 0;
        return 0;
    }
    if (keyed) {
        while (entry != head && BRIDGE_MEM32(entry + 8) < key)
            entry = BRIDGE_MEM32(entry);
        if (entry == head) entry = BRIDGE_MEM32(head);
    }
    ke_remove_device_queue_entry(entry);
    return entry;
}



/* APC lists and callback arguments live in the original guest thread/KAPC
 * structures, so the memory checkpoint retains ownership and ordering. */
#define KERNEL_IO_COMPLETE_APC (KERNEL_VA_END + 0u)
#define KERNEL_IO_FREE_APC     (KERNEL_VA_END + 4u)
#define KERNEL_POOL_FREE_APC   (KERNEL_VA_END + 8u)
#define KERNEL_TIMER_APC       (KERNEL_VA_END + 12u)
#define KERNEL_TIMER_DPC       (KERNEL_VA_END + 16u)
static void io_complete_apc(uint32_t apc, uint32_t file);
static uint32_t ke_insert_queue(uint32_t queue,uint32_t entry,int first);
static uint32_t ke_current_thread(void)
{
    return g_fs_base ? BRIDGE_MEM32(g_fs_base + 0x28) : 0;
}

static void ke_list_unlink(uint32_t entry)
{
    uint32_t next=BRIDGE_MEM32(entry),prev=BRIDGE_MEM32(entry+4);
    BRIDGE_MEM32(prev)=next;BRIDGE_MEM32(next+4)=prev;
}

static void ke_list_before(uint32_t before,uint32_t entry)
{
    uint32_t prev=BRIDGE_MEM32(before+4);
    BRIDGE_MEM32(entry)=before;BRIDGE_MEM32(entry+4)=prev;
    BRIDGE_MEM32(prev)=entry;BRIDGE_MEM32(before+4)=entry;
}

static void ke_initialize_apc(uint32_t apc,uint32_t thread,uint32_t kernel,
    uint32_t rundown,uint32_t normal,uint32_t mode,uint32_t context)
{
    BRIDGE_MEM16(apc)=0x12;BRIDGE_MEM8(apc+2)=normal?(uint8_t)mode:0;
    BRIDGE_MEM8(apc+3)=0;BRIDGE_MEM32(apc+4)=thread;
    BRIDGE_MEM32(apc+0x10)=kernel;BRIDGE_MEM32(apc+0x14)=rundown;
    BRIDGE_MEM32(apc+0x18)=normal;BRIDGE_MEM32(apc+0x1c)=normal?context:0;
}

static int ke_insert_apc(uint32_t apc,uint32_t arg1,uint32_t arg2)
{
    uint32_t thread=BRIDGE_MEM32(apc+4),mode=BRIDGE_MEM8(apc+2);
    if (!BRIDGE_MEM8(thread+0x4b)) return 0;
    /* Xbox updates these arguments even when the already-inserted check fails. */
    BRIDGE_MEM32(apc+0x20)=arg1;BRIDGE_MEM32(apc+0x24)=arg2;
    if (BRIDGE_MEM8(apc+3)) return 0;
    uint32_t head=thread+0x34+mode*8,before=head;
    if (!BRIDGE_MEM32(apc+0x18)) {
        before=BRIDGE_MEM32(head);
        while(before!=head && !BRIDGE_MEM32(before+0x10)) before=BRIDGE_MEM32(before);
    }
    ke_list_before(before,apc+8);BRIDGE_MEM8(apc+3)=1;
    if (!mode) BRIDGE_MEM8(thread+0x49)=1;
    return 1;
}

static int ke_remove_apc(uint32_t apc)
{
    if (!BRIDGE_MEM8(apc+3)) return 0;
    uint32_t thread=BRIDGE_MEM32(apc+4),mode=BRIDGE_MEM8(apc+2),head=thread+0x34+mode*8;
    ke_list_unlink(apc+8);BRIDGE_MEM8(apc+3)=0;
    if (BRIDGE_MEM32(head)==head) BRIDGE_MEM8(thread+0x49+mode)=0;
    return 1;
}

static uint64_t kernel_native_cont_enter(RecompContFrame *frame,uint32_t function,
                                        const RecompContLocal *locals,size_t count)
{
    RecompContFrame *parent=recomp_cont_current();
    if(parent && parent->function>=KERNEL_VA_BASE && parent->function<KERNEL_VA_END && !parent->site)
        recomp_cont_site(parent,(11ULL<<32)|parent->function);
    uint64_t resume=recomp_cont_enter(frame,function,locals,count);
    if(resume && resume!=((6ULL<<32)|function))recomp_cont_invalid();
    recomp_cont_site(frame,(6ULL<<32)|function);
    return resume;
}

static uint32_t ke_call_guest(uint32_t routine,const uint32_t *args,unsigned count,int restore_stack)
{
    uint32_t top=g_esp;RecompContFrame frame;
    const RecompContLocal locals[]={{"routine",&routine,sizeof routine},{"top",&top,sizeof top}};
    int tracked=recomp_cont_active();
    uint64_t resume=tracked?recomp_cont_enter(&frame,KERNEL_CONT_CALLBACK,locals,2):0;
    if(resume && resume!=((6ULL<<32)|KERNEL_CONT_CALLBACK))recomp_cont_invalid();
    recomp_func_t fn=recomp_lookup(routine);
    if (!fn) fn=recomp_lookup_manual(routine);
    if (!fn) fn=recomp_lookup_kernel(routine);
    if (!fn) {fprintf(stderr,"[STOP] kernel callback %08X is not translated\n",routine);exit(5);}
    if(!resume) {
        for(unsigned i=count;i;i--) {g_esp-=4;BRIDGE_MEM32(g_esp)=args[i-1];}
        g_esp-=4;BRIDGE_MEM32(g_esp)=0;
    }
    if(tracked)recomp_cont_site(&frame,(6ULL<<32)|KERNEL_CONT_CALLBACK);
    fn();
    if(restore_stack)g_esp=top; /* Original SEH trampoline restores its saved ESP. */
    if(g_esp!=top) {fprintf(stderr,"[STOP] kernel callback %08X changed stack %08X -> %08X\n",routine,top,g_esp);exit(5);}
    if(tracked)recomp_cont_leave(&frame);
    return g_eax;
}

int xbox_ThreadApcsEnabled(void)
{
    uint32_t thread=t_guest_thread?ke_current_thread():0;
    return thread && BRIDGE_MEM8(thread+0x4b);
}

#define GUEST_REGISTER_CONT_LOCALS(object,prefix) \
    {prefix ".eax",&object.eax,sizeof object.eax}, \
    {prefix ".ecx",&object.ecx,sizeof object.ecx}, \
    {prefix ".edx",&object.edx,sizeof object.edx}, \
    {prefix ".esp",&object.esp,sizeof object.esp}, \
    {prefix ".ebx",&object.ebx,sizeof object.ebx}, \
    {prefix ".esi",&object.esi,sizeof object.esi}, \
    {prefix ".edi",&object.edi,sizeof object.edi}, \
    {prefix ".ebp",&object.ebp,sizeof object.ebp}, \
    {prefix ".seh_ebp",&object.seh_ebp,sizeof object.seh_ebp}, \
    {prefix ".fs_base",&object.fs_base,sizeof object.fs_base}, \
    {prefix ".fp_stack[0]",&object.fp_stack[0],sizeof object.fp_stack[0]}, \
    {prefix ".fp_stack[1]",&object.fp_stack[1],sizeof object.fp_stack[1]}, \
    {prefix ".fp_stack[2]",&object.fp_stack[2],sizeof object.fp_stack[2]}, \
    {prefix ".fp_stack[3]",&object.fp_stack[3],sizeof object.fp_stack[3]}, \
    {prefix ".fp_stack[4]",&object.fp_stack[4],sizeof object.fp_stack[4]}, \
    {prefix ".fp_stack[5]",&object.fp_stack[5],sizeof object.fp_stack[5]}, \
    {prefix ".fp_stack[6]",&object.fp_stack[6],sizeof object.fp_stack[6]}, \
    {prefix ".fp_stack[7]",&object.fp_stack[7],sizeof object.fp_stack[7]}, \
    {prefix ".mm[0]",&object.mm[0],sizeof object.mm[0]}, \
    {prefix ".mm[1]",&object.mm[1],sizeof object.mm[1]}, \
    {prefix ".mm[2]",&object.mm[2],sizeof object.mm[2]}, \
    {prefix ".mm[3]",&object.mm[3],sizeof object.mm[3]}, \
    {prefix ".mm[4]",&object.mm[4],sizeof object.mm[4]}, \
    {prefix ".mm[5]",&object.mm[5],sizeof object.mm[5]}, \
    {prefix ".mm[6]",&object.mm[6],sizeof object.mm[6]}, \
    {prefix ".mm[7]",&object.mm[7],sizeof object.mm[7]}, \
    {prefix ".fp_top",&object.fp_top,sizeof object.fp_top}, \
    {prefix ".fp_cmp",&object.fp_cmp,sizeof object.fp_cmp}, \
    {prefix ".df",&object.df,sizeof object.df}, \
    {prefix ".fp_control_word",&object.fp_control_word,sizeof object.fp_control_word}, \
    {prefix ".fp_cc",&object.fp_cc,sizeof object.fp_cc}, \
    {prefix ".interrupt_state",&object.interrupt_state,sizeof object.interrupt_state}, \
    {prefix ".irql",&object.irql,sizeof object.irql}, \
    {prefix ".eflags",&object.eflags,sizeof object.eflags}, \
    {prefix ".xmm[0][0]",&object.xmm[0][0],8}, \
    {prefix ".xmm[0][8]",&object.xmm[0][8],8}, \
    {prefix ".xmm[1][0]",&object.xmm[1][0],8}, \
    {prefix ".xmm[1][8]",&object.xmm[1][8],8}, \
    {prefix ".xmm[2][0]",&object.xmm[2][0],8}, \
    {prefix ".xmm[2][8]",&object.xmm[2][8],8}, \
    {prefix ".xmm[3][0]",&object.xmm[3][0],8}, \
    {prefix ".xmm[3][8]",&object.xmm[3][8],8}, \
    {prefix ".xmm[4][0]",&object.xmm[4][0],8}, \
    {prefix ".xmm[4][8]",&object.xmm[4][8],8}, \
    {prefix ".xmm[5][0]",&object.xmm[5][0],8}, \
    {prefix ".xmm[5][8]",&object.xmm[5][8],8}, \
    {prefix ".xmm[6][0]",&object.xmm[6][0],8}, \
    {prefix ".xmm[6][8]",&object.xmm[6][8],8}, \
    {prefix ".xmm[7][0]",&object.xmm[7][0],8}, \
    {prefix ".xmm[7][8]",&object.xmm[7][8],8}, \
    {prefix ".fp_status_word",&object.fp_status_word,sizeof object.fp_status_word}, \
    {prefix ".fp_tag_word",&object.fp_tag_word,sizeof object.fp_tag_word}, \
    {prefix ".fp_opcode",&object.fp_opcode,sizeof object.fp_opcode}, \
    {prefix ".fp_code_selector",&object.fp_code_selector,sizeof object.fp_code_selector}, \
    {prefix ".fp_data_selector",&object.fp_data_selector,sizeof object.fp_data_selector}, \
    {prefix ".fp_instruction_pointer",&object.fp_instruction_pointer,sizeof object.fp_instruction_pointer}, \
    {prefix ".fp_data_pointer",&object.fp_data_pointer,sizeof object.fp_data_pointer}, \
    {prefix ".mxcsr",&object.mxcsr,sizeof object.mxcsr}, \
    {prefix ".fp_raw[0][0]",&object.fp_raw[0][0],8}, \
    {prefix ".fp_raw[0][8]",&object.fp_raw[0][8],2}, \
    {prefix ".fp_raw[1][0]",&object.fp_raw[1][0],8}, \
    {prefix ".fp_raw[1][8]",&object.fp_raw[1][8],2}, \
    {prefix ".fp_raw[2][0]",&object.fp_raw[2][0],8}, \
    {prefix ".fp_raw[2][8]",&object.fp_raw[2][8],2}, \
    {prefix ".fp_raw[3][0]",&object.fp_raw[3][0],8}, \
    {prefix ".fp_raw[3][8]",&object.fp_raw[3][8],2}, \
    {prefix ".fp_raw[4][0]",&object.fp_raw[4][0],8}, \
    {prefix ".fp_raw[4][8]",&object.fp_raw[4][8],2}, \
    {prefix ".fp_raw[5][0]",&object.fp_raw[5][0],8}, \
    {prefix ".fp_raw[5][8]",&object.fp_raw[5][8],2}, \
    {prefix ".fp_raw[6][0]",&object.fp_raw[6][0],8}, \
    {prefix ".fp_raw[6][8]",&object.fp_raw[6][8],2}, \
    {prefix ".fp_raw[7][0]",&object.fp_raw[7][0],8}, \
    {prefix ".fp_raw[7][8]",&object.fp_raw[7][8],2},

typedef struct GuestApcDelivery {
    RecompContFrame frame;
    XboxGuestRegisters saved;
    uint32_t thread,phase,mode,apc,kernel,scratch,old_irql;
    int held,was_thread,delivered,user_apcs;
} GuestApcDelivery;

static void ke_apc_delivery_leave(RecompContFrame *frame)
{
    GuestApcDelivery *state=(GuestApcDelivery *)frame;
    if(frame->retained)return;
    if(state->phase==2 && !state->mode)BRIDGE_MEM8(state->thread+0x48)=0;
    if(state->phase)xbox_KfLowerIrql((KIRQL)state->saved.irql);
    if(frame->parent && (frame->parent->site>>32)==10)
        recomp_cont_site(frame->parent,(2ULL<<32)|(uint32_t)frame->parent->site);
}

int xbox_DeliverThreadApcs(BOOLEAN user_apcs)
{
    int recovering=recomp_cont_recovering(KERNEL_CONT_APC);
    uint32_t thread=0;
    if(!recovering) {
        if(!xbox_ThreadApcsEnabled())return 0;
        if(xbox_CurrentIrql()>=APC_LEVEL || !xbox_CpuInterruptFlag())return 0;
        thread=ke_current_thread();
        if(!thread || !BRIDGE_MEM8(thread+0x4b))return 0;
        if(!BRIDGE_MEM8(thread+0x49) && (!user_apcs || BRIDGE_MEM32(thread+0x3c)==thread+0x3c))return 0;
    }
    GuestApcDelivery state={0};state.thread=thread;state.user_apcs=user_apcs;
    state.held=guest_cpu_is_held();state.was_thread=t_guest_thread;
    const RecompContLocal locals[]={
        GUEST_REGISTER_CONT_LOCALS(state.saved,"saved")
        {"thread",&state.thread,sizeof state.thread},
        {"phase",&state.phase,sizeof state.phase},
        {"mode",&state.mode,sizeof state.mode},
        {"apc",&state.apc,sizeof state.apc},
        {"kernel",&state.kernel,sizeof state.kernel},
        {"scratch",&state.scratch,sizeof state.scratch},
        {"old_irql",&state.old_irql,sizeof state.old_irql},
        {"held",&state.held,sizeof state.held},
        {"was_thread",&state.was_thread,sizeof state.was_thread},
        {"delivered",&state.delivered,sizeof state.delivered},
        {"user_apcs",&state.user_apcs,sizeof state.user_apcs},
    };
    int tracked=recomp_cont_active();
    if(tracked) {
        RecompContFrame *parent=recomp_cont_current();
        if((parent->site>>32)==2)recomp_cont_site(parent,(10ULL<<32)|(uint32_t)parent->site);
    }
    uint64_t resume=tracked?recomp_cont_enter(&state.frame,KERNEL_CONT_APC,locals,sizeof locals/sizeof locals[0]):0;
    if(tracked)state.frame.on_leave=ke_apc_delivery_leave;
    if(!guest_cpu_is_held())guest_cpu_join();
    if(resume) {
        if(resume!=((6ULL<<32)|KERNEL_CONT_APC))recomp_cont_invalid();
        if(state.phase==1)goto kernel_callback;
        if(state.phase==2)goto normal_callback;
        recomp_cont_invalid();
    }
    xbox_CaptureGuestRegisters(&state.saved);
    for(state.mode=0;state.mode<=(state.user_apcs?1u:0u);state.mode++) {
        uint32_t head=state.thread+0x34+state.mode*8;
        while(BRIDGE_MEM32(head)!=head) {
            state.apc=BRIDGE_MEM32(head)-8;
            uint32_t normal=BRIDGE_MEM32(state.apc+0x18);
            if(!state.mode && normal && (BRIDGE_MEM32(state.thread+0x68) || BRIDGE_MEM8(state.thread+0x48)))break;
            state.kernel=BRIDGE_MEM32(state.apc+0x10);state.scratch=g_esp-16;
            BRIDGE_MEM32(state.scratch)=normal;BRIDGE_MEM32(state.scratch+4)=BRIDGE_MEM32(state.apc+0x1c);
            BRIDGE_MEM32(state.scratch+8)=BRIDGE_MEM32(state.apc+0x20);BRIDGE_MEM32(state.scratch+12)=BRIDGE_MEM32(state.apc+0x24);
            ke_remove_apc(state.apc);g_esp=state.scratch;
            state.old_irql=xbox_KfRaiseIrql(APC_LEVEL);state.phase=1;
            if(tracked)recomp_cont_site(&state.frame,(6ULL<<32)|KERNEL_CONT_APC);
kernel_callback:
            if(state.kernel==KERNEL_IO_COMPLETE_APC)io_complete_apc(state.apc,BRIDGE_MEM32(state.scratch+8));
            else if(state.kernel==KERNEL_IO_FREE_APC)xbox_IoFreeIrp(XBOX_TO_NATIVE(state.apc-0x3c));
            else if(state.kernel==KERNEL_POOL_FREE_APC)xbox_HeapFree(state.apc);
            else if(state.kernel==KERNEL_TIMER_APC)ke_timer_apc_kernel(state.apc,state.scratch);
            else {
                uint32_t args[]={state.apc,state.scratch,state.scratch+4,state.scratch+8,state.scratch+12};
                ke_call_guest(state.kernel,args,5,0);
            }
            xbox_KfLowerIrql((KIRQL)state.old_irql);
            if(BRIDGE_MEM32(state.scratch)) {
                if(!state.mode)BRIDGE_MEM8(state.thread+0x48)=1;
                state.phase=2;
normal_callback: ;
                uint32_t args[]={BRIDGE_MEM32(state.scratch+4),BRIDGE_MEM32(state.scratch+8),BRIDGE_MEM32(state.scratch+12)};
                ke_call_guest(BRIDGE_MEM32(state.scratch),args,3,0);
                if(!state.mode)BRIDGE_MEM8(state.thread+0x48)=0;
            }
            state.phase=0;g_esp=state.scratch+16;if(state.mode)state.delivered=1;
            /* Recovery jumps over the loop's original native local setup. */
            head=state.thread+0x34+state.mode*8;
        }
    }
    xbox_RestoreGuestRegisters(&state.saved);
    if(tracked)recomp_cont_leave(&state.frame);
    if(!state.held){guest_cpu_part();t_guest_thread=state.was_thread;}
    return state.delivered;
}

/* Xbox IRP and IO_STACK_LOCATION are guest structures, including every
 * pointer. Native pointer-sized C structures cannot represent this ABI. */
enum { XBOX_IRP_BYTES = 0x68, XBOX_IO_STACK_BYTES = 0x24 };

static uint32_t io_guest_address(const void *pointer)
{
    return pointer ? (uint32_t)((uintptr_t)pointer - g_xbox_mem_offset) : 0;
}

VOID __stdcall xbox_IoInitializeIrp(PVOID Irp, USHORT PacketSize, CCHAR StackSize)
{
    uint32_t irp = io_guest_address(Irp);
    memset(Irp, 0, PacketSize);
    BRIDGE_MEM16(irp) = 6;
    BRIDGE_MEM16(irp + 2) = PacketSize;
    BRIDGE_MEM32(irp + 8) = irp + 8;
    BRIDGE_MEM32(irp + 12) = irp + 8;
    BRIDGE_MEM8(irp + 0x18) = StackSize;
    BRIDGE_MEM8(irp + 0x19) = StackSize + 1;
    BRIDGE_MEM32(irp + 0x5c) = irp + XBOX_IRP_BYTES + StackSize * XBOX_IO_STACK_BYTES;
}

PVOID __stdcall xbox_IoAllocateIrp(CCHAR StackSize)
{
    uint32_t bytes = XBOX_IRP_BYTES + (uint8_t)StackSize * XBOX_IO_STACK_BYTES;
    uint32_t irp = xbox_HeapAlloc(bytes, 16);
    if (!irp) return NULL;
    xbox_IoInitializeIrp(XBOX_TO_NATIVE(irp), (USHORT)bytes, StackSize);
    return XBOX_TO_NATIVE(irp);
}

VOID __stdcall xbox_IoFreeIrp(PVOID Irp)
{
    uint32_t irp = io_guest_address(Irp);
    if (BRIDGE_MEM16(irp) != 6) {
        fprintf(stderr,"[STOP] IoFreeIrp received an invalid IRP %08X\n",irp);
        exit(5);
    }
    BRIDGE_MEM16(irp) = 0;
    xbox_HeapFree(irp);
}

VOID __stdcall xbox_IoQueueThreadIrp(PVOID Irp)
{
    int held=guest_cpu_is_held();if(!held)guest_cpu_join();
    uint32_t irp = io_guest_address(Irp);
    uint32_t head = BRIDGE_MEM32(irp + 0x50) + 0x134;
    ke_list_before(BRIDGE_MEM32(head), irp + 8);
    if(!held)guest_cpu_part();
}

static void io_attach_request_thread(uint32_t irp, int link)
{
    BRIDGE_MEM32(irp + 0x50) = ke_current_thread();
    if (link) xbox_IoQueueThreadIrp(XBOX_TO_NATIVE(irp));
}

PVOID __stdcall xbox_IoBuildAsynchronousFsdRequest(ULONG MajorFunction,
    PVOID DeviceObject, PVOID Buffer, ULONG Length, PLARGE_INTEGER StartingOffset,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock)
{
    uint32_t device = io_guest_address(DeviceObject);
    PVOID packet = xbox_IoAllocateIrp(BRIDGE_MEM8(device + 0x1e));
    if (!packet) return NULL;
    uint32_t irp = io_guest_address(packet);
    uint32_t stack = BRIDGE_MEM32(irp + 0x5c) - XBOX_IO_STACK_BYTES;
    BRIDGE_MEM8(stack) = (uint8_t)MajorFunction;
    if (MajorFunction != 6 && MajorFunction != 12) {
        BRIDGE_MEM32(irp + 0x30) = io_guest_address(Buffer);
        BRIDGE_MEM32(stack + 4) = Length;
        BRIDGE_MEM32(stack + 0xc) = (uint32_t)StartingOffset->QuadPart;
        BRIDGE_MEM32(stack + 0x10) = (uint32_t)((uint64_t)StartingOffset->QuadPart >> 32);
        if ((BRIDGE_MEM32(device + 0x14) & 4) && Length) {
            xbox_MmLockUnlockBufferPages(Buffer, Length, FALSE);
            BRIDGE_MEM32(irp + 4) |= 0x200;
            BRIDGE_MEM32(irp + 0x38) = Length;
        }
    }
    BRIDGE_MEM32(irp + 0x1c) = io_guest_address(IoStatusBlock);
    io_attach_request_thread(irp, 0);
    return packet;
}

PVOID __stdcall xbox_IoBuildSynchronousFsdRequest(ULONG MajorFunction,
    PVOID DeviceObject, PVOID Buffer, ULONG Length, PLARGE_INTEGER StartingOffset,
    HANDLE Event, PXBOX_IO_STATUS_BLOCK IoStatusBlock)
{
    PVOID packet = xbox_IoBuildAsynchronousFsdRequest(MajorFunction, DeviceObject,
        Buffer, Length, StartingOffset, IoStatusBlock);
    if (packet) {
        uint32_t irp = io_guest_address(packet);
        BRIDGE_MEM32(irp + 0x20) = io_guest_address(Event);
        io_attach_request_thread(irp, 1);
    }
    return packet;
}

PVOID __stdcall xbox_IoBuildDeviceIoControlRequest(ULONG IoControlCode,
    PVOID DeviceObject, PVOID InputBuffer, ULONG InputBufferLength,
    PVOID OutputBuffer, ULONG OutputBufferLength, BOOLEAN InternalDeviceIoControl,
    HANDLE Event, PXBOX_IO_STATUS_BLOCK IoStatusBlock)
{
    uint32_t device = io_guest_address(DeviceObject);
    PVOID packet = xbox_IoAllocateIrp(BRIDGE_MEM8(device + 0x1e));
    if (!packet) return NULL;
    uint32_t irp = io_guest_address(packet);
    uint32_t stack = BRIDGE_MEM32(irp + 0x5c) - XBOX_IO_STACK_BYTES;
    BRIDGE_MEM8(stack) = InternalDeviceIoControl ? 11 : 10;
    BRIDGE_MEM32(stack + 4) = OutputBufferLength;
    BRIDGE_MEM32(stack + 8) = io_guest_address(InputBuffer);
    BRIDGE_MEM32(stack + 0xc) = InputBufferLength;
    BRIDGE_MEM32(stack + 0x10) = IoControlCode;
    BRIDGE_MEM32(irp + 0x30) = io_guest_address(OutputBuffer);
    BRIDGE_MEM32(irp + 0x1c) = io_guest_address(IoStatusBlock);
    BRIDGE_MEM32(irp + 0x20) = io_guest_address(Event);
    if (OutputBufferLength && ((IoControlCode & 3) == 1 || (IoControlCode & 3) == 2)) {
        xbox_MmLockUnlockBufferPages(OutputBuffer, OutputBufferLength, FALSE);
        BRIDGE_MEM32(irp + 4) |= 0x200;
        BRIDGE_MEM32(irp + 0x38) = OutputBufferLength;
    }
    io_attach_request_thread(irp, 1);
    return packet;
}

static int io_is_native_device(uint32_t device);
static NTSTATUS io_native_control(uint32_t device,uint32_t file,uint32_t code,uint32_t input,
    uint32_t input_length,uint32_t output,uint32_t output_length,uint32_t *information,int filesystem);

static NTSTATUS io_native_dispatch(uint32_t device,uint32_t irp) {
    NTSTATUS status=STATUS_INVALID_DEVICE_REQUEST;RecompContFrame frame;
    const RecompContLocal locals[]={{"device",&device,sizeof device},{"irp",&irp,sizeof irp},{"status",&status,sizeof status}};
    int tracked=recomp_cont_active();
    uint64_t resume=tracked?kernel_native_cont_enter(&frame,KERNEL_CONT_NATIVE_DEVICE,locals,3):0;
    if(!resume) {
        uint32_t stack=BRIDGE_MEM32(irp+0x5c),major=BRIDGE_MEM8(stack),information=0;
        if(major==9 || major==10)status=io_native_control(device,BRIDGE_MEM32(stack+0x18),BRIDGE_MEM32(stack+0x10),
            BRIDGE_MEM32(stack+8),BRIDGE_MEM32(stack+0xc),BRIDGE_MEM32(irp+0x30),BRIDGE_MEM32(stack+4),&information,major==9);
        BRIDGE_MEM32(irp+0x10)=(uint32_t)status;BRIDGE_MEM32(irp+0x14)=information;
    }
    xbox_IofCompleteRequest(XBOX_TO_NATIVE(irp),0);
    if(tracked)recomp_cont_leave(&frame);return status;
}

NTSTATUS __fastcall xbox_IofCallDriver(PVOID DeviceObject, PVOID Irp)
{
    uint32_t device = io_guest_address(DeviceObject), irp = io_guest_address(Irp);
    uint32_t routine=0,native_device=0;int release_cpu = !guest_cpu_is_held();
    RecompContFrame frame;
    const RecompContLocal locals[]={{"device",&device,sizeof device},{"irp",&irp,sizeof irp},
        {"routine",&routine,sizeof routine},{"native_device",&native_device,sizeof native_device},{"release_cpu",&release_cpu,sizeof release_cpu}};
    int tracked=recomp_cont_active();
    uint64_t resume=tracked?kernel_native_cont_enter(&frame,KERNEL_CONT_DRIVER,locals,5):0;
    if (!guest_cpu_is_held()) guest_cpu_join();
    if(resume)goto dispatch;
    int location = (int8_t)BRIDGE_MEM8(irp + 0x19) - 1;
    if (location <= 0) {
        fprintf(stderr,"[STOP] IofCallDriver exhausted IRP stack %08X\n",irp);
        exit(5);
    }
    BRIDGE_MEM8(irp + 0x19) = (uint8_t)location;
    uint32_t stack = BRIDGE_MEM32(irp + 0x5c) - XBOX_IO_STACK_BYTES;
    BRIDGE_MEM32(irp + 0x5c) = stack;
    BRIDGE_MEM32(stack + 0x14) = device;
    uint32_t driver = BRIDGE_MEM32(device + 8), major = BRIDGE_MEM8(stack);
    native_device=io_is_native_device(device);
    routine = native_device?0:BRIDGE_MEM32(driver + 12 + major * 4);
dispatch: ;
    uint32_t args[]={device,irp};
    NTSTATUS status = native_device?io_native_dispatch(device,irp):(NTSTATUS)ke_call_guest(routine,args,2,0);
    if(tracked)recomp_cont_leave(&frame);
    if (release_cpu) guest_cpu_part();
    return status;
}


static void io_signal_guest_event(uint32_t event)
{
    if(!event)return;
    int type;HANDLE host=ke_guest_event(event,&type);
    if(!host) {fprintf(stderr,"[STOP] IRP event %08X is not a dispatcher event\n",event);exit(5);}
    ke_prepare_event_wait(event,host,type);ke_change_event(event,host,0);
}

static void io_publish_status(uint32_t irp)
{
    uint32_t iosb=BRIDGE_MEM32(irp+0x1c);
    if(iosb) {
        BRIDGE_MEM32(iosb+4)=BRIDGE_MEM32(irp+0x14);
        BRIDGE_MEM32(iosb)=BRIDGE_MEM32(irp+0x10);
    }
}

/* Request producers transfer their references to completion. A synchronous
 * API's event is caller-owned, and a create request retains its file object. */
static void io_release_request_objects(uint32_t file,uint32_t event,uint32_t flags)
{
    uint32_t phase=0;RecompContFrame frame;
    const RecompContLocal locals[]={{"file",&file,4},{"event",&event,4},{"flags",&flags,4},{"phase",&phase,4}};
    int tracked=recomp_cont_active();
    uint64_t resume=tracked?kernel_native_cont_enter(&frame,KERNEL_CONT_REQUEST_RELEASE,locals,4):0;
    if(resume) {
        if(phase==1)goto release_event;
        if(phase==2)goto release_file;
        recomp_cont_invalid();
    }
    if(event && file && !(flags&4)) {
        phase=1;
release_event:
        ke_dereference_object(event);
    }
    if(file && !(flags&8)) {
        phase=2;
release_file:
        ke_dereference_object(file);
    }
    if(tracked)recomp_cont_leave(&frame);
}

static void io_complete_apc(uint32_t apc,uint32_t file)
{
    uint32_t irp=apc-0x3c,flags=0,event=0,phase=0;RecompContFrame frame;
    const RecompContLocal locals[]={{"irp",&irp,4},{"file",&file,4},{"flags",&flags,4},{"event",&event,4},{"phase",&phase,4}};
    int tracked=recomp_cont_active();
    uint64_t resume=tracked?kernel_native_cont_enter(&frame,KERNEL_CONT_COMPLETE_APC,locals,5):0;
    if(resume) {
        if(phase==1)goto error_release;
        if(phase==2)goto normal_release;
        recomp_cont_invalid();
    }
    flags=BRIDGE_MEM32(irp+4);event=BRIDGE_MEM32(irp+0x20);
    uint32_t status=BRIDGE_MEM32(irp+0x10),iosb=BRIDGE_MEM32(irp+0x1c);
    int error=(status&0xc0000000u)==0xc0000000u;
    int synchronous=(flags&4) || (file && (BRIDGE_MEM8(file+3)&1));
    if(error && (!BRIDGE_MEM8(irp+0x1a) || synchronous)) {
        if(BRIDGE_MEM8(irp+0x1a)) {
            if(flags&4) {io_publish_status(irp);io_signal_guest_event(event?event:file+0x38);}
            else if(file) {BRIDGE_MEM32(file+0x10)=status;io_signal_guest_event(file+0x38);}
        }
        ke_list_unlink(irp+8);BRIDGE_MEM32(irp+8)=irp+8;BRIDGE_MEM32(irp+12)=irp+8;
        phase=1;
error_release:
        io_release_request_objects(file,event,flags);
        xbox_IoFreeIrp(XBOX_TO_NATIVE(irp));goto done;
    }
    io_publish_status(irp);io_signal_guest_event(event);
    if(file && (!event || ((BRIDGE_MEM8(file+3)&1) && !(flags&0x100)))) {
        BRIDGE_MEM32(file+0x10)=status;io_signal_guest_event(file+0x38);
    }
    ke_list_unlink(irp+8);BRIDGE_MEM32(irp+8)=irp+8;BRIDGE_MEM32(irp+12)=irp+8;
    uint32_t routine=(flags&8)?0:BRIDGE_MEM32(irp+0x28),context=BRIDGE_MEM32(irp+0x2c);
    if(routine) {
        ke_initialize_apc(apc,ke_current_thread(),KERNEL_IO_FREE_APC,KERNEL_IO_FREE_APC,routine,1,context);
        if(!ke_insert_apc(apc,iosb,0))xbox_IoFreeIrp(XBOX_TO_NATIVE(irp));
    } else if(file && BRIDGE_MEM32(file+0x20) && context) {
        uint32_t completion=BRIDGE_MEM32(file+0x20);
        BRIDGE_MEM32(irp+0x3c)=BRIDGE_MEM32(completion+4);
        BRIDGE_MEM32(irp+0x5c)=0;
        ke_insert_queue(BRIDGE_MEM32(completion),irp+0x54,0);
    } else xbox_IoFreeIrp(XBOX_TO_NATIVE(irp));
    phase=2;
normal_release:
    io_release_request_objects(file,event,flags);
done:
    if(tracked)recomp_cont_leave(&frame);
}

VOID __stdcall xbox_IoMarkIrpMustComplete(PVOID Irp)
{
    uint32_t irp=io_guest_address(Irp),stack=BRIDGE_MEM32(irp+0x5c);
    BRIDGE_MEM8(stack+3)|=2;
}

VOID __fastcall xbox_IofCompleteRequest(PVOID Irp,CCHAR PriorityBoost)
{
    (void)PriorityBoost;
    uint32_t irp=io_guest_address(Irp),stack=0,routine=0,device=0,context=0,phase=0;
    RecompContFrame frame;
    const RecompContLocal locals[]={{"irp",&irp,sizeof irp},{"stack",&stack,sizeof stack},
        {"routine",&routine,sizeof routine},{"device",&device,sizeof device},{"context",&context,sizeof context},{"phase",&phase,4}};
    int tracked=recomp_cont_active();
    uint64_t resume=tracked?kernel_native_cont_enter(&frame,KERNEL_CONT_COMPLETE,locals,6):0;
    Irp=XBOX_TO_NATIVE(irp);
    if(resume) {
        if(phase==1)goto completion_callback;
        if(phase==2)goto canceled_release;
        recomp_cont_invalid();
    }
    if(BRIDGE_MEM16(irp)!=6 || (int8_t)BRIDGE_MEM8(irp+0x19)>(int8_t)BRIDGE_MEM8(irp+0x18)+1) {
        fprintf(stderr,"[STOP] invalid or repeated IRP completion %08X\n",irp);exit(5);
    }
    stack=BRIDGE_MEM32(irp+0x5c);
    BRIDGE_MEM8(irp+0x19)++;BRIDGE_MEM32(irp+0x5c)+=XBOX_IO_STACK_BYTES;
    while(BRIDGE_MEM8(irp+0x19)<=BRIDGE_MEM8(irp+0x18)+1) {
        uint8_t control=BRIDGE_MEM8(stack+3);
        BRIDGE_MEM8(irp+0x1a)=control&1;
        int invoke=control&((int32_t)BRIDGE_MEM32(irp+0x10)<0?0x80:0x40);
        BRIDGE_MEM8(stack+1)=0;BRIDGE_MEM8(stack+2)=0;BRIDGE_MEM8(stack+3)=0;
        memset(XBOX_TO_NATIVE(stack+4),0,16);BRIDGE_MEM32(stack+0x18)=0;
        if(invoke) {
            device=BRIDGE_MEM8(irp+0x19)==BRIDGE_MEM8(irp+0x18)+1?0:
                BRIDGE_MEM32(BRIDGE_MEM32(irp+0x5c)+0x14);
            context=BRIDGE_MEM32(stack+0x20);routine=BRIDGE_MEM32(stack+0x1c);phase=1;
completion_callback: ;
            uint32_t args[]={device,irp,context};
            if(ke_call_guest(routine,args,3,0)==0xc0000016u)goto done;
        } else if(BRIDGE_MEM8(irp+0x1a) && BRIDGE_MEM8(irp+0x19)<=BRIDGE_MEM8(irp+0x18))
            BRIDGE_MEM8(BRIDGE_MEM32(irp+0x5c)+3)|=1;
        stack+=XBOX_IO_STACK_BYTES;BRIDGE_MEM32(irp+0x5c)+=XBOX_IO_STACK_BYTES;BRIDGE_MEM8(irp+0x19)++;
    }
    uint32_t flags=BRIDGE_MEM32(irp+4);
    if((flags&0x42) || (flags&8)) {
        if(flags&8) {
            /* Completed create packets remain caller-owned, but no longer
             * belong to the issuing thread's unfinished-I/O rundown list. */
            ke_list_unlink(irp+8);BRIDGE_MEM32(irp+8)=BRIDGE_MEM32(irp+12)=irp+8;
        }
        io_publish_status(irp);io_signal_guest_event(BRIDGE_MEM32(irp+0x20));
        if(flags&2)xbox_IoFreeIrp(Irp);goto done;
    }
    if(flags&0x200) {
        xbox_MmLockUnlockBufferPages(XBOX_TO_NATIVE(BRIDGE_MEM32(irp+0x30)),BRIDGE_MEM32(irp+0x38),TRUE);
        BRIDGE_MEM32(irp+4)&=~0x200u;
    }
    if((flags&0x80) && !BRIDGE_MEM8(irp+0x1a))goto done;
    uint32_t thread=BRIDGE_MEM32(irp+0x50),file=BRIDGE_MEM32(irp+0x60),apc=irp+0x3c;
    if(!thread && BRIDGE_MEM8(irp+0x1b)) {
        phase=2;
canceled_release:
        io_release_request_objects(BRIDGE_MEM32(irp+0x60),BRIDGE_MEM32(irp+0x20),BRIDGE_MEM32(irp+4));
        xbox_IoFreeIrp(Irp);goto done;
    }
    ke_initialize_apc(apc,thread,KERNEL_IO_COMPLETE_APC,0,0,0,0);
    if(!ke_insert_apc(apc,file,0)) {fprintf(stderr,"[STOP] completed IRP has no live issuing thread\n");exit(5);}
done:
    if(tracked)recomp_cont_leave(&frame);
}

NTSTATUS __stdcall xbox_IoInvalidDeviceRequest(PVOID DeviceObject,PVOID Irp)
{
    (void)DeviceObject;
    uint32_t irp=io_guest_address(Irp);RecompContFrame frame;
    const RecompContLocal locals[]={{"irp",&irp,sizeof irp}};
    int tracked=recomp_cont_active();
    uint64_t resume=tracked?kernel_native_cont_enter(&frame,KERNEL_CONT_INVALID_REQUEST,locals,1):0;
    if(!resume)BRIDGE_MEM32(irp+0x10)=(uint32_t)STATUS_INVALID_DEVICE_REQUEST;
    xbox_IofCompleteRequest(XBOX_TO_NATIVE(irp),0);
    if(tracked)recomp_cont_leave(&frame);
    return STATUS_INVALID_DEVICE_REQUEST;
}

static void io_reset_guest_event(uint32_t event) {
    int type;HANDLE handle=ke_guest_event(event,&type);
    if(!handle)recomp_cont_invalid();BRIDGE_MEM32(event+4)=0;ResetEvent(handle);
}
static void io_wait_guest_event(uint32_t event) {
    int type;HANDLE handle=ke_guest_event(event,&type);
    if(!handle)recomp_cont_invalid();ke_prepare_event_wait(event,handle,type);
    if(kernel_wait_native(1,&handle,KernelMode,FALSE,INFINITE)!=WAIT_OBJECT_0)recomp_cont_invalid();
    ke_finish_event_wait(event,handle,type);
}

typedef struct GuestControlFile {
    RecompContFrame frame;
    uint32_t file,event,locked;
} GuestControlFile;

static void io_control_release(uint32_t file,uint32_t event,uint32_t locked) {
    RecompContFrame frame;
    const RecompContLocal locals[]={{"file",&file,4},{"event",&event,4},{"locked",&locked,4}};
    int tracked=recomp_cont_active();
    uint64_t resume=tracked?kernel_native_cont_enter(&frame,KERNEL_CONT_CONTROL_RELEASE,locals,3):0;
    if(!resume && locked) {
        int32_t remaining=(int32_t)--BRIDGE_MEM32(file+0x24);
        if(locked==2 && remaining>=0)io_signal_guest_event(file+0x28);
    }
    io_release_request_objects(file,event,0);
    if(tracked)recomp_cont_leave(&frame);
}
static void io_control_finish(GuestControlFile *call) {
    uint32_t event=call->event,file=call->file,locked=call->locked;
    call->event=call->file=call->locked=0;
    io_control_release(file,event,locked);
}
static void io_control_leave(RecompContFrame *frame) {
    GuestControlFile *call=(GuestControlFile *)frame;
    if(!frame->retained && !checkpoint_root_cancelled() && (call->file || call->event || call->locked))io_control_finish(call);
}

static void io_control_unwind(RecompContFrame *frame) {
    io_control_finish((GuestControlFile *)frame);
}

static NTSTATUS io_control_file(HANDLE handle,HANDLE event_handle,uint32_t apc,uint32_t context,
    uint32_t ios,uint32_t code,uint32_t input,uint32_t input_length,uint32_t output,uint32_t output_length,int filesystem)
{
    uint32_t device=0,irp=0,phase=0,synchronous=0;GuestControlFile call={0};
    int held=guest_cpu_is_held(),was_thread=t_guest_thread;NTSTATUS status=0;
    const RecompContLocal locals[]={
        {"file",&call.file,4},{"event",&call.event,4},{"device",&device,4},{"irp",&irp,4},
        {"phase",&phase,4},{"synchronous",&synchronous,4},{"locked",&call.locked,4},{"status",&status,4},
        {"ios",&ios,4},{"code",&code,4},{"input",&input,4},{"input_length",&input_length,4},
        {"output",&output,4},{"output_length",&output_length,4},{"apc",&apc,4},{"context",&context,4},
        {"filesystem",&filesystem,sizeof filesystem},{"held",&held,sizeof held},{"was_thread",&was_thread,sizeof was_thread}};
    int tracked=recomp_cont_active();
    uint64_t resume=tracked?kernel_native_cont_enter(&call.frame,KERNEL_CONT_CONTROL_FILE,locals,sizeof locals/sizeof locals[0]):0;
    if(tracked){call.frame.on_leave=io_control_leave;call.frame.on_unwind=io_control_unwind;}
    if(!guest_cpu_is_held())guest_cpu_join();
    if(resume) {
        if(phase==1)goto wait_lock;
        if(phase==2)goto dispatch;
        if(phase==3)goto complete_immediate;
        if(phase==4)goto deliver_apcs;
        if(phase==5)goto wait_completion;
        if(phase==6)goto done;
        recomp_cont_invalid();
    }
    if(!ios || !bridge_buf_ok(ios,8,"file control IOSB") || !bridge_buf_ok(input,input_length,"file control input") ||
       !bridge_buf_ok(output,output_length,"file control output")) {status=(NTSTATUS)0xc0000005u;goto done;}
    status=ke_reference_handle(handle,XBOX_KERNEL_DATA_BASE+KDATA_FILE_OBJ_TYPE,&call.file);if(status<0)goto done;
    if(BRIDGE_MEM32(call.file+0x20) && apc){status=STATUS_INVALID_PARAMETER;goto done;}
    if(event_handle) {
        status=ke_reference_handle(event_handle,XBOX_KERNEL_DATA_BASE+KDATA_EVENT_OBJ_TYPE,&call.event);if(status<0)goto done;
        io_reset_guest_event(call.event);
    }
    synchronous=!!(BRIDGE_MEM8(call.file+3)&1);
    if(synchronous) {
        ++BRIDGE_MEM32(call.file+0x24);call.locked=1;
        if(BRIDGE_MEM32(call.file+0x24)) {
            phase=1;
wait_lock:
            io_wait_guest_event(call.file+0x28);
        }
        call.locked=2;
    }
    device=BRIDGE_MEM32(call.file+4);io_reset_guest_event(call.file+0x38);
    if(!device){status=STATUS_INVALID_DEVICE_REQUEST;goto done;}
    irp=io_guest_address(xbox_IoBuildDeviceIoControlRequest(code,XBOX_TO_NATIVE(device),XBOX_TO_NATIVE(input),input_length,
        XBOX_TO_NATIVE(output),output_length,FALSE,XBOX_TO_NATIVE(call.event),XBOX_TO_NATIVE(ios)));
    if(!irp){status=STATUS_INSUFFICIENT_RESOURCES;goto done;}
    uint32_t stack=BRIDGE_MEM32(irp+0x5c)-XBOX_IO_STACK_BYTES;
    BRIDGE_MEM8(stack)=filesystem?9:10;BRIDGE_MEM32(stack+0x18)=call.file;
    BRIDGE_MEM32(irp+0x60)=call.file;BRIDGE_MEM32(irp+0x28)=apc;BRIDGE_MEM32(irp+0x2c)=context;
    if(filesystem)BRIDGE_MEM32(irp+4)|=0x80;
    /* Completion owns one call.file reference and the supplied call.event reference.
     * This call retains its own call.file reference until unlocking/waiting ends. */
    if(ke_reference_pointer(call.file,XBOX_KERNEL_DATA_BASE+KDATA_FILE_OBJ_TYPE)<0)recomp_cont_invalid();
    call.event=0;phase=2;
dispatch:
    status=xbox_IofCallDriver(XBOX_TO_NATIVE(device),XBOX_TO_NATIVE(irp));
    if(filesystem && status!=STATUS_PENDING) {
        phase=3;
complete_immediate:
        io_complete_apc(irp+0x3c,call.file);
    }
    phase=4;
deliver_apcs:
    xbox_DeliverThreadApcs(FALSE);
    if(synchronous && status==STATUS_PENDING) {
        phase=5;
wait_completion:
        io_wait_guest_event(call.file+0x38);status=(NTSTATUS)BRIDGE_MEM32(call.file+0x10);
    }
done:
    phase=6;io_control_finish(&call);
    if(tracked){call.frame.on_leave=NULL;recomp_cont_leave(&call.frame);}
    if(!held){guest_cpu_part();t_guest_thread=was_thread;}
    return status;
}

static NTSTATUS io_control_native(HANDLE handle,HANDLE event,PIO_APC_ROUTINE apc,PVOID context,
    PXBOX_IO_STATUS_BLOCK32 ios,ULONG code,PVOID input,ULONG input_length,PVOID output,ULONG output_length,int filesystem)
{
    const void *pointers[]={ios,input,output,(const void *)apc,context};
    for(unsigned i=0;i<sizeof pointers/sizeof pointers[0];i++)if(pointers[i] &&
        XBOX_TO_NATIVE(io_guest_address(pointers[i]))!=pointers[i])return STATUS_INVALID_PARAMETER;
    return io_control_file(handle,event,io_guest_address((const void *)apc),io_guest_address(context),io_guest_address(ios),
        code,io_guest_address(input),input_length,io_guest_address(output),output_length,filesystem);
}

NTSTATUS __stdcall xbox_NtFsControlFile(HANDLE handle,HANDLE event,PIO_APC_ROUTINE apc,PVOID context,
    PXBOX_IO_STATUS_BLOCK32 ios,ULONG code,PVOID input,ULONG input_length,PVOID output,ULONG output_length)
{
    return io_control_native(handle,event,apc,context,ios,code,input,input_length,output,output_length,1);
}
NTSTATUS __stdcall xbox_NtDeviceIoControlFile(HANDLE handle,HANDLE event,PIO_APC_ROUTINE apc,PVOID context,
    PXBOX_IO_STATUS_BLOCK32 ios,ULONG code,PVOID input,ULONG input_length,PVOID output,ULONG output_length)
{
    return io_control_native(handle,event,apc,context,ios,code,input,input_length,output,output_length,0);
}

static uint32_t io_synchronous_frame(int type)
{
    g_esp-=24;uint32_t event=g_esp;
    memset(XBOX_TO_NATIVE(event),0,24);
    if(!ke_initialize_event(event,type,0)) {g_esp+=24;return 0;}
    return event;
}

/* Cleanup belongs to the last public handle; close belongs to the last
 * object reference. Both original requests retain their IRP until return. */
static void io_file_request(uint32_t file,uint32_t major) {
    uint32_t device=0,irp=0,event=0,phase=0,synchronous=0;
    RecompContFrame frame;
    const RecompContLocal locals[]={{"file",&file,4},{"major",&major,4},{"device",&device,4},
        {"irp",&irp,4},{"event",&event,4},{"phase",&phase,4},{"synchronous",&synchronous,4}};
    int tracked=recomp_cont_active();
    uint64_t resume=tracked?kernel_native_cont_enter(&frame,KERNEL_CONT_FILE_REQUEST,locals,7):0;
    if(resume) {
        if(phase==1)goto wait_lock;
        if(phase==2)goto dispatch;
        if(phase==3)goto wait_completion;
        recomp_cont_invalid();
    }
    if(major==13)BRIDGE_MEM8(file+3)|=0x20;
    device=BRIDGE_MEM32(file+4);synchronous=!!(BRIDGE_MEM8(file+3)&1);
    if(!device)goto done;
    if(synchronous) {
        if(++BRIDGE_MEM32(file+0x24)) {
            phase=1;
wait_lock:
            io_wait_guest_event(file+0x28);
        }
    }
    io_reset_guest_event(file+0x38);
    /* The native directory backend is retired by its actual NtClose below.
     * Guest drivers receive their own cleanup/close dispatch, even when the
     * device originally came from that native mount. */
    if(io_is_native_device(device))goto unlock;
    event=io_synchronous_frame(1);if(!event)recomp_cont_invalid();
    irp=io_guest_address(xbox_IoAllocateIrp(BRIDGE_MEM8(device+0x1e)));
    if(!irp)recomp_cont_invalid();
    BRIDGE_MEM32(irp+4)=0x44;BRIDGE_MEM32(irp+0x60)=file;
    BRIDGE_MEM32(irp+0x20)=event;BRIDGE_MEM32(irp+0x1c)=major==13?irp+0x10:event+16;
    uint32_t stack=BRIDGE_MEM32(irp+0x5c)-XBOX_IO_STACK_BYTES;
    BRIDGE_MEM8(stack)=(uint8_t)major;BRIDGE_MEM32(stack+0x18)=file;
    io_attach_request_thread(irp,1);phase=2;
dispatch:
    if(xbox_IofCallDriver(XBOX_TO_NATIVE(device),XBOX_TO_NATIVE(irp))==STATUS_PENDING) {
        phase=3;
wait_completion:
        io_wait_guest_event(event);
    }
    ke_list_unlink(irp+8);BRIDGE_MEM32(irp+8)=BRIDGE_MEM32(irp+12)=irp+8;
    xbox_IoFreeIrp(XBOX_TO_NATIVE(irp));ke_shadow_remove(event);g_esp=event+24;
unlock:
    /* Delete owns the locked body until it is freed, as in IoFileObjectType. */
    if(major==13 && synchronous && (int32_t)--BRIDGE_MEM32(file+0x24)>=0)
        io_signal_guest_event(file+0x28);
done:
    if(tracked)recomp_cont_leave(&frame);
}

static void io_delete_file(uint32_t file) {
    uint32_t phase=0,completion=0;RecompContFrame frame;
    const RecompContLocal locals[]={{"file",&file,4},{"phase",&phase,4},{"completion",&completion,4}};
    int tracked=recomp_cont_active();
    uint64_t resume=tracked?kernel_native_cont_enter(&frame,KERNEL_CONT_FILE_DELETE,locals,3):0;
    if(resume) {
        if(phase==1)goto cleanup;
        if(phase==2)goto close_file;
        if(phase==3)goto release_completion;
        if(phase==4)goto release_create_related;
        if(phase==5)goto release_create_device;
        recomp_cont_invalid();
    }
    if(!BRIDGE_MEM32(file+4))goto done;
    if(!(BRIDGE_MEM8(file+3)&0x20)) {
        phase=1;
cleanup:
        io_file_request(file,13);
    }
    phase=2;
close_file:
    io_file_request(file,1);
    completion=BRIDGE_MEM32(file+0x20);
    if(completion) {
        phase=3;
release_completion:
        ke_dereference_object(BRIDGE_MEM32(completion));
        xbox_HeapFree(completion);BRIDGE_MEM32(file+0x20)=0;
    }
    if(io_is_guest_file(file)) {
        if(BRIDGE_MEM32(file+0x60)) {
            phase=4;
release_create_related:
            ke_dereference_object(BRIDGE_MEM32(file+0x60));BRIDGE_MEM32(file+0x60)=0;
        }
        phase=5;
release_create_device:
        xbox_kernel_release_file_device(BRIDGE_MEM32(file+4));
        BRIDGE_MEM32(file+4)=0;
    }
done:
    if(tracked)recomp_cont_leave(&frame);
}

static NTSTATUS io_synchronous_call(PVOID device,PVOID irp,uint32_t event,PULONG returned)
{
    NTSTATUS status=STATUS_INSUFFICIENT_RESOURCES;uint32_t phase=1;
    RecompContFrame frame;
    const RecompContLocal locals[]={{"status",&status,sizeof status},{"phase",&phase,sizeof phase}};
    int tracked=recomp_cont_active();
    uint64_t resume=tracked?kernel_native_cont_enter(&frame,KERNEL_CONT_SYNC_CALL,locals,2):0;
    if(resume) {
        if(phase==2)goto deliver_apcs;
        if(phase==3)goto wait_for_completion;
        if(phase!=1)recomp_cont_invalid();
    }
    if(irp) {
        status=xbox_IofCallDriver(device,irp);
        phase=2;
deliver_apcs:
        xbox_DeliverThreadApcs(FALSE);
        if(status==0x103) {
            phase=3;
wait_for_completion: ;
            int type;HANDLE host=ke_guest_event(event,&type);
            if(!host) {fprintf(stderr,"[STOP] synchronous request event allocation failed\n");exit(5);}
            DWORD result=kernel_wait_native(1,&host,KernelMode,FALSE,INFINITE);
            if(result!=WAIT_OBJECT_0) {fprintf(stderr,"[STOP] synchronous request wait failed\n");exit(5);}
            status=(NTSTATUS)BRIDGE_MEM32(event+16);
        }
    }
    if(returned)*returned=BRIDGE_MEM32(event+20);
    ke_shadow_remove(event);g_esp=event+24;
    if(tracked)recomp_cont_leave(&frame);
    return status;
}

typedef struct GuestIoSynchronous {
    RecompContFrame frame;
    uint32_t event,irp,device,returned;
    RecompContLocal locals[4];
    int tracked;
} GuestIoSynchronous;

static uint64_t io_synchronous_enter(GuestIoSynchronous *call,PVOID device,PULONG returned)
{
    call->event=call->irp=0;call->device=io_guest_address(device);call->returned=io_guest_address(returned);
    call->locals[0]=(RecompContLocal){"event",&call->event,sizeof call->event};
    call->locals[1]=(RecompContLocal){"irp",&call->irp,sizeof call->irp};
    call->locals[2]=(RecompContLocal){"device",&call->device,sizeof call->device};
    call->locals[3]=(RecompContLocal){"returned",&call->returned,sizeof call->returned};
    call->tracked=recomp_cont_active();
    return call->tracked?kernel_native_cont_enter(&call->frame,KERNEL_CONT_SYNC_API,call->locals,4):0;
}

static NTSTATUS io_synchronous_finish(GuestIoSynchronous *call)
{
    NTSTATUS status=call->event?io_synchronous_call(XBOX_TO_NATIVE(call->device),
        XBOX_TO_NATIVE(call->irp),call->event,XBOX_TO_NATIVE(call->returned)):STATUS_INSUFFICIENT_RESOURCES;
    if(call->tracked)recomp_cont_leave(&call->frame);
    return status;
}

NTSTATUS __stdcall xbox_IoSynchronousDeviceIoControlRequest(ULONG code,PVOID device,
    PVOID input,ULONG input_length,PVOID output,ULONG output_length,PULONG returned,BOOLEAN internal)
{
    GuestIoSynchronous call;
    if(!io_synchronous_enter(&call,device,returned)) {
        call.event=io_synchronous_frame(0);
        if(call.event)call.irp=io_guest_address(xbox_IoBuildDeviceIoControlRequest(code,device,input,
            input_length,output,output_length,internal,XBOX_TO_NATIVE(call.event),XBOX_TO_NATIVE(call.event+16)));
    }
    return io_synchronous_finish(&call);
}

NTSTATUS __stdcall xbox_IoSynchronousFsdRequest(ULONG major,PVOID device,PVOID buffer,
    ULONG length,PLARGE_INTEGER offset)
{
    GuestIoSynchronous call;
    if(!io_synchronous_enter(&call,device,NULL)) {
        call.event=io_synchronous_frame(0);
        if(call.event)call.irp=io_guest_address(xbox_IoBuildSynchronousFsdRequest(major,device,buffer,length,
            offset,XBOX_TO_NATIVE(call.event),XBOX_TO_NATIVE(call.event+16)));
    }
    return io_synchronous_finish(&call);
}


static void ke_queue_signal(uint32_t queue)
{
    HANDLE event=ke_shadow_lookup(queue);
    if(!event){fprintf(stderr,"[STOP] queue %08X has no dispatcher event\n",queue);exit(5);}
    if(BRIDGE_MEM32(queue+4) && BRIDGE_MEM32(queue+0x18)<BRIDGE_MEM32(queue+0x1c))SetEvent(event);
    else ResetEvent(event);
}

static int ke_initialize_queue(uint32_t queue,uint32_t maximum)
{
    HANDLE event=CreateEventW(NULL,TRUE,FALSE,NULL);
    if(!event)return 0;
    if(!ke_shadow_insert(queue,event))return 0;
    ke_initialize_header(queue,4,10,0);
    BRIDGE_MEM32(queue+0x10)=queue+0x10;BRIDGE_MEM32(queue+0x14)=queue+0x10;
    BRIDGE_MEM32(queue+0x18)=0;BRIDGE_MEM32(queue+0x1c)=maximum?maximum:1;
    BRIDGE_MEM32(queue+0x20)=queue+0x20;BRIDGE_MEM32(queue+0x24)=queue+0x20;
    return 1;
}

static uint32_t ke_insert_queue(uint32_t queue,uint32_t entry,int first)
{
    uint32_t previous=BRIDGE_MEM32(queue+4),head=queue+0x10;
    ke_list_before(first?BRIDGE_MEM32(head):head,entry);
    BRIDGE_MEM32(queue+4)=previous+1;ke_queue_signal(queue);return previous;
}

/* A queue-associated worker releases its active slot while blocking on any
 * dispatcher object. The guest KTHREAD remains the sole association record. */
uint32_t xbox_QueueThreadWaitBegin(void)
{
    if(!t_guest_thread)return 0;
    int held=guest_cpu_is_held();if(!held)guest_cpu_join();
    uint32_t thread=ke_current_thread(),queue=thread?BRIDGE_MEM32(thread+0x78):0;
    if(queue && BRIDGE_MEM8(thread+0x2c)!=5) {
        BRIDGE_MEM8(thread+0x2c)=5;BRIDGE_MEM32(queue+0x18)--;ke_queue_signal(queue);
    } else queue=0;
    if(!held){guest_cpu_part();t_guest_thread=1;}
    return queue;
}

void xbox_QueueThreadWaitEnd(uint32_t queue)
{
    if(!queue)return;
    int held=guest_cpu_is_held();if(!held)guest_cpu_join();
    BRIDGE_MEM8(ke_current_thread()+0x2c)=2;BRIDGE_MEM32(queue+0x18)++;ke_queue_signal(queue);
    if(!held){guest_cpu_part();t_guest_thread=1;}
}

static uint32_t ke_rundown_queue(uint32_t queue)
{
    uint32_t head=queue+0x10,first=BRIDGE_MEM32(head);
    if(first==head)first=0;
    else {uint32_t last=BRIDGE_MEM32(head+4);BRIDGE_MEM32(last)=first;BRIDGE_MEM32(first+4)=last;}
    head=queue+0x20;
    while(BRIDGE_MEM32(head)!=head) {
        uint32_t link=BRIDGE_MEM32(head);BRIDGE_MEM32(link-4)=0;ke_list_unlink(link);
    }
    return first;
}

static uint32_t ke_remove_queue(uint32_t queue,KPROCESSOR_MODE mode,PLARGE_INTEGER timeout)
{
    int held=guest_cpu_is_held(),was_thread=t_guest_thread;
    uint32_t thread=0;DWORD milliseconds=0,remaining=0;ULONGLONG started=0;
    RecompContFrame frame;
    const RecompContLocal locals[]={{"queue",&queue,sizeof queue},{"mode",&mode,sizeof mode},
        {"held",&held,sizeof held},{"was_thread",&was_thread,sizeof was_thread},{"thread",&thread,sizeof thread},
        {"milliseconds",&milliseconds,sizeof milliseconds},{"remaining",&remaining,sizeof remaining},
        {"started",&started,sizeof started}};
    int tracked=recomp_cont_active();
    uint64_t resume=tracked?kernel_native_cont_enter(&frame,KERNEL_CONT_QUEUE,locals,8):0;
    if(!guest_cpu_is_held())guest_cpu_join();
    uint32_t result;
    if(resume)goto waiting;
    thread=ke_current_thread();uint32_t old_queue=BRIDGE_MEM32(thread+0x78);
    if(old_queue) {
        BRIDGE_MEM32(old_queue+0x18)--;
        if(old_queue!=queue) {ke_list_unlink(thread+0x7c);ke_queue_signal(old_queue);}
    }
    if(old_queue!=queue) {BRIDGE_MEM32(thread+0x78)=queue;ke_list_before(queue+0x20,thread+0x7c);}
    BRIDGE_MEM8(thread+0x2c)=5;
    if(old_queue && old_queue!=queue)ke_maybe_release_queue(old_queue);
    milliseconds=xbox_nt_timeout_to_ms(timeout);started=xbox_RuntimeTickCount64();
    for(;;) {
        if(BRIDGE_MEM32(queue+4) && BRIDGE_MEM32(queue+0x18)<BRIDGE_MEM32(queue+0x1c)) {
            result=BRIDGE_MEM32(queue+0x10);ke_list_unlink(result);
            BRIDGE_MEM32(result)=0;BRIDGE_MEM32(queue+4)--;break;
        }
        ke_queue_signal(queue);
        remaining=milliseconds;
        if(milliseconds!=INFINITE) {
            ULONGLONG elapsed=xbox_RuntimeTickCount64()-started;
            remaining=elapsed>=milliseconds?0:milliseconds-(DWORD)elapsed;
        }
waiting: ;
        HANDLE event=ke_shadow_lookup(queue);
        DWORD wait=kernel_wait_native(1,&event,mode,FALSE,remaining);
        if(wait==WAIT_TIMEOUT){result=0x102;break;}
        if(wait==XBOX_WAIT_USER_APC){result=0xc0;break;}
        if(wait!=WAIT_OBJECT_0){fprintf(stderr,"[STOP] queue wait failed %08X\n",wait);exit(5);}
    }
    BRIDGE_MEM32(queue+0x18)++;BRIDGE_MEM8(thread+0x2c)=2;ke_queue_signal(queue);
    if(tracked)recomp_cont_leave(&frame);
    if(!held){guest_cpu_part();t_guest_thread=was_thread;}
    return result;
}

NTSTATUS __stdcall xbox_IoSetIoCompletion(PVOID IoCompletion,PVOID KeyContext,
    PVOID ApcContext,NTSTATUS IoStatus,ULONG_PTR IoStatusInformation)
{
    uint32_t packet=xbox_HeapAlloc(0x1c,16);if(!packet)return STATUS_INSUFFICIENT_RESOURCES;
    BRIDGE_MEM32(packet+8)=1;BRIDGE_MEM32(packet+12)=io_guest_address(KeyContext);
    BRIDGE_MEM32(packet+16)=io_guest_address(ApcContext);BRIDGE_MEM32(packet+20)=(uint32_t)IoStatus;
    BRIDGE_MEM32(packet+24)=(uint32_t)IoStatusInformation;
    ke_insert_queue(io_guest_address(IoCompletion),packet,0);return STATUS_SUCCESS;
}

static void io_disassociate_thread_irp(uint32_t thread)
{
    uint32_t head=thread+0x134,link=BRIDGE_MEM32(head);
    if(link==head)return;
    uint32_t irp=link-8;
    /* An already completed request belongs to its queued completion APC. */
    if((int8_t)BRIDGE_MEM8(irp+0x19)==(int8_t)BRIDGE_MEM8(irp+0x18)+2)return;
    BRIDGE_MEM32(irp+0x50)=0;
    ke_list_unlink(link);BRIDGE_MEM32(link)=BRIDGE_MEM32(link+4)=link;
}

static void ke_rundown_current_thread(void)
{
    int held=guest_cpu_is_held();
    uint32_t thread=0,head=0,waits=0,phase=0,apc=0,rundown=0;
    RecompContFrame frame;
    const RecompContLocal locals[]={{"held",&held,sizeof held},{"thread",&thread,sizeof thread},
        {"head",&head,sizeof head},{"waits",&waits,sizeof waits},{"phase",&phase,sizeof phase},
        {"apc",&apc,sizeof apc},{"rundown",&rundown,sizeof rundown}};
    int tracked=recomp_cont_active();
    uint64_t resume=tracked?kernel_native_cont_enter(&frame,KERNEL_CONT_RUNDOWN,locals,7):0;
    if(!guest_cpu_is_held())guest_cpu_join();
    if(resume) {
        if(phase==1)goto deliver_irps;
        if(phase==2)goto wait_for_irps;
        if(phase==3)goto rundown_callback;
        if(phase==4)goto release_calls;
        recomp_cont_invalid();
    }
    thread=ke_current_thread();head=thread+0x134;
    BRIDGE_MEM8(thread+0x77)=1;
    xbox_KfLowerIrql(PASSIVE_LEVEL);
    for(uint32_t link=BRIDGE_MEM32(head);link!=head;link=BRIDGE_MEM32(link))
        BRIDGE_MEM8(link-8+0x1b)=1;
    while(BRIDGE_MEM32(head)!=head) {
        phase=1;
deliver_irps:
        xbox_DeliverThreadApcs(FALSE);
        if(BRIDGE_MEM32(head)==head)break;
        phase=2;
wait_for_irps:
        kernel_wait_native(0,NULL,KernelMode,FALSE,100);
        /* Original I/O rundown allows 3,002 100-ms waits, then detaches
         * one unfinished request per wait without freeing driver-owned IRPs. */
        if(waits++>3000)io_disassociate_thread_irp(thread);
    }
    ke_rundown_thread_timers(thread);
    BRIDGE_MEM8(thread+0x4b)=0;
    head=thread+0x3c;
    while(BRIDGE_MEM32(head)!=head) {
        apc=BRIDGE_MEM32(head)-8;rundown=BRIDGE_MEM32(apc+0x14);
        ke_remove_apc(apc);
        if(rundown==KERNEL_IO_FREE_APC)xbox_IoFreeIrp(XBOX_TO_NATIVE(apc-0x3c));
        else if(!rundown || rundown==KERNEL_POOL_FREE_APC)xbox_HeapFree(apc);
        else {
            phase=3;
rundown_callback: ;
            uint32_t args[]={apc};ke_call_guest(rundown,args,1,0);
        }
    }
    if(BRIDGE_MEM32(thread+0x34)!=thread+0x34) {
        fprintf(stderr,"[STOP] thread %08X exits with a kernel APC pending\n",thread);exit(5);
    }
    uint32_t queue=BRIDGE_MEM32(thread+0x78);
    if(queue) {
        ke_list_unlink(thread+0x7c);BRIDGE_MEM32(thread+0x78)=0;
        BRIDGE_MEM32(queue+0x18)--;ke_queue_signal(queue);ke_maybe_release_queue(queue);
    }
    phase=4;
release_calls:
    recomp_cont_prepare_unwind(NULL);
    BRIDGE_MEM8(thread+0x2c)=4;BRIDGE_MEM32(thread+4)=1;
    if(tracked)recomp_cont_leave(&frame);
    if(!held)guest_cpu_part();
}

/* A worker that returns still has live I/O and APC rundown. Keep that work
 * inside an identifiable root until it completes, just like explicit exit. */
static void bridge_finish_thread(void)
{
    uint32_t exit_status=g_eax;
    RecompContFrame frame;
    const RecompContLocal locals[]={{"exit_status",&exit_status,sizeof exit_status}};
    kernel_native_cont_enter(&frame,KERNEL_CONT_THREAD_EXIT,locals,1);
    ke_rundown_current_thread();
    BRIDGE_MEM32(ke_current_thread()+0x120)=exit_status;
    g_eax=exit_status;t_guest_terminated=1;
    recomp_cont_leave(&frame);
}

static recomp_func_t checkpoint_lookup_root(uint32_t function)
{
    return function==KERNEL_CONT_THREAD_EXIT?bridge_finish_thread:recomp_lookup(function);
}

static void io_start_driver_packet(uint32_t device, uint32_t irp)
{
    uint32_t driver = BRIDGE_MEM32(device + 8);
    uint32_t routine = BRIDGE_MEM32(driver);
    recomp_func_t fn = recomp_lookup(routine);
    if (!fn) fn = recomp_lookup_manual(routine);
    if (!fn) {
        fprintf(stderr,"[STOP] DriverStartIo target %08X is not translated\n",routine);
        exit(5);
    }
    BRIDGE_MEM32(device + 0x10) = irp;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = irp;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = device;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
    fn();
}

/* Xbox removes the Windows cancellation arguments. Both the direct export
 * table and import bridges use these same guest packet operations. */
VOID __stdcall xbox_IoStartPacket(PVOID DeviceObject, PVOID Irp, PULONG Key)
{
    uint32_t device = (uint32_t)((uintptr_t)DeviceObject - g_xbox_mem_offset);
    uint32_t irp = (uint32_t)((uintptr_t)Irp - g_xbox_mem_offset);
    int release_cpu = !guest_cpu_is_held();
    if (release_cpu) guest_cpu_join();
    int saved_irql = xbox_IrqlEnterInterrupt(DISPATCH_LEVEL);
    if (!ke_insert_device_queue(device + 0x28, irp + 0x3c, Key != NULL, Key ? *Key : 0))
        io_start_driver_packet(device, irp);
    xbox_IrqlLeaveInterrupt(saved_irql);
    if (release_cpu) guest_cpu_part();
}

static void io_start_next_packet(PVOID DeviceObject, int keyed, uint32_t key)
{
    uint32_t device = (uint32_t)((uintptr_t)DeviceObject - g_xbox_mem_offset);
    int release_cpu = !guest_cpu_is_held();
    if (release_cpu) guest_cpu_join();
    int saved_irql = xbox_IrqlEnterInterrupt(DISPATCH_LEVEL);
    BRIDGE_MEM32(device + 0x10) = 0;
    uint32_t entry = ke_remove_device_queue(device + 0x28, keyed, key);
    if (entry) io_start_driver_packet(device, entry - 0x3c);
    xbox_IrqlLeaveInterrupt(saved_irql);
    if (release_cpu) guest_cpu_part();
}

VOID __stdcall xbox_IoStartNextPacket(PVOID DeviceObject)
{
    io_start_next_packet(DeviceObject, 0, 0);
}

VOID __stdcall xbox_IoStartNextPacketByKey(PVOID DeviceObject, ULONG Key)
{
    io_start_next_packet(DeviceObject, 1, Key);
}

/* ── KeSetEvent (ordinal 145) ────────────────────────────── */
static void bridge_KeSetEvent(void)
{
    uint32_t guest_va = STACK_ARG(0);
    uint32_t increment = STACK_ARG(1);
    uint32_t wait = STACK_ARG(2);
    HANDLE h;

    (void)increment;
    (void)wait;

    {   /* A registered kernel or title-initialized event. */
        int type;
        HANDLE ge = ke_guest_event(guest_va, &type);
        if (ge) {
            ke_prepare_event_wait(guest_va, ge, type);
            g_eax = ke_change_event(guest_va, ge, 0);
            return;
        }
    }
    h = ke_shadow_lookup(guest_va);
    if (!h)
        h = bridge_resolve_handle(guest_va);
    if (!h)
        h = XBOX_TO_NATIVE(guest_va);
    if (h)
        g_eax = (uint32_t)SetEvent(h);
    else
        g_eax = 0;
}


/* ── KeWaitForSingleObject (ordinal 159) ─────────────────── */
static void bridge_KeWaitForSingleObject(void)
{
    uint32_t object = STACK_ARG(0);
    uint32_t wait_reason = STACK_ARG(1);
    uint32_t wait_mode = STACK_ARG(2);
    uint32_t alertable = STACK_ARG(3);
    uint32_t timeout_ptr = STACK_ARG(4);
    HANDLE h;

    {   /* Wait on the same native event used by signal, timer and checkpoint
         * operations; synchronize the guest header without replaying signals. */
        int type;
        HANDLE ge = ke_guest_event(object, &type);
        if (ge) {
            ke_prepare_event_wait(object, ge, type);
            g_eax = (uint32_t)xbox_KeWaitForSingleObject(
                ge, wait_reason, wait_mode,
                (BOOLEAN)alertable, XBOX_TO_NATIVE(timeout_ptr));
            ke_finish_event_wait(object, ge, type);
            return;
        }
    }

    h = ke_shadow_lookup(object);
    if (!h)
        h = bridge_resolve_handle(object);
    if (!h)
        h = XBOX_TO_NATIVE(object);

    g_eax = (uint32_t)xbox_KeWaitForSingleObject(
        h, wait_reason, wait_mode,
        (BOOLEAN)alertable, XBOX_TO_NATIVE(timeout_ptr));
}

/* ── NtWaitForSingleObject (ordinal 233) ─────────────────── */
/*
 * The synchronous sibling of ...Ex. Halo's synchronous ReadFile issues the read
 * and then waits on its completion event through this; unbridged it fell to the
 * "return 0" default (STATUS_SUCCESS = "already signalled"), so the read handshake
 * completed before the data arrived and the UI-map precache never made progress.
 */
static void bridge_NtWaitForSingleObject(void)
{
    HANDLE   handle      = bridge_resolve_handle(STACK_ARG(0));
    uint32_t alertable   = STACK_ARG(1);
    uint32_t timeout_ptr = STACK_ARG(2);

    g_eax = (uint32_t)xbox_NtWaitForSingleObject(
        handle, (BOOLEAN)alertable, XBOX_TO_NATIVE(timeout_ptr));
}

/* ── NtClearEvent (ordinal 186) ──────────────────────────── */
/* Resets an event to non-signalled. Halo clears the read-completion event
 * before each async map read; a no-op here left the event stuck signalled. */
static void bridge_NtClearEvent(void)
{
    HANDLE handle = bridge_resolve_handle(STACK_ARG(0));
    g_eax = (uint32_t)xbox_NtClearEvent(handle);
}

/* ── NtSetEvent (ordinal 225) ────────────────────────────── */
/* Signals an event and optionally returns its previous state. Unbridged it
 * no-op'd, so a producer's "work ready" signal never landed -- Halo's map-copy
 * worker thread then slept forever in WaitForSingleObject on the decompress
 * context's go-event and only the first 14 KB of the map ever loaded. */
static void bridge_NtSetEvent(void)
{
    HANDLE   handle = bridge_resolve_handle(STACK_ARG(0));
    uint32_t prev   = STACK_ARG(1);
    g_eax = (uint32_t)xbox_NtSetEvent(handle, XBOX_TO_NATIVE(prev));
}

/* ── NtPulseEvent (ordinal 205) ──────────────────────────── */
/* Signal-then-reset: releases threads currently waiting, then leaves the event
 * non-signalled. Same unbridged-no-op hazard as NtSetEvent in the map-load
 * handoff chain. PulseEvent carries the (deprecated, lossy) Xbox semantics
 * faithfully -- a waiter not yet blocked misses it, exactly as on hardware. */
static void bridge_NtPulseEvent(void)
{
    HANDLE handle = bridge_resolve_handle(STACK_ARG(0));
    g_eax = (uint32_t)xbox_NtPulseEvent(handle, XBOX_TO_NATIVE(STACK_ARG(1)));
}

/* ── NtWaitForSingleObjectEx (ordinal 234) ───────────────── */
/*
 * Unbridged, this fell through to the "no bridge, returning 0" default -- and 0
 * is STATUS_SUCCESS, so every wait returned instantly as though the object were
 * already signalled. Halo's main loop then spun: 91 million calls in 100
 * seconds, no blocking, no progress. A wait that always succeeds is worse than
 * one that always fails, because it looks like the game is running.
 */
static HANDLE bridge_resolve_handle(uint32_t token);

static void bridge_NtWaitForSingleObjectEx(void)
{
    HANDLE   handle      = bridge_resolve_handle(STACK_ARG(0));
    uint32_t wait_mode   = STACK_ARG(1);
    uint32_t alertable   = STACK_ARG(2);
    uint32_t timeout_ptr = STACK_ARG(3);

    static int logged = 0;
    if (logged++ < 20) {
        fprintf(stderr, "  [KERNEL] NtWaitForSingleObjectEx: token=0x%08X "
                "handle=%p timeout=%s\n",
                STACK_ARG(0), handle, timeout_ptr ? "finite" : "INFINITE");
        fflush(stderr);
    }

    g_eax = (uint32_t)xbox_NtWaitForSingleObjectEx(
        handle, (KPROCESSOR_MODE)wait_mode, (BOOLEAN)alertable,
        XBOX_TO_NATIVE(timeout_ptr));
}

/* ── MmQueryAddressProtect (ordinal 179) ─────────────────── */
/* NtWaitForMultipleObjectsEx (ordinal 235, 6 args = 24 bytes)
 *
 * NTSTATUS NtWaitForMultipleObjectsEx(ULONG Count, HANDLE *Handles,
 *                                     WAIT_TYPE WaitType,
 *                                     KPROCESSOR_MODE WaitMode,
 *                                     BOOLEAN Alertable,
 *                                     PLARGE_INTEGER Timeout);
 *
 * Six, not five: WaitMode sits between WaitType and Alertable. Half-Life 2's
 * own call site settles it -- sub_0059BE0F pushes six dwords before the
 * thunk (esi, eax, ebx, 1, [ebp+0x18], edi) and the callee is expected to pop
 * them all.
 *
 * Getting it wrong cost 4 bytes of guest stack per call and shifted every
 * argument after WaitType, so the wait read Alertable as its timeout pointer
 * and reported INFINITE for every wait. The leak was invisible to the esp
 * invariant because sub_0059BE0F restores esp with `leave`: its own frame
 * came back correct while `pop edi; pop esi; pop ebx` took their values one
 * slot out, so the caller's `this` -- kept in ebx by sub_005ACDC0 -- came
 * back holding what esi had, and the next [ebx+0x94] read a string as a
 * pointer.
 *
 * xbox_NtWaitForMultipleObjectsEx has been in kernel_sync.c all along; only
 * the bridge wrapper was missing, so the thunk fell through to the fallback
 * and returned 0 -- STATUS_SUCCESS, meaning 'object 0 is signalled'. A wait
 * that always reports signalled turns a blocking wait into a busy loop, which
 * is exactly what Half-Life 2 does after spawning its first worker: the main
 * thread spins in a CUtlLinkedList walk making no indirect calls at all.
 *
 * Handles is a guest array of tokens, so each has to be resolved
 * individually -- the array cannot just be pointed at. Bounded because a
 * bogus Count would otherwise read arbitrary guest memory onto the stack;
 * MAXIMUM_WAIT_OBJECTS is the real kernel's own limit.
 */
static void bridge_NtWaitForMultipleObjectsEx(void)
{
    uint32_t count       = STACK_ARG(0);
    uint32_t handles_va  = STACK_ARG(1);
    uint32_t wait_type   = STACK_ARG(2);
    uint32_t wait_mode   = STACK_ARG(3);   /* KernelMode / UserMode */
    uint32_t alertable   = STACK_ARG(4);
    uint32_t timeout_ptr = STACK_ARG(5);

    (void)wait_mode;
    HANDLE   handles[MAXIMUM_WAIT_OBJECTS];
    uint32_t i;

    if (count == 0 || count > MAXIMUM_WAIT_OBJECTS || !handles_va) {
        g_eax = 0xC000000Du;             /* STATUS_INVALID_PARAMETER */
        return;
    }
    for (i = 0; i < count; i++)
        handles[i] = bridge_resolve_handle(BRIDGE_MEM32(handles_va + i * 4));

    {
        static int logged;
        if (logged++ < 20) {
            fprintf(stderr, "  [KERNEL] NtWaitForMultipleObjectsEx: count=%u type=%u timeout=%s\n",
                    count, wait_type, timeout_ptr ? "finite" : "INFINITE");
            for (i = 0; i < count; i++)
                fprintf(stderr, "      [%u] token=0x%08X host=%p\n", i,
                        BRIDGE_MEM32(handles_va + i * 4), handles[i]);
            fflush(stderr);
        }
    }

    g_eax = (uint32_t)xbox_NtWaitForMultipleObjectsEx(
        count, handles, wait_type, (KPROCESSOR_MODE)wait_mode, (BOOLEAN)alertable,
        XBOX_TO_NATIVE(timeout_ptr));
}

/*
 * Takes an Xbox VA, so the native pointer has to be formed before the query --
 * an unbridged 0 return reads as PAGE_NOACCESS. Halo walks all 22 MB of its
 * physical memory map asserting every page is PAGE_READWRITE
 * (physical_memory_map.c:77), so a zero here stops startup on the first page.
 */
static void bridge_MmQueryAddressProtect(void)
{
    uint32_t address = STACK_ARG(0);

    g_eax = address ? (uint32_t)xbox_MmQueryAddressProtect(XBOX_TO_NATIVE(address))
                    : 0;
}

/* ── NtUserIoApcDispatcher (ordinal 232) ─────────────────── */
/*
 * The kernel side of XAPI's ReadFileEx/WriteFileEx. XAPI passes *this* as the
 * ApcRoutine to NtReadFile and puts the title's completion routine in
 * ApcContext, so the dispatcher's only job is to call it with Win32 argument
 * shape:
 *
 *   VOID CALLBACK Completion(DWORD dwErrorCode,
 *                            DWORD dwNumberOfBytesTransfered,
 *                            LPOVERLAPPED lpOverlapped)   // __stdcall, ret 12
 *
 * lpOverlapped is the IO_STATUS_BLOCK pointer: an NT OVERLAPPED begins with
 * Internal/InternalHigh, which is exactly a IO_STATUS_BLOCK, so the title's
 * OVERLAPPED and the block it handed to NtReadFile are the same address.
 * Halo's cache_files_windows completion relies on that -- it reads its own
 * field at lpOverlapped+0x10 and sets the flag the setup loop polls.
 */
static void bridge_NtUserIoApcDispatcher(void)
{
    uint32_t apc_context = STACK_ARG(0);
    uint32_t iostatus    = STACK_ARG(1);
    uint32_t status      = iostatus ? BRIDGE_MEM32(iostatus) : 0;
    uint32_t information = iostatus ? BRIDGE_MEM32(iostatus + 4) : 0;
    recomp_func_t fn;

    fn = recomp_lookup(apc_context);
    if (!fn) fn = recomp_lookup_manual(apc_context);
    if (!fn) {
        fprintf(stderr, "  [KERNEL] NtUserIoApcDispatcher: completion routine "
                "0x%08X not in dispatch\n", apc_context);
        fflush(stderr);
        g_eax = 0;
        return;
    }

    /* __stdcall, right-to-left. The callee's `ret 12` consumes the dummy
     * return address and all three arguments, so g_esp needs no fixup here. */
    g_esp -= 4; BRIDGE_MEM32(g_esp) = iostatus;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = information;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = (status == 0) ? 0 : status;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
    fn();

    g_eax = 0;
}

/* ── KeDelayExecutionThread (ordinal 99) ─────────────────── */
/* Unbridged this returned instantly, turning every "sleep and retry" in the
 * title into a hot spin. Halo's cache-partition setup retries this way. */
static void bridge_KeDelayExecutionThread(void)
{
    uint32_t wait_mode    = STACK_ARG(0);
    uint32_t alertable    = STACK_ARG(1);
    uint32_t interval_ptr = STACK_ARG(2);


    g_eax = (uint32_t)xbox_KeDelayExecutionThread(
        (KPROCESSOR_MODE)wait_mode, (BOOLEAN)alertable,
        XBOX_TO_NATIVE(interval_ptr));
}

/* ── KeBugCheck (ordinal 95) / KeBugCheckEx (96) ─────────── */
/*
 * The title asking the kernel to die. Unbridged this returned 0 and execution
 * carried on into whatever the bug check was there to prevent, so the real
 * failure surfaced later somewhere unrelated. Report the code and stop
 * pretending the call succeeded.
 */
static void bridge_KeBugCheck(void)
{
    fprintf(stderr, "  [KERNEL] *** KeBugCheck: code=0x%08X ***\n",
            STACK_ARG(0));
    fflush(stderr);
    g_eax = 0;
}

static void bridge_KeBugCheckEx(void)
{
    fprintf(stderr, "  [KERNEL] *** KeBugCheckEx: code=0x%08X "
            "(0x%08X, 0x%08X, 0x%08X, 0x%08X) ***\n",
            STACK_ARG(0), STACK_ARG(1), STACK_ARG(2),
            STACK_ARG(3), STACK_ARG(4));
    fflush(stderr);
    g_eax = 0;
}

/* ── NtYieldExecution (ordinal 238) ──────────────────────── */
static void bridge_NtYieldExecution(void)
{
    g_eax = (uint32_t)xbox_NtYieldExecution();
}

/* ── MmGetPhysicalAddress (ordinal 173) ──────────────────── */
static void bridge_MmGetPhysicalAddress(void)
{
    uint32_t addr = STACK_ARG(0);
    /* Calls the same xbox_* the thunk table exposes, so there is one
     * implementation rather than two that have to be kept in agreement.
     * Note this bridge itself is not covered by any test: bridge functions
     * are static and driven by guest CPU state, and nothing in tests/ can
     * reach them. */
    g_eax = (uint32_t)xbox_MmGetPhysicalAddress((PVOID)(uintptr_t)addr);
    if (getenv("RECOMP_APU_SSL_TRACE")) {
        fprintf(stderr, "  [DMAIDENT] MmGetPhysicalAddress from=%08X guest=%08X physical=%08X\n",
                g_xbox_kernel_caller, addr, g_eax);
        fflush(stderr);
    }
}

/* ── MmSetAddressProtect (ordinal 182) ───────────────────── */
static void bridge_MmSetAddressProtect(void)
{
    uint32_t addr = STACK_ARG(0);
    uint32_t size = STACK_ARG(1);
    uint32_t prot = STACK_ARG(2);

    xbox_MmSetAddressProtect(XBOX_TO_NATIVE(addr), size, prot);
    g_eax = 0;
}

/* ── AvSetDisplayMode (ordinal 3) ────────────────────────── */
static void bridge_AvSetDisplayMode(void)
{
    uint32_t addr = STACK_ARG(0);
    uint32_t step = STACK_ARG(1);
    uint32_t mode = STACK_ARG(2);
    uint32_t format = STACK_ARG(3);
    uint32_t pitch = STACK_ARG(4);
    uint32_t fb = STACK_ARG(5);

    /* The framebuffer the display is meant to scan out, and the format it is
     * in. This is the only place the address is stated: the title never writes
     * PCRTC_START itself, so without this there is nothing that says where the
     * guest believes its picture is. */
    fprintf(stderr, "  [AV] SetDisplayMode mode=0x%08X format=0x%08X"
                    " pitch=%u fb=0x%08X\n", mode, format, pitch, fb);
    fflush(stderr);

    {
        /* Point the framebuffer window at whatever the title just set, and
         * start it on the first display mode -- before that there is nothing
         * to show and no pitch to interpret it with. */
        extern void xbox_FramebufferWindowSet(uint32_t, uint32_t);
        extern void xbox_FramebufferWindowStart(void);
        uint32_t fb_va = fb;

        /* AvSetDisplayMode reports the scanout address the way the CRTC wants
         * it -- a physical address. The window has to read it the way the CPU
         * sees it. Where the title allocated says which: a framebuffer from
         * MmAllocateContiguousMemory lives in the window at XBOX_CONTIG_BASE,
         * so physical P is visible at XBOX_CONTIG_BASE + P. Reading P
         * directly lands in the loaded image instead, which is why the window
         * showed black while the executor was clearing and rasterising
         * correctly a few megabytes away. */
        if (fb_va && fb_va < XBOX_CONTIG_SIZE)
            fb_va = XBOX_CONTIG_BASE + fb_va;

        xbox_FramebufferWindowSet(fb_va, pitch);
        xbox_FramebufferWindowStart();

        /* Record the resolved address, not the physical one the title passed.
         * Every reader of this wants to read guest memory with it, and the
         * physical form lands in the loaded image -- so the checksum probe
         * reported an unchanging zero while the title was drawing correctly a
         * few megabytes away, and so did every dump that asked for "the"
         * framebuffer. Resolving once here is the fix; resolving in each
         * caller is how there came to be two of them disagreeing. */
        xbox_SetDisplayFramebuffer(fb_va, pitch);
    }
    xbox_AvSetDisplayMode(XBOX_TO_NATIVE(addr), step, mode, format, pitch, fb);
    g_eax = 0;
}

/* ── PsTerminateSystemThread (ordinal 258) ───────────────
 * VOID PsTerminateSystemThread(NTSTATUS ExitStatus)
 *
 * On real Xbox, this terminates the calling thread (never returns).
 * Both main and worker roots leave through the common continuation cleanup.
 */
static void bridge_PsTerminateSystemThread(void)
{
    uint32_t exit_status = STACK_ARG(0);
    RecompContFrame frame;
    const RecompContLocal locals[]={{"exit_status",&exit_status,sizeof exit_status}};
    int tracked=recomp_cont_active();
    uint64_t resume=tracked?kernel_native_cont_enter(&frame,KERNEL_CONT_TERMINATE,locals,1):0;
    if(!resume) {
        fprintf(stderr, "  [KERNEL] PsTerminateSystemThread: status=0x%08X%s\n",
                exit_status, g_is_spawned_thread ? " (worker)" : " (main)");
        fflush(stderr);
    }
    ke_rundown_current_thread();
    BRIDGE_MEM32(ke_current_thread()+0x120)=exit_status;
    g_eax = exit_status;
    if(tracked)recomp_cont_leave(&frame);
    checkpoint_terminate_thread();
}

/* ── HalReadSMCTrayState (ordinal 47) ─────────────────────
 * VOID HalReadSMCTrayState(PDWORD TrayState, PDWORD TrayStateChangeCount)
 *
 * Returns DVD tray state. 0x10 = no disc, 0x14 = tray closed with disc.
 */
static void bridge_HalReadSMCTrayState(void)
{
    uint32_t state_ptr = STACK_ARG(0);
    uint32_t count_ptr = STACK_ARG(1);

    if (state_ptr) BRIDGE_MEM32(state_ptr) = 0x10;  /* No disc */
    if (count_ptr) BRIDGE_MEM32(count_ptr) = 0;
    g_eax = 0;
}

/* -- KeInsertQueueDpc (ordinal 119) ------------------------
 * BOOLEAN KeInsertQueueDpc(PKDPC Dpc, PVOID SystemArgument1,
 *                          PVOID SystemArgument2)
 *
 * The original timer worker drains the pending queue after device IRQ
 * delivery, then invokes each deferred routine at DISPATCH_LEVEL with
 * ownership of the same guest CPU used by the game's threads.
 */
/* Run a DPC's deferred routine on the calling thread.
 *
 * VOID DeferredRoutine(PKDPC, PVOID Context, PVOID Arg1, PVOID Arg2),
 * __stdcall: its `ret 16` consumes the dummy return address and all four
 * arguments, so g_esp needs no fixup afterwards. The caller must already have
 * a guest stack -- true on a guest thread, and true on a host thread that has
 * taken a worker slice.
 *
 * Returns 1 if the routine was found and called.
 */
static volatile LONG g_dpc_cpu_waiters;
typedef struct GuestDpcCall {
    RecompContFrame frame;
    uint32_t dpc,routine,context,arg1,arg2,pcr,stack,old_chain,old_esp,old_ebp,old_seh;
    int old_irql,release_cpu;
} GuestDpcCall;
static void kernel_dpc_call_leave(RecompContFrame *frame) {
    GuestDpcCall *call=(GuestDpcCall*)frame;
    if(frame->retained || !call->stack)return;
    BRIDGE_MEM32(call->pcr)=call->old_chain;
    BRIDGE_MEM32(call->pcr+0x58)=0;
    g_esp=call->old_esp;g_ebp=call->old_ebp;g_seh_ebp=call->old_seh;
    xbox_IrqlLeaveInterrupt(call->old_irql);
    call->stack=0;
}
static int kernel_run_dpc(uint32_t dpc_va, uint32_t arg1, uint32_t arg2)
{
    uint32_t routine=0, context=0;
    recomp_func_t fn;
    int release_cpu=0;
    if(recomp_cont_recovering(KERNEL_CONT_DPC))goto activation;

    if (!dpc_va)
        return 0;
    release_cpu = !guest_cpu_is_held();
    if (release_cpu) {
        InterlockedIncrement(&g_dpc_cpu_waiters);
        guest_cpu_join();
        InterlockedDecrement(&g_dpc_cpu_waiters);
    }
    routine = BRIDGE_MEM32(dpc_va + 12);
    context = BRIDGE_MEM32(dpc_va + 16);
    if (!routine) {
        if (release_cpu) guest_cpu_part();
        return 0;
    }

    BRIDGE_MEM32(dpc_va + 20) = arg1;
    BRIDGE_MEM32(dpc_va + 24) = arg2;

    if(routine==KERNEL_TIMER_DPC) {
        KIRQL old=xbox_KfRaiseIrql(DISPATCH_LEVEL);
        if(BRIDGE_MEM8(context+0x78))ke_insert_apc(context+0x28,arg1,arg2);
        xbox_KfLowerIrql(old);
        if(release_cpu)guest_cpu_part();return 1;
    }

    fn = recomp_lookup(routine);
    if (!fn) fn = recomp_lookup_manual(routine);
    if (!fn) {
        fprintf(stderr, "  [KERNEL] DPC routine 0x%08X not in dispatch\n",
                routine);
        fflush(stderr);
        if (release_cpu) guest_cpu_part();
        return 0;
    }


activation: ;
    GuestDpcCall call={0};
    call.dpc=dpc_va;call.routine=routine;call.context=context;call.arg1=arg1;call.arg2=arg2;
    call.release_cpu=release_cpu;
    const RecompContLocal locals[]={
#define DPC_LOCAL(x) {#x,&call.x,sizeof call.x}
        DPC_LOCAL(dpc),DPC_LOCAL(routine),DPC_LOCAL(context),DPC_LOCAL(arg1),DPC_LOCAL(arg2),
        DPC_LOCAL(pcr),DPC_LOCAL(stack),DPC_LOCAL(old_chain),
        DPC_LOCAL(old_esp),DPC_LOCAL(old_ebp),DPC_LOCAL(old_seh),DPC_LOCAL(old_irql),DPC_LOCAL(release_cpu)
#undef DPC_LOCAL
    };
    int tracked=recomp_cont_active();
    uint64_t resume=tracked?kernel_native_cont_enter(&call.frame,KERNEL_CONT_DPC,locals,sizeof locals/sizeof locals[0]):0;
    if(tracked)call.frame.on_leave=kernel_dpc_call_leave;
    if(!resume) {
        call.pcr=XBOX_FS_BASE;call.old_esp=g_esp;call.old_ebp=g_ebp;call.old_seh=g_seh_ebp;
        call.old_chain=BRIDGE_MEM32(call.pcr);
        if(BRIDGE_MEM32(call.pcr+0x58))recomp_cont_invalid();
        call.stack=BRIDGE_MEM32(call.pcr+0x5c);
        if(!call.stack) {fprintf(stderr,"[STOP] DPC has no guest stack\n");exit(5);}
        BRIDGE_MEM32(call.pcr)=UINT32_MAX;
        BRIDGE_MEM32(call.pcr+0x5c)=call.stack;
        g_esp=call.stack;
        g_esp-=4;BRIDGE_MEM32(g_esp)=call.old_esp;
        g_esp-=4;BRIDGE_MEM32(g_esp)=0;
        BRIDGE_MEM32(call.pcr+0x58)=g_esp;
        g_esp-=4;BRIDGE_MEM32(g_esp)=call.arg2;
        g_esp-=4;BRIDGE_MEM32(g_esp)=call.arg1;
        g_esp-=4;BRIDGE_MEM32(g_esp)=call.context;
        g_esp-=4;BRIDGE_MEM32(g_esp)=call.dpc;
        g_esp-=4;BRIDGE_MEM32(g_esp)=0;
        call.old_irql=xbox_IrqlEnterInterrupt(2);
    }
    fn=recomp_lookup(call.routine);if(!fn)fn=recomp_lookup_manual(call.routine);
    if(!fn)recomp_cont_invalid();
    fn();
    if(tracked)recomp_cont_leave(&call.frame);else kernel_dpc_call_leave(&call.frame);
    if(call.release_cpu)guest_cpu_part();
    return 1;
}

/* -- KeSynchronizeExecution (ordinal 153) ------------------
 * BOOLEAN KeSynchronizeExecution(PKINTERRUPT Interrupt,
 *                                PKSYNCHRONIZE_ROUTINE SynchronizeRoutine,
 *                                PVOID SynchronizeContext)
 *
 * Runs a routine while holding the interrupt's spinlock at the ISR's IRQL,
 * which is how a driver touches its hardware from anywhere that is not the
 * ISR without racing the ISR. Unbridged it returned 0 and the routine never
 * ran -- the same failure as an unqueued DPC, and just as quiet: the driver
 * asks for exclusive access, is told it did not get it, and skips the work.
 *
 * The original single guest CPU serializes this callback with ISR delivery.
 * Its IRQL is raised to the registered device level for the entire callback,
 * then restored to the caller's level.
 */
static void bridge_KeSynchronizeExecution(void)
{
    uint32_t interrupt = STACK_ARG(0);
    uint32_t routine = STACK_ARG(1);
    uint32_t context = STACK_ARG(2);
    recomp_func_t fn;

    fn = routine ? recomp_lookup(routine) : NULL;
    if (!fn && routine) fn = recomp_lookup_manual(routine);
    if (!fn) {
        fprintf(stderr, "  [KERNEL] KeSynchronizeExecution: routine 0x%08X "
                        "not in dispatch\n", routine);
        fflush(stderr);
        g_eax = 0;
        return;
    }

    /* BOOLEAN SynchronizeRoutine(PVOID Context), __stdcall. Its `ret 4` takes
     * the dummy return address and the argument, so g_esp needs no fixup. */
    int saved_irql = xbox_IrqlEnterInterrupt((int)BRIDGE_MEM32(interrupt + 12));
    g_esp -= 4; BRIDGE_MEM32(g_esp) = context;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
    fn();
    xbox_IrqlLeaveInterrupt(saved_irql);
    /* g_eax is whatever the routine returned, which is this call's result. */
}

/* KeRemoveQueueDpc removes a callback that has not been claimed by the
 * worker on the guest CPU. Inserted is cleared before callback execution. */
static void bridge_KeRemoveQueueDpc(void);   /* defined with the queue */

/* The pending DPC queue.
 *
 * These used to run inline, on whichever thread queued them, before the caller
 * returned. That is not what a DPC is: an interrupt service routine masks its
 * source and queues, and the deferred routine runs afterwards at DISPATCH_LEVEL
 * -- after the ISR has returned. Running it inline inverts that, so a routine
 * that re-enables interrupts does it while the ISR that masked them is still on
 * the stack, and a driver's state machine re-enters itself from inside its own
 * interrupt.
 *
 * Queued properly now, and drained by the timer thread, which is the one thread
 * here that already has a guest stack and a TIB and runs nothing else urgent.
 *
 * This single-processor queue runs each routine at DISPATCH_LEVEL. A callback
 * may enqueue further work after its own Inserted flag has been cleared.
 */
#define XBOX_MAX_PENDING_DPC 64
typedef struct { uint32_t dpc, arg1, arg2; } PendingDpc;
static PendingDpc *g_dpc_queue;
static LONG g_dpc_capacity;
static volatile LONG g_dpc_head, g_dpc_tail;

/* The queue is fed from several host threads at once -- the USB and APU
 * controller threads raise interrupts whose ISRs queue DPCs, and the title
 * queues its own -- and was an unlocked ring: two inserts could take the same
 * slot and one DPC was lost. A lost USB DPC is a done queue the driver never
 * acknowledges; the controller then completes nothing more and the pad goes
 * dead mid-game, which is what it did.
 *
 * And a DPC already queued is not queued twice: the kernel keeps an Inserted
 * flag in the KDPC (+2) and KeInsertQueueDpc returns FALSE while it is set.
 * Running a driver's DPC twice for one interrupt makes it walk a done list it
 * has already consumed. */
static CRITICAL_SECTION g_dpc_lock;
static INIT_ONCE g_dpc_lock_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK dpc_lock_init(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&g_dpc_lock);
    g_dpc_queue=calloc(XBOX_MAX_PENDING_DPC,sizeof *g_dpc_queue);
    if(!g_dpc_queue) { fprintf(stderr,"[STOP] DPC queue allocation failed\n");exit(5); }
    g_dpc_capacity=XBOX_MAX_PENDING_DPC;
    return TRUE;
}

static void dpc_lock(void)
{
    InitOnceExecuteOnce(&g_dpc_lock_once, dpc_lock_init, NULL, NULL);
    EnterCriticalSection(&g_dpc_lock);
}

static void bridge_KeInsertQueueDpc(void)
{
    uint32_t dpc  = STACK_ARG(0);
    uint32_t arg1 = STACK_ARG(1);
    uint32_t arg2 = STACK_ARG(2);
    LONG tail, next;

    if (!dpc) { g_eax = 0; return; }

    dpc_lock();
    if (BRIDGE_MEM8(dpc + 2)) {                 /* already queued */
        LeaveCriticalSection(&g_dpc_lock);
        g_eax = 0;
        return;
    }
    tail = g_dpc_tail;
    next = (tail + 1) % g_dpc_capacity;
    if (next == g_dpc_head) {
        /* KeInsertQueueDpc only rejects an already inserted object. Grow the
         * original queue, preserving the order and identities of its entries. */
        if(g_dpc_capacity>INT32_MAX/2) { fprintf(stderr,"[STOP] DPC queue capacity overflow\n");exit(5); }
        LONG capacity=g_dpc_capacity*2,count=0;
        PendingDpc *queue=calloc((size_t)capacity,sizeof *queue);
        if(!queue) { fprintf(stderr,"[STOP] DPC queue allocation failed\n");exit(5); }
        for(LONG i=g_dpc_head;i!=g_dpc_tail;i=(i+1)%g_dpc_capacity)
            if(g_dpc_queue[i].dpc) queue[count++]=g_dpc_queue[i];
        free(g_dpc_queue);g_dpc_queue=queue;g_dpc_capacity=capacity;
        g_dpc_head=0;tail=count;next=count+1;
    }
    g_dpc_queue[tail].dpc  = dpc;
    g_dpc_queue[tail].arg1 = arg1;
    g_dpc_queue[tail].arg2 = arg2;
    BRIDGE_MEM32(dpc + 20) = arg1;
    BRIDGE_MEM32(dpc + 24) = arg2;
    BRIDGE_MEM8(dpc + 2) = 1;
    g_dpc_tail = next;
    LeaveCriticalSection(&g_dpc_lock);
    g_eax = 1;
}

/* Cancel a queued DPC: take it out of the queue if it is still there. */
static int kernel_remove_dpc(uint32_t dpc)
{
    LONG i;

    int removed=0;
    if (!dpc)
        return 0;
    dpc_lock();
    if (BRIDGE_MEM8(dpc + 2)) {
        for (i = g_dpc_head; i != g_dpc_tail; i = (i + 1) % g_dpc_capacity)
            if (g_dpc_queue[i].dpc == dpc)
                g_dpc_queue[i].dpc = 0;         /* drained as a no-op */
        BRIDGE_MEM8(dpc + 2) = 0;
        removed=1;
    }
    LeaveCriticalSection(&g_dpc_lock);return removed;
}
static void bridge_KeRemoveQueueDpc(void) {g_eax=(uint32_t)kernel_remove_dpc(STACK_ARG(0));}

/* Call a connected interrupt service routine.
 *
 * BOOLEAN ServiceRoutine(PKINTERRUPT, PVOID ServiceContext), __stdcall: its
 * `ret 8` takes the dummy return address and both arguments, so g_esp needs no
 * fixup. The caller must already have a guest stack and a TIB, which the timer
 * thread has.
 *
 * Returns what the routine returned -- an ISR that does not recognise the
 * interrupt returns FALSE, and that is worth seeing rather than assuming.
 */
uint32_t xbox_GetConnectedInterrupt(uint32_t vector);   /* defined below */

static int kernel_raise_interrupt(uint32_t vector)
{
    /* Recheck at each delivery, including another service of a held level. */
    if (xbox_IrqlBlocksInterrupts())
        return 0;
    uint32_t kint = xbox_GetConnectedInterrupt(vector);
    uint32_t routine, context;
    recomp_func_t fn;

    if (!kint)
        return 0; /* An unconnected bus interrupt is masked. */
    routine = BRIDGE_MEM32(kint + 0);
    context = BRIDGE_MEM32(kint + 4);
    if (!routine)
        return -1;
    fn = recomp_lookup(routine);
    if (!fn) fn = recomp_lookup_manual(routine);
    if (!fn) {
        fprintf(stderr, "[STOP] connected ISR not found in dispatch: 0x%08X\n", routine);
        return -1;
    }

    g_esp -= 4; BRIDGE_MEM32(g_esp) = context;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = kint;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
    { int _if = xbox_CpuEnterInterrupt();
      int _irql = xbox_IrqlEnterInterrupt((int)BRIDGE_MEM32(kint + 12));
      fn(); xbox_IrqlLeaveInterrupt(_irql); xbox_CpuLeaveInterrupt(_if); }
    return (int)(g_eax & 1u);
}

/* Deliver the connected GPU's actual aggregated level on the existing
 * timer worker, which already owns a guest stack and TIB. No display event
 * or completion bit is fabricated. Device consumption resumes after the
 * original ISR returns, so its MMIO acknowledgements cannot run more guest
 * methods halfway through notification handling. */
#define NV2A_VECTOR 3u
extern void nv2a_pb_request_progress(void);
static void kernel_nv2a_tick(void)
{
    nv2a_pb_request_progress();
    NV2AState *gpu = nv2a_get_state();
    if (!gpu) return;
    qemu_mutex_lock(&gpu->pfifo.lock);
    nv2a_vblank_tick(gpu);
    int deliver = !xbox_IrqlBlocksInterrupts() && gpu->pmc.pending_interrupts &&
                  gpu->pmc.enabled_interrupts && xbox_GetConnectedInterrupt(NV2A_VECTOR);
    qemu_mutex_unlock(&gpu->pfifo.lock);
    if (!deliver) return;
    /* The guest CPU precedes the device lock, as in APU delivery. A driver
     * cannot disconnect and free an interrupt while its original ISR runs. */
    InterlockedIncrement(&g_dpc_cpu_waiters);
    guest_cpu_join();
    InterlockedDecrement(&g_dpc_cpu_waiters);
    qemu_mutex_lock(&gpu->pfifo.lock);
    if (!xbox_IrqlBlocksInterrupts() && gpu->pmc.pending_interrupts && gpu->pmc.enabled_interrupts &&
        xbox_GetConnectedInterrupt(NV2A_VECTOR)) {
        gpu->irq_in_service = true;
        int claimed = kernel_raise_interrupt(NV2A_VECTOR);
        gpu->irq_in_service = false;
        if (claimed < 0) {
            fprintf(stderr, "[STOP] NV2A IRQ has no callable connected ISR\n");
            exit(5);
        }
        static unsigned shown;
        if (shown++ < 8)
            fprintf(stderr, "[NV2A] IRQ -> original ISR %s; PGRAPH pending=0x%08X\n",
                    claimed ? "claimed" : "declined", gpu->pgraph.pending_interrupts);
        nv2a_fifo_kick(gpu);
    }
    qemu_mutex_unlock(&gpu->pfifo.lock);
    guest_cpu_part();
}

static void kernel_start_timer_worker(void);

/* Run whatever is queued. Called from the timer thread, which has the guest
 * stack and TIB that a deferred routine needs. */
static void kernel_drain_dpcs(void)
{
    for (;;) {
        PendingDpc d;
        dpc_lock();
        int pending = g_dpc_head != g_dpc_tail;
        LeaveCriticalSection(&g_dpc_lock);
        if (!pending) break;

        /* Claim the callback only while owning the guest CPU. Until then a
         * guest caller can still cancel, reinitialize or retire the object. */
        InterlockedIncrement(&g_dpc_cpu_waiters);
        guest_cpu_join();
        InterlockedDecrement(&g_dpc_cpu_waiters);
        dpc_lock();
        if (g_dpc_head == g_dpc_tail) {
            LeaveCriticalSection(&g_dpc_lock);
            guest_cpu_part();
            break;
        }
        d = g_dpc_queue[g_dpc_head];
        g_dpc_head = (g_dpc_head + 1) % g_dpc_capacity;
        if (d.dpc) BRIDGE_MEM8(d.dpc + 2) = 0;
        LeaveCriticalSection(&g_dpc_lock);
        if (d.dpc) kernel_run_dpc(d.dpc, d.arg1, d.arg2);
        guest_cpu_part();
    }
}


/* ── KeInitializeDpc (ordinal 107) ────────────────────────
 * VOID KeInitializeDpc(PKDPC Dpc, PKDEFERRED_ROUTINE DeferredRoutine,
 *                       PVOID DeferredContext)
 *
 * Initializes a DPC object. The 32-bit Xbox KDPC structure is 28 bytes.
 * We zero it and set the routine and context pointers.
 */
static void bridge_KeInitializeDpc(void)
{
    uint32_t dpc_va = STACK_ARG(0);
    uint32_t routine = STACK_ARG(1);
    uint32_t context = STACK_ARG(2);

    if (!bridge_buf_ok(dpc_va, 28, "KeInitializeDpc")) {
        g_eax = 0;
        return;
    }

    memset(XBOX_TO_NATIVE(dpc_va), 0, 28);

    /* Set Type (0x13 = DpcObject) and fields */
    BRIDGE_MEM16(dpc_va + 0) = 0x13;   /* Type */
    BRIDGE_MEM32(dpc_va + 12) = routine; /* DeferredRoutine */
    BRIDGE_MEM32(dpc_va + 16) = context; /* DeferredContext */
    g_eax = 0;
}

/* NV2A interrupt registration (ordinals 44, 98, 109).
 * The existing kernel worker invokes the connected guest ISR from actual
 * NV2A pending/enabled state; display timing is not generated here. */

/* ULONG HalGetInterruptVector(ULONG BusInterruptLevel, PKIRQL Irql) */
#define XBOX_IRQ_BASE 0x30u
#define XBOX_MAX_BUS_IRQ 27u
static void bridge_HalGetInterruptVector(void)
{
    uint32_t level = STACK_ARG(0);
    g_eax = 0;
    if (level <= XBOX_MAX_BUS_IRQ) {
        if (STACK_ARG(1))
            BRIDGE_MEM8(STACK_ARG(1)) = (uint8_t)(XBOX_MAX_BUS_IRQ - level);
        g_eax = XBOX_IRQ_BASE + level;
    }
}

/* VOID KeInitializeInterrupt(PKINTERRUPT, ServiceRoutine, ServiceContext,
 *                            Vector, Irql, InterruptMode, ShareVector) */
static void bridge_KeInitializeInterrupt(void)
{
    uint32_t interrupt_va = STACK_ARG(0);
    if (!bridge_buf_ok(interrupt_va, 112, "KeInitializeInterrupt")) {
        g_eax = 0;
        return;
    }
    /* KINTERRUPT has a 24-byte header and 22 guest dispatch-code words. */
    memset(XBOX_TO_NATIVE(interrupt_va), 0, 112);
    BRIDGE_MEM32(interrupt_va) = STACK_ARG(1);
    BRIDGE_MEM32(interrupt_va + 4) = STACK_ARG(2);
    BRIDGE_MEM32(interrupt_va + 8) = STACK_ARG(3) - XBOX_IRQ_BASE;
    BRIDGE_MEM32(interrupt_va + 12) = STACK_ARG(4);
    BRIDGE_MEM8(interrupt_va + 17) = (uint8_t)STACK_ARG(6);
    BRIDGE_MEM8(interrupt_va + 18) = (uint8_t)STACK_ARG(5);
    g_eax = 0;
}

/* The interrupt objects a title has connected, by vector.
 *
 * KeInitializeInterrupt already writes the service routine, its context and
 * the vector into the guest KINTERRUPT; connecting is what says the title is
 * ready to be called on it. Recording that is what lets a device model raise
 * an interrupt at all -- without it the routine is written down in guest
 * memory and nothing on this side knows it is there.
 *
 * A small fixed table rather than a list: an Xbox has 26 interrupt vectors and
 * a title connects a handful, so the whole thing is smaller than the comment.
 */
#define XBOX_MAX_VECTORS 32
static uint32_t g_connected_isr[XBOX_MAX_VECTORS];   /* KINTERRUPT guest VA */

/* BOOLEAN KeConnectInterrupt(PKINTERRUPT Interrupt) */
static void bridge_KeConnectInterrupt(void)
{
    uint32_t interrupt_va = STACK_ARG(0);
    g_eax = 0;
    if (!interrupt_va || BRIDGE_MEM8(interrupt_va + 16))
        return;
    uint32_t level = BRIDGE_MEM32(interrupt_va + 8);
    if (level > XBOX_MAX_BUS_IRQ ||
        InterlockedCompareExchange((volatile LONG *)&g_connected_isr[level],
                                   (LONG)interrupt_va, 0) != 0)
        return;
    BRIDGE_MEM8(interrupt_va + 16) = 1;
    if (level == NV2A_VECTOR || level == 5 || level == 6)
        kernel_start_timer_worker();
    g_eax = 1;
}

/* The KINTERRUPT a title connected on this vector, or 0. Device models use it
 * to find the routine to call; the routine and its context are at +0 and +4. */
uint32_t xbox_GetConnectedInterrupt(uint32_t vector)
{
    return vector < XBOX_MAX_VECTORS ?
        (uint32_t)InterlockedCompareExchange((volatile LONG *)&g_connected_isr[vector], 0, 0) : 0;
}

/* ── MmClaimGpuInstanceMemory (ordinal 168) ───────────────
 * PVOID MmClaimGpuInstanceMemory(SIZE_T NumberOfBytes, SIZE_T *Padding)
 *
 * Reserves the GPU instance memory the NV2A keeps its object context in. On
 * hardware it sits at the very top of physical RAM, so the returned address is
 * the end of the contiguous window minus the request. D3D8 stores this and
 * indexes off it, so returning 0 (the unbridged default) had it building
 * pointers from a null base.
 *
 * MAXULONG_PTR means "claim everything left"; the console answers with the
 * default instance size rather than the whole of RAM.
 */
static void bridge_MmClaimGpuInstanceMemory(void)
{
    uint32_t bytes      = STACK_ARG(0);
    uint32_t padding_va = STACK_ARG(1);

    if (bytes == 0xFFFFFFFFu) {
        bytes = XBOX_GPU_INSTANCE_DEFAULT;
    }
    if (padding_va) {
        BRIDGE_MEM32(padding_va) = 0;
    }
    g_eax = XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE - bytes;
}

/* Original HAL registration is a guest-owned, priority-ordered list. */
VOID __stdcall xbox_HalRegisterShutdownNotification(PVOID registration,BOOLEAN Register)
{
    uint32_t object=io_guest_address(registration),head=XBOX_KERNEL_DATA_BASE+KDATA_SHUTDOWN_HEAD;
    if(!bridge_buf_ok(object,16,"shutdown registration")){fprintf(stderr,"[STOP] shutdown registration is not mapped guest memory\n");exit(5);}
    int held=guest_cpu_is_held(),was_thread=t_guest_thread;if(!held)guest_cpu_join();
    KIRQL old=xbox_KeRaiseIrqlToDpcLevel();uint32_t before=BRIDGE_MEM32(head);
    if(Register) {
        while(before!=head && (int32_t)BRIDGE_MEM32(object+4)<=(int32_t)BRIDGE_MEM32(before-4))before=BRIDGE_MEM32(before);
        ke_list_before(before,object+8);
    } else {
        while(before!=head && before!=object+8)before=BRIDGE_MEM32(before);
        if(before!=head)ke_list_unlink(before);
    }
    xbox_KfLowerIrql(old);
    if(!held){guest_cpu_part();t_guest_thread=was_thread;}
}

typedef struct GuestShutdownCall {
    RecompContFrame frame;
    uint32_t registration,routine,phase;
    int held,was_thread;
} GuestShutdownCall;

static void ke_shutdown_leave(RecompContFrame *frame)
{
    GuestShutdownCall *call=(GuestShutdownCall *)frame;
    if(frame->retained)return;
    if(!call->held && guest_cpu_is_held()){guest_cpu_part();t_guest_thread=call->was_thread;}
}

void xbox_HalNotifyShutdown(void)
{
    GuestShutdownCall call={0};call.held=guest_cpu_is_held();call.was_thread=t_guest_thread;
    const RecompContLocal locals[]={
        {"registration",&call.registration,4},{"routine",&call.routine,4},{"phase",&call.phase,4},
        {"held",&call.held,sizeof call.held},{"was_thread",&call.was_thread,sizeof call.was_thread}};
    int tracked=recomp_cont_active();
    uint64_t resume=tracked?kernel_native_cont_enter(&call.frame,KERNEL_CONT_SHUTDOWN,locals,sizeof locals/sizeof locals[0]):0;
    if(tracked)call.frame.on_leave=ke_shutdown_leave;
    if(!guest_cpu_is_held())guest_cpu_join();
    if(resume){if(call.phase!=1)recomp_cont_invalid();goto callback;}
    for(;;) {
        uint32_t head=XBOX_KERNEL_DATA_BASE+KDATA_SHUTDOWN_HEAD;
        KIRQL old=xbox_KeRaiseIrqlToDpcLevel();uint32_t entry=BRIDGE_MEM32(head);
        if(entry!=head){call.registration=entry-8;call.routine=BRIDGE_MEM32(call.registration);ke_list_unlink(entry);}
        xbox_KfLowerIrql(old);
        if(entry==head)break;
        call.phase=1;
callback:
        ke_call_guest(call.routine,&call.registration,1,0);
        call.phase=0;
    }
    if(tracked)recomp_cont_leave(&call.frame);else ke_shutdown_leave(&call.frame);
}

static void bridge_HalRegisterShutdownNotification(void)
{
    xbox_HalRegisterShutdownNotification(XBOX_TO_NATIVE(STACK_ARG(0)),(BOOLEAN)STACK_ARG(1));g_eax=0;
}

/* ── Kernel timers (ordinals 113, 149, 150, 97) ───────────
 *
 * Timers are waitable: KeInitializeTimerEx backs every guest KTIMER with a
 * Win32 event in the shadow table (manual reset for notification timers, auto
 * reset for synchronization timers), so a KeWaitForSingleObject on a timer
 * sleeps on a real object. KeSetTimer/KeSetTimerEx arm the polling table below.
 * Expiration signals the event before the DPC; cancellation removes the timer
 * without changing its existing signal state. */

/* ── KeInitializeTimerEx (ordinal 113) ────────────────────
 * VOID KeInitializeTimerEx(PKTIMER Timer, TIMER_TYPE Type)
 */
static int ke_initialize_timer(uint32_t timer,uint32_t type)
{
    memset(XBOX_TO_NATIVE(timer),0,40);
    ke_initialize_header(timer,(uint8_t)(8+(type&1)),10,0);
    HANDLE event=CreateEventW(NULL,type==0,FALSE,NULL);
    return event && ke_shadow_insert(timer,event);
}
static void bridge_KeInitializeTimerEx(void)
{
    if(!ke_initialize_timer(STACK_ARG(0),STACK_ARG(1))) {
        fprintf(stderr,"[STOP] timer dispatcher allocation failed\n");exit(5);
    }
    g_eax=0;
}

/* -- KeSetTimer / KeSetTimerEx (ordinal 149/150) ----------
 * BOOLEAN KeSetTimer(PKTIMER Timer, LARGE_INTEGER DueTime, PKDPC Dpc)
 * BOOLEAN KeSetTimerEx(PKTIMER Timer, LARGE_INTEGER DueTime, LONG Period,
 *                      PKDPC Dpc)
 *
 * These used to return FALSE and do nothing, on the grounds that timers were
 * not needed for basic execution. They are needed for more than that: a driver
 * that polls its hardware does it from a timer DPC, and one that never fires
 * is a state machine that never advances. Half-Life 2's USB stack sets one up
 * during XInitDevices and enumerates from it.
 *
 * A due time is in 100 ns units, negative for relative and positive for an
 * absolute time since 1601. Both forms use the existing virtual runtime clock,
 * which also supplies the deadline saved in the guest timer object.
 *
 * One worker scans the existing active timer table on a 10 ms tick. The
 * table grows with live timers; the tick resolution and periodic deadline
 * calculation remain those of the existing scheduler.
 */
#define XBOX_MAX_TIMERS 32
typedef struct {
    uint32_t timer_va;      /* the guest KTIMER, 0 for a free slot */
    uint32_t dpc_va;
    long long due_ms;       /* host tick when it fires */
    long      period_ms;    /* 0 for one-shot */
} XboxTimer;
static XboxTimer *g_timers;
static unsigned g_timer_capacity;
/* Derived active prefix; protected by g_timer_lock, not checkpoint bytes. */
static unsigned g_timer_scan_end;
static void kernel_trim_timer_scan(void) {
    while(g_timer_scan_end && !g_timers[g_timer_scan_end-1].timer_va)
        --g_timer_scan_end;
}
static CRITICAL_SECTION g_timer_lock;
static int g_timer_started;
static HANDLE g_timer_irq_event;
static void kernel_gpu_irq_notify(void *opaque, int level)
{
    if (level && !SetEvent((HANDLE)opaque)) {
        fprintf(stderr, "[STOP] GPU IRQ worker notification failed\n");
        exit(5);
    }
}

static void kernel_apu_tick(void)
{
    extern int mcpx_apu_irqs_pending(void);
    if (xbox_IrqlBlocksInterrupts() || !mcpx_apu_irqs_pending()) return;
    /* Own the existing guest CPU before the device lock, through every
     * service of this held level. Guest voice retirement must wait until
     * the original ISR has consumed the remaining acknowledgement. */
    InterlockedIncrement(&g_dpc_cpu_waiters);
    guest_cpu_join();
    InterlockedDecrement(&g_dpc_cpu_waiters);
    if (!xbox_IrqlBlocksInterrupts()) {
        extern void mcpx_apu_service_irqs(int (*deliver)(uint32_t));
        mcpx_apu_service_irqs(kernel_raise_interrupt);
    }
    guest_cpu_part();
}

static DWORD WINAPI kernel_timer_thread(LPVOID unused)
{
    xbox_CheckpointDeviceEnter(XBOX_CHECKPOINT_TIMER);
    int slot = xbox_worker_stack_alloc();

    (void)unused;
    if (slot < 0) {
        fprintf(stderr, "[STOP] timer/IRQ worker has no guest stack\n");
        fflush(stderr);
        exit(5);
    }
    g_esp = XBOX_WORKER_STACK_TOP(slot);
    {
        /* Its own TIB, for the same reason bridge_thread_main gives one to
         * every worker: a DPC routine with an SEH prologue reads fs:[0],
         * and a thread whose g_fs_base is zero faults inside the runtime
         * before the routine runs. */
        uint32_t tib = xbox_AllocThreadTib();
        if (!tib) {
            fprintf(stderr, "[STOP] timer/IRQ worker has no guest TIB\n");
            fflush(stderr);
            exit(5);
        }
        xbox_IrqlBindThreadPcr(tib);
    }

    xbox_CheckpointDeviceLeave(XBOX_CHECKPOINT_TIMER);
    for (;;) {
        long long now;
        int i;

        DWORD wake = WaitForSingleObject(g_timer_irq_event, 10);
        if (wake != WAIT_OBJECT_0 && wake != WAIT_TIMEOUT) {
            fprintf(stderr, "[STOP] timer/IRQ worker wait failed\n");
            exit(5);
        }
        xbox_CheckpointDeviceEnter(XBOX_CHECKPOINT_TIMER);
        xbox_UpdateKernelTick(g_xbox_mem_offset);
        kernel_nv2a_tick();    /* actual device IRQ level */
        kernel_apu_tick();
        kernel_drain_dpcs();   /* deferred work, before due timers */
        now = (long long)xbox_RuntimeTickCount64();

        for (i = 0; ; i++) {
            uint32_t dpc, fired_va;

            EnterCriticalSection(&g_timer_lock);
            if ((unsigned)i>=g_timer_scan_end) {
                LeaveCriticalSection(&g_timer_lock);
                break;
            }
            if (!g_timers[i].timer_va || now < g_timers[i].due_ms) {
                LeaveCriticalSection(&g_timer_lock);
                continue;
            }
            LeaveCriticalSection(&g_timer_lock);
            /* Same ordering as guest timer calls: CPU, then timer lock.
             * Cancellation can complete while the worker waits for the CPU. */
            InterlockedIncrement(&g_dpc_cpu_waiters);
            guest_cpu_join();
            InterlockedDecrement(&g_dpc_cpu_waiters);
            EnterCriticalSection(&g_timer_lock);
            now = (long long)xbox_RuntimeTickCount64();
            if (!g_timers[i].timer_va || now < g_timers[i].due_ms) {
                LeaveCriticalSection(&g_timer_lock);
                guest_cpu_part();
                continue;
            }
            fired_va = g_timers[i].timer_va;
            dpc = g_timers[i].dpc_va;
            if (g_timers[i].period_ms > 0) {
                g_timers[i].due_ms = now + g_timers[i].period_ms;
                uint64_t deadline = (uint64_t)g_timers[i].due_ms * 10000;
                BRIDGE_MEM32(fired_va + 16) = (uint32_t)deadline;
                BRIDGE_MEM32(fired_va + 20) = (uint32_t)(deadline >> 32);
            }
            else {
                g_timers[i].timer_va = 0;
                kernel_trim_timer_scan();
                BRIDGE_MEM8(fired_va + 3) = 0;
            }
            /* Expiration signals the object before dispatching its DPC. */
            BRIDGE_MEM32(fired_va + 4) = 1;
            HANDLE ev = ke_shadow_lookup(fired_va);
            if (ev) SetEvent(ev);
            LeaveCriticalSection(&g_timer_lock);

            /* Outside the lock: the routine can set or cancel timers. */
            if (dpc) {
                LARGE_INTEGER time;xbox_KeQuerySystemTime(&time);
                kernel_run_dpc(dpc,(uint32_t)time.QuadPart,(uint32_t)((uint64_t)time.QuadPart>>32));
            }
            guest_cpu_part();
        }
        xbox_CheckpointDeviceLeave(XBOX_CHECKPOINT_TIMER);
    }
}

static INIT_ONCE g_timer_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK kernel_start_timer_once(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    (void)once; (void)parameter; (void)context;
    InitializeCriticalSection(&g_timer_lock);
    g_timers=calloc(XBOX_MAX_TIMERS,sizeof *g_timers);
    if(!g_timers) { fprintf(stderr,"[STOP] timer table allocation failed\n");exit(5); }
    g_timer_capacity=XBOX_MAX_TIMERS;
    g_timer_irq_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!g_timer_irq_event) {
        fprintf(stderr, "[STOP] timer/IRQ wake event creation failed\n");
        exit(5);
    }
    NV2AState *gpu = nv2a_get_state();
    if (gpu) {
        gpu->parent_obj.irq_opaque = g_timer_irq_event;
        gpu->parent_obj.irq_notify = kernel_gpu_irq_notify;
        kernel_gpu_irq_notify(g_timer_irq_event, gpu->parent_obj.irq_level);
    }
    xbox_CheckpointDeviceCreating(XBOX_CHECKPOINT_TIMER);
    HANDLE worker = CreateThread(NULL, 0, kernel_timer_thread, NULL, 0, NULL);
    if (!worker) {
        fprintf(stderr, "[STOP] kernel timer/IRQ worker creation failed: %lu\n", GetLastError());
        exit(5);
    }
    g_timer_started = 1;
    CloseHandle(worker);
    return TRUE;
}

static void kernel_start_timer_worker(void)
{
    if (!InitOnceExecuteOnce(&g_timer_once, kernel_start_timer_once, NULL, NULL)) {
        fprintf(stderr, "[STOP] kernel timer/IRQ worker initialization failed\n");
        exit(5);
    }
}

/* Shared by KeSetTimer and KeSetTimerEx; period is 0 for the former. */
static void kernel_set_timer(uint32_t timer_va, long long due_100ns,
                             long period_ms, uint32_t dpc_va)
{
    uint64_t remaining;
    if (due_100ns < 0)
        remaining = 0u - (uint64_t)due_100ns;
    else {
        LARGE_INTEGER system_time;
        xbox_KeQuerySystemTime(&system_time);
        remaining = due_100ns > system_time.QuadPart ?
                    (uint64_t)(due_100ns - system_time.QuadPart) : 0;
    }
    long long delay_ms = (long long)(remaining / 10000 + (remaining % 10000 != 0));
    int i, free_slot = -1;
    uint32_t was_set = 0;

    kernel_start_timer_worker();

    EnterCriticalSection(&g_timer_lock);
    for (i = 0; i < (int)g_timer_scan_end; i++) {
        if (g_timers[i].timer_va == timer_va) { free_slot = i; was_set = 1; break; }
        if (!g_timers[i].timer_va && free_slot < 0) free_slot = i;
    }
    if(free_slot<0 && g_timer_scan_end<g_timer_capacity)
        free_slot=(int)g_timer_scan_end;
    if(free_slot<0) {
        if(g_timer_capacity>INT32_MAX/2) { fprintf(stderr,"[STOP] timer table capacity overflow\n");exit(5); }
        unsigned capacity=g_timer_capacity*2;
        XboxTimer *timers=realloc(g_timers,(size_t)capacity*sizeof *timers);
        if(!timers) { fprintf(stderr,"[STOP] timer table allocation failed\n");exit(5); }
        memset(timers+g_timer_capacity,0,(capacity-g_timer_capacity)*sizeof *timers);
        free_slot=(int)g_timer_capacity;g_timers=timers;g_timer_capacity=capacity;
    }
    if (free_slot >= 0) {
        g_timers[free_slot].timer_va  = timer_va;
        if((unsigned)free_slot>=g_timer_scan_end)g_timer_scan_end=(unsigned)free_slot+1;
        g_timers[free_slot].dpc_va    = dpc_va;
        g_timers[free_slot].due_ms    = (long long)xbox_RuntimeTickCount64() + delay_ms;
        g_timers[free_slot].period_ms = period_ms;
        BRIDGE_MEM8(timer_va + 1) = due_100ns >= 0;
        BRIDGE_MEM8(timer_va + 3) = 1;
        BRIDGE_MEM32(timer_va + 4) = 0;
        uint64_t deadline = (uint64_t)g_timers[free_slot].due_ms * 10000;
        BRIDGE_MEM32(timer_va + 16) = (uint32_t)deadline;
        BRIDGE_MEM32(timer_va + 20) = (uint32_t)(deadline >> 32);
        BRIDGE_MEM32(timer_va + 32) = dpc_va;
        BRIDGE_MEM32(timer_va + 36) = (uint32_t)period_ms;
        HANDLE ev = ke_shadow_lookup(timer_va);
        if (ev) ResetEvent(ev);
    }
    LeaveCriticalSection(&g_timer_lock);
    g_eax = was_set;
}

static void bridge_KeSetTimer(void)
{
/* LARGE_INTEGER is two stack slots. */
    long long due = (long long)((uint64_t)STACK_ARG(1)
                              | ((uint64_t)STACK_ARG(2) << 32));
    kernel_set_timer(STACK_ARG(0), due, 0, STACK_ARG(3));
}

static void bridge_KeSetTimerEx(void)
{
    long long due = (long long)((uint64_t)STACK_ARG(1)
                              | ((uint64_t)STACK_ARG(2) << 32));
    kernel_set_timer(STACK_ARG(0), due, (long)STACK_ARG(3), STACK_ARG(4));
}

/* Drop a timer so it stops firing. Returns whether it was armed. */
int xbox_kernel_cancel_timer(uint32_t timer_va)
{
    int i, was_set = 0;

    if (!g_timer_started)
        return 0;
    EnterCriticalSection(&g_timer_lock);
    for (i = 0; i < (int)g_timer_scan_end; i++)
        if (g_timers[i].timer_va == timer_va) {
            g_timers[i].timer_va = 0;
            BRIDGE_MEM8(timer_va + 3) = 0;
            was_set = 1;
        }
    kernel_trim_timer_scan();
    LeaveCriticalSection(&g_timer_lock);
    return was_set;
}

static void ke_cancel_timer_apc(uint32_t timer) {
    xbox_kernel_cancel_timer(timer);
    if(BRIDGE_MEM8(timer+0x78)) {
        BRIDGE_MEM8(timer+0x78)=0;ke_list_unlink(timer+0x6c);
        kernel_remove_dpc(timer+0x50);ke_remove_apc(timer+0x28);
        ke_dereference_object(timer);
    }
}
static void ke_timer_apc_kernel(uint32_t apc,uint32_t normal_pointer) {
    uint32_t timer=apc-0x28;
    if(!BRIDGE_MEM8(timer+0x78) || BRIDGE_MEM32(apc+4)!=ke_current_thread()) {
        BRIDGE_MEM32(normal_pointer)=0;return;
    }
    if(!BRIDGE_MEM32(timer+0x74)) {
        BRIDGE_MEM8(timer+0x78)=0;ke_list_unlink(timer+0x6c);ke_dereference_object(timer);
    }
}
static void ke_rundown_thread_timers(uint32_t thread) {
    uint32_t head=thread+0x124;
    while(BRIDGE_MEM32(head)!=head) {
        uint32_t timer=BRIDGE_MEM32(head)-0x6c;
        if(ke_reference_pointer(timer,0)<0)recomp_cont_invalid();
        ke_cancel_timer_apc(timer);ke_dereference_object(timer);
    }
}

typedef struct KernelSchedulerCheckpoint {
    uint32_t timer_started,timer_capacity,dpc_capacity;
    uint32_t isr[XBOX_MAX_VECTORS];
    LONG head,tail;
} KernelSchedulerCheckpoint;
int xbox_SaveKernelScheduler(FILE *out)
{
    if(!out || !xbox_CheckpointFrozen()) return 0;
    KernelSchedulerCheckpoint saved={0};saved.timer_started=g_timer_started;
    if(g_timer_started) EnterCriticalSection(&g_timer_lock);
    dpc_lock();saved.timer_capacity=g_timer_capacity;saved.dpc_capacity=g_dpc_capacity;
    saved.head=g_dpc_head;saved.tail=g_dpc_tail;
    memcpy(saved.isr,g_connected_isr,sizeof g_connected_isr);
    int ok=fwrite("XBSCHED",7,1,out)==1 && fwrite(&saved,sizeof saved,1,out)==1 &&
        (!saved.timer_capacity || fwrite(g_timers,sizeof *g_timers,saved.timer_capacity,out)==saved.timer_capacity) &&
        fwrite(g_dpc_queue,sizeof *g_dpc_queue,saved.dpc_capacity,out)==saved.dpc_capacity;
    LeaveCriticalSection(&g_dpc_lock);
    if(g_timer_started) LeaveCriticalSection(&g_timer_lock);
    return ok && xbox_SaveRuntimeClock(out);
}
int xbox_LoadKernelScheduler(FILE *in)
{
    if(!in || !xbox_CheckpointFrozen()) return 0;
    char magic[7];KernelSchedulerCheckpoint saved;
    if(fread(magic,7,1,in)!=1 || memcmp(magic,"XBSCHED",7) || fread(&saved,sizeof saved,1,in)!=1 ||
        saved.timer_started!=(uint32_t)g_timer_started ||
        (saved.timer_started ? saved.timer_capacity<XBOX_MAX_TIMERS : saved.timer_capacity!=0) ||
        saved.timer_capacity>INT32_MAX || saved.dpc_capacity<XBOX_MAX_PENDING_DPC || saved.dpc_capacity>INT32_MAX ||
        saved.head<0 || (uint32_t)saved.head>=saved.dpc_capacity ||
        saved.tail<0 || (uint32_t)saved.tail>=saved.dpc_capacity) return 0;
    XboxTimer *timers=saved.timer_capacity ? malloc((size_t)saved.timer_capacity*sizeof *timers) : NULL;
    PendingDpc *dpcs=malloc((size_t)saved.dpc_capacity*sizeof *dpcs);
    if((saved.timer_capacity && !timers) || !dpcs) {free(timers);free(dpcs);return 0;}
    int ok=(!saved.timer_capacity || fread(timers,sizeof *timers,saved.timer_capacity,in)==saved.timer_capacity) &&
        fread(dpcs,sizeof *dpcs,saved.dpc_capacity,in)==saved.dpc_capacity;
    void *clock=ok ? xbox_PrepareRuntimeClock(in) : NULL;
    if(!clock) {free(timers);free(dpcs);return 0;}
    if(g_timer_started) EnterCriticalSection(&g_timer_lock);
    dpc_lock();free(g_timers);g_timers=timers;g_timer_capacity=saved.timer_capacity;
    g_timer_scan_end=g_timer_capacity;kernel_trim_timer_scan();
    free(g_dpc_queue);g_dpc_queue=dpcs;g_dpc_capacity=(LONG)saved.dpc_capacity;
    g_dpc_head=saved.head;g_dpc_tail=saved.tail;
    LeaveCriticalSection(&g_dpc_lock);
    if(g_timer_started) LeaveCriticalSection(&g_timer_lock);
    memcpy(g_connected_isr,saved.isr,sizeof g_connected_isr);
    xbox_ApplyRuntimeClock(clock);free(clock);
    return 1;
}
void xbox_CheckpointStartKernelDevices(void) { kernel_start_timer_worker(); }

/* ── ExQueryPoolBlockSize (ordinal 24) ────────────────────
 * ULONG ExQueryPoolBlockSize(PVOID PoolBlock)
 *
 * Returns the size of a pool memory block.
 * Since we use HeapAlloc, we can query the Windows heap.
 */
static void bridge_ExQueryPoolBlockSize(void)
{
    /* Pool blocks come from xbox_HeapAlloc, so the block table has the real
     * answer. It used to return a literal 0 on the theory that this is only
     * ever used for stats -- which is a guess about the caller, and a title
     * that sizes a copy from it copies nothing. */
    g_eax = xbox_HeapBlockSize(STACK_ARG(0));
}

/* ── RtlNtStatusToDosError (ordinal 301) ─────────────────
 * ULONG RtlNtStatusToDosError(NTSTATUS Status)
 *
 * Converts an NTSTATUS to a Win32 error code.
 */
static void bridge_RtlNtStatusToDosError(void)
{
    uint32_t status = STACK_ARG(0);

    /* Simple mapping of common status codes */
    switch (status) {
    case 0x00000000: g_eax = 0; break;          /* STATUS_SUCCESS → ERROR_SUCCESS */
    case 0xC0000034: g_eax = 2; break;          /* STATUS_OBJECT_NAME_NOT_FOUND → ERROR_FILE_NOT_FOUND */
    case 0xC000003A: g_eax = 3; break;          /* STATUS_OBJECT_PATH_NOT_FOUND → ERROR_PATH_NOT_FOUND */
    case 0xC0000022: g_eax = 5; break;          /* STATUS_ACCESS_DENIED → ERROR_ACCESS_DENIED */
    case 0xC0000008: g_eax = 6; break;          /* STATUS_INVALID_HANDLE → ERROR_INVALID_HANDLE */
    case 0xC0000017: g_eax = 8; break;          /* STATUS_NO_MEMORY → ERROR_NOT_ENOUGH_MEMORY */
    case 0xC000000D: g_eax = 87; break;         /* STATUS_INVALID_PARAMETER → ERROR_INVALID_PARAMETER */
    /* The informational and warning codes, which are not failures and must
     * not fall through to the generic answer.
     *
     * 317 is ERROR_MR_MID_NOT_FOUND -- "there is no message text for this
     * number" -- and as a default for a status nobody has mapped yet it is
     * honest. As an answer to "is this request still in flight?" it is not:
     * a caller comparing against ERROR_IO_PENDING gets "no" and takes the
     * branch for a request that never started.
     *
     * Shin Megami Tensei: Nine does exactly that. Its resource loader issues
     * a read, sees the call fail, asks for the error, and marks the object as
     * loading only when the answer is ERROR_IO_PENDING. With 317 the object
     * stayed idle, the poll that finishes the load returned "not started" on
     * every frame, and the title sat in its first boot state forever with
     * everything else working. */
    case 0x00000103: g_eax = 997; break;        /* STATUS_PENDING → ERROR_IO_PENDING */
    case 0x00000102: g_eax = 1460; break;       /* STATUS_TIMEOUT → ERROR_TIMEOUT */
    case 0x00000104: g_eax = 0; break;          /* STATUS_REPARSE → ERROR_SUCCESS */
    case 0x80000005: g_eax = 234; break;        /* STATUS_BUFFER_OVERFLOW → ERROR_MORE_DATA */
    case 0x80000006: g_eax = 18; break;         /* STATUS_NO_MORE_FILES → ERROR_NO_MORE_FILES */
    case 0xC0000011: g_eax = 38; break;         /* STATUS_END_OF_FILE → ERROR_HANDLE_EOF */
    case 0xC0000023: g_eax = 122; break;        /* STATUS_BUFFER_TOO_SMALL → ERROR_INSUFFICIENT_BUFFER */
    case 0xC0000035: g_eax = 183; break;        /* STATUS_OBJECT_NAME_COLLISION → ERROR_ALREADY_EXISTS */
    case 0xC00000BB: g_eax = 50; break;         /* STATUS_NOT_SUPPORTED → ERROR_NOT_SUPPORTED */
    /* The CRT heap-grow path retries at a new address only on
     * ERROR_INVALID_ADDRESS; any other answer makes it give up. */
    case 0xC0000018: g_eax = 487; break;        /* STATUS_CONFLICTING_ADDRESSES → ERROR_INVALID_ADDRESS */

    default:         g_eax = 317; break;         /* ERROR_MR_MID_NOT_FOUND (generic) */
    }
}

/* ── File I/O bridge helpers ─────────────────────────────── */

/*
 * Xbox structures use 32-bit pointers. On Win64, the C structs
 * (XBOX_OBJECT_ATTRIBUTES, etc.) have 64-bit pointers, so we can't
 * cast Xbox memory to them directly. Instead, parse the 32-bit
 * Xbox layout manually:
 *
 * XBOX_OBJECT_ATTRIBUTES (12 bytes):
 *   offset 0: RootDirectory  (uint32_t)
 *   offset 4: ObjectName     (uint32_t, Xbox VA to ANSI_STRING)
 *   offset 8: Attributes     (uint32_t)
 *
 * XBOX_ANSI_STRING (8 bytes):
 *   offset 0: Length          (uint16_t)
 *   offset 2: MaximumLength   (uint16_t)
 *   offset 4: Buffer          (uint32_t, Xbox VA to char[])
 *
 * XBOX_IO_STATUS_BLOCK (8 bytes):
 *   offset 0: Status          (uint32_t)
 *   offset 4: Information     (uint32_t)
 */

/* Extract the ANSI path string from an Xbox OBJECT_ATTRIBUTES */
static const char* bridge_get_xbox_path(uint32_t obj_attrs_va)
{
    uint32_t ansi_str_va, buf_va;
    if (!obj_attrs_va) return NULL;
    ansi_str_va = BRIDGE_MEM32(obj_attrs_va + 4);
    if (!ansi_str_va) return NULL;
    buf_va = BRIDGE_MEM32(ansi_str_va + 4);
    if (!buf_va) return NULL;
    /* XDK directory searches pass a counted prefix of "directory\\*".
     * The byte after Length need not be NUL or part of the object name. */
    static RECOMP_TLS char path[65536];
    uint16_t length=BRIDGE_MEM16(ansi_str_va);
    if(length>BRIDGE_MEM16(ansi_str_va+2)) return NULL;
    /* Length <= MaximumLength says the string is self-consistent; it does not
     * say the buffer it names is inside the mapping. Without this, a string
     * near the top of guest memory reads up to 64 KB off the end. */
    if (length && !bridge_va_mapped(buf_va, length)) return NULL;
    memcpy(path,XBOX_TO_NATIVE(buf_va),length);
    path[length]='\0';
    return path;
}

/* Write NTSTATUS + Information into Xbox IO_STATUS_BLOCK */
static void bridge_write_iostatus(uint32_t ios_va, NTSTATUS status, uint32_t info)
{
    if (ios_va) {
        BRIDGE_MEM32(ios_va + 0) = (uint32_t)status;
        BRIDGE_MEM32(ios_va + 4) = info;
    }
}

/*
 * Handle table.
 *
 * Xbox memory only has 32-bit handle slots, but native HANDLEs are 64-bit
 * pointers (win32_compat objects, or real Win32 handles on Windows). Map
 * 32-bit tokens <-> native HANDLEs so a handle survives a round-trip through
 * Xbox memory. Tokens carry a tag in the high byte so they never collide
 * with the synthetic handles (0xDEAD0001 / 0xBEEF0010) used elsewhere.
 */
#define BRIDGE_HANDLE_TAG  0x48000000u
#define BRIDGE_HANDLE_MASK 0x00FFFFFFu
#define BRIDGE_HANDLE_MAX  16384
static HANDLE s_handle_table[BRIDGE_HANDLE_MAX];
static uint8_t s_handle_private[BRIDGE_HANDLE_MAX];

/* Allocate a token for a native handle not already registered in the table. */
static uint32_t bridge_allocate_handle_token(HANDLE h)
{
    int i;
    if (!h || h == INVALID_HANDLE_VALUE) return 0;
    for (i = 1; i < BRIDGE_HANDLE_MAX; i++)
        if (s_handle_table[i] == NULL) {
            s_handle_table[i] = h;
            s_handle_private[i] = 0;
            return BRIDGE_HANDLE_TAG | (uint32_t)i;
        }
    fprintf(stderr, "  [BRIDGE] handle table full\n");
    return 0;
}

static uint32_t bridge_handle_token(HANDLE h)
{
    if (!h || h == INVALID_HANDLE_VALUE) return 0;
    for (int i = 1; i < BRIDGE_HANDLE_MAX; i++)
        if (s_handle_table[i] == h) return BRIDGE_HANDLE_TAG | (uint32_t)i;
    return bridge_allocate_handle_token(h);
}

/* Store a native HANDLE into a 32-bit Xbox memory slot (as a token). */
static void bridge_write_handle(uint32_t handle_va, HANDLE h)
{
    if (handle_va)
        BRIDGE_MEM32(handle_va) = bridge_handle_token(h);
}

/* Resolve a 32-bit Xbox handle slot back to a native HANDLE. */
static HANDLE bridge_read_handle(uint32_t va)
{
    uint32_t token = BRIDGE_MEM32(va);
    if ((token & 0xFF000000u) == BRIDGE_HANDLE_TAG) {
        uint32_t i = token & BRIDGE_HANDLE_MASK;
        return (i > 0 && i < BRIDGE_HANDLE_MAX) ? s_handle_table[i] : NULL;
    }
    /* Untagged value: synthetic/dummy handle -- pass through unchanged. */
    return (HANDLE)(uintptr_t)token;
}

/* Resolve a token to a HANDLE and release its table slot (for NtClose). */
/* Resolve a handle token passed BY VALUE, without consuming it.
 *
 * Three accessors, easily confused, and confusing two of them broke all file
 * I/O: bridge_read_handle(va) reads a token *from memory* and suits a PHANDLE
 * out-parameter; bridge_take_handle(token) resolves and CLEARS the table slot,
 * which is NtClose semantics; this one resolves and leaves the slot alone,
 * which is what every by-value HANDLE argument needs.
 *
 * NtSetInformationFile and friends take the handle by value, but were calling
 * bridge_read_handle on it -- dereferencing the token as if it were an address.
 * Halo created its save file successfully and then failed the very next call,
 * which surfaced as "couldn't open or create saved game file". */
static HANDLE bridge_resolve_handle(uint32_t token)
{
    if ((token & 0xFF000000u) == BRIDGE_HANDLE_TAG) {
        uint32_t i = token & BRIDGE_HANDLE_MASK;
        return (i > 0 && i < BRIDGE_HANDLE_MAX) ? s_handle_table[i] : NULL;
    }
    /* Pseudo-handles (NtCurrentProcess() = -1, NtCurrentThread() = -2) are
     * negative. Zero-extending them on a 64-bit host yields 0x00000000FFFFFFFE,
     * which Win32 rejects: X-Men Legends' CRT duplicates NtCurrentThread()
     * and spun forever on the STATUS_UNSUCCESSFUL that came back. */
    if (token >= 0xFFFFFFF0u)
        return (HANDLE)(intptr_t)(int32_t)token;
    /* Untagged: synthetic/dummy handle -- pass through unchanged. */
    return (HANDLE)(uintptr_t)token;
}

static HANDLE bridge_take_handle(uint32_t token)
{
    if ((token & 0xFF000000u) == BRIDGE_HANDLE_TAG) {
        uint32_t i = token & BRIDGE_HANDLE_MASK;
        if (i > 0 && i < BRIDGE_HANDLE_MAX) {
            HANDLE h = s_handle_table[i];
            s_handle_table[i] = NULL;
            s_handle_private[i] = 0;
            return h;
        }
    }
    return NULL;   /* untagged -> not a table handle, do not close */
}

/* Retire the exact native identity before a final-release callback can wait.
 * A caller can reuse the token during that callback; returning NtClose must
 * not erase the new identity. Failed native closes never reach this point. */
#ifdef _WIN32
static void bridge_retire_native_handle(HANDLE handle) {
    for(unsigned i=1;i<BRIDGE_HANDLE_MAX;i++)if(s_handle_table[i]==handle) {
        uint32_t token=BRIDGE_HANDLE_TAG|i;
        bridge_take_handle(token);bridge_forget_async_handle(token);
    }
}
#endif

/* Build a native OBJECT_ATTRIBUTES wrapping the translated Xbox path. */
static void bridge_build_oa(uint32_t obj_attrs_va,
                            XBOX_OBJECT_ATTRIBUTES* oa, XBOX_ANSI_STRING* name)
{
    const char* path = bridge_get_xbox_path(obj_attrs_va);
    name->Buffer        = (PCHAR)path;
    name->Length        = path ? (USHORT)strlen(path) : 0;
    name->MaximumLength = (USHORT)(name->Length + 1);
    uint32_t root = obj_attrs_va ? BRIDGE_MEM32(obj_attrs_va) : 0;
    /* -3 is the XDK DOS-device namespace, not a file handle. */
    oa->RootDirectory = root && root != 0xFFFFFFFDu ? bridge_resolve_handle(root) : NULL;
    oa->ObjectName    = name;
    oa->Attributes    = 0;
}

/* Open a file by delegating to the ported xbox_NtCreateFile kernel HLE. */
static NTSTATUS bridge_create_file_impl(
    uint32_t handle_va, ACCESS_MASK access, uint32_t obj_attrs_va,
    uint32_t iostatus_va, ULONG file_attrs, ULONG share,
    ULONG disposition, ULONG options,uint32_t allocation_va)
{
    XBOX_OBJECT_ATTRIBUTES oa={0};XBOX_ANSI_STRING name;
    XBOX_IO_STATUS_BLOCK ios={0};HANDLE h=NULL;NTSTATUS st=0;RecompContFrame frame;
    const RecompContLocal locals[]={{"handle_va",&handle_va,4},{"access",&access,4},{"obj_attrs_va",&obj_attrs_va,4},
        {"iostatus_va",&iostatus_va,4},{"file_attrs",&file_attrs,4},{"share",&share,4},
        {"disposition",&disposition,4},{"options",&options,4},{"allocation_va",&allocation_va,4}};
    int tracked=recomp_cont_active();
    if(tracked)kernel_native_cont_enter(&frame,KERNEL_CONT_CREATE_IMPORT,locals,9);
    /* Callback recovery retains its callback ESP until the existing callback
     * frame consumes it. Restore the original import arguments before parsing. */
    if(!recomp_cont_recovering(KERNEL_CONT_CREATE_FILE)) {
        if(!bridge_va_mapped(handle_va,4) || !bridge_va_mapped(iostatus_va,8) ||
           (allocation_va && !bridge_va_mapped(allocation_va,8))) {st=(NTSTATUS)0xc0000005u;goto done;}
        st=bridge_read_object_attributes(obj_attrs_va,&oa,&name);
        if(st<0)goto done;
        if(!name.Buffer) {st=STATUS_OBJECT_PATH_NOT_FOUND;goto done;}
    }
    st=xbox_NtCreateFile(&h,access,&oa,&ios,allocation_va?XBOX_TO_NATIVE(allocation_va):NULL,
        file_attrs,share,disposition,options);
    if(NT_SUCCESS(st)) {
        bridge_write_handle(handle_va,h);
        if((options&0x30u)==0u)bridge_note_async_handle(BRIDGE_MEM32(handle_va));
        bridge_write_iostatus(iostatus_va,ios.Status,(uint32_t)ios.Information);
    }
done:
    if(st<0 && bridge_va_mapped(iostatus_va,8))bridge_write_iostatus(iostatus_va,st,0);
    if(tracked)recomp_cont_leave(&frame);
    return st;
}

/* ── RtlInitAnsiString (ordinal 289) ──────────────────────
 * VOID RtlInitAnsiString(PANSI_STRING Destination, PCSZ Source)
 *
 * Fills an ANSI_STRING { USHORT Length; USHORT MaximumLength; PCHAR Buffer; }.
 * Unbridged this returned 0 and wrote nothing, so every path a title built
 * this way arrived at NtCreateFile as a null Buffer and failed with
 * STATUS_OBJECT_PATH_NOT_FOUND -- which looks like a missing file rather than
 * a missing bridge. Halo builds its map paths exactly this way.
 */
/* -- RtlEqualString (ordinal 279, 3 args) ----------------
 * BOOLEAN RtlEqualString(PSTRING String1, PSTRING String2, BOOLEAN CaseInSens)
 *
 * The fields are read out by hand rather than casting the guest struct. A
 * guest ANSI_STRING is {USHORT Length, USHORT MaximumLength, 32-bit Buffer},
 * eight bytes; the native one has a 64-bit PCHAR, so a cast would read
 * MaximumLength and Buffer from the wrong offsets and then dereference a guest
 * VA as a host address. RtlInitAnsiString stores a guest VA in that field --
 * see the bridge below -- so it has to be translated, not passed through.
 *
 * Stubbed, this returned 0: "never equal". Wreckless initialises a string and
 * compares it in a critical-section-protected lookup, so every comparison
 * missing turned that lookup into unbounded recursion and the process died of
 * a host stack overflow 200 kernel calls in.
 */
static void bridge_RtlEqualString(void)
{
    uint32_t s1_va  = STACK_ARG(0);
    uint32_t s2_va  = STACK_ARG(1);
    uint32_t nocase = STACK_ARG(2);
    XBOX_ANSI_STRING a, b;

    if (!s1_va || !s2_va) {
        g_eax = 0;
        return;
    }
    a.Length        = BRIDGE_MEM16(s1_va + 0);
    a.MaximumLength = BRIDGE_MEM16(s1_va + 2);
    a.Buffer        = (PCHAR)XBOX_TO_NATIVE(BRIDGE_MEM32(s1_va + 4));
    b.Length        = BRIDGE_MEM16(s2_va + 0);
    b.MaximumLength = BRIDGE_MEM16(s2_va + 2);
    b.Buffer        = (PCHAR)XBOX_TO_NATIVE(BRIDGE_MEM32(s2_va + 4));

    if (!a.Buffer || !b.Buffer) {
        g_eax = 0;
        return;
    }
    g_eax = xbox_RtlEqualString(&a, &b, (BOOLEAN)nocase) ? 1 : 0;
}

static void bridge_RtlInitAnsiString(void)
{
    uint32_t dest_va = STACK_ARG(0);
    uint32_t src_va  = STACK_ARG(1);

    if (!dest_va) {
        g_eax = 0;
        return;
    }
    if (src_va) {
        const char *src = (const char *)XBOX_TO_NATIVE(src_va);
        size_t len = strlen(src);
        if (len > 0xFFFE) {
            len = 0xFFFE;
        }
        BRIDGE_MEM16(dest_va + 0) = (uint16_t)len;
        BRIDGE_MEM16(dest_va + 2) = (uint16_t)(len + 1);
        BRIDGE_MEM32(dest_va + 4) = src_va;
    } else {
        BRIDGE_MEM16(dest_va + 0) = 0;
        BRIDGE_MEM16(dest_va + 2) = 0;
        BRIDGE_MEM32(dest_va + 4) = 0;
    }
    g_eax = 0;
}

/* ── NtCreateFile (ordinal 190, 9 args = 36 bytes) ─────── */
static void bridge_NtCreateFile(void)
{
    ULONGLONG load_started = GetTickCount64();
    uint32_t handle_va   = STACK_ARG(0);  /* PHANDLE */
    uint32_t access      = STACK_ARG(1);  /* ACCESS_MASK */
    uint32_t obj_attrs   = STACK_ARG(2);  /* POBJECT_ATTRIBUTES */
    uint32_t iostatus    = STACK_ARG(3);  /* PIO_STATUS_BLOCK */
    /* arg4: AllocationSize - ignored */
    uint32_t file_attrs  = STACK_ARG(5);  /* FileAttributes */
    uint32_t share       = STACK_ARG(6);  /* ShareAccess */
    uint32_t disposition = STACK_ARG(7);  /* CreateDisposition */
    uint32_t options     = STACK_ARG(8);  /* CreateOptions */

    /* The out-parameter addresses matter as much as the result: this bridge
     * hands them to a real Win32 call, so a bogus one has Windows itself write
     * into Xbox memory. That is how a wild write ends up with a stack inside
     * ntdll and no recompiled frame to blame. */
    g_eax = (uint32_t)bridge_create_file_impl(
        handle_va, access, obj_attrs, iostatus,
        file_attrs, share, disposition, options,STACK_ARG(4));

    /* FILE_SYNCHRONOUS_IO_ALERT | FILE_SYNCHRONOUS_IO_NONALERT. Neither set
     * means the caller wants asynchronous completion on this handle. */

    /* An FMV the host can decode itself.
     *
     * The title's own decoder is emulated like everything else, but it only
     * produces pixels once there is something to execute its GPU work -- so on
     * a bring-up where that does not exist yet, the video the game just asked
     * for can still be shown. The trigger is the title opening the file, so
     * this plays when the game decides to play it, not on a timer, and it
     * plays the file the game chose.
     *
     * Off unless RECOMP_FMV_HOST is set: it is a substitute for the title's
     * own output, and that should be a decision rather than a default. */
    if (g_eax == 0 && getenv("RECOMP_FMV_HOST")) {
        /* Declared here rather than included: the player lives in xbox_video,
         * which links xbox_d3d8, and having the kernel include its header
         * would make the dependency circular for no gain. Both land in the
         * same executable. */
        extern int xbox_VideoPlayFile(const char *host_path);
        extern int xbox_VideoIsPlaying(void);

        char host[MAX_PATH * 2];
        size_t n = 0;

        const wchar_t *w = xbox_LastHostPath();

        while (n < sizeof(host) - 1 && w[n]) {
            host[n] = (char)w[n];
            n++;
        }
        host[n] = 0;
        if (n > 4 && _stricmp(host + n - 4, ".wmv") == 0
                && !xbox_VideoIsPlaying())
            xbox_VideoPlayFile(host);
    }

    if (getenv("RECOMP_LOAD_TIMING"))
        fprintf(stderr, "  [OPEN TIME] t_ms=%llu tid=%lu from=%08X elapsed_ms=%llu\n",
                (unsigned long long)load_started, (unsigned long)GetCurrentThreadId(),
                g_xbox_kernel_caller, (unsigned long long)(GetTickCount64()-load_started));

    /* Paired with the [PATH] line the translation just printed: that says what
     * was asked for, this says whether it opened. A failed open is not itself
     * a bug -- a title probing the cache partition before the disc expects one
     * -- so the status is what separates a probe from a real miss. */
    {
        extern uint32_t xbox_LastFileError(void);
        uint32_t _e = g_eax ? xbox_LastFileError() : 0u;
        if (g_eax)
            fprintf(stderr, "  [FILE] -> 0x%08X FAILED (win32 err=%u%s)\n",
                    g_eax, _e,
                    _e == 32u ? " ERROR_SHARING_VIOLATION"
                  : _e ==  2u ? " ERROR_FILE_NOT_FOUND"
                  : _e ==  3u ? " ERROR_PATH_NOT_FOUND" : "");
        else
            fprintf(stderr, "  [FILE] -> 0x%08X\n", g_eax);
    }
    fflush(stderr);
}

/* ── NtOpenFile (ordinal 202, 6 args = 24 bytes) ──────── */
static void bridge_NtOpenFile(void)
{
    uint32_t handle_va = STACK_ARG(0);  /* PHANDLE */
    uint32_t access    = STACK_ARG(1);  /* ACCESS_MASK */
    uint32_t obj_attrs = STACK_ARG(2);  /* POBJECT_ATTRIBUTES */
    uint32_t iostatus  = STACK_ARG(3);  /* PIO_STATUS_BLOCK */
    uint32_t share     = STACK_ARG(4);  /* ShareAccess */
    uint32_t options   = STACK_ARG(5);  /* OpenOptions */

    /* NtOpenFile = NtCreateFile with FILE_OPEN disposition */
    g_eax = (uint32_t)bridge_create_file_impl(
        handle_va, access, obj_attrs, iostatus,
        0, share, 1 /* FILE_OPEN */, options,0);
}

/* File requests use the issuing thread's user APC list. Signalling an
 * event does not authorize executing a user APC before an alertable wait. */
static void bridge_complete_file_io(uint32_t event_token, uint32_t apc_routine,
                                    uint32_t apc_context, uint32_t iostatus)
{
    if(event_token) {HANDLE event=bridge_resolve_handle(event_token);if(event)SetEvent(event);}
    if(apc_routine) {
        uint32_t apc=xbox_HeapAlloc(0x28,16);
        if(!apc) {fprintf(stderr,"[STOP] completed file request cannot allocate APC\n");exit(5);}
        int held=guest_cpu_is_held(),was_thread=t_guest_thread;if(!held)guest_cpu_join();
        ke_initialize_apc(apc,ke_current_thread(),KERNEL_POOL_FREE_APC,KERNEL_POOL_FREE_APC,
                          apc_routine,1,apc_context);
        if(!ke_insert_apc(apc,iostatus,0)) xbox_HeapFree(apc);
        if(!held) {guest_cpu_part();t_guest_thread=was_thread;}
    }
}

/* -- XeLoadSection / XeUnloadSection (ordinals 327/328, 1 arg = 4 bytes) --
 *
 * NTSTATUS XeLoadSection(PXBE_SECTION_HEADER Section);
 *
 * On hardware a section marked non-preload is paged in from disc on demand,
 * and a title that keeps its video decoder in one -- Wreckless keeps WMVDEC
 * there -- calls this before touching it. Every section is already resident
 * here, so the work is the bookkeeping: hand back success and keep the
 * reference count the title can read.
 *
 * Done against guest memory rather than the PXBE_SECTION_HEADER struct: the
 * on-disc header is nine 32-bit fields and a digest, and the native struct
 * declares some of them as pointers, so on x64 its layout is not the 56 bytes
 * actually there.
 *
 *   +0x14  section name address      +0x18  section reference count
 */
#define XBE_SECTION_REFCOUNT_OFFSET 0x18

static void bridge_XeSection(int load)
{
    uint32_t section = STACK_ARG(0);
    uint32_t count;

    if (!section) {
        g_eax = 0xC000000Du;              /* STATUS_INVALID_PARAMETER */
        return;
    }
    count = BRIDGE_MEM32(section + XBE_SECTION_REFCOUNT_OFFSET);
    if (load)
        count++;
    else if (count)
        count--;
    BRIDGE_MEM32(section + XBE_SECTION_REFCOUNT_OFFSET) = count;

    if (KERNEL_LOG_ON())
        fprintf(stderr, "  [XBE] Xe%sSection(0x%08X) refcount=%u\n",
                load ? "Load" : "Unload", section, count);
    g_eax = 0;
}

static void bridge_XeLoadSection(void)   { bridge_XeSection(1); }
static void bridge_XeUnloadSection(void) { bridge_XeSection(0); }

/* RtlUnwind retains the original registration while its handler runs.
 * The original 4627 execute-handler trampoline installs a collision guard;
 * only ContinueSearch and CollidedUnwind dispositions unlink a registration. */
#define XBOX_SEH_END_OF_CHAIN 0xFFFFFFFFu
#define XBOX_EXCEPTION_UNWINDING  0x02u
#define XBOX_EXCEPTION_EXIT_UNWIND 0x04u
static void bridge_capture_context(uint32_t,const XboxGuestRegisters *,uint32_t,uint32_t);
static void bridge_restore_context(uint32_t,XboxGuestRegisters *);
static uint32_t bridge_raise_record(uint32_t,uint32_t,uint32_t,uint32_t);
static void bridge_unhandled_exception(uint32_t record,uint32_t address) {
    fprintf(stderr,"[STOP] unhandled guest exception %08X at %08X\n",BRIDGE_MEM32(record),address);
    fflush(stderr);ExitProcess(5);
}

typedef struct GuestUnwind {
    RecompContFrame frame;XboxGuestRegisters saved;
    uint32_t target_frame,record,return_value,context,registration,handler,dispatcher,guard;
    uint32_t address,phase,error_record,error_status,resume_target,resume_ebp;
} GuestUnwind;
static int bridge_unwind_registration_valid(uint32_t registration) {
    uint32_t bottom=BRIDGE_MEM32(XBOX_FS_BASE+8),top=BRIDGE_MEM32(XBOX_FS_BASE+4);
    if((registration&3u) || !bridge_va_mapped(registration,8))return 0;
    if((!bottom || registration>=bottom) && (!top || (uint64_t)registration+8<=top))return 1;
    if(xbox_CurrentIrql()<DISPATCH_LEVEL || !BRIDGE_MEM32(XBOX_FS_BASE+0x58))return 0;
    top=BRIDGE_MEM32(XBOX_FS_BASE+0x5c);
    return top>=0x3000 && registration>=top-0x3000 && (uint64_t)registration+8<=top;
}
static void bridge_unwind_remove_guard(GuestUnwind *state) {
    if(state->guard && BRIDGE_MEM32(XBOX_FS_BASE)==state->guard)
        BRIDGE_MEM32(XBOX_FS_BASE)=BRIDGE_MEM32(state->guard);
    state->guard=0;
}
static void bridge_unwind_leave(RecompContFrame *frame) {
    if(!frame->retained)bridge_unwind_remove_guard((GuestUnwind*)frame);
}
static void bridge_unwind_collision_guard(void) {
    uint32_t record=BRIDGE_MEM32(g_esp+4),registration=BRIDGE_MEM32(g_esp+8);
    uint32_t dispatcher=BRIDGE_MEM32(g_esp+16);g_eax=1;
    if(BRIDGE_MEM32(record+4)&6u) {
        BRIDGE_MEM32(dispatcher)=BRIDGE_MEM32(registration+8);g_eax=3;
    }
    g_esp+=20;
}
static void bridge_RtlUnwind(void) {
    GuestUnwind state={0};
    const RecompContLocal locals[]={
        GUEST_REGISTER_CONT_LOCALS(state.saved,"saved")
        {"target_frame",&state.target_frame,4},{"record",&state.record,4},
        {"return_value",&state.return_value,4},{"context",&state.context,4},
        {"registration",&state.registration,4},{"handler",&state.handler,4},
        {"dispatcher",&state.dispatcher,4},{"guard",&state.guard,4},
        {"address",&state.address,4},{"phase",&state.phase,4},
        {"error_record",&state.error_record,4},{"error_status",&state.error_status,4},
        {"resume_target",&state.resume_target,4},{"resume_ebp",&state.resume_ebp,4}
    };
    int tracked=recomp_cont_active();
    uint64_t resume=tracked?kernel_native_cont_enter(&state.frame,KERNEL_CONT_UNWIND,locals,sizeof locals/sizeof locals[0]):0;
    if(tracked)state.frame.on_leave=bridge_unwind_leave;
    if(!guest_cpu_is_held())guest_cpu_join();
    if(resume) {
        if(state.phase==1)goto handler;
        if(state.phase==2)goto raising;
        if(state.phase==3)goto transfer;
        recomp_cont_invalid();
    }
    xbox_CaptureGuestRegisters(&state.saved);
    state.target_frame=STACK_ARG(0);state.record=STACK_ARG(2);state.return_value=STACK_ARG(3);
    state.address=BRIDGE_MEM32(state.saved.esp-4);
    state.context=(state.saved.esp-0x240u)&~15u;state.dispatcher=state.context-0x54u;
    if(!bridge_va_mapped(state.context-0xb0u,0x2f0u))recomp_cont_invalid();
    bridge_capture_context(state.context,&state.saved,state.address,state.saved.esp+16);
    BRIDGE_MEM32(state.context+0x21c)=state.return_value;
    if(!state.record) {
        state.record=state.context-0x50u;memset(XBOX_TO_NATIVE(state.record),0,0x50);
        BRIDGE_MEM32(state.record)=0xc0000027u;BRIDGE_MEM32(state.record+12)=state.address;
    } else if(!bridge_va_mapped(state.record,0x50))recomp_cont_invalid();
    BRIDGE_MEM32(state.record+4)|=XBOX_EXCEPTION_UNWINDING|
        (state.target_frame?0:XBOX_EXCEPTION_EXIT_UNWIND);
    state.registration=BRIDGE_MEM32(XBOX_FS_BASE);
    for(;;) {
        if(state.registration==state.target_frame)break;
        if(state.registration==XBOX_SEH_END_OF_CHAIN) {
            if(state.target_frame==XBOX_SEH_END_OF_CHAIN)break;
            /* The original second-chance raise cannot return to a discarded
             * frame when the requested target is absent or this is exit unwind. */
            bridge_unhandled_exception(state.record,BRIDGE_MEM32(state.context+0x224));
            recomp_cont_invalid();
        }
        if(state.target_frame && state.target_frame<state.registration) {
            state.error_status=0xc0000029u;goto error;
        }
        if(!bridge_unwind_registration_valid(state.registration)) {
            state.error_status=0xc0000028u;goto error;
        }
        state.handler=BRIDGE_MEM32(state.registration+4);
        BRIDGE_MEM32(state.dispatcher)=0;state.guard=state.dispatcher-12;
        BRIDGE_MEM32(state.guard)=BRIDGE_MEM32(XBOX_FS_BASE);
        BRIDGE_MEM32(state.guard+4)=KERNEL_UNWIND_COLLISION_GUARD;
        BRIDGE_MEM32(state.guard+8)=state.registration;BRIDGE_MEM32(XBOX_FS_BASE)=state.guard;
        g_esp=state.guard;state.phase=1;
handler: ;
        uint32_t args[]={state.record,state.registration,state.context,state.dispatcher};
        uint32_t disposition=ke_call_guest(state.handler,args,4,1);
        bridge_unwind_remove_guard(&state);
        if(!xbox_RestoreGuestRegisters(&state.saved))recomp_cont_invalid();
        if(disposition==3)state.registration=BRIDGE_MEM32(state.dispatcher);
        else if(disposition!=1) {state.error_status=0xc0000026u;goto error;}
        if(!bridge_unwind_registration_valid(state.registration)) {
            state.error_status=0xc0000028u;goto error;
        }
        uint32_t previous=state.registration;state.registration=BRIDGE_MEM32(previous);
        BRIDGE_MEM32(XBOX_FS_BASE)=state.registration;state.phase=0;
        if(state.registration!=XBOX_SEH_END_OF_CHAIN && state.registration<=previous) {
            state.error_status=0xc0000028u;goto error;
        }
    }
    bridge_restore_context(state.context,&state.saved);
    state.resume_target=BRIDGE_MEM32(state.context+0x224);state.resume_ebp=g_ebp;
    if(state.resume_target!=state.address) {state.phase=3;goto transfer;}
    /* The import thunk still owns its four-argument stdcall cleanup. */
    g_esp-=16;
    if(tracked)recomp_cont_leave(&state.frame);return;
error:
    state.error_record=state.context-0xb0u;
    memset(XBOX_TO_NATIVE(state.error_record),0,0x50);
    BRIDGE_MEM32(state.error_record)=state.error_status;BRIDGE_MEM32(state.error_record+4)=1;
    BRIDGE_MEM32(state.error_record+8)=state.record;BRIDGE_MEM32(state.error_record+12)=state.address;
    state.phase=2;
raising:
    g_esp=state.error_record;
    bridge_raise_record(state.error_record,state.address,state.saved.ebp,state.saved.esp+16);
    recomp_cont_invalid();return;
transfer:
    xbox_ExceptionContinue(state.resume_target,state.resume_ebp);
    recomp_cont_invalid();
}


/* Which open file handles were asked for asynchronously.
 *
 * NtCreateFile takes FILE_SYNCHRONOUS_IO_ALERT (0x10) and
 * FILE_SYNCHRONOUS_IO_NONALERT (0x20) in CreateOptions. With neither, the
 * handle is asynchronous: NtReadFile on it returns STATUS_PENDING even with
 * no event, and the caller waits on the handle. Completing every read
 * synchronously answers a different question than the one that was asked.
 *
 * A flat array because a title has a handful of files open at once and a
 * linear scan of sixty-four entries costs less than a hash would.
 */
#define BRIDGE_ASYNC_MAX 64
static uint32_t g_async_handles[BRIDGE_ASYNC_MAX];

static void bridge_note_async_handle(uint32_t token)
{
    int i;
    if (!token)
        return;
    for (i = 0; i < BRIDGE_ASYNC_MAX; i++)
        if (g_async_handles[i] == token)
            return;
    for (i = 0; i < BRIDGE_ASYNC_MAX; i++)
        if (!g_async_handles[i]) { g_async_handles[i] = token; return; }
}

static void bridge_forget_async_handle(uint32_t token)
{
    int i;
    for (i = 0; i < BRIDGE_ASYNC_MAX; i++)
        if (g_async_handles[i] == token) { g_async_handles[i] = 0; return; }
}

static int bridge_handle_is_async(uint32_t token)
{
    int i;
    if (!token)
        return 0;
    for (i = 0; i < BRIDGE_ASYNC_MAX; i++)
        if (g_async_handles[i] == token)
            return 1;
    return 0;
}

/* Off unless RECOMP_ASYNC_IO is set, so the two behaviours stay comparable
 * rather than one being swapped in blind. */
static int bridge_async_io_enabled(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("RECOMP_ASYNC_IO") ? 1 : 0;
    return on;
}

/* ── NtReadFile (ordinal 219, 8 args = 32 bytes) ──────── */
static void bridge_NtReadFile(void)
{
    ULONGLONG load_started = GetTickCount64();
    HANDLE   handle    = bridge_resolve_handle(STACK_ARG(0));
    uint32_t iostatus  = STACK_ARG(4);
    uint32_t buffer_va = STACK_ARG(5);
    uint32_t length    = STACK_ARG(6);
    uint32_t offset_va = STACK_ARG(7);
    XBOX_IO_STATUS_BLOCK ios;
    LARGE_INTEGER  off = {0};
    PLARGE_INTEGER poff = NULL;

    memset(&ios, 0, sizeof(ios));
    if (!bridge_buf_ok(buffer_va, length, "NtReadFile")) {
        bridge_write_iostatus(iostatus, (NTSTATUS)0xC0000005, 0);
        g_eax = 0xC0000005u;                 /* STATUS_ACCESS_VIOLATION */
        return;
    }
    if (offset_va) {
        off.LowPart  = BRIDGE_MEM32(offset_va);
        off.HighPart = (LONG)BRIDGE_MEM32(offset_va + 4);
        poff = &off;
    }
    g_eax = (uint32_t)xbox_NtReadFile(handle, NULL, NULL, NULL, &ios,
                XBOX_TO_NATIVE(buffer_va), length, poff);

    if (getenv("RECOMP_LOAD_TIMING"))
        fprintf(stderr, "  [READ TIME] t_ms=%llu tid=%lu from=%08X handle=%08X buffer=%08X elapsed_ms=%llu\n",
                (unsigned long long)load_started, (unsigned long)GetCurrentThreadId(),
                g_xbox_kernel_caller, STACK_ARG(0), buffer_va,
                (unsigned long long)(GetTickCount64()-load_started));

    /* What a read actually delivered. A decoder that rejects its input cannot
     * say whether the bytes were wrong or the read was, and the two look
     * identical from inside the title -- the first bytes settle it. */
    if (KERNEL_LOG_ON()) {
        const uint8_t *p = (const uint8_t *)XBOX_TO_NATIVE(buffer_va);
        uint32_t got = (uint32_t)ios.Information;
        /* The offset matters as much as the length. A title streaming a pack
         * file reads sector-aligned chunks, so the first bytes belong to
         * whatever precedes the file it actually wants, and a read that stops
         * early looks identical to one that never started -- until you can
         * see where each one landed. */
        if (poff)
            fprintf(stderr, "  [READ] from=0x%08X ev=%08X apc=%08X @%lld want=%u got=%u st=0x%08X  %02X %02X %02X %02X\n",
                    g_xbox_kernel_caller, STACK_ARG(1), STACK_ARG(2),
                    (long long)off.QuadPart, length, got,
                    (uint32_t)ios.Status,
                    got > 0 ? p[0] : 0, got > 1 ? p[1] : 0,
                    got > 2 ? p[2] : 0, got > 3 ? p[3] : 0);
        else
            fprintf(stderr, "  [READ] from=0x%08X @seq want=%u got=%u st=0x%08X  %02X %02X %02X %02X\n",
                    g_xbox_kernel_caller,
                    length, got, (uint32_t)ios.Status,
                    got > 0 ? p[0] : 0, got > 1 ? p[1] : 0,
                    got > 2 ? p[2] : 0, got > 3 ? p[3] : 0);
        fflush(stderr);
    }
    bridge_write_iostatus(iostatus, ios.Status, (uint32_t)ios.Information);
    bridge_complete_file_io(STACK_ARG(1), STACK_ARG(2), STACK_ARG(3),
                            iostatus);

    /* An asynchronous request returns STATUS_PENDING, even when the data was
     * already there.
     *
     * A caller that asked to be told later is told later on Windows: the read
     * returns 0x00000103 and the status block carries the result once the
     * handle signals. Returning STATUS_SUCCESS instead is not a harmless
     * shortcut, it is a different contract, and code written against the real
     * one takes the branch that says nothing is in flight.
     *
     * The read itself stays synchronous here: the status block is already
     * written and the event already signalled, so a caller that waits is
     * satisfied immediately. Only the answer changes.
     *
     * An event or APC alone does not make a read asynchronous: on a
     * synchronous handle the kernel waits and returns the final status. */
    if (bridge_async_io_enabled() && bridge_handle_is_async(STACK_ARG(0)))
        g_eax = STATUS_PENDING;
}

/* ── NtWriteFile (ordinal 236, 8 args = 32 bytes) ─────── */
static void bridge_NtWriteFile(void)
{
    HANDLE   handle    = bridge_resolve_handle(STACK_ARG(0));
    uint32_t iostatus  = STACK_ARG(4);
    uint32_t buffer_va = STACK_ARG(5);
    uint32_t length    = STACK_ARG(6);
    uint32_t offset_va = STACK_ARG(7);
    XBOX_IO_STATUS_BLOCK ios;
    LARGE_INTEGER  off = {0};
    PLARGE_INTEGER poff = NULL;

    memset(&ios, 0, sizeof(ios));
    if (!bridge_buf_ok(buffer_va, length, "NtWriteFile")) {
        bridge_write_iostatus(iostatus, (NTSTATUS)0xC0000005, 0);
        g_eax = 0xC0000005u;                 /* STATUS_ACCESS_VIOLATION */
        return;
    }
    if (offset_va) {
        off.LowPart  = BRIDGE_MEM32(offset_va);
        off.HighPart = (LONG)BRIDGE_MEM32(offset_va + 4);
        poff = &off;
    }
    g_eax = (uint32_t)xbox_NtWriteFile(handle, NULL, NULL, NULL, &ios,
                XBOX_TO_NATIVE(buffer_va), length, poff);
    bridge_write_iostatus(iostatus, ios.Status, (uint32_t)ios.Information);
    bridge_complete_file_io(STACK_ARG(1), STACK_ARG(2), STACK_ARG(3),
                            iostatus);
}

/* ── NtQueryInformationFile (ordinal 211, 5 args = 20 bytes) */
static void bridge_NtQueryInformationFile(void)
{
    HANDLE   handle    = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va    = STACK_ARG(1);
    uint32_t info_va   = STACK_ARG(2);
    uint32_t length    = STACK_ARG(3);
    uint32_t infoclass = STACK_ARG(4);
    XBOX_IO_STATUS_BLOCK ios;

    memset(&ios, 0, sizeof(ios));
    if (!bridge_buf_ok(info_va, length, "NtQueryInformationFile")) {
        bridge_write_iostatus(ios_va, (NTSTATUS)0xC0000005, 0);
        g_eax = 0xC0000005u;                 /* STATUS_ACCESS_VIOLATION */
        return;
    }
    g_eax = (uint32_t)xbox_NtQueryInformationFile(handle, &ios,
                XBOX_TO_NATIVE(info_va), length,
                (XBOX_FILE_INFORMATION_CLASS)infoclass);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtSetInformationFile (ordinal 226, 5 args = 20 bytes) ─ */
static void bridge_NtSetInformationFile(void)
{
    HANDLE   handle    = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va    = STACK_ARG(1);
    uint32_t info_va   = STACK_ARG(2);
    uint32_t length    = STACK_ARG(3);
    uint32_t infoclass = STACK_ARG(4);
    XBOX_IO_STATUS_BLOCK ios;

    memset(&ios, 0, sizeof(ios));
    if (!bridge_buf_ok(info_va, length, "NtSetInformationFile")) {
        bridge_write_iostatus(ios_va, (NTSTATUS)0xC0000005, 0);
        g_eax = 0xC0000005u;                 /* STATUS_ACCESS_VIOLATION */
        return;
    }
    g_eax = (uint32_t)xbox_NtSetInformationFile(handle, &ios,
                XBOX_TO_NATIVE(info_va), length,
                (XBOX_FILE_INFORMATION_CLASS)infoclass);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtQueryVolumeInformationFile (ordinal 218, 5 args = 20 bytes) */
static void bridge_NtQueryVolumeInformationFile(void)
{
    HANDLE   handle    = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va    = STACK_ARG(1);
    uint32_t info_va   = STACK_ARG(2);
    uint32_t length    = STACK_ARG(3);
    uint32_t infoclass = STACK_ARG(4);
    XBOX_IO_STATUS_BLOCK ios;

    memset(&ios, 0, sizeof(ios));
    if (!bridge_buf_ok(info_va, length, "NtQueryVolumeInformationFile")) {
        bridge_write_iostatus(ios_va, (NTSTATUS)0xC0000005, 0);
        g_eax = 0xC0000005u;                 /* STATUS_ACCESS_VIOLATION */
        return;
    }
    g_eax = (uint32_t)xbox_NtQueryVolumeInformationFile(handle, &ios,
                XBOX_TO_NATIVE(info_va), length,
                (XBOX_FS_INFORMATION_CLASS)infoclass);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtQueryFullAttributesFile (ordinal 210, 2 args = 8 bytes) */
static void bridge_NtQueryFullAttributesFile(void)
{
    uint32_t obj_attrs = STACK_ARG(0);
    uint32_t info_va   = STACK_ARG(1);
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING       name;

    bridge_build_oa(obj_attrs, &oa, &name);
    if (!name.Buffer) { g_eax = STATUS_OBJECT_PATH_NOT_FOUND; return; }
    g_eax = (uint32_t)xbox_NtQueryFullAttributesFile(&oa,
                (PXBOX_FILE_NETWORK_OPEN_INFORMATION)XBOX_TO_NATIVE(info_va));
}

/* ── NtFlushBuffersFile (ordinal 198, 2 args = 8 bytes) ─── */
static void bridge_NtFlushBuffersFile(void)
{
    HANDLE   handle = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va = STACK_ARG(1);
    XBOX_IO_STATUS_BLOCK ios;

    memset(&ios, 0, sizeof(ios));
    g_eax = (uint32_t)xbox_NtFlushBuffersFile(handle, &ios);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtDeleteFile (ordinal 195, 1 arg = 4 bytes) ─────── */
static void bridge_NtDeleteFile(void)
{
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING       name;

    bridge_build_oa(STACK_ARG(0), &oa, &name);
    if (!name.Buffer) { g_eax = STATUS_OBJECT_PATH_NOT_FOUND; return; }
    g_eax = (uint32_t)xbox_NtDeleteFile(&oa);
}

/* ── NtQueryDirectoryFile (ordinal 207, 10 args = 40 bytes) ─ */
static void bridge_NtQueryDirectoryFile(void)
{
    HANDLE   handle      = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va      = STACK_ARG(4);
    uint32_t info_va     = STACK_ARG(5);
    uint32_t length      = STACK_ARG(6);
    uint32_t info_class  = STACK_ARG(7);
    uint32_t filename_va = STACK_ARG(8);  /* PXBOX_ANSI_STRING */
    uint32_t restart     = STACK_ARG(9);  /* BOOLEAN */
    XBOX_IO_STATUS_BLOCK ios;
    XBOX_ANSI_STRING     fn;
    PXBOX_ANSI_STRING    pfn = NULL;

    memset(&ios, 0, sizeof(ios));
    if (filename_va) {
        /* Xbox ANSI_STRING: 0=Length(u16), 2=MaximumLength(u16), 4=Buffer(u32) */
        uint32_t fn_buf  = BRIDGE_MEM32(filename_va + 4);
        fn.Length        = BRIDGE_MEM16(filename_va);
        fn.MaximumLength = BRIDGE_MEM16(filename_va + 2);
        fn.Buffer        = fn_buf ? (PCHAR)XBOX_TO_NATIVE(fn_buf) : NULL;
        if (fn.Buffer) pfn = &fn;
    }
    g_eax = (uint32_t)xbox_NtQueryDirectoryFile(handle, NULL, NULL, NULL, &ios,
                XBOX_TO_NATIVE(info_va), length, (XBOX_FILE_INFORMATION_CLASS)info_class,
                pfn, (BOOLEAN)restart);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtOpenSymbolicLinkObject (ordinal 203, 2 args = 8 bytes) */
static void bridge_NtOpenSymbolicLinkObject(void)
{
    XBOX_OBJECT_ATTRIBUTES oa;XBOX_ANSI_STRING name;HANDLE handle=NULL;
    NTSTATUS status=bridge_read_object_attributes(STACK_ARG(1),&oa,&name);
    if(!bridge_va_mapped(STACK_ARG(0),4))status=(NTSTATUS)0xc0000005u;
    if(status>=0)status=xbox_NtOpenSymbolicLinkObject(&handle,&oa);
    if(bridge_va_mapped(STACK_ARG(0),4))bridge_write_handle(STACK_ARG(0),handle);
    g_eax=(uint32_t)status;
}

/* ── NtQuerySymbolicLinkObject (ordinal 215, 3 args = 12 bytes) */
static void bridge_NtQuerySymbolicLinkObject(void)
{
    uint32_t output=STACK_ARG(1),returned=STACK_ARG(2);XBOX_ANSI_STRING target={0};ULONG length=0;
    if(!bridge_va_mapped(output,8) || (returned && !bridge_va_mapped(returned,4))) {g_eax=0xc0000005u;return;}
    target.MaximumLength=BRIDGE_MEM16(output+2);uint32_t buffer=BRIDGE_MEM32(output+4);
    if(target.MaximumLength && !bridge_va_mapped(buffer,target.MaximumLength)) {g_eax=0xc0000005u;return;}
    target.Buffer=XBOX_TO_NATIVE(buffer);target.Length=BRIDGE_MEM16(output);
    NTSTATUS status=xbox_NtQuerySymbolicLinkObject(bridge_resolve_handle(STACK_ARG(0)),&target,&length);
    if(status>=0 || status==(NTSTATUS)0xc0000023u) {
        BRIDGE_MEM16(output)=target.Length;if(returned)BRIDGE_MEM32(returned)=length;
    }
    g_eax=(uint32_t)status;
}

/* ── IoCreateFile (ordinal 67, 10 args = 40 bytes) ────── */
static void bridge_IoCreateFile(void)
{
    /* Same as NtCreateFile with an extra Options arg at the end */
    uint32_t handle_va   = STACK_ARG(0);
    uint32_t access      = STACK_ARG(1);
    uint32_t obj_attrs   = STACK_ARG(2);
    uint32_t iostatus    = STACK_ARG(3);
    uint32_t file_attrs  = STACK_ARG(5);
    uint32_t share       = STACK_ARG(6);
    uint32_t disposition = STACK_ARG(7);
    uint32_t options     = STACK_ARG(8);

    g_eax = (uint32_t)bridge_create_file_impl(
        handle_va, access, obj_attrs, iostatus,
        file_attrs, share, disposition, options,STACK_ARG(4));
}

/* -- NtDeviceIoControlFile (ordinal 196, 10 args = 40 bytes) ----
 *
 * NTSTATUS NtDeviceIoControlFile(HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID,
 *                                PIO_STATUS_BLOCK, ULONG IoControlCode,
 *                                PVOID In, ULONG InLen,
 *                                PVOID Out, ULONG OutLen);
 */
#define IOCTL_DISK_GET_DRIVE_GEOMETRY 0x00070000u
#define IOCTL_DISK_GET_PARTITION_INFO 0x00074004u

static NTSTATUS io_native_control(uint32_t device,uint32_t file,uint32_t ioctl,uint32_t input,
    uint32_t input_length,uint32_t out_va,uint32_t out_len,uint32_t *information,int filesystem)
{
    *information=0;HANDLE native=file?ke_shadow_lookup(file):NULL;
    if(!bridge_buf_ok(input,input_length,"native control input") ||
       !bridge_buf_ok(out_va,out_len,"native control output"))return (NTSTATUS)0xc0000005u;
    NTSTATUS status=native?xbox_kernel_file_volume_status(native):0;if(status<0)return status;
    if(filesystem) {
        if(ioctl==0x90020u)return xbox_kernel_dismount_file_volume(native);
        if(ioctl==0x9411cu || ioctl==0x98120u) {
            if(input_length<12)return STATUS_INVALID_PARAMETER;
            uint32_t offset=BRIDGE_MEM32(input),length=BRIDGE_MEM32(input+4);
            if(offset>=0x1000 || length>0x1000 || length>0x1000-offset)return STATUS_INVALID_PARAMETER;
        }
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    if (xbox_kernel_file_device_type(native) == 2) {
        uint32_t in_va=input,in_len=input_length,status=(uint32_t)STATUS_INVALID_DEVICE_REQUEST;
        if(ioctl==0x4d014u) {
            status=(uint32_t)STATUS_INVALID_PARAMETER;
            if(in_len>=44 && bridge_buf_ok(in_va,in_len,"NtDeviceIoControlFile")) {
                status=(uint32_t)STATUS_INVALID_DEVICE_REQUEST;
                if(BRIDGE_MEM8(in_va+28)==0x5a && (BRIDGE_MEM8(in_va+30)&0x3f)==0x3e) {
                    uint32_t data=BRIDGE_MEM32(in_va+20),length=BRIDGE_MEM32(in_va+12);
                    status=0xc0000005u;
                    if(bridge_buf_ok(data,length,"DVD MODE SENSE")) {
                        if(length)memset(XBOX_TO_NATIVE(data),0,length);
                        if(length>=2)BRIDGE_MEM8(data+1)=26;
                        if(length>=10){BRIDGE_MEM8(data+8)=0x3e;BRIDGE_MEM8(data+9)=18;}
                        if(length>=13){BRIDGE_MEM8(data+10)=1;BRIDGE_MEM8(data+11)=1;BRIDGE_MEM8(data+12)=1;}
                        BRIDGE_MEM8(in_va+2)=0;status=0;
                    }
                }
            }
        }
        *information=status?0:in_len;return (NTSTATUS)status;
    }

    /* These handles are real host directories. They have no Xbox block
     * device, partition layout or raw first-page storage to query. A native
     * directory is not permission to manufacture a successful disk reply. */
    return STATUS_INVALID_DEVICE_REQUEST;
}

static void bridge_control_file(int filesystem) {
    g_eax=(uint32_t)(filesystem?xbox_NtFsControlFile:xbox_NtDeviceIoControlFile)(
        bridge_resolve_handle(STACK_ARG(0)),bridge_resolve_handle(STACK_ARG(1)),
        (PIO_APC_ROUTINE)XBOX_TO_NATIVE(STACK_ARG(2)),XBOX_TO_NATIVE(STACK_ARG(3)),XBOX_TO_NATIVE(STACK_ARG(4)),STACK_ARG(5),
        XBOX_TO_NATIVE(STACK_ARG(6)),STACK_ARG(7),XBOX_TO_NATIVE(STACK_ARG(8)),STACK_ARG(9));
}
static void bridge_NtDeviceIoControlFile(void) {bridge_control_file(0);}
static void bridge_NtFsControlFile(void) {bridge_control_file(1);}

/* ── NtCreateDirectoryObject (ordinal 188) ──────────────── */
static void bridge_NtCreateDirectoryObject(void)
{
    bridge_create_dispatcher_object(188,STACK_ARG(0),STACK_ARG(1),0,0);
}

/* IoCreateSymbolicLink (ordinal 67, 2 args)
 *
 * Was a bare "return STATUS_SUCCESS": the title was told its link existed and
 * nothing recorded it. Titles mount their own drive letters this way --
 * Wreckless links \??\Z: to \Device\Harddisk0\Partition1\ and then loads
 * every asset through z:\ -- so dropping the link left xbox_translate_path
 * applying the generic "Z: is the cache partition" rule, and every asset open
 * failed with ERROR_FILE_NOT_FOUND a whole boot later.
 *
 * Both arguments are guest ANSI_STRINGs whose Buffer field holds a guest VA,
 * so passing the structs straight through would have xbox_copy_ansi read a
 * 32-bit guest address as a 64-bit host pointer. Rebuild them by hand, the way
 * bridge_RtlEqualString does.
 */
static void bridge_IoCreateSymbolicLink(void)
{
    uint32_t link_va   = STACK_ARG(0);
    uint32_t target_va = STACK_ARG(1);
    XBOX_ANSI_STRING link, target;

    if (!link_va) {
        g_eax = 0xC000000Du;  /* STATUS_INVALID_PARAMETER */
        return;
    }
    link.Length        = BRIDGE_MEM16(link_va + 0);
    link.MaximumLength = BRIDGE_MEM16(link_va + 2);
    link.Buffer        = (PCHAR)XBOX_TO_NATIVE(BRIDGE_MEM32(link_va + 4));

    if (target_va) {
        target.Length        = BRIDGE_MEM16(target_va + 0);
        target.MaximumLength = BRIDGE_MEM16(target_va + 2);
        target.Buffer        = (PCHAR)XBOX_TO_NATIVE(BRIDGE_MEM32(target_va + 4));
    } else {
        target.Length = target.MaximumLength = 0;
        target.Buffer = NULL;
    }

    g_eax = (uint32_t)xbox_IoCreateSymbolicLink(&link,
                                                target_va ? &target : NULL);
}

/* ── ObReferenceObjectByHandle (ordinal 246) ─────────────── */
static void bridge_ObReferenceObjectByHandle(void)
{
    uint32_t object=0,output=STACK_ARG(2);
    g_eax=output?(uint32_t)ke_reference_handle(bridge_resolve_handle(STACK_ARG(0)),STACK_ARG(1),&object):0xc000000du;
    if(output)BRIDGE_MEM32(output)=object;
}

/* Xbox CONTEXT has the extended register area before its integer state.
 * These offsets are shared by the original capture, raise and continue paths. */
static void bridge_capture_float_context(uint32_t context,const XboxGuestRegisters *state) {
    RecompFpuState fp;
    memcpy(fp.stack,state->fp_stack,sizeof fp.stack);memcpy(fp.mm,state->mm,sizeof fp.mm);
    memcpy(fp.raw,state->fp_raw,sizeof fp.raw);memcpy(fp.xmm,state->xmm,sizeof fp.xmm);
    fp.control=state->fp_control_word;
    fp.status=(uint16_t)((state->fp_status_word&~0x7f00u)|state->fp_cc|((state->fp_top&7u)<<11));
    fp.tag=state->fp_tag_word;fp.opcode=state->fp_opcode;
    fp.code_selector=state->fp_code_selector;fp.data_selector=state->fp_data_selector;
    fp.instruction_pointer=state->fp_instruction_pointer;fp.data_pointer=state->fp_data_pointer;
    fp.mxcsr=state->mxcsr;recomp_fp_save_extended(XBOX_TO_NATIVE(context+4),&fp);
}
static void bridge_restore_float_context(uint32_t context,XboxGuestRegisters *state) {
    RecompFpuState fp;recomp_fp_load_extended(XBOX_TO_NATIVE(context+4),&fp);
    memcpy(state->fp_stack,fp.stack,sizeof fp.stack);memcpy(state->mm,fp.mm,sizeof fp.mm);
    memcpy(state->fp_raw,fp.raw,sizeof fp.raw);memcpy(state->xmm,fp.xmm,sizeof fp.xmm);
    state->fp_control_word=fp.control;state->fp_top=(fp.status>>11)&7u;
    state->fp_cc=fp.status&0x4700u;state->fp_status_word=fp.status&~0x7f00u;
    state->fp_cmp=(state->fp_cc&0x400u)?2:(state->fp_cc&0x100u)?-1:(state->fp_cc&0x4000u)?0:1;
    state->fp_tag_word=fp.tag;state->fp_opcode=fp.opcode;
    state->fp_code_selector=fp.code_selector;state->fp_data_selector=fp.data_selector;
    state->fp_instruction_pointer=fp.instruction_pointer;state->fp_data_pointer=fp.data_pointer;
    state->mxcsr=fp.mxcsr;
}
static void bridge_capture_context(uint32_t context,const XboxGuestRegisters *state,
                                   uint32_t address,uint32_t stack) {
    memset(XBOX_TO_NATIVE(context),0,0x240);
    BRIDGE_MEM32(context)=0x10007u;bridge_capture_float_context(context,state);
    BRIDGE_MEM32(context+0x208)=state->edi; BRIDGE_MEM32(context+0x20c)=state->esi;
    BRIDGE_MEM32(context+0x210)=state->ebx; BRIDGE_MEM32(context+0x214)=state->edx;
    BRIDGE_MEM32(context+0x218)=state->ecx; BRIDGE_MEM32(context+0x21c)=state->eax;
    BRIDGE_MEM32(context+0x220)=state->ebp; BRIDGE_MEM32(context+0x224)=address;
    BRIDGE_MEM32(context+0x228)=8; BRIDGE_MEM32(context+0x234)=0x10;
    BRIDGE_MEM32(context+0x22c)=(state->eflags & ~0x30600u)|2u|
        ((state->interrupt_state&1u)<<9)|(state->df?0x400u:0u);
    BRIDGE_MEM32(context+0x230)=stack;
}
static void bridge_restore_context(uint32_t context,XboxGuestRegisters *state) {
    bridge_restore_float_context(context,state);
    state->edi=BRIDGE_MEM32(context+0x208);state->esi=BRIDGE_MEM32(context+0x20c);
    state->ebx=BRIDGE_MEM32(context+0x210);state->edx=BRIDGE_MEM32(context+0x214);
    state->ecx=BRIDGE_MEM32(context+0x218);state->eax=BRIDGE_MEM32(context+0x21c);
    state->ebp=state->seh_ebp=BRIDGE_MEM32(context+0x220);
    state->eflags=BRIDGE_MEM32(context+0x22c)|2u;state->df=(state->eflags>>10)&1u;
    state->interrupt_state=(state->eflags>>9)&1u;state->esp=BRIDGE_MEM32(context+0x230);
    if(!xbox_RestoreGuestRegisters(state))recomp_cont_invalid();
}
/* The original execute-handler trampoline installs this registration so a
 * second exception can identify the handler already being dispatched. */
static void bridge_exception_nested_guard(void) {
    uint32_t record=BRIDGE_MEM32(g_esp+4),registration=BRIDGE_MEM32(g_esp+8);
    uint32_t dispatcher=BRIDGE_MEM32(g_esp+16);
    g_eax=1;
    if(!(BRIDGE_MEM32(record+4)&6u)) {
        BRIDGE_MEM32(dispatcher)=BRIDGE_MEM32(registration+8);g_eax=2;
    }
    g_esp+=20;
}
typedef struct GuestExceptionDispatch {
    RecompContFrame frame;XboxGuestRegisters saved;
    uint32_t record,context,registration,nested,dispatcher,next,handler,secondary,phase,guard;
} GuestExceptionDispatch;
static void bridge_exception_remove_guard(GuestExceptionDispatch *state) {
    if(state->guard && BRIDGE_MEM32(XBOX_FS_BASE)==state->guard)
        BRIDGE_MEM32(XBOX_FS_BASE)=BRIDGE_MEM32(state->guard);
    state->guard=0;
}
static void bridge_exception_dispatch_leave(RecompContFrame *frame) {
    if(!frame->retained)bridge_exception_remove_guard((GuestExceptionDispatch*)frame);
}
static int bridge_dispatch_exception(uint32_t record,uint32_t context) {
    GuestExceptionDispatch state={0};state.record=record;state.context=context;
    const RecompContLocal locals[]={
        GUEST_REGISTER_CONT_LOCALS(state.saved,"saved")
        {"record",&state.record,4},{"context",&state.context,4},
        {"registration",&state.registration,4},{"nested",&state.nested,4},
        {"dispatcher",&state.dispatcher,4},{"next",&state.next,4},
        {"handler",&state.handler,4},{"secondary",&state.secondary,4},
        {"phase",&state.phase,4},{"guard",&state.guard,4}
    };
    int tracked=recomp_cont_active(),handled=0;
    uint64_t resume=tracked?kernel_native_cont_enter(&state.frame,KERNEL_CONT_EXCEPTION_DISPATCH,locals,sizeof locals/sizeof locals[0]):0;
    if(tracked)state.frame.on_leave=bridge_exception_dispatch_leave;
    if(resume) {
        if(state.phase==1)goto handler;
        if(state.phase==2)goto secondary;
        recomp_cont_invalid();
    }
    xbox_CaptureGuestRegisters(&state.saved);
    state.registration=BRIDGE_MEM32(XBOX_FS_BASE);state.dispatcher=(state.saved.esp-4u)&~3u;
    if(!bridge_va_mapped(state.dispatcher,4))recomp_cont_invalid();
    while(state.registration && state.registration!=XBOX_SEH_END_OF_CHAIN) {
        if(!bridge_unwind_registration_valid(state.registration)) {
            BRIDGE_MEM32(state.record+4)|=8u;break;
        }
        state.next=BRIDGE_MEM32(state.registration);state.handler=BRIDGE_MEM32(state.registration+4);
        BRIDGE_MEM32(state.dispatcher)=0;state.guard=state.dispatcher-12;
        if(!bridge_va_mapped(state.guard,12))recomp_cont_invalid();
        BRIDGE_MEM32(state.guard)=BRIDGE_MEM32(XBOX_FS_BASE);
        BRIDGE_MEM32(state.guard+4)=KERNEL_EXCEPTION_NESTED_GUARD;
        BRIDGE_MEM32(state.guard+8)=state.registration;
        BRIDGE_MEM32(XBOX_FS_BASE)=state.guard;g_esp=state.guard;state.phase=1;
handler: ;
        uint32_t args[]={state.record,state.registration,state.context,state.dispatcher};
        uint32_t disposition=ke_call_guest(state.handler,args,4,1);
        bridge_exception_remove_guard(&state);
        if(!xbox_RestoreGuestRegisters(&state.saved))recomp_cont_invalid();
        if(state.nested==state.registration) {BRIDGE_MEM32(state.record+4)&=~0x10u;state.nested=0;}
        if(disposition==0 && !(BRIDGE_MEM32(state.record+4)&1u)) {handled=1;break;}
        if(disposition==2) {
            BRIDGE_MEM32(state.record+4)|=0x10u;
            if(BRIDGE_MEM32(state.dispatcher)>state.nested)state.nested=BRIDGE_MEM32(state.dispatcher);
        } else if(disposition!=1) {
            state.secondary=state.dispatcher-0x50;
            memset(XBOX_TO_NATIVE(state.secondary),0,0x50);
            BRIDGE_MEM32(state.secondary)=disposition==0?0xC0000025u:0xC0000026u;
            BRIDGE_MEM32(state.secondary+4)=1;BRIDGE_MEM32(state.secondary+8)=state.record;
            BRIDGE_MEM32(state.secondary+12)=BRIDGE_MEM32(state.record+12);
            g_esp=state.secondary;state.phase=2;
secondary:
            bridge_dispatch_exception(state.secondary,state.context);
            if(!xbox_RestoreGuestRegisters(&state.saved))recomp_cont_invalid();
            break;
        }
        if(state.next!=XBOX_SEH_END_OF_CHAIN && state.next<=state.registration) {
            BRIDGE_MEM32(state.record+4)|=8u;break;
        }
        state.registration=state.next;state.phase=0;
    }
    if(tracked)recomp_cont_leave(&state.frame);return handled;
}
static uint32_t bridge_raise_record(uint32_t record,uint32_t address,uint32_t frame_ebp,
                                    uint32_t resumed_stack) {
    XboxGuestRegisters saved;uint32_t context=0;RecompContFrame frame;
    const RecompContLocal locals[]={
        GUEST_REGISTER_CONT_LOCALS(saved,"saved")
        {"record",&record,4},{"address",&address,4},{"frame_ebp",&frame_ebp,4},
        {"resumed_stack",&resumed_stack,4},{"context",&context,4}
    };
    int tracked=recomp_cont_active();
    uint64_t resume=tracked?kernel_native_cont_enter(&frame,KERNEL_CONT_EXCEPTION_RECORD,locals,sizeof locals/sizeof locals[0]):0;
    if(!resume) {
        xbox_CaptureGuestRegisters(&saved);saved.ebp=frame_ebp;context=(g_esp-0x240u)&~15u;
        if(!bridge_va_mapped(record,0x50) || !bridge_va_mapped(context,0x240))recomp_cont_invalid();
        bridge_capture_context(context,&saved,address,resumed_stack);
        BRIDGE_MEM32(record+12)=address;g_esp=context;
    }
    if(!bridge_dispatch_exception(record,context)) {
        bridge_unhandled_exception(record,address);
    }
    uint32_t target=BRIDGE_MEM32(context+0x224);bridge_restore_context(context,&saved);
    if(tracked)recomp_cont_leave(&frame);return target;
}
uint32_t xbox_CpuException(uint32_t code,uint32_t instruction,uint32_t frame_ebp) {
    uint32_t stack=0,record=0;RecompContFrame frame;
    const RecompContLocal locals[]={{"code",&code,4},{"instruction",&instruction,4},
        {"frame_ebp",&frame_ebp,4},{"stack",&stack,4},{"record",&record,4}};
    int tracked=recomp_cont_active();
    uint64_t resume=tracked?kernel_native_cont_enter(&frame,KERNEL_CONT_CPU_EXCEPTION,locals,5):0;
    if(!resume) {
        stack=g_esp;record=(stack-0x50u)&~15u;
        if(!bridge_va_mapped(record,0x50))recomp_cont_invalid();
        memset(XBOX_TO_NATIVE(record),0,0x50);BRIDGE_MEM32(record)=code;g_esp=record;
    }
    uint32_t target=bridge_raise_record(record,instruction,frame_ebp,stack);
    if(tracked)recomp_cont_leave(&frame);return target;
}

/* ── RtlRaiseException (ordinal 302) ─────────────────────
 * VOID RtlRaiseException(PEXCEPTION_RECORD ExceptionRecord)
 *
 * Called by CRT / SEH code to raise structured exceptions.
 * On Xbox this triggers the kernel exception dispatcher.
 * C++ throws execute the registered guest handlers and their original cleanup.
 * The guest CRT transfers to the selected catch through xbox_ExceptionContinue.
 */
static void bridge_raise_entry(uint32_t kind) {
    uint32_t record_ptr=0,address=0,stack=0,frame_ebp=0,target=0,phase=0;
    RecompContFrame frame;
    const RecompContLocal locals[]={{"kind",&kind,4},{"record",&record_ptr,4},
        {"address",&address,4},{"stack",&stack,4},{"frame_ebp",&frame_ebp,4},
        {"target",&target,4},{"phase",&phase,4}};
    int tracked=recomp_cont_active();
    uint64_t resume=tracked?kernel_native_cont_enter(&frame,KERNEL_CONT_EXCEPTION_ENTRY,locals,7):0;
    if(resume && phase==2)goto transfer;
    if(!resume) {
        record_ptr=STACK_ARG(0);stack=g_esp;address=BRIDGE_MEM32(stack-4);frame_ebp=g_ebp;
        if(kind==2) {
            uint32_t status=record_ptr;record_ptr=(stack-0x50u)&~15u;
            if(!bridge_va_mapped(record_ptr,0x50))recomp_cont_invalid();
            memset(XBOX_TO_NATIVE(record_ptr),0,0x50);
            BRIDGE_MEM32(record_ptr)=status;BRIDGE_MEM32(record_ptr+4)=1;g_esp=record_ptr;
        }
        if(!kind) {
            uint32_t code=record_ptr?BRIDGE_MEM32(record_ptr):0;
    static int raise_count = 0;
    raise_count++;
    if (raise_count <= 10) {
        fprintf(stderr, "  [KERNEL] RtlRaiseException: record=0x%08X code=0x%08X (#%d)\n",
                record_ptr, code, raise_count);
        /* Retain the first exception's original record and exact active
         * continuation chain before a later fault destroys that context. */
#ifdef _WIN32
        uint32_t record[20];
        SIZE_T copied = 0;
        if (record_ptr && ReadProcessMemory(GetCurrentProcess(),
                XBOX_TO_NATIVE(record_ptr), record, sizeof record, &copied) &&
                copied == sizeof record) {
            fprintf(stderr, "    thread=%lu flags=%08X address=%08X parameters=%u",
                    GetCurrentThreadId(), record[1], record[3], record[4]);
            for (unsigned i = 0; i < record[4] && i < 15; i++)
                fprintf(stderr, " %08X", record[5 + i]);
            fputc('\n', stderr);
        } else {
            fprintf(stderr, "    exception record unavailable (%zu bytes read)\n", (size_t)copied);
        }
#endif
        RecompContFrame *frame = recomp_cont_current();
        for (unsigned depth = 0; frame && depth < 64; depth++, frame = frame->parent)
            fprintf(stderr, "    active[%u] function=%08X site=%llX\n",
                    depth, frame->function, (unsigned long long)frame->site);
        if (frame) fprintf(stderr, "    active chain truncated after 64 frames\n");
        fflush(stderr);
    }

        }
        guest_cpu_join();phase=1;
    }
    target=bridge_raise_record(record_ptr,address,frame_ebp,stack+(!kind?4:0));
    if(kind==2)recomp_cont_invalid();
    frame_ebp=g_ebp;phase=2;
transfer:
    if(target!=address)xbox_ExceptionContinue(target,frame_ebp);
    g_esp-=4;
    if(tracked)recomp_cont_leave(&frame);
}
static void bridge_RtlRaiseException(void) {bridge_raise_entry(0);}

/* ── MmMapIoSpace (ordinal 177) ──────────────────────────
 * PVOID MmMapIoSpace(ULONG_PTR PhysicalAddress, ULONG NumberOfBytes, ULONG Protect)
 *
 * Maps physical I/O memory (GPU registers, etc.) into virtual address space.
 * Allocate from Xbox heap so the returned pointer is a valid Xbox VA.
 */
static void bridge_MmMapIoSpace(void)
{
    uint32_t phys_addr = STACK_ARG(0);
    uint32_t num_bytes = STACK_ARG(1);
    uint32_t protect = STACK_ARG(2);
    uint32_t xbox_va = xbox_HeapAlloc(num_bytes, 4096);

    fprintf(stderr, "  [KERNEL] MmMapIoSpace: phys=0x%08X size=%u → Xbox VA 0x%08X\n",
            phys_addr, num_bytes, xbox_va);
    fflush(stderr);

    g_eax = xbox_va;
}

/* ── MmPersistContiguousMemory (ordinal 178) ─────────────
 * VOID MmPersistContiguousMemory(PVOID BaseAddress, ULONG NumberOfBytes, BOOLEAN Persist)
 *
 * Marks contiguous memory as persistent across reboots (for save data).
 * No-op for recompilation.
 */
static void bridge_MmPersistContiguousMemory(void)
{
    /* No-op stub */
    g_eax = 0;
}

/* ── Generic fallback for simple value-only functions ────── */
static void bridge_generic_stub(void)
{
    /* Success-returning stub for functions whose callers only check for 0.
     * Deliberately silent: the caller (kernel_thunk_dispatch) warns for
     * ordinals with no bridge at all, which is the case worth hearing about. */
    g_eax = 0;
}


/* ══════════════════════════════════════════════════════════════════════════
 * Wrappers for the ordinals Halo 2276's thunk table binds but the bridge did
 * not route. Every one of these already had a working xbox_* implementation in
 * src/kernel/*.c; only the wrapper that moves arguments off the simulated stack
 * was missing, so each call was silently a no-op returning 0.
 *
 * Guest pointers go through XBOX_TO_NATIVE, which maps NULL to NULL. Scalars
 * pass straight through. Handles are tokens, not host HANDLEs, so they go
 * through bridge_resolve_handle / bridge_write_handle.
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── AvGetSavedDataAddress (ordinal 1, void) */
static void bridge_AvGetSavedDataAddress(void)
{
    g_eax = (uint32_t)xbox_AvGetSavedDataAddress();
}

/* ── AvSendTVEncoderOption (ordinal 2, 4 args) */
static void bridge_AvSendTVEncoderOption(void)
{
    xbox_AvSendTVEncoderOption(XBOX_TO_NATIVE(STACK_ARG(0)),
                               STACK_ARG(1), STACK_ARG(2),
                               (PULONG)XBOX_TO_NATIVE(STACK_ARG(3)));
    g_eax = 0;
}

/* ── ExFreePool (ordinal 17, 1 arg)
 * Was resolving to a DATA address before the kernel_data_va_for_ordinal fix,
 * so the title was calling into kernel data. Even after that it was an
 * unbridged no-op, which leaks every pool block the title ever frees.
 *
 * Deliberately does NOT call xbox_ExFreePool: that one HeapFrees its argument
 * on the host heap, but every pool block reaches the title as a guest VA from
 * xbox_HeapAlloc, so only the guest heap can release it. xbox_HeapFree answers
 * through the block table and takes the raw 32-bit guest VA. */
static void bridge_ExFreePool(void)
{
    xbox_HeapFree(STACK_ARG(0));
    g_eax = 0;
}

/* IoCreateDevice uses the 32-bit Xbox DEVICE_OBJECT (0x48 bytes), followed
 * by the caller's extension. Host pointer-bearing structs cannot be used here. */
#define XBOX_DEVICE_OBJECT_SIZE 0x48u

static void bridge_IoCreateDevice(void)
{
    uint32_t name_va=STACK_ARG(2),out_va=STACK_ARG(5);XBOX_ANSI_STRING name={0};PVOID object=NULL;
    if(!out_va || !bridge_va_mapped(out_va,4)){g_eax=STATUS_INVALID_PARAMETER;return;}
    if(name_va) {
        if(!bridge_va_mapped(name_va,8)){g_eax=0xc0000005u;return;}
        name.Length=BRIDGE_MEM16(name_va);name.MaximumLength=BRIDGE_MEM16(name_va+2);
        uint32_t buffer=BRIDGE_MEM32(name_va+4);
        if(name.Length && !bridge_va_mapped(buffer,name.Length)){g_eax=0xc0000005u;return;}
        name.Buffer=XBOX_TO_NATIVE(buffer);
    }
    NTSTATUS status=xbox_IoCreateDevice(XBOX_TO_NATIVE(STACK_ARG(0)),STACK_ARG(1),name_va?&name:NULL,
        STACK_ARG(3),(BOOLEAN)STACK_ARG(4),&object);
    BRIDGE_MEM32(out_va)=io_guest_address(object);g_eax=(uint32_t)status;
}

/* ── KeCancelTimer (ordinal 97, 1 arg)
 * BOOLEAN KeCancelTimer(PKTIMER Timer) -- returns whether it was set. */
static void bridge_KeCancelTimer(void)
{
    /* Guest timers belong to the existing scheduler, not the host timer struct. */
    g_eax = (uint32_t)xbox_kernel_cancel_timer(STACK_ARG(0));
}

/* ── KeDisconnectInterrupt (ordinal 100, 1 arg) */
static void bridge_KeDisconnectInterrupt(void)
{
    uint32_t interrupt_va = STACK_ARG(0);
    if (interrupt_va) {
        uint32_t level = BRIDGE_MEM32(interrupt_va + 8);
        if (level < XBOX_MAX_VECTORS)
            InterlockedCompareExchange((volatile LONG *)&g_connected_isr[level],
                                       0, (LONG)interrupt_va);
        BRIDGE_MEM8(interrupt_va + 16) = 0;
    }
    g_eax = 0; /* VOID on Xbox */
}

/* ── KeSetBasePriorityThread (ordinal 143, 2 args) */
/* KeQueryBasePriorityThread (ordinal 124, 1 arg). The implementation has
 * existed in kernel_thread.c all along; only the bridge wrapper was
 * missing, so the thunk fell through to the fallback and returned 0. */
static void bridge_KeQueryBasePriorityThread(void)
{
    g_eax = (uint32_t)xbox_KeQueryBasePriorityThread(
        ke_shadow_lookup(STACK_ARG(0)));
}

static void bridge_KeSetBasePriorityThread(void)
{
    g_eax = (uint32_t)xbox_KeSetBasePriorityThread(
        ke_shadow_lookup(STACK_ARG(0)), (LONG)STACK_ARG(1));
}

/* ── KeStallExecutionProcessor (ordinal 151, 1 arg) */
static void bridge_KeStallExecutionProcessor(void)
{
    xbox_KeStallExecutionProcessor(STACK_ARG(0));
    g_eax = 0;
}

/* ── MmLockUnlockBufferPages (ordinal 175, 3 args) */
static void bridge_MmLockUnlockBufferPages(void)
{
    xbox_MmLockUnlockBufferPages(XBOX_TO_NATIVE(STACK_ARG(0)),
                                 STACK_ARG(1), (BOOLEAN)STACK_ARG(2));
    g_eax = 0;
}

/* ── MmQueryAllocationSize (ordinal 180, 1 arg)
 *
 * Answered from the guest heap's block table, NOT from xbox_MmQueryAllocationSize.
 * That one calls VirtualQuery, which on a translated guest address reports the
 * size of the whole 64 MB guest mapping -- a confidently wrong answer where the
 * title expects the size of the block it allocated. This is the memory-model
 * check the parked-bridge list below asks for, done: the question is about
 * guest memory, so only the guest allocator can answer it.
 */
static void bridge_MmQueryAllocationSize(void)
{
    extern uint32_t xbox_ContiguousBlockSize(uint32_t addr);
    uint32_t va = STACK_ARG(0);
    uint32_t n = xbox_ContiguousBlockSize(va);   /* XPhysicalSize asks this too */
    g_eax = n ? n : xbox_HeapBlockSize(va);
}

/* ── NtCreateMutant (ordinal 192, 3 args) */
static void bridge_NtCreateMutant(void)
{
    bridge_create_dispatcher_object(192,STACK_ARG(0),STACK_ARG(1),STACK_ARG(2),0);
}

/* ── NtReleaseMutant (ordinal 221, 2 args)
 *
 * NTSTATUS NtReleaseMutant(HANDLE MutantHandle, PLONG PreviousCount);
 *
 * The partner of NtCreateMutant above, and routing one without the other is a
 * deadlock generator: the create succeeds, the release silently does nothing,
 * and the mutex stays held forever by a thread that has already exited.
 *
 * That is what the Xbox Dashboard's audio streaming did. Each ambient WAV gets
 * five events, a worker thread and a mutant; the worker finished, failed to
 * release, and terminated. The next attempt could not take the mutex, so the
 * dashboard reopened the same file and spawned another worker with another
 * 512 KB stack, forever -- visible only as a heap that climbed and a tick that
 * never returned.
 *
 * Memory model: a handle token in, an optional 4-byte LONG out through a guest
 * address. Nothing allocates, frees, or hands back a host pointer.
 */
static void bridge_NtReleaseMutant(void)
{
    uint32_t count_va = STACK_ARG(1);

    g_eax = (uint32_t)xbox_NtReleaseMutant(
        bridge_resolve_handle(STACK_ARG(0)),
        count_va ? (PLONG)XBOX_TO_NATIVE(count_va) : NULL);
}

/* -- NtSuspendThread (ordinal 231, 2 args) --------------------------------
 *
 * NTSTATUS NtSuspendThread(HANDLE ThreadHandle, PULONG PreviousSuspendCount);
 *
 * The implementation was already here and only the dispatch entry was missing,
 * which is worse than an outright stub: the call returned 0, so a thread that
 * parked itself believed it had stopped and carried straight on. Wreckless
 * does that on a worker, and the "suspended" thread spun through 289 million
 * kernel calls while the title thought it was idle.
 */
static void bridge_NtSuspendThread(void)
{
    uint32_t count_va = STACK_ARG(1);

    g_eax = (uint32_t)xbox_NtSuspendThread(
        bridge_resolve_handle(STACK_ARG(0)),
        count_va ? (PULONG)XBOX_TO_NATIVE(count_va) : NULL);
}

/* ── NtResumeThread (ordinal 224, 2 args) */
static void bridge_NtResumeThread(void)
{
    g_eax = (uint32_t)xbox_NtResumeThread(
        bridge_resolve_handle(STACK_ARG(0)),
        (PULONG)XBOX_TO_NATIVE(STACK_ARG(1)));
}

/* ── ObfDereferenceObject (ordinal 250, fastcall: object in ecx)
 * Not STACK_ARG(0). Xbox uses __fastcall here, so the argument never reaches
 * the stack and the arg-size entry is 0. Reading it off the stack would
 * dereference whatever the caller happened to leave there. */
static void bridge_ObfDereferenceObject(void)
{
    ke_dereference_object(g_ecx);
}

/* ══════════════════════════════════════════════════════════════════════════
 * More wrappers for xbox_* implementations that had no route. Same rule as
 * the block above: each one was checked against the memory-model bar --
 * reads/writes only happen at caller-supplied guest addresses, XBOX_TO_NATIVE
 * maps guest NULL to host NULL, and none of them allocates, frees, or hands
 * back a host pointer. Any exception is noted in place.
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── ObfReferenceObject (ordinal 251, fastcall: object in ecx)
 * Mirror of 250. Xbox uses __fastcall here too, so the argument never reaches
 * the stack and the arg-size entry is 0; reading it off the stack would
 * dereference whatever the caller happened to leave there. */
static void bridge_ObfReferenceObject(void)
{
    if(ke_reference_pointer(g_ecx,0)<0)recomp_cont_invalid();
}

/* ── HalReadSMBusValue / HalWriteSMBusValue (ordinals 45 / 50, 4 args)
 *
 * The SMC side of the AV-pack / temperature queries titles make at init.
 * Unrouted they read 0 -- "no AV pack connected" -- which made D3D pick the
 * lowest common denominator display mode and 40C sensors read as 0C.
 *
 * HalReadSMBusValue writes one ULONG through a caller-supplied DataValue and
 * nothing else; HalWriteSMBusValue is all scalars. Both are the safe kind the
 * note below names.
 */
static void bridge_HalReadSMBusValue(void)
{
    uint32_t data_va = STACK_ARG(3);

    g_eax = (uint32_t)xbox_HalReadSMBusValue(
        (UCHAR)STACK_ARG(0), (UCHAR)STACK_ARG(1),
        (BOOLEAN)STACK_ARG(2), (PULONG)XBOX_TO_NATIVE(data_va));
}

static void bridge_HalWriteSMBusValue(void)
{
    g_eax = (uint32_t)xbox_HalWriteSMBusValue(
        (UCHAR)STACK_ARG(0), (UCHAR)STACK_ARG(1),
        (BOOLEAN)STACK_ARG(2), STACK_ARG(3));
}

/* ── HalReadWritePCISpace (ordinal 46, 6 args)
 *
 * Pass the guest buffer to the HAL's connected NV2A configuration state.
 * Unimplemented devices and writes are diagnosed by that boundary. */
static void bridge_HalReadWritePCISpace(void)
{
    xbox_HalReadWritePCISpace(
        STACK_ARG(0), STACK_ARG(1), STACK_ARG(2),
        XBOX_TO_NATIVE(STACK_ARG(3)), STACK_ARG(4), (BOOLEAN)STACK_ARG(5));
    g_eax = 0;
}

/* ── HalRequestSoftwareInterrupt / HalClearSoftwareInterrupt (48 / 38, 1 arg)
 * DPC/APC delivery hooks with no interrupt-driven machinery behind them here;
 * both xbox_* versions are documented no-ops. */
static void bridge_HalRequestSoftwareInterrupt(void)
{
    xbox_HalRequestSoftwareInterrupt((KIRQL)STACK_ARG(0));
    g_eax = 0;
}

static void bridge_HalClearSoftwareInterrupt(void)
{
    xbox_HalClearSoftwareInterrupt((KIRQL)STACK_ARG(0));
    g_eax = 0;
}

/* ── ExSaveNonVolatileSetting (ordinal 29, 4 args)
 *
 * The write half of the EEPROM pair whose read side is already routed.
 * xbox_ExSaveNonVolatileSetting logs and returns success without ever reading
 * the Value buffer, so marshalling it across is cosmetic but keeps the write
 * path out of the unknowable-stub category. */
static void bridge_ExSaveNonVolatileSetting(void)
{
    g_eax = (uint32_t)xbox_ExSaveNonVolatileSetting(
        STACK_ARG(0), STACK_ARG(1), XBOX_TO_NATIVE(STACK_ARG(2)),
        STACK_ARG(3));
}

/* ── MmUnmapIoSpace (ordinal 183, 2 args)
 *
 * Deliberately does NOT call xbox_MmUnmapIoSpace: that one VirtualFrees its
 * argument, but the mapping coming back out of bridge_MmMapIoSpace was carved
 * from the guest heap, so only the guest heap can release it. xbox_HeapFree
 * matches against the block table and takes the raw 32-bit guest VA. */
static void bridge_MmUnmapIoSpace(void)
{
    xbox_HeapFree(STACK_ARG(0));
    g_eax = 0;
}

static void bridge_IoDeleteDevice(void)
{
    xbox_IoDeleteDevice(XBOX_TO_NATIVE(STACK_ARG(0)));g_eax=0;
}

/* ── KeQueryInterruptTime (ordinal 125, void)
 * Returns a 64-bit tick count; the caller reads it as edx:eax, so the high
 * half goes to g_edx exactly like the performance-counter bridges. */
static void bridge_KeQueryInterruptTime(void)
{
    uint64_t t = xbox_KeQueryInterruptTime();

    g_eax = (uint32_t)t;
    g_edx = (uint32_t)(t >> 32);
}

/* ── KeSaveFloatingPointState / KeRestoreFloatingPointState (142 / 139, 1 arg)
 *
 * D3D brackets its fixed-function transform code with these on hardware. Both
 * xbox_* implementations are successful no-ops -- Windows user mode preserves
 * FP state across context switches -- so marshalling is a pass-through of one
 * address. */
static void bridge_KeSaveFloatingPointState(void)
{
    g_eax = (uint32_t)xbox_KeSaveFloatingPointState(
        XBOX_TO_NATIVE(STACK_ARG(0)));
}

static void bridge_KeRestoreFloatingPointState(void)
{
    g_eax = (uint32_t)xbox_KeRestoreFloatingPointState(
        XBOX_TO_NATIVE(STACK_ARG(0)));
}

/* ── NtCreateSemaphore (ordinal 193, 4 args)
 *
 * Handle-shaped twin of NtCreateEvent/NtCreateMutant. The native HANDLE is
 * 8 bytes and the guest slot is 4, so the create goes through a local and the
 * result lands via bridge_write_handle. */
static void bridge_NtCreateSemaphore(void)
{
    bridge_create_dispatcher_object(193,STACK_ARG(0),STACK_ARG(1),STACK_ARG(2),STACK_ARG(3));
}

/* ── NtReleaseSemaphore (ordinal 222, 3 args)
 * Handle token in, LONG by value, optional 4-byte PLONG out. */
static void bridge_NtReleaseSemaphore(void)
{
    uint32_t count_va = STACK_ARG(2);

    g_eax = (uint32_t)xbox_NtReleaseSemaphore(
        bridge_resolve_handle(STACK_ARG(0)), (LONG)STACK_ARG(1),
        count_va ? (PLONG)XBOX_TO_NATIVE(count_va) : NULL);
}

/* ── KeAlertThread (ordinal 93, 2 args)
 * Thread passed as an object; resolves through the handle table the way the
 * Nt* thread calls do, degrading to a pass-through for synthetic handles. */
static void bridge_KeAlertThread(void)
{
    g_eax = (uint32_t)xbox_KeAlertThread(
        bridge_resolve_handle(STACK_ARG(0)), (KPROCESSOR_MODE)STACK_ARG(1));
}

/* ── KeWaitForMultipleObjects (ordinal 158, 8 args)
 *
 * KeWaitForSingleObject is routed; the multiple-object sibling was not. Like
 * NtWaitForMultipleObjectsEx, the Objects[] array in guest memory holds 32-bit
 * handle tokens, so each is resolved before the native wait sees it. */
#define BRIDGE_MAXIMUM_WAIT_OBJECTS 64

static void bridge_KeWaitForMultipleObjects(void)
{
    uint32_t count      = STACK_ARG(0);
    uint32_t objects_va = STACK_ARG(1);
    uint32_t wait_type  = STACK_ARG(2);
    uint32_t alertable  = STACK_ARG(5);   /* 3=WaitReason, 4=WaitMode */
    uint32_t timeout_va = STACK_ARG(6);
    HANDLE handles[BRIDGE_MAXIMUM_WAIT_OBJECTS];
    uint32_t objects[BRIDGE_MAXIMUM_WAIT_OBJECTS];
    int types[BRIDGE_MAXIMUM_WAIT_OBJECTS];
    uint32_t i;

    if (count == 0 || count > BRIDGE_MAXIMUM_WAIT_OBJECTS || !objects_va || wait_type > 1) {
        g_eax = (uint32_t)STATUS_INVALID_PARAMETER;
        return;
    }
    for (i = 0; i < count; i++) {
        objects[i] = BRIDGE_MEM32(objects_va + i * 4);
        types[i] = -1;
        handles[i] = ke_guest_event(objects[i], &types[i]);
        if (handles[i]) {
            ke_prepare_event_wait(objects[i], handles[i], types[i]);
        } else {
            types[i] = -1;
            handles[i] = ke_shadow_lookup(objects[i]);
            if (!handles[i]) handles[i] = bridge_resolve_handle(objects[i]);
        }
    }

    g_eax = (uint32_t)xbox_KeWaitForMultipleObjects(
        count, (PVOID *)handles, wait_type,
        STACK_ARG(3), (KPROCESSOR_MODE)STACK_ARG(4),
        (BOOLEAN)alertable, XBOX_TO_NATIVE(timeout_va),
        XBOX_TO_NATIVE(STACK_ARG(7)));
    for (i = 0; i < count; i++) {
        if (types[i] < 0) continue;
        ke_finish_event_wait(objects[i], handles[i], types[i]);
    }
}

#undef BRIDGE_MAXIMUM_WAIT_OBJECTS

/* ── NtSetSystemTime (ordinal 228, 2 args)
 * Accepts the new time, ignores it, reports the old one through the optional
 * PreviousTime out-parameter (8-byte FILETIME at a guest address). */
static void bridge_NtSetSystemTime(void)
{
    uint32_t prev_va = STACK_ARG(1);

    g_eax = (uint32_t)xbox_NtSetSystemTime(
        XBOX_TO_NATIVE(STACK_ARG(0)),
        prev_va ? (PLARGE_INTEGER)XBOX_TO_NATIVE(prev_va) : NULL);
}

/* ── ObReferenceObjectByName (ordinal 247, 5 args)
 *
 * The ANSI_STRING is rebuilt by hand (native struct has a 64-bit Buffer at
 * offset 8, the guest one a 32-bit Buffer at offset 4) and the Object
 * out-parameter is filled through a local so the 8-byte NULL store lands in a
 * local instead of spilling past a 4-byte guest slot. */
static void bridge_ObReferenceObjectByName(void)
{
    uint32_t name_va   = STACK_ARG(0);
    uint32_t object_va = STACK_ARG(4);
    XBOX_ANSI_STRING name;
    PVOID object_local = NULL;

    if (!name_va) {
        g_eax = (uint32_t)STATUS_INVALID_PARAMETER;
        return;
    }
    name.Length        = BRIDGE_MEM16(name_va);
    name.MaximumLength = BRIDGE_MEM16(name_va + 2);
    name.Buffer        = (PCHAR)XBOX_TO_NATIVE(BRIDGE_MEM32(name_va + 4));

    g_eax = (uint32_t)xbox_ObReferenceObjectByName(
        &name, STACK_ARG(1), XBOX_TO_NATIVE(STACK_ARG(2)),
        XBOX_TO_NATIVE(STACK_ARG(3)), &object_local);
    if (object_va)
        BRIDGE_MEM32(object_va) = object_local?(uint32_t)((uintptr_t)object_local-g_xbox_mem_offset):0;
}

/* ── RtlInitUnicodeString (ordinal 290, 2 args)
 *
 * Mirror of the RtlInitAnsiString bridge: the guest UNICODE_STRING is
 * { USHORT Length; USHORT MaximumLength; 32-bit Buffer; } and the Buffer field
 * must carry the guest VA of the source, so the fields are written by hand
 * rather than letting the native xbox_* write a 64-bit host pointer into a
 * 4-byte guest slot. Lengths are in bytes (wide chars x2). */
static void bridge_RtlInitUnicodeString(void)
{
    uint32_t dest_va = STACK_ARG(0);
    uint32_t src_va  = STACK_ARG(1);

    if (!dest_va) {
        g_eax = 0;
        return;
    }
    if (src_va) {
        const uint16_t *wp = (const uint16_t *)XBOX_TO_NATIVE(src_va);
        size_t bytes = 0;

        while (wp[bytes / 2])
            bytes += sizeof(uint16_t);
        if (bytes > 0xFFFE)
            bytes = 0xFFFE;
        BRIDGE_MEM16(dest_va + 0) = (uint16_t)bytes;
        BRIDGE_MEM16(dest_va + 2) = (uint16_t)(bytes + sizeof(uint16_t));
        BRIDGE_MEM32(dest_va + 4) = src_va;
    } else {
        BRIDGE_MEM16(dest_va + 0) = 0;
        BRIDGE_MEM16(dest_va + 2) = 0;
        BRIDGE_MEM32(dest_va + 4) = 0;
    }
    g_eax = 0;
}

/* ── RtlTimeFieldsToTime (ordinal 304, 2 args)
 * Counterpart of the routed RtlTimeToTimeFields: reads a XBOX_TIME_FIELDS and
 * writes a LARGE_INTEGER, both at caller-supplied guest addresses. */
static void bridge_RtlTimeFieldsToTime(void)
{
    g_eax = (uint32_t)xbox_RtlTimeFieldsToTime(
        (PXBOX_TIME_FIELDS)XBOX_TO_NATIVE(STACK_ARG(0)),
        (PLARGE_INTEGER)XBOX_TO_NATIVE(STACK_ARG(1)));
}

/* ── PhyGetLinkState (ordinal 252, 1 arg) */
static void bridge_PhyGetLinkState(void)
{
    g_eax = (uint32_t)xbox_PhyGetLinkState((BOOLEAN)STACK_ARG(0));
}

/* ── PhyInitialize (ordinal 253, 2 args) */
static void bridge_PhyInitialize(void)
{
    g_eax = (uint32_t)xbox_PhyInitialize((BOOLEAN)STACK_ARG(0),
                                         XBOX_TO_NATIVE(STACK_ARG(1)));
}

/* ── RtlTimeToTimeFields (ordinal 305, 2 args) */
static void bridge_RtlTimeToTimeFields(void)
{
    xbox_RtlTimeToTimeFields(
        (PLARGE_INTEGER)XBOX_TO_NATIVE(STACK_ARG(0)),
        (PXBOX_TIME_FIELDS)XBOX_TO_NATIVE(STACK_ARG(1)));
    g_eax = 0;
}

/* ── XcSHAInit / XcSHAUpdate / XcSHAFinal (ordinals 335-337) */
static void bridge_XcSHAInit(void)
{
    xbox_XcSHAInit((PXBOX_SHA_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)));
    g_eax = 0;
}

static void bridge_XcSHAUpdate(void)
{
    xbox_XcSHAUpdate((PXBOX_SHA_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)),
                     (const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(1)),
                     STACK_ARG(2));
    g_eax = 0;
}

static void bridge_XcSHAFinal(void)
{
    xbox_XcSHAFinal((PXBOX_SHA_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)),
                    (UCHAR*)XBOX_TO_NATIVE(STACK_ARG(1)));
    g_eax = 0;
}

/* ── XcRC4Key / XcRC4Crypt (ordinals 338-339) */
static void bridge_XcRC4Key(void)
{
    xbox_XcRC4Key((PXBOX_RC4_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)),
                  STACK_ARG(1),
                  (const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(2)));
    g_eax = 0;
}

static void bridge_XcRC4Crypt(void)
{
    xbox_XcRC4Crypt((PXBOX_RC4_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)),
                    STACK_ARG(1),
                    (UCHAR*)XBOX_TO_NATIVE(STACK_ARG(2)));
    g_eax = 0;
}

/* ── XcHMAC (ordinal 340, 7 args) */
static void bridge_XcHMAC(void)
{
    xbox_XcHMAC((const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(0)), STACK_ARG(1),
                (const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(2)), STACK_ARG(3),
                (const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(4)), STACK_ARG(5),
                (UCHAR*)XBOX_TO_NATIVE(STACK_ARG(6)));
    g_eax = 0;
}

/* ── XcDESKeyParity (ordinal 346, 2 args) */
static void bridge_XcDESKeyParity(void)
{
    xbox_XcDESKeyParity((PUCHAR)XBOX_TO_NATIVE(STACK_ARG(0)), STACK_ARG(1));
    g_eax = 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * The rest of the xbox_* implementations that had no route. Each was checked
 * against the memory-model bar: reads or writes only happen at
 * caller-supplied guest addresses, XBOX_TO_NATIVE maps guest NULL to host
 * NULL, and nothing allocates, frees, or hands back a host pointer unless the
 * exception is stated in place. I/O Manager and HAL entries are mostly
 * documented stubs whose whole contract is a return value; the Xc* crypto
 * entries are stubs and guest-buffer operations.
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── DbgBreakPoint (ordinal 5, void) */
static void bridge_DbgBreakPoint(void)
{
    xbox_DbgBreakPoint();
    g_eax = 0;
}

/* ── AvSetSavedDataAddress (ordinal 4, 1 arg) */
static void bridge_AvSetSavedDataAddress(void)
{
    xbox_AvSetSavedDataAddress(STACK_ARG(0));
    g_eax = 0;
}

/* ── HalDisableSystemInterrupt (ordinal 39, 2 args) */
static void bridge_HalDisableSystemInterrupt(void)
{
    xbox_HalDisableSystemInterrupt(STACK_ARG(0), (KIRQL)STACK_ARG(1));
    g_eax = 0;
}

/* ── HalInitiateShutdown (ordinal 360, void)
 * The implementation calls ExitProcess, so this never actually returns. */
static void bridge_HalInitiateShutdown(void)
{
    xbox_HalInitiateShutdown();
    g_eax = 0;
}

/* ── HalIsResetOrShutdownPending (ordinal 358, void) */
static void bridge_HalIsResetOrShutdownPending(void)
{
    g_eax = (uint32_t)xbox_HalIsResetOrShutdownPending();
}

/* ── WRITE_PORT_BUFFER_ULONG / WRITE_PORT_BUFFER_USHORT (334 / 333, 3 args)
 * Port I/O stubs that ignore every argument, so pointers pass through without
 * translation -- nothing dereferences them. */
static void bridge_WRITE_PORT_BUFFER_ULONG(void)
{
    xbox_WRITE_PORT_BUFFER_ULONG((PULONG)(uintptr_t)STACK_ARG(0),
        (PULONG)(uintptr_t)STACK_ARG(1), STACK_ARG(2));
    g_eax = 0;
}

static void bridge_WRITE_PORT_BUFFER_USHORT(void)
{
    xbox_WRITE_PORT_BUFFER_USHORT((PUSHORT)(uintptr_t)STACK_ARG(0),
        (PUSHORT)(uintptr_t)STACK_ARG(1), STACK_ARG(2));
    g_eax = 0;
}

/* ── I/O Manager stubs (ordinals 61, 62, 69, 73, 74, 79, 81-87, 359)
 *
 * The Xbox I/O manager is used internally by the XDK libraries we replace, so
 * these are all stubs whose contract is a return value plus occasional writes
 * to caller-supplied guest structures (IRP, IO_STATUS_BLOCK, symlink table).
 * None of them hands back a pointer the title dereferences.
 *
 * IofCallDriver / IofCompleteRequest are __fastcall on Xbox, so their
 * arguments live in ecx/edx, never on the stack -- the arg-size entries are 0
 * and reading STACK_ARG would dereference whatever the caller left there.
 */
static void bridge_IoBuildDeviceIoControlRequest(void)
{
    g_eax = io_guest_address(xbox_IoBuildDeviceIoControlRequest(
        STACK_ARG(0), XBOX_TO_NATIVE(STACK_ARG(1)),
        XBOX_TO_NATIVE(STACK_ARG(2)), STACK_ARG(3),
        XBOX_TO_NATIVE(STACK_ARG(4)), STACK_ARG(5),
        (BOOLEAN)STACK_ARG(6), XBOX_TO_NATIVE(STACK_ARG(7)),
        (PXBOX_IO_STATUS_BLOCK)XBOX_TO_NATIVE(STACK_ARG(8))));
}

static void bridge_IoBuildSynchronousFsdRequest(void)
{
    g_eax = io_guest_address(xbox_IoBuildSynchronousFsdRequest(
        STACK_ARG(0), XBOX_TO_NATIVE(STACK_ARG(1)),
        XBOX_TO_NATIVE(STACK_ARG(2)), STACK_ARG(3),
        XBOX_TO_NATIVE(STACK_ARG(4)), XBOX_TO_NATIVE(STACK_ARG(5)),
        (PXBOX_IO_STATUS_BLOCK)XBOX_TO_NATIVE(STACK_ARG(6))));
}

static void bridge_IoDeleteSymbolicLink(void)
{
    uint32_t name_va = STACK_ARG(0);
    XBOX_ANSI_STRING name;

    if (!name_va) {
        g_eax = 0xC000000Du;   /* STATUS_INVALID_PARAMETER */
        return;
    }
    name.Length        = BRIDGE_MEM16(name_va + 0);
    name.MaximumLength = BRIDGE_MEM16(name_va + 2);
    name.Buffer        = (PCHAR)XBOX_TO_NATIVE(BRIDGE_MEM32(name_va + 4));

    g_eax = (uint32_t)xbox_IoDeleteSymbolicLink(&name);
}

static void bridge_IoInitializeIrp(void)
{
    xbox_IoInitializeIrp(XBOX_TO_NATIVE(STACK_ARG(0)),
        (USHORT)STACK_ARG(1), (CCHAR)STACK_ARG(2));
    g_eax = 0;
}

static void bridge_IoInvalidDeviceRequest(void)
{
    g_eax = (uint32_t)xbox_IoInvalidDeviceRequest(
        XBOX_TO_NATIVE(STACK_ARG(0)), XBOX_TO_NATIVE(STACK_ARG(1)));
}

static void bridge_IoMarkIrpMustComplete(void)
{
    xbox_IoMarkIrpMustComplete(XBOX_TO_NATIVE(STACK_ARG(0)));
    g_eax = 0;
}

static void bridge_IoSetIoCompletion(void)
{
    g_eax = (uint32_t)xbox_IoSetIoCompletion(
        XBOX_TO_NATIVE(STACK_ARG(0)), XBOX_TO_NATIVE(STACK_ARG(1)),
        XBOX_TO_NATIVE(STACK_ARG(2)), (NTSTATUS)STACK_ARG(3),
        (ULONG_PTR)STACK_ARG(4));
}

static void bridge_IoStartNextPacket(void)
{
    xbox_IoStartNextPacket(XBOX_TO_NATIVE(STACK_ARG(0)));
    g_eax = 0;
}

static void bridge_IoStartNextPacketByKey(void)
{
    xbox_IoStartNextPacketByKey(XBOX_TO_NATIVE(STACK_ARG(0)), STACK_ARG(1));
    g_eax = 0;
}

static void bridge_IoStartPacket(void)
{
    xbox_IoStartPacket(XBOX_TO_NATIVE(STACK_ARG(0)),
        XBOX_TO_NATIVE(STACK_ARG(1)), (PULONG)XBOX_TO_NATIVE(STACK_ARG(2)));
    g_eax = 0;
}

static void bridge_IoSynchronousDeviceIoControlRequest(void)
{
    g_eax = (uint32_t)xbox_IoSynchronousDeviceIoControlRequest(
        STACK_ARG(0), XBOX_TO_NATIVE(STACK_ARG(1)),
        XBOX_TO_NATIVE(STACK_ARG(2)), STACK_ARG(3),
        XBOX_TO_NATIVE(STACK_ARG(4)), STACK_ARG(5),
        (PULONG)XBOX_TO_NATIVE(STACK_ARG(6)), (BOOLEAN)STACK_ARG(7));
}

static void bridge_IoSynchronousFsdRequest(void)
{
    g_eax = (uint32_t)xbox_IoSynchronousFsdRequest(
        STACK_ARG(0), XBOX_TO_NATIVE(STACK_ARG(1)),
        XBOX_TO_NATIVE(STACK_ARG(2)), STACK_ARG(3),
        XBOX_TO_NATIVE(STACK_ARG(4)));
}

static void bridge_IofCallDriver(void)
{
    g_eax = (uint32_t)xbox_IofCallDriver(XBOX_TO_NATIVE(g_ecx),
                                        XBOX_TO_NATIVE(g_edx));
}

static void bridge_IofCompleteRequest(void)
{
    xbox_IofCompleteRequest(XBOX_TO_NATIVE(g_ecx), (CCHAR)g_edx);
    g_eax = 0;
}

/* ── MmLockUnlockPhysicalPage (ordinal 176, 2 args)
 * Page locking is a no-op in user mode; the physical address is a value,
 * never dereferenced. */
static void bridge_MmLockUnlockPhysicalPage(void)
{
    xbox_MmLockUnlockPhysicalPage(STACK_ARG(0), (BOOLEAN)STACK_ARG(1));
    g_eax = 0;
}

/* ── MmFreeSystemMemory (ordinal 172, 2 args)
 * Guest-heap twin of the allocator above; nothing on the host heap has ever
 * seen this address. */
static void bridge_MmFreeSystemMemory(void)
{
    xbox_HeapFree(STACK_ARG(0));
    g_eax = 0;
}

/* ── MmCreateKernelStack / MmDeleteKernelStack (169 / 170, 2 args)
 *
 * Same memory model as the pair above. The Xbox convention is that
 * MmCreateKernelStack returns the TOP of the stack and the delete call
 * receives (StackBase=top, StackLimit=base). Both halves of that come from the
 * guest heap here: create returns base+size, delete frees the base (arg 1). */
static void bridge_MmCreateKernelStack(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t base_va = xbox_HeapAlloc(size, 16);

    g_eax = base_va ? base_va + size : 0;
}

static void bridge_MmDeleteKernelStack(void)
{
    xbox_HeapFree(STACK_ARG(1));
    g_eax = 0;
}

/* ── Xc* remaining crypto (ordinals 341-345, 347-351)
 *
 * Split into two kinds, both of which clear the memory-model bar:
 *  - the public-key / DES / ModExp entries are documented stubs that ignore
 *    their arguments (no Xbox Live, no on-console key derivation), so the
 *    pointers pass through without being dereferenced;
 *  - XcBlockCrypt/XcKeyTable/XcCryptService/XcUpdateCrypto are the same shape.
 * XcVerifyPKCS1Signature is worth singling out: it returns TRUE so that
 * signature checks succeed instead of rebooting the dashboard. */
static void bridge_XcPKGetKeyLen(void)
{
    g_eax = (uint32_t)xbox_XcPKGetKeyLen(XBOX_TO_NATIVE(STACK_ARG(0)));
}

static void bridge_XcPKDecPrivate(void)
{
    g_eax = (uint32_t)xbox_XcPKDecPrivate(XBOX_TO_NATIVE(STACK_ARG(0)),
        XBOX_TO_NATIVE(STACK_ARG(1)), XBOX_TO_NATIVE(STACK_ARG(2)));
}

static void bridge_XcPKEncPublic(void)
{
    g_eax = (uint32_t)xbox_XcPKEncPublic(XBOX_TO_NATIVE(STACK_ARG(0)),
        XBOX_TO_NATIVE(STACK_ARG(1)), XBOX_TO_NATIVE(STACK_ARG(2)));
}

static void bridge_XcVerifyPKCS1Signature(void)
{
    g_eax = (uint32_t)xbox_XcVerifyPKCS1Signature(
        XBOX_TO_NATIVE(STACK_ARG(0)), XBOX_TO_NATIVE(STACK_ARG(1)),
        XBOX_TO_NATIVE(STACK_ARG(2)));
}

static void bridge_XcModExp(void)
{
    g_eax = (uint32_t)xbox_XcModExp((PULONG)XBOX_TO_NATIVE(STACK_ARG(0)),
        (PULONG)XBOX_TO_NATIVE(STACK_ARG(1)),
        (PULONG)XBOX_TO_NATIVE(STACK_ARG(2)),
        (PULONG)XBOX_TO_NATIVE(STACK_ARG(3)), STACK_ARG(4));
}

static void bridge_XcKeyTable(void)
{
    xbox_XcKeyTable(STACK_ARG(0), XBOX_TO_NATIVE(STACK_ARG(1)),
        (const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(2)));
    g_eax = 0;
}

static void bridge_XcBlockCrypt(void)
{
    xbox_XcBlockCrypt(STACK_ARG(0), XBOX_TO_NATIVE(STACK_ARG(1)),
        XBOX_TO_NATIVE(STACK_ARG(2)), XBOX_TO_NATIVE(STACK_ARG(3)),
        STACK_ARG(4));
    g_eax = 0;
}

static void bridge_XcBlockCryptCBC(void)
{
    xbox_XcBlockCryptCBC(STACK_ARG(0), STACK_ARG(1),
        XBOX_TO_NATIVE(STACK_ARG(2)), XBOX_TO_NATIVE(STACK_ARG(3)),
        XBOX_TO_NATIVE(STACK_ARG(4)), STACK_ARG(5),
        XBOX_TO_NATIVE(STACK_ARG(6)));
    g_eax = 0;
}

static void bridge_XcCryptService(void)
{
    xbox_XcCryptService(STACK_ARG(0), XBOX_TO_NATIVE(STACK_ARG(1)));
    g_eax = 0;
}

static void bridge_XcUpdateCrypto(void)
{
    xbox_XcUpdateCrypto(XBOX_TO_NATIVE(STACK_ARG(0)),
        XBOX_TO_NATIVE(STACK_ARG(1)));
    g_eax = 0;
}

/* ── RtlRip (ordinal 352, 3 args)
 * The CRT rip/assert path. All three pointers are guest C strings, so each is
 * translated; nothing is written. */
static void bridge_RtlRip(void)
{
    xbox_RtlRip((PCHAR)XBOX_TO_NATIVE(STACK_ARG(0)),
        (PCHAR)XBOX_TO_NATIVE(STACK_ARG(1)),
        (PCHAR)XBOX_TO_NATIVE(STACK_ARG(2)));
    g_eax = 0;
}

/* ── RtlAnsiStringToUnicodeString (ordinal 260, 3 args) ──────────────────
 *
 * NOT a call into xbox_RtlAnsiStringToUnicodeString. That one HeapAllocs on
 * the host heap and stores the resulting 64-bit host pointer into the guest
 * string's 4-byte Buffer field -- the memory-model mismatch. This bridge does
 * the whole conversion by hand:
 *
 *  - Source fields are read from the guest struct (Buffer at offset 4);
 *  - the destination stays wherever the title put it (non-allocating), or is
 *    carved from the guest heap (allocating) and its guest VA stored at
 *    offset 4;
 *  - conversion writes only at guest addresses.
 */
static void bridge_RtlAnsiStringToUnicodeString(void)
{
    uint32_t dst_va  = STACK_ARG(0);
    uint32_t src_va  = STACK_ARG(1);
    uint32_t do_alloc = STACK_ARG(2);
    uint32_t s_len, s_buf_va, d_max, d_buf_va = 0;
    uint32_t unicode_bytes;
    int result;

    if (!dst_va || !src_va) {
        g_eax = 0xC000000Du;   /* STATUS_INVALID_PARAMETER */
        return;
    }
    s_len   = BRIDGE_MEM16(src_va + 0);
    s_buf_va = BRIDGE_MEM32(src_va + 4);
    if (!s_buf_va) {
        g_eax = 0xC000000Du;
        return;
    }
    unicode_bytes = (uint32_t)(s_len + 1) * sizeof(WCHAR);

    d_max = BRIDGE_MEM16(dst_va + 2);
    if (do_alloc) {
        d_buf_va = xbox_HeapAlloc(unicode_bytes, 16);
        if (!d_buf_va) {
            g_eax = 0xC0000017u;   /* STATUS_NO_MEMORY */
            return;
        }
        BRIDGE_MEM16(dst_va + 2) = (uint16_t)unicode_bytes;
        BRIDGE_MEM32(dst_va + 4) = d_buf_va;
    } else {
        if (d_max < unicode_bytes) {
            g_eax = 0xC0000205u;   /* STATUS_BUFFER_OVERFLOW */
            return;
        }
        d_buf_va = BRIDGE_MEM32(dst_va + 4);
        if (!d_buf_va) {
            g_eax = 0xC000000Du;
            return;
        }
    }

    result = MultiByteToWideChar(CP_ACP, 0,
        (const char*)XBOX_TO_NATIVE(s_buf_va), (int)s_len,
        (WCHAR*)XBOX_TO_NATIVE(d_buf_va), (int)(unicode_bytes / sizeof(WCHAR)));
    if (result > 0) {
        BRIDGE_MEM16(dst_va + 0) = (uint16_t)(result * sizeof(WCHAR));
        ((WCHAR*)XBOX_TO_NATIVE(d_buf_va))[result] = 0;
        g_eax = 0;
    } else {
        g_eax = 0xC0000001u;   /* STATUS_UNSUCCESSFUL */
    }
}

/* ── RtlUnicodeStringToAnsiString (ordinal 308, 3 args)
 * Mirror of the ANSI→Unicode bridge above. */
static void bridge_RtlUnicodeStringToAnsiString(void)
{
    uint32_t dst_va  = STACK_ARG(0);
    uint32_t src_va  = STACK_ARG(1);
    uint32_t do_alloc = STACK_ARG(2);
    uint32_t s_len, s_buf_va, d_max, d_buf_va = 0;
    uint32_t ansi_bytes;
    int result;

    if (!dst_va || !src_va) {
        g_eax = 0xC000000Du;
        return;
    }
    s_len   = BRIDGE_MEM16(src_va + 0);
    s_buf_va = BRIDGE_MEM32(src_va + 4);
    if (!s_buf_va) {
        g_eax = 0xC000000Du;
        return;
    }
    ansi_bytes = s_len / sizeof(WCHAR) + 1;

    d_max = BRIDGE_MEM16(dst_va + 2);
    if (do_alloc) {
        d_buf_va = xbox_HeapAlloc(ansi_bytes, 16);
        if (!d_buf_va) {
            g_eax = 0xC0000017u;
            return;
        }
        BRIDGE_MEM16(dst_va + 2) = (uint16_t)ansi_bytes;
        BRIDGE_MEM32(dst_va + 4) = d_buf_va;
    } else {
        if (d_max < ansi_bytes) {
            g_eax = 0xC0000205u;
            return;
        }
        d_buf_va = BRIDGE_MEM32(dst_va + 4);
        if (!d_buf_va) {
            g_eax = 0xC000000Du;
            return;
        }
    }

    result = WideCharToMultiByte(CP_ACP, 0,
        (const WCHAR*)XBOX_TO_NATIVE(s_buf_va), (int)(s_len / sizeof(WCHAR)),
        (char*)XBOX_TO_NATIVE(d_buf_va), (int)ansi_bytes, NULL, NULL);
    if (result > 0) {
        BRIDGE_MEM16(dst_va + 0) = (uint16_t)result;
        if ((uint16_t)result < d_max)
            ((char*)XBOX_TO_NATIVE(d_buf_va))[result] = 0;
        g_eax = 0;
    } else {
        g_eax = 0xC0000001u;
    }
}

/* ── NtDuplicateObject (ordinal 197, 3 args)
 *
 * Xbox NtDuplicateObject has one process, and the handle table is ours: a
 * by-value source token in, a 4-byte guest slot for the duplicated target.
 * The source resolves through the table, DuplicateHandle produces a real
 * native handle, and the target lands via bridge_write_handle as a fresh
 * token -- same shape as NtCreateEvent. DUPLICATE_CLOSE_SOURCE also closes
 * the native handle behind the source token. Retire that token before assigning
 * the duplicate, since Windows may reuse the same native handle value. */
static void bridge_NtDuplicateObject(void)
{
    uint32_t source_token = STACK_ARG(0), options = STACK_ARG(2);
    uint32_t target_va = STACK_ARG(1);
    HANDLE src = bridge_resolve_handle(source_token);
    uint32_t object=ke_object_for_handle(src);
    HANDLE dup = NULL;

    if (!target_va) {
        g_eax = 0xC000000Du;   /* STATUS_INVALID_PARAMETER */
        return;
    }
    int async = bridge_handle_is_async(source_token);
    uint32_t queue=(options&1)?ke_completion_for_handle(src):0;
    NTSTATUS status=xbox_NtDuplicateObject(src,&dup,options);
    if(options&1) {
        /* The native value may already have been reused by the duplicate.
         * Retire the source token before assigning the new native handle. */
        bridge_take_handle(source_token);
        bridge_forget_async_handle(source_token);
#ifdef _WIN32
        xbox_dir_context_drop(src);
#endif
    }
    bridge_write_handle(target_va, dup);
    if(dup && async)bridge_note_async_handle(BRIDGE_MEM32(target_va));
    if(queue)ke_maybe_release_queue(queue);
    if(object)ke_release_object_if_unused(object);
    g_eax = (uint32_t)status;
}

/* ── Dispatch table: ordinal → bridge function + stack arg bytes ── */

typedef void (*bridge_func_t)(void);

/**
 * stdcall arg byte count for each kernel ordinal.
 * On x86 stdcall, the callee cleans (ret N). Our bridges must do the same
 * via g_esp += N after execution so the simulated stack stays balanced.
 *
 * Special cases:
 *   - KfRaiseIrql/KfLowerIrql: fastcall (arg in ecx), 0 stack bytes
 *   - KeSetTimer: DueTime is LARGE_INTEGER (8 bytes on stack) + Timer + Dpc
 */
/* ---- Section B: Rtl* bridge functions (ordinals 261-321) ----
 * Drawn from bridges_Rtl.c and spliced here at byte level. Macro and status
 * definitions inside remain #ifndef-guarded so the block still compiles
 * standalone. Routing lives in the two dispatch tables below. */
/* NTSTATUS values kernel.h does not define. */
#ifndef STATUS_BUFFER_TOO_SMALL
#define STATUS_BUFFER_TOO_SMALL    ((NTSTATUS)0xC0000023L)
#endif
#ifndef STATUS_INTEGER_OVERFLOW
#define STATUS_INTEGER_OVERFLOW    ((NTSTATUS)0xC0000095L)
#endif

/* ── Guest string helpers ─────────────────────────────────── */

/* Byte length (in UTF-16 bytes) of the guest C string at src_va, capped so the
 * caller can always add a terminator without overflowing USHORT Length. */
static uint32_t bridge_guest_wcslen_bytes(uint32_t src_va)
{
    const uint16_t *wp = (const uint16_t *)XBOX_TO_NATIVE(src_va);
    uint32_t bytes = 0;

    while (wp[bytes / 2]) {
        bytes += sizeof(uint16_t);
        if (bytes > 0xFFFE)
            return 0xFFFE;
    }
    return bytes;
}

/* ── Rtl Append (ordinals 261, 262, 263) ────────────────────
 * Each appends a source to a destination XBOX_*_STRING, honouring the
 * destination MaximumLength; the source bump and the destination length both
 * live in guest memory. Returns STATUS_SUCCESS or STATUS_BUFFER_TOO_SMALL. */

/* 261: NTSTATUS RtlAppendStringToString(PXBOX_ANSI_STRING, PXBOX_ANSI_STRING) */
static void bridge_RtlAppendStringToString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint16_t d_len = 0, d_max = 0, s_len = 0;
    uint32_t d_buf = 0, s_buf = 0;

    if (!dst_va || !src_va) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    d_len = BRIDGE_MEM16(dst_va + 0);
    d_max = BRIDGE_MEM16(dst_va + 2);
    d_buf = BRIDGE_MEM32(dst_va + 4);
    s_len = BRIDGE_MEM16(src_va + 0);
    s_buf = BRIDGE_MEM32(src_va + 4);
    if (!d_buf || !s_buf) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    if ((uint32_t)d_len + s_len > d_max) { g_eax = (uint32_t)STATUS_BUFFER_TOO_SMALL; return; }
    memcpy((char *)XBOX_TO_NATIVE(d_buf) + d_len, XBOX_TO_NATIVE(s_buf), s_len);
    BRIDGE_MEM16(dst_va + 0) = (uint16_t)(d_len + s_len);
    g_eax = 0;   /* STATUS_SUCCESS */
}

/* 262: NTSTATUS RtlAppendUnicodeStringToString(PXBOX_UNICODE_STRING, PXBOX_UNICODE_STRING) */
static void bridge_RtlAppendUnicodeStringToString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint16_t d_len = 0, d_max = 0, s_len = 0;
    uint32_t d_buf = 0, s_buf = 0;

    if (!dst_va || !src_va) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    d_len = BRIDGE_MEM16(dst_va + 0);
    d_max = BRIDGE_MEM16(dst_va + 2);
    d_buf = BRIDGE_MEM32(dst_va + 4);
    s_len = BRIDGE_MEM16(src_va + 0);
    s_buf = BRIDGE_MEM32(src_va + 4);
    if (!d_buf || !s_buf) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    if ((uint32_t)d_len + s_len > d_max) { g_eax = (uint32_t)STATUS_BUFFER_TOO_SMALL; return; }
    memcpy((char *)XBOX_TO_NATIVE(d_buf) + d_len, XBOX_TO_NATIVE(s_buf), s_len);
    BRIDGE_MEM16(dst_va + 0) = (uint16_t)(d_len + s_len);
    g_eax = 0;   /* STATUS_SUCCESS */
}

/* 263: NTSTATUS RtlAppendUnicodeToString(PXBOX_UNICODE_STRING, PCWSTR) */
static void bridge_RtlAppendUnicodeToString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint16_t d_len = 0, d_max = 0;
    uint32_t d_buf = 0, add_bytes;

    if (!dst_va || !src_va) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    d_len = BRIDGE_MEM16(dst_va + 0);
    d_max = BRIDGE_MEM16(dst_va + 2);
    d_buf = BRIDGE_MEM32(dst_va + 4);
    if (!d_buf) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    add_bytes = bridge_guest_wcslen_bytes(src_va);
    if ((uint32_t)d_len + add_bytes > d_max) { g_eax = (uint32_t)STATUS_BUFFER_TOO_SMALL; return; }
    memcpy((char *)XBOX_TO_NATIVE(d_buf) + d_len, XBOX_TO_NATIVE(src_va), add_bytes);
    BRIDGE_MEM16(dst_va + 0) = (uint16_t)(d_len + add_bytes);
    g_eax = 0;   /* STATUS_SUCCESS */
}

/* ── Rtl Compare (ordinals 270, 271, 280) ───────────────────
 * <0 / 0 / >0 against the string bodies (not just the prefix), matching the
 * NT RtlCompare* contract: compare up to the common length, then lengths. */

/* 270: LONG RtlCompareString(PXBOX_ANSI_STRING, PXBOX_ANSI_STRING, BOOLEAN) */
static void bridge_RtlCompareString(void)
{
    uint32_t s1_va = STACK_ARG(0);
    uint32_t s2_va = STACK_ARG(1);
    uint32_t nocase = STACK_ARG(2);
    uint32_t l1, l2, common, i;
    const uint8_t *p1, *p2;
    int diff;

    if (!s1_va || !s2_va) { g_eax = 0; return; }
    l1 = BRIDGE_MEM16(s1_va + 0);
    l2 = BRIDGE_MEM16(s2_va + 0);
    p1 = (const uint8_t *)XBOX_TO_NATIVE(BRIDGE_MEM32(s1_va + 4));
    p2 = (const uint8_t *)XBOX_TO_NATIVE(BRIDGE_MEM32(s2_va + 4));
    if (!p1 || !p2) { g_eax = 0; return; }

    common = l1 < l2 ? l1 : l2;
    diff = 0;
    for (i = 0; i < common; i++) {
        int a = p1[i], b = p2[i];
        if (nocase) { a = tolower(a); b = tolower(b); }
        if (a != b) { diff = a - b; break; }
    }
    if (!diff)
        diff = (int)l1 - (int)l2;
    g_eax = (uint32_t)(int32_t)diff;
}

/* 271: LONG RtlCompareUnicodeString(PXBOX_UNICODE_STRING, PXBOX_UNICODE_STRING, BOOLEAN) */
static void bridge_RtlCompareUnicodeString(void)
{
    uint32_t s1_va = STACK_ARG(0);
    uint32_t s2_va = STACK_ARG(1);
    uint32_t nocase = STACK_ARG(2);
    uint32_t b1, b2, common, i;
    const uint16_t *p1, *p2;
    int diff;

    if (!s1_va || !s2_va) { g_eax = 0; return; }
    b1 = BRIDGE_MEM16(s1_va + 0);
    b2 = BRIDGE_MEM16(s2_va + 0);
    p1 = (const uint16_t *)XBOX_TO_NATIVE(BRIDGE_MEM32(s1_va + 4));
    p2 = (const uint16_t *)XBOX_TO_NATIVE(BRIDGE_MEM32(s2_va + 4));
    if (!p1 || !p2) { g_eax = 0; return; }

    common = b1 < b2 ? b1 : b2;
    diff = 0;
    for (i = 0; i < common / 2; i++) {
        int a = p1[i], b = p2[i];
        if (nocase) { a = towlower((wint_t)a); b = towlower((wint_t)b); }
        if (a != b) { diff = a - b; break; }
    }
    if (!diff)
        diff = (int)b1 - (int)b2;
    g_eax = (uint32_t)(int32_t)diff;
}

/* 280: BOOLEAN RtlEqualUnicodeString(PXBOX_UNICODE_STRING, PXBOX_UNICODE_STRING, BOOLEAN) */
static void bridge_RtlEqualUnicodeString(void)
{
    uint32_t s1_va = STACK_ARG(0);
    uint32_t s2_va = STACK_ARG(1);
    uint32_t nocase = STACK_ARG(2);
    uint32_t b1, b2, i;
    const uint16_t *p1, *p2;

    if (!s1_va || !s2_va) { g_eax = 0; return; }
    b1 = BRIDGE_MEM16(s1_va + 0);
    b2 = BRIDGE_MEM16(s2_va + 0);
    if (b1 != b2) { g_eax = 0; return; }
    p1 = (const uint16_t *)XBOX_TO_NATIVE(BRIDGE_MEM32(s1_va + 4));
    p2 = (const uint16_t *)XBOX_TO_NATIVE(BRIDGE_MEM32(s2_va + 4));
    if ((!p1 || !p2) && b1) { g_eax = 0; return; }

    for (i = 0; i < b1 / 2; i++) {
        int a = p1[i], b = p2[i];
        if (nocase) {
            a = towlower((wint_t)a);
            b = towlower((wint_t)b);
        }
        if (a != b) { g_eax = 0; return; }
    }
    g_eax = 1;
}

/* ── Rtl Copy (ordinals 272, 273, 317) ────────────────────── */

/* 272: VOID RtlCopyString(PXBOX_ANSI_STRING Dest, PXBOX_ANSI_STRING Src) */
static void bridge_RtlCopyString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint16_t d_max = 0, s_len = 0;
    uint32_t d_buf = 0, s_buf = 0, copy;

    if (!dst_va || !src_va) { g_eax = 0; return; }
    d_max = BRIDGE_MEM16(dst_va + 2);
    d_buf = BRIDGE_MEM32(dst_va + 4);
    s_len = BRIDGE_MEM16(src_va + 0);
    s_buf = BRIDGE_MEM32(src_va + 4);
    copy = s_len < d_max ? s_len : d_max;
    if (copy && d_buf && s_buf)
        memcpy(XBOX_TO_NATIVE(d_buf), XBOX_TO_NATIVE(s_buf), copy);
    BRIDGE_MEM16(dst_va + 0) = (uint16_t)copy;
    g_eax = 0;
}

/* 273: VOID RtlCopyUnicodeString(PXBOX_UNICODE_STRING Dest, PXBOX_UNICODE_STRING Src) */
static void bridge_RtlCopyUnicodeString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint16_t d_max = 0, s_len = 0;
    uint32_t d_buf = 0, s_buf = 0, copy;

    if (!dst_va || !src_va) { g_eax = 0; return; }
    d_max = BRIDGE_MEM16(dst_va + 2);
    d_buf = BRIDGE_MEM32(dst_va + 4);
    s_len = BRIDGE_MEM16(src_va + 0);
    s_buf = BRIDGE_MEM32(src_va + 4);
    copy = s_len < d_max ? s_len : d_max;
    if (copy && d_buf && s_buf)
        memcpy(XBOX_TO_NATIVE(d_buf), XBOX_TO_NATIVE(s_buf), copy);
    BRIDGE_MEM16(dst_va + 0) = (uint16_t)copy;
    g_eax = 0;
}

/* 317: VOID RtlUpperString(PXBOX_ANSI_STRING Dest, PXBOX_ANSI_STRING Src) */
static void bridge_RtlUpperString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint16_t d_max = 0, s_len = 0;
    uint32_t d_buf = 0, s_buf = 0, copy, i;
    const uint8_t *sp;
    uint8_t *dp;

    if (!dst_va || !src_va) { g_eax = 0; return; }
    d_max = BRIDGE_MEM16(dst_va + 2);
    d_buf = BRIDGE_MEM32(dst_va + 4);
    s_len = BRIDGE_MEM16(src_va + 0);
    s_buf = BRIDGE_MEM32(src_va + 4);
    copy = s_len < d_max ? s_len : d_max;
    if (copy && d_buf && s_buf) {
        sp = (const uint8_t *)XBOX_TO_NATIVE(s_buf);
        dp = (uint8_t *)XBOX_TO_NATIVE(d_buf);
        for (i = 0; i < copy; i++)
            dp[i] = (uint8_t)toupper(sp[i]);
    }
    BRIDGE_MEM16(dst_va + 0) = (uint16_t)copy;
    g_eax = 0;
}

/* ── Rtl Create / Free string (ordinals 274, 287) ─────────── */

/* 274: BOOLEAN RtlCreateUnicodeString(PXBOX_UNICODE_STRING Dest, PCWSTR Src)
 * The buffer is carved from the guest heap so the Buffer field stays a guest
 * VA the title can dereference.  Returns TRUE on success. */
static void bridge_RtlCreateUnicodeString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint32_t bytes, buf_va;

    if (!dst_va || !src_va) { g_eax = 0; return; }
    bytes = bridge_guest_wcslen_bytes(src_va);
    buf_va = xbox_HeapAlloc(bytes + sizeof(uint16_t), 16);
    if (!buf_va) { g_eax = 0; return; }
    memcpy(XBOX_TO_NATIVE(buf_va), XBOX_TO_NATIVE(src_va), bytes);
    ((uint16_t *)XBOX_TO_NATIVE(buf_va))[bytes / 2] = 0;
    BRIDGE_MEM16(dst_va + 0) = (uint16_t)bytes;
    BRIDGE_MEM16(dst_va + 2) = (uint16_t)(bytes + sizeof(uint16_t));
    BRIDGE_MEM32(dst_va + 4) = buf_va;
    g_eax = 1;
}

/* 287: VOID RtlFreeUnicodeString(PXBOX_UNICODE_STRING Str) */
static void bridge_RtlFreeUnicodeString(void)
{
    uint32_t str_va = STACK_ARG(0);
    uint32_t buf_va;

    if (!str_va) { g_eax = 0; return; }
    buf_va = BRIDGE_MEM32(str_va + 4);
    if (buf_va)
        xbox_HeapFree(buf_va);
    BRIDGE_MEM16(str_va + 0) = 0;
    BRIDGE_MEM16(str_va + 2) = 0;
    BRIDGE_MEM32(str_va + 4) = 0;
    g_eax = 0;
}

/* ── Rtl Control characters (ordinals 275, 296, 313, 316) ── */

/* 275: WCHAR RtlDowncaseUnicodeChar(WCHAR Ch) */
static void bridge_RtlDowncaseUnicodeChar(void)
{
    uint32_t ch = STACK_ARG(0);
    g_eax = (uint32_t)(uint16_t)towlower((wint_t)(uint16_t)ch);
}

/* 296: CHAR RtlLowerChar(CHAR Ch) */
static void bridge_RtlLowerChar(void)
{
    uint32_t ch = STACK_ARG(0);
    g_eax = (uint32_t)(uint8_t)tolower((int)(uint8_t)ch);
}

/* 313: WCHAR RtlUpcaseUnicodeChar(WCHAR Ch) */
static void bridge_RtlUpcaseUnicodeChar(void)
{
    uint32_t ch = STACK_ARG(0);
    g_eax = (uint32_t)(uint16_t)towupper((wint_t)(uint16_t)ch);
}

/* 316: CHAR RtlUpperChar(CHAR Ch) */
static void bridge_RtlUpperChar(void)
{
    uint32_t ch = STACK_ARG(0);
    g_eax = (uint32_t)(uint8_t)toupper((int)(uint8_t)ch);
}

/* ── Rtl Upcase/Downcase string (ordinals 276, 314) ─────────
 * NT name suggests (2 args) but the real signature takes a third
 * BOOLEAN AllocateDestString (12 stack bytes).  If AllocateDest is set the
 * destination buffer is carved from the guest heap and its guest VA stored in
 * Dest->Buffer; otherwise the caller's existing buffer must fit. */

/* 276: NTSTATUS RtlDowncaseUnicodeString(Dest, Src, AllocDest) */
static void bridge_RtlDowncaseUnicodeString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint32_t do_alloc = STACK_ARG(2);
    uint16_t s_len, d_max = 0;
    uint32_t s_buf, d_buf = 0, i;
    const uint16_t *sp;
    uint16_t *dp;

    if (!dst_va || !src_va) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    s_len = BRIDGE_MEM16(src_va + 0);
    s_buf = BRIDGE_MEM32(src_va + 4);
    if (s_len && !s_buf) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }

    if (do_alloc) {
        d_buf = xbox_HeapAlloc((uint32_t)s_len + sizeof(uint16_t), 16);
        if (!d_buf) { g_eax = (uint32_t)STATUS_NO_MEMORY; return; }
        BRIDGE_MEM16(dst_va + 2) = (uint16_t)(s_len + sizeof(uint16_t));
        BRIDGE_MEM32(dst_va + 4) = d_buf;
    } else {
        d_max = BRIDGE_MEM16(dst_va + 2);
        d_buf = BRIDGE_MEM32(dst_va + 4);
        if (d_max < s_len) { g_eax = (uint32_t)STATUS_BUFFER_TOO_SMALL; return; }
        if (s_len && !d_buf) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    }

    sp = (const uint16_t *)XBOX_TO_NATIVE(s_buf);
    dp = (uint16_t *)XBOX_TO_NATIVE(d_buf);
    for (i = 0; i < (uint32_t)(s_len / 2); i++)
        dp[i] = (uint16_t)towlower((wint_t)sp[i]);
    dp[(uint32_t)(s_len / 2)] = 0;
    BRIDGE_MEM16(dst_va + 0) = s_len;
    g_eax = 0;   /* STATUS_SUCCESS */
}

/* 314: NTSTATUS RtlUpcaseUnicodeString(Dest, Src, AllocDest) */
static void bridge_RtlUpcaseUnicodeString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint32_t do_alloc = STACK_ARG(2);
    uint16_t s_len, d_max = 0;
    uint32_t s_buf, d_buf = 0, i;
    const uint16_t *sp;
    uint16_t *dp;

    if (!dst_va || !src_va) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    s_len = BRIDGE_MEM16(src_va + 0);
    s_buf = BRIDGE_MEM32(src_va + 4);
    if (s_len && !s_buf) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }

    if (do_alloc) {
        d_buf = xbox_HeapAlloc((uint32_t)s_len + sizeof(uint16_t), 16);
        if (!d_buf) { g_eax = (uint32_t)STATUS_NO_MEMORY; return; }
        BRIDGE_MEM16(dst_va + 2) = (uint16_t)(s_len + sizeof(uint16_t));
        BRIDGE_MEM32(dst_va + 4) = d_buf;
    } else {
        d_max = BRIDGE_MEM16(dst_va + 2);
        d_buf = BRIDGE_MEM32(dst_va + 4);
        if (d_max < s_len) { g_eax = (uint32_t)STATUS_BUFFER_TOO_SMALL; return; }
        if (s_len && !d_buf) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    }

    sp = (const uint16_t *)XBOX_TO_NATIVE(s_buf);
    dp = (uint16_t *)XBOX_TO_NATIVE(d_buf);
    for (i = 0; i < (uint32_t)(s_len / 2); i++)
        dp[i] = (uint16_t)towupper((wint_t)sp[i]);
    dp[(uint32_t)(s_len / 2)] = 0;
    BRIDGE_MEM16(dst_va + 0) = s_len;
    g_eax = 0;   /* STATUS_SUCCESS */
}

/* ── Rtl Integer conversion (ordinals 292, 293) ─────────────
 * NT pads RtlIntegerToChar's output to Length with leading zeros and expects
 * Length+1 bytes of buffer (the NUL goes at [Length]). */

/* 292: NTSTATUS RtlIntegerToChar(ULONG Value, ULONG Base, ULONG Length, PCHAR String) */
static void bridge_RtlIntegerToChar(void)
{
    static const char digs[] = "0123456789ABCDEF";
    uint32_t value = STACK_ARG(0);
    uint32_t base  = STACK_ARG(1);
    uint32_t len   = STACK_ARG(2);
    uint32_t str_va = STACK_ARG(3);
    char tmp[40];
    uint32_t i, n;

    if (!str_va) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    if (base < 2 || base > 16) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }

    n = 0;
    {
        uint32_t v = value;
        if (v == 0)
            tmp[n++] = '0';
        while (v) {
            tmp[n++] = digs[v % base];
            v /= base;
        }
        for (i = 0; i < n / 2; i++) {
            char t = tmp[i];
            tmp[i] = tmp[n - 1 - i];
            tmp[n - 1 - i] = t;
        }
    }
    if (n > len) { g_eax = (uint32_t)STATUS_BUFFER_TOO_SMALL; return; }

    for (i = 0; i < len - n; i++)
        BRIDGE_MEM8(str_va + i) = (uint8_t)'0';
    for (i = 0; i < n; i++)
        BRIDGE_MEM8(str_va + (len - n + i)) = (uint8_t)tmp[i];
    BRIDGE_MEM8(str_va + len) = 0;
    g_eax = 0;   /* STATUS_SUCCESS */
}

/* 293: NTSTATUS RtlIntegerToUnicodeString(ULONG Value, ULONG Base, PXBOX_UNICODE_STRING String) */
static void bridge_RtlIntegerToUnicodeString(void)
{
    static const uint16_t digs[] = { '0','1','2','3','4','5','6','7','8','9',
                                     'A','B','C','D','E','F' };
    uint32_t value = STACK_ARG(0);
    uint32_t base  = STACK_ARG(1);
    uint32_t str_va = STACK_ARG(2);
    uint16_t body[32], d_max;
    uint32_t d_buf, i, n_body = 0, total;
    uint32_t v;
    int neg = 0;
    uint16_t *dst;

    if (!str_va) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    if (base < 2 || base > 16) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    d_max = BRIDGE_MEM16(str_va + 2);
    d_buf = BRIDGE_MEM32(str_va + 4);
    if (!d_buf) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }

    if (base == 10 && (LONG)value < 0) {
        neg = 1;
        v = (uint32_t)(0u - value);
    } else {
        v = value;
    }
    if (v == 0)
        body[n_body++] = digs[0];
    while (v) {
        body[n_body++] = digs[v % base];
        v /= base;
    }
    for (i = 0; i < n_body / 2; i++) {
        uint16_t t = body[i];
        body[i] = body[n_body - 1 - i];
        body[n_body - 1 - i] = t;
    }

    total = n_body + (uint32_t)neg;
    if (total * 2 > d_max) { g_eax = (uint32_t)STATUS_BUFFER_TOO_SMALL; return; }

    dst = (uint16_t *)XBOX_TO_NATIVE(d_buf);
    {
        uint32_t k = 0;
        if (neg)
            dst[k++] = (uint16_t)L'-';
        for (i = 0; i < n_body; i++)
            dst[k++] = body[i];
        dst[k] = 0;
    }
    BRIDGE_MEM16(str_va + 0) = (uint16_t)(total * 2);
    g_eax = 0;   /* STATUS_SUCCESS */
}

/* ── Rtl Memory (ordinals 284, 298, 320) ──────────────────── */

/* 284: VOID RtlFillMemory(PVOID Dest, SIZE_T Len, UCHAR Fill) */
static void bridge_RtlFillMemory(void)
{
    uint32_t dest_va = STACK_ARG(0);
    uint32_t len     = STACK_ARG(1);
    uint32_t fill    = STACK_ARG(2);

    if (dest_va && len)
        memset(XBOX_TO_NATIVE(dest_va), (int)(UCHAR)fill, (size_t)len);
    g_eax = 0;
}

/* 298: VOID RtlMoveMemory(PVOID Dest, PVOID Src, SIZE_T Len) */
static void bridge_RtlMoveMemory(void)
{
    uint32_t dest_va = STACK_ARG(0);
    uint32_t src_va  = STACK_ARG(1);
    uint32_t len     = STACK_ARG(2);

    if (dest_va && src_va && len)
        memmove(XBOX_TO_NATIVE(dest_va), XBOX_TO_NATIVE(src_va), (size_t)len);
    g_eax = 0;
}

/* 320: VOID RtlZeroMemory(PVOID Dest, SIZE_T Len) */
static void bridge_RtlZeroMemory(void)
{
    uint32_t dest_va = STACK_ARG(0);
    uint32_t len     = STACK_ARG(1);

    if (dest_va && len)
        memset(XBOX_TO_NATIVE(dest_va), 0, (size_t)len);
    g_eax = 0;
}

/* ── Rtl MultiByte <-> Unicode (ordinals 299, 300, 310, 311, 315) ──
 * All buffers are guest VAs.  MultiByteToWideChar / WideCharToMultiByte are
 * available on both host platforms.  UNICODE_STRING lengths are byte counts. */

/* 299: NTSTATUS RtlMultiByteToUnicodeN(PWCH UnicodeString, ULONG MaxBytesInUnicodeString,
 *       PULONG BytesInUnicodeString, PCCH MultiByteString, ULONG BytesInMultiByteString) */
static void bridge_RtlMultiByteToUnicodeN(void)
{
    uint32_t uni_va    = STACK_ARG(0);
    uint32_t max_bytes = STACK_ARG(1);
    uint32_t bytes_va  = STACK_ARG(2);
    uint32_t mb_va     = STACK_ARG(3);
    uint32_t mb_bytes  = STACK_ARG(4);
    int chars, result;

    if (!uni_va && max_bytes) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    chars = (int)(max_bytes / sizeof(WCHAR));
    result = MultiByteToWideChar(CP_ACP, 0,
        (const char *)XBOX_TO_NATIVE(mb_va), (int)mb_bytes,
        uni_va ? (WCHAR *)XBOX_TO_NATIVE(uni_va) : NULL, chars);
    if (bytes_va)
        BRIDGE_MEM32(bytes_va) = result > 0 ? (uint32_t)(result * sizeof(WCHAR)) : 0;
    g_eax = result > 0 ? 0 : (uint32_t)STATUS_UNSUCCESSFUL;
}

/* 300: NTSTATUS RtlMultiByteToUnicodeSize(PULONG BytesInUnicodeString,
 *       PCCH MultiByteString, ULONG BytesInMultiByteString) */
static void bridge_RtlMultiByteToUnicodeSize(void)
{
    uint32_t bytes_va = STACK_ARG(0);
    uint32_t mb_va    = STACK_ARG(1);
    uint32_t mb_bytes = STACK_ARG(2);
    int result;

    result = MultiByteToWideChar(CP_ACP, 0,
        (const char *)XBOX_TO_NATIVE(mb_va), (int)mb_bytes, NULL, 0);
    if (bytes_va)
        BRIDGE_MEM32(bytes_va) = result > 0 ? (uint32_t)(result * sizeof(WCHAR)) : 0;
    g_eax = result > 0 ? 0 : (uint32_t)STATUS_UNSUCCESSFUL;
}

/* 310: NTSTATUS RtlUnicodeToMultiByteN(PCHAR MultiByteString, ULONG MaxBytesInMultiByteString,
 *       PULONG BytesInMultiByteString, PCWCH UnicodeString, ULONG BytesInUnicodeString) */
static void bridge_RtlUnicodeToMultiByteN(void)
{
    uint32_t mb_va    = STACK_ARG(0);
    uint32_t max_bytes = STACK_ARG(1);
    uint32_t bytes_va = STACK_ARG(2);
    uint32_t uni_va   = STACK_ARG(3);
    uint32_t uni_bytes = STACK_ARG(4);
    int result;

    if (!mb_va && max_bytes) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    result = WideCharToMultiByte(CP_ACP, 0,
        (const WCHAR *)XBOX_TO_NATIVE(uni_va), (int)(uni_bytes / sizeof(WCHAR)),
        mb_va ? (char *)XBOX_TO_NATIVE(mb_va) : NULL, (int)max_bytes,
        NULL, NULL);
    if (bytes_va)
        BRIDGE_MEM32(bytes_va) = result > 0 ? (uint32_t)result : 0;
    g_eax = result > 0 ? 0 : (uint32_t)STATUS_UNSUCCESSFUL;
}

/* 311: NTSTATUS RtlUnicodeToMultiByteSize(PULONG BytesInMultiByteString,
 *       PCWCH UnicodeString, ULONG BytesInUnicodeString) */
static void bridge_RtlUnicodeToMultiByteSize(void)
{
    uint32_t bytes_va = STACK_ARG(0);
    uint32_t uni_va   = STACK_ARG(1);
    uint32_t uni_bytes = STACK_ARG(2);
    int result;

    result = WideCharToMultiByte(CP_ACP, 0,
        (const WCHAR *)XBOX_TO_NATIVE(uni_va), (int)(uni_bytes / sizeof(WCHAR)),
        NULL, 0, NULL, NULL);
    if (bytes_va)
        BRIDGE_MEM32(bytes_va) = result > 0 ? (uint32_t)result : 0;
    g_eax = result > 0 ? 0 : (uint32_t)STATUS_UNSUCCESSFUL;
}

/* 315: NTSTATUS RtlUpcaseUnicodeToMultiByteN(PCHAR MultiByteString,
 *       ULONG MaxBytesInUnicodeString, PULONG BytesInMultiByteString,
 *       PCWCH UnicodeString, ULONG BytesInUnicodeString)
 * WideCharToMultiByte has no case flag, so the source is upcased into a
 * temporary host buffer first. */
static void bridge_RtlUpcaseUnicodeToMultiByteN(void)
{
    uint32_t mb_va     = STACK_ARG(0);
    uint32_t max_bytes = STACK_ARG(1);
    uint32_t bytes_va  = STACK_ARG(2);
    uint32_t uni_va    = STACK_ARG(3);
    uint32_t uni_bytes = STACK_ARG(4);
    uint32_t wc = uni_bytes / sizeof(WCHAR);
    uint16_t *tmp = NULL;
    int result;
    uint32_t i;

    if (!mb_va && max_bytes) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    if (wc) {
        tmp = (uint16_t *)malloc(((size_t)wc + 1) * sizeof(uint16_t));
        if (!tmp) { g_eax = (uint32_t)STATUS_NO_MEMORY; return; }
        {
            const uint16_t *sp = (const uint16_t *)XBOX_TO_NATIVE(uni_va);
            for (i = 0; i < wc; i++)
                tmp[i] = (uint16_t)towupper((wint_t)sp[i]);
            tmp[wc] = 0;
        }
    }
    result = WideCharToMultiByte(CP_ACP, 0,
        tmp, (int)wc,
        mb_va ? (char *)XBOX_TO_NATIVE(mb_va) : NULL, (int)max_bytes,
        NULL, NULL);
    free(tmp);
    if (bytes_va)
        BRIDGE_MEM32(bytes_va) = result > 0 ? (uint32_t)result : 0;
    g_eax = result > 0 ? 0 : (uint32_t)STATUS_UNSUCCESSFUL;
}

/* ── Rtl String -> Integer (ordinals 267, 309) ────────────── */

/* 267: NTSTATUS RtlCharToInteger(PCSTR Str, ULONG Base, PULONG Value)
 * Base 0 autodetects (0x -> 16, 0 -> 8, else 10), mirroring strtoul. */
static void bridge_RtlCharToInteger(void)
{
    uint32_t str_va = STACK_ARG(0);
    uint32_t base   = STACK_ARG(1);
    uint32_t val_va = STACK_ARG(2);
    const char *p;
    unsigned long long acc = 0;
    uint32_t base_ = base;
    int neg = 0;
    uint32_t st = 0;   /* STATUS_SUCCESS */

    if (!val_va) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    if (base_ != 0 && (base_ < 2 || base_ > 16)) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    if (!str_va) { BRIDGE_MEM32(val_va) = 0; g_eax = 0; return; }
    p = (const char *)XBOX_TO_NATIVE(str_va);

    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
           *p == '\f' || *p == '\v')
        p++;
    if (*p == '-') { neg = 1; p++; }
    else if (*p == '+') p++;

    if (base_ == 0) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base_ = 16; p += 2; }
        else if (p[0] == '0') { base_ = 8; }
        else base_ = 10;
    }

    for (; *p; p++) {
        int d, c = (unsigned char)*p;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        if (d >= (int)base_) break;
        if (acc > (0xFFFFFFFFULL - (unsigned)d) / base_) {
            st = (uint32_t)STATUS_INTEGER_OVERFLOW;
            acc = 0xFFFFFFFFULL;
            break;
        }
        acc = acc * base_ + (unsigned)d;
    }
    if (neg)
        acc = (unsigned long long)(uint32_t)(0u - (uint32_t)acc);
    BRIDGE_MEM32(val_va) = (uint32_t)acc;
    g_eax = st;
}

/* 309: NTSTATUS RtlUnicodeStringToInteger(PXBOX_UNICODE_STRING Str, ULONG Base, PULONG Value) */
static void bridge_RtlUnicodeStringToInteger(void)
{
    uint32_t str_va = STACK_ARG(0);
    uint32_t base   = STACK_ARG(1);
    uint32_t val_va = STACK_ARG(2);
    uint16_t s_len;
    uint32_t s_buf;
    const uint16_t *wp;
    unsigned long long acc = 0;
    uint32_t base_ = base;
    int neg = 0, i, nch;
    uint32_t st = 0;   /* STATUS_SUCCESS */

    if (!val_va) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    if (base_ != 0 && (base_ < 2 || base_ > 16)) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    if (!str_va) { BRIDGE_MEM32(val_va) = 0; g_eax = 0; return; }
    s_len = BRIDGE_MEM16(str_va + 0);
    s_buf = BRIDGE_MEM32(str_va + 4);
    if (!s_buf) { BRIDGE_MEM32(val_va) = 0; g_eax = 0; return; }
    wp = (const uint16_t *)XBOX_TO_NATIVE(s_buf);
    nch = s_len / 2;

    i = 0;
    while (i < nch && (wp[i] == (uint16_t)' ' || wp[i] == (uint16_t)'\t' ||
                       wp[i] == (uint16_t)'\n' || wp[i] == (uint16_t)'\r'))
        i++;
    if (i < nch && wp[i] == (uint16_t)'-') { neg = 1; i++; }
    else if (i < nch && wp[i] == (uint16_t)'+') i++;

    if (base_ == 0) {
        if (i + 1 < nch && wp[i] == (uint16_t)'0' &&
            (wp[i + 1] == (uint16_t)'x' || wp[i + 1] == (uint16_t)'X')) {
            base_ = 16;
            i += 2;
        } else if (i < nch && wp[i] == (uint16_t)'0') {
            base_ = 8;
        } else {
            base_ = 10;
        }
    }

    for (; i < nch; i++) {
        uint16_t c = wp[i];
        int d;
        if (c >= (uint16_t)'0' && c <= (uint16_t)'9') d = c - (uint16_t)'0';
        else if (c >= (uint16_t)'a' && c <= (uint16_t)'f') d = c - (uint16_t)'a' + 10;
        else if (c >= (uint16_t)'A' && c <= (uint16_t)'F') d = c - (uint16_t)'A' + 10;
        else break;
        if (d >= (int)base_) break;
        if (acc > (0xFFFFFFFFULL - (unsigned)d) / base_) {
            st = (uint32_t)STATUS_INTEGER_OVERFLOW;
            acc = 0xFFFFFFFFULL;
            break;
        }
        acc = acc * base_ + (unsigned)d;
    }
    if (neg)
        acc = (unsigned long long)(uint32_t)(0u - (uint32_t)acc);
    BRIDGE_MEM32(val_va) = (uint32_t)acc;
    g_eax = st;
}

/* ── Rtl Byte swaps (ordinals 307, 318) ─────────────────────
 * Hand-rolled rather than MSVC _byteswap_* so the file stays portable. */

/* 307: ULONG RtlUlongByteSwap(ULONG Value) */
static void bridge_RtlUlongByteSwap(void)
{
    uint32_t v = STACK_ARG(0);
    g_eax = ((v & 0x000000FFu) << 24) |
            ((v & 0x0000FF00u) << 8)  |
            ((v & 0x00FF0000u) >> 8)  |
            ((v & 0xFF000000u) >> 24);
}

/* 318: USHORT RtlUshortByteSwap(USHORT Value) */
static void bridge_RtlUshortByteSwap(void)
{
    uint32_t v = STACK_ARG(0);
    g_eax = (uint32_t)(uint16_t)(((v & 0xFFu) << 8) | ((v >> 8) & 0xFFu));
}

/* ── Rtl Critical sections (ordinals 278, 295, 306) ─────────
 * 277/294 (plain Enter/Leave) are routed via the xbox_Rtl* wrappers already.
 *
 * TryEnter cannot reuse xbox_cs_shadow (it is private to kernel_rtl.c), so it
 * keeps its own keyed host-lock table: guest critical-section address -> host
 * CRITICAL_SECTION, created and probed under a guard.  Only the try path uses
 * it, so entering with Try and leaving with LeaveCriticalSection are both
 * routed at the shadow level and stay consistent on the host side. */

#define BRIDGE_TRY_CS_SLOTS 512

typedef struct {
    uint32_t key;             /* guest critical section; 0 means free */
    int      ready;
    CRITICAL_SECTION cs;
} BRIDGE_TRY_CS_SLOT;

static BRIDGE_TRY_CS_SLOT g_try_cs[BRIDGE_TRY_CS_SLOTS];
static CRITICAL_SECTION g_try_cs_guard;
static int g_try_cs_guard_ready;

static void bridge_try_cs_init_guard(void)
{
    /* First call in practice comes from the title's main thread during setup;
     * a second InitializeCriticalSection on the same object is benign. */
    if (!g_try_cs_guard_ready) {
        InitializeCriticalSection(&g_try_cs_guard);
        g_try_cs_guard_ready = 1;
    }
}

static CRITICAL_SECTION *bridge_try_cs_shadow(uint32_t guest)
{
    size_t home, i;

    bridge_try_cs_init_guard();
    EnterCriticalSection(&g_try_cs_guard);
    home = (size_t)((guest >> 4) % BRIDGE_TRY_CS_SLOTS);
    for (i = 0; i < BRIDGE_TRY_CS_SLOTS; i++) {
        BRIDGE_TRY_CS_SLOT *slot = &g_try_cs[(home + i) % BRIDGE_TRY_CS_SLOTS];
        if (slot->key == guest) {
            LeaveCriticalSection(&g_try_cs_guard);
            return &slot->cs;
        }
        if (slot->key == 0) {
            if (!slot->ready) {
                InitializeCriticalSection(&slot->cs);
                slot->ready = 1;
            }
            slot->key = guest;
            LeaveCriticalSection(&g_try_cs_guard);
            return &slot->cs;
        }
    }
    LeaveCriticalSection(&g_try_cs_guard);
    return NULL;
}

/* 278: VOID RtlEnterCriticalSectionAndRegion(PRTL_CRITICAL_SECTION) */
static void bridge_RtlEnterCriticalSectionAndRegion(void)
{
    xbox_RtlEnterCriticalSection((PRTL_CRITICAL_SECTION)XBOX_TO_NATIVE(STACK_ARG(0)));
    g_eax = 0;
}

/* 295: VOID RtlLeaveCriticalSectionAndRegion(PRTL_CRITICAL_SECTION) */
static void bridge_RtlLeaveCriticalSectionAndRegion(void)
{
    xbox_RtlLeaveCriticalSection((PRTL_CRITICAL_SECTION)XBOX_TO_NATIVE(STACK_ARG(0)));
    g_eax = 0;
}

/* 306: BOOLEAN RtlTryEnterCriticalSection(PRTL_CRITICAL_SECTION) */
static void bridge_RtlTryEnterCriticalSection(void)
{
    CRITICAL_SECTION *cs = bridge_try_cs_shadow(STACK_ARG(0));
    if (!cs) { g_eax = 0; return; }
    g_eax = TryEnterCriticalSection(cs) ? 1 : 0;
}

/* ── Rtl Misc stubs (ordinals 288, 297, 319, 321) ─────────── */

/* 288: VOID RtlGetCallersAddress(PVOID *CallersAddress, PVOID *CallersCaller) */
static void bridge_RtlGetCallersAddress(void)
{
    uint32_t a_va = STACK_ARG(0);
    uint32_t c_va = STACK_ARG(1);

    if (a_va) BRIDGE_MEM32(a_va) = 0;
    if (c_va) BRIDGE_MEM32(c_va) = 0;
    g_eax = 0;
}

/* 297: VOID RtlMapGenericMask(PACCESS_MASK AccessMask, PRTL_GENERIC_MAPPING) */
static void bridge_RtlMapGenericMask(void)
{
    g_eax = 0;
}

/* 319: ULONG RtlWalkFrameChain(PVOID *Callers, ULONG Count, ULONG Flags) */
static void bridge_RtlWalkFrameChain(void)
{
    g_eax = 0;
}

/* 321: XboxEEPROMKey (data export, 0 args)
 * Not in the kernel_data_va_for_ordinal table, so it needs a bridge that hands
 * the caller a guest-addressable 16-byte key.  Carved from the guest heap once. */
static void bridge_XboxEEPROMKey(void)
{
    static uint32_t eeprom_va = 0;

    if (!eeprom_va) {
        eeprom_va = xbox_HeapAlloc(16, 16);
        if (eeprom_va)
            memset(XBOX_TO_NATIVE(eeprom_va), 0, 16);
    }
    g_eax = eeprom_va;
}

/* ── Rtl Exception (ordinals 264, 265, 266, 303) ──────────── */

/* 264: VOID RtlAssert(PVOID FailedAssertion, PVOID FileName, ULONG LineNumber) */
static void bridge_RtlAssert(void)
{
    uint32_t assert_va = STACK_ARG(0);
    uint32_t file_va   = STACK_ARG(1);
    uint32_t line      = STACK_ARG(2);
    const char *a = assert_va ? (const char *)XBOX_TO_NATIVE(assert_va) : NULL;
    const char *f = file_va   ? (const char *)XBOX_TO_NATIVE(file_va) : NULL;

    xbox_log(XBOX_LOG_ERROR, XBOX_LOG_RTL, "RtlAssert: %s:%u %s",
             f ? f : "?", line, a ? a : "?");
#ifdef _DEBUG
    DebugBreak();
#endif
    g_eax = 0;
}

/* RtlCaptureContext captures its caller's frame, as the original routine does. */
static void bridge_RtlCaptureContext(void)
{
    uint32_t context=STACK_ARG(0);XboxGuestRegisters state;
    xbox_CaptureGuestRegisters(&state);
    if(!bridge_va_mapped(context,0x240) || !bridge_va_mapped(g_ebp,8))recomp_cont_invalid();
    state.ebp=BRIDGE_MEM32(g_ebp);
    bridge_capture_context(context,&state,BRIDGE_MEM32(g_ebp+4),g_ebp+8);
}

/* 266: ULONG RtlCaptureStackBackTrace(FramesToSkip, FramesToCapture,
 *       BackTrace, BackTraceHash) - stub, returns 0 frames. Stack is cleaned
 *       up per stdcall_args_for_ordinal (20 bytes in kernel_bridge.c). */
static void bridge_RtlCaptureStackBackTrace(void)
{
    g_eax = 0;
}


/* --- Ke Object Shadow Table --- */
#define KE_SHADOW_SIZE 4096
typedef struct {
    uint32_t guest_va;HANDLE host_handle;uint32_t in_use;
    /* Object-manager references share the existing dispatcher identity.
     * allocation owns either the object body or a worker's complete TIB. */
    uint32_t object_type,references,allocation;
    uint32_t name_parent,name_address,name_length,name_attributes,namespace_root,path_rule;
} KE_SHADOW_SLOT;
static KE_SHADOW_SLOT g_ke_shadow[KE_SHADOW_SIZE];
static KE_SHADOW_SLOT *ke_object_slot(uint32_t);
static CRITICAL_SECTION g_ke_shadow_cs;
static int g_ke_shadow_init;

static void ke_shadow_init(void)
{
    if (g_ke_shadow_init)
        return;
    InitializeCriticalSection(&g_ke_shadow_cs);
    /* Wipe any stale pointers from a prior lifecycle (defensive). */
    memset(g_ke_shadow, 0, sizeof(g_ke_shadow));
    g_ke_shadow_init = 1;
}

/* Insert a mapping guest_va -> host HANDLE. Replaces any prior mapping for
 * the same guest_va (closing the old handle and inserting the new one). */
static int ke_shadow_insert(uint32_t guest_va, HANDLE host)
{
    uint32_t i;

    if (!guest_va || !host) {
        if (host) CloseHandle(host);
        return 0;
    }

    ke_shadow_init();

    EnterCriticalSection(&g_ke_shadow_cs);

    /* Reuse an existing slot for this VA, if present. */
    for (i = 0; i < KE_SHADOW_SIZE; i++) {
        if (g_ke_shadow[i].in_use && g_ke_shadow[i].guest_va == guest_va) {
            if (g_ke_shadow[i].host_handle && g_ke_shadow[i].host_handle != host)
                CloseHandle(g_ke_shadow[i].host_handle);
            if(g_ke_shadow[i].object_type)recomp_cont_invalid();
            g_ke_shadow[i]=(KE_SHADOW_SLOT){guest_va,host,1};
            LeaveCriticalSection(&g_ke_shadow_cs);
            return 1;
        }
    }

    /* Otherwise find a free slot. */
    for (i = 0; i < KE_SHADOW_SIZE; i++) {
        if (!g_ke_shadow[i].in_use) {
            g_ke_shadow[i].in_use      = 1;
            g_ke_shadow[i].guest_va    = guest_va;
            g_ke_shadow[i].host_handle = host;
            LeaveCriticalSection(&g_ke_shadow_cs);
            return 1;
        }
    }

    /* Table full -- log and drop the new object. The caller's subsequent
     * waits will fall back to XBOX_TO_NATIVE and fail gracefully. */
    fprintf(stderr, "  [KERNEL] Ke shadow table full; dropping VA 0x%08X\n",
            guest_va);
    LeaveCriticalSection(&g_ke_shadow_cs);
    CloseHandle(host);
    return 0;
}

/* Look up the host HANDLE for a guest object VA. Returns NULL if not found. */
static HANDLE ke_shadow_lookup(uint32_t guest_va)
{
    uint32_t i;
    HANDLE h = NULL;

    if (!guest_va)
        return NULL;

    ke_shadow_init();

    EnterCriticalSection(&g_ke_shadow_cs);
    for (i = 0; i < KE_SHADOW_SIZE; i++) {
        if (g_ke_shadow[i].in_use && g_ke_shadow[i].guest_va == guest_va) {
            h = g_ke_shadow[i].host_handle;
            break;
        }
    }
    LeaveCriticalSection(&g_ke_shadow_cs);
    return h;
}

/* Remove (and close) the mapping for a guest object VA. */
static void ke_shadow_remove(uint32_t guest_va)
{
    uint32_t i;

    if (!guest_va)
        return;

    ke_shadow_init();

    EnterCriticalSection(&g_ke_shadow_cs);
    for (i = 0; i < KE_SHADOW_SIZE; i++) {
        if (g_ke_shadow[i].in_use && g_ke_shadow[i].guest_va == guest_va) {
            if (g_ke_shadow[i].host_handle)
                CloseHandle(g_ke_shadow[i].host_handle);
            memset(&g_ke_shadow[i],0,sizeof g_ke_shadow[i]);
            break;
        }
    }
    LeaveCriticalSection(&g_ke_shadow_cs);
}

/* Resolve a guest event or timer through its existing host shadow. Kernel
 * initialized objects and title-built events expose the same SignalState.
 * RECOMP_TITLE_KEVENTS enables creation of shadows for title-built events;
 * it does not disable state synchronization for objects already registered. */
static HANDLE ke_guest_event(uint32_t guest_va, int *type)
{
    static int enabled = -1;
    uint32_t hdr, i;
    HANDLE h = NULL;

    if (enabled < 0) {
        const char *e = getenv("RECOMP_TITLE_KEVENTS");
        enabled = e && *e == '1';
    }
    if (guest_va < 0x10000u || (guest_va & 3u))
        return NULL;
    if (guest_va >= 0x04000000u && (guest_va < 0x80000000u || guest_va >= 0x84000000u))
        return NULL;                               /* RAM or the contiguous window */
    hdr = BRIDGE_MEM32(guest_va);
    unsigned object_type = hdr & 0xFFu, words = (hdr >> 16) & 0xFFu;
    if (!((object_type <= 1 && words == 4) ||
          ((object_type == 8 || object_type == 9) && words == 10)))
        return NULL;
    *type = (int)(object_type & 1u);

    /* A guest-driver file owns its original FILE_OBJECT.Event through the
     * same anonymous event that carries its public file-handle identity. */
    if(guest_va>=0x38) {
        KE_SHADOW_SLOT *file=ke_object_slot(guest_va-0x38);
        if(file && file->in_use==4 && file->object_type==XBOX_KERNEL_DATA_BASE+KDATA_FILE_OBJ_TYPE)
            return file->host_handle;
    }
    ke_shadow_init();
    EnterCriticalSection(&g_ke_shadow_cs);
    for (i = 0; i < KE_SHADOW_SIZE; i++) {
        if (g_ke_shadow[i].in_use && g_ke_shadow[i].guest_va == guest_va) {
            h = g_ke_shadow[i].host_handle;
            if (g_ke_shadow[i].in_use == 2) *type |= KE_EVENT_TITLE_HEADER;
            LeaveCriticalSection(&g_ke_shadow_cs);
            return h;
        }
    }
    if (!enabled || object_type >= 8) {
        LeaveCriticalSection(&g_ke_shadow_cs);
        return NULL;
    }
    for (i = 0; i < KE_SHADOW_SIZE; i++) {
        if (!g_ke_shadow[i].in_use) {
            h = CreateEventA(NULL, *type == 0, BRIDGE_MEM32(guest_va + 4) != 0, NULL);
            if (h) {
                g_ke_shadow[i].in_use      = 2;
                g_ke_shadow[i].guest_va    = guest_va;
                g_ke_shadow[i].host_handle = h;
            }
            break;
        }
    }
    LeaveCriticalSection(&g_ke_shadow_cs);
    if (h) {
        static int shown;
        if (shown++ < 16)
            fprintf(stderr, "  [KERNEL] event 0x%08X built by the title: host event created "
                            "(%s)\n", guest_va, *type ? "synchronization" : "notification");
    }
    if (h) *type |= KE_EVENT_TITLE_HEADER;
    return h;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Data Exports (102, 120, 154, 240, 245, 249)
 *
 * These ordinals are DATA exports, not functions. The bridge returns a guest
 * VA inside the kernel data page so the title can dereference it. The offsets
 * below (0x510-0x558) are free in xbox_memory_layout.h's KDATA block (existing
 * slots end at 0x4B0+64, KiBugCheckData uses 0x500, page is 4 KB). Prefer
 * moving them into xbox_memory_layout.h and dropping the guarded defines here.
 * ═══════════════════════════════════════════════════════════════════════════
 */
#ifndef KDATA_MMGLOBAL
#define KDATA_MMGLOBAL          0x510  /* MmGlobalData (4 bytes) */
#endif
#ifndef KDATA_INTERRUPT_TIME
#define KDATA_INTERRUPT_TIME    0x520  /* KeInterruptTime (LARGE_INTEGER, 8 bytes) */
#endif
#ifndef KDATA_SYSTEM_TIME
#define KDATA_SYSTEM_TIME       0x530  /* KeSystemTime (LARGE_INTEGER, 8 bytes) */
#endif
#ifndef KDATA_OBJ_DIR_TYPE
#define KDATA_OBJ_DIR_TYPE      0x540  /* ObDirectoryObjectType (4 bytes) */
#endif
#ifndef KDATA_OBJ_HANDLE_TABLE
#define KDATA_OBJ_HANDLE_TABLE  0x548  /* ObpObjectHandleTable (4 bytes) */
#endif
#ifndef KDATA_OBJ_SYM_LINK_TYPE
#define KDATA_OBJ_SYM_LINK_TYPE 0x550  /* ObSymbolicLinkObjectType (4 bytes) */
#endif

/* --- MmGlobalData (ordinal 102, 0 args) --- */
static void bridge_MmGlobalData(void)
{
    /* Titles rarely dereference this; give it a stable, mapped address that
     * reads back the address itself (self-referential placeholder). */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_MMGLOBAL) =
        XBOX_KERNEL_DATA_BASE + KDATA_MMGLOBAL;
    g_eax = XBOX_KERNEL_DATA_BASE + KDATA_MMGLOBAL;
}

/* --- KeInterruptTime (ordinal 120, 0 args) --- */
static void bridge_KeInterruptTime(void)
{
    /* Refresh the 100ns-since-boot counter before handing out the pointer. */
    ULONGLONG t = (ULONGLONG)GetTickCount64() * 10000ull;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_INTERRUPT_TIME + 0) = (uint32_t)t;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_INTERRUPT_TIME + 4) = (uint32_t)(t >> 32);
    g_eax = XBOX_KERNEL_DATA_BASE + KDATA_INTERRUPT_TIME;
}

/* --- KeSystemTime (ordinal 154, 0 args) --- */
static void bridge_KeSystemTime(void)
{
    /* Refresh the 100ns-since-1601 clock before handing out the pointer. */
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_SYSTEM_TIME + 0) = ft.dwLowDateTime;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_SYSTEM_TIME + 4) = ft.dwHighDateTime;
    g_eax = XBOX_KERNEL_DATA_BASE + KDATA_SYSTEM_TIME;
}

/* --- ObDirectoryObjectType (ordinal 240, 0 args) --- */
static void bridge_ObDirectoryObjectType(void)
{
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_OBJ_DIR_TYPE) =
        XBOX_KERNEL_DATA_BASE + KDATA_OBJ_DIR_TYPE;
    g_eax = XBOX_KERNEL_DATA_BASE + KDATA_OBJ_DIR_TYPE;
}

/* --- ObpObjectHandleTable (ordinal 245, 0 args) --- */
static void bridge_ObpObjectHandleTable(void)
{
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_OBJ_HANDLE_TABLE) =
        XBOX_KERNEL_DATA_BASE + KDATA_OBJ_HANDLE_TABLE;
    g_eax = XBOX_KERNEL_DATA_BASE + KDATA_OBJ_HANDLE_TABLE;
}

/* --- ObSymbolicLinkObjectType (ordinal 249, 0 args) --- */
static void bridge_ObSymbolicLinkObjectType(void)
{
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_OBJ_SYM_LINK_TYPE) =
        XBOX_KERNEL_DATA_BASE + KDATA_OBJ_SYM_LINK_TYPE;
    g_eax = XBOX_KERNEL_DATA_BASE + KDATA_OBJ_SYM_LINK_TYPE;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Dbg (6, 7, 8, 10, 11)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- DbgBreakPointWithStatus (ordinal 6, 1 arg = 4 bytes) --- */
static void bridge_DbgBreakPointWithStatus(void)
{
    (void)STACK_ARG(0);
    DebugBreak();
    g_eax = 0;
}

/* --- DbgLoadImageSymbols (ordinal 7, 3 args = 12 bytes) --- */
static void bridge_DbgLoadImageSymbols(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    g_eax = 0;
}

/* --- DbgPrint (ordinal 8, __cdecl varargs, 0 fixed args) --- */
static void bridge_DbgPrint(void)
{
    /* First vararg is a guest format string. Log it verbatim (never treat it
     * as a format, it is guest-owned); caller cleans the stack (__cdecl). */
    const char *fmt = (const char *)XBOX_TO_NATIVE(STACK_ARG(0));
    if (fmt && KERNEL_LOG_ON()) {
        size_t i, n = 0;
        for (i = 0; i < 512 && fmt[i]; i++)
            n++;
        fwrite(fmt, 1, n, stderr);
        fputc('\n', stderr);
        fflush(stderr);
    }
    g_eax = 0;
}

/* --- DbgPrompt (ordinal 10, 2 args = 8 bytes) --- */
static void bridge_DbgPrompt(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    g_eax = 0;
}

/* --- DbgUnLoadImageSymbols (ordinal 11, 3 args = 12 bytes) --- */
static void bridge_DbgUnLoadImageSymbols(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    g_eax = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Ex (12, 13, 18, 19, 20, 21, 25, 26, 27, 28)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- ExAcquireReadWriteLockExclusive (ordinal 12, 1 arg = 4 bytes) --- */
static void bridge_ExAcquireReadWriteLockExclusive(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

/* --- ExAcquireReadWriteLockShared (ordinal 13, 1 arg = 4 bytes) --- */
static void bridge_ExAcquireReadWriteLockShared(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

/* --- ExInitializeReadWriteLock (ordinal 18, 1 arg = 4 bytes) --- */
static void bridge_ExInitializeReadWriteLock(void)
{
    uint32_t lock_va = STACK_ARG(0);
    if (lock_va)
        memset(XBOX_TO_NATIVE(lock_va), 0, 24);
    g_eax = 0;
}

/* --- ExInterlockedAddLargeInteger (ordinal 19, 3 args = 12 bytes)
 * LARGE_INTEGER ExInterlockedAddLargeInteger(PLARGE_INTEGER Addend,
 *                                            LARGE_INTEGER Increment);
 * Addend (4) + Increment.Low (4) + Increment.High (4). Returns the PREVIOUS
 * value of Addend in edx:eax. */
static void bridge_ExInterlockedAddLargeInteger(void)
{
    uint32_t addend_va = STACK_ARG(0);
    LONGLONG increment = ((LONGLONG)(uint32_t)STACK_ARG(2) << 32)
                       | (LONGLONG)(uint32_t)STACK_ARG(1);
    volatile LONG *p = (volatile LONG *)XBOX_TO_NATIVE(addend_va);
    LONGLONG old = 0;

    if (p) {
        if (((uintptr_t)p & 7) == 0) {
            volatile LONGLONG *qp = (volatile LONGLONG *)p;
            LONGLONG cmp = *qp;
            for (;;) {
                LONGLONG desired = cmp + increment;
                LONGLONG actual = InterlockedCompareExchange64(qp, desired, cmp);
                if (actual == cmp)
                    break;
                cmp = actual;
            }
            old = cmp;
        } else {
            /* Unaligned: single-threaded guest, plain RMW is good enough. */
            old = ((LONGLONG)(uint32_t)p[1] << 32) | (uint32_t)p[0];
            {
                LONGLONG newv = old + increment;
                p[0] = (LONG)(uint32_t)newv;
                p[1] = (LONG)(uint32_t)((uint64_t)newv >> 32);
            }
        }
    }

    g_eax = (uint32_t)old;
    g_edx = (uint32_t)((uint64_t)old >> 32);
}

/* --- ExInterlockedAddLargeStatistic (ordinal 20, 2 args = 8 bytes)
 * LARGE_INTEGER ExInterlockedAddLargeStatistic(PLARGE_INTEGER Addend,
 *                                              ULONG Increment);
 * Addend (4) + Increment (4). Atomic add; per task spec, returns 0. */
static void bridge_ExInterlockedAddLargeStatistic(void)
{
    uint32_t addend_va = STACK_ARG(0);
    LONG increment = (LONG)STACK_ARG(1);
    volatile LONG *p = (volatile LONG *)XBOX_TO_NATIVE(addend_va);

    if (p) {
        if (((uintptr_t)p & 7) == 0) {
            volatile LONGLONG *qp = (volatile LONGLONG *)p;
            LONGLONG cmp = *qp;
            for (;;) {
                LONGLONG desired = cmp + (LONGLONG)increment;
                LONGLONG actual = InterlockedCompareExchange64(qp, desired, cmp);
                if (actual == cmp)
                    break;
                cmp = actual;
            }
        } else {
            LONGLONG val = ((LONGLONG)(uint32_t)p[1] << 32) | (uint32_t)p[0];
            LONGLONG newv = val + (LONGLONG)increment;
            p[0] = (LONG)(uint32_t)newv;
            p[1] = (LONG)(uint32_t)((uint64_t)newv >> 32);
        }
    }
    g_eax = 0;
}

/* --- ExInterlockedCompareExchange64 (ordinal 21, 4 args = 16 bytes)
 * LONGLONG ExInterlockedCompareExchange64(PLONGLONG Destination,
 *                                         LONGLONG Exchange,
 *                                         PLONGLONG Comparand);
 * Arg layout on a 32-bit stack is Destination (4) + Exchange (8) +
 * Comparand.Low (4) -- the Comparand high dword is not passed, so the
 * comparison is against a zero-extended 32-bit value. Returns the old value
 * in edx:eax. */
static void bridge_ExInterlockedCompareExchange64(void)
{
    LONGLONG old = 0;
    volatile LONGLONG *qp;
    LONGLONG exchange;
    LONGLONG comparand;

    exchange  = ((LONGLONG)(uint32_t)STACK_ARG(2) << 32)
              | (LONGLONG)(uint32_t)STACK_ARG(1);
    comparand = (LONGLONG)(uint32_t)STACK_ARG(3);
    qp = (volatile LONGLONG *)XBOX_TO_NATIVE(STACK_ARG(0));

    if (qp)
        old = InterlockedCompareExchange64(qp, exchange, comparand);

    g_eax = (uint32_t)old;
    g_edx = (uint32_t)((uint64_t)old >> 32);
}

/* --- ExReadWriteRefurbInfo (ordinal 25, 3 args = 12 bytes) --- */
static void bridge_ExReadWriteRefurbInfo(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    g_eax = 0;
}

/* --- ExRaiseException (ordinal 26, 1 arg = 4 bytes) --- */
static void bridge_ExRaiseException(void) {bridge_raise_entry(1);}

/* --- ExRaiseStatus (ordinal 27, 1 arg = 4 bytes) --- */
static void bridge_ExRaiseStatus(void) {bridge_raise_entry(2);}

/* --- ExReleaseReadWriteLock (ordinal 28, 1 arg = 4 bytes) --- */
static void bridge_ExReleaseReadWriteLock(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * ExfInterlocked linked-list operations (32, 33, 34)
 *
 * The Xbox ABI keeps a spin-lock around these; with a single-threaded guest
 * the two-pointer fix-up after the atomic head swap is sufficient.
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- ExfInterlockedInsertHeadList (ordinal 32, 2 args = 8 bytes)
 * PLIST_ENTRY ExfInterlockedInsertHeadList(ListHead, Entry);
 * Returns the entry's old Flink (the previous first entry, or ListHead
 * itself for an empty list). */
static void bridge_ExfInterlockedInsertHeadList(void)
{
    uint32_t entry_va = STACK_ARG(0);
    uint32_t head_va  = STACK_ARG(1);
    volatile LONG *h_flink = (volatile LONG *)XBOX_TO_NATIVE(head_va);
    uint32_t old_flink = 0;

    if (h_flink) {
        /* Atomically seat the new head; remember what used to be first. */
        old_flink = (uint32_t)InterlockedExchange(h_flink, (LONG)entry_va);
        BRIDGE_MEM32(entry_va + 0) = old_flink;  /* Entry->Flink = old first */
        BRIDGE_MEM32(entry_va + 4) = head_va;    /* Entry->Blink = head       */
        if (old_flink)
            BRIDGE_MEM32(old_flink + 4) = entry_va; /* old first->Blink = Entry */
    }
    g_eax = old_flink;
}

/* --- ExfInterlockedInsertTailList (ordinal 33, 2 args = 8 bytes)
 * PLIST_ENTRY ExfInterlockedInsertTailList(ListHead, Entry);
 * Returns the previous tail. NT semantics return the old Blink (the entry
 * that was last, or ListHead for an empty list); note the task brief said
 * "old Flink" but the canonical Xbox/NT behaviour is the old Blink. */
static void bridge_ExfInterlockedInsertTailList(void)
{
    uint32_t entry_va = STACK_ARG(0);
    uint32_t head_va  = STACK_ARG(1);
    volatile LONG *h_blink = (volatile LONG *)XBOX_TO_NATIVE(head_va + 4);
    uint32_t old_blink = 0;

    if (h_blink) {
        /* Atomically seat the new tail; remember what used to be last. */
        old_blink = (uint32_t)InterlockedExchange(h_blink, (LONG)entry_va);
        BRIDGE_MEM32(entry_va + 0) = head_va;     /* Entry->Flink = head      */
        BRIDGE_MEM32(entry_va + 4) = old_blink;   /* Entry->Blink = old last  */
        if (old_blink)
            BRIDGE_MEM32(old_blink + 0) = entry_va; /* old last->Flink = Entry */
    }
    g_eax = old_blink;
}

/* --- ExfInterlockedRemoveHeadList (ordinal 34, 1 arg = 4 bytes)
 * PLIST_ENTRY ExfInterlockedRemoveHeadList(ListHead);
 * Returns the removed first entry, or 0 when the list is empty. */
static void bridge_ExfInterlockedRemoveHeadList(void)
{
    uint32_t head_va = STACK_ARG(0);
    volatile LONG *h_flink = (volatile LONG *)XBOX_TO_NATIVE(head_va);
    uint32_t removed = 0;

    if (h_flink) {
        for (;;) {
            uint32_t first = (uint32_t)*h_flink;
            if (first == head_va) {   /* empty list */
                removed = 0;
                break;
            }
            /* Pull Entry = first; Entry's Flink becomes the new head, but only
             * if the head still points at first (single-threaded: it does). */
            {
                uint32_t entry_flink = BRIDGE_MEM32(first + 0);
                if (InterlockedCompareExchange(h_flink,
                                              (LONG)entry_flink,
                                              (LONG)first) == (LONG)first) {
                    if (entry_flink)
                        BRIDGE_MEM32(entry_flink + 4) = head_va;
                    removed = first;
                    break;
                }
            }
        }
    }
    g_eax = removed;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Fsc (36)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- FscInvalidateIdleBlocks (ordinal 36, 0 args = 0 bytes) --- */
static void bridge_FscInvalidateIdleBlocks(void)
{
    g_eax = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Hal (43, 365, 366)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- HalEnableSystemInterrupt (ordinal 43, 2 args = 8 bytes) --- */
static void bridge_HalEnableSystemInterrupt(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    g_eax = 0;
}

/* --- HalEnableSecureTrayEject (ordinal 365, 0 args = 0 bytes) --- */
static void bridge_HalEnableSecureTrayEject(void)
{
    g_eax = 0;
}

/* --- HalWriteSMCScratchRegister (ordinal 366, 1 arg = 4 bytes) --- */
static void bridge_HalWriteSMCScratchRegister(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Interlocked (51-58) -- __fastcall: args travel in ecx/edx/eax, never on
 * the guest stack, so the arg-size entries are all 0 for these. The guest VA
 * in ecx/edx must go through XBOX_TO_NATIVE, exactly like the existing
 * bridge_ObfDereferenceObject.
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- InterlockedCompareExchange (ordinal 51)
 * LONG InterlockedCompareExchange(PLONG Destination, LONG Exchange,
 *                                 LONG Comparand);
 * fastcall: ecx=Destination, edx=Exchange, eax=Comparand-in/old-out. */
static void bridge_InterlockedCompareExchange(void)
{
    LONG *dst = (LONG *)XBOX_TO_NATIVE(g_ecx);
    if (!dst) {
        g_eax = 0;
        return;
    }
    g_eax = (uint32_t)InterlockedCompareExchange(dst, (LONG)g_edx, (LONG)g_eax);
}

/* --- InterlockedDecrement (ordinal 52)
 * fastcall: ecx=Destination. */
static void bridge_InterlockedDecrement(void)
{
    LONG *dst = (LONG *)XBOX_TO_NATIVE(g_ecx);
    if (!dst) {
        g_eax = 0;
        return;
    }
    g_eax = (uint32_t)InterlockedDecrement(dst);
}

/* --- InterlockedIncrement (ordinal 53)
 * fastcall: ecx=Destination. */
static void bridge_InterlockedIncrement(void)
{
    LONG *dst = (LONG *)XBOX_TO_NATIVE(g_ecx);
    if (!dst) {
        g_eax = 0;
        return;
    }
    g_eax = (uint32_t)InterlockedIncrement(dst);
}

/* --- InterlockedExchange (ordinal 54)
 * fastcall: ecx=Destination, edx=Value. */
static void bridge_InterlockedExchange(void)
{
    LONG *dst = (LONG *)XBOX_TO_NATIVE(g_ecx);
    if (!dst) {
        g_eax = 0;
        return;
    }
    g_eax = (uint32_t)InterlockedExchange(dst, (LONG)g_edx);
}

/* --- InterlockedExchangeAdd (ordinal 55)
 * fastcall: ecx=Destination, edx=Value. */
static void bridge_InterlockedExchangeAdd(void)
{
    LONG *dst = (LONG *)XBOX_TO_NATIVE(g_ecx);
    if (!dst) {
        g_eax = 0;
        return;
    }
    g_eax = (uint32_t)InterlockedExchangeAdd(dst, (LONG)g_edx);
}

/* --- InterlockedFlushSList (ordinal 56, fastcall) --- */
static void bridge_InterlockedFlushSList(void)
{
    (void)g_ecx;
    g_eax = 0;
}

/* --- InterlockedPopEntrySList (ordinal 57, fastcall) --- */
static void bridge_InterlockedPopEntrySList(void)
{
    (void)g_ecx;
    g_eax = 0;
}

/* --- InterlockedPushEntrySList (ordinal 58, fastcall) --- */
static void bridge_InterlockedPushEntrySList(void)
{
    (void)g_ecx;
    (void)g_edx;
    g_eax = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Io (59, 60, 63, 72, 75, 76, 77, 78, 80, 90, 91)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- IoAllocateIrp (ordinal 59, 2 args = 8 bytes)
 * PIRP IoAllocateIrp(CCHAR StackSize, BOOLEAN ChargeQuota);
 * Allocates an IRP header plus StackSize IO_STACK_LOCATIONs from the guest
 * heap. Pair with IoFreeIrp (ordinal 72), which frees through the same heap. */
static void bridge_IoAllocateIrp(void)
{
    g_eax = io_guest_address(xbox_IoAllocateIrp((CCHAR)STACK_ARG(0)));
}

/* --- IoBuildAsynchronousFsdRequest (ordinal 60, 7 args = 28 bytes) --- */
static void bridge_IoBuildAsynchronousFsdRequest(void)
{
    g_eax = io_guest_address(xbox_IoBuildAsynchronousFsdRequest(STACK_ARG(0),
        XBOX_TO_NATIVE(STACK_ARG(1)), XBOX_TO_NATIVE(STACK_ARG(2)), STACK_ARG(3),
        XBOX_TO_NATIVE(STACK_ARG(4)), XBOX_TO_NATIVE(STACK_ARG(5))));
}

/* --- IoCheckShareAccess (ordinal 63, 5 args = 20 bytes) --- */
static void bridge_IoCheckShareAccess(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    g_eax = 0;
}

/* --- IoFreeIrp (ordinal 72, 1 arg = 4 bytes) --- */
static void bridge_IoFreeIrp(void)
{
    xbox_IoFreeIrp(XBOX_TO_NATIVE(STACK_ARG(0)));
    g_eax = 0;
}

/* --- IoQueryFileInformation (ordinal 75, 5 args = 20 bytes) --- */
static void bridge_IoQueryFileInformation(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    g_eax = 0xC000000D;  /* STATUS_INVALID_PARAMETER */
}

/* --- IoQueryVolumeInformation (ordinal 76, 5 args = 20 bytes) --- */
static void bridge_IoQueryVolumeInformation(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    g_eax = 0xC000000D;  /* STATUS_INVALID_PARAMETER */
}

/* --- IoQueueThreadIrp (ordinal 77, 1 arg = 4 bytes) --- */
static void bridge_IoQueueThreadIrp(void)
{
    xbox_IoQueueThreadIrp(XBOX_TO_NATIVE(STACK_ARG(0)));
}

/* --- IoRemoveShareAccess (ordinal 78, 2 args = 8 bytes) --- */
static void bridge_IoRemoveShareAccess(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
}

/* --- IoSetShareAccess (ordinal 80, 5 args = 20 bytes) --- */
static void bridge_IoSetShareAccess(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
}

/* --- IoDismountVolume (ordinal 90, 1 arg = 4 bytes) --- */
static void bridge_IoDismountVolume(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

/* --- IoDismountVolumeByName (ordinal 91, 1 arg = 4 bytes) --- */
static void bridge_IoDismountVolumeByName(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Kd (88, 89)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- KdDebuggerEnabled (ordinal 88, 0 args = 0 bytes) --- */
static void bridge_KdDebuggerEnabled(void)
{
    g_eax = 0;  /* not connected */
}

/* --- KdDebuggerNotPresent (ordinal 89, 0 args = 0 bytes) --- */
static void bridge_KdDebuggerNotPresent(void)
{
    g_eax = 1;  /* true: no debugger present */
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Ke -- shadow-table object bridges (92-163)
 *
 * The KeInitialize* family creates a real Win32 object, records guest-VA ->
 * host-HANDLE in the shadow table, and zeroes/initialises the guest dispatcher
 * header. The KePulse/Release/Reset/Set family resolves back through the
 * shadow table (with XBOX_TO_NATIVE as a fallback for legacy callers).
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- KeInitializeEvent (ordinal 108, 3 args = 12 bytes) --- */
static void bridge_KeInitializeEvent(void)
{
    if (!ke_initialize_event(STACK_ARG(0), STACK_ARG(1), STACK_ARG(2))) {
        fprintf(stderr, "  [KERNEL] KeInitializeEvent: host event creation failed\n");
    }
    g_eax = 0;
}

/* --- KeInitializeMutant (ordinal 110, 2 args = 8 bytes) --- */
static void bridge_KeInitializeMutant(void)
{
    uint32_t guest_va = STACK_ARG(0);
    uint32_t initial_owner = STACK_ARG(1);

    HANDLE h = CreateMutexW(NULL, initial_owner ? TRUE : FALSE, NULL);
    if (!h) {
        fprintf(stderr, "  [KERNEL] KeInitializeMutant: CreateMutexW failed (error %u)\n",
                GetLastError());
        g_eax = 0;
        return;
    }

    ke_shadow_insert(guest_va, h);

    /* Mutant dispatcher header. SignalState semantics differ from events:
     * a mutant is SIGNALLED (owned count 0) when its SignalState is 0.
     * Win32 CreateMutex(initialOwner=TRUE) is owned, so SignalState=1. */
    BRIDGE_MEM8(guest_va + 0)  = 19;   /* DispatcherObjectType */
    BRIDGE_MEM8(guest_va + 1)  = 0;
    BRIDGE_MEM8(guest_va + 2)  = 16;
    BRIDGE_MEM8(guest_va + 3)  = 1;
    BRIDGE_MEM32(guest_va + 4) = initial_owner ? 1 : 0; /* SignalState */
    BRIDGE_MEM32(guest_va + 8) = 0;   /* WaitListHead */
    BRIDGE_MEM32(guest_va + 12) = 0;

    g_eax = 0;
}

/* --- KeInitializeSemaphore (ordinal 112, 3 args = 12 bytes) --- */
static void bridge_KeInitializeSemaphore(void)
{
    uint32_t guest_va = STACK_ARG(0);
    uint32_t count = STACK_ARG(1);
    uint32_t limit = STACK_ARG(2);

    HANDLE h = CreateSemaphoreW(NULL, (LONG)count, (LONG)limit, NULL);
    if (!h) {
        fprintf(stderr, "  [KERNEL] KeInitializeSemaphore: CreateSemaphoreW failed (error %u)\n",
                GetLastError());
        g_eax = 0;
        return;
    }

    ke_shadow_insert(guest_va, h);

    BRIDGE_MEM8(guest_va + 0)  = 18;   /* DispatcherObjectType */
    BRIDGE_MEM8(guest_va + 1)  = 0;
    BRIDGE_MEM8(guest_va + 2)  = 16;
    BRIDGE_MEM8(guest_va + 3)  = 1;
    BRIDGE_MEM32(guest_va + 4) = (LONG)count; /* SignalState = count */
    BRIDGE_MEM32(guest_va + 8) = 0;   /* WaitListHead */
    BRIDGE_MEM32(guest_va + 12) = 0;

    g_eax = 0;
}

/* --- KePulseEvent (ordinal 123, 3 args = 12 bytes) --- */
static void bridge_KePulseEvent(void)
{
    uint32_t guest_va = STACK_ARG(0);
    uint32_t increment = STACK_ARG(1);
    uint32_t wait = STACK_ARG(2);
    HANDLE h;

    (void)increment;
    (void)wait;

    {
        int type;
        HANDLE ge = ke_guest_event(guest_va, &type);
        if (ge) {
            ke_prepare_event_wait(guest_va, ge, type);
            g_eax = ke_change_event(guest_va, ge, 2);
            return;
        }
    }
    h = ke_shadow_lookup(guest_va);
    if (!h)
        h = XBOX_TO_NATIVE(guest_va);
    if (h)
        PulseEvent(h);

    g_eax = 0; /* previous state unknown */
}

/* --- KeReleaseMutant (ordinal 131, 4 args = 16 bytes) --- */
static void bridge_KeReleaseMutant(void)
{
    uint32_t guest_va = STACK_ARG(0);
    uint32_t increment = STACK_ARG(1);
    uint32_t wait = STACK_ARG(2);
    uint32_t abandoned = STACK_ARG(3);
    HANDLE h;

    (void)increment;
    (void)wait;
    (void)abandoned;

    h = ke_shadow_lookup(guest_va);
    if (!h)
        h = XBOX_TO_NATIVE(guest_va);
    if (h)
        ReleaseMutex(h);

    g_eax = 0;
}

/* --- KeReleaseSemaphore (ordinal 132, 4 args = 16 bytes) --- */
static void bridge_KeReleaseSemaphore(void)
{
    uint32_t guest_va = STACK_ARG(0);
    uint32_t increment = STACK_ARG(1);
    uint32_t adjustment = STACK_ARG(2);  /* LONG */
    uint32_t wait = STACK_ARG(3);
    HANDLE h;

    (void)increment;
    (void)wait;

    h = ke_shadow_lookup(guest_va);
    if (!h)
        h = XBOX_TO_NATIVE(guest_va);
    if (h)
        ReleaseSemaphore(h, (LONG)adjustment, NULL);

    g_eax = 0;
}

/* --- KeResetEvent (ordinal 138, 1 arg = 4 bytes) --- */
static void bridge_KeResetEvent(void)
{
    uint32_t guest_va = STACK_ARG(0);
    HANDLE h;

    {
        int type;
        HANDLE ge = ke_guest_event(guest_va, &type);
        if (ge) {
            ke_prepare_event_wait(guest_va, ge, type);
            g_eax = ke_change_event(guest_va, ge, 1);
            return;
        }
    }
    h = ke_shadow_lookup(guest_va);
    if (!h)
        h = XBOX_TO_NATIVE(guest_va);
    if (h)
        ResetEvent(h);

    g_eax = 0;
}

/* --- KeResumeThread (ordinal 140, 1 arg = 4 bytes) --- */
static void bridge_KeResumeThread(void)
{
    HANDLE hThread = bridge_resolve_handle(STACK_ARG(0));
    if (!hThread)
        hThread = XBOX_TO_NATIVE(STACK_ARG(0));

    if (hThread)
        g_eax = (uint32_t)ResumeThread(hThread);
    else
        g_eax = 0;
}

/* --- KeSuspendThread (ordinal 152, 1 arg = 4 bytes) --- */
static void bridge_KeSuspendThread(void)
{
    HANDLE hThread = bridge_resolve_handle(STACK_ARG(0));
    if (!hThread)
        hThread = XBOX_TO_NATIVE(STACK_ARG(0));

    if (hThread)
        g_eax = (uint32_t)SuspendThread(hThread);
    else
        g_eax = 0;
}

/* --- KeAlertResumeThread (ordinal 92, 1 arg = 4 bytes) --- */
static void bridge_KeAlertResumeThread(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

/* --- KeBoostPriorityThread (ordinal 94, 2 args = 8 bytes) --- */
static void bridge_KeBoostPriorityThread(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    g_eax = 0;
}

/* --- KeEnterCriticalRegion (ordinal 101, 0 args = 0 bytes) --- */
static void bridge_KeEnterCriticalRegion(void)
{
    BRIDGE_MEM32(ke_current_thread()+0x68)--;
}

/* --- KeInitializeApc (ordinal 105, 7 args = 28 bytes) --- */
static void bridge_KeInitializeApc(void)
{
    ke_initialize_apc(STACK_ARG(0),STACK_ARG(1),STACK_ARG(2),STACK_ARG(3),
                      STACK_ARG(4),STACK_ARG(5),STACK_ARG(6));
}

/* --- KeInitializeDeviceQueue (ordinal 106, 1 arg = 4 bytes) --- */
static void bridge_KeInitializeDeviceQueue(void)
{
    if (STACK_ARG(0))
        ke_initialize_device_queue(STACK_ARG(0));
    g_eax = 0;
}

/* --- KeInitializeQueue (ordinal 111, 2 args = 8 bytes) --- */
static void bridge_KeInitializeQueue(void)
{
    if(!ke_initialize_queue(STACK_ARG(0),STACK_ARG(1))) {fprintf(stderr,"[STOP] queue initialization failed\n");exit(5);}
}

/* --- KeInsertByKeyDeviceQueue (ordinal 114, 3 args = 12 bytes) --- */
static void bridge_KeInsertByKeyDeviceQueue(void)
{
    g_eax = ke_insert_device_queue(STACK_ARG(0), STACK_ARG(1), 1, STACK_ARG(2));
}

/* --- KeInsertDeviceQueue (ordinal 115, 2 args = 8 bytes) --- */
static void bridge_KeInsertDeviceQueue(void)
{
    g_eax = ke_insert_device_queue(STACK_ARG(0), STACK_ARG(1), 0, 0);
}

/* --- KeInsertHeadQueue (ordinal 116, 2 args = 8 bytes) --- */
static void bridge_KeInsertHeadQueue(void)
{
    g_eax=ke_insert_queue(STACK_ARG(0),STACK_ARG(1),1);
}

/* --- KeInsertQueue (ordinal 117, 2 args = 8 bytes) --- */
static void bridge_KeInsertQueue(void)
{
    g_eax=ke_insert_queue(STACK_ARG(0),STACK_ARG(1),0);
}

/* --- KeInsertQueueApc (ordinal 118, 4 args = 16 bytes) --- */
static void bridge_KeInsertQueueApc(void)
{
    g_eax=ke_insert_apc(STACK_ARG(0),STACK_ARG(1),STACK_ARG(2));
}

/* --- KeInsertQueueDpc (ordinal 119) ---
 * BOOLEAN KeInsertQueueDpc(PKDPC Dpc, PVOID SystemArgument1,
 *                          PVOID SystemArgument2)
 *
 * Enqueues a DPC for the timer thread to drain. See the queue above. */

/* --- KeIsExecutingDpc (ordinal 121, 0 args = 0 bytes) --- */
static void bridge_KeIsExecutingDpc(void)
{
    g_eax = BRIDGE_MEM32(XBOX_FS_BASE+0x58)!=0;
}

/* --- KeLeaveCriticalRegion (ordinal 122, 0 args = 0 bytes) --- */
static void bridge_KeLeaveCriticalRegion(void)
{
    uint32_t thread=ke_current_thread();
    if(++BRIDGE_MEM32(thread+0x68)==0 && BRIDGE_MEM32(thread+0x34)!=thread+0x34)
        BRIDGE_MEM8(thread+0x49)=1;
}

/* --- KeRaiseIrqlToSynchLevel (ordinal 130, 0 args = 0 bytes) --- */
static void bridge_KeRaiseIrqlToSynchLevel(void)
{
    g_eax = 0;  /* PASSIVE_LEVEL; IRQL is not modelled */
}

/* --- KeRemoveByKeyDeviceQueue (ordinal 133, 2 args = 8 bytes) --- */
static void bridge_KeRemoveByKeyDeviceQueue(void)
{
    g_eax = ke_remove_device_queue(STACK_ARG(0), 1, STACK_ARG(1));
}

/* --- KeRemoveDeviceQueue (ordinal 134, 1 arg = 4 bytes) --- */
static void bridge_KeRemoveDeviceQueue(void)
{
    g_eax = ke_remove_device_queue(STACK_ARG(0), 0, 0);
}

/* --- KeRemoveEntryDeviceQueue (ordinal 135, 2 args = 8 bytes) --- */
static void bridge_KeRemoveEntryDeviceQueue(void)
{
    (void)STACK_ARG(0);
    g_eax = ke_remove_device_queue_entry(STACK_ARG(1));
}

/* --- KeRemoveQueue (ordinal 136, 3 args = 12 bytes) --- */
static void bridge_KeRemoveQueue(void)
{
    g_eax=ke_remove_queue(STACK_ARG(0),(KPROCESSOR_MODE)STACK_ARG(1),XBOX_TO_NATIVE(STACK_ARG(2)));
}

/* --- KeRemoveQueueDpc (ordinal 137, 1 arg = 4 bytes) ---
 * BOOLEAN KeRemoveQueueDpc(PKDPC Dpc)
 *
 * Removes a queued DPC. See the queue above. */

/* --- KeRundownQueue (ordinal 141, 1 arg = 4 bytes) --- */
static void bridge_KeRundownQueue(void)
{
    g_eax=ke_rundown_queue(STACK_ARG(0));
}

/* --- KeSetEventBoostPriority (ordinal 146, 2 args = 8 bytes) --- */
static void bridge_KeSetEventBoostPriority(void)
{
    uint32_t guest_va = STACK_ARG(0);
    uint32_t increment = STACK_ARG(1);
    HANDLE h;

    (void)increment;

    h = ke_shadow_lookup(guest_va);
    if (!h)
        h = XBOX_TO_NATIVE(guest_va);
    if (h)
        SetEvent(h);

    g_eax = 0;
}

/* --- KeSetPriorityProcess (ordinal 147, 2 args = 8 bytes) --- */
static void bridge_KeSetPriorityProcess(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    g_eax = 0;
}

/* --- KeSetPriorityThread (ordinal 148, 2 args = 8 bytes) --- */
static void bridge_KeSetPriorityThread(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    g_eax = 0;
}

/* --- KeTestAlertThread (ordinal 155, 1 arg = 4 bytes) --- */
static void bridge_KeTestAlertThread(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

/* --- KiBugCheckData (ordinal 162, 0 args = 0 bytes) --- */
static void bridge_KiBugCheckData(void)
{
    g_eax = XBOX_KERNEL_DATA_BASE + 0x500;
}

/* --- KiUnlockDispatcherDatabase (ordinal 163, 1 arg = 4 bytes) --- */
static void bridge_KiUnlockDispatcherDatabase(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

/* --- KeGetCurrentIrql (ordinal 103, 0 args = 0 bytes)
 * Stack-based with 0 args (not the Kf* fastcall form). IRQL is unmounted, so
 * report PASSIVE_LEVEL. */
static void bridge_KeGetCurrentIrql(void)
{
    g_eax = 0;  /* PASSIVE_LEVEL */
}

/* --- KeGetCurrentThread (ordinal 104, 0 args = 0 bytes) --- */
static void bridge_KeGetCurrentThread(void)
{
    g_eax=ke_current_thread();
}

/* --- KeSetDisableBoostThread (ordinal 144, 2 args = 8 bytes) --- */
static void bridge_KeSetDisableBoostThread(void)
{
    uint32_t thread=STACK_ARG(0);BOOLEAN disable=(BOOLEAN)STACK_ARG(1);
    HANDLE handle=ke_shadow_lookup(thread);
    if(!handle || !SetThreadPriorityBoost(handle,disable!=0))recomp_cont_invalid();
    g_eax=BRIDGE_MEM8(thread+0x73);BRIDGE_MEM8(thread+0x73)=disable;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Mm (174, 374, 375, 376, 377, 378)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- MmIsAddressValid (ordinal 174, 1 arg = 4 bytes) --- */
static void bridge_MmIsAddressValid(void)
{
    (void)STACK_ARG(0);
    g_eax = 1;  /* assume every guest address is valid */
}

/* --- MmDbgAllocateMemory (ordinal 374, 2 args = 8 bytes)
 * PVOID MmDbgAllocateMemory(ULONG Size, ULONG AllocationTag);
 * Allocates zeroed guest memory. */
static void bridge_MmDbgAllocateMemory(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t tag = STACK_ARG(1);
    uint32_t va;

    (void)tag;
    if (!size) {
        g_eax = 0;
        return;
    }
    va = xbox_HeapAlloc(size, 16);
    if (va)
        memset((void *)((uintptr_t)va + g_xbox_mem_offset), 0, size);
    g_eax = va;
}

/* --- MmDbgFreeMemory (ordinal 375, 2 args = 8 bytes)
 * VOID MmDbgFreeMemory(PVOID BaseAddress, ULONG Size); */
static void bridge_MmDbgFreeMemory(void)
{
    (void)STACK_ARG(1);
    xbox_HeapFree(STACK_ARG(0));
    g_eax = 0;
}

/* --- MmDbgQueryAvailablePages (ordinal 376, 0 args = 0 bytes) --- */
static void bridge_MmDbgQueryAvailablePages(void)
{
    g_eax = 0x10000;  /* large made-up free-page count */
}

/* --- MmDbgReleaseAddress (ordinal 377, 2 args = 8 bytes) --- */
static void bridge_MmDbgReleaseAddress(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    g_eax = 0;
}

/* --- MmDbgWriteCheck (ordinal 378, 2 args = 8 bytes)
 * Passthrough: returns the first argument (the guest VA). */
static void bridge_MmDbgWriteCheck(void)
{
    (void)STACK_ARG(1);
    g_eax = STACK_ARG(0);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Nt (185, 191, 194, 201, 204, 206, 208, 209, 212, 213, 214, 216, 220, 223,
 *      227, 229, 230, 237)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* NtCancelTimer removes the original scheduled timer and pending APC. */
static void bridge_NtCancelTimer(void)
{
    uint32_t timer=0,output=STACK_ARG(1);
    NTSTATUS status=ke_reference_handle(bridge_resolve_handle(STACK_ARG(0)),
        XBOX_KERNEL_DATA_BASE+KDATA_TIMER_OBJ_TYPE,&timer);
    if(status>=0) {
        ke_cancel_timer_apc(timer);
        uint8_t previous=(uint8_t)ke_native_event_state(ke_shadow_lookup(timer));
        BRIDGE_MEM32(timer+4)=previous;ke_dereference_object(timer);
        if(output)BRIDGE_MEM8(output)=previous;
    }
    g_eax=(uint32_t)status;
}

/* --- NtCreateIoCompletion (ordinal 191, 4 args = 16 bytes) --- */
static void bridge_NtCreateIoCompletion(void)
{
    bridge_create_dispatcher_object(191,STACK_ARG(0),STACK_ARG(2),STACK_ARG(3),0);
}

/* NtCreateTimer creates the same waitable dispatcher object used by Ke timers. */
static void bridge_NtCreateTimer(void)
{
    bridge_create_dispatcher_object(194,STACK_ARG(0),STACK_ARG(1),STACK_ARG(2),0);
}

/* --- NtOpenDirectoryObject (ordinal 201, 2 args = 8 bytes) --- */
static void bridge_NtOpenDirectoryObject(void)
{
    XBOX_OBJECT_ATTRIBUTES oa;XBOX_ANSI_STRING name;HANDLE handle=NULL;
    NTSTATUS status=bridge_read_object_attributes(STACK_ARG(1),&oa,&name);
    if(!STACK_ARG(0))status=STATUS_INVALID_PARAMETER;
    if(status>=0)status=ke_open_named_object(&oa,XBOX_KERNEL_DATA_BASE+KDATA_OBJ_DIR_TYPE,&handle);
    if(STACK_ARG(0))bridge_write_handle(STACK_ARG(0),handle);
    g_eax=(uint32_t)status;
}

/* --- NtProtectVirtualMemory (ordinal 204, 4 args = 16 bytes)
 * NTSTATUS NtProtectVirtualMemory(PVOID* BaseAddress, PSIZE_T RegionSize,
 *                                 ULONG NewProtect, PULONG OldProtect);
 * Both pointers live in guest memory; translate and let VirtualProtect do the
 * work. (No xbox_NtProtectVirtualMemory() exists in kernel.h; done inline.) */
static void bridge_NtProtectVirtualMemory(void)
{
    uint32_t base_ptr = STACK_ARG(0);
    uint32_t size_ptr = STACK_ARG(1);
    uint32_t new_prot = STACK_ARG(2);
    uint32_t old_ptr  = STACK_ARG(3);
    uint32_t base_va = base_ptr ? BRIDGE_MEM32(base_ptr) : 0;
    uint32_t size    = size_ptr ? BRIDGE_MEM32(size_ptr) : 0;
    DWORD old = 0;
#if defined(_WIN32)
    int gpu_write=nv2a_pb_host_write_begin(XBOX_TO_NATIVE(base_va),size);
#endif
    BOOL protected=VirtualProtect(XBOX_TO_NATIVE(base_va), size, (DWORD)new_prot, &old);
#if defined(_WIN32)
    nv2a_pb_host_write_end(gpu_write);
#endif
    if (protected) {
        if (old_ptr)
            BRIDGE_MEM32(old_ptr) = (uint32_t)old;
        g_eax = 0;  /* STATUS_SUCCESS */
    } else {
        g_eax = 0xC0000005u;  /* STATUS_ACCESS_VIOLATION */
    }
}

/* --- NtQueueApcThread (ordinal 206, 5 args = 20 bytes) --- */
static void bridge_NtQueueApcThread(void)
{
    HANDLE handle=bridge_resolve_handle(STACK_ARG(0));
    uint32_t thread=ke_thread_for_handle(handle);
    if(!thread){g_eax=GetThreadId(handle)?0xc0000001u:(uint32_t)STATUS_INVALID_HANDLE;return;}
    uint32_t apc=xbox_HeapAlloc(0x28,16);
    if(!apc){g_eax=0xc0000017u;return;}
    ke_initialize_apc(apc,thread,KERNEL_POOL_FREE_APC,0,STACK_ARG(1),1,STACK_ARG(2));
    if(!ke_insert_apc(apc,STACK_ARG(3),STACK_ARG(4))) {xbox_HeapFree(apc);g_eax=0xc0000001u;return;}
    g_eax=0;
}

/* --- NtQueryDirectoryObject (ordinal 208, 7 args = 28 bytes) --- */
static void bridge_NtQueryDirectoryObject(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    (void)STACK_ARG(5);
    (void)STACK_ARG(6);
    g_eax = 0;
}

/* --- NtQueryEvent (ordinal 209, 4 args = 16 bytes) --- */
static void bridge_NtQueryEvent(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    g_eax = 0;
}

/* --- NtQueryIoCompletion (ordinal 212, 5 args = 20 bytes) --- */
static void bridge_NtQueryIoCompletion(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    g_eax = 0;
}

/* --- NtQueryMutant (ordinal 213, 5 args = 20 bytes) --- */
static void bridge_NtQueryMutant(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    g_eax = 0;
}

/* --- NtQuerySemaphore (ordinal 214, 5 args = 20 bytes) --- */
static void bridge_NtQuerySemaphore(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    g_eax = 0;
}

/* NtQueryTimer takes a handle and a TIMER_BASIC_INFORMATION output. */
static void bridge_NtQueryTimer(void)
{
    uint32_t timer=0,output=STACK_ARG(1);
    NTSTATUS status=ke_reference_handle(bridge_resolve_handle(STACK_ARG(0)),
        XBOX_KERNEL_DATA_BASE+KDATA_TIMER_OBJ_TYPE,&timer);
    if(status>=0) {
        uint64_t deadline=BRIDGE_MEM32(timer+0x10)|((uint64_t)BRIDGE_MEM32(timer+0x14)<<32);
        uint64_t remaining=deadline-xbox_KeQueryInterruptTime();
        uint8_t state=(uint8_t)ke_native_event_state(ke_shadow_lookup(timer));
        BRIDGE_MEM32(timer+4)=state;ke_dereference_object(timer);
        BRIDGE_MEM32(output)=(uint32_t)remaining;BRIDGE_MEM32(output+4)=(uint32_t)(remaining>>32);
        BRIDGE_MEM8(output+8)=state;
    }
    g_eax=(uint32_t)status;
}

/* --- NtReadFileScatter (ordinal 220, 8 args = 32 bytes) --- */
static void bridge_NtReadFileScatter(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    (void)STACK_ARG(5);
    (void)STACK_ARG(6);
    (void)STACK_ARG(7);
    g_eax = 0;
}

/* --- NtRemoveIoCompletion (ordinal 223, 5 args = 20 bytes) --- */
static void bridge_NtRemoveIoCompletion(void)
{
    uint32_t queue=0,key_out=0,context_out=0,status_out=0,timeout=0;
    RecompContFrame frame;
    const RecompContLocal locals[]={{"queue",&queue,sizeof queue},{"key_out",&key_out,sizeof key_out},
        {"context_out",&context_out,sizeof context_out},{"status_out",&status_out,sizeof status_out},
        {"timeout",&timeout,sizeof timeout}};
    int tracked=recomp_cont_active();
    uint64_t resume=tracked?kernel_native_cont_enter(&frame,KERNEL_CONT_COMPLETION,locals,5):0;
    if(!resume) {
        queue=ke_completion_for_handle(bridge_resolve_handle(STACK_ARG(0)));
        if(!queue){g_eax=(uint32_t)STATUS_INVALID_HANDLE;goto done;}
        key_out=STACK_ARG(1);context_out=STACK_ARG(2);status_out=STACK_ARG(3);timeout=STACK_ARG(4);
    }
    uint32_t entry=ke_remove_queue(queue,KernelMode,XBOX_TO_NATIVE(timeout));
    if(entry==0x102 || entry==0xc0){g_eax=entry;goto done;}
    uint32_t key,context,status,information;
    if(BRIDGE_MEM32(entry+8)) {
        key=BRIDGE_MEM32(entry+12);context=BRIDGE_MEM32(entry+16);
        status=BRIDGE_MEM32(entry+20);information=BRIDGE_MEM32(entry+24);xbox_HeapFree(entry);
    } else {
        uint32_t irp=entry-0x54;key=BRIDGE_MEM32(irp+0x3c);context=BRIDGE_MEM32(irp+0x2c);
        status=BRIDGE_MEM32(irp+0x10);information=BRIDGE_MEM32(irp+0x14);xbox_IoFreeIrp(XBOX_TO_NATIVE(irp));
    }
    BRIDGE_MEM32(key_out)=key;BRIDGE_MEM32(context_out)=context;
    BRIDGE_MEM32(status_out)=status;BRIDGE_MEM32(status_out+4)=information;g_eax=0;
done:
    if(tracked)recomp_cont_leave(&frame);
}

/* --- NtSetIoCompletion (ordinal 227, 5 args = 20 bytes) --- */
static void bridge_NtSetIoCompletion(void)
{
    uint32_t queue=ke_completion_for_handle(bridge_resolve_handle(STACK_ARG(0)));
    if(!queue){g_eax=(uint32_t)STATUS_INVALID_HANDLE;return;}
    g_eax=(uint32_t)xbox_IoSetIoCompletion(XBOX_TO_NATIVE(queue),XBOX_TO_NATIVE(STACK_ARG(1)),
        XBOX_TO_NATIVE(STACK_ARG(2)),(NTSTATUS)STACK_ARG(3),STACK_ARG(4));
}

/* --- NtSetTimerEx (ordinal 229, 8 args = 32 bytes) --- */
static void bridge_NtSetTimerEx(void)
{
    int32_t period=(int32_t)STACK_ARG(6);
    if(period<0){g_eax=0xc000000du;return;}
    int64_t due;memcpy(&due,XBOX_TO_NATIVE(STACK_ARG(1)),sizeof due);
    uint32_t timer=0,routine=STACK_ARG(2),output=STACK_ARG(7);
    NTSTATUS status=ke_reference_handle(bridge_resolve_handle(STACK_ARG(0)),
        XBOX_KERNEL_DATA_BASE+KDATA_TIMER_OBJ_TYPE,&timer);
    if(status<0){g_eax=(uint32_t)status;return;}
    ke_cancel_timer_apc(timer);
    uint8_t previous=(uint8_t)ke_native_event_state(ke_shadow_lookup(timer));
    BRIDGE_MEM32(timer+0x74)=(uint32_t)period;
    if(routine) {
        uint32_t thread=ke_current_thread();
        ke_initialize_apc(timer+0x28,thread,KERNEL_TIMER_APC,0,routine,STACK_ARG(4),STACK_ARG(3));
        ke_list_before(thread+0x124,timer+0x6c);BRIDGE_MEM8(timer+0x78)=1;
    }
    kernel_set_timer(timer,due,period,routine?timer+0x50:0);
    if(!routine)ke_dereference_object(timer);
    if(output)BRIDGE_MEM8(output)=previous;
    g_eax=STACK_ARG(5)?0x40000025u:0;
}

/* --- NtSignalAndWaitForSingleObjectEx (ordinal 230, 5 args = 20 bytes) --- */
static void bridge_NtSignalAndWaitForSingleObjectEx(void)
{
    g_eax=(uint32_t)xbox_NtSignalAndWaitForSingleObjectEx(
        bridge_resolve_handle(STACK_ARG(0)),bridge_resolve_handle(STACK_ARG(1)),
        (KPROCESSOR_MODE)STACK_ARG(2),(BOOLEAN)STACK_ARG(3),XBOX_TO_NATIVE(STACK_ARG(4)));
}

/* --- NtWriteFileGather (ordinal 237, 8 args = 32 bytes) --- */
static void bridge_NtWriteFileGather(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    (void)STACK_ARG(5);
    (void)STACK_ARG(6);
    (void)STACK_ARG(7);
    g_eax = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Ob (239, 241, 242, 243, 244, 248)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- ObCreateObject (ordinal 239, 7 args = 28 bytes) --- */
static void bridge_ObCreateObject(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    (void)STACK_ARG(5);
    (void)STACK_ARG(6);
    g_eax = 0;
}

/* --- ObInsertObject (ordinal 241, 6 args = 24 bytes) --- */
static void bridge_ObInsertObject(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    (void)STACK_ARG(5);
    g_eax = 0;
}

/* --- ObMakeTemporaryObject (ordinal 242, 1 arg = 4 bytes) --- */
static void bridge_ObMakeTemporaryObject(void)
{
    xbox_kernel_make_temporary(XBOX_TO_NATIVE(STACK_ARG(0)));
    g_eax=0;
}

/* --- ObOpenObjectByName (ordinal 243, 4 args = 16 bytes) --- */
static void bridge_ObOpenObjectByName(void)
{
    XBOX_OBJECT_ATTRIBUTES oa;XBOX_ANSI_STRING name;HANDLE handle=NULL;
    NTSTATUS status=bridge_read_object_attributes(STACK_ARG(0),&oa,&name);
    if(!STACK_ARG(3))status=STATUS_INVALID_PARAMETER;
    if(status>=0)status=ke_open_named_object(&oa,STACK_ARG(1),&handle);
    if(STACK_ARG(3))bridge_write_handle(STACK_ARG(3),handle);
    g_eax=(uint32_t)status;
}

/* --- ObOpenObjectByPointer (ordinal 244, 3 args = 12 bytes) --- */
static void bridge_ObOpenObjectByPointer(void)
{
    uint32_t object=STACK_ARG(0),output=STACK_ARG(2);HANDLE handle=NULL;
    NTSTATUS status=output?ke_reference_pointer(object,STACK_ARG(1)):STATUS_INVALID_PARAMETER;
    if(status>=0) {
        status=xbox_NtDuplicateObject(ke_shadow_lookup(object),&handle,0);
        ke_dereference_object(object);
    }
    if(output)bridge_write_handle(output,handle);
    g_eax=(uint32_t)status;
}

/* --- ObReferenceObjectByPointer (ordinal 248, object and type) --- */
static void bridge_ObReferenceObjectByPointer(void)
{
    g_eax=(uint32_t)ke_reference_pointer(STACK_ARG(0),STACK_ARG(1));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Ps (254, 256, 257)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- PsCreateSystemThread (ordinal 254, 7 args = 28 bytes)
 * NTSTATUS PsCreateSystemThread(PHANDLE ThreadHandle, ULONG KernelStackSize,
 *                               ULONG TlsDataSize, PULONG ThreadId,
 *                               PVOID StartContext1, PVOID StartContext2,
 *                               PXBOX_SYSTEM_ROUTINE StartRoutine);
 * Mirror of PsCreateSystemThreadEx (ordinal 255) with the ThreadExtraSize
 * argument dropped: same first-call-inline / worker-spawn behaviour.
 * Requires bridge_spawn_thread/bridge_run_thread_inline/g_thread_mode from
 * Section A of kernel_bridge.c. */
static void bridge_PsCreateSystemThread(void)
{
    uint32_t xbox_handle_ptr = STACK_ARG(0);
    uint32_t start_context1  = STACK_ARG(4);
    uint32_t start_context2  = STACK_ARG(5);
    uint32_t start_routine   = STACK_ARG(6);
    int is_first_call = (g_thread_mode == XBOX_THREAD_MODE_INLINE)
                        && (g_thread_call_count == 0);

    g_thread_call_count++;

    /* Fake handle until the worker path can write a real one. */
    if (xbox_handle_ptr)
        BRIDGE_MEM32(xbox_handle_ptr) = 0xBEEF0002;

    if (start_routine) {
        recomp_func_t fn = recomp_lookup(start_routine);
        if (!fn)
            fn = recomp_lookup_manual(start_routine);
        if (fn) {
            if (is_first_call) {
                g_esp -= 4; BRIDGE_MEM32(g_esp) = start_context2;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = start_context1;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
                fn();
                g_esp += 12;
            } else {
                const char *inline_workers = getenv("RECOMP_WORKERS");
                uint32_t stack_top;

                if (inline_workers && !strcmp(inline_workers, "inline")) {
                    bridge_run_thread_inline(fn, start_context1, start_context2);
                    g_eax = 0;
                    return;
                }
                stack_top = xbox_AllocThreadStack();
                if (!stack_top) {
                    bridge_run_thread_inline(fn, start_context1, start_context2);
                } else {
                    HANDLE th = bridge_spawn_thread(fn, start_context1,
                                                    start_context2, stack_top);
                    if (xbox_handle_ptr && th)
                        bridge_write_handle(xbox_handle_ptr, th);
                }
            }
        } else {
            fprintf(stderr, "  [KERNEL] PsCreateSystemThread: start routine 0x%08X not found in dispatch!\n",
                    start_routine);
        }
    }
    g_eax = 0;  /* STATUS_SUCCESS */
}

/* --- PsQueryStatistics (ordinal 256, 1 arg = 4 bytes)
 * Zeroes the caller's statistics structure. */
static void bridge_PsQueryStatistics(void)
{
    uint32_t stats_va = STACK_ARG(0);
    if (stats_va)
        memset(XBOX_TO_NATIVE(stats_va), 0, 0x80);
    g_eax = 0;
}

/* --- PsSetCreateThreadNotifyRoutine (ordinal 257, 2 args = 8 bytes) --- */
static void bridge_PsSetCreateThreadNotifyRoutine(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    g_eax = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Rtl (281, 282, 283, 285, 286, 303, 361, 362, 363, 364)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- RtlExtendedIntegerMultiply (ordinal 281, 3 args = 12 bytes)
 * LARGE_INTEGER RtlExtendedIntegerMultiply(LARGE_INTEGER Multiplicand,
 *                                          LONG Multiplier);
 * Multiplicand.Low (4) + Multiplicand.High (4) + Multiplier (4). C's int64
 * multiply wraps mod 2^64, which is exactly what the x86 imul pair produces
 * for the low 64 bits the API returns. */
static void bridge_RtlExtendedIntegerMultiply(void)
{
    int32_t mult = (int32_t)STACK_ARG(2);
    int64_t multi = ((int64_t)(int32_t)STACK_ARG(1) << 32)
                  | (uint32_t)STACK_ARG(0);
    int64_t result = multi * (int64_t)mult;

    g_eax = (uint32_t)result;
    g_edx = (uint32_t)((uint64_t)result >> 32);
}

/* --- RtlExtendedLargeIntegerDivide (ordinal 282, 4 args = 16 bytes)
 * LARGE_INTEGER RtlExtendedLargeIntegerDivide(LARGE_INTEGER Dividend,
 *                                             ULONG Divisor,
 *                                             PULONG Remainder);
 * Dividend.Low (4) + Dividend.High (4) + Divisor (4) + Remainder-ptr (4). */
static void bridge_RtlExtendedLargeIntegerDivide(void)
{
    uint64_t dividend = ((uint64_t)(uint32_t)STACK_ARG(1) << 32)
                      | (uint32_t)STACK_ARG(0);
    uint32_t divisor = STACK_ARG(2);
    uint32_t rem_ptr = STACK_ARG(3);
    uint32_t remainder;
    uint64_t quotient;

    if (divisor == 0) {
        /* Division by zero: report 0/*; Real hardware faults here. */
        if (rem_ptr)
            BRIDGE_MEM32(rem_ptr) = 0;
        g_eax = 0;
        g_edx = 0;
        return;
    }

    quotient  = dividend / divisor;
    remainder = (uint32_t)(dividend % divisor);

    if (rem_ptr)
        BRIDGE_MEM32(rem_ptr) = remainder;
    g_eax = (uint32_t)quotient;
    g_edx = (uint32_t)(quotient >> 32);
}

/* --- RtlExtendedMagicDivide (ordinal 283, 4 args = 16 bytes)
 * RtlExtendedMagicDivide(Dividend.Low, Dividend.High, Divisor, Remainder-ptr)
 * per the 16-byte stack layout. The precomputed magic multiplier is not
 * available, so a real division produces the identical quotient. */
static void bridge_RtlExtendedMagicDivide(void)
{
    uint64_t dividend = ((uint64_t)(uint32_t)STACK_ARG(1) << 32)
                      | (uint32_t)STACK_ARG(0);
    uint32_t divisor = STACK_ARG(2);
    uint32_t rem_ptr = STACK_ARG(3);
    uint32_t remainder;
    uint64_t quotient;

    if (divisor == 0) {
        if (rem_ptr)
            BRIDGE_MEM32(rem_ptr) = 0;
        g_eax = 0;
        g_edx = 0;
        return;
    }

    quotient  = dividend / divisor;
    remainder = (uint32_t)(dividend % divisor);

    if (rem_ptr)
        BRIDGE_MEM32(rem_ptr) = remainder;
    g_eax = (uint32_t)quotient;
    g_edx = (uint32_t)(quotient >> 32);
}

/* --- RtlFillMemoryUlong (ordinal 285, 3 args = 12 bytes)
 * VOID RtlFillMemoryUlong(PVOID Destination, ULONG Length, ULONG Pattern);
 * Length is multiples of 4 by contract. */
static void bridge_RtlFillMemoryUlong(void)
{
    volatile uint32_t *dst = (volatile uint32_t *)XBOX_TO_NATIVE(STACK_ARG(0));
    uint32_t length = STACK_ARG(1);
    uint32_t pattern = STACK_ARG(2);
    uint32_t i;

    if (!dst)
        return;
    for (i = 0; i < length / 4; i++)
        dst[i] = pattern;
    g_eax = 0;
}

/* --- RtlFreeAnsiString (ordinal 286, 1 arg = 4 bytes)
 * VOID RtlFreeAnsiString(PANSI_STRING AnsiString);
 * Frees AnsiString->Buffer (a guest-heap allocation) and clears the struct.
 * ANSI_STRING layout: +0 Length(USHORT), +2 MaximumLength(USHORT), +4 Buffer. */
static void bridge_RtlFreeAnsiString(void)
{
    uint32_t str_va = STACK_ARG(0);
    uint32_t buf;

    if (!str_va) {
        g_eax = 0;
        return;
    }
    buf = BRIDGE_MEM32(str_va + 4);
    if (buf)
        xbox_HeapFree(buf);
    BRIDGE_MEM16(str_va + 0) = 0;
    BRIDGE_MEM16(str_va + 2) = 0;
    BRIDGE_MEM32(str_va + 4) = 0;
    g_eax = 0;
}

/* --- RtlRaiseStatus (ordinal 303, 1 arg = 4 bytes) --- */
static void bridge_RtlRaiseStatus(void)
{
    bridge_ExRaiseStatus();
}

/* --- Rtl* printf family (ordinals 361-364) ---
 *
 * int RtlSnprintf(char*, size_t count, const char* fmt, ...)
 * int RtlSprintf(char*, const char* fmt, ...)
 * int RtlVsnprintf(char*, size_t count, const char* fmt, va_list)
 * int RtlVsprintf(char*, const char* fmt, va_list)
 *
 * All four are __cdecl on the console, so the stdcall table pops nothing
 * ("caller cleans", like DbgPrint) and the varargs stay readable on the guest
 * stack while the bridge runs. The V* variants take an x86 va_list, which is
 * just a guest pointer to where the argument slots begin.
 *
 * The x64 CRT's vsnprintf cannot consume an x86 va_list, so these bridges used
 * to copy the format string verbatim ("%d" came out as the literal text) and
 * read the wrong fixed arguments for the two non-counted variants: RtlSprintf's
 * "count" was the format pointer, and the vararg list started one slot early.
 * This walks the format itself and formats each conversion with the host CRT,
 * reading 32-bit argument slots from guest memory (64-bit args 8-aligned, as
 * on x86).
 */

/* Guest cursor over the variadic argument slots. */
static uint32_t bridge_va_next32(uint32_t *pos)
{
    uint32_t v = BRIDGE_MEM32(*pos);
    *pos += 4;
    return v;
}

static uint64_t bridge_va_next64(uint32_t *pos)
{
    uint32_t p = (*pos + 7) & ~7u;
    uint32_t lo = BRIDGE_MEM32(p);
    uint32_t hi = BRIDGE_MEM32(p + 4);
    *pos = p + 8;
    return (uint64_t)lo | ((uint64_t)hi << 32);
}

/* Format fmt, pulling varargs from guest memory at `pos`, into out (at most
 * cap-1 chars, NUL-terminated). Returns the number of characters written. */
static int bridge_format_printf(char *out, size_t cap, const char *fmt,
                                uint32_t pos)
{
    size_t used = 0;
    size_t i = 0;

    if (!out || cap == 0)
        return 0;

    while (fmt[i] && used + 1 < cap) {
        if (fmt[i] != '%') {
            out[used++] = fmt[i++];
            continue;
        }
        i++;
        if (fmt[i] == '%') {
            out[used++] = '%';
            i++;
            continue;
        }

        {
            char flags[8];
            int  nflags = 0;
            char conv[32];
            char *cp = conv;
            int  width = 0, have_width = 0, width_star = 0;
            int  prec  = 0, have_prec  = 0, prec_star  = 0;
            char len = 0;          /* 0 none, 1 h, 2 hh, 3 l, 4 ll */
            char spec;
            char tmp[512];
            size_t tlen;

            while (fmt[i] && strchr("-+ 0#", fmt[i]) && nflags < 7)
                flags[nflags++] = fmt[i++];
            flags[nflags] = 0;

            if (fmt[i] == '*') { width_star = 1; have_width = 1; i++; }
            else if (fmt[i] >= '0' && fmt[i] <= '9') {
                have_width = 1;
                while (fmt[i] >= '0' && fmt[i] <= '9') {
                    width = width * 10 + (fmt[i] - '0');
                    i++;
                }
            }

            if (fmt[i] == '.') {
                i++;
                have_prec = 1;
                if (fmt[i] == '*') { prec_star = 1; i++; }
                else while (fmt[i] >= '0' && fmt[i] <= '9') {
                    prec = prec * 10 + (fmt[i] - '0');
                    i++;
                }
            }

            if (fmt[i] == 'h') { len = 1; i++; if (fmt[i] == 'h') { len = 2; i++; } }
            else if (fmt[i] == 'l') { len = 3; i++; if (fmt[i] == 'l') { len = 4; i++; } }
            else if (fmt[i] == 'q') { len = 4; i++; }
            else if (fmt[i] == 'I' && fmt[i+1] == '6' && fmt[i+2] == '4') {
                len = 4; i += 3;
            }

            spec = fmt[i];
            if (!spec)
                break;
            i++;

            *cp++ = '%';
            if (nflags) { memcpy(cp, flags, (size_t)nflags); cp += nflags; }

            if (width_star) {
                int w = (int32_t)bridge_va_next32(&pos);
                if (w < 0) { *cp++ = '-'; w = -w; }
                cp += sprintf(cp, "%d", w);
            } else if (have_width) {
                cp += sprintf(cp, "%d", width);
            }
            if (have_prec) {
                *cp++ = '.';
                if (prec_star) {
                    int p = (int32_t)bridge_va_next32(&pos);
                    if (p < 0) p = 0;
                    cp += sprintf(cp, "%d", p);
                } else {
                    cp += sprintf(cp, "%d", prec);
                }
            }
            /* Single 'l' is a 32-bit long on the console; the host long is 32
             * bits too, so 'l' is passed through for the d/i/u/o/x/X paths. */
            if (len == 2) { *cp++ = 'h'; *cp++ = 'h'; }
            else if (len == 1) { *cp++ = 'h'; }
            else if (len == 4) { *cp++ = 'l'; *cp++ = 'l'; }

            switch (spec) {
            case 'd': case 'i': {
                if (len == 4) {
                    long long v = (long long)bridge_va_next64(&pos);
                    *cp++ = 'd'; *cp = 0;
                    snprintf(tmp, sizeof tmp, conv, v);
                } else {
                    int v = (int)(int32_t)bridge_va_next32(&pos);
                    *cp++ = 'd'; *cp = 0;
                    snprintf(tmp, sizeof tmp, conv, v);
                }
                break;
            }
            case 'u': {
                if (len == 4) {
                    unsigned long long v = bridge_va_next64(&pos);
                    *cp++ = 'u'; *cp = 0;
                    snprintf(tmp, sizeof tmp, conv, v);
                } else {
                    unsigned int v = bridge_va_next32(&pos);
                    *cp++ = 'u'; *cp = 0;
                    snprintf(tmp, sizeof tmp, conv, v);
                }
                break;
            }
            case 'o': case 'x': case 'X': {
                if (len == 4) {
                    unsigned long long v = bridge_va_next64(&pos);
                    *cp++ = spec; *cp = 0;
                    snprintf(tmp, sizeof tmp, conv, v);
                } else {
                    unsigned int v = bridge_va_next32(&pos);
                    *cp++ = spec; *cp = 0;
                    snprintf(tmp, sizeof tmp, conv, v);
                }
                break;
            }
            case 'c': {
                int v = (int)bridge_va_next32(&pos);
                *cp++ = 'c'; *cp = 0;
                snprintf(tmp, sizeof tmp, conv, v);
                break;
            }
            case 's': {
                uint32_t sva = bridge_va_next32(&pos);
                const char *gs = sva ? (const char *)XBOX_TO_NATIVE(sva)
                                     : "(null)";
                *cp++ = 's'; *cp = 0;
                snprintf(tmp, sizeof tmp, conv, gs);
                break;
            }
            case 'p': {
                /* 32-bit pointer as padded lower-case hex, like the x86 CRT */
                unsigned int v = bridge_va_next32(&pos);
                const char *pfx = strchr(flags, '#') ? "%#08x" : "%08x";
                snprintf(tmp, sizeof tmp, pfx, v);
                break;
            }
            case 'n': {
                uint32_t tva = bridge_va_next32(&pos);
                if (tva)
                    BRIDGE_MEM32(tva) = (uint32_t)used;   /* int written */
                tlen = 0;
                break;
            }
            case 'f': case 'e': case 'g': case 'E': case 'G': {
                double v = (double)bridge_va_next64(&pos);
                *cp++ = spec; *cp = 0;
                snprintf(tmp, sizeof tmp, conv, v);
                break;
            }
            default:
                /* Unsupported conversion: keep '%' and the specifier */
                out[used++] = '%';
                out[used++] = spec;
                continue;
            }

            tlen = (spec == 'n') ? 0 : strlen(tmp);
            {
                size_t k;
                for (k = 0; k < tlen && used + 1 < cap; k++)
                    out[used++] = tmp[k];
            }
        }
    }

    if (used + 1 >= cap)
        used = cap - 1;
    out[used] = 0;
    return (int)used;
}

/* --- RtlSnprintf (ordinal 361, __cdecl, caller cleans) --- */
static void bridge_RtlSnprintf(void)
{
    char     *dst  = (char *)XBOX_TO_NATIVE(STACK_ARG(0));
    uint32_t  count = STACK_ARG(1);
    const char *fmt = (const char *)XBOX_TO_NATIVE(STACK_ARG(2));

    if (!dst || !fmt) {
        g_eax = 0;
        return;
    }
    /* Varargs start after buffer/count/format on the guest stack. */
    g_eax = bridge_format_printf(dst, count, fmt, g_esp + 12);
}

/* --- RtlSprintf (ordinal 362, __cdecl, caller cleans) --- */
static void bridge_RtlSprintf(void)
{
    char     *dst  = (char *)XBOX_TO_NATIVE(STACK_ARG(0));
    const char *fmt = (const char *)XBOX_TO_NATIVE(STACK_ARG(1));

    if (!dst || !fmt) {
        g_eax = 0;
        return;
    }
    /* Unbounded like the CRT's vsprintf; a wide cap prevents a wild write. */
    g_eax = bridge_format_printf(dst, 65536, fmt, g_esp + 8);
}

/* --- RtlVsnprintf (ordinal 363, __cdecl, caller cleans) --- */
static void bridge_RtlVsnprintf(void)
{
    char     *dst  = (char *)XBOX_TO_NATIVE(STACK_ARG(0));
    uint32_t  count = STACK_ARG(1);
    const char *fmt = (const char *)XBOX_TO_NATIVE(STACK_ARG(2));
    uint32_t  list = STACK_ARG(3);   /* x86 va_list = guest pointer to args */

    if (!dst || !fmt || !list) {
        g_eax = 0;
        return;
    }
    g_eax = bridge_format_printf(dst, count, fmt, list);
}

/* --- RtlVsprintf (ordinal 364, __cdecl, caller cleans) --- */
static void bridge_RtlVsprintf(void)
{
    char     *dst  = (char *)XBOX_TO_NATIVE(STACK_ARG(0));
    const char *fmt = (const char *)XBOX_TO_NATIVE(STACK_ARG(1));
    uint32_t  list = STACK_ARG(2);   /* x86 va_list = guest pointer to args */

    if (!dst || !fmt || !list) {
        g_eax = 0;
        return;
    }
    g_eax = bridge_format_printf(dst, 65536, fmt, list);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Port I/O (329, 330, 331, 332)
 *
 * The port argument is an I/O window the title mapped with MmMapIoSpace, so
 * it is an ordinary guest VA in the memory model. The READ variants copy the
 * requested element count from the mapped window into the caller's buffer;
 * the WRITE variants are no-ops (mirroring the existing 333/334 bridges).
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- READ_PORT_BUFFER_UCHAR (ordinal 329, 3 args = 12 bytes) --- */
static void bridge_READ_PORT_BUFFER_UCHAR(void)
{
    void *src = XBOX_TO_NATIVE(STACK_ARG(0));
    void *dst = XBOX_TO_NATIVE(STACK_ARG(1));
    uint32_t count = STACK_ARG(2);

    if (src && dst)
        memcpy(dst, src, count);
    g_eax = 0;
}

/* --- READ_PORT_BUFFER_USHORT (ordinal 330, 3 args = 12 bytes) --- */
static void bridge_READ_PORT_BUFFER_USHORT(void)
{
    void *src = XBOX_TO_NATIVE(STACK_ARG(0));
    void *dst = XBOX_TO_NATIVE(STACK_ARG(1));
    uint32_t count = STACK_ARG(2);

    if (src && dst)
        memcpy(dst, src, count * sizeof(uint16_t));
    g_eax = 0;
}

/* --- READ_PORT_BUFFER_ULONG (ordinal 331, 3 args = 12 bytes) --- */
static void bridge_READ_PORT_BUFFER_ULONG(void)
{
    void *src = XBOX_TO_NATIVE(STACK_ARG(0));
    void *dst = XBOX_TO_NATIVE(STACK_ARG(1));
    uint32_t count = STACK_ARG(2);

    if (src && dst)
        memcpy(dst, src, count * sizeof(uint32_t));
    g_eax = 0;
}

/* --- WRITE_PORT_BUFFER_UCHAR (ordinal 332, 3 args = 12 bytes) --- */
static void bridge_WRITE_PORT_BUFFER_UCHAR(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    g_eax = 0;
}

static int stdcall_args_for_ordinal(ULONG ordinal)
{
    switch (ordinal) {
    /* ── Display / AV ── */
    case   1: return  0;  /* AvGetSavedDataAddress (void) */
    case   2: return 16;  /* AvSendTVEncoderOption (4) */
    case   3: return 24;  /* AvSetDisplayMode (6) */
    case   4: return  4;  /* AvSetSavedDataAddress (1) */
    case   5: return  0;  /* DbgBreakPoint (void) */
    case   8: return  0;  /* DbgPrint - __cdecl varargs, caller cleans */
    case   9: return  8;  /* HalReadSMCTrayState (2) */
    case  14: return  4;  /* ExAllocatePool (1) */
    case  15: return  8;  /* ExAllocatePoolWithTag (2) */
    case  17: return  4;  /* ExFreePool (1) */
    case  23: return  4;  /* ExQueryPoolBlockSize (1) */
    case  24: return 20;  /* ExQueryNonVolatileSetting (5) */
    case  29: return 16;  /* ExSaveNonVolatileSetting (4) */
    case  35: return  0;  /* FscGetCacheSize (void) */
    case  37: return  4;  /* FscSetCacheSize (1) */
    case  38: return  4;  /* HalClearSoftwareInterrupt (1) */
    case  39: return  8;  /* HalDisableSystemInterrupt (2) */
    case  42: return  0;  /* HalDiskSerialNumber - data export */
    case  44: return  8;  /* HalGetInterruptVector (2) */
    case  47: return  8;  /* HalRegisterShutdownNotification (2) */
    case  46: return 24;  /* HalReadWritePCISpace (6) */
    case  45: return 16;  /* HalReadSMBusValue (4) */
    case  50: return 16;  /* HalWriteSMBusValue (4) */
    case  48: return  4;  /* HalRequestSoftwareInterrupt (1) */
    case  49: return  4;  /* HalReturnToFirmware (1) */
    case  61: return 36;  /* IoBuildDeviceIoControlRequest (9) */
    case  62: return 28;  /* IoBuildSynchronousFsdRequest (7) */
    case  65: return 24;  /* IoCreateDevice (6) */
    case  66: return 40;  /* IoCreateFile (10) */
    case  67: return  8;  /* IoCreateSymbolicLink (2) */
    case  68: return  4;  /* IoDeleteDevice (1) */
    case  69: return  4;  /* IoDeleteSymbolicLink (1) */
    case  73: return 12;  /* IoInitializeIrp (3) */
    case  74: return  8;  /* IoInvalidDeviceRequest (2) */
    case  79: return 20;  /* IoSetIoCompletion (5) */
    case  81: return  4;  /* IoStartNextPacket (1) */
    case  82: return  8;  /* IoStartNextPacketByKey (2) */
    case  83: return 12;  /* IoStartPacket (3) */
    case  84: return 32;  /* IoSynchronousDeviceIoControlRequest (8) */
    case  85: return 20;  /* IoSynchronousFsdRequest (5) */
    case  86: return  0;  /* IofCallDriver (fastcall: args in ecx/edx) */
    case  87: return  0;  /* IofCompleteRequest (fastcall: args in ecx/edx) */
    /* Missing this entry cost the Xbox Dashboard its whole boot. Ordinal 91 has
     * no bridge, so the generic stub ran -- and with no arg count it left the
     * one pushed argument on the guest stack. The caller (sub_00032859) then
     * ran `pop edi; pop esi; pop ebx` four bytes low and came back with its
     * registers rotated, which destroyed the XApp `this` pointer two frames up.
     * Its scene manager was never created, its scene never loaded, and it
     * returned to firmware -- reported as nothing more than "returning 0". */
    case  90: return  4;  /* IoDismountVolume (1) */
    case  91: return  4;  /* IoDismountVolumeByName (1) */
    case  93: return  8;  /* KeAlertThread (2) */
    case  95: return  4;  /* KeBugCheck (1) */
    case  96: return 20;  /* KeBugCheckEx (5) */
    case  97: return  4;  /* KeCancelTimer (1) */
    case  98: return  4;  /* KeConnectInterrupt (1) */
    case  99: return 12;  /* KeDelayExecutionThread (3) */
    case 100: return  4;  /* KeDisconnectInterrupt (1) */
    case 107: return 12;  /* KeInitializeDpc (3) */
    case 109: return 28;  /* KeInitializeInterrupt (7) */
    case 113: return  8;  /* KeInitializeTimerEx (2) */
    case 119: return 12;  /* KeInsertQueueDpc (3) */
    case 124: return  4;  /* KeQueryBasePriorityThread (1) */
    case 125: return  0;  /* KeQueryInterruptTime (void) */
    case 126: return  0;  /* KeQueryPerformanceCounter (void) */
    case 127: return  0;  /* KeQueryPerformanceFrequency (void) */
    case 128: return  4;  /* KeQuerySystemTime (1) */
    case 129: return  0;  /* KeRaiseIrqlToDpcLevel (void) */
    case 137: return  4;  /* KeRemoveQueueDpc (1) */
    case 139: return  4;  /* KeRestoreFloatingPointState (1) */
    case 142: return  4;  /* KeSaveFloatingPointState (1) */
    case 143: return  8;  /* KeSetBasePriorityThread (2) */
    case 144: return  8;  /* KeSetDisableBoostThread (2) */
    case 145: return 12;  /* KeSetEvent (3) */
    case 149: return 16;  /* KeSetTimer (Timer+DueTime[8]+Dpc) */
    case 150: return 20;  /* KeSetTimerEx (Timer+DueTime[8]+Period+Dpc) */
    case 151: return  4;  /* KeStallExecutionProcessor (1) */
    case 153: return 12;  /* KeSynchronizeExecution (3) */
    case 158: return 32;  /* KeWaitForMultipleObjects (8) */
    case 159: return 20;  /* KeWaitForSingleObject (5) */
    case 160: return  0;  /* KfRaiseIrql (fastcall: arg in ecx) */
    case 161: return  0;  /* KfLowerIrql (fastcall: arg in ecx) */
    case 165: return  4;  /* MmAllocateContiguousMemory (1) */
    case 166: return 20;  /* MmAllocateContiguousMemoryEx (5) */
    case 167: return  8;  /* MmAllocateSystemMemory (2) */
    case 168: return  8;  /* MmClaimGpuInstanceMemory (2) */
    case 169: return  8;  /* MmCreateKernelStack (2) */
    case 170: return  8;  /* MmDeleteKernelStack (2) */
    case 171: return  4;  /* MmFreeContiguousMemory (1) */
    case 172: return  8;  /* MmFreeSystemMemory (2) */
    case 173: return  4;  /* MmGetPhysicalAddress (1) */
    case 175: return 12;  /* MmLockUnlockBufferPages (3) */
    case 176: return  8;  /* MmLockUnlockPhysicalPage (2) */
    case 177: return 12;  /* MmMapIoSpace (3) */
    case 178: return 12;  /* MmPersistContiguousMemory (3) */
    case 179: return  4;  /* MmQueryAddressProtect (1) */
    case 180: return  4;  /* MmQueryAllocationSize (1) */
    case 181: return  4;  /* MmQueryStatistics (1) */
    case 182: return 12;  /* MmSetAddressProtect (3) */
    case 183: return  8;  /* MmUnmapIoSpace (2) */
    case 184: return 20;  /* NtAllocateVirtualMemory (5) */
    case 185: return  8;  /* NtCancelTimer (2) */
    case 186: return  4;  /* NtClearEvent (1) */
    case 187: return  4;  /* NtClose (1) */
    case 188: return  8;  /* NtCreateDirectoryObject (2) */
    case 189: return 16;  /* NtCreateEvent (4) */
    case 190: return 36;  /* NtCreateFile (9) */
    case 191: return 16;  /* NtCreateIoCompletion (4) */
    case 192: return 12;  /* NtCreateMutant (3) */
    case 193: return 16;  /* NtCreateSemaphore (4) */
    case 194: return 12;  /* NtCreateTimer (3) */
    case 195: return  4;  /* NtDeleteFile (1) */
    case 196: return 40;  /* NtDeviceIoControlFile (10) */
    case 197: return 12;  /* NtDuplicateObject (3) */
    case 198: return  8;  /* NtFlushBuffersFile (2) */
    case 199: return 12;  /* NtFreeVirtualMemory (3) */
    case 200: return 40;  /* NtFsControlFile (10) */
    case 202: return 24;  /* NtOpenFile (6) */
    case 203: return  8;  /* NtOpenSymbolicLinkObject (2) */
    case 204: return 16;  /* NtProtectVirtualMemory (4) */
    case 205: return  8;  /* NtPulseEvent (2) */
    case 206: return 20;  /* NtQueueApcThread (5) */
    case 207: return 40;  /* NtQueryDirectoryFile (10) */
    case 210: return  8;  /* NtQueryFullAttributesFile (2) */
    case 211: return 20;  /* NtQueryInformationFile (5) */
    case 215: return 12;  /* NtQuerySymbolicLinkObject (3) */
    case 216: return  8;  /* NtQueryTimer (2) */
    case 217: return 8;   /* NtQueryVirtualMemory (2) -- Xbox takes
                             BaseAddress and Info only, not NT's four */
    case 218: return 20;  /* NtQueryVolumeInformationFile (5) */
    case 219: return 32;  /* NtReadFile (8) */
    case 220: return 32;  /* NtReadFileScatter (8) */
    case 221: return  8;  /* NtReleaseMutant (2) */
    case 222: return 12;  /* NtReleaseSemaphore (3) */
    case 223: return 20;  /* NtRemoveIoCompletion (5) */
    case 224: return  8;  /* NtResumeThread (2) */
    case 225: return  8;  /* NtSetEvent (2) */
    case 226: return 20;  /* NtSetInformationFile (5) */
    case 227: return 20;  /* NtSetIoCompletion (5) */
    case 228: return  8;  /* NtSetSystemTime (2) */
    case 229: return 32;  /* NtSetTimerEx (8) */
    case 230: return 20;  /* NtSignalAndWaitForSingleObjectEx (5) */
    case 231: return  8;  /* NtSuspendThread (2) */
    case 232: return 12;  /* NtUserIoApcDispatcher (3) */
    case 233: return 12;  /* NtWaitForSingleObject (3) */
    case 234: return 16;  /* NtWaitForSingleObjectEx (4) */
    case 235: return 24;  /* NtWaitForMultipleObjectsEx (6) */
    case 236: return 32;  /* NtWriteFile (8) */
    case 237: return 32;  /* NtWriteFileGather (8) */
    case 238: return  0;  /* NtYieldExecution (void) */
    case 243: return 16;  /* ObOpenObjectByName (4) */
    case 247: return 20;  /* ObReferenceObjectByName (5) */
    case 250: return  0;  /* ObfDereferenceObject (fastcall: arg in ecx) */
    case 251: return  0;  /* ObfReferenceObject (fastcall: arg in ecx) */
    case 252: return  4;  /* PhyGetLinkState (1) */
    case 253: return  8;  /* PhyInitialize (2) */
    case 255: return 40;  /* PsCreateSystemThreadEx (10) */
    case 258: return  4;  /* PsTerminateSystemThread (1) */
    case 260: return 12;  /* RtlAnsiStringToUnicodeString (3) */
    case 268: return 12;  /* RtlCompareMemory (3) */
    case 269: return 12;  /* RtlCompareMemoryUlong (3) */
    case 270: return 12;  /* RtlCompareString (3) */
    case 261: return  8;  /* RtlAppendStringToString (2) */
    case 262: return  8;  /* RtlAppendUnicodeStringToString (2) */
    case 263: return  8;  /* RtlAppendUnicodeToString (2) */
    case 264: return 12;  /* RtlAssert (3) */
    case 265: return  4;  /* RtlCaptureContext (1) */
    case 266: return 20;  /* RtlCaptureStackBackTrace (5) */
    case 267: return 12;  /* RtlCharToInteger (3) */
    case 271: return 12;  /* RtlCompareUnicodeString (3) */
    case 272: return  8;  /* RtlCopyString (2) */
    case 273: return  8;  /* RtlCopyUnicodeString (2) */
    case 274: return  8;  /* RtlCreateUnicodeString (2) */
    case 275: return  4;  /* RtlDowncaseUnicodeChar (1) */
    case 276: return 12;  /* RtlDowncaseUnicodeString (3) */
    case 278: return  4;  /* RtlEnterCriticalSectionAndRegion (1) */
    case 280: return 12;  /* RtlEqualUnicodeString (3) */
    case 284: return 12;  /* RtlFillMemory (3) */
    case 287: return  4;  /* RtlFreeUnicodeString (1) */
    case 288: return  8;  /* RtlGetCallersAddress (2) */
    case 292: return 16;  /* RtlIntegerToChar (4) */
    case 293: return 12;  /* RtlIntegerToUnicodeString (3) */
    case 295: return  4;  /* RtlLeaveCriticalSectionAndRegion (1) */
    case 296: return  4;  /* RtlLowerChar (1) */
    case 297: return  8;  /* RtlMapGenericMask (2) */
    case 298: return 12;  /* RtlMoveMemory (3) */
    case 299: return 20;  /* RtlMultiByteToUnicodeN (5) */
    case 300: return 12;  /* RtlMultiByteToUnicodeSize (3) */
    case 306: return  4;  /* RtlTryEnterCriticalSection (1) */
    case 307: return  4;  /* RtlUlongByteSwap (1) */
    case 309: return 12;  /* RtlUnicodeStringToInteger (3) */
    case 310: return 20;  /* RtlUnicodeToMultiByteN (5) */
    case 311: return 12;  /* RtlUnicodeToMultiByteSize (3) */
    case 313: return  4;  /* RtlUpcaseUnicodeChar (1) */
    case 314: return 12;  /* RtlUpcaseUnicodeString (3) */
    case 315: return 20;  /* RtlUpcaseUnicodeToMultiByteN (5) */
    case 316: return  4;  /* RtlUpperChar (1) */
    case 317: return  8;  /* RtlUpperString (2) */
    case 318: return  4;  /* RtlUshortByteSwap (1) */
    case 319: return 12;  /* RtlWalkFrameChain (3) */
    case 320: return  8;  /* RtlZeroMemory (2) */
    case 321: return  0;  /* XboxEEPROMKey (void) */
    case 277: return  4;  /* RtlEnterCriticalSection (1) */
    case 279: return 12;  /* RtlEqualString (3) */
    case 285: return 12;  /* RtlFillMemoryUlong (3) */
    case 286: return  4;  /* RtlFreeAnsiString (1) */
    case 289: return  8;  /* RtlInitAnsiString (2) */
    case 290: return  8;  /* RtlInitUnicodeString (2) */
    case 291: return  4;  /* RtlInitializeCriticalSection (1) */
    case 294: return  4;  /* RtlLeaveCriticalSection (1) */
    case 301: return  4;  /* RtlNtStatusToDosError (1) */
    case 302: return  4;  /* RtlRaiseException (1) */
    case 304: return  8;  /* RtlTimeFieldsToTime (2) */
    case 305: return  8;  /* RtlTimeToTimeFields (2) */
    case 308: return 12;  /* RtlUnicodeStringToAnsiString (3) */
    case 312: return 16;  /* RtlUnwind (4) */
    case 327: return  4;  /* XeLoadSection (1) */
    case 328: return  4;  /* XeUnloadSection (1) */
    case 333: return 12;  /* WRITE_PORT_BUFFER_USHORT (3) */
    case 334: return 12;  /* WRITE_PORT_BUFFER_ULONG (3) */
    case 335: return  4;  /* XcSHAInit (1) */
    case 336: return 12;  /* XcSHAUpdate (3) */
    case 337: return  8;  /* XcSHAFinal (2) */
    case 338: return 12;  /* XcRC4Key (3) */
    case 339: return 12;  /* XcRC4Crypt (3) */
    case 340: return 28;  /* XcHMAC (7) */
    case 341: return 12;  /* XcPKEncPublic (3) */
    case 342: return 12;  /* XcPKDecPrivate (3) */
    case 343: return  4;  /* XcPKGetKeyLen (1) */
    case 344: return 12;  /* XcVerifyPKCS1Signature (3) */
    case 345: return 20;  /* XcModExp (5) */
    case 346: return  8;  /* XcDESKeyParity (2) */
    case 347: return 12;  /* XcKeyTable (3) */
    case 348: return 20;  /* XcBlockCrypt (5) */
    case 349: return 28;  /* XcBlockCryptCBC (7) */
    case 350: return  8;  /* XcCryptService (2) */
    case 351: return  8;  /* XcUpdateCrypto (2) */
    case 352: return 12;  /* RtlRip (3) */
    case 358: return  0;  /* HalIsResetOrShutdownPending (void) */
    case 359: return  4;  /* IoMarkIrpMustComplete (1) */

    /* ── Unknown stubs ── */

    /* ── Pool Allocator ── */
    /* ordinal 16 is the ExEventObjectType data export; see
     * kernel_thunks.c, which points its thunk at kernel data.
     * 17 is ExFreePool and is a real function. */

    /* ── HAL ── */

    /* ── I/O Manager ── */
    /* ordinal 64 is the IoCompletionObjectType data export; see
     * kernel_thunks.c. 65 is IoCreateDevice, a real function. */
    /* case  71: DATA export - IoDeviceObjectType */

    /* ── Kernel Synchronization ── */
    /* case 156: DATA export - KeTickCount */

    /* ── Launch Data ── */
    /* case 164: DATA export - LaunchDataPage */

    /* ── Memory Management ── */

    /* ── NT Virtual Memory ── */

    /* ── NT File I/O & Handle ── */

    /* ── Object Manager ── */
    case 246: return 12;  /* ObReferenceObjectByHandle(3) - Xbox: Handle,Type,Object* */
    case 360: return  0;  /* HalInitiateShutdown (void) */

    /* ── Network / PHY ── */

    /* ── Threading ── */
    /* case 259: DATA export - PsThreadObjectType */

    /* ── Runtime Library ── */

    /* ── Xbox Identity (data exports) ── */
    /* cases 322-328, 355-357: DATA exports */

    /* ── Port I/O ── */

    /* ── Crypto ── */

    /* Data exports */
    case 102: return 0;  /* MmGlobalData (void/data) */
    case 120: return 0;  /* KeInterruptTime (void/data) */
    case 154: return 0;  /* KeSystemTime (void/data) */
    case 240: return 0;  /* ObDirectoryObjectType (void/data) */
    case 245: return 0;  /* ObpObjectHandleTable (void/data) */
    case 249: return 0;  /* ObSymbolicLinkObjectType (void/data) */

    /* Dbg */
    case   6: return  4;  /* DbgBreakPointWithStatus (1) */
    case   7: return 12;  /* DbgLoadImageSymbols (3) */
    case  10: return  8;  /* DbgPrompt (2) */
    case  11: return 12;  /* DbgUnLoadImageSymbols (3) */

    /* Ex */
    case  12: return  4;  /* ExAcquireReadWriteLockExclusive (1) */
    case  13: return  4;  /* ExAcquireReadWriteLockShared (1) */
    case  18: return  4;  /* ExInitializeReadWriteLock (1) */
    case  19: return 12;  /* ExInterlockedAddLargeInteger (3) */
    case  20: return  8;  /* ExInterlockedAddLargeStatistic (2) */
    case  21: return 16;  /* ExInterlockedCompareExchange64 (4) */
    case  25: return 12;  /* ExReadWriteRefurbInfo (3) */
    case  26: return  4;  /* ExRaiseException (1) */
    case  27: return  4;  /* ExRaiseStatus (1) */
    case  28: return  4;  /* ExReleaseReadWriteLock (1) */

    /* ExfInterlocked */
    case  32: return  8;  /* ExfInterlockedInsertHeadList (2) */
    case  33: return  8;  /* ExfInterlockedInsertTailList (2) */
    case  34: return  4;  /* ExfInterlockedRemoveHeadList (1) */

    /* Fsc */
    case  36: return  0;  /* FscInvalidateIdleBlocks (void) */

    /* Hal */
    case  43: return  8;  /* HalEnableSystemInterrupt (2) */
    case 365: return  0;  /* HalEnableSecureTrayEject (void) */
    case 366: return  4;  /* HalWriteSMCScratchRegister (1) */

    /* Interlocked (fastcall, args in ecx/edx) */
    case  51: return  0;  /* InterlockedCompareExchange (fastcall) */
    case  52: return  0;  /* InterlockedDecrement (fastcall) */
    case  53: return  0;  /* InterlockedIncrement (fastcall) */
    case  54: return  0;  /* InterlockedExchange (fastcall) */
    case  55: return  0;  /* InterlockedExchangeAdd (fastcall) */
    case  56: return  0;  /* InterlockedFlushSList (fastcall) */
    case  57: return  0;  /* InterlockedPopEntrySList (fastcall) */
    case  58: return  0;  /* InterlockedPushEntrySList (fastcall) */

    /* Io */
    case  59: return  4;  /* IoAllocateIrp (1) */
    case  60: return 24;  /* IoBuildAsynchronousFsdRequest (6) */
    case  63: return 20;  /* IoCheckShareAccess (5) */
    case  72: return  4;  /* IoFreeIrp (1) */
    case  75: return 20;  /* IoQueryFileInformation (5) */
    case  76: return 20;  /* IoQueryVolumeInformation (5) */
    case  77: return  4;  /* IoQueueThreadIrp (1) */
    case  78: return  8;  /* IoRemoveShareAccess (2) */
    case  80: return 20;  /* IoSetShareAccess (5) */

    /* Kd */
    case  88: return  0;  /* KdDebuggerEnabled (void) */
    case  89: return  0;  /* KdDebuggerNotPresent (void) */

    /* Ke (new) */
    case  92: return  4;  /* KeAlertResumeThread (1) */
    case  94: return  8;  /* KeBoostPriorityThread (2) */
    case 101: return  0;  /* KeEnterCriticalRegion (void) */
    case 103: return  0;  /* KeGetCurrentIrql (void) */
    case 104: return  0;  /* KeGetCurrentThread (void) */
    case 105: return 28;  /* KeInitializeApc (7) */
    case 106: return  4;  /* KeInitializeDeviceQueue (1) */
    case 108: return 12;  /* KeInitializeEvent (3) */
    case 110: return  8;  /* KeInitializeMutant (2) */
    case 111: return  8;  /* KeInitializeQueue (2) */
    case 112: return 12;  /* KeInitializeSemaphore (3) */
    case 114: return 12;  /* KeInsertByKeyDeviceQueue (3) */
    case 115: return  8;  /* KeInsertDeviceQueue (2) */
    case 116: return  8;  /* KeInsertHeadQueue (2) */
    case 117: return  8;  /* KeInsertQueue (2) */
    case 118: return 16;  /* KeInsertQueueApc (4) */
    case 121: return  0;  /* KeIsExecutingDpc (void) */
    case 122: return  0;  /* KeLeaveCriticalRegion (void) */
    case 123: return 12;  /* KePulseEvent (3) */
    case 130: return  0;  /* KeRaiseIrqlToSynchLevel (void) */
    case 131: return 16;  /* KeReleaseMutant (4) */
    case 132: return 16;  /* KeReleaseSemaphore (4) */
    case 133: return  8;  /* KeRemoveByKeyDeviceQueue (2) */
    case 134: return  4;  /* KeRemoveDeviceQueue (1) */
    case 135: return  8;  /* KeRemoveEntryDeviceQueue (2) */
    case 136: return 12;  /* KeRemoveQueue (3) */
    case 138: return  4;  /* KeResetEvent (1) */
    case 140: return  4;  /* KeResumeThread (1) */
    case 141: return  4;  /* KeRundownQueue (1) */
    case 146: return  8;  /* KeSetEventBoostPriority (2) */
    case 147: return  8;  /* KeSetPriorityProcess (2) */
    case 148: return  8;  /* KeSetPriorityThread (2) */
    case 152: return  4;  /* KeSuspendThread (1) */
    case 155: return  4;  /* KeTestAlertThread (1) */
    case 162: return  0;  /* KiBugCheckData (void/data) */
    case 163: return  4;  /* KiUnlockDispatcherDatabase (1) */

    /* Mm */
    case 174: return  4;  /* MmIsAddressValid (1) */
    case 374: return  8;  /* MmDbgAllocateMemory (2) */
    case 375: return  8;  /* MmDbgFreeMemory (2) */
    case 376: return  0;  /* MmDbgQueryAvailablePages (void) */
    case 377: return  8;  /* MmDbgReleaseAddress (2) */
    case 378: return  8;  /* MmDbgWriteCheck (2) */

    /* Nt */
    case 201: return  8;  /* NtOpenDirectoryObject (2) */
    case 208: return 28;  /* NtQueryDirectoryObject (7) */
    case 209: return 16;  /* NtQueryEvent (4) */
    case 212: return 20;  /* NtQueryIoCompletion (5) */
    case 213: return 20;  /* NtQueryMutant (5) */
    case 214: return 20;  /* NtQuerySemaphore (5) */

    /* Ob */
    case 239: return 28;  /* ObCreateObject (7) */
    case 241: return 24;  /* ObInsertObject (6) */
    case 242: return  4;  /* ObMakeTemporaryObject (1) */
    case 244: return 12;  /* ObOpenObjectByPointer (3) */
    case 248: return  8;  /* ObReferenceObjectByPointer (2) */

    /* Ps */
    case 254: return 28;  /* PsCreateSystemThread (7) */
    case 256: return  4;  /* PsQueryStatistics (1) */
    case 257: return  8;  /* PsSetCreateThreadNotifyRoutine (2) */

    /* Rtl */
    case 281: return 12;  /* RtlExtendedIntegerMultiply (3) */
    case 282: return 16;  /* RtlExtendedLargeIntegerDivide (4) */
    case 283: return 16;  /* RtlExtendedMagicDivide (4) */
    case 303: return  4;  /* RtlRaiseStatus (1) */
    case 361: return  0;  /* RtlSnprintf - __cdecl, caller cleans */
    case 362: return  0;  /* RtlSprintf - __cdecl, caller cleans */
    case 363: return  0;  /* RtlVsnprintf - __cdecl, caller cleans */
    case 364: return  0;  /* RtlVsprintf - __cdecl, caller cleans */

    /* Port I/O */
    case 329: return 12;  /* READ_PORT_BUFFER_UCHAR (3) */
    case 330: return 12;  /* READ_PORT_BUFFER_USHORT (3) */
    case 331: return 12;  /* READ_PORT_BUFFER_ULONG (3) */
    case 332: return 12;  /* WRITE_PORT_BUFFER_UCHAR (3) */

    /* Not 0: a genuine zero-argument function and an ordinal nobody has
     * written down are both "pop nothing", but only one of them is a
     * problem, and the warning below could not tell them apart -- it
     * accused FscGetCacheSize, which really does take no arguments. */
    default:  return -1;  /* DATA exports or truly unknown */
    }
}

static bridge_func_t bridge_for_ordinal(ULONG ordinal)
{
    switch (ordinal) {
    /* Threading */
    case 255: return bridge_PsCreateSystemThreadEx;
    case 258: return bridge_PsTerminateSystemThread;

    /* File/Handle */
    case 187: return bridge_NtClose;
    case 190: return bridge_NtCreateFile;
    case 279: return bridge_RtlEqualString;
    case 289: return bridge_RtlInitAnsiString;
    case 195: return bridge_NtDeleteFile;
    case 196: return bridge_NtDeviceIoControlFile;
    case 198: return bridge_NtFlushBuffersFile;
    case 200: return bridge_NtFsControlFile;
    case 202: return bridge_NtOpenFile;
    case 203: return bridge_NtOpenSymbolicLinkObject;
    case 207: return bridge_NtQueryDirectoryFile;
    case 210: return bridge_NtQueryFullAttributesFile;
    case 211: return bridge_NtQueryInformationFile;
    case 218: return bridge_NtQueryVolumeInformationFile;
    case 219: return bridge_NtReadFile;
    case 312: return bridge_RtlUnwind;
    case 327: return bridge_XeLoadSection;
    case 328: return bridge_XeUnloadSection;
    case 226: return bridge_NtSetInformationFile;
    case 236: return bridge_NtWriteFile;

    /* Memory - contiguous */
    case 165: return bridge_MmAllocateContiguousMemory;
    case 166: return bridge_MmAllocateContiguousMemoryEx;
    case 171: return bridge_MmFreeContiguousMemory;
    case 173: return bridge_MmGetPhysicalAddress;
    case 182: return bridge_MmSetAddressProtect;
    case 181: return bridge_MmQueryStatistics;

    /* Memory - virtual */
    case 184: return bridge_NtAllocateVirtualMemory;
    case 199: return bridge_NtFreeVirtualMemory;
    case 215: return bridge_NtQuerySymbolicLinkObject;
    case 217: return bridge_NtQueryVirtualMemory;

    /* Pool */
    case  14: return bridge_ExAllocatePool;
    case  15: return bridge_ExAllocatePoolWithTag;
    case  23: return bridge_ExQueryPoolBlockSize;
    case 268: return bridge_RtlCompareMemory;
    case 269: return bridge_RtlCompareMemoryUlong;
    case  35: return bridge_FscGetCacheSize;
    case  37: return bridge_FscSetCacheSize;
    case  24: return bridge_ExQueryNonVolatileSetting;

    /* IRQL */
    case 160: return bridge_KfRaiseIrql;
    case 161: return bridge_KfLowerIrql;
    case 129: return bridge_KeRaiseIrqlToDpcLevel;

    /* Critical sections */
    case 291: return bridge_RtlInitializeCriticalSection;
    /* Wreckless asks for the AV pack during D3D device creation; unbridged it
     * read 0, which is "no pack connected". Writes one ULONG through a guest
     * pointer it is given and nothing else, and XBOX_TO_NATIVE turns a guest
     * NULL into a host NULL so xbox_AvSendTVEncoderOption's own !Result guard
     * still fires. */
    case   2: return bridge_AvSendTVEncoderOption;
    /* Reads a LARGE_INTEGER and fills a TIME_FIELDS, both at caller-supplied
     * guest addresses -- the case the NOT ROUTED note below names as fine. */
    case 305: return bridge_RtlTimeToTimeFields;
    case 277: return bridge_RtlEnterCriticalSection;
    case 294: return bridge_RtlLeaveCriticalSection;

    /* Timing */
    case 126: return bridge_KeQueryPerformanceCounter;
    case 127: return bridge_KeQueryPerformanceFrequency;
    case 128: return bridge_KeQuerySystemTime;
    case 149: return bridge_KeSetTimer;
    case 150: return bridge_KeSetTimerEx;

    /* DPC / Timer init */
    case 107: return bridge_KeInitializeDpc;
    case 119: return bridge_KeInsertQueueDpc;
    case 137: return bridge_KeRemoveQueueDpc;
    case 153: return bridge_KeSynchronizeExecution;
    case 113: return bridge_KeInitializeTimerEx;

    /* NV2A interrupt plumbing */
    case  44: return bridge_HalGetInterruptVector;
    case  98: return bridge_KeConnectInterrupt;
    case 109: return bridge_KeInitializeInterrupt;
    case  47: return bridge_HalRegisterShutdownNotification;
    case 168: return bridge_MmClaimGpuInstanceMemory;

    /* Synchronization */
    case 189: return bridge_NtCreateEvent;
    case 145: return bridge_KeSetEvent;
    case 159: return bridge_KeWaitForSingleObject;
    case  99: return bridge_KeDelayExecutionThread;
    case 179: return bridge_MmQueryAddressProtect;
    case 232: return bridge_NtUserIoApcDispatcher;
    case  95: return bridge_KeBugCheck;
    case  96: return bridge_KeBugCheckEx;
    case 186: return bridge_NtClearEvent;
    case 205: return bridge_NtPulseEvent;
    case 225: return bridge_NtSetEvent;
    case 233: return bridge_NtWaitForSingleObject;
    case 234: return bridge_NtWaitForSingleObjectEx;
    case 235: return bridge_NtWaitForMultipleObjectsEx;
    case 238: return bridge_NtYieldExecution;

    /* Hardware */
    case   9: return bridge_HalReadSMCTrayState;
    case  49: return bridge_HalReturnToFirmware;

    /* Display */
    case   3: return bridge_AvSetDisplayMode;

    /* I/O */
    case  66: return bridge_IoCreateFile;
    case  65: return bridge_IoCreateDevice;
    case  67: return bridge_IoCreateSymbolicLink;
    case 188: return bridge_NtCreateDirectoryObject;
    case 246: return bridge_ObReferenceObjectByHandle;

    /* Memory - I/O mapping */
    case 177: return bridge_MmMapIoSpace;
    case 178: return bridge_MmPersistContiguousMemory;

    /* RTL */
    case 301: return bridge_RtlNtStatusToDosError;
    case 302: return bridge_RtlRaiseException;

    /* Section B: per-ordinal bridges (261-321), defined in the splice above. */
    case 261: return bridge_RtlAppendStringToString;
    case 262: return bridge_RtlAppendUnicodeStringToString;
    case 263: return bridge_RtlAppendUnicodeToString;
    case 264: return bridge_RtlAssert;
    case 265: return bridge_RtlCaptureContext;
    case 266: return bridge_RtlCaptureStackBackTrace;
    case 267: return bridge_RtlCharToInteger;
    case 270: return bridge_RtlCompareString;
    case 271: return bridge_RtlCompareUnicodeString;
    case 272: return bridge_RtlCopyString;
    case 273: return bridge_RtlCopyUnicodeString;
    case 274: return bridge_RtlCreateUnicodeString;
    case 275: return bridge_RtlDowncaseUnicodeChar;
    case 276: return bridge_RtlDowncaseUnicodeString;
    case 278: return bridge_RtlEnterCriticalSectionAndRegion;
    case 280: return bridge_RtlEqualUnicodeString;
    case 284: return bridge_RtlFillMemory;
    case 287: return bridge_RtlFreeUnicodeString;
    case 288: return bridge_RtlGetCallersAddress;
    case 292: return bridge_RtlIntegerToChar;
    case 293: return bridge_RtlIntegerToUnicodeString;
    case 295: return bridge_RtlLeaveCriticalSectionAndRegion;
    case 296: return bridge_RtlLowerChar;
    case 297: return bridge_RtlMapGenericMask;
    case 298: return bridge_RtlMoveMemory;
    case 299: return bridge_RtlMultiByteToUnicodeN;
    case 300: return bridge_RtlMultiByteToUnicodeSize;
    case 306: return bridge_RtlTryEnterCriticalSection;
    case 307: return bridge_RtlUlongByteSwap;
    case 309: return bridge_RtlUnicodeStringToInteger;
    case 310: return bridge_RtlUnicodeToMultiByteN;
    case 311: return bridge_RtlUnicodeToMultiByteSize;
    case 313: return bridge_RtlUpcaseUnicodeChar;
    case 314: return bridge_RtlUpcaseUnicodeString;
    case 315: return bridge_RtlUpcaseUnicodeToMultiByteN;
    case 316: return bridge_RtlUpperChar;
    case 317: return bridge_RtlUpperString;
    case 318: return bridge_RtlUshortByteSwap;
    case 319: return bridge_RtlWalkFrameChain;
    case 320: return bridge_RtlZeroMemory;
    case 321: return bridge_XboxEEPROMKey;


    /* Routed. The XcRC4 pair was turned OFF mid-bisect and never turned back
     * on. Both pass the memory-model bar: a guest RC4_CONTEXT and a guest data
     * buffer, every access translated through XBOX_TO_NATIVE, no allocation
     * and no host pointer returned. Unrouted, savegame/EEPROM code that used
     * RC4 silently did nothing. */
    case 338: return bridge_XcRC4Key;
    case 339: return bridge_XcRC4Crypt;

    /* Routed. The lesson of the block that used to sit here was that an
     * xbox_* existence does not make a wrapper mechanical: each candidate has
     * to be checked for which side owns the allocation and whether an
     * out-pointer must carry a guest VA. IoCreateDevice was fixed by
     * allocating the device object in guest memory; ExFreePool below answers
     * through the guest heap; MmUnmapIoSpace/IoDeleteDevice do the same.
     *
     * The three beside them were parked, never broken: AvGetSavedDataAddress
     * returns a ULONG value, KeCancelTimer takes a guest KTIMER through
     * XBOX_TO_NATIVE, KeDisconnectInterrupt takes a guest KINTERRUPT the same
     * way. None of them allocates, frees, or hands back a host pointer. */
    case   1: return bridge_AvGetSavedDataAddress;
    case  17: return bridge_ExFreePool;
    case  97: return bridge_KeCancelTimer;
    case 100: return bridge_KeDisconnectInterrupt;
    /* Routed: thread base priority, queried and set. Neither allocates,
     * frees, nor returns a pointer -- each takes a thread handle and a
     * LONG. Half-Life 2 calls both while starting its worker threads,
     * and unbridged they returned 0, so every thread read its own base
     * priority as 0 and any priority the title set was discarded. */
    case 124: return bridge_KeQueryBasePriorityThread;
    case 143: return bridge_KeSetBasePriorityThread;
    /* Routed. Both clear the memory-model bar above: neither allocates,
     * frees, nor hands back a host pointer. KeStallExecutionProcessor takes a
     * microsecond count and busy-waits -- no pointers at all.
     * MmLockUnlockBufferPages takes (BaseAddress, Length, UnlockPages) and
     * pins physical pages, which is a no-op on the host; XBOX_TO_NATIVE is the
     * correct marshalling for its one address argument, and it writes nothing
     * through it.
     *
     * Half-Life 2 calls both during C++ static initialisation. Unbridged they
     * returned 0 without stalling or locking anything -- harmless in isolation,
     * but they are exactly the kind of silent no-op that makes a later failure
     * unattributable. */
    case 151: return bridge_KeStallExecutionProcessor;
    case 175: return bridge_MmLockUnlockBufferPages;
    /* Routed, both checked against the memory-model bar above.
     *
     * MmQueryAllocationSize now answers from the guest heap's block table
     * instead of the host's VirtualQuery, so nothing crosses the two worlds.
     *
     * NtCreateMutant creates a host mutex and hands it back through
     * bridge_write_handle, which is a guest token -- the same shape as
     * NtCreateEvent, which has been routed all along. It allocates no guest
     * memory and returns no host pointer.
     *
     * The Xbox Dashboard needs the mutant: its audio thread creates one during
     * the first tick, and unbridged the call returned STATUS_SUCCESS without
     * writing a handle, so the main thread waited on five events that nothing
     * would ever signal. */
    case 180: return bridge_MmQueryAllocationSize;
    case 192: return bridge_NtCreateMutant;
    case 221: return bridge_NtReleaseMutant;
    /* Routed. Checked against the memory-model warning above rather than
     * assumed mechanical: NtResumeThread takes a handle token and writes a
     * 4-byte suspend count through an optional out-parameter. Guest ULONG and
     * host ULONG are both 4 bytes, XBOX_TO_NATIVE already maps NULL to NULL,
     * and xbox_NtResumeThread checks the pointer before writing. Nothing
     * allocates, frees, or hands back a host pointer -- which is what
     * disqualified IoCreateDevice and ExFreePool.
     *
     * Halo 2276 calls this immediately before its first camera frustum build;
     * unbridged it returned 0 (STATUS_SUCCESS) without resuming anything, so a
     * thread the title had created suspended never started. */
    case 224: return bridge_NtResumeThread;
    case 231: return bridge_NtSuspendThread;
    /* Routed. ObfDereferenceObject is the fastcall partner of 251 (see its
     * wrapper) and only touches the object's reference count; PhyGetLinkState
     * reports link-up and PhyInitialize is a successful no-op. None of the
     * three allocates, frees, or hands back a host pointer; PhyGetLinkState is
     * what titles poll for Ethernet presence before selecting their network
     * path. */
    case 250: return bridge_ObfDereferenceObject;
    case 252: return bridge_PhyGetLinkState;
    case 253: return bridge_PhyInitialize;
    /* Routed. The memory-model note above already names this group as the
     * safe kind: each one reads or writes bytes at an address the caller
     * supplied, and none allocates, frees, or hands back a host pointer.
     *
     * Unbridged they returned 0 without hashing anything, which is invisible
     * until something checks a digest. The Xbox Dashboard verifies each XIP
     * archive it loads against a 20-byte digest in its own table
     * (sub_00034924) and calls HalReturnToFirmware(4) when the compare fails
     * -- so a no-op SHA does not corrupt anything, it reboots the console. */
    case 335: return bridge_XcSHAInit;
    case 336: return bridge_XcSHAUpdate;
    case 337: return bridge_XcSHAFinal;
    case 340: return bridge_XcHMAC;
    case 346: return bridge_XcDESKeyParity;

    /* Routed. Each of these was checked against the memory-model bar
     * described above before being added: reads or writes only happen at
     * caller-supplied guest addresses, and nothing allocates, frees, or hands
     * back a host pointer. Two of them (MmUnmapIoSpace, IoDeleteDevice) free
     * guest memory, so they deliberately answer through the guest heap rather
     * than calling the xbox_* host-heap version -- see the wrappers. */
    case  29: return bridge_ExSaveNonVolatileSetting;
    case  38: return bridge_HalClearSoftwareInterrupt;
    case  45: return bridge_HalReadSMBusValue;
    case  46: return bridge_HalReadWritePCISpace;
    case  48: return bridge_HalRequestSoftwareInterrupt;
    case  50: return bridge_HalWriteSMBusValue;
    case  68: return bridge_IoDeleteDevice;
    case  93: return bridge_KeAlertThread;
    case 125: return bridge_KeQueryInterruptTime;
    case 139: return bridge_KeRestoreFloatingPointState;
    case 142: return bridge_KeSaveFloatingPointState;
    case 158: return bridge_KeWaitForMultipleObjects;
    case 183: return bridge_MmUnmapIoSpace;
    case 193: return bridge_NtCreateSemaphore;
    case 222: return bridge_NtReleaseSemaphore;
    case 228: return bridge_NtSetSystemTime;
    case 247: return bridge_ObReferenceObjectByName;
    case 251: return bridge_ObfReferenceObject;
    case 290: return bridge_RtlInitUnicodeString;
    case 304: return bridge_RtlTimeFieldsToTime;

    /* Routed. The next batch is every remaining xbox_* implementation that
     * clears the memory-model bar, split by category:
     *
     *  - HAL/AV/port-I/O and the I/O Manager stubs all take or produce value
     *    arguments and caller-supplied guest structures; none of them returns
     *    a pointer the title dereferences. IofCallDriver/IofCompleteRequest
     *    are __fastcall (ns in ecx/edx).
     *  - The Xc* entries are documented stubs (no Xbox Live, no on-console key
     *    derivation); pointers are translated but never dereferenced.
     *  - MmAllocateSystemMemory/MmCreateKernelStack answer through the guest
     *    heap rather than VirtualAlloc so the title sees a 32-bit guest VA;
     *    their frees pair with them.
     *  - NtDuplicateObject dups a real native handle and hands it back as a
     *    fresh guest token; RtlRip logs guest strings; the ANSI<->Unicode
     *    conversion bridges do the whole conversion in guest memory (their
     *    xbox_* versions write host pointers into 4-byte guest slots).
     */
    case   4: return bridge_AvSetSavedDataAddress;
    case   5: return bridge_DbgBreakPoint;
    case  39: return bridge_HalDisableSystemInterrupt;
    case 360: return bridge_HalInitiateShutdown;
    case 358: return bridge_HalIsResetOrShutdownPending;
    case 333: return bridge_WRITE_PORT_BUFFER_USHORT;
    case 334: return bridge_WRITE_PORT_BUFFER_ULONG;

    case  61: return bridge_IoBuildDeviceIoControlRequest;
    case  62: return bridge_IoBuildSynchronousFsdRequest;
    case  69: return bridge_IoDeleteSymbolicLink;
    case  73: return bridge_IoInitializeIrp;
    case  74: return bridge_IoInvalidDeviceRequest;
    case  79: return bridge_IoSetIoCompletion;
    case  81: return bridge_IoStartNextPacket;
    case  82: return bridge_IoStartNextPacketByKey;
    case  83: return bridge_IoStartPacket;
    case  84: return bridge_IoSynchronousDeviceIoControlRequest;
    case  85: return bridge_IoSynchronousFsdRequest;
    case  86: return bridge_IofCallDriver;
    case  87: return bridge_IofCompleteRequest;
    case 359: return bridge_IoMarkIrpMustComplete;

    case 176: return bridge_MmLockUnlockPhysicalPage;
    /* Allocator and free together: both answer from the guest heap, and the
     * allocator page-aligns and zeroes because that is what the console's
     * page allocator returns and callers assume it. */
    case 167: return bridge_MmAllocateSystemMemory;
    case 172: return bridge_MmFreeSystemMemory;
    case 169: return bridge_MmCreateKernelStack;
    case 170: return bridge_MmDeleteKernelStack;

    case 341: return bridge_XcPKEncPublic;
    case 342: return bridge_XcPKDecPrivate;
    case 343: return bridge_XcPKGetKeyLen;
    case 344: return bridge_XcVerifyPKCS1Signature;
    case 345: return bridge_XcModExp;
    case 347: return bridge_XcKeyTable;
    case 348: return bridge_XcBlockCrypt;
    case 349: return bridge_XcBlockCryptCBC;
    case 350: return bridge_XcCryptService;
    case 351: return bridge_XcUpdateCrypto;

    case 352: return bridge_RtlRip;
    case 260: return bridge_RtlAnsiStringToUnicodeString;
    case 308: return bridge_RtlUnicodeStringToAnsiString;
    case 197: return bridge_NtDuplicateObject;

    /* Data exports */
    case 102: return bridge_MmGlobalData;
    case 120: return bridge_KeInterruptTime;
    case 154: return bridge_KeSystemTime;
    case 240: return bridge_ObDirectoryObjectType;
    case 245: return bridge_ObpObjectHandleTable;
    case 249: return bridge_ObSymbolicLinkObjectType;

    /* Dbg */
    case   6: return bridge_DbgBreakPointWithStatus;
    case   7: return bridge_DbgLoadImageSymbols;
    case   8: return bridge_DbgPrint;
    case  10: return bridge_DbgPrompt;
    case  11: return bridge_DbgUnLoadImageSymbols;

    /* Ex */
    case  12: return bridge_ExAcquireReadWriteLockExclusive;
    case  13: return bridge_ExAcquireReadWriteLockShared;
    case  18: return bridge_ExInitializeReadWriteLock;
    case  19: return bridge_ExInterlockedAddLargeInteger;
    case  20: return bridge_ExInterlockedAddLargeStatistic;
    case  21: return bridge_ExInterlockedCompareExchange64;
    case  25: return bridge_ExReadWriteRefurbInfo;
    case  26: return bridge_ExRaiseException;
    case  27: return bridge_ExRaiseStatus;
    case  28: return bridge_ExReleaseReadWriteLock;

    /* ExfInterlocked */
    case  32: return bridge_ExfInterlockedInsertHeadList;
    case  33: return bridge_ExfInterlockedInsertTailList;
    case  34: return bridge_ExfInterlockedRemoveHeadList;

    /* Fsc */
    case  36: return bridge_FscInvalidateIdleBlocks;

    /* Hal */
    case  43: return bridge_HalEnableSystemInterrupt;
    case 365: return bridge_HalEnableSecureTrayEject;
    case 366: return bridge_HalWriteSMCScratchRegister;

    /* Interlocked */
    case  51: return bridge_InterlockedCompareExchange;
    case  52: return bridge_InterlockedDecrement;
    case  53: return bridge_InterlockedIncrement;
    case  54: return bridge_InterlockedExchange;
    case  55: return bridge_InterlockedExchangeAdd;
    case  56: return bridge_InterlockedFlushSList;
    case  57: return bridge_InterlockedPopEntrySList;
    case  58: return bridge_InterlockedPushEntrySList;

    /* Io */
    case  59: return bridge_IoAllocateIrp;
    case  60: return bridge_IoBuildAsynchronousFsdRequest;
    case  63: return bridge_IoCheckShareAccess;
    case  72: return bridge_IoFreeIrp;
    case  75: return bridge_IoQueryFileInformation;
    case  76: return bridge_IoQueryVolumeInformation;
    case  77: return bridge_IoQueueThreadIrp;
    case  78: return bridge_IoRemoveShareAccess;
    case  80: return bridge_IoSetShareAccess;
    case  90: return bridge_IoDismountVolume;
    case  91: return bridge_IoDismountVolumeByName;

    /* Kd */
    case  88: return bridge_KdDebuggerEnabled;
    case  89: return bridge_KdDebuggerNotPresent;

    /* Ke (new) */
    case  92: return bridge_KeAlertResumeThread;
    case  94: return bridge_KeBoostPriorityThread;
    case 101: return bridge_KeEnterCriticalRegion;
    case 103: return bridge_KeGetCurrentIrql;
    case 104: return bridge_KeGetCurrentThread;
    case 105: return bridge_KeInitializeApc;
    case 106: return bridge_KeInitializeDeviceQueue;
    case 108: return bridge_KeInitializeEvent;
    case 110: return bridge_KeInitializeMutant;
    case 111: return bridge_KeInitializeQueue;
    case 112: return bridge_KeInitializeSemaphore;
    case 114: return bridge_KeInsertByKeyDeviceQueue;
    case 115: return bridge_KeInsertDeviceQueue;
    case 116: return bridge_KeInsertHeadQueue;
    case 117: return bridge_KeInsertQueue;
    case 118: return bridge_KeInsertQueueApc;
    case 121: return bridge_KeIsExecutingDpc;
    case 122: return bridge_KeLeaveCriticalRegion;
    case 123: return bridge_KePulseEvent;
    case 130: return bridge_KeRaiseIrqlToSynchLevel;
    case 131: return bridge_KeReleaseMutant;
    case 132: return bridge_KeReleaseSemaphore;
    case 133: return bridge_KeRemoveByKeyDeviceQueue;
    case 134: return bridge_KeRemoveDeviceQueue;
    case 135: return bridge_KeRemoveEntryDeviceQueue;
    case 136: return bridge_KeRemoveQueue;
    case 138: return bridge_KeResetEvent;
    case 140: return bridge_KeResumeThread;
    case 141: return bridge_KeRundownQueue;
    case 144: return bridge_KeSetDisableBoostThread;
    case 146: return bridge_KeSetEventBoostPriority;
    case 147: return bridge_KeSetPriorityProcess;
    case 148: return bridge_KeSetPriorityThread;
    case 152: return bridge_KeSuspendThread;
    case 155: return bridge_KeTestAlertThread;
    case 162: return bridge_KiBugCheckData;
    case 163: return bridge_KiUnlockDispatcherDatabase;

    /* Mm */
    case 174: return bridge_MmIsAddressValid;
    case 374: return bridge_MmDbgAllocateMemory;
    case 375: return bridge_MmDbgFreeMemory;
    case 376: return bridge_MmDbgQueryAvailablePages;
    case 377: return bridge_MmDbgReleaseAddress;
    case 378: return bridge_MmDbgWriteCheck;

    /* Nt */
    case 185: return bridge_NtCancelTimer;
    case 191: return bridge_NtCreateIoCompletion;
    case 194: return bridge_NtCreateTimer;
    case 201: return bridge_NtOpenDirectoryObject;
    case 204: return bridge_NtProtectVirtualMemory;
    case 206: return bridge_NtQueueApcThread;
    case 208: return bridge_NtQueryDirectoryObject;
    case 209: return bridge_NtQueryEvent;
    case 212: return bridge_NtQueryIoCompletion;
    case 213: return bridge_NtQueryMutant;
    case 214: return bridge_NtQuerySemaphore;
    case 216: return bridge_NtQueryTimer;
    case 220: return bridge_NtReadFileScatter;
    case 223: return bridge_NtRemoveIoCompletion;
    case 227: return bridge_NtSetIoCompletion;
    case 229: return bridge_NtSetTimerEx;
    case 230: return bridge_NtSignalAndWaitForSingleObjectEx;
    case 237: return bridge_NtWriteFileGather;

    /* Ob */
    case 239: return bridge_ObCreateObject;
    case 241: return bridge_ObInsertObject;
    case 242: return bridge_ObMakeTemporaryObject;
    case 243: return bridge_ObOpenObjectByName;
    case 244: return bridge_ObOpenObjectByPointer;
    case 248: return bridge_ObReferenceObjectByPointer;

    /* Ps */
    case 254: return bridge_PsCreateSystemThread;
    case 256: return bridge_PsQueryStatistics;
    case 257: return bridge_PsSetCreateThreadNotifyRoutine;

    /* Rtl */
    case 281: return bridge_RtlExtendedIntegerMultiply;
    case 282: return bridge_RtlExtendedLargeIntegerDivide;
    case 283: return bridge_RtlExtendedMagicDivide;
    case 285: return bridge_RtlFillMemoryUlong;
    case 286: return bridge_RtlFreeAnsiString;
    case 303: return bridge_RtlRaiseStatus;
    case 361: return bridge_RtlSnprintf;
    case 362: return bridge_RtlSprintf;
    case 363: return bridge_RtlVsnprintf;
    case 364: return bridge_RtlVsprintf;

    /* Port I/O */
    case 329: return bridge_READ_PORT_BUFFER_UCHAR;
    case 330: return bridge_READ_PORT_BUFFER_USHORT;
    case 331: return bridge_READ_PORT_BUFFER_ULONG;
    case 332: return bridge_WRITE_PORT_BUFFER_UCHAR;

    default:  return NULL;
    }
}

/* ── Per-slot bridge functions (resolved at init) ────────── */

static bridge_func_t g_slot_bridges[XBOX_KERNEL_THUNK_TABLE_SIZE];
static int g_slot_arg_bytes[XBOX_KERNEL_THUNK_TABLE_SIZE];
static uint8_t g_slot_arg_unknown[XBOX_KERNEL_THUNK_TABLE_SIZE];

/* Xbox VA to sample around each bridge call; 0 = off. See dispatch. */
uint32_t g_kernel_watch_va = 0;

/* Arm the watch from the environment.
 *
 * The facility existed but nothing set it, so it was unreachable.
 * RECOMP_KERNEL_WATCH=<guest addr> samples that dword either side of every
 * bridge call and names the ordinal that changed it -- which is the one fact
 * a hardware watchpoint cannot give, because a bridge corrupting Xbox memory
 * faults inside ntdll with no recompiled frame to blame.
 *
 * A change seen between the previous call's "after" and this call's "before"
 * is guest code, not a bridge, which is just as useful to know. */
static void kernel_watch_arm_once(void)
{
    static int done;
    const char *env;
    if (done)
        return;
    done = 1;
    env = getenv("RECOMP_KERNEL_WATCH");
    if (env)
        g_kernel_watch_va = (uint32_t)strtoul(env, NULL, 0);
}

/* Current dispatching slot */
static RECOMP_TLS int g_kernel_dispatch_slot = -1;

static void kernel_thunk_dispatch_body(void);

/* The guest CPU lock (RECOMP_GUEST_LOCK=1).
 *
 * The Xbox has one CPU: two guest threads never run guest code at the same
 * instant, and titles lean on that without knowing it -- a worker fills a
 * buffer and sets a flag with plain stores, and the reader trusts the order.
 * Here every guest thread runs on its own host core, and the lifted C gives
 * no ordering either. T()NY crashes somewhere different on every run
 * once its loader workers start.
 *
 * With the lock on, a guest thread holds it while it runs guest code and
 * lets go for kernel calls below DISPATCH_LEVEL -- the waits, sleeps and I/O
 * where the console's scheduler can switch threads. Raised IRQL keeps CPU
 * ownership across calls. Timer DPCs use the same ownership. Taken local
 * backward branches offer a host scheduling point at ten-millisecond
 * intervals; waiting deferred work can acquire it at the next eligible
 * backedge. The original IRQL/IF gate applies before releasing this lock.
 * Interrupt callbacks retain their existing masked device-delivery path. */
static CRITICAL_SECTION g_guest_cpu;
static HANDLE g_guest_cpu_acquired;
static LONG g_guest_cpu_waiters;
static LONG g_guest_cpu_handoff_waiters;
static int g_guest_cpu_on = -1;

static RECOMP_TLS int t_guest_held;       /* and holds the guest CPU */
static RECOMP_TLS ULONGLONG t_guest_yield_tick;
typedef struct GuestKernelWait {
    RecompContFrame frame;
    uint32_t slot, entry_esp, remaining_ms;
    uint32_t main_routine, main_saved_depth, watch_before;
    uint32_t object_count,object_tokens_va,signal_done;
    uint32_t resume_esp,native_held;
    RecompContLocal locals[10];
    ULONGLONG apc_started;
    int restoring, dispatch_started;
    struct GuestKernelWait *parent;
} GuestKernelWait;
static RECOMP_TLS GuestKernelWait *t_kernel_wait;
static void (*g_checkpoint_hook)(void);
#define CHECKPOINT_THREADS 64
typedef struct CheckpointOwnership {
    HANDLE handle;
    uint32_t address,depth,site;
} CheckpointOwnership;
typedef struct GuestCheckpointThread {
    int active, parked, failed;
    DWORD host_id;
    uint32_t pcr;
    FILE *packet;
    HANDLE restore_control;
    CheckpointOwnership *owners;
    unsigned owner_count;
    volatile LONG owner_done;
    int owner_release,root_ready,hot_command,hot_stop,hot_loaded;
    volatile LONG hot_ready;
} GuestCheckpointThread;
static GuestCheckpointThread g_checkpoint_threads[CHECKPOINT_THREADS];
static RECOMP_TLS GuestCheckpointThread *t_checkpoint_thread;
static RECOMP_TLS jmp_buf t_checkpoint_root_jump;
static RECOMP_TLS int t_checkpoint_root_valid,t_checkpoint_cancelled;
static RECOMP_TLS recomp_func_t t_checkpoint_root_function;
static SRWLOCK g_checkpoint_lock=SRWLOCK_INIT, g_checkpoint_devices=SRWLOCK_INIT;
static INIT_ONCE g_checkpoint_once=INIT_ONCE_STATIC_INIT;
static HANDLE g_checkpoint_release;
static volatile LONG g_checkpoint_requested, g_checkpoint_busy, g_checkpoint_stop_devices;
static RECOMP_TLS int t_checkpoint_controller;
static RECOMP_TLS unsigned t_checkpoint_device_depth;
static struct { int active,has_guest_context; XboxGuestRegisters registers; } g_checkpoint_device_context[XBOX_CHECKPOINT_DEVICE_COUNT];
static XboxGuestRegisters g_checkpoint_device_restore[XBOX_CHECKPOINT_DEVICE_COUNT];
static int g_checkpoint_device_restore_pending[XBOX_CHECKPOINT_DEVICE_COUNT];
static volatile LONG g_checkpoint_device_starting[XBOX_CHECKPOINT_DEVICE_COUNT];
void xbox_CheckpointDeviceCreating(unsigned device) {if(device && device<XBOX_CHECKPOINT_DEVICE_COUNT)InterlockedExchange(&g_checkpoint_device_starting[device],1);}
void xbox_CheckpointDeviceCreated(unsigned device) {if(device && device<XBOX_CHECKPOINT_DEVICE_COUNT)InterlockedExchange(&g_checkpoint_device_starting[device],0);}
uint32_t xbox_CheckpointDeviceMask(void) {
    uint32_t mask=0;for(unsigned i=1;i<XBOX_CHECKPOINT_DEVICE_COUNT;i++)
        if(InterlockedCompareExchange((LONG *)&g_checkpoint_device_context[i].active,0,0))mask|=1u<<i;
    return mask;
}
static int checkpoint_clear_objects(void);
static int checkpoint_record_identity(GuestCheckpointThread *,uint32_t *);
static int checkpoint_read_thread_packet(FILE *);
static void checkpoint_park_control(void);
static int checkpoint_root_cancelled(void) { return t_checkpoint_cancelled; }
static BOOL CALLBACK checkpoint_init(PINIT_ONCE once,PVOID parameter,PVOID *context) {
    (void)once;(void)parameter;(void)context;
    g_checkpoint_release=CreateEvent(NULL,TRUE,TRUE,NULL);
    return g_checkpoint_release!=NULL;
}
static void *checkpoint_new_thread(void) {
    if (!InitOnceExecuteOnce(&g_checkpoint_once,checkpoint_init,NULL,NULL)) recomp_cont_invalid();
    AcquireSRWLockExclusive(&g_checkpoint_lock);
    for (unsigned i=0;i<CHECKPOINT_THREADS;i++) if (!g_checkpoint_threads[i].active) {
        GuestCheckpointThread *thread=&g_checkpoint_threads[i];
        memset(thread,0,sizeof *thread);thread->active=1;
        thread->restore_control=CreateEvent(NULL,FALSE,FALSE,NULL);
        if(!thread->restore_control) recomp_cont_invalid();
        ReleaseSRWLockExclusive(&g_checkpoint_lock);return thread;
    }
    ReleaseSRWLockExclusive(&g_checkpoint_lock);recomp_cont_invalid();return NULL;
}
static void checkpoint_set_thread_pcr(void *record,uint32_t pcr,DWORD host_id) {
    AcquireSRWLockExclusive(&g_checkpoint_lock);
    GuestCheckpointThread *thread=record;thread->pcr=pcr;thread->host_id=host_id;
    ReleaseSRWLockExclusive(&g_checkpoint_lock);
}
static uint32_t ke_thread_for_handle(HANDLE handle) {
    DWORD id=GetThreadId(handle);if(!id)return 0;
    uint32_t object=ke_object_for_handle(handle);if(object)return object;
    if(id==GetCurrentThreadId())return ke_current_thread();
    uint32_t result=0;AcquireSRWLockShared(&g_checkpoint_lock);
    for(unsigned i=0;i<CHECKPOINT_THREADS;i++) {
        GuestCheckpointThread *thread=&g_checkpoint_threads[i];
        if(thread->active && thread->host_id==id && thread->pcr) {result=BRIDGE_MEM32(thread->pcr+0x28);break;}
    }
    ReleaseSRWLockShared(&g_checkpoint_lock);return result;
}
static void checkpoint_bind_thread(void *record) {
    if (!record) return;
    t_checkpoint_thread=record;
    AcquireSRWLockExclusive(&g_checkpoint_lock);
    t_checkpoint_thread->host_id=GetCurrentThreadId();
    t_checkpoint_thread->pcr=g_fs_base;
    t_checkpoint_thread->root_ready=t_checkpoint_root_valid;
    ReleaseSRWLockExclusive(&g_checkpoint_lock);
}
static void checkpoint_discard_thread(void *record) {
    if (!record) return;
    AcquireSRWLockExclusive(&g_checkpoint_lock);
    GuestCheckpointThread *thread=record;
    if (thread->packet) fclose(thread->packet);
    if (thread->restore_control) CloseHandle(thread->restore_control);
    free(thread->owners);
    memset(thread,0,sizeof *thread);
    ReleaseSRWLockExclusive(&g_checkpoint_lock);
}
static void checkpoint_drop_thread(void) {
    checkpoint_discard_thread(t_checkpoint_thread);t_checkpoint_thread=NULL;
}
unsigned xbox_CheckpointGuestRootCount(void) {
    unsigned count=0;
    AcquireSRWLockShared(&g_checkpoint_lock);
    for(unsigned i=0;i<CHECKPOINT_THREADS;i++)count+=g_checkpoint_threads[i].active!=0;
    ReleaseSRWLockShared(&g_checkpoint_lock);
    return count;
}
static int checkpoint_read_thread_packet(FILE *packet) {
    uint32_t identity[4];XboxGuestRegisters registers;rewind(packet);
    if(fread(identity,sizeof identity,1,packet)!=1 || fread(&registers,sizeof registers,1,packet)!=1 ||
       !xbox_RestoreGuestRegisters(&registers) || !recomp_cont_load(packet)) return 0;
    if(t_checkpoint_thread)t_checkpoint_thread->pcr=g_fs_base;
    g_thread_stack_top=identity[1];g_is_spawned_thread=(int)identity[2];
    t_checkpoint_root_function=checkpoint_lookup_root(identity[3]);return t_checkpoint_root_function!=NULL;
}
static void checkpoint_park_control(void) {
    GuestCheckpointThread *thread=t_checkpoint_thread;
    HANDLE gates[]={g_checkpoint_release,thread->restore_control};
    for (;;) {
        DWORD result=WaitForMultipleObjects(2,gates,FALSE,INFINITE);
        if(result==WAIT_OBJECT_0) break;
        if(result!=WAIT_OBJECT_0+1) recomp_cont_invalid();
        if(thread->hot_command==1) {
            if(!t_checkpoint_root_valid) recomp_cont_invalid();
            thread->hot_command=0;t_guest_thread=0;t_checkpoint_cancelled=1;
            recomp_cont_discard_root();recomp_discard_native_jumps();
            if(t_kernel_wait || t_dispatch_depth || t_guest_held) recomp_cont_invalid();
            longjmp(t_checkpoint_root_jump,1);
        }
        if(thread->hot_command==2) {
            thread->hot_command=0;
            if(!checkpoint_read_thread_packet(thread->packet)) recomp_cont_invalid();
            thread->hot_loaded=1;InterlockedExchange(&thread->hot_ready,1);
        }
        for(unsigned i=0;i<thread->owner_count;i++) {
            CheckpointOwnership *owner=&thread->owners[i];
            if(owner->handle) {
                for(uint32_t n=0;n<owner->depth;n++) {
                    if(thread->owner_release) {if(!ReleaseMutex(owner->handle))recomp_cont_invalid();}
                    else if(WaitForSingleObject(owner->handle,0)!=WAIT_OBJECT_0)recomp_cont_invalid();
                }
            } else if(thread->owner_release) {
                for(uint32_t n=0;n<owner->depth;n++) xbox_RtlLeaveCriticalSection((PRTL_CRITICAL_SECTION)XBOX_TO_NATIVE(owner->address));
            } else xbox_RestoreCriticalSectionOwner(owner->address,owner->depth,owner->site);
        }
        if(thread->owner_count) InterlockedExchange(&thread->owner_done,1);
    }
    AcquireSRWLockExclusive(&g_checkpoint_lock);thread->parked=0;ReleaseSRWLockExclusive(&g_checkpoint_lock);
}
void xbox_ExceptionContinue(uint32_t target, uint32_t frame_ebp) {
    if (!t_checkpoint_root_valid) recomp_cont_invalid();
    XboxGuestRegisters saved;RecompContFrame frame;
    const RecompContLocal locals[]={
        GUEST_REGISTER_CONT_LOCALS(saved,"saved")
        {"target",&target,4},{"frame_ebp",&frame_ebp,4}
    };
    uint64_t resume=kernel_native_cont_enter(&frame,KERNEL_CONT_EXCEPTION_TRANSFER,locals,sizeof locals/sizeof locals[0]);
    if(!recomp_lookup(target))recomp_cont_invalid();
    if(!resume) {
        xbox_CaptureGuestRegisters(&saved);saved.ebp=saved.seh_ebp=frame_ebp;
    }
    recomp_cont_prepare_unwind(recomp_cont_transfer_owner(frame_ebp));
    recomp_cont_prepare_transfer(frame_ebp,target);
    if (t_kernel_wait || t_dispatch_depth) recomp_cont_invalid();
    if (!xbox_RestoreGuestRegisters(&saved)) recomp_cont_invalid();
    longjmp(t_checkpoint_root_jump,2);
}

static void checkpoint_terminate_thread(void) {
    if(!t_checkpoint_root_valid)recomp_cont_invalid();
    /* Run the existing frame leave callbacks before abandoning native frames.
     * They release dispatch ownership and restore the parent wait chain. */
    recomp_cont_discard_root();recomp_discard_native_jumps();
    if(t_kernel_wait || t_dispatch_depth)recomp_cont_invalid();
    t_guest_terminated=1;
    longjmp(t_checkpoint_root_jump,3);
}
void xbox_RunGuestRoot(void (*fn)(void)) {
    if(t_checkpoint_root_valid) {fn();return;}
    if(recomp_cont_active()) recomp_cont_invalid();
    t_checkpoint_root_function=fn;t_checkpoint_cancelled=0;t_guest_terminated=0;t_checkpoint_root_valid=1;
    if(t_checkpoint_thread) t_checkpoint_thread->root_ready=1;
    int root_transfer=setjmp(t_checkpoint_root_jump);
    if(root_transfer==3) {t_checkpoint_root_valid=0;return;}
    if(root_transfer==1) {
        GuestCheckpointThread *thread=t_checkpoint_thread;
        if(thread->hot_stop) {
            XboxGuestRegisters clean;xbox_CaptureGuestRegisters(&clean);
            clean.irql=0;clean.interrupt_state=1;
            if(!xbox_RestoreGuestRegisters(&clean))recomp_cont_invalid();
            t_checkpoint_cancelled=1;t_checkpoint_root_valid=0;return;
        }
        InterlockedExchange(&thread->hot_ready,1);checkpoint_park_control();
        if(!thread->hot_loaded) recomp_cont_invalid();thread->hot_loaded=0;t_checkpoint_cancelled=0;guest_cpu_join();
    }
    if(root_transfer!=0 && root_transfer!=1 && root_transfer!=2) recomp_cont_invalid();
    t_checkpoint_root_function();t_checkpoint_root_valid=0;
}
static int checkpoint_restore_thread(struct bridge_thread_start *start) {
    CheckpointRestoreBatch *batch=start->restore_batch;
    if (WaitForSingleObject(batch->start,INFINITE)!=WAIT_OBJECT_0) recomp_cont_invalid();
    if (!batch->commit) {
        checkpoint_discard_thread(start->checkpoint_record);return 0;
    }
    checkpoint_bind_thread(start->checkpoint_record);
    if(!checkpoint_read_thread_packet(start->restore_packet))recomp_cont_invalid();
    AcquireSRWLockExclusive(&g_checkpoint_lock);
    t_checkpoint_thread->parked=1;
    ReleaseSRWLockExclusive(&g_checkpoint_lock);
    /* The coordinator still owns the original CPU and device gates. No guest
     * instruction executes until every saved root is ready and End releases. */
    checkpoint_park_control();
    return 1;
}
typedef struct CheckpointThreadsPrepared {
    unsigned count;int quiesced,applied;
    struct CheckpointThreadEntry {FILE *packet;uint32_t identity[4];recomp_func_t fn;HANDLE handle;
            GuestCheckpointThread *record;struct bridge_thread_start *start;int existing;} entries[CHECKPOINT_THREADS];
    GuestCheckpointThread *old[CHECKPOINT_THREADS];unsigned old_count;
    CheckpointRestoreBatch batch;
} CheckpointThreadsPrepared;
void xbox_CheckpointFreeThreads(void *opaque) {
    CheckpointThreadsPrepared *prepared=opaque;if(!prepared)return;
    if(prepared->quiesced && !prepared->applied) recomp_cont_invalid();
    if(!prepared->applied && prepared->batch.start) {
        SetEvent(prepared->batch.start);
        for(unsigned i=0;i<prepared->count;i++) if(prepared->entries[i].start) {
            if(WaitForSingleObject(prepared->entries[i].handle,INFINITE)!=WAIT_OBJECT_0)recomp_cont_invalid();
        }
    }
    for(unsigned i=0;i<prepared->count;i++) {
        if(prepared->entries[i].packet)fclose(prepared->entries[i].packet);
        if(!prepared->applied && prepared->entries[i].handle)CloseHandle(prepared->entries[i].handle);
    }
    if(prepared->batch.start)CloseHandle(prepared->batch.start);free(prepared);
}
void *xbox_CheckpointPrepareThreads(FILE *in,unsigned capacity) {
    if(!t_checkpoint_controller || !in || !capacity)return NULL;
    char magic[8];uint32_t count;
    if(fread(magic,8,1,in)!=1 || memcmp(magic,"XBTHREAD",8) || fread(&count,4,1,in)!=1 ||
       !count || count>CHECKPOINT_THREADS || count>capacity)return NULL;
    CheckpointThreadsPrepared *prepared=calloc(1,sizeof *prepared);if(!prepared)return NULL;
    prepared->count=count;
    struct CheckpointThreadEntry *staged=prepared->entries;
    int ok=1;
    for (unsigned i=0;ok && i<count;i++) {
        uint32_t bytes;XboxGuestRegisters registers;uint32_t root_function;
        ok=fread(&bytes,sizeof bytes,1,in)==1 && bytes>sizeof staged[i].identity+sizeof registers;
        if (!ok) break;
        FILE *packet=staged[i].packet=tmpfile();
        if (!packet) { ok=0;break; }
        unsigned char buffer[4096];
        while (ok && bytes) {
            size_t chunk=bytes<sizeof buffer?bytes:sizeof buffer;
            ok=fread(buffer,chunk,1,in)==1 && fwrite(buffer,chunk,1,packet)==1;
            bytes-=(uint32_t)chunk;
        }
        rewind(packet);
        ok=ok && fread(staged[i].identity,sizeof staged[i].identity,1,packet)==1 &&
           fread(&registers,sizeof registers,1,packet)==1 && xbox_ValidateGuestRegisters(&registers) &&
           staged[i].identity[0]==registers.fs_base && staged[i].identity[0]>=0x1000 &&
           (uint64_t)staged[i].identity[0]+0x400<=xbox_GetMappedSize() &&
           staged[i].identity[2]<=1 && registers.esp>=4 &&
           (uint64_t)registers.esp+4<=xbox_GetMappedSize() &&
           (!staged[i].identity[2] || (staged[i].identity[1] && staged[i].identity[1]<=xbox_GetMappedSize())) &&
           recomp_cont_validate(packet,&root_function) && root_function==staged[i].identity[3] &&
           fgetc(packet)==EOF && !ferror(packet);
        if (ok) {
            staged[i].fn=checkpoint_lookup_root(root_function);ok=staged[i].fn!=NULL;
            for (unsigned j=0;ok && j<i;j++)
                ok=memcmp(staged[j].identity,staged[i].identity,sizeof staged[i].identity)!=0;
        }
    }

    for(unsigned n=0;ok && n<CHECKPOINT_THREADS;n++) if(g_checkpoint_threads[n].active) {
        GuestCheckpointThread *thread=&g_checkpoint_threads[n];uint32_t identity[4];
        ok=thread->parked && thread->root_ready && checkpoint_record_identity(thread,identity);
        unsigned match=count;
        for(unsigned i=0;ok && i<count;i++) if(!memcmp(identity,staged[i].identity,16)){match=i;break;}
        if(ok && !identity[2] && match==count) ok=0;
        if(!ok)break;
        prepared->old[prepared->old_count++]=thread;
        if(match<count) {
            staged[match].record=thread;staged[match].existing=1;
#ifdef _WIN32
            staged[match].handle=OpenThread(THREAD_ALL_ACCESS,FALSE,thread->host_id);
#else
            staged[match].handle=NULL;
#endif
            ok=staged[match].handle!=NULL;
        }
    }
    if(ok){prepared->batch.start=CreateEvent(NULL,TRUE,FALSE,NULL);ok=prepared->batch.start!=NULL;}
    for(unsigned i=0;ok && i<count;i++) if(!staged[i].existing) {
        struct bridge_thread_start *entry=calloc(1,sizeof *entry);if(!entry){ok=0;break;}
        entry->fn=staged[i].fn;entry->restore_packet=staged[i].packet;entry->restore_batch=&prepared->batch;
        staged[i].handle=CreateThread(NULL,0,bridge_thread_main,entry,0,NULL);
        if(!staged[i].handle){free(entry);ok=0;break;}staged[i].start=entry;
    }
    if(!ok){xbox_CheckpointFreeThreads(prepared);return NULL;}return prepared;
}
int xbox_CheckpointQuiesceThreads(void *opaque) {
    CheckpointThreadsPrepared *prepared=opaque;
    if(!prepared || prepared->quiesced || !t_checkpoint_controller)return 0;
    /* Cold startup also owns device and namespace objects before a guest root exists. */
    if(!checkpoint_clear_objects())return 0;
    for(unsigned n=0;n<prepared->old_count;n++) {
        GuestCheckpointThread *thread=prepared->old[n];int mapped=0;
        for(unsigned i=0;i<prepared->count;i++)mapped|=prepared->entries[i].record==thread;
        thread->hot_stop=!mapped;thread->hot_loaded=0;InterlockedExchange(&thread->hot_ready,0);
        thread->hot_command=1;if(!SetEvent(thread->restore_control))recomp_cont_invalid();
    }
    for(;;) {
        int ready=1;AcquireSRWLockShared(&g_checkpoint_lock);
        for(unsigned n=0;n<prepared->old_count;n++) {
            GuestCheckpointThread *thread=prepared->old[n];
            if(thread->active)ready&=InterlockedCompareExchange(&thread->hot_ready,0,0)!=0;
        }
        ReleaseSRWLockShared(&g_checkpoint_lock);if(ready)break;Sleep(1);
    }
    for(unsigned i=0;i<prepared->count;i++) if(!prepared->entries[i].existing)
        prepared->entries[i].record=checkpoint_new_thread();
    prepared->quiesced=1;return 1;
}
int xbox_CheckpointApplyThreads(void *opaque,XboxCheckpointRoot *roots,unsigned *loaded) {
    if(loaded)*loaded=0;CheckpointThreadsPrepared *prepared=opaque;
    if(!prepared || !prepared->quiesced || prepared->applied || !t_checkpoint_controller || !roots || !loaded)return 0;
    for(unsigned i=0;i<prepared->count;i++) {
        GuestCheckpointThread *thread=prepared->entries[i].record;
        if(thread->packet)fclose(thread->packet);thread->packet=prepared->entries[i].packet;
        if(prepared->entries[i].existing) {
            InterlockedExchange(&thread->hot_ready,0);thread->hot_command=2;
            if(!SetEvent(thread->restore_control))recomp_cont_invalid();
        } else prepared->entries[i].start->checkpoint_record=thread;
    }
    prepared->batch.commit=1;SetEvent(prepared->batch.start);
    for(;;) {
        int ready=1;AcquireSRWLockShared(&g_checkpoint_lock);
        for(unsigned i=0;i<prepared->count;i++) {
            GuestCheckpointThread *thread=prepared->entries[i].record;
            ready&=prepared->entries[i].existing?thread->hot_loaded:thread->parked;
        }
        ReleaseSRWLockShared(&g_checkpoint_lock);if(ready)break;Sleep(1);
    }
    for(unsigned i=0;i<prepared->count;i++) {
        uint32_t *identity=prepared->entries[i].identity;
        roots[i]=(XboxCheckpointRoot){identity[0],identity[1],identity[2],identity[3],prepared->entries[i].handle};
        prepared->entries[i].packet=NULL;
    }
    prepared->applied=1;*loaded=prepared->count;return 1;
}
int xbox_CheckpointLoadThreads(FILE *in,XboxCheckpointRoot *roots,unsigned capacity,unsigned *loaded) {
    if(loaded)*loaded=0;
    for(unsigned i=0;i<CHECKPOINT_THREADS;i++)if(g_checkpoint_threads[i].active)return 0;
    void *prepared=xbox_CheckpointPrepareThreads(in,capacity);if(!prepared)return 0;
    int ok=xbox_CheckpointQuiesceThreads(prepared) && xbox_CheckpointApplyThreads(prepared,roots,loaded);
    xbox_CheckpointFreeThreads(prepared);return ok;
}
static int checkpoint_record_identity(GuestCheckpointThread *thread,uint32_t *identity) {
    if (!thread->packet) return 0;
    long position=ftell(thread->packet);rewind(thread->packet);
    int ok=fread(identity,16,1,thread->packet)==1;
    return fseek(thread->packet,position,SEEK_SET)==0 && ok;
}
int xbox_CheckpointThreadIdentity(DWORD host_id,uint32_t *identity) {
    if (!t_checkpoint_controller) return 0;
    for (unsigned i=0;i<CHECKPOINT_THREADS;i++)
        if (g_checkpoint_threads[i].active && g_checkpoint_threads[i].host_id==host_id)
            return checkpoint_record_identity(&g_checkpoint_threads[i],identity);
    return 0;
}
static GuestCheckpointThread *checkpoint_owner_thread(const uint32_t *identity) {
    uint32_t current[4];
    for (unsigned i=0;i<CHECKPOINT_THREADS;i++) {
        GuestCheckpointThread *thread=&g_checkpoint_threads[i];
        if (thread->active && thread->parked && thread->restore_control &&
            checkpoint_record_identity(thread,current) && !memcmp(current,identity,16)) return thread;
    }
    return NULL;
}
static int checkpoint_queue_owner(const uint32_t *identity,HANDLE handle,uint32_t address,uint32_t depth,uint32_t site) {
    GuestCheckpointThread *thread=checkpoint_owner_thread(identity);
    if (!thread) return 0;
    CheckpointOwnership *owners=realloc(thread->owners,(thread->owner_count+1)*sizeof *owners);
    if (!owners) return 0;
    thread->owners=owners;owners[thread->owner_count++]=(CheckpointOwnership){handle,address,depth,site};return 1;
}
static void checkpoint_clear_owner_requests(void) {
    for (unsigned i=0;i<CHECKPOINT_THREADS;i++) {
        free(g_checkpoint_threads[i].owners);g_checkpoint_threads[i].owners=NULL;
        g_checkpoint_threads[i].owner_count=0;g_checkpoint_threads[i].owner_done=0;
        g_checkpoint_threads[i].owner_release=0;
    }
}
static void checkpoint_restore_owners(void) {
    for (unsigned i=0;i<CHECKPOINT_THREADS;i++) if (g_checkpoint_threads[i].owner_count)
        if (!SetEvent(g_checkpoint_threads[i].restore_control)) recomp_cont_invalid();
    for (;;) {
        int complete=1;
        for (unsigned i=0;i<CHECKPOINT_THREADS;i++) if (g_checkpoint_threads[i].owner_count)
            complete&=InterlockedCompareExchange(&g_checkpoint_threads[i].owner_done,0,0)!=0;
        if (complete) break;Sleep(1);
    }
    checkpoint_clear_owner_requests();
}
#ifdef _WIN32
/* Native query layouts from phnt/ntobapi.h and ntexapi.h. Queries describe the
 * original Windows objects without consuming an auto event or semaphore. */
typedef NTSTATUS (NTAPI *CheckpointQuery)(HANDLE,ULONG,void *,ULONG,ULONG *);
typedef NTSTATUS (NTAPI *CheckpointCompare)(HANDLE,HANDLE);
static struct { CheckpointQuery object,event,semaphore,mutant;CheckpointCompare compare; } checkpoint_native;
static int checkpoint_native_init(void) {
    HMODULE module=GetModuleHandleA("ntdll.dll");
    checkpoint_native.object=(CheckpointQuery)GetProcAddress(module,"NtQueryObject");
    checkpoint_native.event=(CheckpointQuery)GetProcAddress(module,"NtQueryEvent");
    checkpoint_native.semaphore=(CheckpointQuery)GetProcAddress(module,"NtQuerySemaphore");
    checkpoint_native.mutant=(CheckpointQuery)GetProcAddress(module,"NtQueryMutant");
    checkpoint_native.compare=(CheckpointCompare)GetProcAddress(module,"NtCompareObjects");
    return checkpoint_native.object && checkpoint_native.event && checkpoint_native.semaphore &&
           checkpoint_native.mutant && checkpoint_native.compare;
}
/* Reuse the existing non-consuming native query used by checkpoint capture. */

static uint32_t ke_completion_for_handle(HANDLE handle)
{
    if(!handle || (!checkpoint_native.compare && !checkpoint_native_init()))return 0;
    ke_shadow_init();EnterCriticalSection(&g_ke_shadow_cs);uint32_t queue=0;
    for(unsigned i=0;i<KE_SHADOW_SIZE;i++)if(g_ke_shadow[i].in_use &&
        BRIDGE_MEM8(g_ke_shadow[i].guest_va)==4 && checkpoint_native.compare(handle,g_ke_shadow[i].host_handle)==0) {
        queue=g_ke_shadow[i].guest_va;break;
    }
    LeaveCriticalSection(&g_ke_shadow_cs);return queue;
}
static void ke_mark_owned_queue(uint32_t queue)
{
    EnterCriticalSection(&g_ke_shadow_cs);
    for(unsigned i=0;i<KE_SHADOW_SIZE;i++)if(g_ke_shadow[i].in_use && g_ke_shadow[i].guest_va==queue) {
        g_ke_shadow[i].in_use=3;g_ke_shadow[i].object_type=XBOX_KERNEL_DATA_BASE+KDATA_IO_COMPLETION_TYPE;
        g_ke_shadow[i].allocation=queue;
    }
    LeaveCriticalSection(&g_ke_shadow_cs);
}
static void ke_maybe_release_queue(uint32_t queue)
{
    if(!queue)return;
    ke_unlink_unused_name(queue);
    if(BRIDGE_MEM32(queue+0x20)!=queue+0x20)return;
    HANDLE event=NULL;int owned=0;
    EnterCriticalSection(&g_ke_shadow_cs);
    for(unsigned i=0;i<KE_SHADOW_SIZE;i++)if(g_ke_shadow[i].in_use==3 && g_ke_shadow[i].guest_va==queue) {
        if(g_ke_shadow[i].references || (g_ke_shadow[i].name_address && (g_ke_shadow[i].name_attributes&0x10)))break;
        event=g_ke_shadow[i].host_handle;owned=1;break;
    }
    LeaveCriticalSection(&g_ke_shadow_cs);if(!owned)return;
    if(!checkpoint_native.compare && !checkpoint_native_init())recomp_cont_invalid();
    struct {ULONG values[12];LARGE_INTEGER creation;} basic;
    if(checkpoint_native.object(event,0,&basic,sizeof basic,NULL)<0)recomp_cont_invalid();
    if(basic.values[2]>1)return;
    uint32_t head=queue+0x10;
    while(BRIDGE_MEM32(head)!=head) {
        uint32_t entry=BRIDGE_MEM32(head);ke_list_unlink(entry);
        if(BRIDGE_MEM32(entry+8))xbox_HeapFree(entry);else xbox_IoFreeIrp(XBOX_TO_NATIVE(entry-0x54));
    }
    ke_shadow_remove(queue);xbox_HeapFree(queue);
}

static uint32_t ke_native_event_state(HANDLE event) {
    struct { LONG type, state; } value;
    if ((!checkpoint_native.event && !checkpoint_native_init()) ||
        checkpoint_native.event(event,0,&value,sizeof value,NULL)<0) {
        fprintf(stderr,"[STOP] cannot query registered guest event state\n");
        exit(5);
    }
    return value.state != 0;
}
static uint32_t ke_change_event(uint32_t object, HANDLE event, unsigned operation)
{
    LONG previous;
    NTSTATUS status;
    if(object>=0x38 && io_is_guest_file(object-0x38) && ke_shadow_lookup(object-0x38)==event) {
        /* This is the embedded kernel event, not an Nt event-handle request. */
        extern NTSTATUS NTAPI NtSetEvent(HANDLE,PLONG);
        extern NTSTATUS NTAPI NtResetEvent(HANDLE,PLONG);
        extern NTSTATUS NTAPI NtPulseEvent(HANDLE,PLONG);
        status=operation==0?NtSetEvent(event,&previous):operation==1?NtResetEvent(event,&previous):NtPulseEvent(event,&previous);
    } else status = operation == 0 ? xbox_NtSetEvent(event, &previous) :
                      operation == 1 ? xbox_NtResetEvent(event, &previous) :
                                       xbox_NtPulseEvent(event, &previous);
    if (status < 0) {
        fprintf(stderr,"[STOP] registered guest event operation failed: %08X\n",(unsigned)status);
        exit(5);
    }
    BRIDGE_MEM32(object + 4) = ke_native_event_state(event);
    return (uint32_t)previous;
}
enum { CHECKPOINT_EVENT=1,CHECKPOINT_SEMAPHORE,CHECKPOINT_MUTANT,CHECKPOINT_FILE,CHECKPOINT_THREAD,CHECKPOINT_DIRECTORY,CHECKPOINT_SYMLINK };
typedef struct CheckpointObject {
    uint32_t kind,type;int32_t value,limit;
    uint32_t owner[4],depth;
    XboxFileOpenState file;
} CheckpointObject;
typedef struct CheckpointObjectRef {
    uint32_t space,index,address,flag,group,access,attributes,async;
    uint32_t object_type,references,allocation;
    uint32_t name_parent,name_address,name_length,name_attributes,namespace_root,private_handle,path_rule;
} CheckpointObjectRef;
static int checkpoint_query_basic(HANDLE handle,CheckpointObjectRef *ref,uint32_t *kind) {
    struct { ULONG values[12];LARGE_INTEGER creation; } basic;
    union { uint64_t align;unsigned char bytes[2048]; } buffer;
    struct TypeName { USHORT length,maximum;WCHAR *text; } *name=(void *)buffer.bytes;
    if (checkpoint_native.object(handle,0,&basic,sizeof basic,NULL)<0 ||
        checkpoint_native.object(handle,2,&buffer,sizeof buffer,NULL)<0 ||
        (uintptr_t)name->text<(uintptr_t)buffer.bytes ||
        (uintptr_t)name->text+name->length>(uintptr_t)buffer.bytes+sizeof buffer || (name->length&1)) return 0;
    ref->access=basic.values[1];ref->attributes=basic.values[0];*kind=0;
    const WCHAR *names[]={L"Event",L"Semaphore",L"Mutant",L"File",L"Thread",L"Directory",L"SymbolicLink"};
    for (unsigned i=0;i<7;i++) if (name->length==wcslen(names[i])*sizeof(WCHAR) &&
                                   !memcmp(name->text,names[i],name->length)) *kind=i+1;
    return *kind!=0;
}

/* A handle and an Ob reference name the same native object. The existing Ke
 * shadow owns one native handle while any guest handle/reference needs the
 * guest body; no second object registry or synchronization state is created. */
static uint32_t ke_object_type_for_kind(uint32_t kind) {
    static const uint32_t offsets[]={0,KDATA_EVENT_OBJ_TYPE,KDATA_SEMAPHORE_OBJ_TYPE,
        KDATA_MUTANT_OBJ_TYPE,KDATA_FILE_OBJ_TYPE,KDATA_THREAD_OBJ_TYPE,KDATA_OBJ_DIR_TYPE,KDATA_OBJ_SYM_LINK_TYPE};
    return kind>0 && kind<=CHECKPOINT_SYMLINK?XBOX_KERNEL_DATA_BASE+offsets[kind]:0;
}
static KE_SHADOW_SLOT *ke_object_slot(uint32_t address) {
    for(unsigned i=0;i<KE_SHADOW_SIZE;i++)
        if(g_ke_shadow[i].in_use && g_ke_shadow[i].guest_va==address)return &g_ke_shadow[i];
    return NULL;
}
static int io_is_guest_file(uint32_t file) {
    KE_SHADOW_SLOT *slot=ke_object_slot(file);
    return slot && slot->in_use==4 && slot->object_type==XBOX_KERNEL_DATA_BASE+KDATA_FILE_OBJ_TYPE;
}
static uint32_t ke_object_for_handle(HANDLE handle) {
    if(!handle || handle==INVALID_HANDLE_VALUE)return 0;
    if(!checkpoint_native_init())recomp_cont_invalid();
    ke_shadow_init();uint32_t address=0;
    EnterCriticalSection(&g_ke_shadow_cs);
    for(unsigned i=0;i<KE_SHADOW_SIZE;i++)if(g_ke_shadow[i].in_use && g_ke_shadow[i].object_type &&
        checkpoint_native.compare(handle,g_ke_shadow[i].host_handle)==0) {
        address=g_ke_shadow[i].guest_va;break;
    }
    LeaveCriticalSection(&g_ke_shadow_cs);return address;
}
/* The native NT operation checks native object types itself. Only the
 * existing event-backed guest timers need a logical-type check here; do not
 * compare every event/file/thread identity for each ordinary signal. */
NTSTATUS xbox_kernel_reject_timer_signal(HANDLE handle) {
    NTSTATUS status=0;ke_shadow_init();EnterCriticalSection(&g_ke_shadow_cs);
    for(unsigned i=0;i<KE_SHADOW_SIZE;i++) {
        KE_SHADOW_SLOT *slot=&g_ke_shadow[i];
        if(!slot->in_use || (slot->object_type!=XBOX_KERNEL_DATA_BASE+KDATA_TIMER_OBJ_TYPE &&
            !(slot->in_use==4 && slot->object_type==XBOX_KERNEL_DATA_BASE+KDATA_FILE_OBJ_TYPE)))continue;
        if(!checkpoint_native_init())recomp_cont_invalid();
        if(checkpoint_native.compare(handle,slot->host_handle)==0){status=(NTSTATUS)0xc0000024u;break;}
    }
    LeaveCriticalSection(&g_ke_shadow_cs);return status;
}
static int ke_register_thread_object(HANDLE handle,uint32_t thread,uint32_t tib) {
    if(!checkpoint_native_init())recomp_cont_invalid();
    HANDLE owned=NULL;
    if(!DuplicateHandle(GetCurrentProcess(),handle,GetCurrentProcess(),&owned,0,FALSE,DUPLICATE_SAME_ACCESS))return 0;
    if(!ke_shadow_insert(thread,owned))return 0;
    EnterCriticalSection(&g_ke_shadow_cs);
    KE_SHADOW_SLOT *slot=ke_object_slot(thread);
    slot->object_type=ke_object_type_for_kind(CHECKPOINT_THREAD);slot->allocation=tib;
    LeaveCriticalSection(&g_ke_shadow_cs);return 1;
}
static void ke_release_object_if_unused(uint32_t address) {
    uint32_t allocation=0,type=0,path_rule=0,phase=0;HANDLE handle=NULL;
    RecompContFrame frame;
    const RecompContLocal locals[]={{"address",&address,4},{"allocation",&allocation,4},
        {"type",&type,4},{"path_rule",&path_rule,4},{"phase",&phase,4}};
    int tracked=recomp_cont_active();
    uint64_t resume=tracked?kernel_native_cont_enter(&frame,KERNEL_CONT_OBJECT_RELEASE,locals,5):0;
    if(resume) {
        if(phase==1)goto close_native;
        if(phase==2)goto release_link;
        if(phase==3)goto release_physical;
        if(phase==4)goto delete_file;
        recomp_cont_invalid();
    }
    if(!address)goto done;
    ke_unlink_unused_name(address);
    EnterCriticalSection(&g_ke_shadow_cs);
    KE_SHADOW_SLOT *slot=ke_object_slot(address);
    if(slot && slot->object_type==XBOX_KERNEL_DATA_BASE+KDATA_IO_COMPLETION_TYPE) {
        LeaveCriticalSection(&g_ke_shadow_cs);ke_maybe_release_queue(address);goto done;
    }
    if(!slot || !slot->object_type || slot->references || slot->namespace_root ||
       (slot->name_address && (slot->name_attributes&0x10))) {LeaveCriticalSection(&g_ke_shadow_cs);goto done;}
    if(slot->object_type==ke_object_type_for_kind(CHECKPOINT_THREAD) && BRIDGE_MEM8(address+0x2c)!=4) {
        LeaveCriticalSection(&g_ke_shadow_cs);goto done;
    }
    struct {ULONG values[12];LARGE_INTEGER creation;} basic;
    if(checkpoint_native.object(slot->host_handle,0,&basic,sizeof basic,NULL)<0)recomp_cont_invalid();
    if(basic.values[2]>1) {LeaveCriticalSection(&g_ke_shadow_cs);goto done;}
    allocation=slot->allocation;type=slot->object_type;path_rule=slot->path_rule;
    if(type==ke_object_type_for_kind(CHECKPOINT_FILE)) {
        LeaveCriticalSection(&g_ke_shadow_cs);phase=4;
delete_file:
        io_delete_file(address);
        EnterCriticalSection(&g_ke_shadow_cs);slot=ke_object_slot(address);
        if(!slot || slot->references)recomp_cont_invalid();
    }
    handle=slot->host_handle;memset(slot,0,sizeof *slot);
    LeaveCriticalSection(&g_ke_shadow_cs);
    phase=1;
close_native:
    if(xbox_NtClose(handle)<0)recomp_cont_invalid();
    if(type==XBOX_KERNEL_DATA_BASE+KDATA_TIMER_OBJ_TYPE)xbox_kernel_cancel_timer(address);
    if(type==XBOX_KERNEL_DATA_BASE+KDATA_OBJ_SYM_LINK_TYPE) {
        xbox_ReleaseSymbolicLink(address);phase=2;
release_link:
        ke_dereference_object(BRIDGE_MEM32(address));
    }
    if(type==ke_object_type_for_kind(CHECKPOINT_FILE)) {
        ke_shadow_remove(address+0x28);ke_shadow_remove(address+0x38);
    }
    if(type==XBOX_KERNEL_DATA_BASE+KDATA_IO_DEVICE_TYPE) {
        ke_shadow_remove(address+0x34);
        if(path_rule && (BRIDGE_MEM8(address+0x1c)==3 || BRIDGE_MEM8(address+0x1c)==8)) {
            uint32_t physical=BRIDGE_MEM32(BRIDGE_MEM32(address+0x18));
            if(physical) {
                if(BRIDGE_MEM32(physical+0xc)==address)BRIDGE_MEM32(physical+0xc)=0;
                phase=3;
release_physical:
                ke_dereference_object(BRIDGE_MEM32(BRIDGE_MEM32(address+0x18)));
            }
        }
    }
    if(allocation)xbox_HeapFree(allocation);
done:
    if(tracked)recomp_cont_leave(&frame);
}
static NTSTATUS ke_reference_pointer(uint32_t address,uint32_t expected) {
    NTSTATUS status=0;EnterCriticalSection(&g_ke_shadow_cs);
    KE_SHADOW_SLOT *slot=ke_object_slot(address);
    if(!slot || !slot->object_type)status=(NTSTATUS)0xc0000008u;
    else if(expected && slot->object_type!=expected)status=(NTSTATUS)0xc0000024u;
    else {if(slot->references==UINT32_MAX)recomp_cont_invalid();++slot->references;}
    LeaveCriticalSection(&g_ke_shadow_cs);return status;
}
static void ke_dereference_object(uint32_t address) {
    RecompContFrame frame;const RecompContLocal locals[]={{"address",&address,4}};
    int tracked=recomp_cont_active();
    uint64_t resume=tracked?kernel_native_cont_enter(&frame,KERNEL_CONT_DEREFERENCE,locals,1):0;
    if(!resume) {
        EnterCriticalSection(&g_ke_shadow_cs);KE_SHADOW_SLOT *slot=ke_object_slot(address);
        if(!slot || !slot->object_type || !slot->references)recomp_cont_invalid();
        --slot->references;LeaveCriticalSection(&g_ke_shadow_cs);
    }
    ke_release_object_if_unused(address);
    if(tracked)recomp_cont_leave(&frame);
}
static NTSTATUS ke_reference_handle(HANDLE handle,uint32_t expected,uint32_t *output) {
    *output=0;
    if(!checkpoint_native_init())recomp_cont_invalid();
    CheckpointObjectRef ref={0};uint32_t kind;
    if(!checkpoint_query_basic(handle,&ref,&kind))return (NTSTATUS)0xc0000008u;
    uint32_t address=ke_object_for_handle(handle);
    uint32_t type=address?ke_object_slot(address)->object_type:ke_object_type_for_kind(kind);
    if(expected && type!=expected)return (NTSTATUS)0xc0000024u;
    if(!address && kind==CHECKPOINT_THREAD) {
        address=ke_thread_for_handle(handle);
        if(!address)return (NTSTATUS)0xc0000008u;
        if(!ke_register_thread_object(handle,address,0))return (NTSTATUS)0xc000009au;
    }
    if(!address) {
        uint32_t bytes=kind==CHECKPOINT_DIRECTORY?0x2c:kind==CHECKPOINT_FILE?0x48:kind==CHECKPOINT_MUTANT?0x20:kind==CHECKPOINT_SEMAPHORE?0x14:0x10;
        address=xbox_HeapAlloc(bytes,16);if(!address)return (NTSTATUS)0xc000009au;
        memset(XBOX_TO_NATIVE(address),0,bytes);
        HANDLE owned=NULL;
        NTSTATUS status=xbox_NtDuplicateObject(handle,&owned,0);
        if(status<0 || !ke_shadow_insert(address,owned)) {xbox_HeapFree(address);return status<0?status:(NTSTATUS)0xc000009au;}
        KE_SHADOW_SLOT *slot=ke_object_slot(address);slot->object_type=type;slot->allocation=address;
        if(kind==CHECKPOINT_FILE) {
            XboxFileOpenState file;
            if(!xbox_CaptureFileOpenState(owned,&file))recomp_cont_invalid();
            BRIDGE_MEM16(address)=5;BRIDGE_MEM32(address+4)=file.device;
            BRIDGE_MEM8(address+2)=((file.access&(GENERIC_READ|FILE_READ_DATA))?2:0)|
                ((file.access&(GENERIC_WRITE|FILE_WRITE_DATA))?4:0)|((file.access&DELETE)?8:0)|
                ((file.share&FILE_SHARE_READ)?16:0)|((file.share&FILE_SHARE_WRITE)?32:0)|((file.share&FILE_SHARE_DELETE)?64:0);
            /* FILE_OBJECT flags describe the guest create options, not the
             * synchronous implementation detail of its host file handle. */
            BRIDGE_MEM8(address+3)=((file.options&0x30)?1:0)|((file.options&0x10)?2:0)|
                ((file.options&8)?4:0)|((file.options&4)?8:0)|((file.options&0x800)?0x40:0);
            BRIDGE_MEM32(address+0x14)=(uint32_t)file.position;
            BRIDGE_MEM32(address+0x18)=(uint32_t)((uint64_t)file.position>>32);
            if(file.options&0x30) {
                BRIDGE_MEM32(address+0x24)=UINT32_MAX;
                if(!ke_initialize_event(address+0x28,1,0))recomp_cont_invalid();
            }
            if(!ke_initialize_event(address+0x38,0,0))recomp_cont_invalid();
        } else if(kind!=CHECKPOINT_DIRECTORY) {
            BRIDGE_MEM8(address+2)=(uint8_t)(bytes/4);
            BRIDGE_MEM32(address+8)=BRIDGE_MEM32(address+12)=address+8;
            if(kind==CHECKPOINT_EVENT) {
                struct {LONG type,state;} state;
                if(checkpoint_native.event(owned,0,&state,sizeof state,NULL)<0)recomp_cont_invalid();
                BRIDGE_MEM8(address)=(uint8_t)state.type;BRIDGE_MEM32(address+4)=state.state;
            } else if(kind==CHECKPOINT_SEMAPHORE) {
                struct {LONG count,maximum;} state;
                if(checkpoint_native.semaphore(owned,0,&state,sizeof state,NULL)<0)recomp_cont_invalid();
                BRIDGE_MEM8(address)=5;BRIDGE_MEM32(address+4)=state.count;BRIDGE_MEM32(address+0x10)=state.maximum;
            } else {
                struct {LONG count;BOOLEAN owned,abandoned;} state;
                if(checkpoint_native.mutant(owned,0,&state,sizeof state,NULL)<0)recomp_cont_invalid();
                BRIDGE_MEM8(address)=2;BRIDGE_MEM32(address+4)=state.count;BRIDGE_MEM8(address+0x1c)=state.abandoned;
                if(state.owned)BRIDGE_MEM32(address+0x18)=ke_current_thread();
            }
        }
    }
    NTSTATUS status=ke_reference_pointer(address,expected);if(status>=0)*output=address;return status;
}
static NTSTATUS ke_create_timer_object(PHANDLE output,uint32_t type) {
    if(!output || type>1)return (NTSTATUS)0xc000000du;
    uint32_t timer=xbox_HeapAlloc(0x80,16);if(!timer)return (NTSTATUS)0xc000009au;
    memset(XBOX_TO_NATIVE(timer),0,0x80);
    if(!ke_initialize_timer(timer,type)){xbox_HeapFree(timer);return (NTSTATUS)0xc000009au;}
    KE_SHADOW_SLOT *slot=ke_object_slot(timer);
    slot->object_type=XBOX_KERNEL_DATA_BASE+KDATA_TIMER_OBJ_TYPE;slot->allocation=timer;
    BRIDGE_MEM16(timer+0x50)=0x13;
    BRIDGE_MEM32(timer+0x50+12)=KERNEL_TIMER_DPC;BRIDGE_MEM32(timer+0x50+16)=timer;
    HANDLE alias=NULL;NTSTATUS status=xbox_NtDuplicateObject(slot->host_handle,&alias,0);
    if(status<0){ke_release_object_if_unused(timer);return status;}
    *output=alias;return 0;
}

/* Names are metadata of the original object identity. Native objects stay
 * anonymous: Xbox directory handles and counted ANSI components have their
 * own namespace, independent of the host session's named-object directories. */
static NTSTATUS ke_native_directory(PHANDLE output) {
    if(!checkpoint_native_init())recomp_cont_invalid();
    struct {ULONG length;HANDLE root;void *name;ULONG attributes;void *security,*quality;} attributes={0};
    extern NTSTATUS NTAPI NtCreateDirectoryObject(PHANDLE,ULONG,void *);
    attributes.length=sizeof attributes;
    return NtCreateDirectoryObject(output,0x000f000fu,&attributes);
}
/* Call under the object lock. Shadow ownership and saved wait aliases are
 * private references, not handles visible to the original object manager. */
static uint32_t ke_public_handle_count(const KE_SHADOW_SLOT *slot) {
    struct {ULONG values[12];LARGE_INTEGER creation;} basic;
    if(checkpoint_native.object(slot->host_handle,0,&basic,sizeof basic,NULL)<0)recomp_cont_invalid();
    uint32_t private_handles=1;
    for(unsigned i=1;i<BRIDGE_HANDLE_MAX;i++)if(s_handle_table[i] && s_handle_private[i] &&
        checkpoint_native.compare(slot->host_handle,s_handle_table[i])==0)++private_handles;
    if(basic.values[2]<private_handles)recomp_cont_invalid();
    return basic.values[2]-private_handles;
}
static void ke_unlink_unused_name(uint32_t address) {
    if(!g_ke_shadow_init)return;
    EnterCriticalSection(&g_ke_shadow_cs);KE_SHADOW_SLOT *slot=ke_object_slot(address);
    if(!slot || !slot->name_address || slot->namespace_root || (slot->name_attributes&0x10)) {
        LeaveCriticalSection(&g_ke_shadow_cs);return;
    }
    if(ke_public_handle_count(slot)){LeaveCriticalSection(&g_ke_shadow_cs);return;}
    uint32_t name=slot->name_address,parent=slot->name_parent;
    if(slot->object_type==XBOX_KERNEL_DATA_BASE+KDATA_OBJ_SYM_LINK_TYPE)xbox_UnlinkSymbolicLink(address);
    slot->name_parent=slot->name_address=slot->name_length=slot->name_attributes=0;
    LeaveCriticalSection(&g_ke_shadow_cs);
    xbox_HeapFree(name);if(parent)ke_dereference_object(parent);
}
static NTSTATUS ke_attach_name(uint32_t object,uint32_t parent,const XBOX_ANSI_STRING *name,uint32_t attributes) {
    uint32_t storage=xbox_HeapAlloc((uint32_t)name->Length+1,1);
    if(!storage)return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status=ke_reference_pointer(parent,XBOX_KERNEL_DATA_BASE+KDATA_OBJ_DIR_TYPE);
    if(status<0){xbox_HeapFree(storage);return status;}
    memcpy(XBOX_TO_NATIVE(storage),name->Buffer,name->Length);BRIDGE_MEM8(storage+name->Length)=0;
    KE_SHADOW_SLOT *slot=ke_object_slot(object);
    slot->name_parent=parent;slot->name_address=storage;slot->name_length=name->Length;slot->name_attributes=attributes;
    return 0;
}
static NTSTATUS ke_namespace_root(uint32_t root,uint32_t *address) {
    for(unsigned i=0;i<KE_SHADOW_SIZE;i++)if(g_ke_shadow[i].in_use && g_ke_shadow[i].namespace_root==root) {
        *address=g_ke_shadow[i].guest_va;return 0;
    }
    uint32_t parent=0;
    NTSTATUS status=root==1?0:ke_namespace_root(1,&parent);if(status<0)return status;
    HANDLE handle=NULL;status=ke_native_directory(&handle);if(status<0)return status;
    uint32_t object=0;status=ke_reference_handle(handle,XBOX_KERNEL_DATA_BASE+KDATA_OBJ_DIR_TYPE,&object);
    if(status>=0 && parent) {
        char *text=root==2?"??":root==3?"Win32NamedObjects":"Device";
        XBOX_ANSI_STRING name={(USHORT)strlen(text),(USHORT)strlen(text),text};
        status=ke_attach_name(object,parent,&name,0x10);
    }
    if(status>=0){ke_object_slot(object)->namespace_root=root;*address=object;}
    xbox_NtClose(handle);if(object)ke_dereference_object(object);return status;
}
static uint32_t ke_named_child(uint32_t parent,const XBOX_ANSI_STRING *name) {
    for(unsigned i=0;i<KE_SHADOW_SIZE;i++) {
        KE_SHADOW_SLOT *slot=&g_ke_shadow[i];
        if(!slot->in_use || slot->name_parent!=parent || slot->name_length!=name->Length)continue;
        XBOX_ANSI_STRING stored={(USHORT)slot->name_length,(USHORT)slot->name_length,XBOX_TO_NATIVE(slot->name_address)};
        if(xbox_RtlEqualString(&stored,(PXBOX_ANSI_STRING)name,TRUE))return slot->guest_va;
    }
    return 0;
}
/* On a final missing component return its parent and counted leaf for insert.
 * Intermediate components must already be directory objects. */
static NTSTATUS ke_resolve_name(PXBOX_OBJECT_ATTRIBUTES oa,uint32_t expected,uint32_t *object,uint32_t *parent,XBOX_ANSI_STRING *leaf) {
    *object=*parent=0;memset(leaf,0,sizeof *leaf);
    if(!oa)return (NTSTATUS)0xc0000033u;
    XBOX_ANSI_STRING *name=oa->ObjectName;uint32_t directory=0;
    uint32_t length=name?name->Length:0,start=0;
    if(length && !name->Buffer)return STATUS_INVALID_PARAMETER;
    NTSTATUS status=0;
    if(oa->RootDirectory) {
        if(length && name->Buffer[0]=='\\')return (NTSTATUS)0xc0000033u;
        if(oa->RootDirectory==(HANDLE)(intptr_t)-3 || oa->RootDirectory==(HANDLE)(intptr_t)-4)
            status=ke_namespace_root(oa->RootDirectory==(HANDLE)(intptr_t)-3?2:3,&directory);
        else {
            directory=ke_object_for_handle(oa->RootDirectory);
            if(!directory)return STATUS_INVALID_HANDLE;
            if(ke_object_slot(directory)->object_type!=XBOX_KERNEL_DATA_BASE+KDATA_OBJ_DIR_TYPE)return (NTSTATUS)0xc0000024u;
        }
        if(status<0)return status;
    } else {
        if(!length || name->Buffer[0]!='\\')return (NTSTATUS)0xc0000033u;
        status=ke_namespace_root(1,&directory);if(status<0)return status;
        /* These predefined children must also be visible from the root path. */
        uint32_t child;status=ke_namespace_root(2,&child);if(status>=0)status=ke_namespace_root(3,&child);
        if(status>=0)status=ke_namespace_root(4,&child);
        if(status<0)return status;start=1;
    }
    if(start==length){*object=directory;return 0;}
    while(start<length) {
        uint32_t end=start;while(end<length && name->Buffer[end]!='\\')++end;
        if(end==start || (end<length && end+1==length))return (NTSTATUS)0xc0000033u;
        XBOX_ANSI_STRING component={(USHORT)(end-start),(USHORT)(end-start),name->Buffer+start};
        uint32_t child=ke_named_child(directory,&component);
        if(!child) {
            if(end!=length)return (NTSTATUS)0xc000003au;
            *parent=directory;*leaf=component;return STATUS_OBJECT_NAME_NOT_FOUND;
        }
        if(end!=length || expected!=XBOX_KERNEL_DATA_BASE+KDATA_OBJ_SYM_LINK_TYPE) {
            while(ke_object_slot(child)->object_type==XBOX_KERNEL_DATA_BASE+KDATA_OBJ_SYM_LINK_TYPE)
                child=BRIDGE_MEM32(child);
        }
        if(end==length){*object=child;return 0;}
        if(ke_object_slot(child)->object_type==XBOX_KERNEL_DATA_BASE+KDATA_FILE_OBJ_TYPE ||
           ke_object_slot(child)->object_type==XBOX_KERNEL_DATA_BASE+KDATA_IO_DEVICE_TYPE) {
            *object=child;*leaf=(XBOX_ANSI_STRING){(USHORT)(length-end-1),(USHORT)(length-end-1),name->Buffer+end+1};
            return (NTSTATUS)0xc0000024u;
        }
        if(ke_object_slot(child)->object_type!=XBOX_KERNEL_DATA_BASE+KDATA_OBJ_DIR_TYPE)return (NTSTATUS)0xc000003au;
        directory=child;start=end+1;
    }
    return (NTSTATUS)0xc0000033u;
}

static NTSTATUS ke_native_symbolic(PHANDLE output,uint32_t target) {
    struct {ULONG length;HANDLE root;void *name;ULONG attributes;void *security,*quality;} attributes={0};
    struct {USHORT length,maximum;WCHAR *text;} value;
    extern NTSTATUS NTAPI NtCreateSymbolicLinkObject(PHANDLE,ULONG,void *,void *);
    /* Native handles carry identity and type. Guest lookup/query consume the
     * resolved target and original ANSI string in the existing guest body.
     * Encoding that identity here avoids imposing the host UTF-16 length cap
     * on a valid counted ANSI name; the host never parses a guest target. */
    WCHAR text[32];int length=swprintf_s(text,32,L"\\XboxGuestObject\\%08X",target);
    attributes.length=sizeof attributes;value.length=value.maximum=(USHORT)(length*sizeof *text);value.text=text;
    return NtCreateSymbolicLinkObject(output,0x000f0001u,&attributes,&value);
}
/* Resolve once, as the original object manager does: the link owns its target,
 * while the counted name remains available to NtQuerySymbolicLinkObject. */
static NTSTATUS ke_reference_link_target(PXBOX_ANSI_STRING target,uint32_t *output) {
    XBOX_OBJECT_ATTRIBUTES oa={NULL,target,0x40};XBOX_ANSI_STRING remaining;uint32_t parent,object;
    NTSTATUS status=ke_resolve_name(&oa,0,&object,&parent,&remaining);
    if(status>=0) {
        status=ke_reference_pointer(object,0);if(status>=0)*output=object;return status;
    }
    if(status==(NTSTATUS)0xc0000024u && object && remaining.Length) {
        if(ke_object_slot(object)->object_type==XBOX_KERNEL_DATA_BASE+KDATA_FILE_OBJ_TYPE) {
            oa.RootDirectory=ke_shadow_lookup(object);oa.ObjectName=&remaining;
        }
    } else {
        /* Device paths are owned by the existing filesystem mapping. */
        if(target->Length<8 || _strnicmp(target->Buffer,"\\Device\\",8))return STATUS_INVALID_PARAMETER;
    }
    HANDLE handle=NULL;XBOX_IO_STATUS_BLOCK ios;
    status=xbox_NtOpenFile(&handle,0,&oa,&ios,7,XBOX_FILE_DIRECTORY_FILE);
    if(status>=0) {
        status=ke_reference_handle(handle,XBOX_KERNEL_DATA_BASE+KDATA_FILE_OBJ_TYPE,output);
        xbox_NtClose(handle);
    }
    return status;
}
NTSTATUS __stdcall xbox_IoCreateSymbolicLink(PXBOX_ANSI_STRING name,PXBOX_ANSI_STRING target) {
    if(!name || !name->Length || !name->Buffer || !target || !target->Length || !target->Buffer || target->Buffer[0]!='\\')
        return STATUS_INVALID_PARAMETER;
    int held=guest_cpu_is_held(),was_thread=t_guest_thread;if(!held)guest_cpu_join();ke_shadow_init();
    XBOX_OBJECT_ATTRIBUTES oa={NULL,name,0x50};XBOX_ANSI_STRING leaf;uint32_t object=0,parent=0;
    uint32_t target_object=0;HANDLE handle=NULL;
    NTSTATUS status=ke_reference_link_target(target,&target_object);if(status<0)goto done;
    status=ke_resolve_name(&oa,XBOX_KERNEL_DATA_BASE+KDATA_OBJ_SYM_LINK_TYPE,&object,&parent,&leaf);
    if(status>=0){status=STATUS_OBJECT_NAME_COLLISION;goto done;}
    if(status!=STATUS_OBJECT_NAME_NOT_FOUND || !parent)goto done;
    status=ke_native_symbolic(&handle,target_object);if(status<0)goto done;
    object=xbox_HeapAlloc(12u+target->Length+1,16);
    if(!object){status=STATUS_INSUFFICIENT_RESOURCES;goto done;}
    memset(XBOX_TO_NATIVE(object),0,12);BRIDGE_MEM16(object+4)=BRIDGE_MEM16(object+6)=target->Length;
    BRIDGE_MEM32(object+8)=object+12;memcpy(XBOX_TO_NATIVE(object+12),target->Buffer,target->Length);
    BRIDGE_MEM8(object+12+target->Length)=0;
    if(!ke_shadow_insert(object,handle)){handle=NULL;xbox_HeapFree(object);status=STATUS_INSUFFICIENT_RESOURCES;goto done;}
    handle=NULL;KE_SHADOW_SLOT *slot=ke_object_slot(object);
    slot->object_type=XBOX_KERNEL_DATA_BASE+KDATA_OBJ_SYM_LINK_TYPE;slot->allocation=object;slot->references=1;
    BRIDGE_MEM32(object)=target_object;target_object=0;
    status=ke_attach_name(object,parent,&leaf,oa.Attributes);
    if(status>=0)status=xbox_RegisterSymbolicLink(object,name);
    if(status<0)slot->name_attributes&=~0x10u;
    ke_dereference_object(object);
done:
    if(handle)xbox_NtClose(handle);
    if(target_object)ke_dereference_object(target_object);
    if(!held){guest_cpu_part();t_guest_thread=was_thread;}return status;
}
NTSTATUS __stdcall xbox_NtOpenSymbolicLinkObject(PHANDLE output,PXBOX_OBJECT_ATTRIBUTES attributes) {
    if(!output)return STATUS_INVALID_PARAMETER;
    int held=guest_cpu_is_held(),was_thread=t_guest_thread;if(!held)guest_cpu_join();ke_shadow_init();
    NTSTATUS status=ke_open_named_object(attributes,XBOX_KERNEL_DATA_BASE+KDATA_OBJ_SYM_LINK_TYPE,output);
    if(!held){guest_cpu_part();t_guest_thread=was_thread;}return status;
}
NTSTATUS __stdcall xbox_IoDeleteSymbolicLink(PXBOX_ANSI_STRING name) {
    int held=guest_cpu_is_held(),was_thread=t_guest_thread;if(!held)guest_cpu_join();ke_shadow_init();
    XBOX_OBJECT_ATTRIBUTES attributes={NULL,name,0x40};HANDLE handle=NULL;
    NTSTATUS status=xbox_NtOpenSymbolicLinkObject(&handle,&attributes);
    if(status>=0) {
        uint32_t object=ke_object_for_handle(handle);xbox_kernel_make_temporary(XBOX_TO_NATIVE(object));
        status=xbox_NtClose(handle);
    }
    if(!held){guest_cpu_part();t_guest_thread=was_thread;}return status;
}
NTSTATUS __stdcall xbox_NtQuerySymbolicLinkObject(HANDLE handle,PXBOX_ANSI_STRING output,PULONG returned) {
    if(!output)return STATUS_INVALID_PARAMETER;
    int held=guest_cpu_is_held(),was_thread=t_guest_thread;if(!held)guest_cpu_join();
    uint32_t object=0;NTSTATUS status=ke_reference_handle(handle,XBOX_KERNEL_DATA_BASE+KDATA_OBJ_SYM_LINK_TYPE,&object);
    if(status>=0) {
        USHORT length=BRIDGE_MEM16(object+4),copied=output->MaximumLength<length?output->MaximumLength:length;
        if(copied && !output->Buffer)status=(NTSTATUS)0xc0000005u;
        else {
            if(copied)memcpy(output->Buffer,XBOX_TO_NATIVE(BRIDGE_MEM32(object+8)),copied);
            output->Length=copied;if(returned)*returned=length;
            if(copied<length)status=(NTSTATUS)0xc0000023u;
        }
        ke_dereference_object(object);
    }
    if(!held){guest_cpu_part();t_guest_thread=was_thread;}return status;
}

/* Guest devices own the original ABI body and its existing dispatcher objects.
 * An anonymous native directory provides only handle identity and lifetime. */
NTSTATUS __stdcall xbox_IoCreateDevice(PVOID driver,ULONG extension,PXBOX_ANSI_STRING name,
    ULONG type,BOOLEAN exclusive,PVOID *output) {
    if(!output)return STATUS_INVALID_PARAMETER;*output=NULL;
    if(extension>UINT32_MAX-XBOX_DEVICE_OBJECT_SIZE-7)return STATUS_INVALID_PARAMETER;
    int held=guest_cpu_is_held(),was_thread=t_guest_thread;if(!held)guest_cpu_join();ke_shadow_init();
    uint32_t object=0,parent=0;XBOX_ANSI_STRING leaf={0};
    XBOX_OBJECT_ATTRIBUTES oa={NULL,name,(name?0x10u:0u)|(exclusive?0x20u:0u)};
    NTSTATUS status=0;
    if(name) {
        status=ke_resolve_name(&oa,XBOX_KERNEL_DATA_BASE+KDATA_IO_DEVICE_TYPE,&object,&parent,&leaf);
        if(status>=0){status=STATUS_OBJECT_NAME_COLLISION;goto done;}
        if(status!=STATUS_OBJECT_NAME_NOT_FOUND || !parent)goto done;
    }
    uint32_t size=XBOX_DEVICE_OBJECT_SIZE+extension,allocation=(size+7)&~7u;
    HANDLE handle=NULL;status=ke_native_directory(&handle);if(status<0)goto done;
    object=xbox_HeapAlloc(allocation,16);
    if(!object){xbox_NtClose(handle);status=STATUS_INSUFFICIENT_RESOURCES;goto done;}
    memset(XBOX_TO_NATIVE(object),0,allocation);
    if(!ke_shadow_insert(object,handle)){xbox_HeapFree(object);status=STATUS_INSUFFICIENT_RESOURCES;goto done;}
    KE_SHADOW_SLOT *slot=ke_object_slot(object);
    slot->object_type=XBOX_KERNEL_DATA_BASE+KDATA_IO_DEVICE_TYPE;slot->allocation=object;slot->references=1;
    BRIDGE_MEM16(object)=3;BRIDGE_MEM16(object+2)=(uint16_t)size;
    int storage=type==2 || type==7 || type==0x3a || type==0x3b;
    BRIDGE_MEM32(object+0xc)=storage?0:object;
    BRIDGE_MEM32(object+0x14)=0x10u|(exclusive?2u:0u)|(name?8u:0u);
    BRIDGE_MEM32(object+0x18)=extension?object+XBOX_DEVICE_OBJECT_SIZE:0;
    BRIDGE_MEM8(object+0x1c)=(uint8_t)type;BRIDGE_MEM8(object+0x1e)=1;
    ke_initialize_device_queue(object+0x28);
    if(storage && !ke_initialize_event(object+0x34,1,1))status=STATUS_INSUFFICIENT_RESOURCES;
    else status=name?ke_attach_name(object,parent,&leaf,oa.Attributes):0;
    if(status<0){slot->name_attributes&=~0x10u;ke_dereference_object(object);goto done;}
    BRIDGE_MEM32(object+8)=io_guest_address(driver);*output=XBOX_TO_NATIVE(object);
done:
    if(!held){guest_cpu_part();t_guest_thread=was_thread;}return status;
}

/* The owner reference is released by the driver's delete callback, or directly
 * when the original driver has no callback. I/O references delay this step. */
static void io_delete_device(uint32_t device,int mark_pending) {
    int held=guest_cpu_is_held(),was_thread=t_guest_thread;uint32_t routine=0;
    RecompContFrame frame;
    const RecompContLocal locals[]={{"device",&device,sizeof device},{"routine",&routine,sizeof routine},
        {"held",&held,sizeof held},{"was_thread",&was_thread,sizeof was_thread}};
    int tracked=recomp_cont_active();
    uint64_t resume=tracked?kernel_native_cont_enter(&frame,KERNEL_CONT_DEVICE_DELETE,locals,4):0;
    if(!guest_cpu_is_held())guest_cpu_join();
    if(resume)goto callback;
    if(mark_pending) {
        if(BRIDGE_MEM32(device+0x14)&8)xbox_kernel_make_temporary(XBOX_TO_NATIVE(device));
        BRIDGE_MEM8(device+0x1f)=1;
    }
    if(BRIDGE_MEM8(device+0x1f) && !BRIDGE_MEM32(device+4)) {
        uint32_t driver=BRIDGE_MEM32(device+8);routine=driver?BRIDGE_MEM32(driver+4):0;
        if(routine) {
callback: ;
            uint32_t args[]={device};ke_call_guest(routine,args,1,0);
        } else ke_dereference_object(device);
    }
    if(tracked)recomp_cont_leave(&frame);
    if(!held){guest_cpu_part();t_guest_thread=was_thread;}
}
VOID __stdcall xbox_IoDeleteDevice(PVOID object) {io_delete_device(io_guest_address(object),1);}

/* The existing native directory filesystem mounts a distinct file-system
 * device, as the original parser does. It owns the physical device reference;
 * file object lifetime owns its I/O count, independently of Ob references. */
static int io_is_native_device(uint32_t device) {
    KE_SHADOW_SLOT *slot=ke_object_slot(device);
    return slot && slot->path_rule && !BRIDGE_MEM32(device+8) &&
        slot->object_type==XBOX_KERNEL_DATA_BASE+KDATA_IO_DEVICE_TYPE;
}
static NTSTATUS io_mount_native_volume(uint32_t physical,uint32_t *mounted) {
    KE_SHADOW_SLOT *slot=ke_object_slot(physical);
    if(!slot || !slot->path_rule)return STATUS_INVALID_DEVICE_REQUEST;
    uint32_t device=BRIDGE_MEM32(physical+0xc);
    if(!device) {
        PVOID body=NULL;
        NTSTATUS status=xbox_IoCreateDevice(NULL,4,NULL,BRIDGE_MEM8(physical+0x1c)==2?3:8,FALSE,&body);
        if(status<0)return status;device=io_guest_address(body);
        ke_object_slot(device)->path_rule=slot->path_rule;
        if(ke_reference_pointer(physical,XBOX_KERNEL_DATA_BASE+KDATA_IO_DEVICE_TYPE)<0)recomp_cont_invalid();
        BRIDGE_MEM32(BRIDGE_MEM32(device+0x18))=physical;
        BRIDGE_MEM8(device+0x1e)=BRIDGE_MEM8(physical+0x1e)+1;
        BRIDGE_MEM32(device+0x24)=BRIDGE_MEM32(physical+0x24);
        BRIDGE_MEM32(device+0x20)=BRIDGE_MEM8(physical+0x1c)==2?2048:512;
        BRIDGE_MEM32(device+0x14)&=~0x10u;BRIDGE_MEM32(physical+0xc)=device;
    }
    *mounted=device;return 0;
}
static NTSTATUS io_validate_create_device(uint32_t device,uint32_t related) {
    if(BRIDGE_MEM8(device+0x1f) || (BRIDGE_MEM32(device+0x14)&0x10u))return (NTSTATUS)0xc000000eu;
    if((BRIDGE_MEM32(device+0x14)&2) && BRIDGE_MEM32(device+4) && !related)return STATUS_ACCESS_DENIED;
    return 0;
}

NTSTATUS xbox_kernel_prepare_file_open(PXBOX_OBJECT_ATTRIBUTES input,WCHAR *path,DWORD capacity,uint32_t *output,int *volume) {
    *output=0;*volume=0;
    if(!input || !input->ObjectName || !input->ObjectName->Buffer)return STATUS_OBJECT_PATH_NOT_FOUND;
    int held=guest_cpu_is_held(),was_thread=t_guest_thread;if(!held)guest_cpu_join();
    XBOX_ANSI_STRING name=*input->ObjectName;XBOX_OBJECT_ATTRIBUTES oa=*input;
    oa.ObjectName=&name;uint32_t device=0,object=0,parent=0;XBOX_ANSI_STRING rest={0};NTSTATUS status=0;
    if(input->RootDirectory && name.Length && name.Buffer[0]!='\\' &&
       !(name.Length>=2 && name.Buffer[1]==':'))device=xbox_FileHandleDevice(input->RootDirectory);
    if(!device) {
        while(name.Length>1 && name.Buffer[name.Length-1]=='\\')--name.Length;
        if(name.Length>=2 && name.Buffer[1]==':')oa.RootDirectory=(HANDLE)(intptr_t)-3;
        else if(name.Length && name.Buffer[0]=='\\')oa.RootDirectory=NULL;
        status=ke_resolve_name(&oa,0,&object,&parent,&rest);
        if(status<0 && !(status==(NTSTATUS)0xc0000024u && object))goto done;
        KE_SHADOW_SLOT *slot=ke_object_slot(object);
        if(slot && slot->object_type==XBOX_KERNEL_DATA_BASE+KDATA_IO_DEVICE_TYPE) {
            device=object;*volume=!rest.Length;
        } else if(slot && slot->object_type==XBOX_KERNEL_DATA_BASE+KDATA_FILE_OBJ_TYPE) {
            device=BRIDGE_MEM32(object+4);
        }
    }
    if(!device){status=STATUS_INVALID_DEVICE_REQUEST;goto done;}
    if((BRIDGE_MEM8(device+0x1c)==3 || BRIDGE_MEM8(device+0x1c)==8) &&
       ke_object_slot(device)->path_rule && !BRIDGE_MEM32(BRIDGE_MEM32(device+0x18))) {
        status=(NTSTATUS)0xc000026eu;goto done;
    }
    status=io_validate_create_device(device,input->RootDirectory!=NULL);if(status<0)goto done;
    if(!xbox_FileObjectPath(input,path,capacity)){status=STATUS_OBJECT_PATH_NOT_FOUND;goto done;}
    if(BRIDGE_MEM8(device+0x1c)==2 || BRIDGE_MEM8(device+0x1c)==7) {
        status=io_mount_native_volume(device,&device);if(status<0)goto done;
    }
    if(!ke_object_slot(device)->path_rule){status=STATUS_INVALID_DEVICE_REQUEST;goto done;}
    if(BRIDGE_MEM32(device+4)==UINT32_MAX){status=STATUS_INSUFFICIENT_RESOURCES;goto done;}
    ++BRIDGE_MEM32(device+4);*output=device;status=0;
done:
    if(!held){guest_cpu_part();t_guest_thread=was_thread;}return status;
}
/* The existing guest namespace selects the actual driver device. Native
 * directory-backed files continue through their original host backend. */
typedef struct GuestCreateFile {
    RecompContFrame frame;
    uint32_t device,file,related,irp,token;
} GuestCreateFile;

/* Transfer abandoned-call ownership before cleanup can enter a driver. The
 * original request completion remains responsible for an in-flight packet. */
static void io_create_release(uint32_t file,uint32_t device,uint32_t related,uint32_t irp,uint32_t token)
{
    uint32_t phase=0;RecompContFrame frame;
    const RecompContLocal locals[]={{"file",&file,4},{"device",&device,4},{"related",&related,4},
        {"irp",&irp,4},{"token",&token,4},{"phase",&phase,4}};
    int tracked=recomp_cont_active();
    uint64_t resume=tracked?kernel_native_cont_enter(&frame,KERNEL_CONT_CREATE_RELEASE,locals,6):0;
    if(resume) {
        if(phase==1)goto close_token;
        if(phase==2)goto release_file;
        if(phase==3)goto release_related;
        if(phase==4)goto release_device;
        recomp_cont_invalid();
    }
    if(irp) {
        if(BRIDGE_MEM16(irp)!=6)recomp_cont_invalid();
        if((int8_t)BRIDGE_MEM8(irp+0x19)<=(int8_t)BRIDGE_MEM8(irp+0x18)+1) {
            /* The driver still owns this request. Its existing completion
             * path retains the only file reference until it actually finishes. */
            if(!file || BRIDGE_MEM32(irp+0x60)!=file)recomp_cont_invalid();
            BRIDGE_MEM8(irp+0x1b)=1;
            BRIDGE_MEM32(irp+4)&=~0x88u;
            BRIDGE_MEM32(file+0x60)=related;
            device=file=related=irp=0;
        } else {
            NTSTATUS status=(NTSTATUS)BRIDGE_MEM32(irp+0x10);
            ke_list_unlink(irp+8);BRIDGE_MEM32(irp+8)=BRIDGE_MEM32(irp+12)=irp+8;
            xbox_IoFreeIrp(XBOX_TO_NATIVE(irp));irp=0;
            if(file && status>=0)device=0;
            else if(file)BRIDGE_MEM32(file+4)=0;
        }
    } else if(file) {
        /* No request was dispatched if allocation failed before its producer. */
        if(device)BRIDGE_MEM32(file+4)=0;
    }
    if(token) {
        phase=1;
close_token:
        if(xbox_NtClose(bridge_resolve_handle(token))<0)recomp_cont_invalid();token=0;
    }
    if(file) {
        phase=2;
release_file:
        ke_dereference_object(file);file=0;
    }
    if(related) {
        phase=3;
release_related:
        ke_dereference_object(related);related=0;
    }
    if(device) {
        phase=4;
release_device:
        xbox_kernel_release_file_device(device);device=0;
    }
    if(tracked)recomp_cont_leave(&frame);
}
static void io_create_finish(GuestCreateFile *call)
{
    uint32_t file=call->file,device=call->device,related=call->related,irp=call->irp,token=call->token;
    call->file=call->device=call->related=call->irp=call->token=0;
    io_create_release(file,device,related,irp,token);
}
static void io_create_unwind(RecompContFrame *frame) {io_create_finish((GuestCreateFile *)frame);}
static void io_create_leave(RecompContFrame *frame) {
    GuestCreateFile *call=(GuestCreateFile *)frame;
    if(frame->retained)return;
    if(!checkpoint_root_cancelled() && (call->file || call->device || call->related || call->irp || call->token))
        io_create_finish(call);
}

NTSTATUS xbox_kernel_create_driver_file(PHANDLE output,ACCESS_MASK access,PXBOX_OBJECT_ATTRIBUTES input,
    PXBOX_IO_STATUS_BLOCK ios,PLARGE_INTEGER allocation,ULONG attributes,ULONG share,ULONG disposition,ULONG options,int *handled)
{
    GuestCreateFile call={0};uint32_t phase=0,information=0;
    NTSTATUS status=0;int held=guest_cpu_is_held(),was_thread=t_guest_thread;
    const RecompContLocal locals[]={{"device",&call.device,4},{"file",&call.file,4},{"related",&call.related,4},
        {"irp",&call.irp,4},{"phase",&phase,4},{"token",&call.token,4},{"information",&information,4},
        {"status",&status,4},{"held",&held,sizeof held},{"was_thread",&was_thread,sizeof was_thread}};
    int tracked=recomp_cont_active();
    uint64_t resume=tracked?kernel_native_cont_enter(&call.frame,KERNEL_CONT_CREATE_FILE,locals,10):0;
    if(tracked){call.frame.on_leave=io_create_leave;call.frame.on_unwind=io_create_unwind;}
    *handled=1;if(!guest_cpu_is_held())guest_cpu_join();
    if(resume) {
        if(phase==1)goto dispatch;
        if(phase==2)goto wait_completion;
        if(phase==3)goto release_file;
        recomp_cont_invalid();
    }
    if(!output || !input || !input->ObjectName || !input->ObjectName->Buffer) {*handled=0;goto done;}
    XBOX_ANSI_STRING name=*input->ObjectName,rest={0};XBOX_OBJECT_ATTRIBUTES oa=*input;
    oa.ObjectName=&name;uint32_t object=0,parent=0;
    if(oa.RootDirectory && name.Length && name.Buffer[0]!='\\' && !(name.Length>=2 && name.Buffer[1]==':')) {
        uint32_t root=ke_object_for_handle(oa.RootDirectory);
        if(root && ke_object_slot(root)->object_type==XBOX_KERNEL_DATA_BASE+KDATA_FILE_OBJ_TYPE) {
            call.related=root;call.device=BRIDGE_MEM32(root+4);rest=name;
        }
    }
    if(!call.device) {
        if(name.Length>=2 && name.Buffer[1]==':')oa.RootDirectory=(HANDLE)(intptr_t)-3;
        else if(name.Length && name.Buffer[0]=='\\')oa.RootDirectory=NULL;
        status=ke_resolve_name(&oa,0,&object,&parent,&rest);
        if(status<0 && !(status==(NTSTATUS)0xc0000024u && object)) {*handled=0;status=0;goto done;}
        KE_SHADOW_SLOT *slot=ke_object_slot(object);
        if(slot && slot->object_type==XBOX_KERNEL_DATA_BASE+KDATA_IO_DEVICE_TYPE)call.device=object;
        else if(slot && slot->object_type==XBOX_KERNEL_DATA_BASE+KDATA_FILE_OBJ_TYPE) {
            call.related=object;call.device=BRIDGE_MEM32(object+4);
        }
    }
    if(!call.device || !ke_object_slot(call.device) || ke_object_slot(call.device)->path_rule) {*handled=0;status=0;goto done;}
    status=io_validate_create_device(call.device,call.related);if(status<0)goto done;
    if(!BRIDGE_MEM32(call.device+0xc)) {status=STATUS_INVALID_DEVICE_REQUEST;goto done;}
    call.device=BRIDGE_MEM32(call.device+0xc);
    if(!ke_object_slot(call.device) || !BRIDGE_MEM32(call.device+8) || !BRIDGE_MEM8(call.device+0x1e)) {status=STATUS_INVALID_DEVICE_REQUEST;call.device=0;goto done;}
    if(BRIDGE_MEM32(call.device+4)==UINT32_MAX) {status=STATUS_INSUFFICIENT_RESOURCES;call.device=0;goto done;}
    ++BRIDGE_MEM32(call.device+4);
    if(call.related && ke_reference_pointer(call.related,XBOX_KERNEL_DATA_BASE+KDATA_FILE_OBJ_TYPE)<0)recomp_cont_invalid();
    if(rest.Length && !call.related && rest.Buffer>name.Buffer && rest.Buffer[-1]=='\\') {--rest.Buffer;++rest.Length;}
    call.file=xbox_HeapAlloc(0x68u+rest.Length,16);
    if(!call.file) {status=STATUS_INSUFFICIENT_RESOURCES;goto cleanup;}
    memset(XBOX_TO_NATIVE(call.file),0,0x68u+rest.Length);
    HANDLE signal=CreateEventW(NULL,TRUE,FALSE,NULL);
    if(!signal) {xbox_HeapFree(call.file);call.file=0;status=STATUS_INSUFFICIENT_RESOURCES;goto cleanup;}
    if(!ke_shadow_insert(call.file,signal)) {xbox_HeapFree(call.file);call.file=0;status=STATUS_INSUFFICIENT_RESOURCES;goto cleanup;}
    KE_SHADOW_SLOT *slot=ke_object_slot(call.file);slot->in_use=4;
    slot->object_type=XBOX_KERNEL_DATA_BASE+KDATA_FILE_OBJ_TYPE;slot->allocation=call.file;slot->references=1;
    BRIDGE_MEM16(call.file)=5;BRIDGE_MEM32(call.file+4)=call.device;BRIDGE_MEM32(call.file+0x1c)=call.related;
    BRIDGE_MEM8(call.file+2)=((access&(XBOX_GENERIC_READ|XBOX_FILE_READ_DATA))?2:0)|
        ((access&(XBOX_GENERIC_WRITE|XBOX_FILE_WRITE_DATA))?4:0)|((access&XBOX_DELETE)?8:0)|
        ((share&1)?16:0)|((share&2)?32:0)|((share&4)?64:0);
    BRIDGE_MEM8(call.file+3)=((options&0x30)?1:0)|((options&0x10)?2:0)|((options&8)?4:0)|
        ((options&4)?8:0)|((options&0x800)?0x40:0);
    if(options&0x30) {
        BRIDGE_MEM32(call.file+0x24)=UINT32_MAX;
        if(!ke_initialize_event(call.file+0x28,1,0)) {status=STATUS_INSUFFICIENT_RESOURCES;goto cleanup;}
    }
    BRIDGE_MEM8(call.file+0x3a)=4;BRIDGE_MEM32(call.file+0x40)=BRIDGE_MEM32(call.file+0x44)=call.file+0x40;
    BRIDGE_MEM16(call.file+0x48)=BRIDGE_MEM16(call.file+0x4a)=rest.Length;BRIDGE_MEM32(call.file+0x4c)=call.file+0x68;
    if(rest.Length)memcpy(XBOX_TO_NATIVE(call.file+0x68),rest.Buffer,rest.Length);
    if(allocation)memcpy(XBOX_TO_NATIVE(call.file+0x50),allocation,8);
    call.irp=io_guest_address(xbox_IoAllocateIrp(BRIDGE_MEM8(call.device+0x1e)));
    if(!call.irp) {status=STATUS_INSUFFICIENT_RESOURCES;goto cleanup;}
    BRIDGE_MEM32(call.irp+4)=0x8c;BRIDGE_MEM32(call.irp+0x60)=call.file;
    BRIDGE_MEM32(call.irp+0x20)=call.file+0x38;BRIDGE_MEM32(call.irp+0x1c)=call.file+0x58;
    BRIDGE_MEM32(call.irp+0x10)=STATUS_PENDING;
    uint32_t stack=BRIDGE_MEM32(call.irp+0x5c)-XBOX_IO_STACK_BYTES;
    BRIDGE_MEM8(stack)=0;BRIDGE_MEM8(stack+2)=(input->Attributes&0x40)?0:0x80;
    BRIDGE_MEM32(stack+4)=call.file+0x50;BRIDGE_MEM32(stack+8)=(options&0xffffffu)|(disposition<<24);
    BRIDGE_MEM16(stack+0xc)=(uint16_t)attributes;BRIDGE_MEM16(stack+0xe)=(uint16_t)share;
    BRIDGE_MEM32(stack+0x10)=call.file+0x48;BRIDGE_MEM32(stack+0x18)=call.file;
    io_attach_request_thread(call.irp,1);phase=1;
dispatch:
    status=xbox_IofCallDriver(XBOX_TO_NATIVE(call.device),XBOX_TO_NATIVE(call.irp));
    if(status==STATUS_PENDING) {
        phase=2;
wait_completion:
        io_wait_guest_event(call.file+0x38);
    }
    status=(NTSTATUS)BRIDGE_MEM32(call.irp+0x10);information=BRIDGE_MEM32(call.irp+0x14);
    if(status==STATUS_PENDING) {fprintf(stderr,"[STOP] create returned without completing its IRP\n");exit(5);}
    ke_list_unlink(call.irp+8);BRIDGE_MEM32(call.irp+8)=BRIDGE_MEM32(call.irp+12)=call.irp+8;
    xbox_IoFreeIrp(XBOX_TO_NATIVE(call.irp));call.irp=0;
    if(status>=0) {
        HANDLE handle=NULL;
        if(!DuplicateHandle(GetCurrentProcess(),ke_shadow_lookup(call.file),GetCurrentProcess(),&handle,0,FALSE,DUPLICATE_SAME_ACCESS))
            status=STATUS_INSUFFICIENT_RESOURCES;
        else {call.token=bridge_handle_token(handle);if(!call.token) {CloseHandle(handle);status=STATUS_INSUFFICIENT_RESOURCES;}}
    }
cleanup:
    if(call.file) {
        if(status<0)BRIDGE_MEM32(call.file+4)=0;
        else call.device=0; /* FILE_OBJECT owns this single create I/O reference. */
    }
    phase=3;
release_file: ;
    uint32_t release_file=call.file,release_device=call.device,release_related=call.related;
    call.file=call.device=call.related=0;
    io_create_release(release_file,release_device,release_related,0,0);
done:
    if(*handled) {
        if(status>=0 && call.token)*output=bridge_resolve_handle(call.token);
        else *output=NULL;
        if(ios){ios->Status=status;ios->Information=status<0?0:information;}
    }
    if(tracked){call.frame.on_leave=NULL;call.frame.on_unwind=NULL;recomp_cont_leave(&call.frame);}
    if(!held){guest_cpu_part();t_guest_thread=was_thread;}
    return status;
}

void xbox_kernel_release_file_device(uint32_t device) {
    int held=guest_cpu_is_held(),was_thread=t_guest_thread;RecompContFrame frame;
    const RecompContLocal locals[]={{"device",&device,4},{"held",&held,sizeof held},{"was_thread",&was_thread,sizeof was_thread}};
    int tracked=recomp_cont_active();
    uint64_t resume=tracked?kernel_native_cont_enter(&frame,KERNEL_CONT_FILE_DEVICE_RELEASE,locals,3):0;
    if(!guest_cpu_is_held())guest_cpu_join();
    if(resume)goto delete_device;
    if(!device)goto done;
    if(!ke_object_slot(device) || !BRIDGE_MEM32(device+4))recomp_cont_invalid();
    --BRIDGE_MEM32(device+4);
    if(!BRIDGE_MEM32(device+4) && BRIDGE_MEM8(device+0x1f)) {
delete_device:
        io_delete_device(device,0);
    }
done:
    if(tracked)recomp_cont_leave(&frame);
    if(!held){guest_cpu_part();t_guest_thread=was_thread;}
}
unsigned xbox_kernel_file_device_type(HANDLE handle) {
    int held=guest_cpu_is_held(),was_thread=t_guest_thread;if(!held)guest_cpu_join();
    uint32_t device=xbox_FileHandleDevice(handle);unsigned type=0;
    if(device) {
        type=BRIDGE_MEM8(device+0x1c);
        if((type==3 || type==8) && ke_object_slot(device)->path_rule) {
            device=BRIDGE_MEM32(BRIDGE_MEM32(device+0x18));type=device?BRIDGE_MEM8(device+0x1c):0;
        }
    }
    if(!held){guest_cpu_part();t_guest_thread=was_thread;}return type;
}

NTSTATUS xbox_kernel_file_volume_status(HANDLE handle) {
    int held=guest_cpu_is_held(),was_thread=t_guest_thread;if(!held)guest_cpu_join();
    uint32_t device=xbox_FileHandleDevice(handle);NTSTATUS status=0;
    if(device) {
        /* The open-file policy owns the device I/O reference, so its body
         * remains alive throughout this operation without a namespace scan. */
        if((BRIDGE_MEM8(device+0x1c)==3 || BRIDGE_MEM8(device+0x1c)==8) &&
           !BRIDGE_MEM32(BRIDGE_MEM32(device+0x18)))status=(NTSTATUS)0xc000026eu;
    }
    if(!held){guest_cpu_part();t_guest_thread=was_thread;}return status;
}
NTSTATUS xbox_kernel_dismount_file_volume(HANDLE handle) {
    int held=guest_cpu_is_held(),was_thread=t_guest_thread;if(!held)guest_cpu_join();
    uint32_t device=xbox_FileHandleDevice(handle);NTSTATUS status=STATUS_INVALID_HANDLE;
    if(device) {
        KE_SHADOW_SLOT *slot=ke_object_slot(device);
        if(!slot)recomp_cont_invalid();
        status=STATUS_INVALID_DEVICE_REQUEST;
        if(slot->path_rule && (BRIDGE_MEM8(device+0x1c)==3 || BRIDGE_MEM8(device+0x1c)==8)) {
            uint32_t extension=BRIDGE_MEM32(device+0x18),physical=BRIDGE_MEM32(extension);
            status=(NTSTATUS)0xc000026eu;
            if(physical) {
                if(BRIDGE_MEM32(physical+0xc)==device)BRIDGE_MEM32(physical+0xc)=0;
                BRIDGE_MEM32(extension)=0;ke_dereference_object(physical);
                io_delete_device(device,1);status=0;
            }
        }
    }
    if(!held){guest_cpu_part();t_guest_thread=was_thread;}return status;
}

int xbox_SymbolicLinkPath(const char *name,const char *rest,wchar_t *path,DWORD capacity) {
    int held=guest_cpu_is_held(),was_thread=t_guest_thread;if(!held)guest_cpu_join();
    uint32_t target=xbox_LookupSymbolicLinkTarget(name);int result=-1;
    if(target) {
        KE_SHADOW_SLOT *slot=ke_object_slot(target);
        if(slot && slot->object_type==XBOX_KERNEL_DATA_BASE+KDATA_FILE_OBJ_TYPE) {
            DWORD length=GetFinalPathNameByHandleW(slot->host_handle,path,capacity,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
            if(length && length<capacity) {
                if(!*rest)result=1;
                else if(length+1<capacity) {
                    if(path[length-1]!=L'\\')path[length++]=L'\\';
                    if(MultiByteToWideChar(CP_ACP,0,rest,-1,path+length,(int)(capacity-length))) {
                        for(WCHAR *at=path+length;*at;at++)if(*at==L'/')*at=L'\\';
                        result=1;
                    }
                }
            }
        } else if(slot && slot->object_type==XBOX_KERNEL_DATA_BASE+KDATA_IO_DEVICE_TYPE && slot->path_rule) {
            if(xbox_translate_device_path(slot->path_rule,rest,path,capacity))result=1;
        }
    }
    if(!held){guest_cpu_part();t_guest_thread=was_thread;}return result;
}

static uint32_t ke_created_type(uint32_t ordinal) {
    switch(ordinal) {
    case 188:return XBOX_KERNEL_DATA_BASE+KDATA_OBJ_DIR_TYPE;
    case 189:return XBOX_KERNEL_DATA_BASE+KDATA_EVENT_OBJ_TYPE;
    case 191:return XBOX_KERNEL_DATA_BASE+KDATA_IO_COMPLETION_TYPE;
    case 192:return XBOX_KERNEL_DATA_BASE+KDATA_MUTANT_OBJ_TYPE;
    case 193:return XBOX_KERNEL_DATA_BASE+KDATA_SEMAPHORE_OBJ_TYPE;
    case 194:return XBOX_KERNEL_DATA_BASE+KDATA_TIMER_OBJ_TYPE;
    default:recomp_cont_invalid();return 0;
    }
}
NTSTATUS xbox_kernel_create_named(uint32_t ordinal,PHANDLE output,PXBOX_OBJECT_ATTRIBUTES oa,uint32_t first,uint32_t second) {
    if(!output)return STATUS_INVALID_PARAMETER;*output=NULL;
    if(((ordinal==189 || ordinal==194) && first>1) ||
       (ordinal==193 && ((int32_t)first<0 || (int32_t)second<=0 || first>second)))return STATUS_INVALID_PARAMETER;
    int held=guest_cpu_is_held(),was_thread=t_guest_thread;if(!held)guest_cpu_join();ke_shadow_init();
    uint32_t object=0,parent=0,type=ke_created_type(ordinal);XBOX_ANSI_STRING leaf={0};NTSTATUS status=0;
    int named=oa && oa->ObjectName;
    if(named)status=ke_resolve_name(oa,type,&object,&parent,&leaf);
    if(named && status>=0) {
        if(!(oa->Attributes&0x80))status=(NTSTATUS)0xc0000035u;
        else if(ke_object_slot(object)->object_type!=type)status=(NTSTATUS)0xc0000024u;
        else {status=xbox_NtDuplicateObject(ke_shadow_lookup(object),output,0);if(status>=0)status=(NTSTATUS)0x40000000u;}
        goto done;
    }
    if(status<0 && !(status==STATUS_OBJECT_NAME_NOT_FOUND && parent))goto done;
    switch(ordinal) {
    case 188:status=ke_native_directory(output);break;
    case 189:status=xbox_NtCreateEvent(output,NULL,first,(BOOLEAN)second);break;
    case 191: {
        uint32_t queue=xbox_HeapAlloc(0x28,16);
        if(!queue){status=STATUS_INSUFFICIENT_RESOURCES;break;}
        if(!ke_initialize_queue(queue,first)){xbox_HeapFree(queue);status=STATUS_INSUFFICIENT_RESOURCES;break;}
        ke_mark_owned_queue(queue);status=xbox_NtDuplicateObject(ke_shadow_lookup(queue),output,0);
        if(status<0)ke_maybe_release_queue(queue);
        break;
    }
    case 192:status=xbox_NtCreateMutant(output,NULL,(BOOLEAN)first);break;
    case 193:status=xbox_NtCreateSemaphore(output,NULL,(LONG)first,(LONG)second);break;
    case 194:status=ke_create_timer_object(output,first);break;
    }
    if(status>=0 && (named || ordinal==188)) {
        status=ke_reference_handle(*output,type,&object);
        if(status>=0) {
            if(named)status=ke_attach_name(object,parent,&leaf,oa->Attributes);
            ke_dereference_object(object);
        }
        if(status<0){xbox_NtClose(*output);*output=NULL;}
    }
done:
    if(!held){guest_cpu_part();t_guest_thread=was_thread;}return status;
}
NTSTATUS xbox_kernel_init_devices(void) {
    int held=guest_cpu_is_held(),was_thread=t_guest_thread;if(!held)guest_cpu_join();
    char *parent="\\Device\\Harddisk0";
    XBOX_ANSI_STRING name={(USHORT)strlen(parent),(USHORT)strlen(parent),parent};
    XBOX_OBJECT_ATTRIBUTES oa={NULL,&name,0x10};HANDLE directory=NULL;
    NTSTATUS status=xbox_kernel_create_named(188,&directory,&oa,0,0);
    if(status<0)goto done;xbox_NtClose(directory);
    for(unsigned rule=1;rule<=xbox_path_rule_count();rule++) {
        char path[64];ULONG type;
        if(!xbox_path_device_rule(rule,path,sizeof path,&type))continue;
        name=(XBOX_ANSI_STRING){(USHORT)strlen(path),(USHORT)strlen(path),path};
        PVOID device=NULL;status=xbox_IoCreateDevice(NULL,0,&name,type,FALSE,&device);
        if(status<0)goto done;
        uint32_t address=io_guest_address(device);ke_object_slot(address)->path_rule=rule;
        BRIDGE_MEM32(address+0x14)&=~0x10u;
    }
done:
    if(!held){guest_cpu_part();t_guest_thread=was_thread;}return status;
}

static NTSTATUS ke_open_named_object(PXBOX_OBJECT_ATTRIBUTES oa,uint32_t type,PHANDLE output) {
    *output=NULL;uint32_t object,parent;XBOX_ANSI_STRING leaf;
    NTSTATUS status=ke_resolve_name(oa,type,&object,&parent,&leaf);
    if(status>=0) {
        if(type && ke_object_slot(object)->object_type!=type)return (NTSTATUS)0xc0000024u;
        status=xbox_NtDuplicateObject(ke_shadow_lookup(object),output,0);
    }
    return status;
}
static NTSTATUS bridge_read_object_attributes(uint32_t address,XBOX_OBJECT_ATTRIBUTES *oa,XBOX_ANSI_STRING *name) {
    memset(oa,0,sizeof *oa);memset(name,0,sizeof *name);
    if(!address)return 0;
    if(!bridge_va_mapped(address,12))return (NTSTATUS)0xc0000005u;
    uint32_t root=BRIDGE_MEM32(address),string=BRIDGE_MEM32(address+4);
    oa->RootDirectory=bridge_resolve_handle(root);oa->Attributes=BRIDGE_MEM32(address+8);
    if(root && !oa->RootDirectory)return STATUS_INVALID_HANDLE;
    if(string) {
        if(!bridge_va_mapped(string,8))return (NTSTATUS)0xc0000005u;
        name->Length=BRIDGE_MEM16(string);name->MaximumLength=BRIDGE_MEM16(string+2);
        uint32_t buffer=BRIDGE_MEM32(string+4);
        if(name->Length && !bridge_va_mapped(buffer,name->Length))return (NTSTATUS)0xc0000005u;
        name->Buffer=XBOX_TO_NATIVE(buffer);oa->ObjectName=name;
    }
    return 0;
}
static void bridge_create_dispatcher_object(uint32_t ordinal,uint32_t output,uint32_t attributes,uint32_t first,uint32_t second) {
    XBOX_OBJECT_ATTRIBUTES oa;XBOX_ANSI_STRING name;HANDLE handle=NULL;
    NTSTATUS status=bridge_read_object_attributes(attributes,&oa,&name);
    if(!output)status=STATUS_INVALID_PARAMETER;
    else if(!bridge_va_mapped(output,4))status=(NTSTATUS)0xc0000005u;
    if(status>=0)status=xbox_kernel_create_named(ordinal,&handle,attributes?&oa:NULL,first,second);
    if(output && bridge_va_mapped(output,4)) {
        uint32_t token=bridge_handle_token(handle);
        if(handle && !token){xbox_NtClose(handle);status=STATUS_INSUFFICIENT_RESOURCES;}
        BRIDGE_MEM32(output)=token;
    }
    g_eax=(uint32_t)status;
}
void xbox_kernel_make_temporary(PVOID object) {
    uint32_t address=(uint32_t)((uintptr_t)object-g_xbox_mem_offset);
    KE_SHADOW_SLOT *slot=ke_object_slot(address);if(!slot)recomp_cont_invalid();
    slot->name_attributes&=~0x10u;ke_release_object_if_unused(address);
}
NTSTATUS __stdcall xbox_ObReferenceObjectByName(PXBOX_ANSI_STRING name,ULONG attributes,PVOID type,PVOID context,PVOID *output) {
    (void)context;if(!output)return STATUS_INVALID_PARAMETER;*output=NULL;
    int held=guest_cpu_is_held(),was_thread=t_guest_thread;if(!held)guest_cpu_join();ke_shadow_init();
    XBOX_OBJECT_ATTRIBUTES oa={NULL,name,attributes};XBOX_ANSI_STRING leaf;uint32_t object,parent,expected=0;
    if(type==xbox_ExEventObjectType)expected=XBOX_KERNEL_DATA_BASE+KDATA_EVENT_OBJ_TYPE;
    else if(type==xbox_PsThreadObjectType)expected=XBOX_KERNEL_DATA_BASE+KDATA_THREAD_OBJ_TYPE;
    else if(type==xbox_IoFileObjectType)expected=XBOX_KERNEL_DATA_BASE+KDATA_FILE_OBJ_TYPE;
    else if(type==xbox_IoDeviceObjectType)expected=XBOX_KERNEL_DATA_BASE+KDATA_IO_DEVICE_TYPE;
    else if(type)expected=(uint32_t)((uintptr_t)type-g_xbox_mem_offset);
    NTSTATUS status=ke_resolve_name(&oa,expected,&object,&parent,&leaf);
    if(status>=0)status=ke_reference_pointer(object,expected);
    if(status>=0)*output=XBOX_TO_NATIVE(object);
    if(!held){guest_cpu_part();t_guest_thread=was_thread;}return status;
}
NTSTATUS xbox_kernel_close_handle(HANDLE handle) {
    extern NTSTATUS NTAPI NtClose(HANDLE);
    if(!g_ke_shadow_init || t_checkpoint_controller) {
        NTSTATUS status=NtClose(handle);
        if(status>=0){xbox_dir_context_drop(handle);xbox_FileHandleClosed(handle);bridge_retire_native_handle(handle);}
        return status;
    }
    int held=guest_cpu_is_held(),was_thread=t_guest_thread;uint32_t object=0,device=0,last=0,phase=0;
    NTSTATUS status=0;RecompContFrame frame;
    const RecompContLocal locals[]={{"object",&object,4},{"device",&device,4},{"last",&last,4},{"phase",&phase,4},
        {"status",&status,4},{"held",&held,sizeof held},{"was_thread",&was_thread,sizeof was_thread}};
    int tracked=recomp_cont_active();
    uint64_t resume=tracked?kernel_native_cont_enter(&frame,KERNEL_CONT_CLOSE_HANDLE,locals,7):0;
    if(!guest_cpu_is_held())guest_cpu_join();
    if(resume) {
        if(phase==1)goto release_object;
        if(phase==2)goto release_device;
        if(phase==3)goto cleanup_file;
        if(phase==4)goto release_cleanup_reference;
        recomp_cont_invalid();
    }
    int private_handle=0;
    for(unsigned i=1;i<BRIDGE_HANDLE_MAX;i++)if(s_handle_table[i]==handle && s_handle_private[i]) {
        private_handle=1;break;
    }
    object=ke_object_for_handle(handle);device=xbox_FileHandleDevice(handle);
    if(device) {
        struct {ULONG values[12];LARGE_INTEGER creation;} basic;
        if(checkpoint_native.object(handle,0,&basic,sizeof basic,NULL)>=0)last=basic.values[2]==1;
    }
    status=NtClose(handle);
    if(status>=0) {
        xbox_dir_context_drop(handle);xbox_FileHandleClosed(handle);bridge_retire_native_handle(handle);
        if(object) {
            EnterCriticalSection(&g_ke_shadow_cs);KE_SHADOW_SLOT *slot=ke_object_slot(object);
            int cleanup=!private_handle && slot && slot->object_type==ke_object_type_for_kind(CHECKPOINT_FILE) &&
                !ke_public_handle_count(slot);
            if(cleanup) {
                if(slot->references==UINT32_MAX)recomp_cont_invalid();
                ++slot->references;
            }
            LeaveCriticalSection(&g_ke_shadow_cs);
            if(cleanup) {
                phase=3;
cleanup_file:
                io_file_request(object,13);phase=4;
release_cleanup_reference:
                ke_dereference_object(object);
            } else {
                phase=1;
release_object:
                ke_release_object_if_unused(object);
            }
        }
        if(last) {
            phase=2;
release_device:
            xbox_kernel_release_file_device(device);
        }
    }
    if(tracked)recomp_cont_leave(&frame);
    if(!held){guest_cpu_part();t_guest_thread=was_thread;}return status;
}

VOID __fastcall xbox_ObfReferenceObject(PVOID object) {
    if(ke_reference_pointer((uint32_t)((uintptr_t)object-g_xbox_mem_offset),0)<0)recomp_cont_invalid();
}
VOID __fastcall xbox_ObfDereferenceObject(PVOID object) {
    ke_dereference_object((uint32_t)((uintptr_t)object-g_xbox_mem_offset));
}
NTSTATUS __stdcall xbox_ObReferenceObjectByHandle(HANDLE handle,PVOID type,PVOID *object) {
    if(!object)return (NTSTATUS)0xc000000du;
    uint32_t expected=0,address=0;
    if(type==xbox_PsThreadObjectType)expected=ke_object_type_for_kind(CHECKPOINT_THREAD);
    else if(type==xbox_ExEventObjectType)expected=ke_object_type_for_kind(CHECKPOINT_EVENT);
    else if(type==xbox_IoFileObjectType)expected=ke_object_type_for_kind(CHECKPOINT_FILE);
    else if(type==xbox_IoDeviceObjectType)expected=XBOX_KERNEL_DATA_BASE+KDATA_IO_DEVICE_TYPE;
    else if(type)expected=(uint32_t)((uintptr_t)type-g_xbox_mem_offset);
    NTSTATUS status=ke_reference_handle(handle,expected,&address);*object=XBOX_TO_NATIVE(address);return status;
}

static int checkpoint_capture_mutant_owner(HANDLE handle,CheckpointObject *object,int reject_abandoned) {
        struct { LONG count;BOOLEAN owned,abandoned; } state;
        if (checkpoint_native.mutant(handle,0,&state,sizeof state,NULL)<0 || (reject_abandoned && state.abandoned) || state.count>1) return 0;
        object->depth=(uint32_t)(1LL-state.count);
        if (object->depth) {
            struct { HANDLE process,thread; } owner;
            if (checkpoint_native.mutant(handle,1,&owner,sizeof owner,NULL)<0 ||
                (uintptr_t)owner.process!=GetCurrentProcessId() ||
                !xbox_CheckpointThreadIdentity((DWORD)(uintptr_t)owner.thread,object->owner)) return 0;
        }
        return 1;
}
static int checkpoint_capture_object(HANDLE handle,uint32_t kind,CheckpointObject *object) {
    memset(object,0,sizeof *object);object->kind=kind;
    if(kind==CHECKPOINT_DIRECTORY)return 1;
    if(kind==CHECKPOINT_SYMLINK) {
        uint32_t address=ke_object_for_handle(handle);if(!address)return 0;
        object->value=(int32_t)BRIDGE_MEM32(address+8);object->limit=BRIDGE_MEM16(address+4);
        object->depth=BRIDGE_MEM32(address);return 1;
    }
    if (kind==CHECKPOINT_EVENT) {
        struct { LONG type,state; } state;
        if (checkpoint_native.event(handle,0,&state,sizeof state,NULL)<0 || state.type<0 || state.type>1) return 0;
        object->type=(uint32_t)state.type;object->value=state.state!=0;return 1;
    }
    if (kind==CHECKPOINT_SEMAPHORE) {
        struct { LONG count,maximum; } state;
        if (checkpoint_native.semaphore(handle,0,&state,sizeof state,NULL)<0) return 0;
        object->value=state.count;object->limit=state.maximum;return 1;
    }
    if (kind==CHECKPOINT_MUTANT) return checkpoint_capture_mutant_owner(handle,object,1);
    if (kind==CHECKPOINT_FILE) return xbox_CaptureFileOpenState(handle,&object->file);
    if (kind==CHECKPOINT_THREAD) {
        uint32_t thread=ke_object_for_handle(handle);
        if(thread && BRIDGE_MEM8(thread+0x2c)==4) {
            object->type=1;object->value=(int32_t)BRIDGE_MEM32(thread+0x120);return 1;
        }
        if(xbox_CheckpointThreadIdentity(GetThreadId(handle),object->owner))return 1;
        DWORD status;
        if(WaitForSingleObject(handle,0)!=WAIT_OBJECT_0 || !GetExitCodeThread(handle,&status))return 0;
        object->type=1;object->value=(int32_t)status;return 1;
    }
    return 0;
}
static int checkpoint_clear_objects(void) {
    if(!t_checkpoint_controller || !checkpoint_native_init())return 0;
    HANDLE *handles=calloc(BRIDGE_HANDLE_MAX+KE_SHADOW_SIZE,sizeof *handles);
    XboxCriticalSectionState *critical=calloc(4096,sizeof *critical);unsigned count=0,critical_count=0;
    int ok=handles && critical && xbox_CaptureCriticalSections(critical,4096,&critical_count);
    for(unsigned space=1;ok && space<=2;space++) {
        unsigned end=space==1?BRIDGE_HANDLE_MAX:KE_SHADOW_SIZE;
        for(unsigned i=space==1?1:0;ok && i<end;i++) {
            HANDLE handle=space==1?s_handle_table[i]:g_ke_shadow[i].host_handle;
            if(space==1?!handle:!g_ke_shadow[i].in_use)continue;
            CheckpointObjectRef ref={0};uint32_t kind;ok=checkpoint_query_basic(handle,&ref,&kind);if(!ok)break;
            unsigned j;for(j=0;j<count;j++)if(handles[j]==handle)break;
            if(j<count)continue;
            if(kind==CHECKPOINT_MUTANT) {
                for(j=0;j<count;j++)if(checkpoint_native.compare(handle,handles[j])==0)break;
                if(j==count) {
                    CheckpointObject object={0};ok=checkpoint_capture_mutant_owner(handle,&object,0);
                    if(ok && object.depth)ok=checkpoint_queue_owner(object.owner,handle,0,object.depth,0);
                }
            }
            handles[count++]=handle;
        }
    }
    for(unsigned i=0;ok && i<critical_count;i++)if(critical[i].depth)
        ok=checkpoint_queue_owner(critical[i].owner,NULL,critical[i].address,critical[i].depth,critical[i].site);
    if(ok) {
        for(unsigned i=0;i<CHECKPOINT_THREADS;i++)if(g_checkpoint_threads[i].owner_count)g_checkpoint_threads[i].owner_release=1;
        checkpoint_restore_owners();
        if(!xbox_CheckpointClearCriticalSections())recomp_cont_invalid();
        for(unsigned i=0;i<count;i++) {
            if(!SetHandleInformation(handles[i],HANDLE_FLAG_PROTECT_FROM_CLOSE,0))recomp_cont_invalid();
            if(xbox_NtClose(handles[i])<0)recomp_cont_invalid();
        }
        memset(s_handle_table,0,sizeof s_handle_table);memset(s_handle_private,0,sizeof s_handle_private);
        memset(g_ke_shadow,0,sizeof g_ke_shadow);
        memset(g_async_handles,0,sizeof g_async_handles);
    } else checkpoint_clear_owner_requests();
    free(handles);free(critical);return ok;
}
int xbox_CheckpointWriteObjects(FILE *out) {
    if (!t_checkpoint_controller || !checkpoint_native_init()) return 0;
    uint32_t refs=0,groups=0;
    for (unsigned i=1;i<BRIDGE_HANDLE_MAX;i++) refs+=s_handle_table[i]!=NULL;
    for (unsigned i=0;i<KE_SHADOW_SIZE;i++) refs+=g_ke_shadow[i].in_use!=0;
    CheckpointObjectRef *references=calloc(refs?refs:1,sizeof *references);
    CheckpointObject *objects=calloc(refs?refs:1,sizeof *objects);
    HANDLE *handles=calloc(refs?refs:1,sizeof *handles);
    XboxCriticalSectionState *critical=calloc(4096,sizeof *critical);unsigned critical_count=0;
    int ok=references && objects && handles && critical && xbox_CaptureCriticalSections(critical,4096,&critical_count);
    unsigned next=0;
    for (unsigned space=1;ok && space<=2;space++) {
        unsigned end=space==1?BRIDGE_HANDLE_MAX:KE_SHADOW_SIZE;
        for (unsigned i=space==1?1:0;ok && i<end;i++) {
            HANDLE handle=space==1?s_handle_table[i]:g_ke_shadow[i].host_handle;
            if (space==1?!handle:!g_ke_shadow[i].in_use) continue;
            CheckpointObjectRef *ref=&references[next++];uint32_t kind;
            ref->space=space;ref->index=i;
            if (space==2) {
                ref->address=g_ke_shadow[i].guest_va;ref->flag=g_ke_shadow[i].in_use;
                ref->object_type=g_ke_shadow[i].object_type;ref->references=g_ke_shadow[i].references;
                ref->allocation=g_ke_shadow[i].allocation;
                ref->name_parent=g_ke_shadow[i].name_parent;ref->name_address=g_ke_shadow[i].name_address;
                ref->name_length=g_ke_shadow[i].name_length;ref->name_attributes=g_ke_shadow[i].name_attributes;
                ref->namespace_root=g_ke_shadow[i].namespace_root;ref->path_rule=g_ke_shadow[i].path_rule;
            }
            else {ref->async=(uint32_t)bridge_handle_is_async(BRIDGE_HANDLE_TAG|i);ref->private_handle=s_handle_private[i];}
            ok=checkpoint_query_basic(handle,ref,&kind);if (!ok) break;
            unsigned group=0;
            while (group<groups && checkpoint_native.compare(handle,handles[group])!=0) group++;
            if (group==groups) {
                handles[groups]=handle;ok=checkpoint_capture_object(handle,kind,&objects[groups]);groups++;
            }
            ref->group=group;
        }
    }
    ok=ok && fwrite("XBKOBJ",6,1,out)==1 && fwrite(&groups,4,1,out)==1 && fwrite(&refs,4,1,out)==1 &&
       (!groups || fwrite(objects,sizeof *objects,groups,out)==groups) &&
       (!refs || fwrite(references,sizeof *references,refs,out)==refs) && fwrite(&critical_count,4,1,out)==1 &&
       (!critical_count || fwrite(critical,sizeof *critical,critical_count,out)==critical_count) && xbox_SaveIoNamespace(out);
    free(references);free(objects);free(handles);free(critical);return ok;
}
static int checkpoint_root_index(const uint32_t *identity,const XboxCheckpointRoot *roots,unsigned count) {
    for (unsigned i=0;i<count;i++) {
        uint32_t actual[]={roots[i].pcr,roots[i].stack_top,roots[i].spawned,roots[i].function};
        if (!memcmp(identity,actual,16)) return (int)i;
    }
    return -1;
}
static DWORD WINAPI checkpoint_completed_thread(LPVOID value) {return (DWORD)(uintptr_t)value;}
int xbox_CheckpointLoadObjects(FILE *in,const XboxCheckpointRoot *roots,unsigned root_count) {
    if (!t_checkpoint_controller || !in || (root_count && !roots) || root_count>CHECKPOINT_THREADS) return 0;
    if(!checkpoint_native_init())return 0;
    for (unsigned i=1;i<BRIDGE_HANDLE_MAX;i++) if (s_handle_table[i]) return 0;
    for (unsigned i=0;i<KE_SHADOW_SIZE;i++) if (g_ke_shadow[i].in_use) return 0;
    char magic[6];uint32_t groups,refs,critical_count=0;
    if (fread(magic,6,1,in)!=1 || memcmp(magic,"XBKOBJ",6) || fread(&groups,4,1,in)!=1 ||
        fread(&refs,4,1,in)!=1 || groups>BRIDGE_HANDLE_MAX+KE_SHADOW_SIZE ||
        refs>BRIDGE_HANDLE_MAX+KE_SHADOW_SIZE || groups>refs) return 0;
    CheckpointObject *objects=calloc(groups?groups:1,sizeof *objects);
    CheckpointObjectRef *references=calloc(refs?refs:1,sizeof *references);
    HANDLE *handles=calloc(groups?groups:1,sizeof *handles),*aliases=calloc(refs?refs:1,sizeof *aliases);
    XboxCriticalSectionState *critical=calloc(4096,sizeof *critical);
    int ok=objects && references && handles && aliases && critical;
    ok=ok && (!groups || fread(objects,sizeof *objects,groups,in)==groups) &&
       (!refs || fread(references,sizeof *references,refs,in)==refs) && fread(&critical_count,4,1,in)==1 &&
       critical_count<=4096 && (!critical_count || fread(critical,sizeof *critical,critical_count,in)==critical_count);
    void *namespace_state=ok?xbox_PrepareIoNamespace(in):NULL;ok=ok && namespace_state!=NULL;
    for (unsigned i=0;ok && i<groups;i++) {
        CheckpointObject *object=&objects[i];
        switch (object->kind) {
        case CHECKPOINT_EVENT:ok=object->type<=1 && (object->value==0 || object->value==1);break;
        case CHECKPOINT_SEMAPHORE:ok=object->value>=0 && object->limit>0 && object->value<=object->limit;break;
        case CHECKPOINT_MUTANT:ok=!object->depth || checkpoint_owner_thread(object->owner)!=NULL;break;
        case CHECKPOINT_FILE:ok=xbox_ValidateFileOpenState(&object->file);break;
        case CHECKPOINT_THREAD:ok=object->type==1 || (!object->type && checkpoint_root_index(object->owner,roots,root_count)>=0);break;
        case CHECKPOINT_DIRECTORY:ok=!object->type && !object->value && !object->limit && !object->depth;break;
        case CHECKPOINT_SYMLINK:ok=object->limit>0 && object->limit<=UINT16_MAX && object->depth &&
            bridge_va_mapped((uint32_t)object->value,(uint32_t)object->limit+1);break;
        default:ok=0;break;
        }
    }
    for(unsigned i=0;ok && i<groups;i++)if(objects[i].kind==CHECKPOINT_FILE && objects[i].file.device) {
        uint32_t device=objects[i].file.device;unsigned j;
        for(j=0;j<refs;j++)if(references[j].space==2 && references[j].address==device &&
            references[j].object_type==XBOX_KERNEL_DATA_BASE+KDATA_IO_DEVICE_TYPE && references[j].path_rule)break;
        ok=j<refs && bridge_va_mapped(device,0x48) && BRIDGE_MEM32(device+4)!=0;
    }
    unsigned async_count=0;
    for (unsigned i=0;ok && i<refs;i++) {
        CheckpointObjectRef *ref=&references[i];
        ok=ref->group<groups && ref->async<=1 && ref->private_handle<=1 && (ref->space==1 || !ref->private_handle) &&
           ((ref->space==1 && ref->index>0 && ref->index<BRIDGE_HANDLE_MAX) ||
            (ref->space==2 && ref->index<KE_SHADOW_SIZE && ref->address &&
             (uint64_t)ref->address+16<=xbox_GetMappedSize() && (ref->flag>=1 && ref->flag<=4)));
        async_count+=ref->async;ok=ok && async_count<=BRIDGE_ASYNC_MAX && (ref->space==1 || !ref->async);
        if(ref->flag==4)ok=ok && ref->space==2 &&
            ref->object_type==XBOX_KERNEL_DATA_BASE+KDATA_FILE_OBJ_TYPE && objects[ref->group].kind==CHECKPOINT_EVENT;
        if(ref->object_type)ok=ok && ref->space==2 && (ref->object_type==ke_object_type_for_kind(objects[ref->group].kind) ||
             (ref->object_type==XBOX_KERNEL_DATA_BASE+KDATA_IO_DEVICE_TYPE && objects[ref->group].kind==CHECKPOINT_DIRECTORY &&
              bridge_va_mapped(ref->address,0x48) && BRIDGE_MEM16(ref->address)==3) ||
             (ref->object_type==XBOX_KERNEL_DATA_BASE+KDATA_IO_COMPLETION_TYPE && objects[ref->group].kind==CHECKPOINT_EVENT &&
              ref->flag==3 && BRIDGE_MEM8(ref->address)==4) ||
             (ref->object_type==XBOX_KERNEL_DATA_BASE+KDATA_FILE_OBJ_TYPE && ref->flag==4 &&
              objects[ref->group].kind==CHECKPOINT_EVENT && objects[ref->group].type==0 &&
              bridge_va_mapped(ref->address,0x48) && BRIDGE_MEM16(ref->address)==5 &&
              BRIDGE_MEM8(ref->address+0x38)==0 && BRIDGE_MEM8(ref->address+0x3a)==4) ||
             (ref->object_type==XBOX_KERNEL_DATA_BASE+KDATA_TIMER_OBJ_TYPE && objects[ref->group].kind==CHECKPOINT_EVENT &&
              BRIDGE_MEM8(ref->address)==8+objects[ref->group].type)) &&
            (!ref->allocation || (ref->allocation>=0x10000u && (uint64_t)ref->allocation+16<=xbox_GetMappedSize()));
        else ok=ok && !ref->references && !ref->allocation;
        if(ok && ref->object_type==XBOX_KERNEL_DATA_BASE+KDATA_OBJ_SYM_LINK_TYPE) {
            CheckpointObject *object=&objects[ref->group];uint32_t target=BRIDGE_MEM32(ref->address);
            ok=target==object->depth && BRIDGE_MEM32(ref->address+8)==(uint32_t)object->value && BRIDGE_MEM16(ref->address+4)==object->limit;
            unsigned j;for(j=0;ok && j<refs;j++)if(references[j].space==2 && references[j].address==target &&
                references[j].object_type && references[j].object_type!=XBOX_KERNEL_DATA_BASE+KDATA_OBJ_SYM_LINK_TYPE && references[j].references)break;
            ok=ok && j<refs;
        }
        if(ref->name_address) {
            ok=ok && ref->space==2 && ref->object_type && ref->name_parent && ref->name_length &&
                ref->name_length<=UINT16_MAX && bridge_va_mapped(ref->name_address,ref->name_length+1);
            unsigned j;for(j=0;ok && j<refs;j++)if(references[j].space==2 && references[j].address==ref->name_parent &&
                references[j].object_type==XBOX_KERNEL_DATA_BASE+KDATA_OBJ_DIR_TYPE && references[j].references)break;
            ok=ok && j<refs;
        } else ok=ok && !ref->name_parent && !ref->name_length && !ref->name_attributes;
        if(ref->path_rule)ok=ok && ref->space==2 && ref->object_type==XBOX_KERNEL_DATA_BASE+KDATA_IO_DEVICE_TYPE &&
            xbox_path_device_rule(ref->path_rule,NULL,0,NULL);
        if(ref->namespace_root)ok=ok && ref->namespace_root<=4 && ref->space==2 &&
            ref->object_type==XBOX_KERNEL_DATA_BASE+KDATA_OBJ_DIR_TYPE &&
            (ref->namespace_root==1?!ref->name_address:ref->name_address!=0);
        for (unsigned j=0;ok && j<i;j++)
            ok=ref->space!=references[j].space || (ref->index!=references[j].index &&
               (ref->space!=2 || ref->address!=references[j].address));
    }
    for (unsigned i=0;ok && i<critical_count;i++)
        ok=!critical[i].depth || checkpoint_owner_thread(critical[i].owner)!=NULL;
    if (ok) ok=xbox_ValidateCriticalSections(critical,critical_count);
    for (unsigned i=0;ok && i<groups;i++) {
        CheckpointObject *object=&objects[i];
        switch (object->kind) {
        case CHECKPOINT_EVENT:handles[i]=CreateEventW(NULL,object->type==0,object->value!=0,NULL);break;
        case CHECKPOINT_SEMAPHORE:handles[i]=CreateSemaphoreW(NULL,object->value,object->limit,NULL);break;
        case CHECKPOINT_MUTANT:handles[i]=CreateMutexW(NULL,FALSE,NULL);break;
        case CHECKPOINT_FILE:handles[i]=xbox_RestoreFileOpenState(&object->file);break;
        case CHECKPOINT_DIRECTORY:ok=ke_native_directory(&handles[i])>=0;break;
        case CHECKPOINT_SYMLINK:ok=ke_native_symbolic(&handles[i],object->depth)>=0;break;
        case CHECKPOINT_THREAD:
            if(object->type==1) {
                handles[i]=CreateThread(NULL,0,checkpoint_completed_thread,(void *)(uintptr_t)(uint32_t)object->value,0,NULL);
                ok=handles[i] && WaitForSingleObject(handles[i],INFINITE)==WAIT_OBJECT_0;
            } else ok=DuplicateHandle(GetCurrentProcess(),roots[checkpoint_root_index(object->owner,roots,root_count)].handle,
                GetCurrentProcess(),&handles[i],0,FALSE,DUPLICATE_SAME_ACCESS)!=0;
            break;
        }
        ok=ok && handles[i]!=NULL;
    }
    for (unsigned i=0;ok && i<refs;i++) {
        CheckpointObjectRef *ref=&references[i];
        ok=DuplicateHandle(GetCurrentProcess(),handles[ref->group],GetCurrentProcess(),&aliases[i],ref->access,
                           (ref->attributes&2)!=0,0)!=0;
        if (ok && objects[ref->group].kind==CHECKPOINT_FILE) xbox_FileHandleDuplicated(handles[ref->group],aliases[i],0);
    }
    if (ok) ok=xbox_PrepareCriticalSections(critical,critical_count);
    for (unsigned i=0;ok && i<groups;i++) if (objects[i].kind==CHECKPOINT_MUTANT && objects[i].depth)
        ok=checkpoint_queue_owner(objects[i].owner,handles[i],0,objects[i].depth,0);
    for (unsigned i=0;ok && i<critical_count;i++) if (critical[i].depth)
        ok=checkpoint_queue_owner(critical[i].owner,NULL,critical[i].address,critical[i].depth,critical[i].site);
    if (ok) {
        ke_shadow_init();
        for (unsigned i=0;i<refs;i++) {
            CheckpointObjectRef *ref=&references[i];
            if (ref->space==1) {
                s_handle_table[ref->index]=aliases[i];
                s_handle_private[ref->index]=(uint8_t)ref->private_handle;
                if (ref->async) bridge_note_async_handle(BRIDGE_HANDLE_TAG|ref->index);
            }
            else g_ke_shadow[ref->index]=(KE_SHADOW_SLOT){ref->address,aliases[i],ref->flag,
                ref->object_type,ref->references,ref->allocation,ref->name_parent,ref->name_address,
                ref->name_length,ref->name_attributes,ref->namespace_root,ref->path_rule};
            if (ref->attributes&1) SetHandleInformation(aliases[i],HANDLE_FLAG_PROTECT_FROM_CLOSE,HANDLE_FLAG_PROTECT_FROM_CLOSE);
            aliases[i]=NULL;
        }
        checkpoint_restore_owners();
        xbox_ApplyIoNamespace(namespace_state);
    } else checkpoint_clear_owner_requests();
    for (unsigned i=0;i<groups;i++) if (handles && handles[i]) xbox_NtClose(handles[i]);
    for (unsigned i=0;i<refs;i++) if (aliases && aliases[i]) xbox_NtClose(aliases[i]);
    xbox_DiscardIoNamespace(namespace_state);free(objects);free(references);free(handles);free(aliases);free(critical);return ok;
}
#else
static int checkpoint_clear_objects(void) { return 0; }
#endif
static void checkpoint_poll(void) {
    GuestCheckpointThread *thread=t_checkpoint_thread;
    if (!thread || !ReadAcquire(&g_checkpoint_requested)) return;
    if (t_guest_held && (xbox_CurrentIrql()>=DISPATCH_LEVEL || xbox_IrqlBlocksInterrupts())) return;
    if (!recomp_cont_snapshot_ready()) return;
    FILE *packet=tmpfile();XboxGuestRegisters registers;
    uint32_t identity[]={g_fs_base,g_thread_stack_top,(uint32_t)g_is_spawned_thread,recomp_cont_root_function()};
    xbox_CaptureGuestRegisters(&registers);
    int ok=packet && fwrite(identity,sizeof identity,1,packet)==1 &&
           fwrite(&registers,sizeof registers,1,packet)==1 && recomp_cont_save(packet);
    AcquireSRWLockExclusive(&g_checkpoint_lock);
    if (!ReadAcquire(&g_checkpoint_requested)) {
        ReleaseSRWLockExclusive(&g_checkpoint_lock);if (packet) fclose(packet);return;
    }
    thread->packet=packet;thread->failed=!ok;thread->parked=1;
    ReleaseSRWLockExclusive(&g_checkpoint_lock);
    int held=t_guest_held;
    if (held) { t_guest_held=0;LeaveCriticalSection(&g_guest_cpu); }
    checkpoint_park_control();
    if (held) guest_cpu_join();
}
void xbox_CheckpointDeviceEnter(unsigned device) {
    if (!t_checkpoint_device_depth) {
        while (ReadAcquire(&g_checkpoint_stop_devices)) Sleep(1);
        AcquireSRWLockShared(&g_checkpoint_devices);
    }
    t_checkpoint_device_depth++;
    if (device && device<XBOX_CHECKPOINT_DEVICE_COUNT) {
        if (g_checkpoint_device_restore_pending[device]) {
            if (!xbox_RestoreGuestRegisters(&g_checkpoint_device_restore[device])) recomp_cont_invalid();
            g_checkpoint_device_restore_pending[device]=0;
        }
        g_checkpoint_device_context[device].active=1;
        if (device==XBOX_CHECKPOINT_TIMER || device==XBOX_CHECKPOINT_USB) {
            g_checkpoint_device_context[device].has_guest_context=1;
            xbox_CaptureGuestRegisters(&g_checkpoint_device_context[device].registers);
        }
    }
}
void xbox_CheckpointDeviceLeave(unsigned device) {
    if (device && device<XBOX_CHECKPOINT_DEVICE_COUNT && g_checkpoint_device_context[device].has_guest_context)
        xbox_CaptureGuestRegisters(&g_checkpoint_device_context[device].registers);
    xbox_CheckpointDeviceCreated(device);
    if (!t_checkpoint_device_depth) recomp_cont_invalid();
    t_checkpoint_device_depth--;
    if (!t_checkpoint_device_depth) ReleaseSRWLockShared(&g_checkpoint_devices);
}
static void checkpoint_release_threads(void) {
    AcquireSRWLockExclusive(&g_checkpoint_lock);
    for (unsigned i=0;i<CHECKPOINT_THREADS;i++) {
        GuestCheckpointThread *thread=&g_checkpoint_threads[i];
        if (thread->packet) fclose(thread->packet);
        thread->packet=NULL;thread->failed=0;
    }
    InterlockedExchange(&g_checkpoint_requested,0);
    ReleaseSRWLockExclusive(&g_checkpoint_lock);SetEvent(g_checkpoint_release);
    for (;;) {
        int parked=0;AcquireSRWLockShared(&g_checkpoint_lock);
        for (unsigned i=0;i<CHECKPOINT_THREADS;i++) parked|=g_checkpoint_threads[i].parked;
        ReleaseSRWLockShared(&g_checkpoint_lock);
        if (!parked) break;Sleep(1);
    }
    InterlockedExchange(&g_checkpoint_busy,0);
}
static int checkpoint_begin(DWORD timeout,int bootstrap) {
    if (t_guest_thread || t_checkpoint_controller || t_checkpoint_device_depth ||
        InterlockedCompareExchange(&g_checkpoint_busy,1,0)!=0) return 0;
    if (!InitOnceExecuteOnce(&g_checkpoint_once,checkpoint_init,NULL,NULL) || !guest_cpu_enabled()) {
        InterlockedExchange(&g_checkpoint_busy,0);return 0;
    }
    ResetEvent(g_checkpoint_release);InterlockedExchange(&g_checkpoint_requested,1);
    ULONGLONG started=GetTickCount64();int devices=0,cpu=0;
    for (;;) {
        int count=0,all=1,failed=0;AcquireSRWLockShared(&g_checkpoint_lock);
        for (unsigned i=0;i<CHECKPOINT_THREADS;i++) if (g_checkpoint_threads[i].active) {
            count++;all&=g_checkpoint_threads[i].parked;failed|=g_checkpoint_threads[i].failed;
        }
        ReleaseSRWLockShared(&g_checkpoint_lock);
        for(unsigned i=1;i<XBOX_CHECKPOINT_DEVICE_COUNT;i++)all&=!InterlockedCompareExchange(&g_checkpoint_device_starting[i],0,0);
        if ((!count && !bootstrap) || (count && bootstrap) || failed) break;
        if (all) {
            InterlockedExchange(&g_checkpoint_stop_devices,1);
            if (!devices) devices=TryAcquireSRWLockExclusive(&g_checkpoint_devices)!=0;
            if (devices) cpu=TryEnterCriticalSection(&g_guest_cpu)!=0;
            if (cpu) { t_checkpoint_controller=1;xbox_RuntimeClockPause();xa2_checkpoint_pause();return 1; }
        }
        if (GetTickCount64()-started>=timeout) break;
        Sleep(1);
    }
    InterlockedExchange(&g_checkpoint_stop_devices,0);
    if (devices) ReleaseSRWLockExclusive(&g_checkpoint_devices);
    checkpoint_release_threads();return 0;
}
int xbox_CheckpointBegin(DWORD timeout) { return checkpoint_begin(timeout,0); }
int xbox_CheckpointBootstrapBegin(void) {
    if (recomp_cont_active()) return 0;
    return checkpoint_begin(3000,1);
}
int xbox_CheckpointFrozen(void) { return t_checkpoint_controller!=0; }
void xbox_CheckpointEnd(void) {
    if (!t_checkpoint_controller) recomp_cont_invalid();
    xa2_checkpoint_resume();xbox_RuntimeClockResume();t_checkpoint_controller=0;LeaveCriticalSection(&g_guest_cpu);
    InterlockedExchange(&g_checkpoint_stop_devices,0);
    ReleaseSRWLockExclusive(&g_checkpoint_devices);checkpoint_release_threads();
}
int xbox_CheckpointWriteThreads(FILE *out) {
    if (!t_checkpoint_controller) return 0;
    uint32_t count=0;
    for (unsigned i=0;i<CHECKPOINT_THREADS;i++) count+=g_checkpoint_threads[i].active!=0;
    int ok=fwrite("XBTHREAD",8,1,out)==1 && fwrite(&count,sizeof count,1,out)==1;
    for (unsigned i=0;ok && i<CHECKPOINT_THREADS;i++) if (g_checkpoint_threads[i].active) {
        FILE *packet=g_checkpoint_threads[i].packet;
        long size=packet?ftell(packet):-1;
        if (size<=0 || (unsigned long)size>UINT32_MAX) return 0;
        uint32_t bytes=(uint32_t)size;ok=fwrite(&bytes,sizeof bytes,1,out)==1;rewind(packet);
        unsigned char buffer[4096];
        while (ok && bytes) {
            size_t chunk=bytes<sizeof buffer?bytes:sizeof buffer;
            ok=fread(buffer,chunk,1,packet)==1 && fwrite(buffer,chunk,1,out)==1;
            bytes-=(uint32_t)chunk;
        }
        fseek(packet,0,SEEK_END);
    }
    return ok;
}
int xbox_CheckpointWriteDevices(FILE *out) {
    if (!t_checkpoint_controller) return 0;
    uint32_t count=0;
    for (unsigned i=1;i<XBOX_CHECKPOINT_DEVICE_COUNT;i++) count+=g_checkpoint_device_context[i].active!=0;
    int ok=fwrite("XBDEVICE",8,1,out)==1 && fwrite(&count,sizeof count,1,out)==1;
    for (uint32_t i=1;ok && i<XBOX_CHECKPOINT_DEVICE_COUNT;i++) if (g_checkpoint_device_context[i].active)
        ok=fwrite(&i,sizeof i,1,out)==1 &&
           fwrite(&g_checkpoint_device_context[i].has_guest_context,sizeof(int),1,out)==1 &&
           (!g_checkpoint_device_context[i].has_guest_context ||
            fwrite(&g_checkpoint_device_context[i].registers,sizeof(XboxGuestRegisters),1,out)==1);
    return ok;
}
int xbox_CheckpointLoadDevices(FILE *in) {
    if (!t_checkpoint_controller || !in) return 0;
    char magic[8];uint32_t count;
    if (fread(magic,8,1,in)!=1 || memcmp(magic,"XBDEVICE",8) || fread(&count,4,1,in)!=1 ||
        count>=XBOX_CHECKPOINT_DEVICE_COUNT) return 0;
    struct { int present,has_context;XboxGuestRegisters registers; } staged[XBOX_CHECKPOINT_DEVICE_COUNT]={0};
    for (unsigned i=0;i<count;i++) {
        uint32_t device;int has_context;
        if (fread(&device,4,1,in)!=1 || !device || device>=XBOX_CHECKPOINT_DEVICE_COUNT || staged[device].present ||
            fread(&has_context,sizeof has_context,1,in)!=1 || (has_context!=0 && has_context!=1) ||
            has_context!=(device==XBOX_CHECKPOINT_TIMER || device==XBOX_CHECKPOINT_USB) ||
            !g_checkpoint_device_context[device].active) return 0;
        staged[device].present=1;staged[device].has_context=has_context;
        if (has_context && (fread(&staged[device].registers,sizeof(XboxGuestRegisters),1,in)!=1 ||
                            !xbox_ValidateGuestRegisters(&staged[device].registers) ||
                            !staged[device].registers.fs_base ||
                            (uint64_t)staged[device].registers.fs_base+0x40>xbox_GetMappedSize())) return 0;
    }
    for (unsigned i=1;i<XBOX_CHECKPOINT_DEVICE_COUNT;i++)
        if ((g_checkpoint_device_context[i].active!=0)!=staged[i].present) return 0;
    for (unsigned i=1;i<XBOX_CHECKPOINT_DEVICE_COUNT;i++) if (staged[i].has_context) {
        g_checkpoint_device_context[i].registers=staged[i].registers;
        g_checkpoint_device_restore[i]=staged[i].registers;g_checkpoint_device_restore_pending[i]=1;
    }
    return 1;
}
static void kernel_wait_close_objects(ULONG count,const uint32_t *tokens) {
    for(ULONG i=0;i<count;i++) {
        /* NtClose retires the token itself; preserve the private identity
         * until it has distinguished a wait release from public cleanup. */
        HANDLE handle=bridge_resolve_handle(tokens[i]);
        if(!handle || xbox_NtClose(handle)<0)recomp_cont_invalid();
    }
}
static void kernel_wait_leave(RecompContFrame *frame) {
    GuestKernelWait *wait=(GuestKernelWait *)frame;
    if (t_kernel_wait!=wait) recomp_cont_invalid();
    if(!frame->retained && !checkpoint_root_cancelled()) {
        kernel_wait_close_objects(wait->object_count,XBOX_TO_NATIVE(wait->object_tokens_va));
        if(wait->object_tokens_va)xbox_HeapFree(wait->object_tokens_va);
    }
    wait->object_count=wait->object_tokens_va=wait->signal_done=0;
    t_kernel_wait=wait->parent;
    if (wait->dispatch_started) {
        if ((frame->site>>32)==6) t_dispatch_depth=(int)wait->main_saved_depth;
        t_dispatch_depth--;
        if (t_guest_thread && !t_guest_held && t_dispatch_depth==0) guest_cpu_join();
    }
}
static void kernel_wait_enter(GuestKernelWait *wait,uint32_t slot)
{
    wait->locals[0]=(RecompContLocal){"slot",&wait->slot,sizeof wait->slot};
    wait->locals[1]=(RecompContLocal){"entry_esp",&wait->entry_esp,sizeof wait->entry_esp};
    wait->locals[2]=(RecompContLocal){"remaining_ms",&wait->remaining_ms,sizeof wait->remaining_ms};
    wait->locals[3]=(RecompContLocal){"main_routine",&wait->main_routine,sizeof wait->main_routine};
    wait->locals[4]=(RecompContLocal){"main_saved_depth",&wait->main_saved_depth,sizeof wait->main_saved_depth};
    wait->locals[5]=(RecompContLocal){"watch_before",&wait->watch_before,sizeof wait->watch_before};
    wait->locals[6]=(RecompContLocal){"object_count",&wait->object_count,sizeof wait->object_count};
    wait->locals[7]=(RecompContLocal){"object_tokens_va",&wait->object_tokens_va,sizeof wait->object_tokens_va};
    wait->locals[8]=(RecompContLocal){"signal_done",&wait->signal_done,sizeof wait->signal_done};
    wait->locals[9]=(RecompContLocal){"native_held",&wait->native_held,sizeof wait->native_held};
    uint32_t function=slot==UINT32_MAX?KERNEL_CONT_NATIVE_WAIT:KERNEL_VA_BASE+slot*4;
    wait->slot=slot;wait->entry_esp=g_esp;wait->remaining_ms=0;
    wait->main_routine=wait->main_saved_depth=wait->watch_before=wait->object_count=wait->object_tokens_va=wait->signal_done=0;
    wait->native_held=slot==UINT32_MAX?guest_cpu_is_held():0;
    uint64_t resume=recomp_cont_enter(&wait->frame,function,wait->locals,sizeof wait->locals/sizeof wait->locals[0]);
    if(resume && resume!=((5ULL<<32)|function) && resume!=((8ULL<<32)|function) &&
       (slot==UINT32_MAX || (resume!=((6ULL<<32)|function) && resume!=((9ULL<<32)|function) &&
                            resume!=((11ULL<<32)|function))))recomp_cont_invalid();
    if(wait->slot!=slot || (slot!=UINT32_MAX && slot>=XBOX_KERNEL_THUNK_TABLE_SIZE))recomp_cont_invalid();
    if(wait->signal_done>1 || wait->native_held>1 || wait->object_count>MAXIMUM_WAIT_OBJECTS ||
       (wait->object_count && (!wait->object_tokens_va ||
        (uint64_t)wait->object_tokens_va+wait->object_count*4>xbox_GetMappedSize())) ||
       (!wait->object_count && wait->object_tokens_va))recomp_cont_invalid();
    wait->restoring=resume?(resume>>32)==11?5:(resume>>32)==9?4:(resume>>32)==8?3:(resume>>32)==6?2:1:0;
    wait->resume_esp=g_esp;wait->apc_started=0;wait->dispatch_started=0;
    if(resume) {
        if(wait->restoring==1 || wait->restoring==3)g_esp=wait->entry_esp;
        if(slot!=UINT32_MAX)g_kernel_dispatch_slot=(int)slot;
    }
    wait->parent=t_kernel_wait;t_kernel_wait=wait;wait->frame.on_leave=kernel_wait_leave;
}

/* Native I/O and queue callers use the same wait aliases, snapshot state and
 * cleanup as imported waits. Their surrounding native frame owns the phase. */
static DWORD kernel_wait_native(ULONG count,const HANDLE *handles,KPROCESSOR_MODE mode,
                                BOOLEAN alertable,DWORD milliseconds)
{
    GuestKernelWait wait;int tracked=recomp_cont_active(),held=guest_cpu_is_held();
    if(tracked){kernel_wait_enter(&wait,UINT32_MAX);held=(int)wait.native_held;}
    DWORD result=xbox_checkpoint_wait(count,handles,FALSE,mode,alertable,milliseconds);
    if(held && !guest_cpu_is_held())guest_cpu_join();
    if(tracked)recomp_cont_leave(&wait.frame);
    return result;
}

/* A native wait keeps its objects, even if another guest closes or reuses
 * an input token. Private aliases belong to the existing continuation frame
 * and handle table, so snapshots retain the same identity without resampling
 * a closed caller handle when the wait resumes. */
NTSTATUS xbox_kernel_wait_acquire(ULONG count,const HANDLE *source,HANDLE *owned,uint32_t *tokens) {
    if(count>MAXIMUM_WAIT_OBJECTS)return (NTSTATUS)0xc000000du;
    if(!count)return 0;
    int held=t_guest_held,was_thread=t_guest_thread;if(!held)guest_cpu_join();
    NTSTATUS status=0;
    if(t_kernel_wait && t_kernel_wait->object_count) {
        if(t_kernel_wait->object_count!=count)recomp_cont_invalid();
        for(ULONG i=0;i<count;i++) {
            tokens[i]=BRIDGE_MEM32(t_kernel_wait->object_tokens_va+i*4);owned[i]=bridge_resolve_handle(tokens[i]);
            if(!owned[i])recomp_cont_invalid();
        }
    } else {
        ULONG copied=0;
        for(;copied<count;copied++) {
            HANDLE alias=NULL;status=xbox_NtDuplicateObject(source[copied],&alias,0);
            if(status<0)break;
#ifdef _WIN32
            /* DuplicateHandle created a new live native handle identity. */
            tokens[copied]=bridge_allocate_handle_token(alias);
#else
            tokens[copied]=bridge_handle_token(alias);
#endif
            if(!tokens[copied]) {xbox_NtClose(alias);status=(NTSTATUS)0xc000009au;break;}
            s_handle_private[tokens[copied]&BRIDGE_HANDLE_MASK]=1;
            owned[copied]=alias;
        }
        if(status>=0 && t_kernel_wait && count) {
            uint32_t storage=xbox_HeapAlloc(count*sizeof *tokens,16);
            if(!storage)status=(NTSTATUS)0xc000009au;
            else {
                memcpy(XBOX_TO_NATIVE(storage),tokens,count*sizeof *tokens);
                t_kernel_wait->object_tokens_va=storage;t_kernel_wait->object_count=count;
            }
        }
        if(status<0)kernel_wait_close_objects(copied,tokens);
    }
    if(!held){guest_cpu_part();t_guest_thread=was_thread;}
    return status;
}
void xbox_kernel_wait_release_objects(ULONG count,const uint32_t *tokens) {
    if(!count)return;
    int held=t_guest_held,was_thread=t_guest_thread;if(!held)guest_cpu_join();
    kernel_wait_close_objects(count,tokens);
    if(t_kernel_wait) {
        if(t_kernel_wait->object_tokens_va)xbox_HeapFree(t_kernel_wait->object_tokens_va);
        t_kernel_wait->object_count=t_kernel_wait->object_tokens_va=t_kernel_wait->signal_done=0;
    }
    if(!held){guest_cpu_part();t_guest_thread=was_thread;}
}
int xbox_kernel_wait_signal_done(void) {return t_kernel_wait && t_kernel_wait->signal_done;}
void xbox_kernel_wait_mark_signalled(void) {if(t_kernel_wait)t_kernel_wait->signal_done=1;}
int xbox_kernel_wait_release_cpu(void) {
    int held=t_guest_held;
    if(held){t_guest_held=0;LeaveCriticalSection(&g_guest_cpu);}
    /* An import reacquires in its existing dispatcher after post-wait state
     * publication. A direct native caller must recover its own ownership. */
    return held && !t_dispatch_depth;
}
void xbox_kernel_set_checkpoint_hook(void (*hook)(void)) { g_checkpoint_hook=hook; }
int xbox_kernel_wait_tracked(void) { return t_kernel_wait!=0; }
uint32_t xbox_kernel_wait_begin(uint32_t milliseconds) {
    if(t_kernel_wait && t_kernel_wait->restoring==3)return t_kernel_wait->remaining_ms;
    if (t_kernel_wait && t_kernel_wait->restoring==1) {
        t_kernel_wait->restoring=0;
        return t_kernel_wait->remaining_ms;
    }
    return milliseconds;
}
void xbox_kernel_wait_apc_begin(uint32_t remaining) {
    GuestKernelWait *wait=t_kernel_wait;if(!wait)return;
    if(wait->restoring==3){g_esp=wait->resume_esp;wait->restoring=0;}
    wait->remaining_ms=remaining;wait->apc_started=GetTickCount64();
    recomp_cont_site(&wait->frame,(8ULL<<32)|wait->frame.function);
}
void xbox_kernel_wait_apc_end(void) {
    if(t_kernel_wait)recomp_cont_site(&t_kernel_wait->frame,0);
}
void xbox_kernel_wait_boundary(uint32_t remaining) {
    GuestKernelWait *wait=t_kernel_wait;
    if (!wait) return;
    wait->remaining_ms=remaining;
    recomp_cont_site(&wait->frame, (5ULL<<32) | wait->frame.function);
    ULONGLONG now=GetTickCount64();
    for(GuestKernelWait *parent=wait->parent;parent;parent=parent->parent) {
        if((parent->frame.site>>32)!=8)continue;
        ULONGLONG elapsed=now-parent->apc_started;parent->apc_started=now;
        if(parent->remaining_ms!=INFINITE)
            parent->remaining_ms=elapsed>=parent->remaining_ms?0:parent->remaining_ms-(uint32_t)elapsed;
    }
    checkpoint_poll();
    if (g_checkpoint_hook) g_checkpoint_hook();
    now=GetTickCount64();
    for(GuestKernelWait *parent=wait->parent;parent;parent=parent->parent)
        if((parent->frame.site>>32)==8)parent->apc_started=now;
    recomp_cont_site(&wait->frame,0);
}
static void bridge_run_main_inline(recomp_func_t fn,uint32_t routine,uint32_t ctx1,uint32_t ctx2)
{
    GuestKernelWait *wait=t_kernel_wait;
    int restoring=wait && wait->restoring==2;
    int saved_depth=restoring?(int)wait->main_saved_depth:t_dispatch_depth;
    if (wait && !t_checkpoint_thread) checkpoint_bind_thread(checkpoint_new_thread());
    if (!restoring) {
        g_esp-=4;BRIDGE_MEM32(g_esp)=ctx2;
        g_esp-=4;BRIDGE_MEM32(g_esp)=ctx1;
        g_esp-=4;BRIDGE_MEM32(g_esp)=0;
        if (wait) { wait->main_routine=routine;wait->main_saved_depth=(uint32_t)saved_depth; }
    }
    if (wait) {
        wait->restoring=0;
        recomp_cont_site(&wait->frame,(6ULL<<32)|wait->frame.function);
    }
    t_dispatch_depth=0;
    guest_cpu_join();fn();
    t_dispatch_depth=saved_depth;
    if (wait) recomp_cont_site(&wait->frame,0);
    g_esp+=12;
    fprintf(stderr,"  [KERNEL] PsCreateSystemThreadEx: main thread returned (g_eax=0x%08X)\n",g_eax);
    fflush(stderr);
}
static int bridge_restore_main_inline(void)
{
    if (!t_kernel_wait || t_kernel_wait->restoring!=2) return 0;
    recomp_func_t fn=recomp_lookup(t_kernel_wait->main_routine);
    if (!fn) fn=recomp_lookup_manual(t_kernel_wait->main_routine);
    if (!fn) recomp_cont_invalid();
    bridge_run_main_inline(fn,t_kernel_wait->main_routine,0,0);
    return 1;
}
typedef struct GuestCpuTiming {
    uint32_t thread_id, reserved;
    uint64_t join_ticks, yield_ticks, dispatch_ticks;
    uint64_t join_count, yield_count, dispatch_count;
} GuestCpuTiming;
static GuestCpuTiming g_guest_cpu_timing[64];
static LONG g_guest_cpu_timing_count;
static RECOMP_TLS GuestCpuTiming *t_guest_cpu_timing;
static uint64_t guest_timing_clock(void)
{
    LARGE_INTEGER now;
    if (!t_guest_cpu_timing) return 0;
    QueryPerformanceCounter(&now);
    return (uint64_t)now.QuadPart;
}

static int guest_cpu_enabled(void)
{
    if (g_guest_cpu_on < 0) {
        const char *e = getenv("RECOMP_GUEST_LOCK");
        InitializeCriticalSection(&g_guest_cpu);
        g_guest_cpu_on = e && *e == '1';
        if (g_guest_cpu_on) {
            g_guest_cpu_acquired = CreateEvent(NULL, FALSE, FALSE, NULL);
            if (!g_guest_cpu_acquired) {
                fprintf(stderr, "[STOP] guest CPU handoff event creation failed\n");
                exit(5);
            }
            fprintf(stderr, "  [KERNEL] guest CPU lock on: one guest thread runs at a time\n");
        }
    }
    return g_guest_cpu_on;
}

int guest_cpu_is_held(void) { return t_guest_held; }

/* Called once on each host thread that runs a guest thread, before its first
 * guest instruction. */
void guest_cpu_join(void)
{
    static int timing = -1;
    if (timing < 0) {
        const char *e = getenv("RECOMP_GPU_TIMING");
        timing = e && *e == '1';
    }
    if (timing && !t_guest_cpu_timing) {
        LONG index = InterlockedIncrement(&g_guest_cpu_timing_count) - 1;
        if (index >= 64) {
            fprintf(stderr, "[STOP] guest CPU timing thread capacity exceeded\n");
            exit(5);
        }
        t_guest_cpu_timing = &g_guest_cpu_timing[index];
        t_guest_cpu_timing->thread_id = GetCurrentThreadId();
    }
    t_guest_thread = 1;
    if (guest_cpu_enabled() && !t_guest_held) {
        uint64_t start = guest_timing_clock();
        InterlockedIncrement(&g_guest_cpu_waiters);
        EnterCriticalSection(&g_guest_cpu);
        InterlockedDecrement(&g_guest_cpu_waiters);
        if (ReadAcquire(&g_guest_cpu_handoff_waiters)) SetEvent(g_guest_cpu_acquired);
        if (t_guest_cpu_timing) {
            t_guest_cpu_timing->join_ticks += guest_timing_clock() - start;
            t_guest_cpu_timing->join_count++;
        }
        t_guest_held = 1;
        /* The next quantum starts when guest execution owns the CPU again. */
        t_guest_yield_tick = GetTickCount64();
    }
}

void guest_cpu_part(void)
{
    if (t_guest_held) {
        t_guest_held = 0;
        LeaveCriticalSection(&g_guest_cpu);
    }
    t_guest_thread = 0;
}

static void ke_prepare_event_wait(uint32_t object, HANDLE event, int type)
{
    if (!(type & KE_EVENT_TITLE_HEADER)) return;
    int held = t_guest_held, thread = t_guest_thread;
    if (!held) guest_cpu_join();
    /* XDK events can be reset by a direct header write. The initial header
     * seeded the native event when it was registered; a remaining one is not
     * another signal after an existing native waiter consumes that event. */
    if (!BRIDGE_MEM32(object + 4)) ResetEvent(event);
    if (!held) { guest_cpu_part(); t_guest_thread = thread; }
}

static void ke_finish_event_wait(uint32_t object, HANDLE event, int type)
{
    int held = t_guest_held, thread = t_guest_thread;
    if (!held) guest_cpu_join();
    ke_prepare_event_wait(object, event, type);
    /* Another signal may arrive after the native wait succeeds. Publish the
     * current native state; unconditionally writing zero would erase it. */
    BRIDGE_MEM32(object + 4) = ke_native_event_state(event);
    if (!held) { guest_cpu_part(); t_guest_thread = thread; }
}

void guest_cpu_yield(void)
{
    ULONGLONG now;
    if(recomp_cont_recovering(KERNEL_CONT_APC)) {
        xbox_DeliverThreadApcs(FALSE);return;
    }
    if (t_guest_thread && !t_dispatch_depth && ReadAcquire(&g_checkpoint_requested) &&
        recomp_cont_active()) checkpoint_poll();
    if (g_checkpoint_hook && t_guest_thread && t_guest_held &&
        !t_dispatch_depth && recomp_cont_active())
        g_checkpoint_hook();
    if (t_guest_held) nv2a_pb_request_progress();
    if (!t_guest_thread || !t_guest_held || t_dispatch_depth)
        return;
    xbox_DeliverThreadApcs(FALSE);
    if (!ReadAcquire(&g_guest_cpu_waiters) && !ReadAcquire(&g_dpc_cpu_waiters))
        return;
    now = GetTickCount64();
    /* Deferred work runs above passive guest execution. Its existing CPU
     * join request is eligible at this backedge, before an ordinary quantum. */
    if (now - t_guest_yield_tick < 10 && !g_dpc_cpu_waiters)
        return;
    if (xbox_CurrentIrql() >= DISPATCH_LEVEL || xbox_IrqlBlocksInterrupts())
        return;
    /* A released critical section can be reacquired by its previous owner
     * before a waiting thread wakes. Hand execution to an existing waiter
     * before rejoining, without imposing a host timer delay. */
    int handoff = InterlockedCompareExchange(&g_guest_cpu_waiters, 0, 0) > 0;
    if (handoff) {
        InterlockedIncrement(&g_guest_cpu_handoff_waiters);
        ResetEvent(g_guest_cpu_acquired);
    }
    t_guest_held = 0;
    LeaveCriticalSection(&g_guest_cpu);
    uint64_t start = guest_timing_clock();
    if (handoff) {
        DWORD result=WaitForSingleObject(g_guest_cpu_acquired, INFINITE);
        InterlockedDecrement(&g_guest_cpu_handoff_waiters);
        if (result != WAIT_OBJECT_0) {
            fprintf(stderr, "[STOP] guest CPU handoff wait failed\n");
            exit(5);
        }
    }
    if (t_guest_cpu_timing) {
        t_guest_cpu_timing->yield_ticks += guest_timing_clock() - start;
        t_guest_cpu_timing->yield_count++;
    }
    guest_cpu_join();
}

/* A guest thread can yield at a kernel call only below DISPATCH_LEVEL. */
static void kernel_thunk_dispatch(void)
{
    GuestKernelWait wait;
    int tracked = recomp_cont_active();
    if(tracked)kernel_wait_enter(&wait,(uint32_t)g_kernel_dispatch_slot);
    int held = t_guest_held;
    if(tracked && wait.restoring==4)goto final_apcs;
    /* Object mutations and original callbacks are part of the guest CPU
     * operation. Releasing before them races workers using those same objects.
     * Critical-section entry already releases only on its blocking path. */
    int retain_cpu = 0;
    if (g_kernel_dispatch_slot >= 0 && g_kernel_dispatch_slot < XBOX_KERNEL_THUNK_TABLE_SIZE) {
        switch (g_slot_ordinals[g_kernel_dispatch_slot]) {
        case 44:                         /* interrupt vector result */
        case 65: case 68:                /* device object lifecycle */
        case 59: case 60: case 61: case 62: case 72: case 73: case 74: case 77:
        case 84: case 85: case 86: case 87: case 359: /* IRP state and dispatch */
        case 81: case 82: case 83:       /* driver packet queues and callbacks */
        case 185: case 194: case 216: case 229: /* handle timers share the scheduler and APC lists */
        case 97: case 98: case 100:       /* timer cancel, interrupt ownership */
        case 106: case 107: case 108: case 109: case 113: /* object initialization */
        case 101: case 105: case 118: case 122: /* APC state */
        case 79: case 111: case 116: case 117: case 136: case 141: case 191: case 223: case 227: /* completion queues */
        case 187: case 197: case 206:     /* queue handles and cross-thread APCs */
        case 67: case 69: case 203: case 215: /* symbolic-link namespace and target lifetime */
        case 188: case 189: case 192: case 193: case 201: /* object names and directory identity */
        case 242: case 243: case 244: case 247:
        case 246: case 248: case 250: case 251: /* object reference lifetime */
        case 124: case 143: case 144:     /* thread object properties */
        case 158: case 159: case 230: case 233: case 234: case 235: /* retain objects before releasing to wait */
        case 254: case 255: case 258:     /* thread creation and root transfer */
        case 119: case 137:              /* DPC queue ownership */
        case 114: case 115: case 133: case 134: case 135: /* device queues */
        case 123: case 138: case 145: case 186: case 205: case 225: /* event signal state */
        case 129: case 160: case 161:     /* IRQL transitions */
        case 149: case 150: case 153:     /* timers and synchronized callback */
        case 277: case 278: case 291: case 294: case 295: /* critical sections */
        case 26: case 27: case 265: case 302: case 303: case 312:              /* original exception dispatch */
            retain_cpu = 1;
            break;
        }
    }
    if (held && !retain_cpu && xbox_CurrentIrql() < DISPATCH_LEVEL) {
        t_guest_held = 0;
        LeaveCriticalSection(&g_guest_cpu);
    }
    t_dispatch_depth++;
    if (tracked) wait.dispatch_started=1;
    uint64_t start = guest_timing_clock();
    uint32_t load_caller = t_guest_cpu_timing && g_esp ? BRIDGE_MEM32(g_esp) : 0;
    int load_slot = g_kernel_dispatch_slot;
    ULONGLONG load_started = t_guest_cpu_timing ? GetTickCount64() : 0;
    kernel_thunk_dispatch_body();
    if (t_guest_cpu_timing) {
        ULONGLONG load_elapsed = GetTickCount64()-load_started;
        if (load_elapsed >= 100)
            fprintf(stderr, "  [KWAIT] t_ms=%llu tid=%lu ordinal=%u from=%08X elapsed_ms=%llu result=%08X\n",
                    (unsigned long long)load_started, (unsigned long)GetCurrentThreadId(),
                    load_slot >= 0 && load_slot < XBOX_KERNEL_THUNK_TABLE_SIZE ? g_slot_ordinals[load_slot] : 0,
                    load_caller, (unsigned long long)load_elapsed, g_eax);
        t_guest_cpu_timing->dispatch_ticks += guest_timing_clock() - start;
        t_guest_cpu_timing->dispatch_count++;
    }
    t_dispatch_depth--;
    if (tracked) wait.dispatch_started=0;
    /* Back to guest code: take the CPU again. Only at the outermost call --
     * a guest callback a bridge runs (an APC) returns into the bridge, not to
     * the thread's own code -- and only if the thread still exists as one. */
    if (t_guest_thread && !t_guest_held && t_dispatch_depth == 0)
        guest_cpu_join();
final_apcs:
    if (!t_dispatch_depth && t_guest_thread) {
        if(tracked)recomp_cont_site(&wait.frame,(9ULL<<32)|wait.frame.function);
        xbox_DeliverThreadApcs(FALSE);
    }
    if (tracked) recomp_cont_leave(&wait.frame);
    (void)held;
}

static void kernel_thunk_dispatch_body(void)
{
    int slot = g_kernel_dispatch_slot;
    bridge_func_t bridge;
    ULONG ordinal;
    uint32_t _watch_before = 0;

    if (slot < 0 || slot >= XBOX_KERNEL_THUNK_TABLE_SIZE) {
        fprintf(stderr, "  [KERNEL] bad slot %d\n", slot);
        g_eax = 0;
        g_esp += 4;  /* pop dummy return address */
        return;
    }

    ordinal = g_slot_ordinals[slot];
    bridge = g_slot_bridges[slot];
    if(t_kernel_wait && t_kernel_wait->restoring==5) {
        _watch_before=t_kernel_wait->watch_before;t_kernel_wait->restoring=0;
        goto bridge_invoke;
    }
    if (t_kernel_wait && t_kernel_wait->restoring==2) {
        if (ordinal!=255 || !t_kernel_wait->main_routine) recomp_cont_invalid();
        _watch_before=t_kernel_wait->watch_before;
        bridge_PsCreateSystemThreadEx();
        goto bridge_completed;
    }

    g_kernel_call_count++;
    if (ordinal < XBOX_KERNEL_THUNK_TABLE_SIZE)
        g_ordinal_calls[ordinal]++;

    if (KERNEL_LOG_ON()) {
        /* The guest return address sits at the top of the guest stack: the
         * caller pushed it before dispatching here. Logging it turns "some
         * function is calling this" into "this call site is", which is the
         * difference between guessing and knowing when a title recurses. */
        fprintf(stderr,
                "  [KERNEL] #%lld: ordinal %u (slot %d) esp=0x%08X ret=0x%08X\n",
                g_kernel_call_count, ordinal, slot, g_esp,
                g_esp ? BRIDGE_MEM32(g_esp) : 0);
        fflush(stderr);
    }

    {
        static DWORD last_summary_tick = 0;
        DWORD now = GetTickCount();
        if (last_summary_tick == 0) last_summary_tick = now;
        if (now - last_summary_tick >= 2000 && g_kernel_call_count > 200) {
            char summary[1024];
            int length=snprintf(summary,sizeof summary, "  [KERNEL] summary: %lld total calls, latest ordinal %u (slot %d) esp=0x%08X ret=0x%08X\n",
                    g_kernel_call_count, ordinal, slot, g_esp,
                    g_esp ? BRIDGE_MEM32(g_esp) : 0);
            /* And which ones, ranked. "Latest" names whatever the sample
             * happened to land on; the question behind this line is what a
             * title sitting still is actually asking the kernel for, and
             * that wants counting rather than sampling. */
            {
                static unsigned char shown_ord[XBOX_KERNEL_THUNK_TABLE_SIZE];
                int r, shown;

                memset(shown_ord, 0, sizeof shown_ord);
                for (shown = 0; shown < 6; shown++) {
                    int best = -1;
                    for (r = 0; r < XBOX_KERNEL_THUNK_TABLE_SIZE; r++)
                        if (g_ordinal_calls[r] && !shown_ord[r]
                            && (best < 0 || g_ordinal_calls[r] > g_ordinal_calls[best]))
                            best = r;
                    if (best < 0)
                        break;
                    shown_ord[best] = 1;
                    length+=snprintf(summary+length,sizeof summary-(size_t)length, "  [KERNEL]   ordinal %3d x%llu\n", best,
                            (unsigned long long)g_ordinal_calls[best]);
                }
            }
            fwrite(summary,1,(size_t)length,stderr);
            fflush(stderr);
            last_summary_tick = now;
        }
    }

    /* Pop the dummy return address that PUSH32(esp, 0) pushed before RECOMP_ICALL.
     * On real x86, "call [thunk]" pushes a real return address and "ret" pops it.
     * In our model, the bridge is called directly (not via the simulated stack),
     * so we must manually consume the dummy return address. */
    /* Keep the caller before popping it. A bridge that blocks -- a critical
     * section above all -- can then say which guest call site is holding the
     * lock, which is the one fact a deadlock report otherwise lacks. */
    g_xbox_kernel_caller = g_esp ? BRIDGE_MEM32(g_esp) : 0;
    g_esp += 4;

    /* Name the bridge that corrupts a watched dword.
     *
     * A bridge hands Xbox pointers to real Win32 calls, so a bad one has
     * Windows write into Xbox memory -- the resulting wild write has a stack
     * inside ntdll with no recompiled frame to blame, and a watchpoint just
     * says "something changed". Sampling either side of the call names the
     * ordinal directly, which is the one fact those tools cannot give.
     *
     * Set g_kernel_watch_va to arm; zero (the default) costs one compare. */
    kernel_watch_arm_once();
    if (g_kernel_watch_va) {
        _watch_before = BRIDGE_MEM32(g_kernel_watch_va);
        /* Reporting only on change misses the case that matters most: a value
         * that was already wrong before the first call sampled it. Printing
         * every sample under RECOMP_KERNEL_WATCH_ALL shows when it changed
         * even if no single bridge did it. */
        if (getenv("RECOMP_KERNEL_WATCH_ALL")) {
            static uint32_t seen = 0xDEADBEEFu;
            if (_watch_before != seen) {
                seen = _watch_before;
                fprintf(stderr, "  [KWATCH] 0x%08X = %08X before ordinal %u"
                                " (call #%lld)\n",
                        g_kernel_watch_va, _watch_before, ordinal,
                        g_kernel_call_count);
                fflush(stderr);
            }
        }
    }

    if (t_kernel_wait) t_kernel_wait->watch_before=_watch_before;
bridge_invoke:
    if (bridge) {
        bridge();
    } else {
        fprintf(stderr, "[STOP] no kernel bridge for ordinal %u (slot %d), caller 0x%08X\n",
                ordinal, slot, g_xbox_kernel_caller);
        fflush(stderr);
        exit(4);
    }

bridge_completed: ;
    /* Clean stdcall args from the simulated stack.
     * On real x86, stdcall callee does "ret N" to pop the return address
     * and N bytes of arguments. We already popped the dummy return address
     * above; now pop the args. */
    g_esp += g_slot_arg_bytes[slot];

    if (g_kernel_watch_va) {
        uint32_t _after = BRIDGE_MEM32(g_kernel_watch_va);
        if (_after != _watch_before) {
            fprintf(stderr,
                    "  [KWATCH] ordinal %u changed Xbox VA 0x%08X: "
                    "%08X -> %08X\n",
                    ordinal, g_kernel_watch_va, _watch_before, _after);
            fflush(stderr);
        }
    }

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] → returned 0x%08X\n", g_eax);
        fflush(stderr);
    }
}

/* ── Dispatch lookup ────────────────────────────────────── */

/**
 * Look up a kernel thunk by synthetic VA.
 * Called as a fallback when recomp_lookup() returns NULL.
 */
recomp_func_t recomp_lookup_kernel(uint32_t xbox_va)
{
    if(xbox_va==KERNEL_EXCEPTION_NESTED_GUARD)return bridge_exception_nested_guard;
    if(xbox_va==KERNEL_UNWIND_COLLISION_GUARD)return bridge_unwind_collision_guard;
    if (xbox_va >= KERNEL_VA_BASE && xbox_va < KERNEL_VA_END) {
        int slot = (xbox_va - KERNEL_VA_BASE) / 4;
        if (slot >= 0 && slot < XBOX_KERNEL_THUNK_TABLE_SIZE) {
            g_kernel_dispatch_slot = slot;
            return kernel_thunk_dispatch;
        }
    }
    return NULL;
}

/* ── Initialization ─────────────────────────────────────── */

/*
 * Where this title's kernel thunk table lives. Defaults to the compile-time
 * constant, but every XBE puts it somewhere different (it comes from the
 * header's KernelImageThunkAddress), so xbox_MemoryLayoutInit() parses the
 * real address out of the binary and overrides it here.
 *
 * Halo build 2276 puts it at 0x00253090 against the default's 0x0036B7C0 --
 * without the override the bridge patches ordinals into whatever happens to
 * live at the wrong address and every kernel call goes somewhere arbitrary.
 */
static uint32_t g_thunk_table_base  = XBOX_KERNEL_THUNK_TABLE_BASE;
static uint32_t g_thunk_table_count = XBOX_KERNEL_THUNK_TABLE_SIZE;

/**
 * Return the kernel thunk table address and entry count currently in effect.
 * The address is parsed from the XBE header during memory layout init, so the
 * values are per-title, not hardcoded. *base/*count are set to 0 if unavailable.
 */
void xbox_kernel_get_thunk_address(uint32_t *xbox_va, uint32_t *count)
{
    if (xbox_va) *xbox_va = g_thunk_table_base;
    if (count)  *count  = g_thunk_table_count;
}

void xbox_kernel_set_thunk_address(uint32_t xbox_va, uint32_t count)
{
    if (!xbox_va) {
        return;
    }

    g_thunk_table_base = xbox_va;

    /* count indexes g_slot_* arrays, which are sized by the macro. A title
     * importing more slots than the real kernel exports would run off them. */
    if (count && count <= XBOX_KERNEL_THUNK_TABLE_SIZE) {
        g_thunk_table_count = count;
    } else if (count > XBOX_KERNEL_THUNK_TABLE_SIZE) {
        fprintf(stderr,
                "  Kernel thunk bridge: XBE declares %u thunk slots, clamping to %d\n",
                count, XBOX_KERNEL_THUNK_TABLE_SIZE);
        g_thunk_table_count = XBOX_KERNEL_THUNK_TABLE_SIZE;
    }
}

/**
 * Resolve the kernel thunk table in Xbox memory.
 *
 * Must be called AFTER xbox_MemoryLayoutInit() so Xbox memory is mapped.
 *
 * Reads the actual ordinals from the XBE memory thunk table (0x80000000|ordinal),
 * resolves each to a per-ordinal bridge function, and replaces the entry
 * with a synthetic VA for dispatch.
 */
/* Per-title ordinal remap; NULL = identity (the kernel's own XDK). Set by
 * xbox_kernel_set_ordinal_remap before init. See kernel.h. */
static const unsigned short *g_ordinal_remap = NULL;
static int g_ordinal_remap_count = 0;

void xbox_kernel_set_ordinal_remap(const unsigned short *map, int count)
{
    g_ordinal_remap = map;
    g_ordinal_remap_count = count;
}

void xbox_kernel_bridge_init(void)
{
    int i;
    int resolved = 0;
    int bridged = 0;
    int unbridged = 0;
    DWORD old_protect;

    guest_cpu_join();          /* the caller goes on to run the title's entry point */
    fprintf(stderr, "  Kernel thunk bridge: resolving %d entries at 0x%08X\n",
            g_thunk_table_count, g_thunk_table_base);

    /* The thunk table lives in .rdata which is marked PAGE_READONLY.
     * Temporarily make it writable so we can patch the ordinals. */
    VirtualProtect(
        (LPVOID)((uintptr_t)g_thunk_table_base + g_xbox_mem_offset),
        g_thunk_table_count * 4,
        PAGE_READWRITE,
        &old_protect
    );

    /* Initialize kernel data export values first */
    kernel_data_init();
    if(xbox_kernel_init_devices()<0) {
        fprintf(stderr,"  [KERNEL] device namespace initialization failed\n");recomp_cont_invalid();
    }

    for (i = 0; i < g_thunk_table_count; i++) {
        uint32_t va = g_thunk_table_base + i * 4;
        uint32_t current = BRIDGE_MEM32(va);

        if (current & 0x80000000) {
            /* Read the actual ordinal from Xbox memory, then translate it into
             * the kernel's canonical ordinal space. Identity unless the title
             * set a remap (a different XDK). Every routing decision below --
             * data export, bridge, arg size -- keys off the canonical ordinal,
             * so one translation here covers all three. */
            ULONG ordinal = current & 0x7FFFFFFF;
            if (g_ordinal_remap && ordinal < (ULONG)g_ordinal_remap_count
                && g_ordinal_remap[ordinal]) {
                ordinal = g_ordinal_remap[ordinal];
            }
            g_slot_ordinals[i] = ordinal;

            /* Check if this is a data export */
            uint32_t data_va = kernel_data_va_for_ordinal(ordinal);
            if (data_va) {
                /* DATA export: point thunk to actual data in mapped memory.
                 * This allows the game to dereference the thunk entry. */
                BRIDGE_MEM32(va) = data_va;
                resolved++;
                bridged++;
                continue;
            }

            /* FUNCTION export: use synthetic VA for dispatch */
            g_slot_bridges[i] = bridge_for_ordinal(ordinal);
            { int _n = stdcall_args_for_ordinal(ordinal);
              g_slot_arg_unknown[i] = (uint8_t)(_n < 0);
              g_slot_arg_bytes[i] = (_n < 0) ? 0 : _n; }
            if (g_slot_bridges[i]) {
                bridged++;
            } else {
                unbridged++;
                fprintf(stderr, "  [KERNEL] unbridged function thunk: ordinal %lu"
                        " (slot %u, VA 0x%08X)\n",
                        (unsigned long)ordinal, i, KERNEL_VA_BASE + i * 4);
            }

            /* Replace Xbox memory entry with synthetic VA */
            uint32_t synthetic = KERNEL_VA_BASE + i * 4;
            BRIDGE_MEM32(va) = synthetic;
            resolved++;
        }
    }

    /*
     * Thunk entries below the header-declared base.
     *
     * KernelImageThunkAddress points at the main import run, but the linker can
     * emit further runs just before it, separated by a NULL. Halo has three at
     * base-0x10 (ordinals 52, 51 and 5). Those stay unpatched, so game code
     * doing "mov ebx,[thunk]; call ebx" jumps to the raw 0x8000xxxx marker
     * instead of a kernel function. The indirect call cannot resolve it, yields
     * 0, and a caller looping until it sees an error code never sees one - in
     * Halo that hung main() in a file-enumeration loop before it reached any
     * initialisation.
     *
     * Only entries still carrying the ordinal marker are touched, so scanning
     * back over unrelated .rdata is harmless.
     */
    {
        const int LOOKBEHIND = 16;   /* entries, i.e. 64 bytes */
        DWORD scan_protect;
        uint32_t low = g_thunk_table_base - LOOKBEHIND * 4;

        VirtualProtect((LPVOID)((uintptr_t)low + g_xbox_mem_offset),
                       LOOKBEHIND * 4, PAGE_READWRITE, &scan_protect);

        for (i = 1; i <= LOOKBEHIND; i++) {
            uint32_t va = g_thunk_table_base - i * 4;
            uint32_t current = BRIDGE_MEM32(va);
            int slot;

            if (!(current & 0x80000000)) {
                continue;            /* NULL separator or ordinary data */
            }
            slot = g_thunk_table_count + i;   /* park these above the main run */
            if (slot >= XBOX_KERNEL_THUNK_TABLE_SIZE) {
                break;
            }

            g_slot_ordinals[slot] = current & 0x7FFFFFFF;
            g_slot_bridges[slot] = bridge_for_ordinal(g_slot_ordinals[slot]);
            { int _n = stdcall_args_for_ordinal(g_slot_ordinals[slot]);
              g_slot_arg_unknown[slot] = (uint8_t)(_n < 0);
              g_slot_arg_bytes[slot] = (_n < 0) ? 0 : _n; }
            BRIDGE_MEM32(va) = KERNEL_VA_BASE + slot * 4;
            resolved++;
            if (g_slot_bridges[slot]) bridged++; else unbridged++;

            fprintf(stderr, "  [KERNEL] extra thunk at 0x%08X: ordinal %u (slot %d)\n",
                    va, g_slot_ordinals[slot], slot);
        }

        VirtualProtect((LPVOID)((uintptr_t)low + g_xbox_mem_offset),
                       LOOKBEHIND * 4, scan_protect, &scan_protect);
    }

    /* Restore original protection */
    VirtualProtect(
        (LPVOID)((uintptr_t)g_thunk_table_base + g_xbox_mem_offset),
        g_thunk_table_count * 4,
        old_protect,
        &old_protect
    );

    fprintf(stderr, "  Kernel thunk bridge: %d/%d resolved (%d bridged, %d stub)\n",
            resolved, g_thunk_table_count, bridged, unbridged);
    fprintf(stderr, "  Synthetic VA range: 0x%08X-0x%08X\n",
            KERNEL_VA_BASE, KERNEL_VA_BASE + (resolved - 1) * 4);

}
