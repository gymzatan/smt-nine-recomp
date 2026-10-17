/**
 * Show the guest framebuffer in a window.
 *
 * The title renders into its own framebuffer in guest RAM and tells the kernel
 * where it is through AvSetDisplayMode; on hardware the CRTC scans that memory
 * out. Nothing here scans anything out, so however much of the GPU is
 * implemented, none of it is observable. This is the other half: a window that
 * reads that memory and puts it on screen.
 *
 * Deliberately plain GDI rather than the D3D8 layer. The point is to display
 * whatever the guest actually wrote, so the fewer stages between guest memory
 * and the screen the better -- and it must keep working while the D3D8 layer
 * is busy with something else, such as the FMV player's own window.
 *
 * Off unless RECOMP_FB_WINDOW is set.
 */
#include <stdint.h>
#include "video_player.h"

#if defined(_WIN32)
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel/kernel.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern int xbox_VideoIsPlaying(void);

static volatile LONG s_fb_running;
static HANDLE s_frame_ready;
static volatile LONG s_checkpoint_command;
static int (*s_checkpoint_handler)(int,unsigned);
static wchar_t s_checkpoint_notice[512];
static ULONGLONG s_checkpoint_notice_until;
static wchar_t s_screenshot_directory[MAX_PATH];
static unsigned s_ui_language;
static int s_paused,s_fullscreen;
static LONG s_pause_command,s_screenshot_command;
static WINDOWPLACEMENT s_window_placement={sizeof(WINDOWPLACEMENT)};
static LONG_PTR s_window_style,s_window_exstyle;
void xbox_FramebufferWindowSetControls(const wchar_t *directory,unsigned language) {
    wcsncpy_s(s_screenshot_directory,MAX_PATH,directory,_TRUNCATE);
    s_ui_language=language<3?language:0;
}
static const wchar_t *fb_text(unsigned text) {
    static const wchar_t *const messages[][3]={
        {L"\u5df2\u6682\u505c \u2014 F9 \u7ee7\u7eed",L"\u4e00\u6642\u505c\u6b62\u4e2d \u2014 F9 \u3067\u518d\u958b",L"Paused \u2014 F9 to resume"},
        {L"\u5df2\u7ee7\u7eed",L"\u518d\u958b\u3057\u307e\u3057\u305f",L"Resumed"},
        {L"\u65e0\u6cd5\u6682\u505c\uff0c\u8bf7\u7a0d\u540e\u91cd\u8bd5",L"\u4e00\u6642\u505c\u6b62\u3067\u304d\u307e\u305b\u3093\u3067\u3057\u305f",L"Could not pause; try again shortly"},
        {L"\u622a\u56fe\u5df2\u4fdd\u5b58\uff1a",L"\u4fdd\u5b58\u5148\uff1a",L"Screenshot saved: "},
        {L"\u622a\u56fe\u4fdd\u5b58\u5931\u8d25",L"\u30b9\u30af\u30ea\u30fc\u30f3\u30b7\u30e7\u30c3\u30c8\u3092\u4fdd\u5b58\u3067\u304d\u307e\u305b\u3093\u3067\u3057\u305f",L"Screenshot could not be saved"},
        {L"\u8bf7\u5148\u6309 F9 \u7ee7\u7eed\uff0c\u518d\u4f7f\u7528\u5373\u65f6\u5b58\u8bfb\u6863",L"F9 \u3067\u518d\u958b\u3057\u3066\u304b\u3089\u30b9\u30c6\u30fc\u30c8\u3092\u64cd\u4f5c\u3057\u3066\u304f\u3060\u3055\u3044",L"Resume with F9 before saving or loading a state"}
    };
    return messages[text][s_ui_language];
}
static void fb_notice(const wchar_t *text) {
    wcsncpy_s(s_checkpoint_notice,512,text,_TRUNCATE);s_checkpoint_notice_until=GetTickCount64()+4000;
}
static void fb_toggle_fullscreen(HWND window) {
    if(!s_fullscreen) {
        MONITORINFO monitor={sizeof monitor};
        if(!GetWindowPlacement(window,&s_window_placement) || !GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor))return;
        s_window_style=GetWindowLongPtrW(window,GWL_STYLE);s_window_exstyle=GetWindowLongPtrW(window,GWL_EXSTYLE);
        SetWindowLongPtrW(window,GWL_STYLE,s_window_style & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(window,NULL,monitor.rcMonitor.left,monitor.rcMonitor.top,monitor.rcMonitor.right-monitor.rcMonitor.left,monitor.rcMonitor.bottom-monitor.rcMonitor.top,SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
        s_fullscreen=1;
    }else {
        SetWindowLongPtrW(window,GWL_STYLE,s_window_style);SetWindowLongPtrW(window,GWL_EXSTYLE,s_window_exstyle);
        SetWindowPlacement(window,&s_window_placement);
        SetWindowPos(window,NULL,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
        s_fullscreen=0;
    }
}

void xbox_FramebufferSetCheckpointHandler(int (*handler)(int,unsigned)) { s_checkpoint_handler=handler; }
static uint32_t      s_fb_va, s_fb_pitch, s_fb_width = 640, s_fb_height = 480;
static unsigned s_window_width=640,s_window_height=480;
static float s_display_aspect=4.0f/3.0f;
static SRWLOCK s_host_lock=SRWLOCK_INIT;
static uint32_t *s_host_pixels;
static unsigned s_host_width,s_host_height;
static void fb_close_frame_event(void)
{
    AcquireSRWLockExclusive(&s_host_lock);
    if(s_frame_ready)CloseHandle(s_frame_ready);
    s_frame_ready=NULL;
    ReleaseSRWLockExclusive(&s_host_lock);
}
void xbox_FramebufferWindowPresentHost(const void *pixels,unsigned pitch,unsigned width,unsigned height)
{
    if(!s_fb_running || !width || !height || pitch<width*4 || getenv("RECOMP_FB_VA"))return;
    AcquireSRWLockExclusive(&s_host_lock);
    if(width!=s_host_width || height!=s_host_height) {
        uint32_t *buffer=malloc((size_t)width*height*4);
        if(!buffer){ReleaseSRWLockExclusive(&s_host_lock);return;}
        free(s_host_pixels);s_host_pixels=buffer;s_host_width=width;s_host_height=height;
    }
    for(unsigned y=0;y<height;y++)memcpy(s_host_pixels+(size_t)y*width,(const uint8_t *)pixels+(size_t)y*pitch,(size_t)width*4);
    if(s_frame_ready)SetEvent(s_frame_ready);
    ReleaseSRWLockExclusive(&s_host_lock);
}
void xbox_FramebufferWindowConfigure(unsigned width,unsigned height,float aspect)
{
    s_window_width=width;s_window_height=height;s_display_aspect=aspect;
}
static RECT fb_display_bounds(HWND window)
{
    RECT client;GetClientRect(window,&client);
    LONG width=client.right,height=client.bottom;
    if(height>0 && (float)width/(float)height>s_display_aspect)width=(LONG)((float)height*s_display_aspect+0.5f);
    else if(s_display_aspect>0)height=(LONG)((float)width/s_display_aspect+0.5f);
    LONG x=(client.right-width)/2,y=(client.bottom-height)/2;
    return (RECT){x,y,x+width,y+height};
}
static uint32_t     *s_rgb;           /* converted 32-bit copy for GDI */

/* A finished frame, taken at the flip and shown until the next one.
 *
 * The window used to convert straight out of guest memory every 16 ms. Even
 * pointed at the buffer the title had just finished, that races the executor
 * drawing the next frame into the other one and, whenever the two swap, puts
 * a half-drawn image on the screen -- which is the flicker. Copying the
 * finished frame once per flip means the window never reads memory the
 * rasteriser is writing, so what it shows cannot be half of anything.
 *
 * Two buffers and an index, swapped after the copy completes, so the window
 * thread is never reading the one being filled. */
static uint32_t     *s_present[2];
static volatile LONG s_present_idx = -1;   /* -1 until the first flip */

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    /* RECOMP_FB_VA pins the window to one guest address instead of following
     * whichever surface is being drawn into. A black window cannot distinguish
     * "the read path is broken" from "the title rendered black", and pointing
     * it at memory known to have content settles that. */
    const char *pin = getenv("RECOMP_FB_VA");

    s_fb_va = pin ? (uint32_t)strtoul(pin, NULL, 0) : fb_va;
    if (pitch)
        s_fb_pitch = pitch;
}

/* Called by the pushbuffer executor when the title flips. */
void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch)
{
    const uint8_t *src;
    LONG next;
    uint32_t bpp, x, y;

    if (!s_fb_running || !fb_va || !pitch)
        return;
    if (getenv("RECOMP_FB_VA"))
        return;                       /* pinned: leave the old path alone */
    next = (s_present_idx == 0) ? 1 : 0;
    if (!s_present[next]) {
        s_present[next] = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height,
                                             4);
        if (!s_present[next])
            return;
    }
    bpp = pitch / s_fb_width;
    src = (const uint8_t *)((uintptr_t)fb_va + xbox_GetMemoryOffset());
    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * pitch;
        uint32_t *dst = s_present[next] + (size_t)y * s_fb_width;

        if (bpp == 4) {
            memcpy(dst, row, (size_t)s_fb_width * 4);
        } else if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (x = 0; x < s_fb_width; x++) {
                uint16_t v = p[x];
                uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
                dst[x] = (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)s_fb_width * 4);
        }
    }
    /* Published only once it is whole. */
    InterlockedExchange(&s_present_idx, next);
    AcquireSRWLockShared(&s_host_lock);
    if(s_frame_ready && !s_host_width)SetEvent(s_frame_ready);
    ReleaseSRWLockShared(&s_host_lock);
}

