"""Execute translated conditions against width-bounded integer arithmetic."""
from .disasm import Instruction, Operand
from .lifter import Lifter, _make_condition, _EFLAGS_PRESERVE
from .test_lifter_pushal import _build_and_run


def test_timestamp_and_cpu_identity_preserve_flags():
    assert {'rdtsc', 'cpuid'} <= _EFLAGS_PRESERVE


def test_arithmetic_conditions_at_operand_width():
    conditions = ('jo', 'jno', 'jp', 'jnp', 'jl', 'jge', 'jle', 'jg')
    functions, cases = [], []
    for width, dst, src in ((8, 'al', 'cl'), (16, 'ax', 'cx'), (32, 'eax', 'ecx')):
        mask, top = (1 << width) - 1, 1 << (width - 1)
        values = sorted({0, 1, 2, 15, 16, top - 1, top, top + 1, mask - 1, mask})
        for mnemonic in ('cmp', 'test', 'add', 'sub', 'and', 'or', 'xor', 'neg', 'adc', 'sbb'):
            ops = [Operand(type='reg', reg=dst)]
            if mnemonic != 'neg':
                ops.append(Operand(type='reg', reg=src))
            instruction = Instruction(0x10000, 2, mnemonic, '', '')
            instruction.operands = ops
            lifter = Lifter(); lifter.needs_cf = True
            body = '\n'.join(lifter.lift_instruction(instruction))
            expressions = []
            for index, condition in enumerate(conditions):
                expression = _make_condition(condition, mnemonic, ops)
                assert expression is not None, (mnemonic, width, condition)
                expressions.append(f'(({expression[0]}) ? {1 << index}u : 0u)')
            name = f'flags_{mnemonic}_{width}'
            functions.append(f'''static unsigned {name}(uint32_t a,uint32_t b,int carry) {{
    uint32_t eax=a,ecx=b,_fa=0,_fb=0; int32_t _fas=0,_fbs=0; int _cf=carry;
    {body}
    eax=ecx=0x12345678; /* Consumers must use the captured operands/results. */
    return {' | '.join(expressions)};
}}''')
            for a in values:
                for b in (values if mnemonic != 'neg' else [0]):
                    for carry in ((0, 1) if mnemonic in ('adc', 'sbb') else (0,)):
                        signed_a = a if a < top else a - (1 << width)
                        signed_b = b if b < top else b - (1 << width)
                        if mnemonic in ('cmp', 'sub', 'sbb'):
                            wide = signed_a - signed_b - carry
                            result = (a - b - carry) & mask
                        elif mnemonic in ('add', 'adc'):
                            wide = signed_a + signed_b + carry
                            result = (a + b + carry) & mask
                        elif mnemonic == 'neg':
                            wide = -signed_a; result = -a & mask
                        else:
                            result = {'test': a & b, 'and': a & b, 'or': a | b, 'xor': a ^ b}[mnemonic]
                            wide = result if result < top else result - (1 << width)
                        overflow = not -top <= wide < top
                        parity = (result & 255).bit_count() % 2 == 0
                        less = bool(result & top) != overflow
                        zero = result == 0
                        flags = (overflow, not overflow, parity, not parity,
                                 less, not less, less or zero, not less and not zero)
                        expected = sum(int(value) << bit for bit, value in enumerate(flags))
                        cases.append(f'if({name}(0x{a:X}u,0x{b:X}u,{carry})!={expected}u) return {len(cases) + 1};')
    prelude = '''#include <stdint.h>
#define LO8(v) ((v)&255u)
#define HI8(v) (((v)>>8)&255u)
#define LO16(v) ((v)&65535u)
#define SET_LO8(v,x) ((v)=((v)&0xffffff00u)|((x)&255u))
#define SET_LO16(v,x) ((v)=((v)&0xffff0000u)|((x)&65535u))
#define CMP_L(a,b) ((int32_t)(a)<(int32_t)(b))
#define CMP_GE(a,b) ((int32_t)(a)>=(int32_t)(b))
#define CMP_LE(a,b) ((int32_t)(a)<=(int32_t)(b))
#define CMP_G(a,b) ((int32_t)(a)>(int32_t)(b))
#define TEST_S(a,b) (((int32_t)((a)&(b)))<0)
static unsigned parity8(uint32_t x) { unsigned n=0;for(unsigned i=0;i<8;i++)n+=(x>>i)&1;return !(n&1); }
#define RECOMP_PARITY8(x) parity8(x)
'''
    result = _build_and_run(prelude + '\n'.join(functions) + '\nint main(void) {\n' + '\n'.join(cases) + '\nreturn 0; }')
    assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
