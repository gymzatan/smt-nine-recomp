"""Exercise actual atlas lookup, file validation and demand-mapped storage."""
from pathlib import Path
import shutil
import struct
import subprocess
import pytest

ROOT=Path(__file__).resolve().parents[1]


@pytest.fixture(scope='module')
def font_probe(tmp_path_factory):
    compiler=shutil.which('cl');assert compiler,'requires the existing MSVC environment'
    work=tmp_path_factory.mktemp('font-assets')
    (work/'game_paths.h').write_text('#pragma once\n#define SMT9_RUNTIME_DIR "."\n#define SMT9_GAME_DIR "."\n#define nine_fopen fopen\n')
    (work/'probe.c').write_text(r'''
#include <assert.h>
#include <windows.h>
#include <psapi.h>
#include "font_assets.c"
static TextureAssetLookup callback;
void d3d8_SetTextureAssetLookup(TextureAssetLookup fn){callback=fn;}
int main(int argc,char **argv) {
    _set_error_mode(_OUT_TO_STDERR);_set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
    PROCESS_MEMORY_COUNTERS_EX before={0},after={0};
    GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS *)&before,sizeof before);
    int ok=nine_font_assets_init(2);
    GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS *)&after,sizeof after);
    if(argc>1 && !strcmp(argv[1],"reject")){assert(!ok && !callback);return 0;}
    assert(ok && callback && page_count==3);
    assert(after.PrivateUsage-before.PrivateUsage<2*1024*1024);
    for(unsigned i=0;i<page_count;i++) {
        unsigned width=512,height=512;
        const uint8_t *pixels=callback(pages[i].base,131072,512,512,&width,&height);
        assert(width==1024 && height==1024 && pixels==pages[i].pixels);
        MEMORY_BASIC_INFORMATION info;assert(VirtualQuery(pixels,&info,sizeof info));
        assert(info.Type==MEM_MAPPED && info.Protect==PAGE_READONLY);
        for(size_t j=0;j<pages[i].pixel_bytes;j++)assert(pixels[j]==i+10);
    }
    uint8_t *changed=malloc(131072);memcpy(changed,pages[0].base,131072);changed[1000]^=1;
    unsigned width=512,height=512;assert(!callback(changed,131072,512,512,&width,&height));free(changed);
    printf("private_delta=%llu pages=%u pixels_equal=1\n",(unsigned long long)(after.PrivateUsage-before.PrivateUsage),page_count);
    return 0;
}
''',encoding='utf-8')
    result=subprocess.run([compiler,'/nologo','/O2','/MD','/utf-8','/I'+str(work),'/I'+str(ROOT/'game/src'),str(work/'probe.c'),'/Fe:'+str(work/'probe.exe'),'/link','psapi.lib'],cwd=work,capture_output=True,text=True)
    assert result.returncode==0,result.stdout+result.stderr
    return work/'probe.exe'


@pytest.fixture
def font_files(tmp_path):
    (tmp_path/'fonts').mkdir();(tmp_path/'Media/SysFont').mkdir(parents=True)
    bases=[bytes([i+1])*131072 for i in range(3)]
    bundle=b'N9FONT\0\0'+struct.pack('<II',2,3)
    for i,base in enumerate(bases):
        bundle+=struct.pack('<IIII',1024,1024,len(base),4194304)+base+bytes([i+10])*4194304
    (tmp_path/'fonts/font2.bin').write_bytes(bundle)
    for name,items in [('f24',bases[:2]),('f18',bases[2:])]:
        data=b''.join(items)
        (tmp_path/f'Media/SysFont/sys_{name}.xpr').write_bytes(struct.pack('<III',0x30525058,len(data)+12,12)+data)
    return tmp_path


def test_original_pixels_and_lookup_use_read_only_file_pages(font_probe,font_files):
    result=subprocess.run([str(font_probe)],cwd=font_files,capture_output=True,text=True,timeout=15)
    assert result.returncode==0,result.stdout+result.stderr


@pytest.mark.parametrize('damage',['header','truncated','trailing','base'])
def test_invalid_font_bundle_is_rejected(font_probe,font_files,damage):
    path=font_files/'fonts/font2.bin';data=bytearray(path.read_bytes())
    if damage=='header':data[16:20]=struct.pack('<I',0xffffffff)
    elif damage=='truncated':del data[-1:]
    elif damage=='trailing':data+=b'X'
    else:data[32+1000]^=1
    path.write_bytes(data)
    result=subprocess.run([str(font_probe),'reject'],cwd=font_files,capture_output=True,text=True,timeout=15)
    assert result.returncode==0,result.stdout+result.stderr
