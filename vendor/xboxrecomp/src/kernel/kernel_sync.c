/*
 * kernel_sync.c - Xbox Synchronization Primitives
 *
 * Implements events, semaphores, wait functions, kernel timers, and DPCs
 * using Win32 synchronization objects.
 *
 * Xbox synchronization model:
 *   - Events: notification (manual-reset) or synchronization (auto-reset)
 *   - Semaphores: standard counting semaphores
 *   - Wait functions: single and multiple, with optional timeout
 *   - Timers: kernel timers with optional DPC callback on expiry
 *   - DPCs: deferred procedure calls, executed via thread pool on Windows
 *
 * Time values use NT 100-nanosecond units (negative = relative).
 */

#include "kernel.h"

/* NT event operations return the previous state in the same operation that
 * changes it. Querying and then setting cannot preserve that contract when a
 * synchronization-event waiter consumes the signal between the two calls. */
#if defined(_WIN32)
__declspec(dllimport) NTSTATUS NTAPI NtSetEvent(HANDLE, PLONG);
__declspec(dllimport) NTSTATUS NTAPI NtResetEvent(HANDLE, PLONG);
__declspec(dllimport) NTSTATUS NTAPI NtPulseEvent(HANDLE, PLONG);
__declspec(dllimport) NTSTATUS NTAPI NtSignalAndWaitForSingleObject(HANDLE,HANDLE,BOOLEAN,PLARGE_INTEGER);
#endif


/* ============================================================================
 * Helper: Convert NT 100ns interval to Win32 milliseconds
 *
 * NT time intervals:
 *   - Negative = relative (most common), in 100ns units
 *   - Positive = absolute FILETIME
 *   - NULL = infinite wait
 * ============================================================================ */

DWORD xbox_nt_timeout_to_ms(PLARGE_INTEGER Timeout)
{
    if (!Timeout)
        return INFINITE;

    if (Timeout->QuadPart == 0)
        return 0;

    if (Timeout->QuadPart < 0) {
        /* Relative: negative 100ns units */
        LONGLONG relative_100ns = -Timeout->QuadPart;
        DWORD ms = (DWORD)(relative_100ns / 10000);
        if (ms == 0 && relative_100ns > 0)
            ms = 1;
        return ms;
    }

    /* Absolute: compute delta from now */
    LARGE_INTEGER now;
    GetSystemTimeAsFileTime((LPFILETIME)&now);
    LONGLONG diff = Timeout->QuadPart - now.QuadPart;
    if (diff <= 0)
        return 0;
    return (DWORD)(diff / 10000);
}

/* ============================================================================
 * Events
 *
 * Xbox event types:
 *   XboxNotificationEvent (0) = manual-reset (Win32: TRUE)
 *   XboxSynchronizationEvent (1) = auto-reset (Win32: FALSE)
 * ============================================================================ */

NTSTATUS __stdcall xbox_NtCreateEvent(
    PHANDLE EventHandle,
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes,
    ULONG EventType,
    BOOLEAN InitialState)
{
    HANDLE hEvent;
    BOOL bManualReset;

    if(ObjectAttributes)return xbox_kernel_create_named(189,EventHandle,ObjectAttributes,EventType,InitialState);

    if (!EventHandle || EventType>1)
        return STATUS_INVALID_PARAMETER;

    /* XboxNotificationEvent = manual-reset, XboxSynchronizationEvent = auto-reset */
    bManualReset = (EventType == XboxNotificationEvent) ? TRUE : FALSE;

    hEvent = CreateEventW(NULL, bManualReset, InitialState, NULL);
    if (!hEvent) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_SYNC,
            "NtCreateEvent: CreateEventW failed (error %u)", GetLastError());
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    *EventHandle = hEvent;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_SYNC,
        "NtCreateEvent: handle=%p, type=%s, initial=%d",
        hEvent, bManualReset ? "notification" : "synchronization", InitialState);

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtSetEvent(HANDLE EventHandle, PLONG PreviousState)
{
    NTSTATUS type=xbox_kernel_reject_timer_signal(EventHandle);
    if(type<0)return type;
    return NtSetEvent(EventHandle, PreviousState);
}

