"""Execute CRT x87 save/call/restore semantics and architectural state images."""
from pathlib import Path
from .disasm import Instruction,Operand
from .lifter import Lifter
from .translator import FP_STACK_MACROS
from .test_lifter_pushal import _build_and_run
HEADER=Path(__file__).resolve().parents[4]/'config/recomp_types.h'
PRELUDE=f'#include "{HEADER.as_posix()}"\n'+r'''
RECOMP_TLS double g_fp_stack[8];
RECOMP_TLS int g_fp_top,g_fp_cmp;
RECOMP_TLS uint16_t g_fp_control_word,g_fp_cc,g_fp_status_word,g_fp_tag_word;
RECOMP_TLS uint16_t g_fp_opcode,g_fp_code_selector,g_fp_data_selector;
RECOMP_TLS uint32_t g_fp_instruction_pointer,g_fp_data_pointer,g_mxcsr;
RECOMP_TLS uint8_t g_fp_raw[8][10];
RECOMP_TLS uint8_t g_fp_alias_dirty;
RECOMP_TLS RecompMmx g_mm0,g_mm1,g_mm2,g_mm3,g_mm4,g_mm5,g_mm6,g_mm7;
RECOMP_TLS RecompXmm g_xmm0,g_xmm1,g_xmm2,g_xmm3,g_xmm4,g_xmm5,g_xmm6,g_xmm7;
ptrdiff_t g_xbox_mem_offset;
'''
def emit(m,raw=''):
    insn=Instruction(0x239c19,len(bytes.fromhex(raw)) or 2,m,'[ecx + 8]',raw)
    insn.operands=[Operand(type='mem',mem_base='ecx',mem_disp=8,mem_size=4)]
    lifter=Lifter();lifter.architectural_flags=True
    return '\n'.join(lifter.lift_instruction(insn))

def test_trigonometric_c2_range_and_crt_remainder_completion():
    source=PRELUDE+'RECOMP_TLS uint32_t g_eax,g_eflags;\n#define eax g_eax\n'+'\n'.join(FP_STACK_MACROS)+'\n'
    for mnemonic in ('fsin','fcos','fsincos','fptan','fprem','fprem1'):
        instruction=Instruction(0x23ac9b,2,mnemonic,'','')
        source+='static void run_'+mnemonic+'(void) {\n'+'\n'.join(Lifter().lift_instruction(instruction))+'\n}\n'
    lifter=Lifter();lifter.architectural_flags=True
    status=Instruction(0x23ac9d,2,'fnstsw','ax','dfe0')
    status.operands=[Operand(type='reg',reg='ax')]
    for mnemonic,instruction in [('fnstsw',status),('sahf',Instruction(0x23ac9f,1,'sahf','','9e'))]:
        source+='static void run_'+mnemonic+'(void) {\n'+'\n'.join(lifter.lift_instruction(instruction))+'\n}\n'
    source+=r'''
int main(void) {
 void (*trig[])(void)={run_fsin,run_fcos,run_fsincos,run_fptan};
 const double inputs[]={0.0,-0.0,0.7853981633974483,-0.7853981633974483,100.0};
 const uint16_t previous[]={0,0x600,0x4500,0x4700};
 for(unsigned op=0;op<4;op++) for(unsigned i=0;i<5;i++) for(unsigned c=0;c<4;c++) {
  recomp_fp_reset();g_fp_top=6;g_fp_control_word=0x27f;
  fp_top()=inputs[i];g_fp_cc=previous[c];trig[op]();
  if(g_fp_cc&0x400u)return 1;
  if(g_fp_top!=(op<2?6:5))return 2;
  double sine=sin(inputs[i]),cosine=cos(inputs[i]);
  if(op==0 && fp_top()!=sine)return 3;
  if(op==1 && fp_top()!=cosine)return 4;
  if(op==2 && (fp_top()!=cosine || fp_st1()!=sine))return 5;
  if(op==3 && (fp_top()!=1.0 || fp_st1()!=tan(inputs[i])))return 6;
 }
 const double large[]={0x1p63,-0x1p63,0x1p100,-0x1p100};
 for(unsigned op=0;op<4;op++) for(unsigned i=0;i<4;i++) {
  unsigned char raw[10];recomp_fp_reset();g_fp_top=7;
  recomp_fp80_store(raw,large[i]);raw[0]|=1u;
  recomp_fp_push_raw(&g_fp_top,raw);g_fp_cc=0;
  unsigned top=g_fp_top;double value=fp_top();uint16_t tag=g_fp_tag_word;
  uint8_t dirty=g_fp_alias_dirty;
  trig[op]();
  if(!(g_fp_cc&0x400u)||g_fp_top!=top||fp_top()!=value)return 7;
  if(g_fp_alias_dirty!=dirty||g_fp_tag_word!=tag||memcmp(g_fp_raw[top],raw,10))return 8;
 }
 /* Captured battle state at the CRT's FPREM1/FNSTSW/SAHF/JP back edge. */
 void (*reduction[])(void)={run_fprem,run_fprem1};
 for(unsigned op=0;op<2;op++) {
  recomp_fp_reset();g_fp_top=6;g_fp_control_word=0x27f;
  fp_top()=-0.7071067769704655;fp_st1()=1.4488038916154245e19;g_fp_cc=0x600;
  unsigned iterations;
  for(iterations=0;iterations<4;iterations++) {
   reduction[op]();
   run_fnstsw();run_sahf();
   if(!(g_eflags&0x4u))break;
  }
  if(iterations!=0||g_fp_top!=6||fp_top()!=-0.7071067769704655)return 9;
 }
 return 0;
}
'''
    run=_build_and_run(source);assert run.returncode==0,('TRIG_C2_EXIT',run.returncode,run.stdout,run.stderr)

