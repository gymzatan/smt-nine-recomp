/*
 * kernel_hal.c - Hardware Abstraction Layer
 *
 * Implements IRQL simulation, performance counters, system time,
 * processor stalls, bug checks, floating point state, and hardware stubs.
 *
 * The Xbox HAL provides low-level hardware access that doesn't exist on
 * a standard Windows PC. Most of these functions are either:
 *   - Directly mappable (perf counters, system time)
 *   - Simulated (IRQL tracking via TLS)
 *   - Device-backed where connected (NV2A PCI configuration)
 *   - Still incomplete (other PCI devices, SMC, interrupt delivery)
 */

#include "kernel.h"
#include "xbox_memory_layout.h"   /* RECOMP_TLS, g_fs_base */
#include "../nv2a/nv2a_state.h"
#include <stdio.h>
#include <stdlib.h>
#if defined(_WIN32)
#include <intrin.h>
#endif
#if defined(_MSC_VER) && !defined(__clang__)
#pragma intrinsic(_ReturnAddress)
#define IRQL_CALLER() _ReturnAddress()
#else
#define IRQL_CALLER() __builtin_return_address(0)
#endif

/* ============================================================================
 * IRQL Simulation
 *
 * Xbox uses IRQL (Interrupt Request Level) for synchronization:
 *   PASSIVE_LEVEL (0) - normal thread execution
 *   APC_LEVEL (1) - APC delivery
 *   DISPATCH_LEVEL (2) - scheduler/DPC level, no page faults allowed
 *
 * On Windows, we simulate IRQL with a thread-local variable. Raising to
 * DISPATCH_LEVEL doesn't actually prevent preemption, but the tracking
 * allows code that checks IRQL to function correctly.
 * ============================================================================ */

static XBOX_THREAD_LOCAL KIRQL g_current_irql = PASSIVE_LEVEL;

/* The current IRQL, where guest code reads it: KPCR.Irql, fs:[0x24].
 *
 * The XDK does not always ask the kernel. DirectSound's lock
 * (sub_002F4175 in Burnout 3) reads fs:[0x24] directly and skips its
 * critical section at raised IRQL, because a DPC must never block. Nothing
 * wrote that byte, so it read 0 everywhere: DirectSound's DPC took the
 * critical section, blocked against the thread that held it, and the title
 * froze with IRQL raised on the blocked thread. Every change of
 * g_current_irql is published here. */
extern RECOMP_TLS uint32_t g_fs_base;
/* The offset lifted code adds to every guest address; host code that reads
 * guest memory on its behalf uses the same one. */
extern ptrdiff_t g_xbox_mem_offset;

static void irql_publish(void)
{
    if (g_fs_base)
        *(volatile uint8_t *)((uintptr_t)g_xbox_mem_offset + g_fs_base
                              + 0x24) = (uint8_t)g_current_irql;
}

/* How many threads are holding IRQL at or above DISPATCH_LEVEL.
 *
 * The level itself is per-thread, which is right for a guest that asks "what
 * is my IRQL". It is wrong for the question a device model has to answer:
 * raising IRQL on hardware masks the interrupt for the whole processor, and
 * the title raises it precisely to keep an ISR out of structures it is in the
 * middle of editing. With the level thread-local, a controller thread sees
 * PASSIVE_LEVEL, calls the ISR anyway, and the two race over exactly the
 * state the guest was protecting -- which surfaces as an intermittent fault
 * on a garbage pointer, far from the code that dropped it.
 *
 * A count rather than a flag, because several threads can be raised at once
 * and the last one out is what re-opens the gate. */
static volatile LONG g_irql_raised_count = 0;

/* CPU IF is independent of IRQL. Host guest threads retain their own flag;
 * the device gate remains closed while any of them masks the processor.
 * STI sets IF immediately but keeps maskable IRQs out through the following
 * instruction. Repeated CLI/STI do not add extra mask ownership. */
static XBOX_THREAD_LOCAL int g_cpu_if = 1;
static XBOX_THREAD_LOCAL int g_cpu_sti_shadow = 0;
static volatile LONG g_cpu_irq_mask_count = 0;

int xbox_CpuInterruptFlag(void) { return g_cpu_if; }
int xbox_CpuInterruptState(void) { return g_cpu_if | (g_cpu_sti_shadow << 1); }

void xbox_CpuCli(void)
{
    if (g_cpu_if && !g_cpu_sti_shadow)
        InterlockedIncrement(&g_cpu_irq_mask_count);
    g_cpu_if = 0;
    g_cpu_sti_shadow = 0;
}

KIRQL xbox_CurrentIrql(void) { return g_current_irql; }

void xbox_IrqlBindThreadPcr(uint32_t pcr)
{
    g_fs_base = pcr;
    irql_publish();
}

void xbox_CpuSti(void)
{
    if (!g_cpu_if) {
        g_cpu_if = 1;
        g_cpu_sti_shadow = 1;
    }
}

void xbox_CpuCompleteStiShadow(void)
{
    if (g_cpu_sti_shadow) {
        g_cpu_sti_shadow = 0;
        InterlockedDecrement(&g_cpu_irq_mask_count);
    }
}

int xbox_CpuEnterInterrupt(void)
{
    int saved = g_cpu_if | (g_cpu_sti_shadow << 1);
    xbox_CpuCli();
    return saved;
}

void xbox_CpuLeaveInterrupt(int saved)
{
    int blocked = !g_cpu_if || g_cpu_sti_shadow;
    int restored_if = saved & 1, restored_shadow = (saved >> 1) & 1;
    int restored_blocked = !restored_if || restored_shadow;
    if (restored_blocked && !blocked)
        InterlockedIncrement(&g_cpu_irq_mask_count);
    g_cpu_if = restored_if;
    g_cpu_sti_shadow = restored_shadow;
    if (blocked && !restored_blocked)
        InterlockedDecrement(&g_cpu_irq_mask_count);
}

/* Non-zero while any thread is at or above DISPATCH_LEVEL. Device models call
 * this before delivering an interrupt; OHCI and the NV2A are level-triggered,
 * so a deferred interrupt is delivered on the next poll rather than lost. */
