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
static uint32_t g_esp=0x1000,g_esi=0x2000,g_ebx,g_edi,g_ecx,g_ebp=0x1800;
static float aspect;
float smt9_game_aspect(void){return aspect;}
int smt9_is_chinese(void){return 0;}
unsigned d3d8_RenderScale(void){return 2;}
'''+helper+r'''
static void check_dialogue_connector(void) {
    memset(ram,0,sizeof ram);g_edi=0x2000;MEM32(g_edi+4)=0x3000;MEM32(0x3004)=0x4000;
    const float xy[8][2]={{250.98828f,261.509f},{56,189.509f},{250.98828f,265.509f},{56,193.509f},{56,189.509f},{276,189.509f},{56,193.509f},{276,193.509f}};
    for(unsigned i=0;i<8;i++){MEMF(0x4000+i*20)=xy[i][0];MEMF(0x4004+i*20)=xy[i][1];MEM32(0x4008+i*20)=MEM32(0x400c+i*20)=0x3f800000;MEM32(0x4010+i*20)=i%2?0x604040a0:0xfff0f0f0;}
    unsigned char original[160];memcpy(original,ram+0x4000,160);
    for(unsigned strip=0;strip<2;strip++) {
        MEM32(g_esp)=strip?0x00105F08:0x00105EEC;MEM32(g_esp+4)=6;MEM32(g_esp+8)=strip*4;MEM32(g_esp+12)=4;
        prepare_original_ui_quad();
        for(unsigned i=0;i<8;i++) {
            float want=aspect==4.0f/3.0f || i>strip*4+3?xy[i][0]:320+(xy[i][0]-320)*((4.0f/3.0f)/aspect);
            assert(MEMF(0x4000+i*20)==want);assert(!memcmp(original+i*20+4,ram+0x4004+i*20,16));
        }
    }
}

