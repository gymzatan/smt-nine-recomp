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
    qemu_mutex_lock(&d->pfifo.lock);
    uint32_t value = d->pfifo.regs[user_register(d, addr, size)];
    qemu_mutex_unlock(&d->pfifo.lock);
    return value;
}

void user_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    NV2AState *d = opaque;
    qemu_mutex_lock(&d->pfifo.lock);
    d->pfifo.regs[user_register(d, addr, size)] = (uint32_t)value;
    nv2a_fifo_run(d);
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

static void consume_method(NV2AState *d, uint32_t subchannel,
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

/* Original PFIFO header/control-flow and persistent partial-method state.
 * GET advances after each consumed word; no inferred ring bounds or GET=PUT
 * acknowledgement. Device-disabled and suspended states retain pending work. */
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
    if (regs[NV_PFIFO_CACHE1_DMA_GET] == regs[NV_PFIFO_CACHE1_DMA_PUT])
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
    while (regs[NV_PFIFO_CACHE1_DMA_GET] != regs[NV_PFIFO_CACHE1_DMA_PUT]) {
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
                if (increment) {
                    method = (method + 4) & NV_PFIFO_CACHE1_DMA_STATE_METHOD;
                    SET_MASK(*state, NV_PFIFO_CACHE1_DMA_STATE_METHOD, method >> 2);
                }
                SET_MASK(*state, NV_PFIFO_CACHE1_DMA_STATE_METHOD_COUNT, --count);
                regs[NV_PFIFO_CACHE1_DMA_DCOUNT]++;
                regs[NV_PFIFO_CACHE1_STATUS] |= NV_PFIFO_CACHE1_STATUS_LOW_MARK;
                regs[NV_PFIFO_CACHE1_DMA_GET] = next;
                if (d->pgraph.waiting_for_nop)
                    return;
                if (!count || next == regs[NV_PFIFO_CACHE1_DMA_PUT])
                    break;
                get = next;
                if ((get & 3) || (hwaddr)get + 4 > dma_extent)
                    fifo_stop(d, "DMA read outside mapped data", get);
                word = ldl_le_p(dma + get);
                next = get + 4;
            } while (true);
            continue;
        } else {
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
        regs[NV_PFIFO_CACHE1_DMA_GET] = next;
        if (d->pgraph.waiting_for_nop)
            return;
    }
}