int xbox_IrqlBlocksInterrupts(void)
{
    return InterlockedCompareExchange(&g_irql_raised_count, 0, 0) != 0 ||
           InterlockedCompareExchange(&g_cpu_irq_mask_count, 0, 0) != 0;
}

/* The raw depth, for callers that want to report it. A count that only ever
 * grows is a leak somewhere in the raise/lower pairs, and the number says so
 * where a yes/no cannot. */
int xbox_IrqlRaisedCount(void)
{
    return (int)InterlockedCompareExchange(&g_irql_raised_count, 0, 0);
}

static int  s_trace = -1;
static volatile LONG s_traced = 0;

/* Every crossing of the DISPATCH boundary, ever.
 *
 * The depth on its own cannot tell a leaked raise from a guest that is
 * genuinely sitting there: both read non-zero for as long as you look. A
 * count that stops moving says the first; one that races says the second.
 * That distinction is the whole difference between "the title is busy" and
 * "no device will ever get an interrupt again". */
static volatile LONG s_transitions = 0;

int xbox_IrqlTransitions(void)
{
    return (int)InterlockedCompareExchange(&s_transitions, 0, 0);
}

/* Which threads are holding the boundary up, and where they raised.
 *
 * A depth that stops changing has to be attributed to code, and the raise
 * that did it happened seconds earlier on a thread that has since gone
 * quiet -- there is nothing left to look at by the time anyone notices.
 * Keeping the caller's address per raised thread turns the number into a
 * name: these are host addresses inside the recompiled image, so nm resolves
 * them to the generated function, which is the guest function. */
#define IRQL_HOLDERS 8
static struct {
    volatile LONG tid;
    void *ra;
    uint32_t guest_ra, guest_esp;   /* the host ra only ever names the bridge */
} s_holders[IRQL_HOLDERS];
extern RECOMP_TLS uint32_t g_esp;

static void irql_holder_add(void *ra)
{
    LONG me = (LONG)GetCurrentThreadId();
    int i;

    for (i = 0; i < IRQL_HOLDERS; i++)
        if (InterlockedCompareExchange(&s_holders[i].tid, me, 0) == 0) {
            s_holders[i].ra = ra;
            s_holders[i].guest_esp = g_esp;
            s_holders[i].guest_ra = g_esp
                ? *(uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp) : 0;
            return;
        }
}

static void irql_holder_drop(void)
{
    LONG me = (LONG)GetCurrentThreadId();
    int i;

    for (i = 0; i < IRQL_HOLDERS; i++)
        if (InterlockedCompareExchange(&s_holders[i].tid, 0, me) == me)
            return;
}

void xbox_IrqlDumpHolders(void)
{
    int i;

    fprintf(stderr, "  [IRQLHOLD] depth=%d transitions=%d, raised threads:\n",
            xbox_IrqlRaisedCount(), xbox_IrqlTransitions());
    for (i = 0; i < IRQL_HOLDERS; i++) {
        LONG t = InterlockedCompareExchange(&s_holders[i].tid, 0, 0);
        if (t)
            fprintf(stderr, "  [IRQLHOLD]   tid %lu raised from host %p, "
                    "guest ret %08X (esp %08X)\n",
                    (unsigned long)t, s_holders[i].ra,
                    s_holders[i].guest_ra, s_holders[i].guest_esp);
    }
    fflush(stderr);
}

static void irql_track(KIRQL old_level, KIRQL new_level, void *ra)
{
    int was = (old_level >= DISPATCH_LEVEL);
    int now = (new_level >= DISPATCH_LEVEL);
    LONG d;

    if (now == was)
        return;

    d = now ? InterlockedIncrement(&g_irql_raised_count)
            : InterlockedDecrement(&g_irql_raised_count);
    InterlockedIncrement(&s_transitions);
    if (now)
        irql_holder_add(ra);
    else
        irql_holder_drop();

    /* RECOMP_IRQL_TRACE prints the first few transitions. The pairing is what
     * matters: a raise to 2 followed by a lower from 2 nets out, and a lower
     * whose old level is not the level the raise set is a calling-convention
     * bug upstream of here, not a title doing something exotic. */
    if (s_trace < 0)
        s_trace = getenv("RECOMP_IRQL_TRACE") ? 1 : 0;
    if (s_trace && InterlockedIncrement(&s_traced) <= 20) {
        fprintf(stderr, "  [IRQL] tid %lu %s %d->%d depth=%ld\n",
                (unsigned long)GetCurrentThreadId(),
                now ? "raise" : "lower", old_level, new_level, (long)d);
        fflush(stderr);
    }
}

/* Bracket a host-delivered ISR or DPC.
 *
 * The kernel's interrupt and DPC dispatchers put the processor back at the
 * interrupted IRQL when the routine returns, whatever the routine left it at.
 * The host threads that deliver interrupts and drain DPCs here had no such
 * epilogue, so a routine that returned at DISPATCH_LEVEL left its thread
 * there and the global depth at one -- and every later USB interrupt then
 * sat out the forced-delivery timeout, a pad polled twice a second.
 *
 * They also run the routine at the level it expects -- DISPATCH_LEVEL for a
 * DPC, the device level for an ISR -- and code checks: see irql_publish. */
int xbox_IrqlEnterInterrupt(int level)
{
    int saved = (int)g_current_irql;
    if (g_current_irql != (KIRQL)level) {
        irql_track(g_current_irql, (KIRQL)level, IRQL_CALLER());
        g_current_irql = (KIRQL)level;
        irql_publish();
    }
    return saved;
}

void xbox_IrqlLeaveInterrupt(int saved)
{
    if (g_current_irql != (KIRQL)saved) {
        irql_track(g_current_irql, (KIRQL)saved, IRQL_CALLER());
        g_current_irql = (KIRQL)saved;
        irql_publish();
    }
}

/*
 * KfRaiseIrql - Raises IRQL to the specified level.
 * Returns the previous IRQL. Uses __fastcall (ECX = NewIrql).
 */