/* Which keys are down, for the pad stand-in in src/input.
 *
 * GetAsyncKeyState looked like the cheaper way to ask and does not work
 * here: it reads a state Wine keeps for the X server, and a guest process
 * drawing through GDI never sees it change. The window that has the focus
 * is the thing that receives the keys, so that is what has to remember
 * them.
 *
 * Reading this needs no lock. Each entry is written only by the window
 * thread and read only by the USB thread, one byte at a time, and a press
 * seen a frame late is indistinguishable from one made a frame later. */
static volatile unsigned char s_key_down[256];
static void fb_set_key_state(WPARAM key,LPARAM bits,int down)
{
    if(key==VK_SHIFT)key=MapVirtualKeyW((UINT)(bits>>16)&0xFF,MAPVK_VSC_TO_VK_EX);
    if(key<256)s_key_down[key]=(unsigned char)down;
    if(key==VK_LSHIFT || key==VK_RSHIFT)
        s_key_down[VK_SHIFT]=(unsigned char)(s_key_down[VK_LSHIFT] || s_key_down[VK_RSHIFT]);
}

/* Title bar, the way ps3recomp's window shows it. Written by the flip,
 * read once a second by the window thread; a torn read shows one stale
 * number for a second, which nobody can tell apart from a real one. */
static wchar_t       s_title[48] = L"Xbox Recomp";
static volatile LONG s_flips;
static volatile LONG s_frame_draws;
typedef struct FramebufferCheckpoint {
    uint32_t va,pitch,width,height,present,display[2];
    int32_t running,index,flips,draws;wchar_t title[48];
} FramebufferCheckpoint;
int xbox_FramebufferSaveCheckpoint(FILE *out) {
    if(!out || !xbox_CheckpointFrozen() || xbox_VideoIsPlaying() || getenv("RECOMP_FMV_HOST"))return 0;
    FramebufferCheckpoint saved={s_fb_va,s_fb_pitch,s_fb_width,s_fb_height,0,{0},s_fb_running,s_present_idx,s_flips,s_frame_draws,{0}};
    xbox_CaptureDisplayState(saved.display);memcpy(saved.title,s_title,sizeof s_title);
    for(unsigned i=0;i<2;i++)if(s_present[i])saved.present|=1u<<i;
    int ok=fwrite("XBFRONT",7,1,out)==1 && fwrite(&saved,sizeof saved,1,out)==1;
    for(unsigned i=0;ok && i<2;i++)if(saved.present&(1u<<i))
        ok=fwrite(s_present[i],(size_t)saved.width*saved.height*4,1,out)==1;
    uint32_t host[2]={s_host_width,s_host_height};
    if(ok)ok=fwrite(host,sizeof host,1,out)==1;
    if(ok && host[0])ok=fwrite(s_host_pixels,(size_t)host[0]*host[1]*4,1,out)==1;
    return ok;
}
int xbox_FramebufferLoadCheckpoint(FILE *in) {
    if(!in || !xbox_CheckpointFrozen() || xbox_VideoIsPlaying() || getenv("RECOMP_FMV_HOST"))return 0;
    char magic[7];FramebufferCheckpoint saved;uint32_t *pixels[2]={0};
    int ok=fread(magic,7,1,in)==1 && !memcmp(magic,"XBFRONT",7) && fread(&saved,sizeof saved,1,in)==1 &&
        saved.running==s_fb_running && saved.width==s_fb_width && saved.height==s_fb_height && saved.present<=3 &&
        saved.index>=-1 && saved.index<=1 && (saved.index<0 || (saved.present&(1u<<saved.index))) && saved.title[47]==0;
    for(unsigned i=0;ok && i<2;i++)if(saved.present&(1u<<i)) {
        size_t bytes=(size_t)saved.width*saved.height*4;pixels[i]=malloc(bytes);
        ok=pixels[i] && fread(pixels[i],bytes,1,in)==1;
    }
    uint32_t host[2]={0};uint32_t *host_pixels=NULL;
    if(ok)ok=fread(host,sizeof host,1,in)==1 && ((!host[0] && !host[1]) ||
        (host[0] && host[1] && host[0]<=16384 && host[1]<=16384 && (uint64_t)host[0]*host[1]*4<=SIZE_MAX));
    if(ok && host[0]) {
        size_t bytes=(size_t)host[0]*host[1]*4;host_pixels=malloc(bytes);
        ok=host_pixels && fread(host_pixels,bytes,1,in)==1;
    }
    if(ok) {
        AcquireSRWLockExclusive(&s_host_lock);
        free(s_host_pixels);s_host_pixels=host_pixels;host_pixels=NULL;s_host_width=host[0];s_host_height=host[1];
        ReleaseSRWLockExclusive(&s_host_lock);
        s_fb_va=saved.va;s_fb_pitch=saved.pitch;s_flips=saved.flips;s_frame_draws=saved.draws;
        memcpy(s_title,saved.title,sizeof s_title);xbox_RestoreDisplayState(saved.display);
        for(unsigned i=0;i<2;i++){free(s_present[i]);s_present[i]=pixels[i];pixels[i]=NULL;}
        InterlockedExchange(&s_present_idx,saved.index);
        if(s_rgb && saved.index>=0)memcpy(s_rgb,s_present[saved.index],(size_t)saved.width*saved.height*4);
        const char *dump=getenv("RECOMP_CHECKPOINT_DUMP");
        if(dump)xbox_FramebufferDumpBmp(dump);
    }
    free(host_pixels);for(unsigned i=0;i<2;i++)free(pixels[i]);return ok;
}

