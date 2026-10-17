"""Decoded IF instructions and the actual production HAL device gate."""
from pathlib import Path
import shutil
import subprocess
import tempfile

import pytest

from .disasm import BasicBlock, Disassembler
from .lifter import Lifter, lift_basic_block


def decoded(hex_bytes):
    d = Disassembler()
    return [d._decode_instruction(i) for i in d._cs.disasm(bytes.fromhex(hex_bytes), 0x316077)]


def emitted(insns):
    lifter = Lifter()
    lifter.func_start = insns[0].address
    lifter.func_end = insns[-1].address + insns[-1].size
    lifter.configure_interrupt_shadows(insns)
    return lift_basic_block(lifter, BasicBlock(start=insns[0].address, instructions=insns))[0]


def test_nine_mov_completes_before_the_next_arithmetic_instruction():
    source = '\n'.join(emitted(decoded('fafb8bf083e07f')))
    assert source.index('esi = eax;') < source.index('xbox_CpuCompleteStiShadow();') < source.index('eax = eax &')
    assert 'RECOMP_UNIMPL' not in source


def test_following_compare_is_not_fused_past_the_release_boundary():
    source = '\n'.join(emitted(decoded('fafb83f8007400')))
    assert source.index('/* cmp ') < source.index('xbox_CpuCompleteStiShadow();') < source.index('if (')


def test_shadow_instruction_can_end_a_basic_block():
    insns = decoded('fafb8bf083e07f')
    lifter = Lifter()
    lifter.configure_interrupt_shadows(insns)
    source, _ = lift_basic_block(lifter, BasicBlock(start=insns[2].address, instructions=insns[2:3]))
    assert source[-1] == 'xbox_CpuCompleteStiShadow();'


@pytest.mark.parametrize('machine_code', ['ebfe', 'e2fe', 'e3fe', '83f80074fb', '83c00175fb'])
def test_taken_decoded_backedge_offers_the_original_guest_cpu(machine_code):
    source = '\n'.join(emitted(decoded(machine_code)))
    assert 'guest_cpu_yield(); goto loc_00316077;' in source
    if machine_code not in ('ebfe',):
        assert source.index('if (') < source.index('guest_cpu_yield();')


@pytest.mark.parametrize('machine_code', ['eb00', 'e200', 'e300', '83f8007400'])
def test_forward_decoded_branch_does_not_offer_a_quantum(machine_code):
    assert 'guest_cpu_yield' not in '\n'.join(emitted(decoded(machine_code)))


@pytest.mark.parametrize('following', ['c3', 'eb00', '7400', 'e800000000'])
def test_unrepresented_control_transfer_shadow_stops(following):
    assert 'RECOMP_UNIMPL("sti"' in '\n'.join(emitted(decoded('fb' + following)))