def test_original_crt_save_call_restore_and_image_layout():
    source=PRELUDE+'\n'.join(FP_STACK_MACROS)+r'''
int main(void) {
 unsigned char memory[256];uint32_t ecx=1;g_xbox_mem_offset=(ptrdiff_t)memory;
 recomp_fp_reset();g_fp_control_word=0x127f;g_fp_top=5;
 g_fp_cc=0x4500;g_fp_status_word=0x82a1;g_fp_tag_word=0xffff;
 fp_push(1.0);fp_push(-0.0);fp_push(3.25);
 g_fp_opcode=0x6db;g_fp_instruction_pointer=0x12345678;g_fp_data_pointer=0x87654321;
 g_fp_code_selector=8;g_fp_data_selector=0x10;g_mxcsr=0x5f80;g_xmm0.u[0]=0x123abc;
 unsigned top=g_fp_top;uint16_t status=recomp_fp_status();
''' +emit('fnsave','dd7108')+r'''
 unsigned char *image=memory+9;
 if(recomp_fp_get16(image)!=0x127f || recomp_fp_get16(image+4)!=status)return 1;
 if(recomp_fp_get32(image+12)!=0x12345678||recomp_fp_get16(image+18)!=0x6db)return 2;
 if(recomp_fp80_load(image+28)!=3.25 || !signbit(recomp_fp80_load(image+38)))return 3;
 if(g_fp_tag_word!=0xffff||g_fp_top||g_fp_control_word!=0x37f||recomp_fp_status())return 4;
 fp_push(99);g_fp_control_word=0x777;g_fp_cc=0;
''' +emit('frstor','dd6108')+r'''
 if(g_fp_top!=top||fp_top()!=3.25||fp_st(2)!=1.0||!signbit(fp_st(1)))return 5;
 if(g_fp_control_word!=0x127f||recomp_fp_status()!=status||g_fp_cmp!=2)return 6;
 if(g_fp_instruction_pointer!=0x12345678||g_fp_data_pointer!=0x87654321)return 7;
 if(g_xmm0.u[0]!=0x123abc||g_mxcsr!=0x5f80)return 8;
 if(((g_fp_tag_word>>((top+1)*2))&3u)!=1)return 9;
 fp_pop();if(((g_fp_tag_word>>(top*2))&3u)!=3)return 10;
 return 0;
}
'''
    run=_build_and_run(source);assert run.returncode==0,(run.returncode,run.stdout,run.stderr)