void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars)
{
    int i;
    for (i = 0; i < max_chars && i < 47 && name[i]; i++)
        s_title[i] = (wchar_t)name[i];
    if (i) {
        s_title[i] = 0;
        if (i >= 6 && !wcscmp(s_title + i - 6, L" (JPN)"))
            s_title[i - 6] = 0;
    }
}

void xbox_FramebufferWindowFrameStats(uint32_t draws)
{
    InterlockedIncrement(&s_flips);
    InterlockedExchange(&s_frame_draws, (LONG)draws);
}

int xbox_FramebufferKeyDown(int vk)
{
    if ((unsigned)vk > 255)
        return 0;
    return s_key_down[vk] != 0;
}

static LRESULT CALLBACK fb_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CLOSE:
    case WM_DESTROY:
        InterlockedExchange(&s_fb_running, 0);
        return 0;

    case WM_LBUTTONDBLCLK:
        fb_toggle_fullscreen(h);return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if(m==WM_KEYDOWN && (w==VK_F8 || w==VK_F9)) {
            if(!(l & (1L<<30))) {
                if(w==VK_F8)InterlockedExchange(&s_screenshot_command,1);
                else InterlockedExchange(&s_pause_command,1);
            }
            return 0;
        }
        if(s_checkpoint_handler && m==WM_KEYDOWN && w>=VK_F1 && w<=VK_F5) {
            if(!(l & (1L<<30))) {
                LONG command=(LONG)(w-VK_F1+1)+(s_key_down[VK_LSHIFT]?5:0);
                InterlockedCompareExchange(&s_checkpoint_command,command,0);
            }
            return 0;
        }
        fb_set_key_state(w,l,1);
        /* RECOMP_KEY_TRACE: each key as it arrives, edge-triggered.
         *
         * The obvious diagnostic -- sampling which keys are held, once a
         * second, from the input path -- cannot tell a key that was never
         * pressed from one that was tapped: a 100 ms press is caught about
         * one time in ten. That ambiguity is expensive when the only way
         * to test is to ask someone to press a key and describe what
         * happened. This answers "did it arrive" on its own. */
        if (getenv("RECOMP_KEY_TRACE")) {
            static unsigned n;
            if (n++ < 40) {
                fprintf(stderr, "  [KEY] down vk=0x%02X\n", (unsigned)w);
                fflush(stderr);
            }
        }
        /* System keys still go to Windows, or Alt+F4 stops closing us. */
        return m == WM_SYSKEYDOWN ? DefWindowProcA(h, m, w, l) : 0;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        fb_set_key_state(w,l,0);
        return m == WM_SYSKEYUP ? DefWindowProcA(h, m, w, l) : 0;

    /* Alt-tabbing away with a key held would leave it held for ever. */
    case WM_KILLFOCUS:
        memset((void *)s_key_down, 0, sizeof s_key_down);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

