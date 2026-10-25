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
static unsigned char ram[0x600000];
#define MEM32(a) (*(uint32_t *)(ram+((uint32_t)(a)&0x7fffffffu)))
#define MEMF(a) (*(float *)(ram+((uint32_t)(a)&0x7fffffffu)))
static uint32_t g_esp=0x1000,g_esi=0x2000,g_ebx,g_edi,g_ecx,g_ebp=0x1800;
static float aspect;
float smt9_game_aspect(void){return aspect;}
static unsigned font_chinese,font_render_scale=2;
int smt9_is_chinese(void){return font_chinese;}
unsigned d3d8_RenderScale(void){return font_render_scale;}
'''+(Path(__file__).resolve().parents[1] / 'src/font_assets.h').read_text(encoding='utf-8')+helper+r'''
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
    MEM32(g_ebp-0x30)=0x6000;MEM32(0x6004)=0x003B21E8;
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
static void check_original_backdrop_strip(void) {
    for(unsigned tile=0;tile<2;tile++) {
        memset(ram,0,sizeof ram);MEM32(g_esp)=0x001452C5;
        MEM32(g_ebp-0x30)=0x6000;MEM32(0x6004)=0x003B2480;
        MEM32(g_esp+4)=9;MEM32(g_esp+8)=0;MEM32(g_esp+12)=22;
        MEM32(g_ebp-12)=0x3000;MEM32(0x3004)=0x4000;
        for(unsigned i=0;i<22;i++) {
            unsigned v=0x4000+i*28;
            MEMF(v)=(tile?512:0)+(i&1?(tile?128:512):0);MEMF(v+4)=(i/2)*48;
            for(unsigned offset=8;offset<28;offset+=4)MEM32(v+offset)=0x3f000000+i+offset;
        }
        unsigned char before[22*28];memcpy(before,ram+0x4000,sizeof before);
        prepare_original_ui_quad();assert(!memcmp(before,ram+0x4000,sizeof before));
    }
}
static void check_settled_portrait(float width,float height,int fullscreen) {
    memset(ram,0,sizeof ram);g_esi=0x2000;
    MEM32(g_esp+0x5c)=0x6000;MEM32(0x6004)=0x003B21E8;
    MEM32(g_esp)=0x0014617A;MEM32(g_esp+4)=7;MEM32(g_esp+8)=4;
    MEM32(g_esp+12)=0x4000;MEM32(g_esp+16)=28;
    MEM32(g_esp+0x13c)=512;MEM32(g_esp+0x140)=512;
    float left=(640-width)*0.5f,top=480-height;
    for(unsigned i=0;i<4;i++) {
        unsigned v=0x4000+i*28;
        MEMF(v)=left+(i&1?width:0);MEMF(v+4)=top+(i&2?height:0);
        MEM32(v+8)=0x3f000000+i;MEM32(v+12)=0x3f800000;MEM32(v+16)=0xff123400+i;
        MEMF(v+20)=i&1?1:0;MEMF(v+24)=i&2?1:0;
    }
    unsigned char before[112];memcpy(before,ram+0x4000,sizeof before);
    prepare_original_ui_quad();
    float factor=(4.0f/3.0f)/aspect;
    for(unsigned i=0;i<4;i++) {
        unsigned v=0x4000+i*28;
        float x;memcpy(&x,before+i*28,4);
        assert(MEMF(v)==(fullscreen?x:320+(x-320)*factor));
        assert(!memcmp(before+i*28+4,ram+v+4,16));
        if(fullscreen)assert(!memcmp(before+i*28+20,ram+v+20,8));
        else {
            /* The existing atlas endpoint correction remains active once. */
            float inset=0.5f/512,edge=511.0f/512;
            assert(fabsf(MEMF(v+20)-(i&1?edge-inset:inset))<0.0000001f);
            assert(fabsf(MEMF(v+24)-(i&2?edge-inset:inset))<0.0000001f);
        }
    }
    if(!fullscreen)assert(fabsf((MEMF(0x401c)-MEMF(0x4000))*aspect-width*(4.0f/3.0f))<0.0001f);
}
static void check_original_backdrop_tiles(void) {
    /* 112B74 registers the original two-sprite background list at 3B2480.
     * 1463D7 can queue its original record or the inline draw-state copy.
     * The saved 145A39 manager identifies both without inspecting pixels. */
    for(unsigned copied=0;copied<2;copied++)for(unsigned tile=0;tile<2;tile++)for(unsigned padded=0;padded<2;padded++) {
        memset(ram,0,sizeof ram);g_esi=copied?0x00552F54:0x003B2480+tile*0x3c;
        MEM32(g_esp)=0x0014617A;MEM32(g_esp+4)=7;MEM32(g_esp+8)=4;
        MEM32(g_esp+12)=0x4000;MEM32(g_esp+16)=28;
        MEM32(g_esp+0x5c)=0x6000;MEM32(0x6004)=0x003B2480;
        unsigned texture_width=tile && !padded?128:512;
        MEM32(g_esp+0x13c)=texture_width;MEM32(g_esp+0x140)=512;
        float left=tile?512:0,width=tile?128:512;
        for(unsigned i=0;i<4;i++) {
            unsigned v=0x4000+i*28;
            MEMF(v)=left+(i&1?width:0);MEMF(v+4)=i&2?480:0;
            MEM32(v+8)=0x3f000000+i;MEM32(v+12)=0x3f800000;MEM32(v+16)=0xff123400+i;
            MEMF(v+20)=i&1?(width-1)/(texture_width-1):0;MEMF(v+24)=i&2?479.0f/511:0;
        }
        unsigned char before[112];memcpy(before,ram+0x4000,sizeof before);
        prepare_original_ui_quad();
        /* Each original tile must keep its screen extent and UVs so the
         * complete background reaches both selected-aspect screen edges. */
        assert(!memcmp(before,ram+0x4000,sizeof before));
    }
    g_esi=0x2000;
}
static void check_original_title_tiles(void) {
    /* The first two original title records share a list with the menu UI.
     * Exercise both the original table and the queue's inline sprite copy. */
    for(unsigned copied=0;copied<2;copied++)for(unsigned tile=0;tile<2;tile++) {
        memset(ram,0,sizeof ram);g_esi=copied?0x00552F54:0x00530408+tile*0x3c;
        MEM32(g_esp+0x5c)=0x6000;MEM32(0x6004)=0x00530408;
        MEM32(g_esi+4)=tile;MEM32(g_esi+0x14)=0;MEM32(g_esi+0x18)=0;
        MEM32(g_esi+0x1c)=tile?128:512;MEM32(g_esi+0x20)=480;
        MEM32(g_esp)=0x0014617A;MEM32(g_esp+4)=7;MEM32(g_esp+8)=4;
        MEM32(g_esp+12)=0x4000;MEM32(g_esp+16)=28;
        MEM32(g_esp+0x13c)=512;MEM32(g_esp+0x140)=512;
        for(unsigned i=0;i<4;i++) {
            unsigned v=0x4000+i*28;
            MEMF(v)=(tile?512:0)+(i&1?(tile?128:512):0);MEMF(v+4)=i&2?480:0;
            MEM32(v+8)=0x3f000000+i;MEM32(v+12)=0x3f800000;MEM32(v+16)=0xffabcdef;
            MEMF(v+20)=i&1?(tile?127:511)/511.0f:0;MEMF(v+24)=i&2?479/511.0f:0;
        }
        unsigned char before[112];memcpy(before,ram+0x4000,sizeof before);
        prepare_original_ui_quad();
        assert(!memcmp(before,ram+0x4000,sizeof before));
    }
    /* Press Start remains proportional and uses the accepted title anchor. */
    memset(ram,0,sizeof ram);g_esi=0x00530480;
    MEM32(g_esp+0x5c)=0x6000;MEM32(0x6004)=0x00530408;
    MEM32(g_esi+4)=1;MEM32(g_esi+0x14)=128;MEM32(g_esi+0x18)=64;
    MEM32(g_esi+0x1c)=256;MEM32(g_esi+0x20)=24;
    MEM32(g_esp)=0x0014617A;MEM32(g_esp+4)=7;MEM32(g_esp+8)=4;
    MEM32(g_esp+12)=0x4000;MEM32(g_esp+16)=28;
    MEM32(g_esp+0x13c)=512;MEM32(g_esp+0x140)=512;
    for(unsigned i=0;i<4;i++) {
        unsigned v=0x4000+i*28;MEMF(v)=65+(i&1?256:0);MEMF(v+4)=250+(i&2?24:0);
        MEMF(v+20)=(128+(i&1?255:0))/511.0f;MEMF(v+24)=(64+(i&2?23:0))/511.0f;
    }
    prepare_original_ui_quad();
    for(unsigned i=0;i<4;i++) {
        float original=65+(i&1?256:0),factor=(4.0f/3.0f)/aspect;
        assert(MEMF(0x4000+i*28)==NINE_TITLE_UI_ORIGIN+(original-NINE_TITLE_UI_ORIGIN)*factor);
        assert(MEMF(0x4004+i*28)==250+(i&2?24:0));
    }
}
static void check_original_endchar_portrait(void) {
    /* Original 3358EC: texture0, sourceX128,Y0,W360,H512. */
    memset(ram,0,sizeof ram);g_esi=0x003B21E8;
    MEM32(g_esp)=0x0014617A;MEM32(g_esp+4)=7;MEM32(g_esp+8)=4;
    MEM32(g_esp+12)=0x4000;MEM32(g_esp+16)=28;
    MEM32(g_esp+0x5c)=0x6000;MEM32(0x6004)=0x003B21E8;
    MEM32(g_esp+0x13c)=512;MEM32(g_esp+0x140)=512;
    for(unsigned i=0;i<4;i++) {
        unsigned v=0x4000+i*28;MEMF(v)=140+(i&1?360:0);MEMF(v+4)=-32+(i&2?512:0);
        MEM32(v+8)=0x3f000000+i;MEM32(v+12)=0x3f800000;MEM32(v+16)=0xff123400+i;
        MEMF(v+20)=(i&1?487.0f:128.0f)/511;MEMF(v+24)=i&2?1:0;
    }
    unsigned char before[112];memcpy(before,ram+0x4000,sizeof before);
    prepare_original_ui_quad();
    assert(fabsf((MEMF(0x401c)-MEMF(0x4000))*aspect-360*(4.0f/3.0f))<0.0001f);
    for(unsigned i=0;i<4;i++) {
        unsigned v=0x4000+i*28;
        assert(!memcmp(before+i*28+4,ram+v+4,16));
        assert(fabsf(MEMF(v+20)-(i&1?486.5f:128.5f)/512)<0.0000001f);
        assert(fabsf(MEMF(v+24)-(i&2?510.5f:0.5f)/512)<0.0000001f);
    }
    g_esi=0x2000;
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
static void check_status_preview(void) {
    const uint32_t original[6]={60,84,168,184,0,0x3f800000};
    float factor=(4.0f/3.0f)/aspect;
    for(unsigned frame=0;frame<120;frame++) {
        memset(ram,0,sizeof ram);MEM32(g_esp)=0x001427E9;MEM32(g_esp+4)=0x4000;
        memcpy(ram+0x4000,original,sizeof original);
        prepare_original_preview_viewport();
        float effect_center=MEM32(0x4000)+MEM32(0x4008)*0.5f;
        /* Body c36 and fixed-function animated effects share this preview. */
        MEM32(g_esp)=0x001427AE;MEM32(g_esp+4)=0x5000;
        MEMF(0x5000)=144;MEMF(0x5004)=176;
        MEMF(g_esp+8)=184.0f/168;MEMF(g_esp+12)=84;
        prepare_original_preview_projection();
        assert(fabsf(effect_center-MEMF(0x5000))<0.001f);
        assert(fabsf(MEM32(0x4008)-168*factor)<0.001f);
        assert(MEM32(0x4004)==84 && !memcmp(ram+0x400c,original+3,12));
        if(aspect==4.0f/3.0f)assert(!memcmp(ram+0x4000,original,sizeof original));
        MEM32(g_esp)=0x00142A5A;MEM32(g_esp+4)=0x4000;
        uint32_t scene[6]={0,0,640,480,0,0x3f800000};
        memcpy(ram+0x4000,scene,sizeof scene);prepare_original_preview_viewport();
        assert(!memcmp(ram+0x4000,scene,sizeof scene));
    }
}
static void check_projection(uint32_t caller,int active,float x,float y,float ratio,float scale) {
    memset(ram,0,sizeof ram);MEM32(g_esp)=caller;MEM32(g_esp+4)=0x4000;
    MEMF(g_esp+8)=ratio;MEMF(g_esp+12)=scale;
    MEMF(0x4000)=x;MEMF(0x4004)=y;MEM32(0x4008)=0x3f000000;
    prepare_original_preview_projection();
    float factor=(4.0f/3.0f)/aspect;
    int appearance=active && caller!=0x001427AE && caller!=0x0010DFFF;
    if(!active || aspect==4.0f/3.0f)assert(MEMF(0x4000)==x);
    else assert(MEMF(0x4000)==320+(x-320)*factor);
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
static void check_sword_preview(void) {
    memset(ram,0,sizeof ram);MEM32(g_esp)=0x0010E033;MEM32(g_esp+4)=0x00333FD0;
    uint32_t original[6]={200,224,112,112,0,0x3f800000};
    memcpy(ram+0x00333FD0,original,sizeof original);
    float selected=aspect;
    /* Static table survives many draws and a display aspect change. */
    for(unsigned mode=0;mode<3;mode++) {
        aspect=mode==1?4.0f/3.0f:selected;
        for(unsigned frame=0;frame<120;frame++) {
            prepare_original_preview_viewport();
            float factor=(4.0f/3.0f)/aspect;
            assert(MEM32(0x00333FD0)==(uint32_t)(320+(200-320)*factor+0.5f));
            assert(MEM32(0x00333FD8)==(uint32_t)(112*factor+0.5f));
            assert(MEM32(0x00333FD4)==224 && !memcmp(ram+0x00333FDC,original+3,12));
            /* Equal physical width/height and centered on the model's X=256. */
            assert(fabsf(MEM32(0x00333FD8)*aspect*480/640-112)<0.001f);
            assert(MEM32(0x00333FD0)+MEM32(0x00333FD8)/2.0f==320+(256-320)*factor);
        }
    }
    aspect=selected;MEM32(g_esp+4)=0x00333FE8;
    uint32_t full[6]={0,0,640,480,0,0x3f800000};memcpy(ram+0x00333FE8,full,sizeof full);
    prepare_original_preview_viewport();assert(!memcmp(ram+0x00333FE8,full,sizeof full));
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

static void check_font_up(void) {
    for(unsigned language=0;language<2;language++)for(unsigned style=0;style<2;style++)for(unsigned scale=1;scale<=3;scale+=2) {
        memset(ram,0,sizeof ram);g_ebx=0x2000;font_chinese=language;font_render_scale=scale;
        MEM32(g_ebx+0x6c)=0x4000;MEM32(g_ebx+0x64)=style?2:0;
        MEM32(g_esp)=0x00150ee2;MEM32(g_esp+4)=6;MEM32(g_esp+8)=4;MEM32(g_esp+12)=0x4000;MEM32(g_esp+16)=28;
        for(unsigned i=0;i<4;i++) {
            unsigned v=0x4000+i*28;
            MEMF(v)=100+(i&1?16:0);MEMF(v+4)=200+(i&2?16:0);
            MEM32(v+8)=0x3f000000+i;MEM32(v+12)=0x3f800000;MEM32(v+16)=0xff123400+i;
            MEMF(v+20)=(float)(i&1?48:32)/512;MEMF(v+24)=(float)(i&2?80:64)/512;
        }
        unsigned char before[112];memcpy(before,ram+0x4000,sizeof before);
        prepare_original_ui_quad();
        float factor=(4.0f/3.0f)/aspect;
        unsigned selected_scale=!style && language?scale:1;
        float inset=0.5f/(512*selected_scale),trim=(style?1.0f:0.0f)/512+inset;
        for(unsigned i=0;i<4;i++) {
            unsigned v=0x4000+i*28;
            float x;memcpy(&x,before+i*28,4);
            assert(MEMF(v)==320+(x-320)*factor);
            assert(!memcmp(before+i*28+4,ram+v+4,16));
            float u=(float)(i&1?48:32)/512,w=(float)(i&2?80:64)/512;
            float expected_u=32.0f/512+inset+(u-32.0f/512)*(16.0f/512-inset-trim)/(16.0f/512);
            float expected_v=64.0f/512+inset+(w-64.0f/512)*(16.0f/512-inset-trim)/(16.0f/512);
            assert(fabsf(MEMF(v+20)-expected_u)<0.0000001f && fabsf(MEMF(v+24)-expected_v)<0.0000001f);
        }
    }
    font_chinese=0;font_render_scale=2;
}

int main(int argc,char **argv) {
    assert(argc==2);aspect=atoi(argv[1])==4?4.0f/3.0f:16.0f/9.0f;
    check(0,0,640,480,1);check(-0.5f,-0.5f,640,480,1);
    check(12,285,224,40,0);check(12,0,100,480,0);
    check_rgb(0,1);check_rgb(0.5f,1);check_rgb(32,1);check_rgb(80,1);check_rgb(127.5f,1);check_rgb(80,0);
    check_portrait_strip(352,164,272);check_portrait_strip(171,217,216);check_portrait_strip(202,163,80);
    check_settled_portrait(320,480,0);check_settled_portrait(512,512,0);
    check_settled_portrait(320,479,0);check_settled_portrait(640,480,1);
    check_settled_portrait(360,512,0);check_original_backdrop_tiles();check_original_title_tiles();
    check_original_backdrop_strip();
    check_original_endchar_portrait();
    check_dialogue_connector();check_speech_anchor();
    check_item_icons();check_font_up();
    check_window_transition();check_sword_preview();check_status_preview();
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
    /* Random encounter gauge frame from the original 110106 stack quad.
     * Both texture branches share the green fill and menu X coordinates. */
    const float gauge_x[]={336,588,588,336},gauge_y[]={248,248,264,264};
    check_menu_color(0x001102DC,7,4,28,gauge_x,gauge_y,1);
    check_menu_color(0x00110305,7,4,28,gauge_x,gauge_y,1);
    check_menu_color(0x001102DB,7,4,28,gauge_x,gauge_y,0);
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
    check_projection(0x0010DFFF,1,256,280,1,112);
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


def test_fusion_disc_anchor(tmp_path):
    """Authored disc positions survive widescreen while model geometry stays proportional."""
    import json
    root = Path(__file__).resolve().parents[2]
    recipe = json.loads((root / 'config/codegen.json').read_text())
    row = next(item for item in recipe['functions'] if item['address'] == 0x368F3)
    edit = next(item for item in row['edits'] if 'disc anchors' in item[2])
    production = edit[2]
    source = r'''
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <assert.h>
static unsigned char ram[4096];
#define MEM32(a) (*(uint32_t *)(ram+(a)))
#define MEMF(a) (*(float *)(ram+(a)))
static float aspect;
float smt9_game_aspect(void){return aspect;}
static void place(void){uint32_t ecx=512,ebp=256;
'''+production+r'''
}
int main(void){
    const float anchors[]={0,-268.16500854f,268.16500854f,0};
    const float aspects[]={4.0f/3.0f,16.0f/9.0f,21.0f/9.0f,4.0f/3.0f};
    for(unsigned a=0;a<4;a++)for(unsigned slot=0;slot<4;slot++)for(unsigned frame=0;frame<120;frame++){
        aspect=aspects[a];
        const float authored[3]={anchors[slot],2.5f,-154.825f};
        memcpy(ram+512+0xB0,authored,12);
        place();
        unsigned consumer=MEM32(256-44);
        assert(consumer==512+0xB0);
        assert(!memcmp(ram+consumer+4,authored+1,8));
        float factor=(4.0f/3.0f)/aspect;
        float projected=320+MEMF(consumer)*factor*1361.107666f/2857.327881f;
        float original=320+authored[0]*1361.107666f/2857.327881f;
        assert(fabsf(projected-original)<0.0001f);
        /* Two vertices retain their normal projected separation; only the anchor moves. */
        float left=320+(MEMF(consumer)-20)*factor;
        float right=320+(MEMF(consumer)+20)*factor;
        assert(fabsf((right-left)*aspect-40*(4.0f/3.0f))<0.0001f);
        if(a==0 || a==3)assert(!memcmp(ram+consumer,authored,12));
    }
    return 0;
}
'''
    c=tmp_path / 'anchor.c';exe=tmp_path / 'anchor.exe';c.write_text(source)
    result=subprocess.run([shutil.which('cl'),'/nologo','/O2',str(c),'/Fe:'+str(exe)],cwd=tmp_path,capture_output=True,text=True)
    assert result.returncode==0,result.stdout+result.stderr
    subprocess.run([str(exe)],check=True,cwd=tmp_path)
