"""Restore original token aliases, file cursor and native synchronization owners."""
from pathlib import Path
import shutil
import struct
import subprocess

import pytest

from tools.recomp.test_continuations import translate


@pytest.fixture(scope='module')
def native_objects(tmp_path_factory):
    root=Path(__file__).resolve().parents[2];work=tmp_path_factory.mktemp('checkpoint_objects')
    compiler=shutil.which('cl');assert compiler, 'requires existing MSVC environment'
    push=lambda value:b'\x68'+struct.pack('<I',value)
    call=lambda pointer:b'\xff\x15'+struct.pack('<I',pointer)
    take=push(0)+push(0)+bytes.fromhex('ff353c000200')+call(0x21000)
    enter=push(0x200a0)+call(0x21004);leave=push(0x200a0)+call(0x21008)
    release=push(0)+bytes.fromhex('ff353c000200')+call(0x2100c)
    program={0x11000:bytes.fromhex('5589e5ff0500000200')+take*2+enter*2+
             bytes.fromhex('ff0504000200a12000020085c074f1')+leave*2+release*2+
             bytes.fromhex('ff0508000200c9c20800')}
    prefix=r'''
#include "kernel/kernel_bridge.c"
#define RECOMP_GENERATED_CODE
#include "recomp_types.h"
#include <assert.h>
extern int nv2a_pb_exec_method(uint32_t,uint32_t,uint32_t,uint8_t *);
void sub_00011000(void);
recomp_func_t recomp_lookup(uint32_t va) { return va==0x11000?sub_00011000:NULL; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va;return NULL; }
void recomp_icall_fail_log(uint32_t va) { fprintf(stderr,"bad target %08x\n",va);exit(5); }
void recomp_icall_not_code_log(uint32_t va) { recomp_icall_fail_log(va); }
'''
    suffix=r'''
#undef esp
static void setup(const char *path,const char *directory) {
    FILE *file=fopen(path,"rb");assert(file);fseek(file,0,SEEK_END);
    long size=ftell(file);rewind(file);void *xbe=malloc(size);assert(xbe);
    assert(fread(xbe,size,1,file)==1 && fclose(file)==0 && xbox_MemoryLayoutInit(xbe,size));free(xbe);
    xbox_path_init(directory,directory);_putenv_s("RECOMP_GUEST_LOCK","1");assert(guest_cpu_enabled());
    void (*bridges[])(void)={bridge_NtWaitForSingleObject,bridge_RtlEnterCriticalSection,
                            bridge_RtlLeaveCriticalSection,bridge_NtReleaseMutant};
    unsigned sizes[]={12,4,4,8};unsigned ordinals[]={233,277,294,221};
    for(unsigned i=0;i<4;i++) { g_slot_bridges[i]=bridges[i];g_slot_arg_bytes[i]=sizes[i];
        g_slot_ordinals[i]=ordinals[i];MEM32(0x21000+i*4)=KERNEL_VA_BASE+i*4; }
}
static void duplicate(uint32_t source,uint32_t target,uint32_t options) {
    g_esp=0x80000;MEM32(g_esp)=source;MEM32(g_esp+4)=target;MEM32(g_esp+8)=options;
    bridge_NtDuplicateObject();assert(g_eax==0);
}
static void make_objects(void) {
    HANDLE handle;assert(xbox_NtCreateEvent(&handle,NULL,0,FALSE)==0);bridge_write_handle(0x20030,handle);
    assert(xbox_NtCreateEvent(&handle,NULL,1,TRUE)==0);bridge_write_handle(0x20034,handle);
    assert(xbox_NtCreateSemaphore(&handle,NULL,2,7)==0);bridge_write_handle(0x20038,handle);
    assert(xbox_NtCreateMutant(&handle,NULL,FALSE)==0);bridge_write_handle(0x2003c,handle);
    duplicate(MEM32(0x20030),0x20040,DUPLICATE_SAME_ACCESS);
    ke_shadow_insert(0x20090,CreateEventW(NULL,TRUE,TRUE,NULL));
    xbox_RtlInitializeCriticalSection((PRTL_CRITICAL_SECTION)XBOX_TO_NATIVE(0x200a0));
    XBOX_ANSI_STRING name={12,13,"D:\\asset.bin"};XBOX_OBJECT_ATTRIBUTES attrs={0,&name,0};XBOX_IO_STATUS_BLOCK ios;
    assert(xbox_NtCreateFile(&handle,XBOX_GENERIC_READ|XBOX_SYNCHRONIZE,&attrs,&ios,NULL,0,7,XBOX_FILE_OPEN,0)==0);
    bridge_write_handle(0x20050,handle);unsigned char data[4];DWORD got;
    bridge_note_async_handle(MEM32(0x20050));
    assert(ReadFile(handle,data,4,&got,NULL) && got==4 && data[3]==3);
    duplicate(MEM32(0x20050),0x20054,DUPLICATE_SAME_ACCESS);
}
static void verify_objects(void) {
    HANDLE manual=bridge_read_handle(0x20030),alias=bridge_read_handle(0x20040);
    assert(MEM32(0x20030)!=MEM32(0x20040) && checkpoint_native.compare(manual,alias)==0);
    assert(WaitForSingleObject(manual,0)==WAIT_TIMEOUT);assert(SetEvent(alias));
    assert(WaitForSingleObject(manual,0)==WAIT_OBJECT_0 && WaitForSingleObject(alias,0)==WAIT_OBJECT_0);
    assert(ResetEvent(manual) && WaitForSingleObject(alias,0)==WAIT_TIMEOUT);
    HANDLE automatic=bridge_read_handle(0x20034);
    assert(WaitForSingleObject(automatic,0)==WAIT_OBJECT_0 && WaitForSingleObject(automatic,0)==WAIT_TIMEOUT);
    HANDLE sem=bridge_read_handle(0x20038);
    assert(WaitForSingleObject(sem,0)==WAIT_OBJECT_0 && WaitForSingleObject(sem,0)==WAIT_OBJECT_0);
    assert(WaitForSingleObject(sem,0)==WAIT_TIMEOUT);LONG previous;
    assert(ReleaseSemaphore(sem,7,&previous) && previous==0 && !ReleaseSemaphore(sem,1,NULL));
    HANDLE file=bridge_read_handle(0x20050),file_alias=bridge_read_handle(0x20054);unsigned char byte;DWORD got;
    assert(bridge_handle_is_async(MEM32(0x20050)) && !bridge_handle_is_async(MEM32(0x20054)));
    assert(checkpoint_native.compare(file,file_alias)==0);
    assert(ReadFile(file,&byte,1,&got,NULL) && got==1 && byte==4);
    assert(ReadFile(file_alias,&byte,1,&got,NULL) && got==1 && byte==5);
    assert(WaitForSingleObject(ke_shadow_lookup(0x20090),0)==WAIT_OBJECT_0);
}
static void cleanup(void) {
    for(unsigned i=1;i<BRIDGE_HANDLE_MAX;i++) if(s_handle_table[i]) { xbox_NtClose(s_handle_table[i]);s_handle_table[i]=NULL; }
    ke_shadow_remove(0x20090);xbox_MemoryLayoutShutdown();
}
int main(int argc,char **argv) {
    assert(argc==7);setup(argv[2],argv[6]);
    if(!strcmp(argv[1],"closed_source_probe")) {
        HANDLE hole=CreateEventW(NULL,TRUE,FALSE,NULL);assert(hole);
        HANDLE handle;assert(xbox_NtCreateEvent(&handle,NULL,0,FALSE)==0);bridge_write_handle(0x20030,handle);CloseHandle(hole);
        uint32_t source=MEM32(0x20030);duplicate(source,0x20040,3);DWORD flags;
        printf("source_closed=%d table_live=%d\n",!GetHandleInformation(handle,&flags),bridge_resolve_handle(source)!=NULL);
        printf("native_reused=%d source_slot_closed=%d\n",handle==bridge_read_handle(0x20040),
               bridge_resolve_handle(source)!=NULL && !GetHandleInformation(bridge_resolve_handle(source),&flags));
        make_objects();duplicate(MEM32(0x20050),0x20058,3);
        XboxFileOpenState file_state;assert(xbox_CaptureFileOpenState(bridge_read_handle(0x20058),&file_state) && file_state.position==4);
        assert(xbox_CheckpointBootstrapBegin());FILE *out=tmpfile();assert(out && xbox_CheckpointWriteObjects(out));fclose(out);
        xbox_CheckpointEnd();cleanup();return 0;
    }

    if(!strncmp(argv[1],"machine_",8)) {
        FILE *image=fopen(argv[2],"rb");assert(image);fseek(image,0,SEEK_END);long size=ftell(image);rewind(image);
        void *data=malloc(size);assert(data && fread(data,size,1,image)==1 && xbox_CheckpointSetGameImage(data,size));fclose(image);free(data);
        assert(xbox_ConnectNv2a() && xbox_ConnectApu());nv2a_get_state()->pfifo.method_consumer=nv2a_pb_exec_method;
        HANDLE worker;
        if(!strcmp(argv[1],"machine_save")) {
            XBOX_ANSI_STRING link={6,7,"\\??\\U:"},target={42,43,"\\Device\\Harddisk0\\Partition1\\checkpoint"};
            target.Length=(USHORT)strlen(target.Buffer);target.MaximumLength=target.Length+1;
            assert(xbox_IoCreateSymbolicLink(&link,&target)==0);
            MEM32(0x24004)=0x13579bdf;
            MEM32(0x24008)=(uint32_t)xbox_MmGetPhysicalAddress((void *)(uintptr_t)0x24004);
            make_objects();RecompContFrame creator;recomp_cont_enter(&creator,0x9000,NULL,0);recomp_cont_site(&creator,0x100009000ULL);
            worker=bridge_spawn_thread(sub_00011000,0,0,xbox_AllocThreadStack());recomp_cont_leave(&creator);assert(worker);bridge_write_handle(0x20044,worker);
            ULONGLONG deadline=GetTickCount64()+3000;while(!MEM32(0x20004)){assert(GetTickCount64()<deadline);Sleep(1);}
            FILE *state=fopen(argv[3],"wb");assert(state && xbox_CheckpointSaveMachine(state,3000));fclose(state);
            assert(xbox_IoDeleteSymbolicLink(&link)==0);
            Sleep(30);uint32_t advanced=MEM32(0x20004);state=fopen(argv[3],"rb");assert(state && xbox_CheckpointLoadMachine(state,0,3000));fclose(state);
            assert(MEM32(0x20000)==1 && MEM32(0x20004)<advanced);worker=bridge_read_handle(0x20044);
        } else {
            FILE *state=fopen(argv[3],"rb");assert(state);
            if(!strcmp(argv[1],"machine_bad")) {
                MEM32(0x20000)=0xdeadbeef;assert(!xbox_CheckpointLoadMachine(state,1,3000) && MEM32(0x20000)==0xdeadbeef);fclose(state);return 0;
            }
            assert(xbox_CheckpointLoadMachine(state,1,3000));fclose(state);assert(MEM32(0x20000)==1);worker=bridge_read_handle(0x20044);
        }
        assert(xbox_LookupSymbolicLink("\\??\\U:") && !strcmp(xbox_LookupSymbolicLink("\\??\\U:"),"\\Device\\Harddisk0\\Partition1\\checkpoint"));
        assert(xbox_ResolvePhysicalBuffer(MEM32(0x24008),4)==XBOX_PTR(0x24004));
        assert(*(uint32_t *)xbox_ResolvePhysicalBuffer(MEM32(0x24008),4)==0x13579bdf);
        MEM32(0x20020)=1;assert(WaitForSingleObject(worker,3000)==WAIT_OBJECT_0 && MEM32(0x20008)==1);
        cleanup();return 0;
    }
    uint32_t saved[3];HANDLE worker;
    if(!strcmp(argv[1],"save") || !strcmp(argv[1],"hot")) {
        make_objects();RecompContFrame creator;recomp_cont_enter(&creator,0x9000,NULL,0);
        recomp_cont_site(&creator,0x100009000ULL);
        worker=bridge_spawn_thread(sub_00011000,0,0,xbox_AllocThreadStack());recomp_cont_leave(&creator);assert(worker);
        bridge_write_handle(0x20044,worker);
        ULONGLONG deadline=GetTickCount64()+3000;
        while(!MEM32(0x20004)) { assert(GetTickCount64()<deadline);Sleep(1); }
        assert(xbox_CheckpointBegin(3000));saved[0]=MEM32(0x20004);
        XboxCriticalSectionState critical[4];unsigned n;
        assert(xbox_CaptureCriticalSections(critical,4,&n) && n==1 && critical[0].depth==2);
        saved[1]=critical[0].site;saved[2]=MEM32(0x2003c);
        FILE *memory=fopen(argv[3],"wb"),*threads=fopen(argv[4],"wb"),*objects=fopen(argv[5],"wb");assert(memory && threads && objects);
        assert(fwrite(saved,sizeof saved,1,memory)==1 && xbox_SavePhysicalMemory(memory));
        assert(xbox_CheckpointWriteThreads(threads) && xbox_CheckpointWriteObjects(objects));
        fclose(memory);fclose(threads);fclose(objects);verify_objects();xbox_CheckpointEnd();
        if(!strcmp(argv[1],"hot")) {
            Sleep(30);assert(MEM32(0x20004)>saved[0] && xbox_CheckpointBegin(3000));
            memory=fopen(argv[3],"rb");threads=fopen(argv[4],"rb");objects=fopen(argv[5],"rb");assert(memory && threads && objects);
            void *prepared=xbox_CheckpointPrepareThreads(threads,4);assert(prepared && xbox_CheckpointQuiesceThreads(prepared));
            assert(fread(saved,sizeof saved,1,memory)==1 && xbox_LoadPhysicalMemory(memory));fclose(memory);fclose(threads);
            XboxCheckpointRoot roots[4];unsigned count;
            assert(xbox_CheckpointApplyThreads(prepared,roots,&count) && count==1);xbox_CheckpointFreeThreads(prepared);worker=roots[0].handle;
            assert(xbox_CheckpointLoadObjects(objects,roots,count));fclose(objects);
            assert(GetThreadId(bridge_read_handle(0x20044))==GetThreadId(worker));
            XboxCriticalSectionState critical[4];unsigned n;
            assert(xbox_CaptureCriticalSections(critical,4,&n) && n==1 && critical[0].depth==2 && critical[0].site==saved[1]);
            assert(MEM32(0x20000)==1 && MEM32(0x20004)==saved[0]);
            verify_objects();xbox_CheckpointEnd();
        }

    } else {
        assert(xbox_CheckpointBootstrapBegin());
        FILE *memory=fopen(argv[3],"rb"),*threads=fopen(argv[4],"rb"),*objects=fopen(argv[5],"rb");assert(memory && threads && objects);
        assert(fread(saved,sizeof saved,1,memory)==1 && xbox_LoadPhysicalMemory(memory));fclose(memory);
        XboxCheckpointRoot roots[4];unsigned count;
        assert(xbox_CheckpointLoadThreads(threads,roots,4,&count) && count==1);fclose(threads);worker=roots[0].handle;
        if(!strcmp(argv[1],"reject")) {
            assert(!xbox_CheckpointLoadObjects(objects,roots,count));fclose(objects);
            for(unsigned i=1;i<BRIDGE_HANDLE_MAX;i++) assert(!s_handle_table[i]);
            assert(MEM32(0x20000)==1 && MEM32(0x20004)==saved[0]);return 0;
        }
        assert(xbox_CheckpointLoadObjects(objects,roots,count));fclose(objects);assert(checkpoint_native_init());
        assert(GetThreadId(bridge_read_handle(0x20044))==GetThreadId(worker) && MEM32(0x2003c)==saved[2]);
        struct { LONG count;BOOLEAN owned,abandoned; } mutant;
        struct { HANDLE process,thread; } owner;
        HANDLE handle=bridge_read_handle(0x2003c);
        assert(checkpoint_native.mutant(handle,0,&mutant,sizeof mutant,NULL)==0 && mutant.count==-1);
        assert(checkpoint_native.mutant(handle,1,&owner,sizeof owner,NULL)==0 && (DWORD)(uintptr_t)owner.thread==GetThreadId(worker));
        assert(WaitForSingleObject(handle,0)==WAIT_TIMEOUT);
        XboxCriticalSectionState critical[4];unsigned n;
        assert(xbox_CaptureCriticalSections(critical,4,&n) && n==1 && critical[0].depth==2 && critical[0].site==saved[1]);
        assert(critical[0].owner[1]==roots[0].stack_top && MEM32(0x20004)==saved[0]);
        verify_objects();xbox_CheckpointEnd();
    }
    Sleep(30);assert(MEM32(0x20000)==1 && MEM32(0x20004)>saved[0]);MEM32(0x20020)=1;
    assert(WaitForSingleObject(worker,3000)==WAIT_OBJECT_0 && MEM32(0x20008)==1);
    HANDLE mutex=bridge_read_handle(0x2003c);assert(WaitForSingleObject(mutex,0)==WAIT_OBJECT_0 && ReleaseMutex(mutex));
    xbox_RtlEnterCriticalSection((PRTL_CRITICAL_SECTION)XBOX_TO_NATIVE(0x200a0));
    xbox_RtlLeaveCriticalSection((PRTL_CRITICAL_SECTION)XBOX_TO_NATIVE(0x200a0));
    if(strcmp(argv[1],"save")) CloseHandle(worker);cleanup();return 0;
}
'''
    patch=pytest.MonkeyPatch()
    try:body=translate(patch,True,program)
    finally:patch.undo()
    (work/'objects.c').write_text(prefix+body+suffix,encoding='utf-8')
    libraries=list((root.parents[1]/'build/native/xboxrecomp/src').glob('*/*.lib'));assert len(libraries)==8
    libraries.append(root.parents[1]/'build_dsp_jit/x86_64-pc-windows-msvc/release/dsp56300_emu_ffi.lib')
    result=subprocess.run([compiler,'/nologo','/O2','/Gy','/MD','/utf-8',
        '/DWIN32_LEAN_AND_MEAN','/DNOMINMAX','/D_CRT_SECURE_NO_WARNINGS',
        '/I'+str(root/'src'),'/I'+str(root/'templates/runtime'),str(work/'objects.c'),
        '/Fe:'+str(work/'objects.exe'),'/link','/OPT:REF',*[str(p) for p in libraries],
        'kernel32.lib','user32.lib','gdi32.lib','advapi32.lib','ole32.lib','shell32.lib',
        'd3d11.lib','d3d12.lib','dxgi.lib','dxguid.lib','d3dcompiler.lib','winmm.lib','xaudio2_8.lib',
        'xinput.lib','bcrypt.lib','ntdll.lib','userenv.lib','ws2_32.lib'],
        cwd=work,capture_output=True,text=True,errors='replace')
    assert result.returncode==0,result.stdout+result.stderr
    return work/'objects.exe',root/'tools/conformance/test.xbe'


