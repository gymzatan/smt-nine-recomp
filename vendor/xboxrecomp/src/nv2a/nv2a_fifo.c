/*
 * NV2A PFIFO DMA submission, adapted from xemu user.c and pfifo.c.
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version. This library is distributed without any warranty;
 * see the GNU Lesser General Public License for details.
 *
 * Source: xemu 478b4f496102379c7eaa7f3ec10e714a703c4300.
 * Uses the existing NV2A DMA objects, RAMHT identities and PFIFO registers.
 * The existing guest-memory renderer consumes resolved NV097 methods.
 */
#include "nv2a_state.h"
#include "kernel/kernel.h"
#include "kernel/nv2a_vsh_interp.h"
extern int nv2a_pb_exec_method(uint32_t, uint32_t, uint32_t, uint8_t *);
extern void nv2a_pb_index_words(const uint32_t *,uint32_t);

extern void nv2a_pb_progress(void);
static HANDLE s_fifo_ready, s_fifo_wake;

/* The original PFIFO queue is the work queue. Submission only wakes its
 * single consumer; GET still advances inside the original parser. */
void nv2a_fifo_kick(NV2AState *d)
{
    if (!d->pfifo.thread.thread) {
        nv2a_fifo_run(d);
        return;
    }
    if (!ReadAcquire(&d->pfifo.fifo_kick) &&
        !InterlockedExchange(&d->pfifo.fifo_kick, 1)) SetEvent(s_fifo_wake);
}

static DWORD WINAPI nv2a_fifo_worker(LPVOID opaque)
{
    NV2AState *d = opaque;
    xbox_CheckpointDeviceEnter(XBOX_CHECKPOINT_GPU);
    xbox_CheckpointDeviceLeave(XBOX_CHECKPOINT_GPU);
    SetEvent(s_fifo_ready);
    for (;;) {
        /* A posted kick already requests consumption. The existing event
         * handles the idle case; a retained signal can only add an idle pass. */
        if (!ReadAcquire(&d->pfifo.fifo_kick) &&
            WaitForSingleObject(s_fifo_wake, INFINITE) != WAIT_OBJECT_0) {
            fprintf(stderr, "[STOP] PFIFO work wait failed\n");
            exit(5);
        }
        qemu_mutex_lock(&d->pfifo.lock);
        bool exiting = d->exiting;
        qemu_mutex_unlock(&d->pfifo.lock);
        if (exiting) break;

        /* Never hold the device lock while waiting for checkpoint thaw. */
        xbox_CheckpointDeviceEnter(XBOX_CHECKPOINT_GPU);
        qemu_mutex_lock(&d->pfifo.lock);
        InterlockedExchange(&d->pfifo.fifo_kick, 0);
        nv2a_fifo_run(d);
        nv2a_pb_progress();
        qemu_cond_broadcast(&d->pfifo.fifo_idle_cond);
        qemu_mutex_unlock(&d->pfifo.lock);
        xbox_CheckpointDeviceLeave(XBOX_CHECKPOINT_GPU);
    }
    return 0;
}

int nv2a_fifo_start(NV2AState *d, int threaded)
{
    if (!d || d->pfifo.thread.thread) return 0;
    if (!threaded) {
        fprintf(stderr, "[PFIFO] graphics commands run on the guest execution thread\n");
        return 1;
    }
    s_fifo_wake = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!s_fifo_wake) return 0;
    s_fifo_ready = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!s_fifo_ready) { CloseHandle(s_fifo_wake);s_fifo_wake = NULL;return 0; }
    d->exiting = false;
    xbox_CheckpointDeviceCreating(XBOX_CHECKPOINT_GPU);
    d->pfifo.thread.thread = CreateThread(NULL, 0, nv2a_fifo_worker, d, 0, NULL);
    if (!d->pfifo.thread.thread) {
        xbox_CheckpointDeviceCreated(XBOX_CHECKPOINT_GPU);
        CloseHandle(s_fifo_ready);s_fifo_ready = NULL;
        CloseHandle(s_fifo_wake);s_fifo_wake = NULL;
        return 0;
    }
    DWORD result = WaitForSingleObject(s_fifo_ready, INFINITE);
    CloseHandle(s_fifo_ready);s_fifo_ready = NULL;
    if (result != WAIT_OBJECT_0) {
        fprintf(stderr, "[STOP] PFIFO worker startup wait failed\n");
        exit(5);
    }
    fprintf(stderr, "[PFIFO] ordered graphics consumer running on a host thread\n");
    return 1;
}