NTSTATUS __stdcall xbox_NtResetEvent(HANDLE EventHandle, PLONG PreviousState)
{
    NTSTATUS type=xbox_kernel_reject_timer_signal(EventHandle);
    if(type<0)return type;
    return NtResetEvent(EventHandle, PreviousState);
}

NTSTATUS __stdcall xbox_NtPulseEvent(HANDLE EventHandle, PLONG PreviousState)
{
    NTSTATUS type=xbox_kernel_reject_timer_signal(EventHandle);
    if(type<0)return type;
    return NtPulseEvent(EventHandle, PreviousState);
}

/*
 * KeSetEvent - kernel-mode event signal.
 * On Xbox, this is the kernel-mode equivalent of NtSetEvent.
 * The Object parameter is treated as a Win32 event HANDLE.
 * Returns the previous signal state.
 */
LONG __stdcall xbox_KeSetEvent(PVOID Event, LONG Increment, BOOLEAN Wait)
{
    HANDLE hEvent = (HANDLE)Event;

    (void)Increment;
    (void)Wait;

    LONG previous = 0;
    xbox_NtSetEvent(hEvent, &previous);
    return previous;
}

/* ============================================================================
 * Semaphores
 * ============================================================================ */

NTSTATUS __stdcall xbox_NtCreateSemaphore(
    PHANDLE SemaphoreHandle,
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes,
    LONG InitialCount,
    LONG MaximumCount)
{
    HANDLE hSemaphore;

    if(ObjectAttributes)return xbox_kernel_create_named(193,SemaphoreHandle,ObjectAttributes,(uint32_t)InitialCount,(uint32_t)MaximumCount);

    if (!SemaphoreHandle || InitialCount<0 || MaximumCount<=0 || InitialCount>MaximumCount)
        return STATUS_INVALID_PARAMETER;

    hSemaphore = CreateSemaphoreW(NULL, InitialCount, MaximumCount, NULL);
    if (!hSemaphore) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_SYNC,
            "NtCreateSemaphore: CreateSemaphoreW failed (error %u)", GetLastError());
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    *SemaphoreHandle = hSemaphore;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_SYNC,
        "NtCreateSemaphore: handle=%p, initial=%d, max=%d",
        hSemaphore, InitialCount, MaximumCount);

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtReleaseSemaphore(
    HANDLE SemaphoreHandle,
    LONG ReleaseCount,
    PLONG PreviousCount)
{
    if (!ReleaseSemaphore(SemaphoreHandle, ReleaseCount, PreviousCount)) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_SYNC,
            "NtReleaseSemaphore: failed for handle %p (error %u)",
            SemaphoreHandle, GetLastError());
        return STATUS_INVALID_HANDLE;
    }

    return STATUS_SUCCESS;
}

/* ============================================================================
 * Wait Functions
 * ============================================================================ */

/*
 * Map Win32 WaitFor* return codes to NTSTATUS.
 */
static NTSTATUS xbox_wait_result_to_ntstatus(DWORD result, ULONG count)
{
    if(result>=0xc0000000u && result!=WAIT_FAILED)return (NTSTATUS)result;
    if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + count)
        return (NTSTATUS)(STATUS_SUCCESS + (result - WAIT_OBJECT_0));

    switch (result) {
        case WAIT_TIMEOUT:          return STATUS_TIMEOUT;
        case WAIT_IO_COMPLETION:    return STATUS_ALERTED;
        case XBOX_WAIT_USER_APC:    return (NTSTATUS)0xc0;
        case WAIT_ABANDONED_0:      return STATUS_ABANDONED;
        case WAIT_FAILED:           return STATUS_UNSUCCESSFUL;
        default:                    return STATUS_UNSUCCESSFUL;
    }
}