def test_original_object_aliases_counts_positions_and_recursive_owners(native_objects,tmp_path):
    executable,xbe=native_objects;memory=tmp_path/'memory.state';threads=tmp_path/'threads.state';objects=tmp_path/'objects.state'
    (tmp_path/'asset.bin').write_bytes(bytes(range(32)))
    def run(mode,path=objects):
        result=subprocess.run([str(executable),mode,str(xbe),str(memory),str(threads),str(path),str(tmp_path)],
                              capture_output=True,text=True,errors='replace',timeout=20)
        assert result.returncode==0,result.stdout+result.stderr
        return result.stdout
    run('save');run('load')
    damaged=tmp_path/'damaged.state';data=objects.read_bytes()
    for contents in [data[:100],b'BROKEN'+data[6:],data[:-1]]:
        damaged.write_bytes(contents);run('reject',damaged)


def test_duplicate_close_source_preserves_actual_native_identity_and_file_policy(native_objects,tmp_path):
    executable,xbe=native_objects
    (tmp_path/'asset.bin').write_bytes(bytes(range(32)))
    result=subprocess.run([str(executable),'closed_source_probe',str(xbe),'unused','unused','unused',str(tmp_path)],
                          capture_output=True,text=True,errors='replace',timeout=20)
    assert result.returncode==0,result.stdout+result.stderr
    assert 'source_slot_closed=0' in result.stdout


def test_original_live_root_and_recursive_owners_resume_without_replaying_entry(native_objects,tmp_path):
    executable,xbe=native_objects
    (tmp_path/'asset.bin').write_bytes(bytes(range(32)))
    result=subprocess.run([str(executable),'hot',str(xbe),str(tmp_path/'memory.state'),
        str(tmp_path/'threads.state'),str(tmp_path/'objects.state'),str(tmp_path)],
        capture_output=True,text=True,errors='replace',timeout=30)
    assert result.returncode==0,result.stdout+result.stderr


def test_mandatory_machine_archive_same_fresh_process_and_integrity(native_objects,tmp_path):
    executable,xbe=native_objects
    (tmp_path/'asset.bin').write_bytes(bytes(range(32)))
    state=tmp_path/'machine.state'
    def run(mode,path=state):
        result=subprocess.run([str(executable),mode,str(xbe),str(path),'-','-',str(tmp_path)],capture_output=True,text=True,errors='replace',timeout=30)
        assert result.returncode==0,result.stdout+result.stderr
    run('machine_save');run('machine_load')
    bad=tmp_path/'bad-machine.state'
    data=bytearray(state.read_bytes());data[-1]^=1;bad.write_bytes(data);run('machine_bad',bad)
