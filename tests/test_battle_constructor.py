"""Check the actual effect constructor validates callback outputs, not stale heap bytes."""
from pathlib import Path
import importlib.util,json,shutil,subprocess,sys
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import generate

def test_effect_constructor_uses_completed_callback_handles(tmp_path):
    base=(ROOT/'game/tests/data/battle_constructor_base.c').read_text()
    row=next(r for r in json.loads((ROOT/'config/codegen.json').read_text())['functions'] if r['name']=='sub_00086D7C')
    offset=7070
    edits=[(at-offset,removed,inserted,before,after) for at,removed,inserted,before,after in row['edits'] if offset <= at < offset+len(base)]
    assert len(edits)==5
    fixed=generate.apply_project_edits(base,dict(row,edits=edits))
    compiler=shutil.which('cl');assert compiler
    fixture=r"""
#include <stdint.h>
#include <string.h>
static unsigned char ram[0x6000];
#define MEM32(a) (*(uint32_t *)(ram+(uint32_t)(a)))
#define PUSH32(s,v) do { (s)-=4;MEM32(s)=(v); } while(0)
#define CMP_GE(a,b) ((int32_t)(a)>=(int32_t)(b))
#define CMP_EQ(a,b) ((a)==(b))
#define CMP_NE(a,b) ((a)!=(b))
static int check(uint32_t callback,uint32_t old0,uint32_t old1,uint32_t out0,uint32_t out1) {
    uint32_t esp=0x4000-20,eax=callback,ebx=0x2000,edi=0,ebp=0x3000,esi=0;
    uint32_t _fa=0,_fb=0;int32_t _fas=0,_fbs=0;int _cf=0;
    memset(ram,0x5a,sizeof ram);MEM32(ebx+0x34)=old0;MEM32(ebx+0x38)=old1;
    MEM32(0x4094)=out0;MEM32(0x4098)=out1;MEM32(ebp+0x14)=0;
    @BLOCK@
loc_00086E89: ;
loc_00086EA1: ;
    if(MEM32(ebx+0x34)!=out0 || MEM32(ebx+0x38)!=out1 || eax!=out1)return -1;
    return 1;
loc_00087264: ;
loc_00087256: ;
    return 0;
}
int main(void) {
    /* Recorded successful callback, with freshly allocated zero object fields. */
    if(check(1,0,0,0x01558b90,0x0155a310)!=1)return 10;
    if(check(1,0,0x847cad7d,0x01558b90,0x0155a310)!=1)return 11;
    if(check(1,0x73777172,0,0x01558b90,0x0155a310)!=1)return 12;
    if(check(1,0x73777172,0x847cad7d,0x01558b90,0x0155a310)!=1)return 13;
    /* Actual callback failures and missing produced handles still reject. */
    if(check(0xffffffff,1,1,0x01558b90,0x0155a310)!=0)return 14;
    if(check(1,1,1,0,0x0155a310)!=0)return 15;
    if(check(1,1,1,0x01558b90,0)!=0)return 16;
    return 0;
}
"""
    for name,body,expected in [('retail',base,10),('fixed',fixed,0)]:
        block=body[body.index('loc_00086E3D: ;'):body.index('loc_00086E89: ;')]
        source=tmp_path/(name+'.c');source.write_text(fixture.replace('@BLOCK@',block))
        exe=tmp_path/(name+'.exe')
        built=subprocess.run([compiler,'/nologo','/O2','/Fe:'+str(exe),str(source)],cwd=tmp_path,capture_output=True,text=True)
        assert built.returncode==0,built.stdout+built.stderr
        assert subprocess.run([str(exe)],cwd=tmp_path,timeout=10).returncode==expected
