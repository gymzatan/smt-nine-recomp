#include <stdio.h>
#include <stdlib.h>
#include "recomp_types.h"
#include "xbox_memory_layout.h"
#include <math.h>
#include "kernel/kernel.h"
#include "font_assets.h"

extern unsigned smt9_game_refresh_hz(void);
extern uint64_t xbox_ReadTimeStampCounter(void);
extern void guest_cpu_join(void);
void smt9_game_frame_begin(void)
{
    uint64_t previous=(uint64_t)MEM32(0x00557228u)|((uint64_t)MEM32(0x0055722Cu)<<32);
    uint64_t period=733333333ull/smt9_game_refresh_hz();
    uint64_t deadline=previous-previous%period+period;
    uint64_t now=xbox_ReadTimeStampCounter();
    if(now>=deadline)return;
    guest_cpu_part();
    do{
        DWORD milliseconds=(DWORD)((deadline-now)*1000ull/733333333ull);
        Sleep(milliseconds);
        now=xbox_ReadTimeStampCounter();
    }while(now<deadline);
    guest_cpu_join();
}
float smt9_game_frame_interval(void)
{
    /* 16117F has already measured this interval before selecting its step.
     * Movie decoding can present below the selected refresh rate. Keep the
     * original movie script synchronized with that elapsed game clock. */
    if(MEM32(0x005548B0u))return MEMF(0x005571F8u);
    return 1.0f/smt9_game_refresh_hz();
}