/* The display formats an Xbox front buffer is actually set to. The pitch says
 * how wide a row is in bytes, so pitch/width gives the pixel size; the exact
 * component layout only matters for 16-bit, where 5:6:5 and 1:5:5:5 differ. */
static void fb_convert(const uint8_t *src, uint32_t bpp)
{
    uint32_t x, y;

    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * s_fb_pitch;
        uint32_t *dst = s_rgb + (size_t)y * s_fb_width;

        if (bpp == 4) {
            memcpy(dst, row, (size_t)s_fb_width * 4);
        } else if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (x = 0; x < s_fb_width; x++) {
                uint16_t v = p[x];
                uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
                dst[x] = (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)s_fb_width * 4);
        }
    }
}

/* Write what the window is currently showing to a 24-bit BMP.
 *
 * A black window is ambiguous: it means either that the read path is wrong or
 * that the title really did render black. Dumping the same converted pixels
 * the window draws settles which, and does it without a screenshot. */
static int fb_write_bmp(FILE *f,const uint32_t *pixels,unsigned width,unsigned height)
{
    uint32_t row=((width*3u)+3u)&~3u,img=row*height,total=54u+img;
    uint8_t hdr[54]={0},*line=calloc(1,row);
    if(!line)return -1;
    hdr[0]='B';hdr[1]='M';memcpy(hdr+2,&total,4);hdr[10]=54;hdr[14]=40;
    memcpy(hdr+18,&width,4);memcpy(hdr+22,&height,4);hdr[26]=1;hdr[28]=24;memcpy(hdr+34,&img,4);
    int ok=fwrite(hdr,1,sizeof hdr,f)==sizeof hdr;
    for(unsigned y=0;ok && y<height;y++) {
        const uint32_t *src=pixels+(size_t)(height-1-y)*width;
        for(unsigned x=0;x<width;x++) {
            line[x*3]=(uint8_t)src[x];line[x*3+1]=(uint8_t)(src[x]>>8);line[x*3+2]=(uint8_t)(src[x]>>16);
        }
        ok=fwrite(line,1,row,f)==row;
    }
    free(line);return ok?0:-1;
}
/* Keep diagnostic dumps in their original guest dimensions. F8 consumes the
 * displayed source and uses the same GDI stretch and configured aspect as the window. */
