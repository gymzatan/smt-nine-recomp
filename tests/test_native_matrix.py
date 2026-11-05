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
    source=tmp_path/'native_completion_test.c'
    text='#define RECOMP_GENERATED_CODE\n#include <assert.h>\n#include <stdio.h>\n'+PRELUDE+'''
    extern __declspec(noalias) uint32_t smt9_cmp32_flags(uint32_t,uint32_t);
    extern __declspec(noalias) uint32_t smt9_cmp8_flags(uint32_t,uint32_t);
    extern __declspec(noalias) uint32_t smt9_cmp16_flags(uint32_t,uint32_t);
    extern __declspec(noalias) uint32_t smt9_cmp8_zero_flags(uint32_t);
    extern __declspec(noalias) uint32_t smt9_cmp32_zero_flags(uint32_t);
    '''
    for width,ops in [('32',['eax','edx']),('16',['ax','dx']),('8',['al','dl'])]:
        text+='\nstatic void case'+width+'(uint32_t a,uint32_t b,uint32_t old){\n uint32_t _fa=0,_fb=0;int32_t _fas=0,_fbs=0;int _cf=0;\n eax=a;edx=b;g_eflags=old;\n'+instruction('cmp',ops)+'''\n uint32_t expected=g_eflags;
     uint32_t actual=(old&~0x8d5u)|(smt9_cmp'''+width+'''_flags(a,b)&0x8d5u);
     assert(actual==expected);assert(eax==a && edx==b);
    }
    '''
    text+='static void case_zero8(uint32_t value,uint32_t old){uint32_t _fa=0,_fb=0;int32_t _fas=0,_fbs=0;int _cf=0;eax=value;edx=0x98e36745u;g_eflags=old;\n_fa = (uint32_t)(LO8(eax)) & 0xFFu; _fb = (uint32_t)(0) & 0xFFu;\n_fas = (int32_t)(int8_t)(_fa); _fbs = (int32_t)(int8_t)(_fb); /* cmp LO8(eax), 0 (8-bit) */\n_cf = (int)(_fa < _fb);\ng_eflags = (g_eflags & ~0x8D5u) | ((CMP_B(_fa, _fb)) ? 0x1u : 0u) | ((RECOMP_PARITY8((_fa) - (_fb))) ? 0x4u : 0u) | ((CMP_EQ(_fa, _fb)) ? 0x40u : 0u) | (((((uint8_t)((uint8_t)(_fas) - (uint8_t)(_fbs)) >> 7) != 0)) ? 0x80u : 0u) | ((((((_fa) ^ (_fb)) & ((_fa) ^ ((_fa - _fb))) & 0x80u) != 0)) ? 0x800u : 0u) | ((_fa ^ _fb ^ (_fa - _fb)) & 0x10u);\nuint32_t expected=g_eflags;uint32_t actual=(old&~0x8D5u)|(smt9_cmp8_zero_flags(value)&0x8D5u);\nassert(actual==expected && _cf==0 && eax==value && edx==0x98e36745u);}\nstatic void case_zero32(uint32_t value,uint32_t old){uint32_t _fa=0,_fb=0;int32_t _fas=0,_fbs=0;int _cf=0;eax=value;edx=0x98e36745u;g_eflags=old;\n_fa = (uint32_t)(eax) & 0xFFFFFFFFu; _fb = (uint32_t)(0) & 0xFFFFFFFFu;\n_fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* cmp eax, 0 (32-bit) */\n_cf = (int)(_fa < _fb);\ng_eflags = (g_eflags & ~0x8D5u) | ((CMP_B(_fa, _fb)) ? 0x1u : 0u) | ((RECOMP_PARITY8((_fa) - (_fb))) ? 0x4u : 0u) | ((CMP_EQ(_fa, _fb)) ? 0x40u : 0u) | (((((uint32_t)((uint32_t)(_fas) - (uint32_t)(_fbs)) >> 31) != 0)) ? 0x80u : 0u) | ((((((_fa) ^ (_fb)) & ((_fa) ^ ((_fa - _fb))) & 0x80000000u) != 0)) ? 0x800u : 0u) | ((_fa ^ _fb ^ (_fa - _fb)) & 0x10u);\nuint32_t expected=g_eflags;uint32_t actual=(old&~0x8D5u)|(smt9_cmp32_zero_flags(value)&0x8D5u);\nassert(actual==expected && _cf==0 && eax==value && edx==0x98e36745u);}\n'
    text+='''
    int main(void){
     const uint32_t edge[]={0,1,2,15,16,127,128,255,256,0x7fffffff,0x80000000,0xfffffffe,0xffffffff};
     const uint32_t flags[]={0u,0xffffffffu,0x202u,0x246fd7u};
     for(unsigned i=0;i<13;i++)for(unsigned j=0;j<13;j++)for(unsigned k=0;k<4;k++)case32(edge[i],edge[j],flags[k]);
     for(unsigned i=0;i<256;i++)for(unsigned j=0;j<256;j++)for(unsigned k=0;k<4;k++)case8(i|0x42670000u,j|0x98e30000u,flags[k]);
     const uint32_t edge16[]={0,1,15,16,127,128,255,256,0x7fff,0x8000,0xfffe,0xffff};
     for(unsigned i=0;i<65536;i++)for(unsigned j=0;j<12;j++)for(unsigned k=0;k<4;k++){
      case16(i|0x42670000u,edge16[j]|0x98e30000u,flags[k]);case16(edge16[j]|0x42670000u,i|0x98e30000u,flags[k]);
     }
     const uint32_t high[]={0u,0x7fff0000u,0x80000000u,0xffff0000u};
     for(unsigned i=0;i<256;i++)for(unsigned f=0;f<4;f++)case_zero8(i|0x42670000u,flags[f]);
     for(unsigned i=0;i<13;i++)for(unsigned f=0;f<4;f++)case_zero32(edge[i],flags[f]);
     for(unsigned i=0;i<65536;i++)for(unsigned h=0;h<4;h++)for(unsigned f=0;f<4;f++)case_zero32(i|high[h],flags[f]);
     puts("ORIGINAL_LIFTER_FLAGS_EQUAL cmp32=676 cmp16=6291456 cmp8=262144 zero8=1024 zero32=1048628 upper_byte_arguments=retained non_arithmetic_flags=retained");return 0;
    }
    '''
    source.write_text(text,encoding='utf8')
    subprocess.run([assembler,'/nologo','/c','/Fo'+str(obj),str(ROOT/'game/src/native_matrix.asm')],
                   cwd=tmp_path,check=True,capture_output=True)
    subprocess.run([compiler,'/nologo','/O2','/MD','/UNDEBUG','/I'+str(ROOT/'vendor/xboxrecomp/src'),
                   str(source),str(obj),'/Fe:'+str(exe)],cwd=tmp_path,check=True,capture_output=True)
    subprocess.run([str(exe)],cwd=tmp_path,check=True,capture_output=True)


