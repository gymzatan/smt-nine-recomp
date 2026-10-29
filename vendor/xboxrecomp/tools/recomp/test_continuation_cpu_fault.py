"""Saved CPU-fault calls retain inputs across a waiting native exception callback."""
from pathlib import Path
import os
import shutil
import subprocess
import pytest
from . import config
from .translator import FunctionTranslator

ROOT=Path(__file__).resolve().parents[4]

@pytest.fixture(scope='module',params=[('div',False),('div',True),('idiv',False),('idiv',True)])
def fault_binary(tmp_path_factory,request):
    operation,skip=request.param
    program={0x11000:bytes.fromhex('5589e5ff0500000200b81100000031d231c9'+('f7f1' if operation=='div' else 'f7f9')+'ff050c000200c9c3'),
             0x12000:bytes.fromhex('ff0504000200b904000000ff05080002004975f7b903000000c3')}
    monkeypatch=pytest.MonkeyPatch()
    monkeypatch.setattr(config,'_SECTIONS',[config.Section('.text',0x11000,0x2000,0,0x2000,True)])
    image=bytearray(0x2000);functions={}
    for address,raw in program.items():
        image[address-0x11000:address-0x11000+len(raw)]=raw
        functions[address]={'start':hex(address),'end':address+len(raw),'size':len(raw),'name':f'sub_{address:08X}'}
    translator=FunctionTranslator(bytes(image),functions,seh_prolog=0,seh_epilog=0,continuations=True)
    try:generated='\n'.join(translator.translate_function(address,row) for address,row in functions.items())
    finally:monkeypatch.undo()
    assert 'case 0x100011012ULL: goto cont_call_00011012;' in generated
    assert '"_cont_fault_ebp_00011012"' in generated
    prefix=f'#define TEST_SKIP {int(skip)}\n#include "{(ROOT/"config/recomp_types.h").as_posix()}"\n'+r'''
#include "kernel/recomp_continuation.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
void sub_00011000(void);void sub_00012000(void);
ptrdiff_t g_xbox_mem_offset;
RECOMP_TLS RecompGprState g_gpr={0,0,0,0,0,0,0,0x202u,0};
RECOMP_TLS uint32_t g_ebp,g_seh_ebp;

#define eax g_eax
#define ecx g_ecx
#define edx g_edx
#define esp g_esp
#define esi g_esi
#define edi g_edi
#define ebx g_ebx
static unsigned char *ram;
static const char *snapshot;
static int saving,transfer;
static void write_arch(FILE *file) {
 uint32_t registers[]={g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_ebp,g_seh_ebp,g_eflags};
 assert(fwrite(registers,sizeof registers,1,file)==1);assert(fwrite(ram,0x100000,1,file)==1);
}
static void read_arch(FILE *file) {
 uint32_t r[10];assert(fread(r,sizeof r,1,file)==1);
 g_eax=r[0];g_ecx=r[1];g_edx=r[2];g_esp=r[3];g_ebx=r[4];g_esi=r[5];g_edi=r[6];g_ebp=r[7];g_seh_ebp=r[8];g_eflags=r[9];
 assert(fread(ram,0x100000,1,file)==1);
}
void guest_cpu_yield(void) {
 g_ebp=0xdeadbeef;
 if(saving && MEM32(0x20008)==2) {
  FILE *file=fopen(snapshot,"wb");assert(file);write_arch(file);
  assert(recomp_cont_snapshot_ready() && recomp_cont_save(file));assert(fclose(file)==0);
  if(saving==1)exit(23);saving=0;
 }
}
uint32_t xbox_CpuException(uint32_t code,uint32_t address,uint32_t frame_ebp) {
 /* Reentry must use original inputs, although the waiting handler has changed EBP. */
 assert(code==0xc0000094u && address==0x11012 && frame_ebp==0x7fff8);
 uint32_t phase=0,saved_code=code,saved_address=address,saved_frame=frame_ebp;
 RecompContLocal locals[]={{"phase",&phase,4},{"code",&saved_code,4},{"address",&saved_address,4},{"frame",&saved_frame,4}};
 RecompContFrame frame;uint64_t site=recomp_cont_enter(&frame,0xfe000101u,locals,4);
 assert(!site || site==0x6fe000101ULL);
 if(!phase) {
  MEM32(0x20010)++;PUSH32(g_esp,0xabcdef00);phase=1;
 }
 recomp_cont_site(&frame,0x6fe000101ULL);
 if(phase==1) {sub_00012000();phase=2;}
 assert(saved_code==code && saved_address==address && saved_frame==frame_ebp);
 MEM32(0x20014)++;g_ebp=saved_frame;
 recomp_cont_leave(&frame);return transfer?0x13000:saved_address+(TEST_SKIP?2:0);
}
void xbox_ExceptionContinue(uint32_t target,uint32_t frame) {
 assert(transfer && target==0x13000 && frame==0x7fff8);
 assert(recomp_cont_current()->function==0x11000 && recomp_cont_current()->site==0x100011012ULL);
 assert(MEM32(0x20000)==1 && MEM32(0x20004)==1 && MEM32(0x20008)==4);
 assert(MEM32(0x2000c)==0 && MEM32(0x20010)==1 && MEM32(0x20014)==1);
 puts("external fault transfer boundary");exit(42);
}
'''
    suffix=r'''
static void check(void) {
 assert(MEM32(0x20000)==1 && MEM32(0x20004)==1 && MEM32(0x20008)==4);
 assert(MEM32(0x2000c)==1 && MEM32(0x20010)==1 && MEM32(0x20014)==1);
 assert(g_eax==(TEST_SKIP?17:5) && g_edx==(TEST_SKIP?0:2) && g_esp==0x80000);
 assert(!recomp_cont_active());
}
static void load(void) {
 FILE *file=fopen(snapshot,"rb");assert(file);read_arch(file);assert(recomp_cont_load(file));assert(fclose(file)==0);
}
int main(int argc,char **argv) {
 assert(argc==3);transfer=getenv("TEST_CPU_TRANSFER")!=NULL;snapshot=argv[2];ram=calloc(1,0x100000);assert(ram);g_xbox_mem_offset=(ptrdiff_t)ram;
 if(!strcmp(argv[1],"load"))load();
 else {g_esp=0x80000;PUSH32(g_esp,0x76543210);g_ebp=0x12345678;}
 saving=!strcmp(argv[1],"save")?1:!strcmp(argv[1],"roundtrip")?2:0;
 sub_00011000();check();
 if(!strcmp(argv[1],"roundtrip")) {load();sub_00011000();check();}
 puts("fault callback resumed once");free(ram);return 0;
}
'''
    work=tmp_path_factory.mktemp('cpu-fault-'+operation+str(int(skip)))
    source=work/'fixture.c';source.write_text(prefix+generated+suffix)
    compiler=shutil.which('cl');assert compiler,'requires existing MSVC environment'
    built=subprocess.run([compiler,'/nologo','/O2','/utf-8','/D_CRT_SECURE_NO_WARNINGS','/I'+str(ROOT/'vendor/xboxrecomp/src'),str(source),str(ROOT/'vendor/xboxrecomp/src/kernel/recomp_continuation.c'),'/Fe:'+str(work/'fixture.exe')],cwd=work,capture_output=True,text=True)
    assert built.returncode==0,built.stdout+built.stderr
    return work/'fixture.exe'

