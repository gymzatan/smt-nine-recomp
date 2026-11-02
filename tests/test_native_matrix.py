"""Check native matrix arithmetic and transpose bits, including aliases."""
from pathlib import Path
import shutil,subprocess

ROOT=Path(__file__).resolve().parents[1]

def test_native_matrix_matches_original_cpu(tmp_path):
    compiler=shutil.which('cl');assembler=shutil.which('ml64')
    assert compiler and assembler,'requires the existing x64 MSVC environment'
    obj=tmp_path/'native_matrix.obj';exe=tmp_path/'native_matrix_test.exe'
    subprocess.run([assembler,'/nologo','/c','/Fo'+str(obj),str(ROOT/'game/src/native_matrix.asm')],
                   cwd=tmp_path,check=True,capture_output=True)
    subprocess.run([compiler,'/nologo','/O2','/D_CRT_SECURE_NO_WARNINGS',
                   str(ROOT/'game/tests/native_matrix_test.c'),str(obj),'/Fe:'+str(exe)],
                   cwd=tmp_path,check=True,capture_output=True)
    subprocess.run([str(exe),str(ROOT/'game/tests/data/native_matrix_vectors.bin')],
                   cwd=tmp_path,check=True,capture_output=True)


def test_native_completion_cmp_matches_original_lifter(tmp_path):
    import sys
    sys.path.insert(0,str(ROOT/'vendor/xboxrecomp'))
    from tools.recomp.test_lifter_architectural_flags import PRELUDE, instruction
    compiler=shutil.which('cl');assembler=shutil.which('ml64')
    assert compiler and assembler,'requires the existing x64 MSVC environment'
    obj=tmp_path/'native_matrix.obj';exe=tmp_path/'native_completion_test.exe'
    emitted=instruction('cmp',['eax','edx'])
    source=tmp_path/'native_completion_test.c'
    source.write_text("#define RECOMP_GENERATED_CODE\n#include <assert.h>\n"+PRELUDE+"\nextern uint32_t smt9_cmp32_flags(uint32_t,uint32_t);\n"+r"""
int main(void){
 const uint32_t edge[]={0,1,2,15,16,127,128,255,256,0x7fffffff,0x80000000,0xfffffffe,0xffffffff};
 for(unsigned i=0;i<13;i++)for(unsigned j=0;j<13;j++)for(unsigned k=0;k<4;k++){
  uint32_t _fa=0,_fb=0;int32_t _fas=0,_fbs=0;int _cf=0;
  uint32_t old=k&1?0xffffffffu:0x246fd7u;eax=edge[i];edx=edge[j];g_eflags=old;
"""+emitted+r"""
  uint32_t expected=g_eflags;
  uint32_t actual=(old&~0x8d5u)|(smt9_cmp32_flags(edge[i],edge[j])&0x8d5u);
  assert(actual==expected);
 }
 return 0;
}
""",encoding='utf8')
    subprocess.run([assembler,'/nologo','/c','/Fo'+str(obj),str(ROOT/'game/src/native_matrix.asm')],
                   cwd=tmp_path,check=True,capture_output=True)
    subprocess.run([compiler,'/nologo','/O2','/UNDEBUG','/I'+str(ROOT/'vendor/xboxrecomp/src'),
                   str(source),str(obj),'/Fe:'+str(exe)],cwd=tmp_path,check=True,capture_output=True)
    subprocess.run([str(exe)],cwd=tmp_path,check=True,capture_output=True)