void nv2a_fifo_wait_idle(NV2AState *d)
{
    qemu_mutex_lock(&d->pfifo.lock);
    while (ReadAcquire(&d->pfifo.fifo_kick))
        qemu_cond_wait(&d->pfifo.fifo_idle_cond, &d->pfifo.lock);
    qemu_mutex_unlock(&d->pfifo.lock);
}

void nv2a_fifo_stop(NV2AState *d)
{
    if (!d || !d->pfifo.thread.thread) return;
    qemu_mutex_lock(&d->pfifo.lock);
    d->exiting = true;
    SetEvent(s_fifo_wake);
    qemu_mutex_unlock(&d->pfifo.lock);
    if (WaitForSingleObject(d->pfifo.thread.thread, INFINITE) != WAIT_OBJECT_0) {
        fprintf(stderr, "[STOP] PFIFO worker shutdown wait failed\n");
        exit(5);
    }
    CloseHandle(d->pfifo.thread.thread);d->pfifo.thread.thread = NULL;
    CloseHandle(s_fifo_wake);s_fifo_wake = NULL;
}

static void fifo_stop(NV2AState *d, const char *reason, uint32_t word)
{
    fprintf(stderr, "[STOP] PFIFO %s: word=0x%08X GET=0x%08X PUT=0x%08X state=0x%08X\n",
            reason, word, d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET],
            d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT],
            d->pfifo.regs[NV_PFIFO_CACHE1_DMA_STATE]);
    exit(5);
}

static uint32_t user_register(NV2AState *d, hwaddr addr, unsigned size)
{
    uint32_t channel = (uint32_t)(addr >> 16);
    uint32_t active = GET_MASK(d->pfifo.regs[NV_PFIFO_CACHE1_PUSH1],
                               NV_PFIFO_CACHE1_PUSH1_CHID);
    if (size != 4 || channel >= NV2A_NUM_CHANNELS || channel != active ||
        !(d->pfifo.regs[NV_PFIFO_MODE] & (1u << channel)))
        fifo_stop(d, "unsupported USER channel/mode/width", (uint32_t)addr);
    switch (addr & 0xFFFF) {
    case NV_USER_DMA_PUT: return NV_PFIFO_CACHE1_DMA_PUT;
    case NV_USER_DMA_GET: return NV_PFIFO_CACHE1_DMA_GET;
    case NV_USER_REF: return NV_PFIFO_CACHE1_REF;
    default: fifo_stop(d, "unsupported USER register", (uint32_t)addr); return 0;
    }
}

uint64_t user_read(void *opaque, hwaddr addr, unsigned size)
{
    NV2AState *d = opaque;
    /* Channel/mode are set by the serialized guest CPU. GET and PUT are
     * the original queue's release/acquire ownership boundary. */
    uint32_t reg = user_register(d, addr, size);
    if (reg == NV_PFIFO_CACHE1_DMA_GET || reg == NV_PFIFO_CACHE1_DMA_PUT)
        return (uint32_t)ReadAcquire((volatile LONG *)&d->pfifo.regs[reg]);
    qemu_mutex_lock(&d->pfifo.lock);
    uint32_t value = d->pfifo.regs[reg];
    qemu_mutex_unlock(&d->pfifo.lock);
    return value;
}

void user_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    NV2AState *d = opaque;
    uint32_t reg = user_register(d, addr, size);
    if (reg == NV_PFIFO_CACHE1_DMA_PUT && d->pfifo.thread.thread) {
        WriteRelease((volatile LONG *)&d->pfifo.regs[reg], (LONG)value);
        nv2a_fifo_kick(d);
        return;
    }
    qemu_mutex_lock(&d->pfifo.lock);
    WriteRelease((volatile LONG *)&d->pfifo.regs[reg], (LONG)value);
    nv2a_fifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
}