int xbox_FramebufferDumpBmp(const char *path) {
    if(!s_rgb || !s_fb_va)return -1;
    FILE *file=fopen(path,"wb");if(!file)return -1;
    int result=fb_write_bmp(file,s_rgb,s_fb_width,s_fb_height);if(fclose(file))result=-1;
    if(!result)fprintf(stderr,"  [FBWIN] wrote %s (%ux%u from 0x%08X)\n",path,s_fb_width,s_fb_height,s_fb_va);
    return result;
}
static int fb_write_screenshot(FILE *file) {
    int result=-1;AcquireSRWLockShared(&s_host_lock);
    const uint32_t *source=s_host_pixels?s_host_pixels:s_rgb;
    unsigned source_width=s_host_pixels?s_host_width:s_fb_width,height=s_host_pixels?s_host_height:s_fb_height;
    unsigned width=(unsigned)((float)height*s_display_aspect+0.5f);
    HDC dc=NULL;HBITMAP bitmap=NULL;void *pixels=NULL;
    if(source && width && height) {
        BITMAPINFO bi={0};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=(LONG)width;
        bi.bmiHeader.biHeight=-(LONG)height;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
        dc=CreateCompatibleDC(NULL);if(dc)bitmap=CreateDIBSection(dc,&bi,DIB_RGB_COLORS,&pixels,NULL,0);
        if(bitmap) {
            HGDIOBJ old=SelectObject(dc,bitmap);SetStretchBltMode(dc,HALFTONE);SetBrushOrgEx(dc,0,0,NULL);
            bi.bmiHeader.biWidth=(LONG)source_width;
            int copied=StretchDIBits(dc,0,0,(int)width,(int)height,0,0,(int)source_width,(int)height,source,&bi,DIB_RGB_COLORS,SRCCOPY);
            GdiFlush();if(copied && copied!=GDI_ERROR)result=fb_write_bmp(file,pixels,width,height);
            SelectObject(dc,old);
        }
    }
    if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);ReleaseSRWLockShared(&s_host_lock);return result;
}
static void fb_screenshot(void) {
    SYSTEMTIME time;GetLocalTime(&time);static unsigned sequence;
    wchar_t path[MAX_PATH],notice[512];
    if(!s_screenshot_directory[0] || (!CreateDirectoryW(s_screenshot_directory,NULL) && GetLastError()!=ERROR_ALREADY_EXISTS)) {fb_notice(fb_text(4));return;}
    int length=swprintf_s(path,MAX_PATH,L"%ls/NINE_%04u%02u%02u_%02u%02u%02u_%03u_%lu_%u.bmp",s_screenshot_directory,
        time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,time.wMilliseconds,GetCurrentProcessId(),++sequence);
    FILE *file=length>0?_wfopen(path,L"wbx"):NULL;
    int result=file?fb_write_screenshot(file):-1;if(file && fclose(file))result=-1;
    if(result) {if(file)DeleteFileW(path);fb_notice(fb_text(4));}
    else {swprintf_s(notice,512,L"%ls%ls",fb_text(3),path);fb_notice(notice);}
}