static DWORD checkpoint_wait(ULONG count,const HANDLE *handles,BOOL wait_all,
                           KPROCESSOR_MODE mode,BOOLEAN alertable,DWORD milliseconds,int signal_wait)
{
    HANDLE owned[MAXIMUM_WAIT_OBJECTS];uint32_t tokens[MAXIMUM_WAIT_OBJECTS];
    NTSTATUS status=xbox_kernel_wait_acquire(count,handles,owned,tokens);
    if(status<0)return (DWORD)status;
    if(signal_wait && (status=xbox_kernel_reject_timer_signal(owned[0]))<0) {
        xbox_kernel_wait_release_objects(count,tokens);return (DWORD)status;
    }
    int signal_pending=signal_wait && !xbox_kernel_wait_signal_done();
    int held=xbox_kernel_wait_release_cpu();
    int tracked=xbox_kernel_wait_tracked(),poll=tracked || xbox_ThreadApcsEnabled();
    DWORD outcome;
    if (tracked) milliseconds=xbox_kernel_wait_begin(milliseconds);
    ULONGLONG started=GetTickCount64();
    for (;;) {
        DWORD remaining=milliseconds;
        if (poll && milliseconds!=INFINITE) {
            ULONGLONG elapsed=GetTickCount64()-started;
            remaining=elapsed>=milliseconds?0:milliseconds-(DWORD)elapsed;
        }
        if(!signal_pending) {
            xbox_kernel_wait_apc_begin(remaining);
            int delivered=xbox_DeliverThreadApcs(alertable && mode==UserMode);
            xbox_kernel_wait_apc_end();
            if(delivered){outcome=XBOX_WAIT_USER_APC;break;}
            if(poll && milliseconds!=INFINITE) {
                ULONGLONG elapsed=GetTickCount64()-started;
                remaining=elapsed>=milliseconds?0:milliseconds-(DWORD)elapsed;
            }
        }
        if (tracked) {
            ULONGLONG boundary_started=GetTickCount64();
            xbox_kernel_wait_boundary(remaining);
            started+=GetTickCount64()-boundary_started;
        }
        DWORD slice=poll && remaining>10?10:remaining;
        uint32_t associated_queue=xbox_QueueThreadWaitBegin();
        DWORD result;
        if(signal_pending) {
            LARGE_INTEGER due;due.QuadPart=-(LONGLONG)slice*10000;
            result=(DWORD)NtSignalAndWaitForSingleObject(owned[0],owned[1],alertable,slice==INFINITE?NULL:&due);
            /* No checkpoint is taken inside the atomic host operation. Its
             * completed signal is persisted before the next wait boundary. */
            signal_pending=0;xbox_kernel_wait_mark_signalled();
            if(result==0xc0)result=WAIT_IO_COMPLETION;
        } else if(signal_wait) result=WaitForSingleObjectEx(owned[1],slice,alertable);
        else result=count?WaitForMultipleObjectsEx(count,owned,wait_all,slice,alertable):SleepEx(slice,alertable);
        xbox_QueueThreadWaitEnd(associated_queue);
        if (!count && result==0) result=WAIT_TIMEOUT;
        /* A final-slice timeout already completes the remaining interval.
         * Do not repeat it while the coarser tick counter has not advanced. */
        if (!poll || result!=WAIT_TIMEOUT ||
            (milliseconds!=INFINITE && slice==remaining) ||
            (milliseconds!=INFINITE && GetTickCount64()-started>=milliseconds))
            {outcome=result;break;}
    }
    if(held)guest_cpu_join();
    xbox_kernel_wait_release_objects(count,tokens);
    return outcome;
}

DWORD xbox_checkpoint_wait(ULONG count,const HANDLE *handles,BOOL wait_all,
                           KPROCESSOR_MODE mode,BOOLEAN alertable,DWORD milliseconds)
{
    return checkpoint_wait(count,handles,wait_all,mode,alertable,milliseconds,0);
}