extern float smt9_game_aspect(void);
float smt9_battle_camera_constant(float original,unsigned role)
{
    unsigned hz=smt9_game_refresh_hz();
    if(hz==30)return original;
    float step=30.0f/hz;
    switch(role){
    case 0:case 2:return original*step;
    case 1:{
        float denominator=MEMF(0x0031B6E4u);
        return denominator*step*(1.0f-powf(1.0f-original/denominator,step));
    }
    case 3:return 1.0f/(1.0f-powf(1.0f-1.0f/original,step));
    default:fprintf(stderr,"[STOP] original battle camera constant role\n");exit(5);
    }
}
extern int smt9_is_japanese(void);
extern int smt9_is_chinese(void);
extern unsigned smt9_native_font_pages(void);
extern unsigned d3d8_RenderScale(void);
static void prepare_original_ui_quad(void)
{
    uint32_t caller=MEM32(g_esp),buffer=0;int texture=0,extra=1,video=0,background=0,texel_endpoints=0;unsigned font_scale=1,vertices=4;
    float texture_width=512,texture_height=512;
    if(caller==0x000F4F82u && MEM32(g_esp+48)==0x00106885u){
        /* 1066F0's timer fill uses F4F90's original four 20-byte vertices. */
        buffer=MEM32(MEM32(g_esi+8)+4)|0x80000000u;
        if(MEM32(g_esp+4)!=6 || MEM32(g_esp+12)!=4){fprintf(stderr,"[STOP] original timer fill contract\n");exit(5);}
        float factor=(4.0f/3.0f)/smt9_game_aspect();
        for(unsigned i=0;i<4;i++){
            uint32_t vertex=buffer+i*20;
            MEMF(vertex)=320+(MEMF(vertex)-320)*factor;
        }
        return;
    }
    if(caller==0x001C4A3Bu){
        /* The shared color-quad entry submits command highlights and fades. */
        if(smt9_game_aspect()==4.0f/3.0f)return;
        if(MEM32(g_esp+4)!=8 || MEM32(g_esp+8)!=0 || MEM32(g_esp+12)!=4){fprintf(stderr,"[STOP] original command highlight contract\n");exit(5);}
        buffer=MEM32(MEM32(g_esi+4)+4)|0x80000000u;
        float xmin=INFINITY,xmax=-INFINITY,ymin=INFINITY,ymax=-INFINITY;
        for(unsigned i=0;i<4;i++){
            uint32_t vertex=buffer+i*20;float x=MEMF(vertex),y=MEMF(vertex+4);
            if(x<xmin)xmin=x;if(x>xmax)xmax=x;if(y<ymin)ymin=y;if(y>ymax)ymax=y;
        }
        if(xmax-xmin>=640 && ymax-ymin>=480)return;
        float factor=(4.0f/3.0f)/smt9_game_aspect();
        for(unsigned i=0;i<4;i++){
            uint32_t vertex=buffer+i*20;
            MEMF(vertex)=320+(MEMF(vertex)-320)*factor;
        }
        return;
    }
    if(caller==0x00188D78u || caller==0x00188E20u || caller==0x00188EC8u || caller==0x00188F70u){
        /* Original 188C87 battle information lines share the font's UI mapping. */
        buffer=MEM32(MEM32(g_ebx+0x300)+4)|0x80000000u;
        float factor=(4.0f/3.0f)/smt9_game_aspect();
        for(unsigned i=0;i<2;i++){
            uint32_t vertex=buffer+i*20;
            MEMF(vertex)=320+(MEMF(vertex)-320)*factor;
        }
        return;
    }
    if(caller==0x000F559Cu){
        /* EF1B0's original 80x30 backdrop grid shares EEF90's source and pan.
         * Match its X mapping to the expanded backdrop UVs before submission. */
        buffer=MEM32(MEM32(g_esi+8)+4)|0x80000000u;
        vertices=MEM32(g_esp+12);
        float factor=(4.0f/3.0f)/smt9_game_aspect();
        for(unsigned i=0;i<vertices;i++){
            uint32_t vertex=buffer+i*28;
            MEMF(vertex)=320+(MEMF(vertex)-320)*factor;
        }
        return;
    }
    switch(caller){
    case 0x001C4888u:
        /* 1958EE rebuilds command panels with 1C3D5F before 195FD2 draws them.
         * The original 1C4800 frame retains this at its saved stack slot. */
        if(smt9_game_aspect()==4.0f/3.0f)return;
        if(MEM32(g_esp+4)!=8 || MEM32(g_esp+8)!=0 || MEM32(g_esp+12)!=4){fprintf(stderr,"[STOP] original command panel contract\n");exit(5);}
        buffer=MEM32(MEM32(MEM32(g_esp+0x1c)+8)+4)|0x80000000u;
        break;
    case 0x001CCD3Bu:
        /* CBBEF rebuilds its four screen-space sprite vertices via CB94F
         * before CCCFE submits them. The CA7C5 world-mesh caller is separate. */
        if(MEM32(g_esp+20)!=0x001CBD5Cu)return;
        if(MEM32(g_esi+0x50)!=0x144u || MEM32(g_esi+0x54)!=28 || MEM32(g_esp+4)!=6){fprintf(stderr,"[STOP] original RTS sprite contract format=%X stride=%u primitive=%u count=%u parent=%X object=%X\n",MEM32(g_esi+0x50),MEM32(g_esi+0x54),MEM32(g_esp+4),MEM32(g_esp+12),MEM32(g_esp+20),g_esi);exit(5);}
        if(smt9_game_aspect()==4.0f/3.0f)return;
        buffer=MEM32(MEM32(g_esi+0x48)+4)|0x80000000u;
        break;
    case 0x0014617Au:
        /* 145A39 submits its stack quad through the original DrawVerticesUP.
         * Its GetLevelDesc result and UVs use width/height minus one. */
        buffer=MEM32(g_esp+12);texture=1;extra=0;texel_endpoints=1;
        texture_width=(float)MEM32(g_esp+0x13c);texture_height=(float)MEM32(g_esp+0x140);
        if(MEM32(g_esp+4)!=7 || MEM32(g_esp+16)!=28 || texture_width<=1 || texture_height<=1){fprintf(stderr,"[STOP] original sprite quad contract\n");exit(5);}
        break;
    case 0x000F49CEu:
        /* EEF90 draws the original field backdrop and its transition image
         * through F46F0. Its completed quad uses the original Lock result. */
        background=MEM32(g_esp+0x44)==0x000EF0C1u || MEM32(g_esp+0x44)==0x000EF1A5u;
        buffer=MEM32(MEM32(g_esi+8)+4)|0x80000000u;break;
    case 0x000118CAu:vertices=MEM32(g_esp+12);if(vertices!=MEM32(g_ebx+0x44)*4 || vertices>4096){fprintf(stderr,"[STOP] original UI batch count\n");exit(5);}buffer=MEM32(g_ebx+0x40)-vertices*28;texture=1;break;
    case 0x00150EE2u:buffer=MEM32(g_ebx+0x6c);texture=1;extra=MEM32(g_ebx+0x64)==2;if(!extra && smt9_is_chinese())font_scale=d3d8_RenderScale();break;
    case 0x00148225u:buffer=MEM32(g_ebx+0x18);break;
    case 0x0014850Bu:buffer=MEM32(g_ebx+0x18);texture=1;texture_width=MEMF(0x0031B57Cu);texture_height=MEMF(0x00333FCCu);break;
    case 0x0015390Bu:buffer=MEM32(g_ebx+0x10);texture=1;break;
    case 0x00157DEEu:buffer=MEM32(g_ebx+0x8b0);texture=1;break;
    case 0x0015D750u:buffer=MEM32(g_esi+0x0c);video=1;break;
    default:return;
    }
    if(!buffer || !vertices || MEM32(g_esp+(texel_endpoints?8:12))!=vertices){fprintf(stderr,"[STOP] original UI quad contract at %08X\n",caller);exit(5);}
    for(unsigned quad=0;quad<vertices/4;quad++,buffer+=4*28){
    float xmin=INFINITY,xmax=-INFINITY,ymin=INFINITY,ymax=-INFINITY;
    float umin=INFINITY,umax=-INFINITY,vmin=INFINITY,vmax=-INFINITY;
    for(unsigned i=0;i<4;i++){
        uint32_t vertex=buffer+i*28;float x=MEMF(vertex),y=MEMF(vertex+4);
        if(x<xmin)xmin=x;if(x>xmax)xmax=x;if(y<ymin)ymin=y;if(y>ymax)ymax=y;
        float u=MEMF(vertex+20),v=MEMF(vertex+24);
        if(u<umin)umin=u;if(u>umax)umax=u;if(v<vmin)vmin=v;if(v>vmax)vmax=v;
    }
    /* These exact callers are the original font, panel and icon producers.
     * Full-screen fills cover the selected aspect; other UI keeps its shape. */
    int fullscreen=ymax-ymin>=480;
    /* Keep 145A39's tiled full-screen backdrop exactly as requested.
     * Only its panels, portraits and icons need atlas edge correction. */
    if(texel_endpoints && !fullscreen){
        float scale_u=(texture_width-1)/texture_width,scale_v=(texture_height-1)/texture_height;
        for(unsigned i=0;i<4;i++){
            uint32_t vertex=buffer+i*28;
            MEMF(vertex+20)*=scale_u;MEMF(vertex+24)*=scale_v;
        }
        umin*=scale_u;umax*=scale_u;vmin*=scale_v;vmax*=scale_v;
    }
    float factor=(4.0f/3.0f)/smt9_game_aspect();
    /* The original phase caption starts beyond the right edge at X=640.
     * Keep that font position anchored to the right edge when widening UI. */
    float ui_origin=caller==0x000118CAu && xmin>=640 ? 640 : 320;
    /* 215220 registers the original 60-byte title sprite table. These are
     * its Press Start and three menu labels/highlights, before the intro art. */
    if(texel_endpoints && g_esi>=0x00530480u && g_esi<=0x00530624u &&
       (g_esi-0x00530408u)%0x3Cu==0)ui_origin=NINE_TITLE_UI_ORIGIN;
    float inset_u=0.5f/(texture_width*font_scale),trim_u=extra/texture_width+inset_u;
    float inset_v=0.5f/(texture_height*font_scale),trim_v=extra/texture_height+inset_v;
    for(unsigned i=0;i<4;i++){
        uint32_t vertex=buffer+i*28;
        if(background)MEMF(vertex+20)=(umin+umax)*0.5f+(MEMF(vertex+20)-(umin+umax)*0.5f)/factor;
        else if(!fullscreen || video)MEMF(vertex)=ui_origin+(MEMF(vertex)-ui_origin)*factor;
        if(fullscreen && texture && !texel_endpoints)MEMF(vertex+4)=240+(MEMF(vertex+4)-240)/factor;
        if(texture && !fullscreen){
            if(umax-umin>inset_u+trim_u)MEMF(vertex+20)=umin+inset_u+(MEMF(vertex+20)-umin)*(umax-umin-inset_u-trim_u)/(umax-umin);
            if(vmax-vmin>inset_v+trim_v)MEMF(vertex+24)=vmin+inset_v+(MEMF(vertex+24)-vmin)*(vmax-vmin-inset_v-trim_v)/(vmax-vmin);
        }
    }
    static uint32_t seen[7];static unsigned count;unsigned n=0;for(;n<count;n++)if(seen[n]==caller)break;
    if(n==count && count<7){seen[count++]=caller;fprintf(stderr,"[UI PRODUCER] caller=%08X buffer=%08X size=%gx%g uv=%g,%g,%g,%g fullscreen=%d\n",caller,buffer,xmax-xmin,ymax-ymin,umin,vmin,umax,vmax,fullscreen);}
    }
}
static void prepare_original_preview_viewport(void)
{
    uint32_t caller=MEM32(g_esp);
    if(caller!=0x000F826Fu && caller!=0x000F8709u)return;
    if(smt9_game_aspect()==4.0f/3.0f)return;
    /* F8070/F84E0 recreate the male/female preview viewport each draw.
     * Use the same horizontal mapping as its surrounding original UI. */
    uint32_t viewport=MEM32(g_esp+4);
    if(MEM32(viewport)!=208 || MEM32(viewport+4)!=88 ||
       MEM32(viewport+8)!=144 || MEM32(viewport+12)!=288){
        fprintf(stderr,"[STOP] original appearance preview viewport contract\n");exit(5);
    }
    float factor=(4.0f/3.0f)/smt9_game_aspect();
    MEM32(viewport)=(uint32_t)floorf(320+(208-320)*factor+0.5f);
    MEM32(viewport+8)=(uint32_t)floorf(144*factor+0.5f);
    static int seen;
    if(!seen){seen=1;fprintf(stderr,"[UI PRODUCER] appearance preview caller=%08X viewport=%u,%u,%u,%u\n",caller,MEM32(viewport),MEM32(viewport+4),MEM32(viewport+8),MEM32(viewport+12));}
}
static void prepare_original_preview_projection(void)
{
    uint32_t caller=MEM32(g_esp);
    if(caller!=0x000F819Eu && caller!=0x000F8638u && caller!=0x001427AEu)return;
    if(smt9_game_aspect()==4.0f/3.0f)return;
    /* Original appearance and status producers set screen origin and X/Y
     * scale ratio before 16370 uploads c36. Keep their Y and depth intact. */
    uint32_t position=MEM32(g_esp+4);
    float factor=(4.0f/3.0f)/smt9_game_aspect();
    MEMF(position)=320+(MEMF(position)-320)*factor;
    MEMF(g_esp+8)*=factor;
    static unsigned seen;
    unsigned bit=caller==0x001427AEu?4:caller==0x000F8638u?2:1;
    if(!(seen&bit)){seen|=bit;fprintf(stderr,"[UI PRODUCER] preview projection caller=%08X origin=%g,%g ratio=%g scale=%g\n",caller,MEMF(position),MEMF(position+4),MEMF(g_esp+8),MEMF(g_esp+12));}
}
void recomp_call_arguments(uint32_t target)
{
    if(target==0x002EDE10u){prepare_original_preview_viewport();return;}
    if(target==0x002F03E0u || target==0x002F00D0u){prepare_original_ui_quad();return;}
    if(target==0x002218D0u && MEM32(g_esp)==0x000EE3B9u){
        /* Keep EE1E0's widescreen zoom inside the available backdrop width
         * before its pan bounds and both scene camera consumers run. */
        float factor=(4.0f/3.0f)/smt9_game_aspect();
        float minimum=2*MEMF(0x0031B58Cu)/(MEMF(0x0031B5A8u)*factor);
        if(factor<1 && MEMF(g_esi+0xd8)<minimum)MEMF(g_esi+0xd8)=minimum;
        return;
    }
    if(target==0x00016100u){
        prepare_original_preview_projection();
        /* 16370 uploads c36 from the original viewport set by 16100. */
        if(MEM32(g_esp)==0x000EE537u && MEM32(g_esp+8)==0x3F400000u){
            float factor=(4.0f/3.0f)/smt9_game_aspect();
            float zoom=MEMF(g_esi+0xd8),center=MEMF(0x0031B58Cu);
            float half=center/(zoom*factor),width=MEMF(0x0031B5A8u);
            float pan=MEMF(g_esi+0xcc);
            /* EE1E0's original pan bounds, enlarged for the wider source
             * window before its two camera consumers read the same pan. */
            if(pan<half)pan=half;if(pan>width-half)pan=width-half;
            MEMF(g_esi+0xcc)=pan;
            MEMF(MEM32(g_esp+4))=center-(pan-center)*zoom*factor;
            /* Original c36.x scaled both axes. The native field shader now
             * reads c36.y for Y; these inputs keep original 4:3 pixels exact. */
            MEMF(g_esp+8)=factor;
            MEMF(g_esp+12)*=0.75f;
        }
        return;
    }
    if(target==0x00220ED1u){
        /* EE5E0's fixed camera shares the field backdrop's pan origin. */
        if(MEM32(g_esp)==0x000EE7A2u)MEMF(g_esp+8)*=(4.0f/3.0f)/smt9_game_aspect();
        return;
    }
    /* The original perspective routine consumes FOV, aspect, near and far.
     * Preserve its calculation and square auxiliary viewports; adjust only
     * the game's 4:3 scene cameras, including the original RTS camera. */
    if(target==0x00012AB5u){
        if(!smt9_is_chinese() && MEM32(g_esp)==0x00150CC8u)MEM32(g_esp+12)=smt9_native_font_pages();
        return;
    }
    uint32_t aspect=MEM32(g_esp+12);
    static uint32_t callers[64],aspects[64];static unsigned count;
    unsigned i=0;for(;i<count;i++)if(callers[i]==MEM32(g_esp)&&aspects[i]==aspect)break;
    if(i==count && count<64){callers[count]=MEM32(g_esp);aspects[count++]=aspect;fprintf(stderr,"[PERSPECTIVE PRODUCER] caller=%08X output=%08X fov=%g aspect=%g bits=%08X\n",MEM32(g_esp),MEM32(g_esp+4),MEMF(g_esp+8),MEMF(g_esp+12),aspect);}
    /* EE1E0 maps the player's position into the backdrop's original pixel
     * coordinates. It is the pan controller, not the display projection. */
    if(aspect==0x3FAAAAABu && MEM32(g_esp)!=0x000EE3B9u)MEMF(g_esp+12)=smt9_game_aspect();
}

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    (void)xbox_va;
    return (recomp_func_t)0;
}

