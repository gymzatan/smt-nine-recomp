"""Actual MmGetPhysicalAddress identities and DMA writes to original CPU bytes."""
from pathlib import Path
import shutil
import subprocess
import tempfile


def test_static_buffer_dma_identity_and_strict_spans():
    compiler = shutil.which('cl')
    assert compiler, 'requires the existing MSVC developer environment'
    root = Path(__file__).resolve().parents[2]
    source_text = r'''
#include "kernel/kernel.h"
int xbox_CheckpointFrozen(void) {return 0;}
#include "kernel/xbox_memory_layout.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>
static uint8_t cpu[8192];
static uint32_t allocation = 0x80001000;
void xbox_log(int a,const char *b,const char *c,...) { }
size_t xbox_GetMappedSize(void) { return 0x04000000; }
uint32_t xbox_HeapBlockSize(uint32_t address) { (void)address; return 0; }
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)(uintptr_t)cpu - 0x006DF000; }
uint32_t xbox_ContiguousAlloc(uint32_t bytes,uint32_t alignment) {
    assert(bytes == 4096 && alignment == 4096);
    uint32_t result=allocation; allocation+=4096; return result;
}
int main(int argc,char **argv) {
    uint32_t pa=(uint32_t)xbox_MmGetPhysicalAddress((void *)(uintptr_t)0x006DF384);
    assert(pa == 0x1384 && pa != 0x006DF384);
    assert(xbox_MmGetPhysicalAddress((void *)(uintptr_t)0x006DF38B) == pa+7);
    assert(allocation == 0x80002000); /* same page keeps its identity */
    assert(xbox_MmGetPhysicalAddress((void *)(uintptr_t)0x80004080) == 0x4080);
    assert(xbox_ResolvePhysicalBuffer(pa,8) == cpu+0x384);
    static const uint8_t descriptor[8]={18,1,0x10,1,0,0,0,8};
    memcpy(xbox_ResolvePhysicalBuffer(pa,8),descriptor,8);
    assert(memcmp(cpu+0x384,descriptor,8) == 0);
    assert(xbox_ResolvePhysicalBuffer(0x4080,4) ==
        (void *)((uintptr_t)xbox_GetMemoryOffset()+0x80004080));
    if(argc>1 && strcmp(argv[1],"cross")==0) xbox_ResolvePhysicalBuffer(0x1FFF,2);
    if(argc>1 && strcmp(argv[1],"span")==0) xbox_ResolvePhysicalBuffer(0x03FFFFFF,2);
    return 0;
}
'''
    with tempfile.TemporaryDirectory() as temporary:
        work=Path(temporary)
        source=work/'dma.c'
        source.write_text(source_text,encoding='utf-8')
        compiled=subprocess.run([compiler,'/nologo','/O2','/Gy','/DWIN32_LEAN_AND_MEAN','/DNOMINMAX',
            '/I'+str(root/'src'),str(source),str(root/'src/kernel/kernel_memory.c'),
            '/Fe:'+str(work/'dma.exe'),'/link','/OPT:REF','kernel32.lib'],
            cwd=work,capture_output=True,text=True,errors='replace')
        assert compiled.returncode == 0,compiled.stdout+compiled.stderr
        subprocess.run([str(work/'dma.exe')],check=True,timeout=5)
        for rejection in ['cross','span']:
            assert subprocess.run([str(work/'dma.exe'),rejection],timeout=5).returncode == 5
