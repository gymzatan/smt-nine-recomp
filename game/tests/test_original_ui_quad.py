"""Original UI mappings preserve full-screen fades and appearance viewports."""
from pathlib import Path
import shutil
import subprocess
import pytest


@pytest.fixture(scope='module')
def quad_probe(tmp_path_factory):
    compiler = shutil.which('cl')
    assert compiler, 'requires the existing MSVC environment'
    source = (Path(__file__).resolve().parents[1] / 'src/recomp_manual.c').read_text(encoding='utf-8')
    helper = source[source.index('static void prepare_original_ui_quad(void)'):source.index('void recomp_call_arguments(uint32_t target)')]
    program = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>
static unsigned char ram[0x10000];
#define MEM32(a) (*(uint32_t *)(ram+((uint32_t)(a)&0x7fffffffu)))
#define MEMF(a) (*(float *)(ram+((uint32_t)(a)&0x7fffffffu)))
#define NINE_TITLE_UI_ORIGIN 0
static uint32_t g_esp=0x1000,g_esi=0x2000,g_ebx;
static float aspect;
float smt9_game_aspect(void){return aspect;}
int smt9_is_chinese(void){return 0;}
unsigned d3d8_RenderScale(void){return 2;}
'''+helper+r'''
static void check(float left,float top,float width,float height,int fullscreen) {
    memset(ram,0,sizeof ram);MEM32(g_esp)=0x001C4A3B;
    MEM32(g_esp+4)=8;MEM32(g_esp+8)=0;MEM32(g_esp+12)=4;
    MEM32(g_esi+4)=0x3000;MEM32(0x3004)=0x4000;
    for(unsigned i=0;i<4;i++) {
        unsigned v=0x4000+i*20;
        MEMF(v)=left+(i&1?width:0);MEMF(v+4)=top+(i&2?height:0);
        MEM32(v+8)=0x3f000000+i;MEM32(v+12)=0x3f800000;MEM32(v+16)=0xfcffffff-i;
    }
    unsigned char before[80];memcpy(before,ram+0x4000,sizeof before);
    prepare_original_ui_quad();
    if(fullscreen || aspect==4.0f/3.0f)assert(!memcmp(before,ram+0x4000,sizeof before));
    else for(unsigned i=0;i<4;i++) {
        float original;memcpy(&original,before+i*20,4);
        assert(MEMF(0x4000+i*20)==320+(original-320)*((4.0f/3.0f)/aspect));
    }
    for(unsigned i=0;i<4;i++)assert(!memcmp(before+i*20+4,ram+0x4000+i*20+4,16));
}
static void check_preview(uint32_t caller,int active) {
    memset(ram,0,sizeof ram);MEM32(g_esp)=caller;MEM32(g_esp+4)=0x4000;
    uint32_t original[6]={208,88,144,288,0x3e800000,0x3f800000};
    memcpy(ram+0x4000,original,sizeof original);
    prepare_original_preview_viewport();
    if(aspect==4.0f/3.0f || !active)assert(!memcmp(ram+0x4000,original,sizeof original));
    else {
        /* At 1280x720 the original panel is x448..712. The preview's
         * matching UI coordinates put it at x472..688, inside that frame. */
        assert(MEM32(0x4000)==236 && MEM32(0x4008)==108);
        assert(MEM32(0x4000)>=224 && MEM32(0x4000)+MEM32(0x4008)<=356);
    }
    assert(MEM32(0x4004)==88 && !memcmp(ram+0x400c,original+3,12));
}
static void check_projection(uint32_t caller,int active,float x,float y,float ratio,float scale) {
    memset(ram,0,sizeof ram);MEM32(g_esp)=caller;MEM32(g_esp+4)=0x4000;
    MEMF(g_esp+8)=ratio;MEMF(g_esp+12)=scale;
    MEMF(0x4000)=x;MEMF(0x4004)=y;MEM32(0x4008)=0x3f000000;
    prepare_original_preview_projection();
    if(aspect==4.0f/3.0f || !active) {
        assert(MEMF(0x4000)==x && MEMF(g_esp+8)==ratio);
    } else {
        assert(MEMF(0x4000)==(x==144?188:290));
        assert(fabsf(MEMF(g_esp+8)-ratio*0.75f)<0.000001f);
    }
    assert(MEMF(0x4004)==y && MEM32(0x4008)==0x3f000000 && MEMF(g_esp+12)==scale);
}
int main(int argc,char **argv) {
    assert(argc==2);aspect=atoi(argv[1])==4?4.0f/3.0f:16.0f/9.0f;
    check(0,0,640,480,1);check(-0.5f,-0.5f,640,480,1);
    check(12,285,224,40,0);check(12,0,100,480,0);
    check_preview(0x000F826F,1);check_preview(0x000F8709,1);
    check_preview(0x000F84D8,0);check_preview(0x000F8969,0);check_preview(0x000EE537,0);
    check_projection(0x001427AE,1,144,176,1.09523809f,84);
    check_projection(0x000F819E,1,280,240,0.75f,320);
    check_projection(0x000F8638,1,280,240,0.75f,320);
    check_projection(0x000EE537,0,280,240,0.75f,320);
    return 0;
}
'''
    work = tmp_path_factory.mktemp('original_ui_quad')
    (work / 'quad.c').write_text(program, encoding='utf-8')
    result = subprocess.run([compiler, '/nologo', '/O2', '/UNDEBUG', 'quad.c', '/Fe:quad.exe'], cwd=work, capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    return work / 'quad.exe'


@pytest.mark.parametrize('aspect', ['4', '16'])
def test_actual_shared_color_quad_preserves_fade_extent_and_payload(quad_probe, aspect):
    subprocess.run([str(quad_probe), aspect], check=True, timeout=10)
