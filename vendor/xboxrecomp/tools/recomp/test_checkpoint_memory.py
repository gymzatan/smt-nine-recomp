"""Cold restore of original physical sections, aliases and allocation state."""
from pathlib import Path
import shutil
import subprocess

import pytest


@pytest.fixture(scope='module')
def native_memory(tmp_path_factory):
    root=Path(__file__).resolve().parents[2];work=tmp_path_factory.mktemp('checkpoint_memory')
    compiler=shutil.which('cl');assert compiler, 'requires existing MSVC environment'
    source=r'''
#include "kernel/kernel.h"
#include "kernel/xbox_memory_layout.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t va) { (void)va;return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va;return NULL; }
static uint32_t *guest(uint32_t address) { return (uint32_t *)((uintptr_t)address+xbox_GetMemoryOffset()); }
int main(int argc,char **argv) {
    assert(argc==4);_putenv_s("RECOMP_GUEST_LOCK","1");_putenv_s("RECOMP_HEAP_RECLAIM","1");
    FILE *xbe_file=fopen(argv[2],"rb");assert(xbe_file);fseek(xbe_file,0,SEEK_END);
    long bytes=ftell(xbe_file);rewind(xbe_file);void *xbe=malloc(bytes);assert(xbe);
    assert(fread(xbe,bytes,1,xbe_file)==1 && fclose(xbe_file)==0);
    assert(xbox_MemoryLayoutInit(xbe,bytes));free(xbe);
    assert(xbox_CheckpointBootstrapBegin());
    uint32_t addresses[3],next_heap,next_contig;
    if(!strcmp(argv[1],"truncated")) {
        uint32_t original=xbox_HeapAlloc(4096,4096);*guest(original)=0xfedcba98;
        FILE *f=fopen(argv[3],"rb");assert(f);
        assert(fread(addresses,sizeof addresses,1,f)==1 && !xbox_LoadPhysicalMemory(f));fclose(f);
        assert(*guest(original)==0xfedcba98 && xbox_HeapBlockSize(original)==4096);
        assert(xbox_HeapAlloc(4096,4096)==original+4096);
        xbox_CheckpointEnd();xbox_MemoryLayoutShutdown();return 0;
    } else if(!strcmp(argv[1],"save")) {
        addresses[0]=xbox_HeapAlloc(4096,4096);
        uint32_t hole=xbox_HeapAlloc(8192,4096);
        addresses[1]=xbox_ContiguousAlloc(4096,4096);
        uint32_t physical_hole=xbox_ContiguousAlloc(8192,4096);
        addresses[2]=xbox_AllocThreadStack();assert(addresses[0] && addresses[1] && addresses[2]);
        xbox_HeapFree(hole);xbox_ContiguousFree(physical_hole);
        assert(xbox_worker_stack_alloc()==0 && xbox_worker_stack_alloc()==1);xbox_worker_stack_free(0);
        *guest(addresses[0])=0x12345678;*guest(addresses[1])=0x89abcdef;*guest(0xfd700100)=0x76543210;
        FILE *f=fopen(argv[3],"wb");assert(f);
        assert(fwrite(addresses,sizeof addresses,1,f)==1 && xbox_SavePhysicalMemory(f));
        assert(fclose(f)==0);
        next_heap=xbox_HeapAlloc(4096,4096);next_contig=xbox_ContiguousAlloc(4096,4096);
        assert(next_heap==hole && next_contig==physical_hole);
        assert(xbox_worker_stack_alloc()==0);
    } else {
        FILE *f=fopen(argv[3],"rb");assert(f);
        assert(fread(addresses,sizeof addresses,1,f)==1 && xbox_LoadPhysicalMemory(f));assert(fclose(f)==0);
        assert(*guest(addresses[0])==0x12345678 && *guest(addresses[1])==0x89abcdef);
        assert(*guest(0xfd700100)==0x76543210 && xbox_HeapBlockSize(addresses[0])==4096);
        *guest(addresses[0]+(uint32_t)xbox_GetMappedSize())=0xabcdef12;
        assert(*guest(addresses[0])==0xabcdef12);
        *guest(XBOX_TILED_BASE+addresses[1]-XBOX_CONTIG_BASE)=0x13579bdf;
        assert(*guest(addresses[1])==0x13579bdf);
        next_heap=xbox_HeapAlloc(4096,4096);next_contig=xbox_ContiguousAlloc(4096,4096);
        assert(xbox_worker_stack_alloc()==0);
        xbox_FreeThreadStack(addresses[2]);
    }
    printf("%08x %08x\n",next_heap,next_contig);
    xbox_CheckpointEnd();xbox_MemoryLayoutShutdown();return 0;
}
'''
    (work/'memory.c').write_text(source,encoding='utf-8')
    libraries=list((root.parents[1]/'build/xboxrecomp/src').glob('*/*.lib'));assert len(libraries)==8
    libraries.append(root.parents[1]/'build_dsp_jit/x86_64-pc-windows-msvc/release/dsp56300_emu_ffi.lib')
    result=subprocess.run([compiler,'/nologo','/O2','/Gy','/MD','/utf-8',
        '/DWIN32_LEAN_AND_MEAN','/DNOMINMAX','/D_CRT_SECURE_NO_WARNINGS','/I'+str(root/'src'),
        str(work/'memory.c'),'/Fe:'+str(work/'memory.exe'),'/link','/OPT:REF',*[str(p) for p in libraries],
        'kernel32.lib','user32.lib','gdi32.lib','advapi32.lib','ole32.lib','shell32.lib',
        'd3d11.lib','dxgi.lib','dxguid.lib','d3dcompiler.lib','winmm.lib','xaudio2_8.lib',
        'xinput.lib','bcrypt.lib','ntdll.lib','userenv.lib','ws2_32.lib'],
        cwd=work,capture_output=True,text=True,errors='replace')
    assert result.returncode==0,result.stdout+result.stderr
    return work/'memory.exe',root/'tools/conformance/test.xbe'


def test_original_memory_aliases_and_next_allocations_survive_new_process(native_memory,tmp_path):
    executable,xbe=native_memory;snapshot=tmp_path/'physical.state'
    saved=subprocess.run([str(executable),'save',str(xbe),str(snapshot)],capture_output=True,text=True,errors='replace',timeout=20)
    assert saved.returncode==0,saved.stdout+saved.stderr
    loaded=subprocess.run([str(executable),'load',str(xbe),str(snapshot)],capture_output=True,text=True,errors='replace',timeout=20)
    assert loaded.returncode==0,loaded.stdout+loaded.stderr
    assert loaded.stdout.splitlines()[-1]==saved.stdout.splitlines()[-1]
    truncated=tmp_path/'truncated.state'
    with snapshot.open('rb') as source:truncated.write_bytes(source.read(4096))
    rejected=subprocess.run([str(executable),'truncated',str(xbe),str(truncated)],
                            capture_output=True,text=True,errors='replace',timeout=20)
    assert rejected.returncode==0,rejected.stdout+rejected.stderr
