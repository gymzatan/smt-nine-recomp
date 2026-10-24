"""Exercise the original window consumer, host producer and framebuffer file contract."""
from pathlib import Path
import os
import shutil
import subprocess

import pytest


@pytest.fixture(scope='module')
def native_framebuffer(tmp_path_factory):
    root=Path(__file__).resolve().parents[2]
    work=tmp_path_factory.mktemp('framebuffer_present')
    compiler=shutil.which('cl');assert compiler, 'requires existing MSVC environment'
    source=r'''
#include <windows.h>
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
static int WINAPI traced_stretch(HDC,int,int,int,int,int,int,int,int,const void *,const BITMAPINFO *,UINT,DWORD);
#define StretchDIBits traced_stretch
#include "video/fb_present.c"
#undef StretchDIBits
static HANDLE in_gdi,producer_done,next_frame;
static volatile LONG intercept;
static unsigned producer_during_gdi;
static uint32_t first[32*24],second[17*13];
ptrdiff_t xbox_GetMemoryOffset(void) {return 0;}
int xbox_VideoIsPlaying(void) {return 0;}
void xbox_CheckpointDeviceEnter(unsigned device) {(void)device;}
void xbox_CheckpointDeviceLeave(unsigned device) {(void)device;}
void xbox_CheckpointDeviceCreating(unsigned device) {(void)device;}
void xbox_CheckpointDeviceCreated(unsigned device) {(void)device;}
int xbox_CheckpointBegin(DWORD timeout) {(void)timeout;return 1;}
void xbox_CheckpointEnd(void) {}
int xbox_CheckpointFrozen(void) {return 1;}
void xbox_CaptureDisplayState(uint32_t *state) {state[0]=123;state[1]=456;}
void xbox_RestoreDisplayState(const uint32_t *state) {assert(state[0]==123 && state[1]==456);}
static int WINAPI traced_stretch(HDC dc,int x,int y,int dw,int dh,int sx,int sy,int sw,int sh,const void *pixels,const BITMAPINFO *bi,UINT usage,DWORD op) {
    if(InterlockedCompareExchange(&intercept,0,1)==1) {
        assert(sw==32 && sh==24 && !memcmp(pixels,first,sizeof first));
        assert(GetStretchBltMode(dc)==HALFTONE && bi->bmiHeader.biBitCount==32);
        SetEvent(in_gdi);
        producer_during_gdi=WaitForSingleObject(producer_done,400)==WAIT_OBJECT_0;
        /* A resized producer must not invalidate or modify this GDI source. */
        assert(!memcmp(pixels,first,sizeof first));
    }else if(intercept==0 && sw==17 && sh==13 && next_frame) {
        assert(!memcmp(pixels,second,sizeof second));SetEvent(next_frame);
    }
    return StretchDIBits(dc,x,y,dw,dh,sx,sy,sw,sh,pixels,bi,usage,op);
}
static DWORD WINAPI produce(LPVOID unused) {
    (void)unused;assert(WaitForSingleObject(in_gdi,5000)==WAIT_OBJECT_0);
    xbox_FramebufferWindowPresentHost(second,17*4,17,13);SetEvent(producer_done);return 0;
}
static void init_host(void) {
    s_fb_running=1;s_frame_ready=CreateEventW(NULL,FALSE,FALSE,NULL);assert(s_frame_ready);
    for(unsigned i=0;i<32*24;i++)first[i]=0xff000000u|(i*7919u&0xffffffu);
    for(unsigned i=0;i<17*13;i++)second[i]=0xff000000u|(i*3571u&0xffffffu);
    uint32_t padded[24][36];memset(padded,0x5a,sizeof padded);
    for(unsigned y=0;y<24;y++)memcpy(padded[y],first+y*32,32*4);
    xbox_FramebufferWindowPresentHost(padded,36*4,32,24);
    assert(s_host_width==32 && s_host_height==24 && !memcmp(s_host_pixels,first,sizeof first));
}

static HANDLE native_changed;
static volatile LONG native_calls,native_successes,native_hold,native_captures,native_restores;
static HWND native_window;
static unsigned native_width,native_height;
static int native_display(void *window,unsigned width,unsigned height,const int32_t bounds[4]) {
    assert(window && width && height && bounds[2]>bounds[0] && bounds[3]>bounds[1]);
    native_window=(HWND)window;native_width=width;native_height=height;
    InterlockedIncrement(&native_calls);
    int ready=!InterlockedCompareExchange(&native_hold,0,0);
    if(ready)InterlockedIncrement(&native_successes);
    SetEvent(native_changed);return ready;
}
static int native_capture(void) {InterlockedIncrement(&native_captures);return 1;}
static int native_restore(const uint32_t *pixels,unsigned width,unsigned height) {
    assert(width==32 && height==24 && pixels && !memcmp(pixels,first,sizeof first));
    InterlockedIncrement(&native_restores);return 1;
}
static void native_close(void) {}
static const FramebufferNativeRenderer native_renderer={native_display,native_capture,native_restore,native_close};
static void native_wait_count(volatile LONG *counter,LONG minimum) {
    ULONGLONG end=GetTickCount64()+5000;
    while(InterlockedCompareExchange(counter,0,0)<minimum) {
        assert(GetTickCount64()<end);WaitForSingleObject(native_changed,20);
    }
}
static LONG native_idle(void) {
    /* Allow initial window paint/size messages to settle before the idle assertion. */
    Sleep(150);LONG calls=InterlockedCompareExchange(&native_calls,0,0);
    Sleep(150);assert(InterlockedCompareExchange(&native_calls,0,0)==calls);return calls;
}
static void check_native_readiness(void) {
    native_changed=CreateEventW(NULL,FALSE,FALSE,NULL);assert(native_changed);
    xbox_FramebufferWindowConfigure(96,72,4.0f/3.0f,0);
    xbox_FramebufferWindowSetRenderer(&native_renderer);
    HANDLE window=CreateThread(NULL,0,fb_thread,NULL,0,NULL);assert(window);
    native_wait_count(&native_successes,1);LONG calls=native_idle();
    for(unsigned i=0;i<32;i++)assert(PostMessageW(native_window,WM_MOUSEMOVE,0,MAKELPARAM(i,i)));
    Sleep(150);assert(native_calls==calls);
    /* A new frame is still pending after callback0; only the original timer may retry it. */
    InterlockedExchange(&native_hold,1);LONG success=native_successes;calls=native_calls;
    SetEvent(s_frame_ready);native_wait_count(&native_calls,calls+2);
    assert(native_successes==success);
    InterlockedExchange(&native_hold,0);native_wait_count(&native_successes,success+1);
    native_idle();
    calls=native_calls;assert(InvalidateRect(native_window,NULL,FALSE));
    native_wait_count(&native_calls,calls+1);native_idle();
    RECT old;assert(GetWindowRect(native_window,&old));calls=native_calls;
    assert(SetWindowPos(native_window,NULL,0,0,old.right-old.left+24,old.bottom-old.top+16,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE));
    native_wait_count(&native_calls,calls+1);native_idle();
    RECT client;assert(GetClientRect(native_window,&client));
    assert(native_width==(unsigned)client.right && native_height==(unsigned)client.bottom);
    calls=native_calls;assert(PostMessageW(native_window,WM_DISPLAYCHANGE,32,MAKELPARAM(640,480)));
    native_wait_count(&native_calls,calls+1);native_idle();
    FILE *saved=tmpfile();assert(saved && xbox_FramebufferSaveCheckpoint(saved));
    assert(native_captures==1);calls=native_calls;
    rewind(saved);assert(xbox_FramebufferLoadCheckpoint(saved));assert(native_restores==1);
    native_wait_count(&native_calls,calls+1);native_idle();fclose(saved);
    fprintf(stderr,"[FRAMEBUFFER] native completion idles;input stays idle;timer retries pending frame;paint/size/displaychange/checkpoint restore redisplay\n");
    InterlockedExchange(&s_fb_running,0);SetEvent(s_frame_ready);
    assert(WaitForSingleObject(window,5000)==WAIT_OBJECT_0);CloseHandle(window);CloseHandle(native_changed);
}

int main(int argc,char **argv) {
    assert(argc==2);init_host();
    if(!strcmp(argv[1],"native")) {
        check_native_readiness();
    }else if(!strcmp(argv[1],"sizing")) {
        HWND window=CreateWindowExW(0,L"STATIC",L"sizing test",WS_OVERLAPPEDWINDOW,100,100,656,519,NULL,NULL,GetModuleHandleW(NULL),NULL);
        assert(window && !IsWindowVisible(window));
        const unsigned sizes[][2]={{640,480},{1920,1080},{1000,700}};
        for(unsigned size=0;size<3;size++) {
            xbox_FramebufferWindowConfigure(sizes[size][0],sizes[size][1],size?4.0f/3.0f:16.0f/9.0f,1);
            for(unsigned edge=WMSZ_LEFT;edge<=WMSZ_BOTTOMRIGHT;edge++) {
                RECT before,client;GetWindowRect(window,&before);GetClientRect(window,&client);
                LONG bw=before.right-before.left-client.right,bh=before.bottom-before.top-client.bottom;
                assert(SetWindowPos(window,NULL,100,100,320+bw,240+bh,SWP_NOZORDER|SWP_NOACTIVATE));
                GetWindowRect(window,&before);GetClientRect(window,&client);
                RECT proposed=before;
                if(edge==WMSZ_LEFT || edge==WMSZ_TOPLEFT || edge==WMSZ_BOTTOMLEFT)proposed.left-=173;
                else if(edge!=WMSZ_TOP && edge!=WMSZ_BOTTOM)proposed.right+=173;
                if(edge==WMSZ_TOP || edge==WMSZ_TOPLEFT || edge==WMSZ_TOPRIGHT)proposed.top-=111;
                else if(edge!=WMSZ_LEFT && edge!=WMSZ_RIGHT)proposed.bottom+=111;
                assert(fb_wndproc(window,WM_SIZING,edge,(LPARAM)&proposed)==TRUE);
                LONG width=proposed.right-proposed.left-bw,height=proposed.bottom-proposed.top-bh;
                assert(llabs((int64_t)width*sizes[size][1]-(int64_t)height*sizes[size][0])<=(sizes[size][0]>sizes[size][1]?sizes[size][0]:sizes[size][1])/2);
                if(edge==WMSZ_LEFT || edge==WMSZ_TOPLEFT || edge==WMSZ_BOTTOMLEFT)assert(proposed.right==before.right);
                else assert(proposed.left==before.left);
                if(edge==WMSZ_TOP || edge==WMSZ_TOPLEFT || edge==WMSZ_TOPRIGHT)assert(proposed.bottom==before.bottom);
                else assert(proposed.top==before.top);
                assert(SetWindowPos(window,NULL,proposed.left,proposed.top,proposed.right-proposed.left,proposed.bottom-proposed.top,SWP_NOZORDER|SWP_NOACTIVATE));
                GetClientRect(window,&client);if(client.right!=width || client.bottom!=height)fprintf(stderr,"[SIZE DIAG] preset=%u edge=%u border=%ld,%ld expected=%ld,%ld actual=%ld,%ld\n",size,edge,bw,bh,width,height,client.right,client.bottom);assert(client.right==width && client.bottom==height);
            }
        }

        MINMAXINFO limits={0};limits.ptMaxTrackSize.x=1600;limits.ptMaxTrackSize.y=1000;
        fb_wndproc(window,WM_GETMINMAXINFO,0,(LPARAM)&limits);
        RECT frame={0};assert(AdjustWindowRectExForDpi(&frame,(DWORD)GetWindowLongPtrW(window,GWL_STYLE),FALSE,(DWORD)GetWindowLongPtrW(window,GWL_EXSTYLE),GetDpiForWindow(window)));
        LONG maxw=limits.ptMaxTrackSize.x-(frame.right-frame.left),maxh=limits.ptMaxTrackSize.y-(frame.bottom-frame.top);
        assert(limits.ptMaxTrackSize.x<=1600 && limits.ptMaxTrackSize.y<=1000);
        assert(llabs((int64_t)maxw*s_window_height-(int64_t)maxh*s_window_width)<=(s_window_width>s_window_height?s_window_width:s_window_height));
        RECT before={100,100,900,777},free_size=before;
        s_fixed_aspect=0;fb_wndproc(window,WM_SIZING,WMSZ_BOTTOMRIGHT,(LPARAM)&free_size);assert(!memcmp(&before,&free_size,sizeof before));
        s_fixed_aspect=1;s_fullscreen=1;fb_wndproc(window,WM_SIZING,WMSZ_BOTTOMRIGHT,(LPARAM)&free_size);assert(!memcmp(&before,&free_size,sizeof before));
        DestroyWindow(window);fb_close_frame_event();
        fprintf(stderr,"[FRAMEBUFFER] all eight resize handles preserve configured client ratio independently of game aspect;free resize/fullscreen unchanged\n");
    }else if(!strcmp(argv[1],"handoff")) {
        in_gdi=CreateEventW(NULL,TRUE,FALSE,NULL);producer_done=CreateEventW(NULL,TRUE,FALSE,NULL);next_frame=CreateEventW(NULL,TRUE,FALSE,NULL);
        assert(in_gdi && producer_done && next_frame);intercept=1;
        xbox_FramebufferWindowConfigure(96,72,4.0f/3.0f,0);
        HANDLE producer=CreateThread(NULL,0,produce,NULL,0,NULL),window=CreateThread(NULL,0,fb_thread,NULL,0,NULL);assert(producer && window);
        assert(WaitForSingleObject(producer,5000)==WAIT_OBJECT_0 && WaitForSingleObject(next_frame,5000)==WAIT_OBJECT_0);
        fprintf(stderr,"[FRAMEBUFFER] producer completed inside GDI=%u;original and resized bytes exact\n",producer_during_gdi);
        assert(producer_during_gdi==1);
        InterlockedExchange(&s_fb_running,0);SetEvent(s_frame_ready);
        assert(WaitForSingleObject(window,5000)==WAIT_OBJECT_0);
        CloseHandle(window);CloseHandle(producer);CloseHandle(in_gdi);CloseHandle(producer_done);CloseHandle(next_frame);
    }else {
        assert(!strcmp(argv[1],"checkpoint"));
        s_fb_width=4;s_fb_height=3;s_fb_pitch=16;s_fb_va=0x1234;s_present_idx=1;
        for(unsigned b=0;b<2;b++) {s_present[b]=malloc(48);for(unsigned i=0;i<12;i++)s_present[b][i]=0xff123400+b*32+i;}
        s_rgb=calloc(12,4);
        FILE *saved=tmpfile();assert(saved && xbox_FramebufferSaveCheckpoint(saved));
        long bytes=ftell(saved);void *original=malloc(bytes),*restored=malloc(bytes);assert(original && restored);
        rewind(saved);assert(fread(original,bytes,1,saved)==1);
        xbox_FramebufferWindowPresentHost(second,17*4,17,13);
        rewind(saved);assert(xbox_FramebufferLoadCheckpoint(saved));
        assert(s_host_width==32 && s_host_height==24 && !memcmp(s_host_pixels,first,sizeof first));
        assert(!memcmp(s_rgb,s_present[1],48));
        FILE *again=tmpfile();assert(again && xbox_FramebufferSaveCheckpoint(again) && ftell(again)==bytes);
        rewind(again);assert(fread(restored,bytes,1,again)==1 && !memcmp(original,restored,bytes));
        FILE *shot=tmpfile();assert(shot && fb_write_display(shot,0,0)==0);rewind(shot);
        BITMAPFILEHEADER header;BITMAPINFOHEADER info;assert(fread(&header,sizeof header,1,shot)==1 && fread(&info,sizeof info,1,shot)==1);
        assert(header.bfType==0x4d42 && info.biWidth==32 && info.biHeight==24);
        fclose(shot);fclose(again);fclose(saved);free(original);free(restored);
        fb_close_frame_event();free(s_rgb);free(s_present[0]);free(s_present[1]);
        fprintf(stderr,"[FRAMEBUFFER] original XBFRONT bytes,guest buffers,host resize restore and F8 BMP dimensions exact\n");
    }
    free(s_host_pixels);return 0;
}
'''
    (work/'framebuffer.c').write_text(source,encoding='utf-8')
    result=subprocess.run([compiler,'/nologo','/O2','/Gy','/MD','/utf-8',
        '/DWIN32_LEAN_AND_MEAN','/DNOMINMAX','/D_CRT_SECURE_NO_WARNINGS',
        '/I'+str(root/'src'),str(work/'framebuffer.c'),'/Fe:'+str(work/'framebuffer.exe'),
        '/link','/OPT:REF','kernel32.lib','user32.lib','gdi32.lib'],
        cwd=work,capture_output=True,text=True,errors='replace')
    assert result.returncode==0,result.stdout+result.stderr
    return work/'framebuffer.exe'


@pytest.mark.parametrize('mode',['handoff','checkpoint','sizing','native'])
def test_original_framebuffer_producer_and_file_contract(native_framebuffer,mode):
    result=subprocess.run([str(native_framebuffer),mode],env=os.environ.copy(),
        capture_output=True,text=True,errors='replace',timeout=15)
    print(result.stderr)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[FRAMEBUFFER]' in result.stderr,result.stdout+result.stderr