KIRQL __fastcall xbox_KfRaiseIrql(KIRQL NewIrql)
{
    KIRQL old = g_current_irql;

    if (NewIrql < old) {
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_HAL,
            "KfRaiseIrql: attempt to lower IRQL from %d to %d (use KfLowerIrql)",
            old, NewIrql);
    }

    irql_track(old, NewIrql, IRQL_CALLER());
    g_current_irql = NewIrql;
    irql_publish();
    return old;
}

/*
 * KfLowerIrql - Lowers IRQL to the specified level.
 * Uses __fastcall (ECX = NewIrql).
 */
VOID __fastcall xbox_KfLowerIrql(KIRQL NewIrql)
{
    if (NewIrql > g_current_irql) {
        /* A lower that raises the count.
         *
         * irql_track works on the edge, so this call crosses the boundary
         * upwards and increments. The pair that would bring it back down has
         * already happened, so the depth is now permanently one too high --
         * and the count is what every device model reads before delivering.
         * One of these ends interrupts for the rest of the run.
         *
         * Loud rather than a filtered warning because of that consequence:
         * the symptom is a device going silent minutes later, with nothing
         * connecting it back to here. */
        static volatile LONG n;
        if (InterlockedIncrement(&n) <= 20) {
            fprintf(stderr, "  [IRQLBUG] tid %lu KfLowerIrql(%d) while at %d"
                    " -- counted as a raise, depth is now stuck\n",
                    (unsigned long)GetCurrentThreadId(),
                    (int)NewIrql, (int)g_current_irql);
            fflush(stderr);
        }
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_HAL,
            "KfLowerIrql: attempt to raise IRQL from %d to %d (use KfRaiseIrql)",
            g_current_irql, NewIrql);
    }

    irql_track(g_current_irql, NewIrql, IRQL_CALLER());
    g_current_irql = NewIrql;
    irql_publish();
}

/*
 * KeRaiseIrqlToDpcLevel - Convenience function to raise to DISPATCH_LEVEL.
 */
KIRQL __stdcall xbox_KeRaiseIrqlToDpcLevel(void)
{
    KIRQL old = g_current_irql;

    irql_track(old, DISPATCH_LEVEL, IRQL_CALLER());
    g_current_irql = DISPATCH_LEVEL;
    irql_publish();
    return old;
}

/* ============================================================================
 * KeTickCount
 *
 * Exported as a data pointer, not a function. The Xbox kernel increments
 * this every ~1ms (approximating the Xbox tick interval).
 * Updated lazily when read, using GetTickCount.
 * ============================================================================ */

volatile ULONG xbox_KeTickCount = 0;
static uint64_t s_clock_tick_bias,s_clock_frozen_tick;
static LONGLONG s_clock_qpc_bias,s_clock_frozen_qpc;
static volatile LONG s_clock_frozen;

uint64_t xbox_RuntimeTickCount64(void)
{
    return s_clock_frozen?s_clock_frozen_tick:GetTickCount64()+s_clock_tick_bias;
}
void xbox_RuntimePerformanceCounter(LARGE_INTEGER *counter)
{
    if(s_clock_frozen) counter->QuadPart=s_clock_frozen_qpc;
    else {QueryPerformanceCounter(counter);counter->QuadPart+=s_clock_qpc_bias;}
}
void xbox_RuntimeClockPause(void)
{
    LARGE_INTEGER now;QueryPerformanceCounter(&now);
    s_clock_frozen_qpc=now.QuadPart+s_clock_qpc_bias;
    s_clock_frozen_tick=GetTickCount64()+s_clock_tick_bias;InterlockedExchange(&s_clock_frozen,1);
}
void xbox_RuntimeClockResume(void)
{
    LARGE_INTEGER now;QueryPerformanceCounter(&now);
    s_clock_qpc_bias=s_clock_frozen_qpc-now.QuadPart;
    s_clock_tick_bias=s_clock_frozen_tick-GetTickCount64();InterlockedExchange(&s_clock_frozen,0);
}

/* Call this periodically or on-demand to update KeTickCount */
static void xbox_update_tick_count(void)
{
    xbox_KeTickCount = (ULONG)xbox_RuntimeTickCount64();
}

/* ============================================================================
 * Performance Counters
 *
 * Direct 1:1 mapping to Win32 QueryPerformanceCounter/Frequency.
 * Both Xbox and Windows return LARGE_INTEGER.
 * ============================================================================ */

LARGE_INTEGER __stdcall xbox_KeQueryPerformanceCounter(void)
{
    LARGE_INTEGER counter;
    xbox_RuntimePerformanceCounter(&counter);
    return counter;
}

LARGE_INTEGER __stdcall xbox_KeQueryPerformanceFrequency(void)
{
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    return freq;
}

/* ============================================================================
 * System Time
 *
 * KeQuerySystemTime returns the current time as a FILETIME (100ns since
 * January 1, 1601). Direct Win32 mapping.
 * ============================================================================ */