/* RAMHT hash and identity resolution from the original xemu PFIFO. */
static uint32_t object_instance(NV2AState *d, uint32_t handle, bool graphics)
{
    uint32_t ramht = d->pfifo.regs[NV_PFIFO_RAMHT];
    uint32_t size = 1u << (GET_MASK(ramht, NV_PFIFO_RAMHT_SIZE) + 12);
    unsigned bits = ctz32(size) - 1;
    uint32_t hash = 0, remaining = handle;
    while (remaining) {
        hash ^= remaining & ((1u << bits) - 1);
        remaining >>= bits;
    }
    uint32_t channel = GET_MASK(d->pfifo.regs[NV_PFIFO_CACHE1_PUSH1],
                                NV_PFIFO_CACHE1_PUSH1_CHID);
    hash ^= channel << (bits - 4);
    hwaddr address = (GET_MASK(ramht, NV_PFIFO_RAMHT_BASE_ADDRESS) << 12) + hash * 8;
    if (hash * 8 >= size || address + 8 > memory_region_size(&d->ramin))
        fifo_stop(d, "RAMHT address outside RAMIN", handle);
    uint32_t entry_handle = ldl_le_p(d->ramin_ptr + address);
    uint32_t context = ldl_le_p(d->ramin_ptr + address + 4);
    if (entry_handle != handle || !(context & NV_RAMHT_STATUS) ||
        GET_MASK(context, NV_RAMHT_CHID) != channel ||
        (graphics && GET_MASK(context, NV_RAMHT_ENGINE) != ENGINE_GRAPHICS))
        fifo_stop(d, "unresolved RAMHT object", handle);
    return GET_MASK(context, NV_RAMHT_INSTANCE) << 4;
}

static
#ifdef _MSC_VER
__forceinline
#else
inline __attribute__((always_inline))
#endif
void consume_method(NV2AState *d, uint32_t subchannel,
                            uint32_t method, uint32_t parameter)
{
    if (method == 0) {
        uint32_t instance = object_instance(d, parameter, true);
        if ((hwaddr)instance + 16 > memory_region_size(&d->ramin))
            fifo_stop(d, "graphics object outside RAMIN", parameter);
        uint8_t object_class = ldl_le_p(d->ramin_ptr + instance) &
                               NV_PGRAPH_CTX_SWITCH1_GRCLASS;
        SET_MASK(d->pfifo.regs[NV_PFIFO_CACHE1_ENGINE], 3u << (subchannel * 4), ENGINE_GRAPHICS);
        SET_MASK(d->pfifo.regs[NV_PFIFO_CACHE1_PULL1], NV_PFIFO_CACHE1_PULL1_ENGINE, ENGINE_GRAPHICS);
        fprintf(stderr, "[PFIFO] bind subchannel=%u handle=0x%08X instance=0x%X class=0x%02X\n",
                subchannel, parameter, instance, object_class);
        pgraph_method(d, subchannel, 0, instance);
        return;
    }
    if (method < 0x100)
        fifo_stop(d, "unsupported puller method", method);
    if (method >= 0x180 && method < 0x200)
        parameter = object_instance(d, parameter, false);
    pgraph_method(d, subchannel, method, parameter);
}

/* The native producer publishes one immutable range in the original queue.
 * A partial operation stays pending at its tag, including across checkpoints.
 * The same storage cursor advances only after every value has been consumed. */