def test_extended_context_and_binary80_values():
    source=PRELUDE+r'''
int main(void) {
 unsigned char p[513];RecompFpuState s={0},r={0};unsigned char b[10];
 const uint64_t cases[]={0,0x8000000000000000ULL,0x3ff0000000000000ULL,
 0xbff0000000000000ULL,1,0xfffffffffffffULL,0x10000000000000ULL,
 0x7fefffffffffffffULL,0x7ff0000000000000ULL,0xfff0000000000000ULL,0x7ff8123456789abcULL};
 for(unsigned i=0;i<sizeof cases/sizeof *cases;i++) {
  double v,w;uint64_t actual;memcpy(&v,cases+i,8);recomp_fp80_store(b,v);w=recomp_fp80_load(b);memcpy(&actual,&w,8);
  if(actual!=cases[i])return 1;
 }
 recomp_fp80_store(b,1.0);if(recomp_fp_get16(b+8)!=0x3fff||b[7]!=0x80)return 2;
 s.control=0x27f;s.status=0x2800;s.tag=0xffff;s.opcode=0x123;
 s.instruction_pointer=0xfeedbeef;s.data_pointer=0x12345678;s.code_selector=8;s.data_selector=16;s.mxcsr=0x3f80;
 s.stack[5]=2.5;s.stack[6]=-0.0;s.tag&=~(15u<<10);s.xmm[7][15]=0xab;
 recomp_fp_save_extended(p+1,&s);recomp_fp_load_extended(p+1,&r);
 if(p[5]!=0x60||recomp_fp_get32(p+25)!=0x3f80||recomp_fp_get32(p+29)!=0xffbf)return 3;
 if(r.stack[5]!=2.5||!signbit(r.stack[6])||r.xmm[7][15]!=0xab||r.mxcsr!=s.mxcsr)return 4;
 if(r.instruction_pointer!=s.instruction_pointer||r.data_pointer!=s.data_pointer||r.status!=s.status)return 5;
 if(((r.tag>>12)&3u)!=1 || ((r.tag>>10)&3u)!=0 || (r.tag&3u)!=3)return 6;
 recomp_fp_save_legacy(p,&s,1);recomp_fp_load_legacy(p,&r,1);
 if(r.instruction_pointer!=0xbeef||r.data_pointer!=0x5678||r.stack[5]!=2.5)return 7;
 return 0;
}
'''
    run=_build_and_run(source);assert run.returncode==0,(run.returncode,run.stdout,run.stderr)

def test_environment_control_and_unsupported_are_explicit():
    for m in ('fnsave','frstor','fnstenv','fldenv','fxsave','fxrstor'):
        output=emit(m,'66dd7108')
        assert f'recomp_{m}(' in output
        if m not in ('fxsave','fxrstor'):assert ', 1)' in output
    unknown=Lifter().lift_instruction(Instruction(0x1234,2,'fbstp','','df30'))
    assert any('RECOMP_UNIMPL' in line for line in unknown)
    assert 'recomp_fp_reset()' in '\n'.join(Lifter().lift_instruction(Instruction(0,2,'fninit','','dbe3')))
    assert '~0x80ffu' in '\n'.join(Lifter().lift_instruction(Instruction(0,2,'fnclex','','dbe2')))
def test_mmx_mixed_image_changes_only_destination_register():
    insn=Instruction(0x10000,3,'pxor','mm0, mm0','0fefc0')
    insn.operands=[Operand(type='reg',reg='mm0'),Operand(type='reg',reg='mm0')]
    operation='\n'.join(Lifter().lift_instruction(insn))
    source='#define RECOMP_GENERATED_CODE 1\n'+PRELUDE+r"""
int main(void) {
 unsigned char image[512],saved[512];RecompFpuState state={0};
 state.control=0x37f;state.mxcsr=0x1f80;state.tag=0;
 for(unsigned i=0;i<8;i++) {state.stack[i]=1.25+i;recomp_fp80_store(state.raw[i],state.stack[i]);}
 /* Preserve an extended low significand bit that the double view cannot hold. */
 state.raw[3][0]=1;
 recomp_fp_save_extended(image,&state);recomp_fp_load_extended(image,&state);
 recomp_fp_save_extended(saved,&state);
 if(memcmp(image,saved,512))return 1;
 recomp_fp_restore(&state,1);
"""+operation+r"""
 recomp_fxsave(saved);
 if(recomp_fp_get16(saved+40)!=0xffffu)return 2;
 uint64_t result;memcpy(&result,saved+32,8);if(result!=0)return 3;
 for(unsigned i=1;i<8;i++)if(memcmp(image+32+16*i,saved+32+16*i,10))return 4;
 if(g_fp_tag_word)return 5;
 recomp_fp_restore(&state,1);
"""+operation+r"""
 recomp_fnsave(saved,0);recomp_frstor(saved,0);recomp_fnsave(image,0);
 if(memcmp(image,saved,108))return 6;
"""+'\n'.join(Lifter().lift_instruction(Instruction(0,2,'emms','','0f77')))+r"""
 if(g_fp_tag_word!=0xffff)return 7;
 /* Same-value arithmetic must retire a precise raw80 load, not compare doubles. */
 unsigned char precise[10],canonical[10],observed[10];
 recomp_fp80_store(precise,1.0);precise[0]=1;
 recomp_fp_push_raw(&g_fp_top,precise);unsigned original=g_fp_top;
 recomp_fp_commit(original);
 if(!(g_fp_alias_dirty&(1u<<original))||memcmp(g_fp_raw[original],precise,10))return 8;
 recomp_fp_copy((original+1)&7u,original);recomp_fp_exchange(original,(original+2)&7u);
 /* Reset changes tags, not the pending values; sync must not reclassify tags. */
 recomp_fp_reset();recomp_fp_store_raw(observed,(original+2)&7u);
 recomp_fp80_store(canonical,1.0);
 if(memcmp(observed,canonical,10)||g_fp_tag_word!=0xffff)return 9;
 recomp_mmx_enter();
 if(g_fp_alias_dirty||memcmp(g_fp_raw[(original+1)&7u],canonical,10))return 10;
 /* Many writes to one slot defer every raw80 conversion until this observer. */
 unsigned char retained[10];memcpy(retained,g_fp_raw[original],10);
 for(unsigned i=0;i<256;i++){g_fp_stack[original]=(double)i+0.5;recomp_fp_commit(original);}
 if(memcmp(retained,g_fp_raw[original],10)||g_fp_alias_dirty!=(1u<<original))return 11;
 recomp_fp_store_raw(observed,original);recomp_fp80_store(canonical,255.5);
 if(g_fp_alias_dirty||memcmp(observed,canonical,10))return 12;
 return 0;
}
"""
    run=_build_and_run(source);assert run.returncode==0,(run.returncode,run.stdout,run.stderr)