static LONGLONG s_time_anchor_100ns;
static LONGLONG s_time_anchor_counts;
static LONGLONG s_time_freq_counts;
static INIT_ONCE s_time_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK anchor_system_time(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    FILETIME ft;
    LARGE_INTEGER f, now;
    (void)once; (void)param; (void)ctx;
    QueryPerformanceFrequency(&f);
    GetSystemTimeAsFileTime(&ft);
    xbox_RuntimePerformanceCounter(&now);
    s_time_freq_counts = f.QuadPart ? f.QuadPart : 1;
    s_time_anchor_100ns = ((LONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    s_time_anchor_counts = now.QuadPart;
    return TRUE;
}

VOID __stdcall xbox_KeQuerySystemTime(PLARGE_INTEGER CurrentTime)
{
    /* Anchored once to the wall clock, advanced by the performance counter.
     *
     * GetSystemTimeAsFileTime alone moves in steps of about 15.6 ms, the
     * host's scheduler tick. The console's clock is far finer, and a title
     * that busy-waits on this -- reading it until enough time has passed --
     * spins for the whole of each step instead of a few iterations.
     *
     * Measured on Shin Megami Tensei: Nine: one such wait called this
     * **11.8 million times in two seconds**, which is most of what the
     * title was doing at that moment, and it came out of the spin in a
     * state where it no longer polled the gamepad.
     *
     * The anchor keeps the absolute value right; the counter supplies the
     * resolution between ticks.
     *
     * Whole seconds and the remainder are scaled separately: scaling the
     * whole count by 10^7 overflows after about a day at 10 MHz. */
    LARGE_INTEGER now;
    LONGLONG delta;

    if (!CurrentTime)
        return;

    InitOnceExecuteOnce(&s_time_once, anchor_system_time, NULL, NULL);
    xbox_RuntimePerformanceCounter(&now);
    delta = now.QuadPart - s_time_anchor_counts;
    CurrentTime->QuadPart = s_time_anchor_100ns
        + (delta / s_time_freq_counts) * 10000000LL
        + (delta % s_time_freq_counts) * 10000000LL / s_time_freq_counts;
}

/* ============================================================================
 * Processor Stall
 *
 * KeStallExecutionProcessor performs a busy-wait for the given number
 * of microseconds. Used for hardware timing (e.g., waiting for GPU).
 * ============================================================================ */

VOID __stdcall xbox_KeStallExecutionProcessor(ULONG MicroSeconds)
{
    LARGE_INTEGER freq, start, now;

    if (MicroSeconds == 0)
        return;

    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);

    LONGLONG target_counts = (freq.QuadPart * MicroSeconds) / 1000000;

    do {
        QueryPerformanceCounter(&now);
    } while ((now.QuadPart - start.QuadPart) < target_counts);
}

/* ============================================================================
 * Floating Point State
 *
 * Xbox kernel requires saving/restoring FP state when kernel code uses
 * floating point. On Windows user-mode this is handled automatically by
 * the OS, so these are no-ops.
 * ============================================================================ */

NTSTATUS __stdcall xbox_KeSaveFloatingPointState(PVOID FloatingPointState)
{
    (void)FloatingPointState;
    /* No-op: Windows user-mode preserves FP state across context switches */
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_KeRestoreFloatingPointState(PVOID FloatingPointState)
{
    (void)FloatingPointState;
    return STATUS_SUCCESS;
}

/* ============================================================================
 * Bug Check (Blue Screen of Death)
 *
 * KeBugCheck/KeBugCheckEx are the Xbox equivalent of BSOD. In our
 * recompilation, we log the error and terminate the process.
 * ============================================================================ */

VOID __stdcall xbox_KeBugCheck(ULONG BugCheckCode)
{
    xbox_log(XBOX_LOG_ERROR, XBOX_LOG_HAL,
        "*** KeBugCheck: code=0x%08X ***", BugCheckCode);
    fprintf(stderr, "[EXIT] KeBugCheck 0x%08X\n", (unsigned)BugCheckCode);

#ifdef _DEBUG
    DebugBreak();
#endif

    ExitProcess(BugCheckCode);
}

VOID __stdcall xbox_KeBugCheckEx(
    ULONG BugCheckCode,
    ULONG_PTR Param1,
    ULONG_PTR Param2,
    ULONG_PTR Param3,
    ULONG_PTR Param4)
{
    xbox_log(XBOX_LOG_ERROR, XBOX_LOG_HAL,
        "*** KeBugCheckEx: code=0x%08X, params=(0x%p, 0x%p, 0x%p, 0x%p) ***",
        BugCheckCode, (void*)Param1, (void*)Param2, (void*)Param3, (void*)Param4);
    fprintf(stderr, "[EXIT] KeBugCheckEx 0x%08X\n", (unsigned)BugCheckCode);

#ifdef _DEBUG
    DebugBreak();
#endif

    ExitProcess(BugCheckCode);
}

/* ============================================================================
 * HAL PCI Access
 *
 * HalReadWritePCISpace reads/writes PCI configuration space. The Xbox uses
 * this for GPU and southbridge setup. NV2A configuration is shared with PBUS.
 * ============================================================================ */

VOID __stdcall xbox_HalReadWritePCISpace(
    ULONG BusNumber,
    ULONG SlotNumber,
    ULONG RegisterNumber,
    PVOID Buffer,
    ULONG Length,
    BOOLEAN WritePCISpace)
{
    NV2AState *gpu = nv2a_get_state();
    /* The observed D3D path accesses AGP configuration at 0x4C. xemu's
     * default PCI write mask retains this device-specific register. Other
     * devices and unimplemented configuration writes remain explicit gaps. */
    if (!gpu || BusNumber != 1 || SlotNumber != 0 || !Buffer ||
        RegisterNumber > sizeof(gpu->parent_obj.config) ||
        Length > sizeof(gpu->parent_obj.config) - RegisterNumber ||
        (WritePCISpace && (RegisterNumber != 0x4C || Length != 4))) {
        fprintf(stderr, "[STOP] unsupported PCI access: bus=%u slot=%u reg=0x%X len=%u %s\n",
                BusNumber, SlotNumber, RegisterNumber, Length,
                WritePCISpace ? "WRITE" : "READ");
        exit(5);
    }
    if (WritePCISpace)
        memcpy(gpu->parent_obj.config + RegisterNumber, Buffer, Length);
    else
        memcpy(Buffer, gpu->parent_obj.config + RegisterNumber, Length);
    fprintf(stderr, "[PCI] NV2A bus=%u slot=%u reg=0x%X len=%u %s value=0x%08X\n",
        BusNumber, SlotNumber, RegisterNumber, Length,
        WritePCISpace ? "WRITE" : "READ",
        Length == 4 ? pci_get_long(gpu->parent_obj.config + RegisterNumber) : 0);
}

static unsigned s_gpio_field_pin;

uint32_t xbox_PortRead(uint16_t port, unsigned width, uint32_t instruction_va)
{
    /* Original MCPX acpi_xbox.c offset-zero field-pin read. */
    if (port == 0x80C0 && width == 1) {
        s_gpio_field_pin = (s_gpio_field_pin + 1) & 1;
        return s_gpio_field_pin << 5;
    }
    fprintf(stderr, "[STOP] unsupported port read: port=0x%04X width=%u VA=0x%08X\n",
            port, width, instruction_va);
    exit(5);
}

void xbox_WriteBackInvalidate(void)
{
    /* CPU and NV2A consume the same coherent host RAM. The guest has no
     * separate CPU cache to invalidate. Windows publishes pending writes
     * across every thread before guest device processing can continue. */
#if defined(_WIN32)
    FlushProcessWriteBuffers();
#else
    fprintf(stderr, "[STOP] write-back/invalidate is not implemented on this host\n");
    exit(5);
#endif
}

void xbox_PortWrite(uint16_t port, uint32_t value, unsigned width,
                    uint32_t instruction_va)
{
    /* MCPX PM GPIO offset 0. xemu acpi_xbox.c's GPIO write handler has no
     * display action here; aspect ratio is a separate register at 0x80D6.
     * This exact byte write is supported; unknown ports are not ignored. */
    if (port == 0x80C0 && width == 1) {
        fprintf(stderr, "[PORT] MCPX PM GPIO write: port=0x%04X value=0x%02X VA=0x%08X\n",
                port, value & 0xFFu, instruction_va);
        return;
    }
    fprintf(stderr, "[STOP] unsupported port write: port=0x%04X value=0x%08X width=%u VA=0x%08X\n",
            port, value, width, instruction_va);
    exit(5);
}

/* ============================================================================
 * HAL Firmware & Shutdown
 *
 * HalReturnToFirmware returns to the Xbox dashboard. For us, this means
 * exit the game cleanly.
 * ============================================================================ */

VOID __stdcall xbox_HalReturnToFirmware(ULONG Routine)
{
    xbox_log(XBOX_LOG_INFO, XBOX_LOG_HAL,
        "HalReturnToFirmware: routine=%u (exiting)", Routine);
    /* stderr too: the kernel log file is rewritten on every run, so a title
     * that ends itself otherwise leaves no trace in the run's own log. */
    fprintf(stderr, "[EXIT] HalReturnToFirmware routine=%u\n", (unsigned)Routine);
    ExitProcess(0);
}

VOID __stdcall xbox_HalInitiateShutdown(void)
{
    xbox_log(XBOX_LOG_INFO, XBOX_LOG_HAL, "HalInitiateShutdown (exiting)");
    fprintf(stderr, "[EXIT] HalInitiateShutdown\n");
    ExitProcess(0);
}

BOOLEAN __stdcall xbox_HalIsResetOrShutdownPending(void)
{
    return FALSE;
}

/* ============================================================================
 * SMC (System Management Controller)
 *
 * HalReadSMCTrayState reads the DVD tray state. No disc tray on PC.
 * ============================================================================ */

ULONG __stdcall xbox_HalReadSMCTrayState(PULONG TrayState, PULONG TrayStateChangeCount)
{
    /* Tray state: 0x10 = media detected (disc present) */
    if (TrayState)
        *TrayState = 0x10;
    if (TrayStateChangeCount)
        *TrayStateChangeCount = 0;
    return 0; /* Success */
}

/* ============================================================================
 * Software Interrupts
 *
 * Used for APC/DPC delivery on Xbox. Stubbed since we don't have real
 * interrupt-driven DPC delivery.
 * ============================================================================ */

VOID __stdcall xbox_HalClearSoftwareInterrupt(KIRQL RequestIrql)
{
    (void)RequestIrql;
}

VOID __stdcall xbox_HalRequestSoftwareInterrupt(KIRQL RequestIrql)
{
    (void)RequestIrql;
}

VOID __stdcall xbox_HalDisableSystemInterrupt(ULONG BusInterruptLevel, KIRQL Irql)
{
    (void)BusInterruptLevel;
    (void)Irql;
}

ULONG __stdcall xbox_HalGetInterruptVector(ULONG BusInterruptLevel, PKIRQL Irql)
{
    (void)BusInterruptLevel;
    if (Irql)
        *Irql = PASSIVE_LEVEL;
    return 0;
}

/* ============================================================================
 * Interrupt Objects
 *
 * Used by DSOUND and other drivers for hardware interrupt handling.
 * Since we replace the audio/graphics subsystems entirely, these are stubs.
 * ============================================================================ */

VOID __stdcall xbox_KeInitializeInterrupt(
    PXBOX_KINTERRUPT Interrupt,
    PVOID ServiceRoutine,
    PVOID ServiceContext,
    ULONG Vector,
    KIRQL Irql,
    ULONG InterruptMode,
    BOOLEAN ShareVector)
{
    (void)Vector;
    (void)InterruptMode;
    (void)ShareVector;

    if (!Interrupt)
        return;

    Interrupt->ServiceRoutine = ServiceRoutine;
    Interrupt->ServiceContext = ServiceContext;
    Interrupt->Irql = Irql;
    Interrupt->Connected = FALSE;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_HAL,
        "KeInitializeInterrupt: interrupt=%p, routine=%p, vector=%u",
        Interrupt, ServiceRoutine, Vector);
}

BOOLEAN __stdcall xbox_KeConnectInterrupt(PXBOX_KINTERRUPT Interrupt)
{
    if (!Interrupt)
        return FALSE;

    Interrupt->Connected = TRUE;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_HAL,
        "KeConnectInterrupt: interrupt=%p (stubbed - no real HW interrupts)",
        Interrupt);

    return TRUE;
}

