"""The original CPU-feature instruction sees the console, independent of host."""
from pathlib import Path
from .disasm import Instruction
from .lifter import Lifter, _EFLAGS_PRESERVE
from .test_lifter_pushal import _build_and_run, translate


def test_cpuid_leaves_and_preserved_registers():
    header=Path(__file__).resolve().parents[4]/'config/recomp_types.h'
    code='\n'.join(Lifter().lift_instruction(Instruction(0x10000,2,'cpuid','','0fa2')))
    source=f'''#include "{header.as_posix()}"
static uint32_t eax,ebx,ecx,edx,esi,edi,ebp,esp;
static void execute(void) {{{code}}}
int main(void) {{
    const uint32_t inputs[]={{0,1,2,3,0x80000000u,0xffffffffu}};
    for(unsigned i=0;i<6;i++) {{
        eax=inputs[i];ebx=ecx=edx=0xdeadbeef;esi=1;edi=2;ebp=3;esp=4;
        execute();
        if(esi!=1 || edi!=2 || ebp!=3 || esp!=4)return 1;
        if(i==0) {{
            char vendor[13]={{0}};memcpy(vendor,&ebx,4);memcpy(vendor+4,&edx,4);memcpy(vendor+8,&ecx,4);
            if(eax!=2 || strcmp(vendor,"GenuineIntel"))return 2;
        }} else if(i==1) {{
            if(eax!=0x68a || ebx || ecx || edx!=0x0383fbffu)return 3;
            if(!(edx&(1u<<23)) || !(edx&(1u<<25)) || (edx&(1u<<26)))return 4;
        }} else if(eax!=0x03020101u || ebx || ecx || edx!=0x0c040841u)return 5;
    }}
    return 0;
}}'''
    result=_build_and_run(source)
    assert result.returncode==0,result.stdout+result.stderr


def test_cpuid_keeps_the_preceding_compare():
    code=translate('39d0 0fa2 7401 c3 c3')
    assert 'recomp_cpuid(eax, &eax, &ebx, &ecx, &edx)' in code
    assert 'CMP_EQ(_fa, _fb)' in code
    assert 'cpuid' in _EFLAGS_PRESERVE
    assert 'UNIMPL' not in code