def test_native_point_matches_original_ordered_block(tmp_path):
    import json,re
    data=json.loads((ROOT/'config/codegen.json').read_text(encoding='utf-8'))
    row=next(q for q in data['functions'] if q['name']=='sub_002201A1')
    edit=next(e for e in row['edits'] if 'smt9_point_rows' in e[2])
    # The project edit retains the original emitted block as its reference.
    # Use the existing vector record declarations and reader, without another
    # point-transform formula or using matrix expected outputs for this routine.
    original=(ROOT/'game/tests/native_matrix_test.c').read_text(encoding='utf-8')
    declarations='\n'.join(re.findall(r'^typedef struct (?:Input|Expected|Record) .*$',original,re.M))
    assert declarations.count('typedef struct ')==3
    text=r'''
#define RECOMP_GENERATED_CODE
#include "recomp_types.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
RECOMP_TLS RecompGprState g_gpr;
RECOMP_TLS RecompXmm g_xmm0,g_xmm1,g_xmm2,g_xmm3,g_xmm4,g_xmm5,g_xmm6,g_xmm7;
ptrdiff_t g_xbox_mem_offset;
static unsigned char buffer[2080],reference[2048];
static RecompXmm *registers[8];
'''+declarations+'\nstatic void before_point_block(void){uint32_t ebp=0x100;\n'+edit[1]+'\n}\nstatic void after_point_block(void){uint32_t ebp=0x100;\n'+edit[2]+r'''
}
static void initialize(const Record *record,unsigned mode,unsigned alignment,uint32_t csr){
    unsigned char *ram=buffer+alignment;
    memset(ram,0xa5,2048);g_xbox_mem_offset=(ptrdiff_t)ram;memset(&g_gpr,0x5a,sizeof g_gpr);
    for(unsigned j=0;j<8;j++)memcpy(registers[j],record->input.xmm[j],16);
    memcpy(ram+0x200,record->input.matrix[0],64);memcpy(ram+0x300,record->input.matrix[1],64);
    static const uint32_t targets[]={0x400,0x200,0x300,0x31c,0x304,0x400};
    MEM32(0x108)=targets[mode];MEM32(0x10c)=mode==5?0x300:0x200;MEM32(0x110)=0x300;
    _mm_setcsr(csr);
}
int main(int argc,char **argv){
    RecompXmm *current[]={&g_xmm0,&g_xmm1,&g_xmm2,&g_xmm3,&g_xmm4,&g_xmm5,&g_xmm6,&g_xmm7};
    memcpy(registers,current,sizeof registers);
    assert(argc==2 && sizeof(Record)==616);FILE *file=fopen(argv[1],"rb");assert(file);
    Record record;unsigned records=0,cases=0;
    while(fread(&record,sizeof record,1,file)==1){
        for(unsigned mode=0;mode<6;mode++)for(unsigned control=0;control<8;control++){
            unsigned alignment=records&15;uint32_t csr=0x1f80|((control&3)<<13)|((control&4)?0x8040:0);
            initialize(&record,mode,alignment,csr);before_point_block();
            RecompGprState before=g_gpr;RecompXmm xmm[8];
            for(unsigned j=0;j<8;j++)xmm[j]=*registers[j];
            uint32_t flags=_mm_getcsr();memcpy(reference,buffer+alignment,sizeof reference);
            initialize(&record,mode,alignment,csr);after_point_block();
            assert(!memcmp(&before,&g_gpr,sizeof before) && flags==_mm_getcsr());
            for(unsigned j=0;j<8;j++)assert(!memcmp(xmm+j,registers[j],16));
            assert(!memcmp(reference,buffer+alignment,sizeof reference));cases++;
        }records++;
    }
    assert(feof(file) && !ferror(file) && records==264 && cases==12672);fclose(file);
    _mm_setcsr(0x1f80);return 0;
}
'''
    source=tmp_path/'native_point_test.c';source.write_text(text,encoding='utf-8')
    compiler=shutil.which('cl');assembler=shutil.which('ml64');assert compiler and assembler
    obj=tmp_path/'native_matrix.obj';exe=tmp_path/'native_point_test.exe'
    subprocess.run([assembler,'/nologo','/c','/Fo'+str(obj),str(ROOT/'game/src/native_matrix.asm')],
                   cwd=tmp_path,check=True,capture_output=True)
    subprocess.run([compiler,'/nologo','/O2','/MD','/UNDEBUG','/D_CRT_SECURE_NO_WARNINGS',
                   '/I'+str(ROOT/'config'),'/I'+str(ROOT/'vendor/xboxrecomp/src'),str(source),str(obj),'/Fe:'+str(exe)],
                   cwd=tmp_path,check=True,capture_output=True)
    subprocess.run([str(exe),str(ROOT/'game/tests/data/native_matrix_vectors.bin')],
                   cwd=tmp_path,check=True,capture_output=True)