BOOLEAN __stdcall xbox_KeDisconnectInterrupt(PXBOX_KINTERRUPT Interrupt)
{
    BOOLEAN was_connected;

    if (!Interrupt)
        return FALSE;

    /* Returns the PREVIOUS connected state, not success. */
    was_connected = Interrupt->Connected;
    Interrupt->Connected = FALSE;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_HAL,
        "KeDisconnectInterrupt: interrupt=%p was_connected=%d",
        Interrupt, (int)was_connected);

    return was_connected;
}

/* ============================================================================
 * Miscellaneous Port I/O Stubs
 * ============================================================================ */

VOID __stdcall xbox_WRITE_PORT_BUFFER_ULONG(PULONG Port, PULONG Buffer, ULONG Count)
{
    (void)Port;
    (void)Buffer;
    (void)Count;
}

VOID __stdcall xbox_WRITE_PORT_BUFFER_USHORT(PUSHORT Port, PUSHORT Buffer, ULONG Count)
{
    (void)Port;
    (void)Buffer;
    (void)Count;
}

/* ============================================================================
 * System Time (Set)
 *
 * NtSetSystemTime - we don't actually change the system clock, just log it.
 * ============================================================================ */

NTSTATUS __stdcall xbox_NtSetSystemTime(PLARGE_INTEGER SystemTime, PLARGE_INTEGER PreviousTime)
{
    if (PreviousTime)
        GetSystemTimeAsFileTime((LPFILETIME)PreviousTime);

    xbox_log(XBOX_LOG_WARN, XBOX_LOG_HAL,
        "NtSetSystemTime: ignored (not setting system clock)");

    return STATUS_SUCCESS;
}