static
#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
int consume_native_constants(NV2AState *d,const uint8_t *dma,
                             hwaddr extent,uint32_t get)
{
    uint32_t *regs=d->pfifo.regs;
    uint32_t put=(uint32_t)ReadAcquire((volatile LONG *)&regs[NV_PFIFO_CACHE1_DMA_PUT]);
    if ((put&3) || (hwaddr)get+12>extent)
        fifo_stop(d,"invalid native constant header",put);
    if (put>get && (hwaddr)put<(hwaddr)get+12) return 0;
    uint32_t count=ldl_le_p(dma+get+8);
    if (count!=4 && count!=16)
        fifo_stop(d,"invalid native constant count",count);
    hwaddr end=(hwaddr)get+(count+3)*4;
    if (end>extent || end>UINT32_MAX)
        fifo_stop(d,"native constants outside DMA data",count);
    if (put>get && (hwaddr)put<end) return 0;
    if (!(regs[NV_PFIFO_CACHE1_PULL0]&NV_PFIFO_CACHE1_PULL0_ACCESS)) return 0;
    if (d->pfifo.method_consumer!=nv2a_pb_exec_method ||
        (d->pgraph.regs[NV_PGRAPH_CTX_CACHE1]&NV_PGRAPH_CTX_SWITCH1_GRCLASS)!=NV_KELVIN_PRIMITIVE)
        fifo_stop(d,"native constants require the active game renderer",count);

    regs[NV_PFIFO_CACHE1_STATUS]&=~NV_PFIFO_CACHE1_STATUS_LOW_MARK;
    /* This establishes the original context, initialization and load cursor. */
    pgraph_method(d,0,NV097_SET_TRANSFORM_CONSTANT_LOAD,ldl_le_p(dma+get+4));
    nv2a_vsh_constant_words((const uint32_t *)(dma+get+12),count);

    /* Retain the driver-visible completed state of the replaced operation. */
    regs[NV_PFIFO_CACHE1_DMA_RSVD_SHADOW]=(count<<18)|NV097_SET_TRANSFORM_CONSTANT;
    regs[NV_PFIFO_CACHE1_DMA_DATA_SHADOW]=ldl_le_p(dma+(uint32_t)end-4);
    uint32_t state=regs[NV_PFIFO_CACHE1_DMA_STATE];
    SET_MASK(state,NV_PFIFO_CACHE1_DMA_STATE_METHOD,(NV097_SET_TRANSFORM_CONSTANT+count*4)>>2);
    SET_MASK(state,NV_PFIFO_CACHE1_DMA_STATE_SUBCHANNEL,0);
    SET_MASK(state,NV_PFIFO_CACHE1_DMA_STATE_METHOD_COUNT,0);
    SET_MASK(state,NV_PFIFO_CACHE1_DMA_STATE_METHOD_TYPE,0);
    regs[NV_PFIFO_CACHE1_DMA_STATE]=state;
    regs[NV_PFIFO_CACHE1_DMA_DCOUNT]=count;
    regs[NV_PFIFO_CACHE1_STATUS]|=NV_PFIFO_CACHE1_STATUS_LOW_MARK;
    WriteRelease((volatile LONG *)&regs[NV_PFIFO_CACHE1_DMA_GET],(LONG)end);
    return 1;
}

/* The game resource owns the source indices; the original queue owns this
 * immutable copy. Pending publication remains at its tag until complete. */
static
#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
int consume_native_indices(NV2AState *d,const uint8_t *dma,hwaddr extent,
                           uint32_t get,uint32_t header)
{
    uint32_t *regs=d->pfifo.regs;
    uint32_t count=(header&NV2A_NATIVE_INDEX_COUNT_MASK)>>18;
    uint32_t put=(uint32_t)ReadAcquire((volatile LONG *)&regs[NV_PFIFO_CACHE1_DMA_PUT]);
    hwaddr end=(hwaddr)get+(count+1)*4;
    if ((put&3) || end>extent || end>UINT32_MAX)
        fifo_stop(d,"native index range outside DMA data",header);
    if (put>get && (hwaddr)put<end) return 0;
    if (count) {
        if (!(regs[NV_PFIFO_CACHE1_PULL0]&NV_PFIFO_CACHE1_PULL0_ACCESS)) return 0;
        if (d->pfifo.method_consumer!=nv2a_pb_exec_method ||
            (d->pgraph.regs[NV_PGRAPH_CTX_CACHE1]&NV_PGRAPH_CTX_SWITCH1_GRCLASS)!=NV_KELVIN_PRIMITIVE)
            fifo_stop(d,"native indices require the active game renderer",header);
        regs[NV_PFIFO_CACHE1_STATUS]&=~NV_PFIFO_CACHE1_STATUS_LOW_MARK;
        pgraph_method(d,0,NV097_ARRAY_ELEMENT16,ldl_le_p(dma+get+4));
        nv2a_pb_index_words((const uint32_t *)(dma+get+8),count-1);
        regs[NV_PFIFO_CACHE1_DMA_DATA_SHADOW]=ldl_le_p(dma+(uint32_t)end-4);
        regs[NV_PFIFO_CACHE1_STATUS]|=NV_PFIFO_CACHE1_STATUS_LOW_MARK;
    }
    regs[NV_PFIFO_CACHE1_DMA_RSVD_SHADOW]=(count<<18)|0x40000000u|NV097_ARRAY_ELEMENT16;
    uint32_t state=regs[NV_PFIFO_CACHE1_DMA_STATE];
    SET_MASK(state,NV_PFIFO_CACHE1_DMA_STATE_METHOD,NV097_ARRAY_ELEMENT16>>2);
    SET_MASK(state,NV_PFIFO_CACHE1_DMA_STATE_SUBCHANNEL,0);
    SET_MASK(state,NV_PFIFO_CACHE1_DMA_STATE_METHOD_COUNT,0);
    SET_MASK(state,NV_PFIFO_CACHE1_DMA_STATE_METHOD_TYPE,1);
    regs[NV_PFIFO_CACHE1_DMA_STATE]=state;
    regs[NV_PFIFO_CACHE1_DMA_DCOUNT]=count;
    WriteRelease((volatile LONG *)&regs[NV_PFIFO_CACHE1_DMA_GET],(LONG)end);
    return 1;
}