NTSTATUS __stdcall xbox_NtSignalAndWaitForSingleObjectEx(
    HANDLE Signal,HANDLE Wait,KPROCESSOR_MODE WaitMode,BOOLEAN Alertable,PLARGE_INTEGER Timeout)
{
    HANDLE objects[]={Signal,Wait};
    return xbox_wait_result_to_ntstatus(checkpoint_wait(2,objects,FALSE,WaitMode,Alertable,
                                        xbox_nt_timeout_to_ms(Timeout),1),1);
}

NTSTATUS __stdcall xbox_NtWaitForSingleObject(
    HANDLE Handle,
    BOOLEAN Alertable,
    PLARGE_INTEGER Timeout)
{
    DWORD ms = xbox_nt_timeout_to_ms(Timeout);
    DWORD result = xbox_checkpoint_wait(1,&Handle,FALSE,UserMode,Alertable,ms);
    return xbox_wait_result_to_ntstatus(result, 1);
}

NTSTATUS __stdcall xbox_NtWaitForSingleObjectEx(
    HANDLE Handle,
    KPROCESSOR_MODE WaitMode,
    BOOLEAN Alertable,
    PLARGE_INTEGER Timeout)
{
    DWORD ms = xbox_nt_timeout_to_ms(Timeout);
    DWORD result;

    result = xbox_checkpoint_wait(1,&Handle,FALSE,WaitMode,Alertable,ms);
    return xbox_wait_result_to_ntstatus(result, 1);
}

NTSTATUS __stdcall xbox_NtWaitForMultipleObjectsEx(
    ULONG Count,
    HANDLE Handles[],
    ULONG WaitType,
    KPROCESSOR_MODE WaitMode,
    BOOLEAN Alertable,
    PLARGE_INTEGER Timeout)
{
    DWORD ms = xbox_nt_timeout_to_ms(Timeout);
    BOOL bWaitAll;
    DWORD result;

    /* WaitType: 0 = WaitAll, 1 = WaitAny (matches NT definitions) */
    bWaitAll = (WaitType == 0) ? TRUE : FALSE;

    result = xbox_checkpoint_wait(Count,Handles,bWaitAll,WaitMode,Alertable,ms);
    return xbox_wait_result_to_ntstatus(result, Count);
}

/*
 * KeWaitForSingleObject - kernel-mode wait on a dispatcher object.
 * On Xbox, Objects can be events, timers, threads, etc.
 * We treat the object pointer as a Win32 HANDLE.
 */
NTSTATUS __stdcall xbox_KeWaitForSingleObject(
    PVOID Object,
    ULONG WaitReason,
    KPROCESSOR_MODE WaitMode,
    BOOLEAN Alertable,
    PLARGE_INTEGER Timeout)
{
    (void)WaitReason;

    HANDLE hObject = (HANDLE)Object;
    DWORD ms = xbox_nt_timeout_to_ms(Timeout);
    DWORD result = xbox_checkpoint_wait(1,&hObject,FALSE,WaitMode,Alertable,ms);
    return xbox_wait_result_to_ntstatus(result, 1);
}

NTSTATUS __stdcall xbox_KeWaitForMultipleObjects(
    ULONG Count,
    PVOID Objects[],
    ULONG WaitType,
    ULONG WaitReason,
    KPROCESSOR_MODE WaitMode,
    BOOLEAN Alertable,
    PLARGE_INTEGER Timeout,
    PVOID WaitBlockArray)
{
    (void)WaitReason;
    (void)WaitBlockArray;

    BOOL bWaitAll = (WaitType == 0) ? TRUE : FALSE;
    DWORD ms = xbox_nt_timeout_to_ms(Timeout);

    /* Objects[] is an array of PVOID which we treat as HANDLE[] */
    DWORD result = xbox_checkpoint_wait(Count,(HANDLE*)Objects,bWaitAll,WaitMode,Alertable,ms);
    return xbox_wait_result_to_ntstatus(result, Count);
}