/* ============================================================================
 * Display / AV
 *
 * These are declared in kernel.h for the thunk table but will be fully
 * implemented by the D3D replacement layer. We provide realistic AV pack
 * detection so games can query display capabilities (480p, 720p, widescreen).
 * ============================================================================ */

static ULONG g_av_saved_data_address = 0;
static ULONG g_av_display_mode = 0;
void xbox_CaptureDisplayState(uint32_t *state) {state[0]=g_av_saved_data_address;state[1]=g_av_display_mode;}
void xbox_RestoreDisplayState(const uint32_t *state) {g_av_saved_data_address=state[0];g_av_display_mode=state[1];}

ULONG __stdcall xbox_AvGetSavedDataAddress(void)
{
    return g_av_saved_data_address;
}

VOID __stdcall xbox_AvSendTVEncoderOption(
    PVOID RegisterBase, ULONG Option, ULONG Param, PULONG Result)
{
    (void)RegisterBase;
    (void)Param;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_HAL,
        "AvSendTVEncoderOption: option=0x%02X param=0x%X", Option, Param);

    if (!Result)
        return;

    switch (Option) {
    case AV_OPTION_QUERY_AVPACK:
        /* Pack type, video standard and refresh rate, in one word.
         *
         * D3D keys its display-mode table on all three: the row flags carry
         * the pack in 0x000000FF, the standard in 0x0000FF00 and the refresh
         * in 0x00C00000 (Half-Life 2's 640x480 60Hz row is 0x00480104).
         * Returning the pack alone left the standard as 0, which matches no
         * row, so the mode scan ran off the end of the table and device
         * creation failed with E_FAIL.
         *
         * HDTV pack keeps 480p/720p available to titles that offer them;
         * NTSC-M and 60Hz are the North American retail default, and match
         * the region reported by ExQueryNonVolatileSetting. */
        *Result = AV_PACK_HDTV
                | (AV_STANDARD_NTSC_M << AV_STANDARD_SHIFT)
                | AV_REFRESH_60Hz;
        break;

    case AV_OPTION_QUERY_MODE:
        /* Return current display mode */
        *Result = g_av_display_mode;
        break;

    case AV_OPTION_QUERY_AV_CAPABILITIES:
        /* Report support for 480i, 480p, 720p, and widescreen */
        *Result = AV_FLAGS_HDTV_480i | AV_FLAGS_HDTV_480p
                | AV_FLAGS_HDTV_720p | AV_FLAGS_WIDESCREEN
                | AV_FLAGS_60Hz;
        break;

    case AV_OPTION_QUERY_ENCODER_TYPE:
        /* Conexant CX25871 (common in retail Xboxes) */
        *Result = 4;
        break;

    case AV_OPTION_QUERY_MODE_CAPS:
        /* Same as capabilities for our purposes */
        *Result = AV_FLAGS_HDTV_480i | AV_FLAGS_HDTV_480p
                | AV_FLAGS_HDTV_720p | AV_FLAGS_WIDESCREEN
                | AV_FLAGS_60Hz;
        break;

    case AV_OPTION_SET_MODE:
        g_av_display_mode = Param;
        *Result = 0;
        break;

    case AV_OPTION_BLANK_SCREEN:
    case AV_OPTION_MACROVISION_MODE:
    case AV_OPTION_FLICKER_FILTER:
    case AV_OPTION_ZERO_MODE:
        *Result = 0;
        break;

    default:
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_HAL,
            "AvSendTVEncoderOption: unknown option 0x%02X", Option);
        *Result = 0;
        break;
    }
}

VOID __stdcall xbox_AvSetSavedDataAddress(ULONG Address)
{
    g_av_saved_data_address = Address;
}

VOID __stdcall xbox_AvSetDisplayMode(
    PVOID RegisterBase, ULONG Step, ULONG Mode,
    ULONG Format, ULONG Pitch, ULONG FrameBuffer)
{
    (void)RegisterBase;
    (void)Step;
    (void)Format;
    (void)Pitch;
    (void)FrameBuffer;

    g_av_display_mode = Mode;

    xbox_log(XBOX_LOG_INFO, XBOX_LOG_HAL,
        "AvSetDisplayMode: step=%u mode=0x%X format=0x%X pitch=%u fb=0x%X",
        Step, Mode, Format, Pitch, FrameBuffer);
}

/* ============================================================================
 * SMBus - HalReadSMBusValue / HalWriteSMBusValue
 *
 * The Xbox SMBus connects the CPU to the System Management Controller (SMC),
 * EEPROM, temperature sensor, and TV encoder. Games use these to detect
 * AV pack type, read EEPROM settings, and check hardware state.
 *
 * We simulate responses for the most commonly queried devices:
 *   - SMC (0x20): firmware version, tray state, AV pack, temperatures
 *   - EEPROM (0xA8): handled separately via ExQueryNonVolatileSetting
 *   - Temperature sensor (0x98): CPU/board temperatures
 * ============================================================================ */