/* The game already packs its shader resource into immutable upload blocks.
 * Consume that resource through the original storage and program-word writer;
 * incomplete publication remains pending at the native operation's tag. */
static
#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
int consume_native_program(NV2AState *d,const uint8_t *dma,hwaddr extent,
                            uint32_t get,uint32_t header)
{
    uint32_t *regs=d->pfifo.regs;
    uint32_t words=(header&NV2A_NATIVE_PROGRAM_COUNT_MASK)>>18;
    hwaddr end=(hwaddr)get+(words+2)*4;
    uint32_t put=(uint32_t)ReadAcquire((volatile LONG *)&regs[NV_PFIFO_CACHE1_DMA_PUT]);
    if ((put&3) || end>extent || end>UINT32_MAX)
        fifo_stop(d,"native program resource outside DMA data",header);
    if (put>get && (hwaddr)put<end) return 0;
    if (!(regs[NV_PFIFO_CACHE1_PULL0]&NV_PFIFO_CACHE1_PULL0_ACCESS)) return 0;
    if (d->pfifo.method_consumer!=nv2a_pb_exec_method ||
        (d->pgraph.regs[NV_PGRAPH_CTX_CACHE1]&NV_PGRAPH_CTX_SWITCH1_GRCLASS)!=NV_KELVIN_PRIMITIVE)
        fifo_stop(d,"native program resource requires the active game renderer",header);
    regs[NV_PFIFO_CACHE1_STATUS]&=~NV_PFIFO_CACHE1_STATUS_LOW_MARK;
    uint32_t load=ldl_le_p(dma+get+4);
    pgraph_method(d,0,NV097_SET_TRANSFORM_PROGRAM_LOAD,load);
    uint32_t last_header=(1u<<18)|NV097_SET_TRANSFORM_PROGRAM_LOAD;
    uint32_t last_data=load,last_count=1,last_method=NV097_SET_TRANSFORM_PROGRAM_LOAD+4;
    for (hwaddr at=(hwaddr)get+8;at<end;) {
        uint32_t block=ldl_le_p(dma+at),count=(block>>18)&0x7FFu;at+=4;
        if ((block&~0x1FFC0000u)!=NV097_SET_TRANSFORM_PROGRAM || count>32 || at+(hwaddr)count*4>end)
            fifo_stop(d,"native program resource contains an invalid upload block",block);
        nv2a_vsh_program_words((const uint32_t *)(dma+at),count);
        if (count) last_data=ldl_le_p(dma+at+(count-1)*4);
        last_header=block;last_count=count;last_method=NV097_SET_TRANSFORM_PROGRAM+count*4;
        at+=(hwaddr)count*4;
    }
    regs[NV_PFIFO_CACHE1_DMA_RSVD_SHADOW]=last_header;
    regs[NV_PFIFO_CACHE1_DMA_DATA_SHADOW]=last_data;
    uint32_t state=regs[NV_PFIFO_CACHE1_DMA_STATE];
    SET_MASK(state,NV_PFIFO_CACHE1_DMA_STATE_METHOD,last_method>>2);
    SET_MASK(state,NV_PFIFO_CACHE1_DMA_STATE_SUBCHANNEL,0);
    SET_MASK(state,NV_PFIFO_CACHE1_DMA_STATE_METHOD_COUNT,0);
    SET_MASK(state,NV_PFIFO_CACHE1_DMA_STATE_METHOD_TYPE,0);
    regs[NV_PFIFO_CACHE1_DMA_STATE]=state;
    regs[NV_PFIFO_CACHE1_DMA_DCOUNT]=last_count;
    regs[NV_PFIFO_CACHE1_STATUS]|=NV_PFIFO_CACHE1_STATUS_LOW_MARK;
    WriteRelease((volatile LONG *)&regs[NV_PFIFO_CACHE1_DMA_GET],(LONG)end);
    return 1;
}