def test_native_lerp_matches_original_ordered_block(tmp_path):
    import json,re
    data=json.loads((ROOT/'config/codegen.json').read_text(encoding='utf-8'))
    row=next(q for q in data['functions'] if q['name']=='sub_0021FA30')
    edit=next(e for e in row['edits'] if 'smt9_lerp_rows' in e[2])
    # Consume the retained emitted reference through its existing identity map.
    reference=re.sub(r'\b_leaf_(xmm[012]|eax|esp)\b',r'\1',edit[1])
    assert '_leaf_' not in reference
    original=(ROOT/'game/tests/native_matrix_test.c').read_text(encoding='utf-8')
    declarations='\n'.join(re.findall(r'^typedef struct (?:Input|Expected|Record) .*$',original,re.M))
    assert declarations.count('typedef struct ')==3
    text=r'''
#define RECOMP_GENERATED_CODE
#include "recomp_types.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
RECOMP_TLS RecompGprState g_gpr;
RECOMP_TLS RecompXmm g_xmm0,g_xmm1,g_xmm2,g_xmm3,g_xmm4,g_xmm5,g_xmm6,g_xmm7;
ptrdiff_t g_xbox_mem_offset;
static unsigned char buffer[2080],expected_ram[2048];
static RecompXmm *registers[8];
'''+declarations+'\nstatic void before_lerp_block(void){\n'+reference+'\n}\nstatic void after_lerp_block(void){\n'+edit[2]+r'''
}
static void initialize(const Record *record,unsigned alignment,uint32_t csr){
    unsigned char *ram=buffer+alignment;
    memset(ram,0xa5,2048);g_xbox_mem_offset=(ptrdiff_t)ram;memset(&g_gpr,0x5a,sizeof g_gpr);
    for(unsigned j=0;j<8;j++)memcpy(registers[j],record->input.xmm[j],16);
    g_esp=0x100;MEM32(0x104)=record->input.xmm[3][0];MEM32(0x110)=record->input.xmm[0][0];
    _mm_setcsr(csr);
}
int main(int argc,char **argv){
    RecompXmm *current[]={&g_xmm0,&g_xmm1,&g_xmm2,&g_xmm3,&g_xmm4,&g_xmm5,&g_xmm6,&g_xmm7};
    memcpy(registers,current,sizeof registers);
    assert(argc==2 && sizeof(Record)==616);FILE *file=fopen(argv[1],"rb");assert(file);
    Record record;unsigned records=0,cases=0;
    while(fread(&record,sizeof record,1,file)==1){
        for(unsigned control=0;control<8;control++){
            unsigned alignment=records&15;uint32_t csr=0x1f80|((control&3)<<13)|((control&4)?0x8040:0);
            initialize(&record,alignment,csr);before_lerp_block();
            RecompGprState before=g_gpr;RecompXmm xmm[8];
            for(unsigned j=0;j<8;j++)xmm[j]=*registers[j];
            uint32_t flags=_mm_getcsr();memcpy(expected_ram,buffer+alignment,sizeof expected_ram);
            initialize(&record,alignment,csr);after_lerp_block();
            assert(!memcmp(&before,&g_gpr,sizeof before) && flags==_mm_getcsr());
            for(unsigned j=0;j<8;j++)assert(!memcmp(xmm+j,registers[j],16));
            assert(!memcmp(expected_ram,buffer+alignment,sizeof expected_ram));cases++;
        }records++;
    }
    assert(feof(file) && !ferror(file) && records==264 && cases==2112);fclose(file);
    _mm_setcsr(0x1f80);return 0;
}
'''
    source=tmp_path/'native_lerp_test.c';source.write_text(text,encoding='utf-8')
    compiler=shutil.which('cl');assembler=shutil.which('ml64');assert compiler and assembler
    obj=tmp_path/'native_matrix.obj';exe=tmp_path/'native_lerp_test.exe'
    subprocess.run([assembler,'/nologo','/c','/Fo'+str(obj),str(ROOT/'game/src/native_matrix.asm')],
                   cwd=tmp_path,check=True,capture_output=True)
    subprocess.run([compiler,'/nologo','/O2','/MD','/UNDEBUG','/D_CRT_SECURE_NO_WARNINGS',
                   '/I'+str(ROOT/'config'),'/I'+str(ROOT/'vendor/xboxrecomp/src'),str(source),str(obj),'/Fe:'+str(exe)],
                   cwd=tmp_path,check=True,capture_output=True)
    subprocess.run([str(exe),str(ROOT/'game/tests/data/native_matrix_vectors.bin')],
                   cwd=tmp_path,check=True,capture_output=True)