static void fail_call(const char *reason, uint32_t va)
{
    fprintf(stderr, "[STOP] %s at VA 0x%08X, ESP 0x%08X, calls %llu\n",
            reason, va, g_esp, (unsigned long long)g_icall_count);
    if ((uint64_t)g_esp + 3 * sizeof(uint32_t) <= xbox_GetMappedSize()) {
        const uint32_t *sp = (const uint32_t *)(
            (uintptr_t)xbox_GetMemoryOffset() + g_esp);
        fprintf(stderr, "  original stack: return=0x%08X arg0=0x%08X arg1=0x%08X\n",
                sp[0], sp[1], sp[2]);
    }
    for (unsigned i = 0; i < ICALL_TRACE_SIZE; ++i)
        fprintf(stderr, "  recent[%u] 0x%08X\n", i,
                g_icall_trace[(g_icall_trace_idx + i) & (ICALL_TRACE_SIZE - 1)]);
    fflush(stderr);
    exit(2);
}

void recomp_icall_fail_log(uint32_t va)
{
    fail_call("unresolved call", va);
}

void recomp_icall_not_code_log(uint32_t va)
{
    fail_call("call outside guest code", va);
}

void recomp_unimpl(const char *instruction, uint32_t va)
{
    fprintf(stderr, "[STOP] unsupported instruction `%s` at VA 0x%08X\n",
            instruction, va);
    fflush(stderr);
    exit(3);
}