static void check_speech_anchor(void) {
    const uint32_t callers[]={0x006EC256,0x006EC26D,0x0018C795,0x0018C906,0x006EC26C};
    const float heads[]={100,320,560},steps[]={0,0.25f,1};
    float factor=(4.0f/3.0f)/aspect;
    for(unsigned c=0;c<5;c++)for(unsigned n=0;n<3;n++) {
        memset(ram,0,sizeof ram);MEM32(g_esp)=callers[c];MEM32(g_esp+4)=0x4000;MEM32(g_esp+8)=0x5000;
        MEMF(0x4000)=heads[n];MEMF(0x4004)=150;MEMF(0x4008)=0.5f;
        MEMF(0x5000)=56;MEMF(0x5004)=80;MEMF(0x5008)=1;
        unsigned char original[12],box[12];memcpy(original,ram+0x4000,12);memcpy(box,ram+0x5000,12);
        prepare_original_speech_anchor();
        assert(!memcmp(original+4,ram+0x4004,8) && !memcmp(box,ram+0x5000,12));
        if(c==4)assert(!memcmp(original,ram+0x4000,12));
        else for(unsigned t=0;t<3;t++) {
            /* Interpolation must connect the displayed head to the mapped
             * box for the whole animation, including partial strips. */
            float generated=MEMF(0x4000)+(56-MEMF(0x4000))*steps[t];
            float displayed=320+(generated-320)*factor;
            float mapped_box=320+(56-320)*factor;
            float expected=heads[n]+(mapped_box-heads[n])*steps[t];
            assert(fabsf(displayed-expected)<0.0001f);
        }
    }
}

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
static void check_rgb(float width,int active) {
    memset(ram,0,sizeof ram);MEM32(g_esp)=active?0x00110102:0x00110101;
    MEM32(g_esp+4)=7;MEM32(g_esp+8)=4;MEM32(g_esp+12)=0x4000;MEM32(g_esp+16)=20;
    const float x[]={240,240+width,240+width,240},y[]={237,237,245,245};
    for(unsigned i=0;i<4;i++) {
        unsigned vertex=0x4000+i*20;MEMF(vertex)=x[i];MEMF(vertex+4)=y[i];
        MEM32(vertex+8)=0x3f000000+i;MEM32(vertex+12)=0x3f800000;MEM32(vertex+16)=0xff60d060+i;
    }
    unsigned char before[80];memcpy(before,ram+0x4000,sizeof before);
    prepare_original_ui_quad();
    if(aspect==4.0f/3.0f || !active)assert(!memcmp(before,ram+0x4000,sizeof before));
    else for(unsigned i=0;i<4;i++)assert(MEMF(0x4000+i*20)==320+(x[i]-320)*((4.0f/3.0f)/aspect));
    for(unsigned i=0;i<4;i++)assert(!memcmp(before+i*20+4,ram+0x4000+i*20+4,16));
}
static void check_menu_color(uint32_t caller,unsigned primitive,unsigned count,unsigned stride,const float *x,const float *y,int active) {
    memset(ram,0,sizeof ram);MEM32(g_esp)=caller;
    MEM32(g_esp+4)=primitive;MEM32(g_esp+8)=count;MEM32(g_esp+12)=0x4000;MEM32(g_esp+16)=stride;
    for(unsigned i=0;i<count;i++) {
        unsigned vertex=0x4000+i*stride;MEMF(vertex)=x[i];MEMF(vertex+4)=y[i];
        for(unsigned offset=8;offset<stride;offset+=4)MEM32(vertex+offset)=0x604040a0+i+offset;
    }
    if(caller==0x001482D5){g_ebx=0x2000;MEM32(g_ebx+0x18)=0x4000;MEM32(g_esp+8)=0;MEM32(g_esp+12)=count;}
    if(caller==0x000F4F82){MEM32(g_esi+8)=0x3000;MEM32(0x3004)=0x4000;MEM32(g_esp+8)=0;MEM32(g_esp+12)=count;}
    unsigned char before[6*28];memcpy(before,ram+0x4000,count*stride);
    prepare_original_ui_quad();
    if(aspect==4.0f/3.0f || !active)assert(!memcmp(before,ram+0x4000,count*stride));
    else for(unsigned i=0;i<count;i++)assert(MEMF(0x4000+i*stride)==320+(x[i]-320)*((4.0f/3.0f)/aspect));
    for(unsigned i=0;i<count;i++)assert(!memcmp(before+i*stride+4,ram+0x4000+i*stride+4,stride-4));
}
static void check_portrait_strip(float left,float top,float height) {
    memset(ram,0,sizeof ram);MEM32(g_esp)=0x001452C5;
    MEM32(g_esp+4)=9;MEM32(g_esp+8)=0;MEM32(g_esp+12)=22;
    MEM32(g_ebp-12)=0x3000;MEM32(0x3004)=0x4000;
    for(unsigned i=0;i<22;i++) {
        unsigned vertex=0x4000+i*28;
        MEMF(vertex)=left+(i&1?80:0);MEMF(vertex+4)=top+(i/2)*height/10;
        for(unsigned offset=8;offset<28;offset+=4)MEM32(vertex+offset)=0x3f000000+i+offset;
    }
    unsigned char before[22*28];memcpy(before,ram+0x4000,sizeof before);
    prepare_original_ui_quad();
    for(unsigned i=0;i<22;i++) {
        float x;memcpy(&x,before+i*28,4);
        assert(MEMF(0x4000+i*28)==320+(x-320)*((4.0f/3.0f)/aspect));
        assert(!memcmp(before+i*28+4,ram+0x4000+i*28+4,24));
    }
    if(aspect==4.0f/3.0f)assert(!memcmp(before,ram+0x4000,sizeof before));
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
    float factor=(4.0f/3.0f)/aspect;
    int appearance=active && caller!=0x001427AE;
    if(!active || aspect==4.0f/3.0f)assert(MEMF(0x4000)==x);
    else assert(MEMF(0x4000)==(x==144?188:290));
    if(appearance) {
        /* The patched appearance shader reads c36.y for Y instead of X.
         * Reconstruct the original scalar on both axes, then widen X only. */
        float original_scalar=ratio*scale;
        assert(fabsf(MEMF(g_esp+12)-original_scalar)<0.00001f);
        assert(fabsf(MEMF(g_esp+8)*MEMF(g_esp+12)-original_scalar*factor)<0.00001f);
    } else {
        assert(fabsf(MEMF(g_esp+8)-ratio*(active?factor:1))<0.000001f);
        assert(MEMF(g_esp+12)==scale);
    }
    assert(MEMF(0x4004)==y && MEM32(0x4008)==0x3f000000);
}
static void check_window_transition(void) {
    memset(ram,0,sizeof ram);g_ecx=0x2000;
    /* Actual Manual fusion capture: x184, y80, width392, height288. */
    uint32_t original[16]={1,0,0x43380000,0x42a00000,20,0,0,392,288,0x3f800000,0x3f800000,0x3f800000,0,0,0,0};
    memcpy(ram+g_ecx+0x34,original,sizeof original);
    smt9_prepare_window_transition(g_ecx);
    if(aspect==4.0f/3.0f)assert(!memcmp(ram+g_ecx+0x34,original,sizeof original));
    else {
        /* Visible list maps 184..576 to 218..512; the source rectangle
         * must cover that same area before the original mesh is built. */
        assert(MEMF(g_ecx+0x3c)==218 && MEM32(g_ecx+0x50)==294);
        original[2]=MEM32(g_ecx+0x3c);original[7]=MEM32(g_ecx+0x50);
        assert(!memcmp(ram+g_ecx+0x34,original,sizeof original));
    }
}
static void check_item_icons(void) {
    /* Reward frame 208..432, icon 220..236, text starts at 248.
     * At 1920x1080 the unmapped icon was left of the mapped frame. */
    for(unsigned frame=0;frame<120;frame++)for(unsigned row=0;row<2;row++) {
        memset(ram,0,sizeof ram);g_esi=0x2000;MEM32(g_esi+0x14)=0x3000;MEM32(0x3004)=0x4000;
        MEM32(g_esp)=0x001C4CF4;MEM32(g_esp+4)=8;MEM32(g_esp+8)=0;MEM32(g_esp+12)=4;
        for(unsigned i=0;i<4;i++) {
            unsigned vertex=0x4000+i*28;
            MEMF(vertex)=220+(i&1?16:0);MEMF(vertex+4)=244+row*22+(i&2?16:0);
            for(unsigned offset=8;offset<28;offset+=4)MEM32(vertex+offset)=0x3f000000+i+offset;
        }
        unsigned char before[112];memcpy(before,ram+0x4000,sizeof before);
        prepare_original_ui_quad();
        float factor=(4.0f/3.0f)/aspect;
        float panel_left=320+(208-320)*factor,text_left=320+(248-320)*factor;
        for(unsigned i=0;i<4;i++) {
            float x;memcpy(&x,before+i*28,4);
            assert(MEMF(0x4000+i*28)==320+(x-320)*factor);
            assert(MEMF(0x4000+i*28)>panel_left && MEMF(0x4000+i*28)<text_left);
            assert(!memcmp(before+i*28+4,ram+0x4000+i*28+4,24));
        }
        if(aspect==4.0f/3.0f)assert(!memcmp(before,ram+0x4000,sizeof before));
    }
}
int main(int argc,char **argv) {
    assert(argc==2);aspect=atoi(argv[1])==4?4.0f/3.0f:16.0f/9.0f;
    check(0,0,640,480,1);check(-0.5f,-0.5f,640,480,1);
    check(12,285,224,40,0);check(12,0,100,480,0);
    check_rgb(0,1);check_rgb(0.5f,1);check_rgb(32,1);check_rgb(80,1);check_rgb(127.5f,1);check_rgb(80,0);
    check_portrait_strip(352,164,272);check_portrait_strip(171,217,216);check_portrait_strip(202,163,80);
    check_dialogue_connector();check_speech_anchor();
    check_item_icons();
    check_window_transition();
    memset(ram,0,sizeof ram);g_ecx=0x2000;MEM32(g_ecx+0x50)=640;MEM32(g_ecx+0x54)=480;
    smt9_prepare_window_transition(g_ecx);assert(MEMF(g_ecx+0x3c)==0 && MEM32(g_ecx+0x50)==640);
    const float guide_x[]={426,426,381,384,381,384},guide_y[]={262,265,280,282,319,319};
    check_menu_color(0x001357E7,6,6,20,guide_x,guide_y,1);
    check_menu_color(0x001357E6,6,6,20,guide_x,guide_y,0);
    const float formation_x[]={231,231,148,148,100,102},formation_y[]={85,87,85,87,137,137};
    check_menu_color(0x001482D5,6,6,28,formation_x,formation_y,1);
    const float bar_x[]={372,381,381,372},bar_y[]={216,216,374,374};
    const float target_x[]={572,581,581,572},target_y[]={120,120,374,374};
    check_menu_color(0x00146153,7,4,28,bar_x,bar_y,1);
    check_menu_color(0x00146153,7,4,28,target_x,target_y,1);
    check_menu_color(0x00146152,7,4,28,bar_x,bar_y,0);
    const float fade_x[]={0,640,640,0},fade_y[]={0,0,480,480};
    check_menu_color(0x00146153,7,4,28,fade_x,fade_y,0);
    const float navi_x[]={542,542,551,551},navi_y[]={408,120,408,120};
    check_menu_color(0x000F4F82,6,4,20,navi_x,navi_y,1);
    const float field_x[]={0,0,639,639},field_y[]={479,0,479,0};
    check_menu_color(0x000F4F82,6,4,20,field_x,field_y,0);
    check_menu_color(0x000F4F82,6,4,20,fade_x,fade_y,0);
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