/* ============================================================================
 * Kernel Timers
 *
 * Xbox kernel timers are dispatcher objects that can be waited on and
 * optionally queue a DPC when they expire.
 *
 * Implementation: Each XBOX_KTIMER contains a Win32 event (for waitable
 * behavior) and uses CreateTimerQueueTimer for the timing mechanism.
 * When the timer fires, it signals the event and optionally invokes the DPC.
 * ============================================================================ */

/* Global timer queue - created lazily on first timer use */
static HANDLE g_timer_queue = NULL;
static CRITICAL_SECTION g_timer_cs;
static BOOL g_timer_cs_init = FALSE;

static void xbox_ensure_timer_queue(void)
{
    if (!g_timer_cs_init) {
        InitializeCriticalSection(&g_timer_cs);
        g_timer_cs_init = TRUE;
    }

    if (!g_timer_queue) {
        EnterCriticalSection(&g_timer_cs);
        if (!g_timer_queue) {
            g_timer_queue = CreateTimerQueue();
            if (!g_timer_queue) {
                xbox_log(XBOX_LOG_ERROR, XBOX_LOG_SYNC,
                    "Failed to create timer queue (error %u)", GetLastError());
            }
        }
        LeaveCriticalSection(&g_timer_cs);
    }
}

/*
 * Timer callback - called by the Windows timer queue thread.
 * Signals the event (for KeWaitForSingleObject) and fires the DPC if set.
 */
static VOID CALLBACK xbox_timer_callback(PVOID lpParameter, BOOLEAN TimerOrWaitFired)
{
    PXBOX_KTIMER timer = (PXBOX_KTIMER)lpParameter;

    (void)TimerOrWaitFired;
    xbox_CheckpointDeviceEnter(0);

    /* Signal the event so waiters wake up */
    if (timer->win32_event)
        SetEvent(timer->win32_event);

    /* Fire the DPC if one is associated */
    if (timer->Dpc && timer->Dpc->DeferredRoutine) {
        xbox_log(XBOX_LOG_TRACE, XBOX_LOG_SYNC, "Timer DPC firing: routine=%p",
            timer->Dpc->DeferredRoutine);
        timer->Dpc->DeferredRoutine(
            timer->Dpc,
            timer->Dpc->DeferredContext,
            timer->Dpc->SystemArgument1,
            timer->Dpc->SystemArgument2);
    }

    /* If not periodic, mark as no longer inserted */
    if (timer->Period == 0)
        timer->Inserted = FALSE;
    xbox_CheckpointDeviceLeave(0);
}

VOID __stdcall xbox_KeInitializeTimerEx(PXBOX_KTIMER Timer, XBOX_TIMER_TYPE Type)
{
    (void)Type;

    if (!Timer)
        return;

    memset(Timer, 0, sizeof(XBOX_KTIMER));

    /* Create a manual-reset event for notification timers,
     * auto-reset for synchronization timers */
    BOOL manual_reset = (Type == XboxNotificationTimer) ? TRUE : FALSE;
    Timer->win32_event = CreateEventW(NULL, manual_reset, FALSE, NULL);
    Timer->Inserted = FALSE;
    Timer->Period = 0;
    Timer->Dpc = NULL;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_SYNC,
        "KeInitializeTimerEx: timer=%p, type=%d, event=%p",
        Timer, Type, Timer->win32_event);
}

BOOLEAN __stdcall xbox_KeSetTimer(PXBOX_KTIMER Timer, LARGE_INTEGER DueTime, PXBOX_KDPC Dpc)
{
    return xbox_KeSetTimerEx(Timer, DueTime, 0, Dpc);
}