static DWORD WINAPI fb_thread(LPVOID unused)
{
    HWND hwnd;
    HDC hdc;
    BITMAPINFO bi;
    RECT r;
    uint32_t *host_snapshot=NULL;
    size_t host_capacity=0;

    (void)unused;
    xbox_CheckpointDeviceEnter(XBOX_CHECKPOINT_FRAMEBUFFER);

    {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.style        = CS_DBLCLKS;
        wc.lpfnWndProc   = fb_wndproc;
        wc.hInstance     = GetModuleHandleA(NULL);
        wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
        wc.hIcon         = LoadIconA(wc.hInstance, MAKEINTRESOURCEA(1));
        wc.lpszClassName = "XboxRecompFramebuffer";
        RegisterClassA(&wc);
    }
    r.left = 0; r.top = 0; r.right = (LONG)s_window_width; r.bottom = (LONG)s_window_height;
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowExA(0, "XboxRecompFramebuffer", "Xbox Recomp - Framebuffer",
                           WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top,
                           NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (!hwnd) {
        InterlockedExchange(&s_fb_running, 0);
        fb_close_frame_event();
        xbox_CheckpointDeviceLeave(XBOX_CHECKPOINT_FRAMEBUFFER);
        return 0;
    }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    hdc = GetDC(hwnd);
    SetStretchBltMode(hdc, HALFTONE);
    SetBrushOrgEx(hdc, 0, 0, NULL);

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = (LONG)s_fb_width;
    bi.bmiHeader.biHeight      = -(LONG)s_fb_height;   /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    s_rgb = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height, 4);

    fprintf(stderr, "  [FBWIN] framebuffer window open (%ux%u)\n",
            s_fb_width, s_fb_height);
    xbox_CheckpointDeviceLeave(XBOX_CHECKPOINT_FRAMEBUFFER);

    while (InterlockedCompareExchange(&s_fb_running, 1, 1)) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if(!s_paused)xbox_CheckpointDeviceEnter(XBOX_CHECKPOINT_FRAMEBUFFER);
        RECT display=fb_display_bounds(hwnd),client;GetClientRect(hwnd,&client);
        RECT bars[4]={{0,0,client.right,display.top},{0,display.bottom,client.right,client.bottom},
            {0,display.top,display.left,display.bottom},{display.right,display.top,client.right,display.bottom}};
        for(unsigned i=0;i<4;i++)FillRect(hdc,&bars[i],(HBRUSH)GetStockObject(BLACK_BRUSH));
        if (s_present_idx >= 0 && s_rgb) {
            /* A finished frame, published by the flip. Copied into s_rgb so
             * the dump path and GDI see one consistent image even if the
             * next flip lands mid-blit. */
            LONG idx = s_present_idx;
            if (s_present[idx])
                memcpy(s_rgb, s_present[idx],
                       (size_t)s_fb_width * s_fb_height * 4);
            if(!s_host_width)StretchDIBits(hdc, display.left, display.top, display.right-display.left, display.bottom-display.top,
                          0, 0, (int)s_fb_width, (int)s_fb_height,
                          s_rgb, &bi, DIB_RGB_COLORS, SRCCOPY);
        } else if (s_fb_va && s_fb_pitch && s_rgb) {
            /* No flip yet, or pinned with RECOMP_FB_VA: read guest memory as
             * before, which is also what a title that never flips needs. */
            const uint8_t *src =
                (const uint8_t *)((uintptr_t)s_fb_va + xbox_GetMemoryOffset());
            fb_convert(src, s_fb_pitch / s_fb_width);
            if(!s_host_width)StretchDIBits(hdc, display.left, display.top, display.right-display.left, display.bottom-display.top,
                          0, 0, (int)s_fb_width, (int)s_fb_height,
                          s_rgb, &bi, DIB_RGB_COLORS, SRCCOPY);
        }
        unsigned host_width=0,host_height=0;
        AcquireSRWLockShared(&s_host_lock);
        if(s_host_pixels) {
            host_width=s_host_width;host_height=s_host_height;
            size_t bytes=(size_t)host_width*host_height*4;
            if(bytes>host_capacity) {
                uint32_t *buffer=realloc(host_snapshot,bytes);
                if(!buffer) {fprintf(stderr,"[STOP] framebuffer host snapshot allocation failed\n");exit(5);}
                host_snapshot=buffer;host_capacity=bytes;
            }
            memcpy(host_snapshot,s_host_pixels,bytes);
        }
        ReleaseSRWLockShared(&s_host_lock);
        /* GDI may wait on the desktop. Keep its complete source private to
         * this window so a new flip need not hold the guest CPU until it returns. */
        if(host_width) {
            BITMAPINFO host_bi=bi;
            host_bi.bmiHeader.biWidth=(LONG)host_width;host_bi.bmiHeader.biHeight=-(LONG)host_height;
            StretchDIBits(hdc,display.left,display.top,display.right-display.left,display.bottom-display.top,
                0,0,(int)host_width,(int)host_height,host_snapshot,&host_bi,DIB_RGB_COLORS,SRCCOPY);
        }
        if(s_paused || (s_checkpoint_notice[0] && GetTickCount64()<s_checkpoint_notice_until)) {
            RECT text={display.left+8,display.bottom-26,display.right-8,display.bottom-8};
            HGDIOBJ font=SelectObject(hdc,GetStockObject(DEFAULT_GUI_FONT));
            int background=SetBkMode(hdc,TRANSPARENT);
            COLORREF color=SetTextColor(hdc,RGB(255,255,255));
            DrawTextW(hdc,GetTickCount64()<s_checkpoint_notice_until?s_checkpoint_notice:fb_text(0),-1,&text,DT_LEFT|DT_BOTTOM|DT_SINGLELINE|DT_NOPREFIX|DT_END_ELLIPSIS);
            SetTextColor(hdc,color);SetBkMode(hdc,background);SelectObject(hdc,font);
        }
        {
            /* One dump a few seconds in, so the title has had time to render
             * something rather than catching the first blank frame.
             *
             * RECOMP_FB_WINDOW_DUMP_EVERY=<frames> dumps repeatedly instead.
             * This window follows the address AvSetDisplayMode gave, which is
             * what the CRTC scans and therefore what a person sees; the
             * pushbuffer executor's own dump follows its draw surface. With
             * double buffering those are different buffers, and measuring
             * progress from the executor's dump reports a blank screen while
             * the window is showing the title's logo. Ask the window. */
            const char *dump = getenv("RECOMP_FB_DUMP");
            const char *every = getenv("RECOMP_FB_WINDOW_DUMP_EVERY");
            static int frames;
            int period = every ? atoi(every) : 0;
            frames++;
            if (dump && period > 0) {
                if (frames % period == 0)
                    xbox_FramebufferDumpBmp(dump);
            } else if (dump && frames == 600) {
                xbox_FramebufferDumpBmp(dump);
            }
        }
        {
            static DWORD t0;
            static LONG f0;
            DWORD now = GetTickCount();
            if (now - t0 >= 1000) {
                LONG f = s_flips;
                wchar_t tb[128];
                _snwprintf(tb, 127, L"%ls | FPS: %.1f | draws: %ld%ls", s_title,
                           t0 ? (f - f0) * 1000.0 / (now - t0) : 0.0,
                           (long)s_frame_draws,s_paused?L" | F9: paused":L"");
                tb[127] = 0;
                SetWindowTextW(hwnd, tb);
                t0 = now; f0 = f;
            }
        }
        if(!s_paused)xbox_CheckpointDeviceLeave(XBOX_CHECKPOINT_FRAMEBUFFER);
        if(InterlockedExchange(&s_screenshot_command,0))fb_screenshot();
        if(InterlockedExchange(&s_pause_command,0)) {
            if(s_paused) {xbox_CheckpointEnd();s_paused=0;fb_notice(fb_text(1));}
            else if(xbox_CheckpointBegin(3000)) {s_paused=1;memset((void *)s_key_down,0,sizeof s_key_down);fb_notice(fb_text(0));}
            else fb_notice(fb_text(2));
            fprintf(stderr,"[FBWIN] F9 paused=%d\n",s_paused);fflush(stderr);
        }
        LONG checkpoint=InterlockedExchange(&s_checkpoint_command,0);
        if(checkpoint && s_paused)fb_notice(fb_text(5));
        else if(checkpoint && s_checkpoint_handler) {
            int load=checkpoint<=5;unsigned slot=(unsigned)(checkpoint-1)%5+1;
            s_checkpoint_notice[0]=0;
            if(s_checkpoint_handler(load,slot)) {
                swprintf_s(s_checkpoint_notice,512,load?L"Loaded Slot %u.":L"Saved to Slot %u.",slot);
                s_checkpoint_notice_until=GetTickCount64()+3000;
            }
        }
        /* Completed frames wake the existing window immediately. Retain the
         * polling interval for pinned buffers and periods without a flip. */
        if(MsgWaitForMultipleObjects(1,&s_frame_ready,FALSE,16,QS_ALLINPUT)==WAIT_FAILED) {
            fprintf(stderr,"[STOP] framebuffer frame wait failed: %lu\n",GetLastError());
            exit(5);
        }
    }

    if(s_paused){xbox_CheckpointEnd();s_paused=0;}
    free(host_snapshot);
    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
    free(s_rgb);
    s_rgb = NULL;
    fb_close_frame_event();
    return 0;
}