NTSTATUS __stdcall xbox_HalReadSMBusValue(
    UCHAR SlaveAddress, UCHAR CommandCode, BOOLEAN ReadWordValue, PULONG DataValue)
{
    if (!DataValue)
        return STATUS_INVALID_PARAMETER;

    *DataValue = 0;

    switch (SlaveAddress) {
    case SMC_SLAVE_ADDRESS:  /* 0x20 - System Management Controller */
        switch (CommandCode) {
        case SMC_CMD_FIRMWARE_VER:
            /* "P01" = production SMC, return 'P' for first byte.
             * Games read version byte-by-byte: P(0x50), 0(0x30), 1(0x31) */
            *DataValue = 0x50; /* 'P' */
            break;
        case SMC_CMD_TRAY_STATE:
            /* 0x60 = media present, tray closed */
            *DataValue = 0x60;
            break;
        case SMC_CMD_AV_PACK:
            /* HDTV/Component pack */
            *DataValue = AV_PACK_HDTV;
            break;
        case SMC_CMD_CPU_TEMP:
            *DataValue = 40; /* 40 degrees C */
            break;
        case SMC_CMD_MB_TEMP:
            *DataValue = 35; /* 35 degrees C */
            break;
        case SMC_CMD_FAN_SPEED:
            *DataValue = 50; /* ~50% fan speed */
            break;
        case SMC_CMD_INTERRUPT_REASON:
            *DataValue = 0;  /* No pending interrupt */
            break;
        case SMC_CMD_ERROR_CODE:
            *DataValue = 0;  /* No error */
            break;
        default:
            xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_HAL,
                "HalReadSMBusValue: SMC unknown cmd=0x%02X", CommandCode);
            break;
        }
        break;

    case TEMP_SLAVE_ADDRESS:  /* 0x98 - ADM1032 temperature sensor */
        /* CommandCode 0x00 = local temp, 0x01 = remote temp */
        if (CommandCode == 0x00)
            *DataValue = 35;  /* Board: 35C */
        else if (CommandCode == 0x01)
            *DataValue = 40;  /* CPU: 40C */
        else
            *DataValue = 30;
        break;

    case ENCODER_SLAVE_ADDRESS:  /* 0xD4 - TV encoder */
        /* Return 0 for most encoder register reads */
        *DataValue = 0;
        break;

    default:
        xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_HAL,
            "HalReadSMBusValue: unknown slave=0x%02X cmd=0x%02X",
            SlaveAddress, CommandCode);
        break;
    }

    xbox_log(XBOX_LOG_TRACE, XBOX_LOG_HAL,
        "HalReadSMBusValue: slave=0x%02X cmd=0x%02X word=%d -> 0x%X",
        SlaveAddress, CommandCode, ReadWordValue, *DataValue);

    (void)ReadWordValue;
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_HalWriteSMBusValue(
    UCHAR SlaveAddress, UCHAR CommandCode, BOOLEAN WriteWordValue, ULONG DataValue)
{
    (void)WriteWordValue;

    xbox_log(XBOX_LOG_TRACE, XBOX_LOG_HAL,
        "HalWriteSMBusValue: slave=0x%02X cmd=0x%02X word=%d val=0x%X (ignored)",
        SlaveAddress, CommandCode, WriteWordValue, DataValue);

    /* Writes to SMC (LED control, fan speed, etc.) are silently accepted */
    return STATUS_SUCCESS;
}

/* ============================================================================
 * HAL Data Exports
 *
 * Ordinals 40, 41 and 42 are variables, not functions. Games read them
 * directly through the thunk table, so the thunk must hand back the address
 * of real storage -- pointing these at a function is what produced garbage
 * disk metadata before.
 *
 * The strings are counted (Length/MaximumLength), not NUL-terminated, matching
 * the kernel's STRING type. Values describe the virtual disk we present; no
 * real hardware is queried.
 * ============================================================================ */

ULONG xbox_HalDiskCachePartitionCount = 3;

static char g_disk_model[]  = "XBOXRECOMP VIRTUAL HDD";
static char g_disk_serial[] = "XR0000000000";

XBOX_ANSI_STRING xbox_HalDiskModelNumber = {
    sizeof(g_disk_model) - 1,
    sizeof(g_disk_model) - 1,
    g_disk_model
};

XBOX_ANSI_STRING xbox_HalDiskSerialNumber = {
    sizeof(g_disk_serial) - 1,
    sizeof(g_disk_serial) - 1,
    g_disk_serial
};

/*
 * Video mode the SMC reported at boot. 0 lets title code fall back to querying
 * the AV pack, which we answer properly in AvGetSavedDataAddress/SMBus.
 */
ULONG xbox_HalBootSMCVideoMode = 0;

/*
 * IDE channel object. Real kernels export a device object for the ATA channel;
 * drivers only ever pass it back to us, so identity is all that is required.
 */
static ULONG g_idex_channel_data = 0x49444558; /* 'IDEX' */
PVOID xbox_IdexChannelObject = &g_idex_channel_data;

/* ============================================================================
 * Shutdown Notification
 * ============================================================================ */

VOID __stdcall xbox_HalRegisterShutdownNotification(
    PVOID ShutdownRegistration,
    BOOLEAN Register)
{
    /*
     * Registers a callback to run on reboot/shutdown. We never initiate an
     * Xbox-style shutdown -- HalReturnToFirmware terminates the process -- so
     * the callback would never fire. Recorded in the log so a title relying on
     * shutdown cleanup is visible rather than silently ignored.
     */
    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_HAL,
        "HalRegisterShutdownNotification: %s registration=%p (never invoked)",
        Register ? "register" : "unregister", ShutdownRegistration);
}

/* ============================================================================
 * Unknown Ordinal Stubs
 * ============================================================================ */

VOID __stdcall xbox_Unknown_8(void)
{
    xbox_log(XBOX_LOG_WARN, XBOX_LOG_HAL, "Unknown ordinal 8 called (stubbed)");
}