BOOLEAN __stdcall xbox_KeSetTimerEx(
    PXBOX_KTIMER Timer,
    LARGE_INTEGER DueTime,
    LONG Period,
    PXBOX_KDPC Dpc)
{
    BOOLEAN was_inserted;
    DWORD due_ms;
    DWORD period_ms;

    if (!Timer)
        return FALSE;

    xbox_ensure_timer_queue();

    was_inserted = Timer->Inserted;

    /* Cancel existing timer if re-arming */
    if (was_inserted && Timer->win32_timer) {
        DeleteTimerQueueTimer(g_timer_queue, Timer->win32_timer, NULL);
        Timer->win32_timer = NULL;
    }

    /* Reset the event */
    if (Timer->win32_event)
        ResetEvent(Timer->win32_event);

    Timer->Dpc = Dpc;
    Timer->Period = Period;

    /* Convert DueTime (100ns units) to milliseconds */
    if (DueTime.QuadPart < 0) {
        LONGLONG relative_100ns = -DueTime.QuadPart;
        due_ms = (DWORD)(relative_100ns / 10000);
        if (due_ms == 0 && relative_100ns > 0)
            due_ms = 1;
    } else if (DueTime.QuadPart == 0) {
        due_ms = 0;
    } else {
        LARGE_INTEGER now;
        GetSystemTimeAsFileTime((LPFILETIME)&now);
        LONGLONG diff = DueTime.QuadPart - now.QuadPart;
        due_ms = (diff > 0) ? (DWORD)(diff / 10000) : 0;
    }

    period_ms = (Period > 0) ? (DWORD)Period : 0;

    DWORD flags = 0;
    if (period_ms == 0)
        flags |= WT_EXECUTEONLYONCE;

    if (!CreateTimerQueueTimer(&Timer->win32_timer, g_timer_queue,
                               xbox_timer_callback, Timer,
                               due_ms, period_ms, flags)) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_SYNC,
            "KeSetTimerEx: CreateTimerQueueTimer failed (error %u)", GetLastError());
        return was_inserted;
    }

    Timer->Inserted = TRUE;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_SYNC,
        "KeSetTimerEx: timer=%p, due=%ums, period=%ums, dpc=%p",
        Timer, due_ms, period_ms, Dpc);

    return was_inserted;
}

BOOLEAN __stdcall xbox_KeCancelTimer(PXBOX_KTIMER Timer)
{
    BOOLEAN was_inserted;

    if (!Timer)
        return FALSE;

    was_inserted = Timer->Inserted;

    if (was_inserted && Timer->win32_timer) {
        DeleteTimerQueueTimer(g_timer_queue, Timer->win32_timer, INVALID_HANDLE_VALUE);
        Timer->win32_timer = NULL;
        Timer->Inserted = FALSE;
    }

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_SYNC,
        "KeCancelTimer: timer=%p, was_inserted=%d", Timer, was_inserted);

    return was_inserted;
}

/* ============================================================================
 * Deferred Procedure Calls (DPCs)
 *
 * Xbox DPCs are typically queued from ISRs or timer callbacks to run at
 * DISPATCH_LEVEL. On Windows, we execute them immediately or via thread pool
 * since we don't have real IRQL levels.
 * ============================================================================ */

VOID __stdcall xbox_KeInitializeDpc(
    PXBOX_KDPC Dpc,
    PKDEFERRED_ROUTINE DeferredRoutine,
    PVOID DeferredContext)
{
    if (!Dpc)
        return;

    Dpc->DeferredRoutine = DeferredRoutine;
    Dpc->DeferredContext = DeferredContext;
    Dpc->SystemArgument1 = NULL;
    Dpc->SystemArgument2 = NULL;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_SYNC,
        "KeInitializeDpc: dpc=%p, routine=%p", Dpc, DeferredRoutine);
}

/*
 * Thread pool callback for executing DPCs.
 */
static VOID CALLBACK xbox_dpc_work_callback(PTP_CALLBACK_INSTANCE Instance,
                                            PVOID Context)
{
    PXBOX_KDPC dpc = (PXBOX_KDPC)Context;

    (void)Instance;

    if (dpc && dpc->DeferredRoutine) {
        dpc->DeferredRoutine(dpc, dpc->DeferredContext,
                            dpc->SystemArgument1, dpc->SystemArgument2);
    }
}

