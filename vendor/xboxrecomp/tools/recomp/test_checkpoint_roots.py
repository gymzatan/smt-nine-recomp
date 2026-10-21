"""Cold restore original bridge roots after restoring their physical memory."""
from pathlib import Path
import shutil
import struct
import subprocess

import pytest

from tools.recomp.test_continuations import translate


@pytest.fixture(scope='module')
def native_roots(tmp_path_factory):
    root=Path(__file__).resolve().parents[2];work=tmp_path_factory.mktemp('checkpoint_roots')
    compiler=shutil.which('cl');assert compiler, 'requires existing MSVC environment'
    # Initial effect and register seed precede the backedge: neither may replay.
    program={0x11000:bytes.fromhex('5589e58b5508ff02b9df9b5713ff4204a12000020085c074f4' 
                                 '894a0cc9c20800')}
    program[0x12000]=program[0x11000].replace(bytes.fromhex('a120000200'),bytes.fromhex('a124000200'))
    program[0x15000]=program[0x11000]
    args=[0x20030,0,0,0,0,0x11223344,0x55667788,0,0,0x14000]
    pushes=b''.join(b'\x68'+struct.pack('<I',value) for value in reversed(args))
    program.update({
        0x13000:bytes.fromhex('5589e5ff0500000200')+pushes+
                bytes.fromhex('ff1524000200a308000200ff0518000200c9c3'),
        0x14000:bytes.fromhex('5589e5ff0510000200ff0514000200a12000020085c074f1'
                             '8b450833450ca31c000200c9c3'),
    })
    prefix=r'''
#include "kernel/kernel_bridge.c"
#define RECOMP_GENERATED_CODE
#include "recomp_types.h"
#include <assert.h>
void sub_00011000(void);void sub_00012000(void);void sub_00015000(void);void sub_00013000(void);void sub_00014000(void);
recomp_func_t recomp_lookup(uint32_t va) {
    switch(va) { case 0x11000:return sub_00011000;case 0x13000:return sub_00013000;
                 case 0x14000:return sub_00014000;case 0x12000:return sub_00012000;case 0x15000:return sub_00015000;default:return NULL; }
}
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va;return NULL; }
void recomp_icall_fail_log(uint32_t va) { fprintf(stderr,"bad target %08x\n",va);exit(5); }
void recomp_icall_not_code_log(uint32_t va) { recomp_icall_fail_log(va); }
static const char *memory_path,*threads_path;
'''
    suffix=r'''
#undef esp
static void setup_memory(const char *path) {
    FILE *file=fopen(path,"rb");assert(file);fseek(file,0,SEEK_END);
    long bytes=ftell(file);rewind(file);void *xbe=malloc(bytes);assert(xbe);
    assert(fread(xbe,bytes,1,file)==1 && fclose(file)==0);
    assert(xbox_MemoryLayoutInit(xbe,bytes));free(xbe);
}
static void verify_stopped(uint32_t *saved) {
    assert(MEM32(0x20000)==1 && MEM32(0x20010)==1);
    assert(MEM32(0x20004)==saved[0] && MEM32(0x20014)==saved[1]);
}
static void save_files(uint32_t *saved) {
    XboxThreadCreationState creation;xbox_CaptureThreadCreationState(&creation);
    FILE *memory=fopen(memory_path,"wb"),*threads=fopen(threads_path,"wb");assert(memory && threads);
    assert(fwrite(saved,8,1,memory)==1 && fwrite(&creation,sizeof creation,1,memory)==1 && xbox_SavePhysicalMemory(memory));
    assert(xbox_CheckpointWriteThreads(threads));fclose(memory);fclose(threads);
}
static DWORD WINAPI save_main_root(void *unused) {
    (void)unused;ULONGLONG deadline=GetTickCount64()+3000;
    while(!MEM32(0x20014)) { assert(GetTickCount64()<deadline);Sleep(1); }
    assert(xbox_CheckpointBegin(3000));
    assert(g_thread_call_count==1 && g_thread_mode==0);
    uint32_t saved[]={MEM32(0x20004),MEM32(0x20014)};save_files(saved);
    xbox_CheckpointEnd();Sleep(40);assert(MEM32(0x20014)>saved[1]);MEM32(0x20020)=1;return 0;
}
int main(int argc,char **argv) {
    assert(argc==5);_putenv_s("RECOMP_GUEST_LOCK","1");setup_memory(argv[2]);
    memory_path=argv[3];threads_path=argv[4];
    g_slot_ordinals[0]=255;g_slot_bridges[0]=bridge_PsCreateSystemThreadEx;g_slot_arg_bytes[0]=40;
    assert(guest_cpu_enabled());uint32_t saved[2];
    int main_root=strstr(argv[1],"main")!=NULL;
    if(!strcmp(argv[1],"hot_dynamic")) {
        RecompContFrame creator;recomp_cont_enter(&creator,0x9000,NULL,0);recomp_cont_site(&creator,0x100009000ULL);
        HANDLE a=bridge_spawn_thread(sub_00011000,0x20000,0,xbox_AllocThreadStack());
        HANDLE b=bridge_spawn_thread(sub_00012000,0x20040,0,xbox_AllocThreadStack());recomp_cont_leave(&creator);assert(a && b);
        ULONGLONG deadline=GetTickCount64()+3000;
        while(!MEM32(0x20004) || !MEM32(0x20044)){assert(GetTickCount64()<deadline);Sleep(1);}
        assert(xbox_CheckpointBegin(3000));saved[0]=MEM32(0x20004);saved[1]=MEM32(0x20044);save_files(saved);xbox_CheckpointEnd();
        Sleep(20);MEM32(0x20024)=1;assert(WaitForSingleObject(b,3000)==WAIT_OBJECT_0 && MEM32(0x20040)==1);
        recomp_cont_enter(&creator,0x9000,NULL,0);recomp_cont_site(&creator,0x100009000ULL);
        HANDLE c=bridge_spawn_thread(sub_00015000,0x20080,0,xbox_AllocThreadStack());recomp_cont_leave(&creator);assert(c);
        deadline=GetTickCount64()+3000;while(!MEM32(0x20084)){assert(GetTickCount64()<deadline);Sleep(1);}
        assert(xbox_CheckpointBegin(3000));FILE *threads=fopen(threads_path,"rb"),*memory=fopen(memory_path,"rb");assert(threads && memory);
        void *prepared=xbox_CheckpointPrepareThreads(threads,64);assert(prepared && xbox_CheckpointQuiesceThreads(prepared));
        assert(WaitForSingleObject(c,3000)==WAIT_OBJECT_0);
        XboxThreadCreationState creation;assert(fread(saved,8,1,memory)==1 && fread(&creation,sizeof creation,1,memory)==1 &&
            xbox_LoadPhysicalMemory(memory) && xbox_RestoreThreadCreationState(&creation));fclose(memory);fclose(threads);
        XboxCheckpointRoot roots[64];unsigned count;assert(xbox_CheckpointApplyThreads(prepared,roots,&count) && count==2);xbox_CheckpointFreeThreads(prepared);
        assert(MEM32(0x20000)==1 && MEM32(0x20040)==1 && MEM32(0x20004)==saved[0] && MEM32(0x20044)==saved[1]);
        assert(!MEM32(0x20080) && !MEM32(0x20084));
        HANDLE restored[]={roots[0].handle,roots[1].handle};assert(WaitForMultipleObjects(2,restored,FALSE,0)==WAIT_TIMEOUT);
        xbox_CheckpointEnd();Sleep(30);
        assert(MEM32(0x20004)>saved[0] && MEM32(0x20044)>saved[1] && !MEM32(0x20084));
        MEM32(0x20020)=1;MEM32(0x20024)=1;assert(WaitForMultipleObjects(2,restored,TRUE,3000)==WAIT_OBJECT_0);
        assert(MEM32(0x2000c)==0x13579bdf && MEM32(0x2004c)==0x13579bdf);
        for(unsigned i=0;i<2;i++)CloseHandle(restored[i]);CloseHandle(a);CloseHandle(b);CloseHandle(c);
        xbox_MemoryLayoutShutdown();return 0;
    }

    if(!strcmp(argv[1],"save_main")) {
        MEM32(0x20024)=KERNEL_VA_BASE;g_esp=0x80000;PUSH32(g_esp,0x76543210);
        HANDLE controller=CreateThread(NULL,0,save_main_root,NULL,0,NULL);assert(controller);
        guest_cpu_join();sub_00013000();guest_cpu_part();checkpoint_drop_thread();
        assert(WaitForSingleObject(controller,3000)==WAIT_OBJECT_0);CloseHandle(controller);
        assert(MEM32(0x20000)==1 && MEM32(0x20010)==1 && MEM32(0x20018)==1);
        assert(MEM32(0x20008)==0 && MEM32(0x2001c)==0x444444cc && g_thread_call_count==1);
    } else if(!strcmp(argv[1],"save")) {
        RecompContFrame creator;recomp_cont_enter(&creator,0x9000,NULL,0);
        recomp_cont_site(&creator,0x100009000ULL);
        HANDLE handles[]={bridge_spawn_thread(sub_00011000,0x20000,0,xbox_AllocThreadStack()),
                          bridge_spawn_thread(sub_00011000,0x20010,0,xbox_AllocThreadStack())};
        recomp_cont_leave(&creator);assert(handles[0] && handles[1]);
        ULONGLONG deadline=GetTickCount64()+3000;
        while(!MEM32(0x20004) || !MEM32(0x20014)) { assert(GetTickCount64()<deadline);Sleep(1); }
        assert(xbox_CheckpointBegin(3000));saved[0]=MEM32(0x20004);saved[1]=MEM32(0x20014);
        save_files(saved);
        xbox_CheckpointEnd();Sleep(40);
        assert(MEM32(0x20004)>saved[0] && MEM32(0x20014)>saved[1]);
        MEM32(0x20020)=1;
        assert(WaitForMultipleObjects(2,handles,TRUE,3000)==WAIT_OBJECT_0);
        CloseHandle(handles[0]);CloseHandle(handles[1]);
    } else {
        assert(xbox_CheckpointBootstrapBegin());
        FILE *memory=fopen(argv[3],"rb"),*threads=fopen(argv[4],"rb");assert(memory && threads);
        XboxThreadCreationState creation;
        assert(fread(saved,sizeof saved,1,memory)==1 && fread(&creation,sizeof creation,1,memory)==1 &&
               xbox_LoadPhysicalMemory(memory) && xbox_RestoreThreadCreationState(&creation));fclose(memory);
        XboxCheckpointRoot roots[64];unsigned count=99;
        if(!strcmp(argv[1],"reject")) {
            assert(!xbox_CheckpointLoadThreads(threads,roots,64,&count) && count==0);
            assert(!g_checkpoint_threads[0].active);verify_stopped(saved);
            fclose(threads);xbox_CheckpointEnd();xbox_MemoryLayoutShutdown();return 0;
        }
        assert(xbox_CheckpointLoadThreads(threads,roots,64,&count) && count==(main_root?1:2));fclose(threads);
        if(main_root) {
            assert(roots[0].pcr==0x4000 && !roots[0].stack_top && !roots[0].spawned && roots[0].function==0x13000);
            assert(g_thread_call_count==1 && g_thread_mode==0);
            Sleep(30);verify_stopped(saved);xbox_CheckpointEnd();Sleep(40);
            assert(MEM32(0x20000)==1 && MEM32(0x20010)==1 && MEM32(0x20014)>saved[1]);
            MEM32(0x20020)=1;assert(WaitForSingleObject(roots[0].handle,3000)==WAIT_OBJECT_0);
            assert(MEM32(0x20008)==0 && MEM32(0x20018)==1 && MEM32(0x2001c)==0x444444cc);
            assert(g_thread_call_count==1);CloseHandle(roots[0].handle);
            xbox_MemoryLayoutShutdown();return 0;
        }
        /* This original no-TLS XBE shares its main PCR across workers. The
         * original stack identity still distinguishes both saved roots. */
        assert(roots[0].pcr==0x4000 && roots[1].pcr==0x4000);
        assert(roots[0].stack_top!=roots[1].stack_top);
        assert(roots[0].function==0x11000 && roots[1].function==0x11000);
        assert(roots[0].spawned==1 && roots[1].spawned==1);
        Sleep(30);verify_stopped(saved);
        /* Exercise repeated serialization of a freshly loaded frozen root. */
        FILE *again=tmpfile();assert(again && xbox_CheckpointWriteThreads(again));fclose(again);
        /* An active native thread set cannot be silently replaced by cold load. */
        threads=fopen(argv[4],"rb");unsigned ignored;
        assert(threads && !xbox_CheckpointLoadThreads(threads,roots,64,&ignored));fclose(threads);
        xbox_CheckpointEnd();Sleep(40);
        assert(MEM32(0x20000)==1 && MEM32(0x20010)==1);
        assert(MEM32(0x20004)>saved[0] && MEM32(0x20014)>saved[1]);
        MEM32(0x20020)=1;
        HANDLE handles[]={roots[0].handle,roots[1].handle};
        assert(WaitForMultipleObjects(2,handles,TRUE,3000)==WAIT_OBJECT_0);
        assert(MEM32(0x2000c)==0x13579bdf && MEM32(0x2001c)==0x13579bdf);
        CloseHandle(handles[0]);CloseHandle(handles[1]);
    }
    xbox_MemoryLayoutShutdown();return 0;
}
'''
    patch=pytest.MonkeyPatch()
    try:body=translate(patch,True,program)
    finally:patch.undo()
    (work/'roots.c').write_text(prefix+body+suffix,encoding='utf-8')
    libraries=list((root.parents[1]/'build/native/xboxrecomp/src').glob('*/*.lib'));assert len(libraries)==8
    libraries.append(root.parents[1]/'build_dsp_jit/x86_64-pc-windows-msvc/release/dsp56300_emu_ffi.lib')
    result=subprocess.run([compiler,'/nologo','/O2','/Gy','/MD','/utf-8',
        '/DWIN32_LEAN_AND_MEAN','/DNOMINMAX','/D_CRT_SECURE_NO_WARNINGS',
        '/I'+str(root/'src'),'/I'+str(root/'templates/runtime'),str(work/'roots.c'),
        '/Fe:'+str(work/'roots.exe'),'/link','/OPT:REF',*[str(p) for p in libraries],
        'kernel32.lib','user32.lib','gdi32.lib','advapi32.lib','ole32.lib','shell32.lib',
        'd3d11.lib','d3d12.lib','dxgi.lib','dxguid.lib','d3dcompiler.lib','winmm.lib','xaudio2_8.lib',
        'xinput.lib','bcrypt.lib','ntdll.lib','userenv.lib','ws2_32.lib'],
        cwd=work,capture_output=True,text=True,errors='replace')
    assert result.returncode==0,result.stdout+result.stderr
    return work/'roots.exe',root/'tools/conformance/test.xbe'


@pytest.mark.parametrize('main_root',[False,True])
def test_original_roots_resume_without_replaying_initial_effects(native_roots,tmp_path,main_root):
    executable,xbe=native_roots;memory=tmp_path/'physical.state';threads=tmp_path/'threads.state'
    def run(mode,path):
        result=subprocess.run([str(executable),mode,str(xbe),str(memory),str(path)],
                              capture_output=True,text=True,errors='replace',timeout=20)
        assert result.returncode==0,result.stdout+result.stderr
    run('save_main' if main_root else 'save',threads);run('load_main' if main_root else 'load',threads)
    data=threads.read_bytes()
    damaged=tmp_path/'damaged.state'
    for contents in [data[:100],b'WRONGTAG'+data[8:],data[:-1]]:
        damaged.write_bytes(contents);run('reject',damaged)


def test_original_hot_roots_recreate_missing_and_discard_added_threads(native_roots,tmp_path):
    executable,xbe=native_roots
    result=subprocess.run([str(executable),'hot_dynamic',str(xbe),str(tmp_path/'memory.state'),str(tmp_path/'threads.state')],
        capture_output=True,text=True,errors='replace',timeout=30)
    assert result.returncode==0,result.stdout+result.stderr