void xbox_FramebufferWindowStart(void)
{
    HANDLE th;

    if (!getenv("RECOMP_FB_WINDOW"))
        return;
    if (InterlockedCompareExchange(&s_fb_running, 1, 0) != 0)
        return;
    s_frame_ready=CreateEventW(NULL,FALSE,FALSE,NULL);
    if(!s_frame_ready){fprintf(stderr,"[STOP] framebuffer frame event failed: %lu\n",GetLastError());exit(5);}
    xbox_CheckpointDeviceCreating(XBOX_CHECKPOINT_FRAMEBUFFER);
    th = CreateThread(NULL, 0, fb_thread, NULL, 0, NULL);
    if (th)
        CloseHandle(th);
    else {
        InterlockedExchange(&s_fb_running, 0);
        fb_close_frame_event();
        xbox_CheckpointDeviceCreated(XBOX_CHECKPOINT_FRAMEBUFFER);
    }
}

#else
void xbox_FramebufferWindowSetControls(const wchar_t *directory,unsigned language) { (void)directory;(void)language; }
void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch) { (void)fb_va; (void)pitch; }
void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch) { (void)fb_va; (void)pitch; }
void xbox_FramebufferWindowStart(void) {}
int xbox_FramebufferKeyDown(int vk) { (void)vk; return 0; }
void xbox_FramebufferSetCheckpointHandler(int (*handler)(int,unsigned)) { (void)handler; }
void xbox_FramebufferWindowSetTitle(const uint16_t *n, int m) { (void)n; (void)m; }
void xbox_FramebufferWindowFrameStats(uint32_t draws) { (void)draws; }
int xbox_FramebufferSaveCheckpoint(FILE *out) { (void)out;return 0; }
int xbox_FramebufferLoadCheckpoint(FILE *in) { (void)in;return 0; }
#endif