@pytest.mark.parametrize('resume',['load','roundtrip'])
def test_cpu_exception_callback_wait_restores_saved_call(fault_binary,tmp_path,resume):
    snapshot=tmp_path/'cpu.state'
    original=subprocess.run([str(fault_binary),'run',str(snapshot)],capture_output=True,text=True)
    assert original.returncode==0,original.stdout+original.stderr
    if resume=='load':
        saved=subprocess.run([str(fault_binary),'save',str(snapshot)],capture_output=True,text=True)
        assert saved.returncode==23,saved.stdout+saved.stderr
    restored=subprocess.run([str(fault_binary),resume,str(snapshot)],capture_output=True,text=True)
    assert restored.returncode==0,restored.stdout+restored.stderr
    assert restored.stdout==original.stdout


def test_external_cpu_exception_target_retains_transfer_boundary(fault_binary,tmp_path):
    snapshot=tmp_path/'external-cpu.state'
    environment=dict(os.environ,TEST_CPU_TRANSFER='1')
    saved=subprocess.run([str(fault_binary),'save',str(snapshot)],env=environment,capture_output=True,text=True)
    assert saved.returncode==23,saved.stdout+saved.stderr
    restored=subprocess.run([str(fault_binary),'load',str(snapshot)],env=environment,capture_output=True,text=True)
    assert restored.returncode==42,restored.stdout+restored.stderr
    assert restored.stdout.strip()=='external fault transfer boundary'