def test_actual_hal_masks_and_unmasks_without_changing_irql():
    compiler = shutil.which('cl')
    assert compiler, 'requires the existing MSVC developer environment'
    root = Path(__file__).resolve().parents[2]
    program = r'''
#include "kernel/kernel.h"
#include "kernel/xbox_memory_layout.h"
#include "nv2a/nv2a_state.h"
#include <assert.h>
#include <stdlib.h>
RECOMP_TLS uint32_t g_fs_base, g_esp;
ptrdiff_t g_xbox_mem_offset;
void xbox_log(int level, const char *subsystem, const char *format, ...) { }
NV2AState *nv2a_get_state(void) { abort(); } /* PCI is outside this IF fixture. */
int xbox_CheckpointFrozen(void) { return 0; }
static HANDLE ready, proceed;
static DWORD WINAPI other_mask(LPVOID arg) {
    assert(xbox_CpuInterruptFlag() == 1);
    xbox_CpuCli(); xbox_CpuCli(); SetEvent(ready);
    assert(WaitForSingleObject(proceed, 5000) == WAIT_OBJECT_0);
    xbox_CpuSti(); assert(xbox_CpuInterruptFlag() == 1);
    assert(xbox_IrqlBlocksInterrupts());
    xbox_CpuCompleteStiShadow();
    assert(xbox_IrqlBlocksInterrupts()); /* main thread still owns a mask */
    return 0;
}
int main(void) {
    assert(!xbox_IrqlBlocksInterrupts());
    xbox_CpuSti(); xbox_CpuCompleteStiShadow();
    assert(!xbox_IrqlBlocksInterrupts()); /* STI with IF already set */
    xbox_CpuCli(); xbox_CpuCli();
    assert(xbox_CpuInterruptFlag() == 0 && xbox_IrqlRaisedCount() == 0);
    Sleep(2100); assert(xbox_IrqlBlocksInterrupts());
    xbox_CpuSti(); assert(xbox_CpuInterruptFlag() == 1 && xbox_IrqlBlocksInterrupts());
    xbox_CpuSti(); xbox_CpuCompleteStiShadow();
    assert(!xbox_IrqlBlocksInterrupts());
    xbox_CpuCli(); xbox_CpuSti(); xbox_CpuCli(); xbox_CpuCompleteStiShadow();
    assert(xbox_CpuInterruptFlag() == 0 && xbox_IrqlBlocksInterrupts());
    xbox_CpuSti(); xbox_CpuCompleteStiShadow();
    assert(!xbox_IrqlBlocksInterrupts());
    assert(xbox_KfRaiseIrql(DISPATCH_LEVEL) == PASSIVE_LEVEL);
    xbox_CpuCli(); xbox_CpuSti(); xbox_CpuCompleteStiShadow();
    assert(xbox_CpuInterruptFlag() == 1 && xbox_IrqlRaisedCount() == 1 && xbox_IrqlBlocksInterrupts());
    xbox_KfLowerIrql(PASSIVE_LEVEL); assert(!xbox_IrqlBlocksInterrupts());
    int saved=xbox_CpuEnterInterrupt(); assert(xbox_CpuInterruptFlag() == 0);
    assert(xbox_IrqlBlocksInterrupts()); xbox_CpuLeaveInterrupt(saved);
    assert(xbox_CpuInterruptFlag() == 1 && !xbox_IrqlBlocksInterrupts());
    xbox_CpuCli(); saved=xbox_CpuEnterInterrupt();
    xbox_CpuSti(); xbox_CpuCompleteStiShadow(); assert(!xbox_IrqlBlocksInterrupts());
    xbox_CpuLeaveInterrupt(saved); assert(xbox_CpuInterruptFlag() == 0 && xbox_IrqlBlocksInterrupts());
    xbox_CpuSti(); xbox_CpuCompleteStiShadow();
    ready=CreateEventA(0,1,0,0); proceed=CreateEventA(0,1,0,0);
    xbox_CpuCli(); HANDLE t=CreateThread(0,0,other_mask,0,0,0); assert(t);
    assert(WaitForSingleObject(ready,5000) == WAIT_OBJECT_0);
    SetEvent(proceed); assert(WaitForSingleObject(t,5000) == WAIT_OBJECT_0);
    assert(xbox_IrqlBlocksInterrupts());
    xbox_CpuSti(); xbox_CpuCompleteStiShadow(); assert(!xbox_IrqlBlocksInterrupts());
    CloseHandle(t); CloseHandle(ready); CloseHandle(proceed); return 0;
}
'''
    with tempfile.TemporaryDirectory() as temporary:
        work = Path(temporary)
        source = work/'interrupts.c'
        source.write_text(program, encoding='utf-8')
        result = subprocess.run([compiler, '/nologo', '/O2', '/Gy', '/DWIN32_LEAN_AND_MEAN', '/DNOMINMAX',
            '/I'+str(root/'src'), str(source), str(root/'src/kernel/kernel_hal.c'),
            '/Fe:'+str(work/'interrupts.exe'), '/link', '/OPT:REF', 'kernel32.lib'],
            cwd=work, capture_output=True, text=True, errors='replace')
        assert result.returncode == 0, result.stdout+result.stderr
        subprocess.run([str(work/'interrupts.exe')], check=True, timeout=12)