def test_continuous_mmx_keeps_mixed_raw_images_without_reloading_aliases():
    counted='#define RECOMP_GENERATED_CODE 1\n#include "kernel/recomp_fpu_state.h"\n'+r'''
static unsigned raw_loads;
static double counted_raw_load(const void *p) {++raw_loads;return recomp_fp80_load(p);}
#define recomp_fp80_load counted_raw_load
'''
    source=counted+PRELUDE+'\n'.join(FP_STACK_MACROS)+r'''
static void accepted_enter(void) {
    RecompMmx *mm[]={&g_mm0,&g_mm1,&g_mm2,&g_mm3,&g_mm4,&g_mm5,&g_mm6,&g_mm7};
    for(unsigned i=0;i<8;i++) {
        double previous=recomp_fp80_load(g_fp_raw[i]);uint64_t a,b;
        memcpy(&a,&previous,8);memcpy(&b,&g_fp_stack[i],8);
        if(a!=b)recomp_fp_commit(i);
        memcpy(&mm[i]->q,g_fp_raw[i],8);
    }
    g_fp_tag_word=0;g_fp_top=0;
}
static void calculation(void) {
    g_mm0=MMX_PXOR(g_mm0,g_mm1);recomp_mmx_commit(0);
    g_mm2=MMX_PADDW(g_mm2,g_mm0);recomp_mmx_commit(2);
}
int main(void) {
    RecompFpuState start,expected,actual,other;
    unsigned char left[512],right[512],raw[10];unsigned old_work=0,new_work=0;
    recomp_fp_reset();
    for(unsigned i=0;i<8;i++)fp_push((double)i+1.25);
    recomp_fp80_store(raw,3.25);raw[0]=1;
    recomp_fp_push_raw(&g_fp_top,raw);
    recomp_fp_capture(&other);
    for(unsigned i=0;i<256;i++) {
        if(i%16==0) {
            /* Real eager x87 writes interleaved with a complete context switch. */
            recomp_fp_restore(&other,1);fp_push((double)i-17.5);
            fp_top()=-fp_top();recomp_fp_commit(g_fp_top);
            recomp_fxsave(left);recomp_fxrstor(left);
        }
        memset(&start,0,sizeof start);recomp_fp_capture(&start);
        recomp_fp_restore(&start,1);raw_loads=0;accepted_enter();old_work+=raw_loads;
        calculation();memset(&expected,0,sizeof expected);recomp_fp_capture(&expected);recomp_fxsave(left);
        recomp_fp_restore(&start,1);raw_loads=0;recomp_mmx_enter();new_work+=raw_loads;
        calculation();memset(&actual,0,sizeof actual);recomp_fp_capture(&actual);recomp_fxsave(right);
        if(memcmp(&expected,&actual,sizeof actual)||memcmp(left,right,512))return 1;
        if(g_fp_top||g_fp_tag_word)return 2;
    }
    if(old_work!=256*8||new_work!=0)return 3;
    printf("continuous MMX images equal; enter raw loads old=%u new=%u\n",old_work,new_work);
    return 0;
}
'''
    run=_build_and_run(source);assert run.returncode==0,(run.returncode,run.stdout,run.stderr)
    print(run.stdout)