BOOLEAN __stdcall xbox_KeInsertQueueDpc(
    PXBOX_KDPC Dpc,
    PVOID SystemArgument1,
    PVOID SystemArgument2)
{
    if (!Dpc || !Dpc->DeferredRoutine)
        return FALSE;

    Dpc->SystemArgument1 = SystemArgument1;
    Dpc->SystemArgument2 = SystemArgument2;

    /* Submit to the Windows thread pool for async execution */
    if (!TrySubmitThreadpoolCallback(xbox_dpc_work_callback, Dpc, NULL)) {
        /* Fallback: execute synchronously */
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_SYNC,
            "KeInsertQueueDpc: threadpool submit failed, executing synchronously");
        Dpc->DeferredRoutine(Dpc, Dpc->DeferredContext,
                            SystemArgument1, SystemArgument2);
    }

    return TRUE;
}

BOOLEAN __stdcall xbox_KeRemoveQueueDpc(PXBOX_KDPC Dpc)
{
    /*
     * On Windows, once submitted to the thread pool we can't easily cancel.
     * DPCs are typically very short-lived, so this is rarely called.
     * Return FALSE to indicate the DPC was not in the queue (may have already run).
     */
    (void)Dpc;
    return FALSE;
}

/* ============================================================================
 * KeSynchronizeExecution
 *
 * On Xbox, this raises IRQL to the interrupt's level and executes a routine.
 * Since we don't have real IRQLs, we just call the routine directly.
 * ============================================================================ */

BOOLEAN __stdcall xbox_KeSynchronizeExecution(
    PXBOX_KINTERRUPT Interrupt,
    PVOID SynchronizeRoutine,
    PVOID SynchronizeContext)
{
    typedef BOOLEAN (__stdcall *PKSYNCHRONIZE_ROUTINE)(PVOID);
    PKSYNCHRONIZE_ROUTINE routine = (PKSYNCHRONIZE_ROUTINE)SynchronizeRoutine;

    (void)Interrupt;

    if (!routine)
        return FALSE;

    return routine(SynchronizeContext);
}

/* ============================================================================
 * Events (continued)
 * ============================================================================ */

NTSTATUS __stdcall xbox_NtClearEvent(HANDLE EventHandle)
{
    return xbox_NtResetEvent(EventHandle,NULL);
}

/* ============================================================================
 * Mutants (mutexes)
 * ============================================================================ */

NTSTATUS __stdcall xbox_NtCreateMutant(
    PHANDLE MutantHandle,
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes,
    BOOLEAN InitialOwner)
{
    HANDLE hMutex;

    if(ObjectAttributes)return xbox_kernel_create_named(192,MutantHandle,ObjectAttributes,InitialOwner,0);

    if (!MutantHandle)
        return STATUS_INVALID_PARAMETER;

    hMutex = CreateMutexW(NULL, InitialOwner, NULL);
    if (!hMutex) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_SYNC,
            "NtCreateMutant: CreateMutexW failed (error %u)", GetLastError());
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    *MutantHandle = hMutex;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_SYNC,
        "NtCreateMutant: handle=%p initial_owner=%d", hMutex, InitialOwner);

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtReleaseMutant(HANDLE MutantHandle, PLONG PreviousCount)
{
    /*
     * ReleaseMutex reports the previous count only through its own bookkeeping,
     * which Win32 does not expose. Callers overwhelmingly use PreviousCount as
     * an ignore-me out-param; report 0 (the count before this release took it
     * to unheld) rather than leaving the caller's storage uninitialised.
     */
    if (!ReleaseMutex(MutantHandle)) {
        DWORD err = GetLastError();
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_SYNC,
            "NtReleaseMutant: ReleaseMutex failed (error %u)", err);
        /* Releasing a mutex this thread does not own is the common failure. */
        return (err == ERROR_NOT_OWNER) ? STATUS_MUTANT_NOT_OWNED
                                        : STATUS_INVALID_HANDLE;
    }

    if (PreviousCount)
        *PreviousCount = 0;

    return STATUS_SUCCESS;
}