VOID __stdcall xbox_Unknown_23(void)
{
    xbox_log(XBOX_LOG_WARN, XBOX_LOG_HAL, "Unknown ordinal 23 called (stubbed)");
}

VOID __stdcall xbox_Unknown_42(void)
{
    xbox_log(XBOX_LOG_WARN, XBOX_LOG_HAL, "Unknown ordinal 42 called (stubbed)");
}

/* ============================================================================
 * Debug / Timing
 * ============================================================================ */

VOID __stdcall xbox_DbgBreakPoint(void)
{
    /*
     * Titles call this from assertion paths. Under a debugger this should
     * break; without one, raising a breakpoint exception would terminate the
     * process on a condition the title may well survive. Log loudly and
     * continue, and only actually break when a debugger is attached to catch it.
     */
    xbox_log(XBOX_LOG_WARN, XBOX_LOG_HAL, "DbgBreakPoint called by title");

    if (IsDebuggerPresent())
        DebugBreak();
}

ULONGLONG __stdcall xbox_KeQueryInterruptTime(void)
{
    /*
     * Time since boot in NT 100ns units. GetTickCount64 is milliseconds, so
     * scale by 10,000. Resolution is coarser than the real kernel's, but it is
     * monotonic, which is the property callers actually depend on.
     */
    return xbox_RuntimeTickCount64() * 10000ULL;
}

/* ============================================================================
 * Time stamp counter
 *
 * Xbox's QueryPerformanceCounter is a bare `rdtsc`, and its
 * QueryPerformanceFrequency returns the CPU clock as a constant the title
 * compiles in: Half-Life 2's is 0x2BB5C755 (733,333,333 Hz) at 0x0059C6C7.
 * So a frame timer computes seconds as counter / 733333333.
 *
 * Returning the host's own TSC would make that division wrong by the ratio of
 * the two clocks -- a 3.5 GHz host would have the guest believe nearly five
 * seconds had passed for every real one. Scaling the host's performance
 * counter to the console's rate keeps the guest's arithmetic honest.
 *
 * Monotonic and shared by every thread, which is what a TSC is. The first
 * call establishes the origin so the counter starts near zero rather than at
 * whatever the host had been running for.
 * ========================================================================= */
#define XBOX_TSC_HZ 733333333ull
static LARGE_INTEGER s_tsc_freq,s_tsc_origin;

uint64_t xbox_ReadTimeStampCounter(void)
{
    LARGE_INTEGER now;

    if (s_tsc_freq.QuadPart == 0) {
        QueryPerformanceFrequency(&s_tsc_freq);
        xbox_RuntimePerformanceCounter(&s_tsc_origin);
        if (s_tsc_freq.QuadPart == 0)
            s_tsc_freq.QuadPart = 1;
    }
    xbox_RuntimePerformanceCounter(&now);

    {
        uint64_t ticks = (uint64_t)(now.QuadPart - s_tsc_origin.QuadPart);
        /* Split the scaling so a long run cannot overflow: whole seconds
         * first, then the remainder. */
        uint64_t secs = ticks / (uint64_t)s_tsc_freq.QuadPart;
        uint64_t rem  = ticks % (uint64_t)s_tsc_freq.QuadPart;
        return secs * XBOX_TSC_HZ
             + (rem * XBOX_TSC_HZ) / (uint64_t)s_tsc_freq.QuadPart;
    }
}
typedef struct RuntimeClockCheckpoint {
    uint64_t tick;LONGLONG qpc,frequency,system_anchor,system_counts,system_frequency;
    LONGLONG tsc_frequency,tsc_origin;ULONG exported_tick,gpio_field_pin;
} RuntimeClockCheckpoint;
int xbox_SaveRuntimeClock(FILE *out)
{
    if(!out || !xbox_CheckpointFrozen() || !s_clock_frozen) return 0;
    InitOnceExecuteOnce(&s_time_once,anchor_system_time,NULL,NULL);
    LARGE_INTEGER frequency;QueryPerformanceFrequency(&frequency);
    RuntimeClockCheckpoint saved={s_clock_frozen_tick,s_clock_frozen_qpc,frequency.QuadPart,
        s_time_anchor_100ns,s_time_anchor_counts,s_time_freq_counts,s_tsc_freq.QuadPart,s_tsc_origin.QuadPart,xbox_KeTickCount,s_gpio_field_pin};
    return fwrite("XBCLOCK",7,1,out)==1 && fwrite(&saved,sizeof saved,1,out)==1;
}
void *xbox_PrepareRuntimeClock(FILE *in)
{
    if(!in || !xbox_CheckpointFrozen() || !s_clock_frozen) return NULL;
    char magic[7];RuntimeClockCheckpoint *saved=malloc(sizeof *saved);if(!saved) return NULL;
    LARGE_INTEGER frequency;QueryPerformanceFrequency(&frequency);
    int ok=fread(magic,7,1,in)==1 && !memcmp(magic,"XBCLOCK",7) && fread(saved,sizeof *saved,1,in)==1 &&
        saved->frequency>0 && saved->frequency==frequency.QuadPart && saved->system_frequency==saved->frequency &&
        (saved->tsc_frequency==0 || saved->tsc_frequency==saved->frequency) && saved->gpio_field_pin<=1;
    if(!ok){free(saved);return NULL;}return saved;
}
void xbox_ApplyRuntimeClock(const void *prepared)
{
    const RuntimeClockCheckpoint *saved=prepared;
    InitOnceExecuteOnce(&s_time_once,anchor_system_time,NULL,NULL);
    s_clock_frozen_tick=saved->tick;s_clock_frozen_qpc=saved->qpc;
    s_time_anchor_100ns=saved->system_anchor;s_time_anchor_counts=saved->system_counts;s_time_freq_counts=saved->system_frequency;
    s_tsc_freq.QuadPart=saved->tsc_frequency;s_tsc_origin.QuadPart=saved->tsc_origin;xbox_KeTickCount=saved->exported_tick;
    s_gpio_field_pin=saved->gpio_field_pin;
}