/* Original PFIFO header/control-flow and persistent partial-method state.
 * GET advances after each legacy word or fully consumed native range; no
 * inferred ring bounds or GET=PUT acknowledgement. Disabled and suspended
 * states retain pending work. */
void nv2a_fifo_run(NV2AState *d)
{
    uint32_t *regs = d->pfifo.regs;
    if (d->pgraph.waiting_for_nop || d->irq_in_service)
        return;
    if (!(regs[NV_PFIFO_CACHE1_PUSH0] & NV_PFIFO_CACHE1_PUSH0_ACCESS) ||
        !(regs[NV_PFIFO_CACHE1_DMA_PUSH] & NV_PFIFO_CACHE1_DMA_PUSH_ACCESS) ||
        (regs[NV_PFIFO_CACHE1_DMA_PUSH] & NV_PFIFO_CACHE1_DMA_PUSH_STATUS) ||
        !(d->pgraph.regs[NV_PGRAPH_FIFO] & NV_PGRAPH_FIFO_ACCESS))
        return;
    uint32_t channel = GET_MASK(regs[NV_PFIFO_CACHE1_PUSH1], NV_PFIFO_CACHE1_PUSH1_CHID);
    if (!(regs[NV_PFIFO_MODE] & (1u << channel)) ||
        !GET_MASK(regs[NV_PFIFO_CACHE1_PUSH1], NV_PFIFO_CACHE1_PUSH1_MODE))
        fifo_stop(d, "active pusher is not in DMA mode", channel);
    if (regs[NV_PFIFO_CACHE1_DMA_GET] == (uint32_t)ReadAcquire((volatile LONG *)&regs[NV_PFIFO_CACHE1_DMA_PUT]))
        return;
    hwaddr instance = GET_MASK(regs[NV_PFIFO_CACHE1_DMA_INSTANCE], NV_PFIFO_CACHE1_DMA_INSTANCE_ADDRESS) << 4;
    if (instance + 12 > memory_region_size(&d->ramin))
        fifo_stop(d, "DMA object outside RAMIN", (uint32_t)instance);
    hwaddr dma_length;
    uint8_t *dma = nv_dma_map(d, instance, &dma_length);
    if (!dma)
        fifo_stop(d, "DMA object is not mapped", (uint32_t)instance);
    hwaddr dma_extent = memory_region_size(d->vram) - (hwaddr)(dma - d->vram_ptr);
    if (dma_length < dma_extent)
        dma_extent = dma_length + 1;
    while (regs[NV_PFIFO_CACHE1_DMA_GET] != (uint32_t)ReadAcquire((volatile LONG *)&regs[NV_PFIFO_CACHE1_DMA_PUT])) {
        uint32_t get = regs[NV_PFIFO_CACHE1_DMA_GET];
        if ((get & 3) || (hwaddr)get + 4 > dma_extent)
            fifo_stop(d, "DMA read outside mapped data", get);
        uint32_t word = ldl_le_p(dma + get);
        uint32_t next = get + 4;
        uint32_t *state = &regs[NV_PFIFO_CACHE1_DMA_STATE];
        uint32_t count = GET_MASK(*state, NV_PFIFO_CACHE1_DMA_STATE_METHOD_COUNT);
        if (count) {
            uint32_t method = GET_MASK(*state, NV_PFIFO_CACHE1_DMA_STATE_METHOD) << 2;
            uint32_t subchannel = GET_MASK(*state, NV_PFIFO_CACHE1_DMA_STATE_SUBCHANNEL);
            bool increment = !GET_MASK(*state, NV_PFIFO_CACHE1_DMA_STATE_METHOD_TYPE);
            do {
                if (!(regs[NV_PFIFO_CACHE1_PULL0] & NV_PFIFO_CACHE1_PULL0_ACCESS))
                    return;
                regs[NV_PFIFO_CACHE1_DMA_DATA_SHADOW] = word;
                regs[NV_PFIFO_CACHE1_STATUS] &= ~NV_PFIFO_CACHE1_STATUS_LOW_MARK;
                consume_method(d, subchannel, method, word);
                uint32_t next_state = *state;
                if (increment) {
                    method = (method + 4) & NV_PFIFO_CACHE1_DMA_STATE_METHOD;
                    SET_MASK(next_state, NV_PFIFO_CACHE1_DMA_STATE_METHOD, method >> 2);
                }
                SET_MASK(next_state, NV_PFIFO_CACHE1_DMA_STATE_METHOD_COUNT, --count);
                *state = next_state;
                regs[NV_PFIFO_CACHE1_DMA_DCOUNT]++;
                regs[NV_PFIFO_CACHE1_STATUS] |= NV_PFIFO_CACHE1_STATUS_LOW_MARK;
                WriteRelease((volatile LONG *)&regs[NV_PFIFO_CACHE1_DMA_GET], (LONG)next);
                if (d->pgraph.waiting_for_nop)
                    return;
                if (!count || next == (uint32_t)ReadAcquire((volatile LONG *)&regs[NV_PFIFO_CACHE1_DMA_PUT]))
                    break;
                get = next;
                if ((get & 3) || (hwaddr)get + 4 > dma_extent)
                    fifo_stop(d, "DMA read outside mapped data", get);
                word = ldl_le_p(dma + get);
                next = get + 4;
            } while (true);
            continue;
        } else {
            if (word == NV2A_NATIVE_CONSTANT_RANGE) {
                if (!consume_native_constants(d,dma,dma_extent,get)) return;
                continue;
            }
            if ((word&~NV2A_NATIVE_INDEX_COUNT_MASK)==NV2A_NATIVE_INDEX_RANGE) {
                if (!consume_native_indices(d,dma,dma_extent,get,word)) return;
                continue;
            }
            if ((word&~NV2A_NATIVE_PROGRAM_COUNT_MASK)==NV2A_NATIVE_PROGRAM_RESOURCE) {
                if (!consume_native_program(d,dma,dma_extent,get,word)) return;
                continue;
            }
            regs[NV_PFIFO_CACHE1_DMA_RSVD_SHADOW] = word;
            uint32_t *subroutine = &regs[NV_PFIFO_CACHE1_DMA_SUBROUTINE];
            if ((word & 0xE0000003u) == 0x20000000u) {
                regs[NV_PFIFO_CACHE1_DMA_GET_JMP_SHADOW] = next;
                next = word & 0x1FFFFFFF;
            } else if ((word & 3) == 1) {
                regs[NV_PFIFO_CACHE1_DMA_GET_JMP_SHADOW] = next;
                next = word & 0xFFFFFFFC;
            } else if ((word & 3) == 2) {
                if (*subroutine & NV_PFIFO_CACHE1_DMA_SUBROUTINE_STATE)
                    fifo_stop(d, "nested DMA CALL", word);
                *subroutine = next | NV_PFIFO_CACHE1_DMA_SUBROUTINE_STATE;
                next = word & 0xFFFFFFFC;
            } else if (word == 0x00020000) {
                if (!(*subroutine & NV_PFIFO_CACHE1_DMA_SUBROUTINE_STATE))
                    fifo_stop(d, "DMA RETURN without CALL", word);
                next = *subroutine & 0xFFFFFFFC;
                *subroutine &= ~NV_PFIFO_CACHE1_DMA_SUBROUTINE_STATE;
            } else if ((word & 0xE0030003u) == 0 ||
                       (word & 0xE0030003u) == 0x40000000u) {
                SET_MASK(*state, NV_PFIFO_CACHE1_DMA_STATE_METHOD, (word & 0x1FFF) >> 2);
                SET_MASK(*state, NV_PFIFO_CACHE1_DMA_STATE_SUBCHANNEL, (word >> 13) & 7);
                SET_MASK(*state, NV_PFIFO_CACHE1_DMA_STATE_METHOD_COUNT, (word >> 18) & 0x7FF);
                SET_MASK(*state, NV_PFIFO_CACHE1_DMA_STATE_METHOD_TYPE, word >> 30);
                regs[NV_PFIFO_CACHE1_DMA_DCOUNT] = 0;
            } else {
                fifo_stop(d, "reserved DMA command", word);
            }
        }
        WriteRelease((volatile LONG *)&regs[NV_PFIFO_CACHE1_DMA_GET], (LONG)next);
        if (d->pgraph.waiting_for_nop)
            return;
    }
}
