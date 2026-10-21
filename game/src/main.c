/**
 * Shin Megami Tensei NINE - Recompiled Game Entry Point
 *
 * This is the Windows executable that hosts the recompiled game code.
 * It performs the following initialization sequence:
 *
 * 1. Load the original XBE file from disk
 * 2. Initialize the Xbox memory layout (map data sections to original VAs)
 * 3. Initialize the Xbox kernel replacement layer
 * 4. Initialize the kernel bridge (thunk table in Xbox memory)
 * 5. Set up game file paths for I/O redirection
 * 6. Initialize the stack pointer
 * 7. Install VEH crash handler for diagnostics
 * 8. Call the game's original entry point (recompiled)
 *
 * XBE Details (from the existing parser):
 *   Title:       Shin Megami Tensei NINE
 *   Title ID:    0x41540002
 *   Base addr:   0x00010000
 *   Entry point: 0x0021AF2F
 *   Sections:    13; eight code sections are statically lifted
 *   Kernel imports: 158
 */

#include <windows.h>
#include <shlobj.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

/* xboxrecomp runtime headers */
#include <xbox/xboxrecomp.h>
#include "ohci.h"
#include "game_paths.h"
#include "config.h"
#include "font_assets.h"
static NineConfig s_display;
int smt9_is_japanese(void){return !wcscmp(s_display.language,L"Japanese");}
int smt9_is_chinese(void){return !wcscmp(s_display.language,L"Chinese");}
static NineLanguage s_language;
unsigned smt9_native_font_pages(void){return s_language.font_pages;}
float smt9_game_aspect(void) { return (float)s_display.aspect_width/(float)s_display.aspect_height; }
unsigned smt9_game_refresh_hz(void) { return s_display.refresh_hz; }
#ifdef SMT9_MACHINE_CHECKPOINT
#include "video_player.h"
#include <io.h>
static const char *s_cold_checkpoint;
static char *instant_checkpoint_path(unsigned slot) {
    const char *base=getenv("RECOMP_CHECKPOINT_PATH");
    if(!base)base=SMT9_CHECKPOINT_PATH;
    size_t length=strlen(base),prefix=length;
    for(size_t i=length;i>0;i--) {
        if(base[i-1]=='/' || base[i-1]=='\\')break;
        if(base[i-1]=='.'){prefix=i-1;break;}
    }
    char *path=malloc(length+7);
    if(path) {
        memcpy(path,base,prefix);memcpy(path+prefix,".slot",5);
        path[prefix+5]=(char)('0'+slot);
        memcpy(path+prefix+6,base+prefix,length-prefix+1);
    }
    return path;
}
static int instant_checkpoint_control(int load,unsigned slot) {
    if(slot<1 || slot>5)return 0;
    char *path=instant_checkpoint_path(slot);
    if(!path){fprintf(stderr,"[CHECKPOINT] slot %u %s rejected: path allocation failed\n",slot,load?"load":"save");return 0;}
    FILE *file=NULL;int ok=0;
    if(load) {
        file=nine_fopen(path,"rb");
        if(file) {ok=xbox_CheckpointLoadMachine(file,0,10000);fclose(file);}
    } else {
        size_t length=strlen(path);char *temporary=malloc(length+5);
        if(temporary) {
            memcpy(temporary,path,length);memcpy(temporary+length,".tmp",5);
            wchar_t target_w[MAX_PATH],temporary_w[MAX_PATH];
            if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,target_w,MAX_PATH) ||
               !MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,temporary,-1,temporary_w,MAX_PATH)) {
                free(temporary);free(path);return 0;
            }
            file=nine_fopen(temporary,"wb");
            if(file) {
                ok=xbox_CheckpointSaveMachine(file,10000);
                if(ok)ok=fflush(file)==0 && _commit(_fileno(file))==0;
                if(fclose(file))ok=0;
                if(ok)ok=MoveFileExW(temporary_w,target_w,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
                if(!ok)DeleteFileW(temporary_w);
            }
            free(temporary);
        }
    }
    fprintf(stderr,"[CHECKPOINT] slot %u %s %s: %s\n",slot,load?"load":"save",ok?"complete":"rejected",path);
    free(path);
    return ok;
}
#endif

/*
 * If xboxrecomp.h is not an umbrella header in your setup, include
 * the individual headers directly:
 *
 * #include "kernel.h"
 * #include "xbox_memory_layout.h"
 * #include "d3d8_xbox.h"
 * #include "dsound_xbox.h"
 * #include "xinput_xbox.h"
 */

/* ── Global register state (defined in xbox_memory_layout.c) ── */

/* RECOMP_TLS is not optional here. The runtime defines these thread-local, and
 * a plain `extern` referencing a __declspec(thread) variable does not resolve
 * to the calling thread's copy -- it resolves to the image's TLS template. The
 * host side then writes g_esp somewhere the generated code never reads, so the
 * guest starts with every register at zero and faults immediately, having
 * apparently ignored the setup that visibly ran. */
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern RECOMP_TLS uint32_t g_seh_ebp;
/* x87 and SSE state. Global for the same reason the volatile GPRs are: one
 * guest routine can lift to several C functions, so a value written in one
 * body is read in the next. Defined in xbox_memory_layout.c like the rest of
 * the register file. */
extern RECOMP_TLS double g_fp_stack[8];
extern RECOMP_TLS int g_fp_top;
extern RECOMP_TLS uint16_t g_fp_control_word;
extern RECOMP_TLS int g_fp_cmp;
extern RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
extern RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;
extern ptrdiff_t g_xbox_mem_offset;

/* ── XBE Constants ─────────────────────────────────────────── */

/* Paths are configured by CMake from the existing live resource directory. */
#define YOUR_GAME_ENTRY_POINT   0x0021AF2F
#define YOUR_GAME_XBE_PATH      SMT9_XBE_PATH
#define YOUR_GAME_DIR           SMT9_GAME_DIR

/* ── Forward declarations ──────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size);

/* Recompiled entry point (generated by recomp pipeline) */
extern void xbe_entry_point(void);

/* ── VEH crash handler ─────────────────────────────────────── */

/*
 * Vectored Exception Handler for crash diagnostics.
 *
 * When the recompiled game hits an access violation, this handler prints
 * the faulting address, all Xbox register values, and a native stack trace.
 * This is your primary debugging tool during bring-up.
 *
 * Customize this for your game:
 *   - Add game-specific address checks (GPU register probes, etc.)
 *   - Add dumps of game-specific globals (heap handles, state flags)
 *   - Add SEH simulation if your game uses __try/__except
 */
/* Name the guest function a fault happened in, and recover the call chain.
 *
 * Recompiled code faults as ordinary native code, so the exception record
 * carries a host RIP and nothing else -- there is no guest program counter to
 * report, and the host address changes every build. Two things recover the
 * guest view:
 *
 *   - every generated function is a real symbol in the image (sub_005A03C0
 *     and so on), so the linker's PDB already maps host address back to guest
 *     function. dbghelp turns an anonymous RIP into that name.
 *
 *   - every lifted call pushes its guest return address onto the guest stack
 *     before jumping, so the stack still holds the chain. Scanning up from esp
 *     for values inside the code sections recovers it.
 *
 * ponytail: the stack scan is a scan, not a frame walk -- these are FPO frames
 * with no reliable ebp chain, so there is nothing to walk. It over-reports,
 * since addresses from returned-from calls linger below esp, but naming the
 * guest function is the whole question at a fault.
 *
 * Requires linking dbghelp and keeping the .pdb beside the .exe.
 */
static void print_guest_context(void *rip)
{
    /* SYMBOL_INFO is variable-length: the name is written past the struct, so
     * it must be over-allocated with MaxNameLen set to the slack. */
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;

    memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)rip, &disp, sym))
        fprintf(stderr, "  in %s+0x%llX\n",
                sym->Name, (unsigned long long)disp);

    if (g_xbox_mem_offset && g_esp) {
        const uint32_t *sp =
            (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        int shown = 0, i;
        fprintf(stderr, "  guest stack (return addresses, innermost first):\n");
        for (i = 0; i < 256 && shown < 24; i++) {
            uint32_t v = sp[i];
            if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "    [esp+%-4d] 0x%08X\n", i * 4, v);
                shown++;
            }
        }
        /* When the frame is gone, say so, and fall back on what is left.
         *
         * A fault inside a block copy leaves no return address in range: the
         * copy is a host memmove and its caller's frame is further up than
         * this reaches. Worse, a copy that ran past its buffer overwrites the
         * stack itself, so the list comes out empty exactly when it is most
         * needed -- and an empty list under a promising heading reads like
         * "nothing to see" rather than "the evidence is destroyed". */
        if (!shown)
            fprintf(stderr, "    (no return addresses in range)\n");

        /* Everything below only pays for itself when the scan came back
         * empty. A crash where the chain was recovered already says what
         * happened, and burying that under another twenty lines makes the
         * common report worse to read. */
        if (!shown) {
            uint32_t k;

            /* The indirect-call history, which the stack cannot overwrite.
             * recomp_manual.c prints this too, but only from
             * recomp_icall_fail_log -- a page fault inside a block copy
             * never reaches that path, and this is the same information by
             * the route the fault actually took.
             *
             * Declared here rather than included: main.c does not pull in
             * recomp_types.h, and recomp_manual.c spells it the same way. */
            extern volatile uint32_t g_icall_trace[16];
            extern volatile uint32_t g_icall_trace_idx;

            fprintf(stderr, "  recent ICALL targets:");
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %08X",
                        g_icall_trace[(g_icall_trace_idx + k) & 15]);
            fprintf(stderr, "\n");

            /* The frame unfiltered. The pointers and lengths handed to
             * whatever faulted live here and look nothing like code, so the
             * filter above drops exactly what is wanted. On the crash this
             * was written for, one of these words was the end of the mapped
             * RAM mirror -- which is what identified the fault as a copy
             * running off its buffer rather than a stray pointer. */
            for (i = 0; i < 20; i++)
                fprintf(stderr, "    raw[esp+%-4d] 0x%08X\n", i * 4, sp[i]);
        }
    }
}

/* APU/AC97 VEH adapters share the original GPU instruction decoder.
 * xbox_ConnectApu owns state initialization and physical RAM binding. */
typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *g_apu_state;
extern bool ac97_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                                  uint32_t fault_xbox_va, int is_write);
extern bool apu_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                                 uint32_t fault_xbox_va, int is_write);

/* Device windows connected by xbox_ConnectApu. */
#define APU_TRAP_BASE 0xFE800000u
#define APU_TRAP_END  0xFE880000u
#include "nv2a_state.h"
extern int nv2a_pb_exec_method(uint32_t subchannel, uint32_t method, uint32_t parameter, uint8_t *output);
extern int nv2a_pb_handle_memory_fault(uintptr_t fault_addr, int is_write);
extern void nv2a_pb_release_memory_views(void);
extern bool nv2a_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                                  uint32_t fault_xbox_va, int is_write);

/* WER's normal dump omits the file-mapped guest heap. Read the original
 * section through a separate view so diagnostic reads do not complete GPU
 * work or change guest page protections. This is raw RAM, not a save state;
 * device threads are not frozen and may still write while it is copied. */
static void write_crash_ram(void)
{
    static volatile LONG started;
    if (InterlockedCompareExchange(&started, 1, 0)) return;
    HANDLE section = xbox_GetMappingHandle();
    size_t bytes = xbox_GetMappedSize();
    if (!section || !bytes || bytes > MAXDWORD) {
        fprintf(stderr, "[CRASH] guest RAM capture unavailable: no bounded mapping\n");
        return;
    }
    void *ram = MapViewOfFile(section, FILE_MAP_READ, 0, 0, bytes);
    if (!ram) {
        fprintf(stderr, "[CRASH] guest RAM view failed: %lu\n", GetLastError());
        return;
    }
    FILETIME time;
    GetSystemTimeAsFileTime(&time);
    wchar_t path[MAX_PATH];
    if (swprintf_s(path, MAX_PATH, L"%s/crash-%lu-%08lX%08lX-ram.bin",
                   nine_paths.runtime_w, GetCurrentProcessId(),
                   time.dwHighDateTime, time.dwLowDateTime) < 0) {
        fprintf(stderr, "[CRASH] guest RAM path too long\n");
        UnmapViewOfFile(ram);
        return;
    }
    HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[CRASH] guest RAM file failed: %lu\n", GetLastError());
    } else {
        DWORD written = 0;
        BOOL ok = WriteFile(file, ram, (DWORD)bytes, &written, NULL);
        DWORD error = ok ? ERROR_SUCCESS : GetLastError();
        CloseHandle(file);
        fprintf(stderr, "[CRASH] guest RAM %s: %ls (%lu/%zu bytes, error=%lu; guest base=0)\n",
                ok && written == bytes ? "saved" : "incomplete", path, written, bytes, error);
    }
    UnmapViewOfFile(ram);
}

static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
        fprintf(stderr, "[CRASH] native stack overflow RIP=0x%llX guest ESP=0x%08X\n",
                (unsigned long long)ep->ContextRecord->Rip, g_esp);
        print_guest_context((void *)ep->ContextRecord->Rip);
        ExitProcess(6);
    }
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t fault_addr = ep->ExceptionRecord->ExceptionInformation[1];

        uint32_t fault_xbox_va =
            (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset);
        if (fault_xbox_va >= 0xFD000000u && fault_xbox_va < 0xFE000000u &&
            nv2a_hook_handle_mmio(ep->ContextRecord, fault_addr, fault_xbox_va,
                                  ep->ExceptionRecord->ExceptionInformation[0] ? 1 : 0)) {
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        if (fault_xbox_va >= 0xFEC00000u && fault_xbox_va < 0xFEC01000u &&
            ac97_hook_handle_mmio(ep->ContextRecord, fault_addr, fault_xbox_va,
                                  ep->ExceptionRecord->ExceptionInformation[0] ? 1 : 0))
            return EXCEPTION_CONTINUE_EXECUTION;

        if (xbox_OhciOwnsAddress(fault_xbox_va) &&
            xbox_OhciHandleMmio(ep->ContextRecord, fault_xbox_va))
            return EXCEPTION_CONTINUE_EXECUTION;

        /* Dispatch original APU register and DSP-window accesses. */
        {
            uint32_t xbox_va =
                (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset);

            if (xbox_va >= APU_TRAP_BASE && xbox_va < APU_TRAP_END
                    && apu_hook_handle_mmio(
                           ep->ContextRecord, fault_addr, xbox_va,
                           ep->ExceptionRecord->ExceptionInformation[0]
                               ? 1 : 0)) {
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }

        if(ep->ExceptionRecord->ExceptionInformation[0]<=1 && nv2a_pb_handle_memory_fault(fault_addr,(int)ep->ExceptionRecord->ExceptionInformation[0]))
            return EXCEPTION_CONTINUE_EXECUTION;

        fprintf(stderr, "[CRASH] Access violation at RIP=0x%llX, fault addr=0x%llX (%s)\n",
            (unsigned long long)ep->ContextRecord->Rip,
            (unsigned long long)fault_addr,
            ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read");
        fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
            g_eax, g_ecx, g_edx, g_esp);
        fprintf(stderr, "  Xbox regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n",
            g_ebx, g_esi, g_edi);
        fprintf(stderr, "  Xbox VA of fault: 0x%08X\n",
            (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset));
        write_crash_ram();
        print_guest_context((void *)ep->ContextRecord->Rip);

        /*
         * TODO: Add game-specific diagnostics here. Examples:
         *
         * Dump CRT heap handle:
         *   uint32_t heap = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + HEAP_HANDLE_VA);
         *   fprintf(stderr, "  CRT heap handle: 0x%08X\n", heap);
         *
         * Dump game state:
         *   uint32_t state = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + GAME_STATE_VA);
         *   fprintf(stderr, "  Game state: %u\n", state);
         */

        /* Print native stack return addresses for debugging */
        {
            uintptr_t *sp = (uintptr_t *)ep->ContextRecord->Rsp;
            fprintf(stderr, "  Native stack (first 8 return addrs):\n");
            for (int i = 0; i < 64 && sp[i]; i++) {
                if (sp[i] >= 0x140000000ULL && sp[i] < 0x150000000ULL) {
                    fprintf(stderr, "    [%d] 0x%llX\n", i, (unsigned long long)sp[i]);
                }
            }
        }
        fflush(stderr);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

/* ── WinMain ───────────────────────────────────────────────── */

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    void *xbe_data = NULL;
    size_t xbe_size = 0;

    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    /* Unbuffered output for immediate visibility during debugging */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    /* NINE's XPR loader registers completion after its overlapped read
     * returns pending. Use the runtime's existing asynchronous-handle path. */
    if (_putenv_s("RECOMP_ASYNC_IO", "1") != 0) {
        fprintf(stderr, "Failed to enable asynchronous file I/O.\n");
        return 1;
    }
    /* Original DSOUND queues use raised IRQL for single-CPU exclusion. */
    if (_putenv_s("RECOMP_GUEST_LOCK", "1") != 0) {
        fprintf(stderr, "Failed to enable guest CPU ownership.\n");
        return 1;
    }

    /* XDK D3D and USB build dispatcher events directly in guest memory. */
    if (_putenv_s("RECOMP_TITLE_KEVENTS", "1") != 0) {
        fprintf(stderr, "Failed to enable title dispatcher events.\n");
        return 1;
    }

    wchar_t config_error[256];char backend[16];
    if(!nine_config_read(&s_display,config_error,256)) {
        MessageBoxW(NULL,config_error,L"NINE",MB_OK|MB_ICONERROR);return 1;
    }
    extern BOOL xbox_InputConfigure(const XboxInputBindings *bindings);
    if(!xbox_InputConfigure(&s_display.controls))return 1;
    if(!nine_language_resolve(s_display.language,&s_language,config_error,256)) {
        MessageBoxW(NULL,config_error,L"NINE",MB_OK|MB_ICONERROR);return 1;
    }
    int language_entry=nine_language_enter(&s_language,config_error,256);
    if(language_entry<=0) {
        if(language_entry<0)MessageBoxW(NULL,config_error,L"NINE",MB_OK|MB_ICONERROR);
        return language_entry<0?1:0;
    }
    if(!WideCharToMultiByte(CP_UTF8,0,s_display.backend,-1,backend,16,NULL,NULL))return 1;
    extern void d3d8_DisplayConfigure(unsigned scale,float aspect);
    extern void d3d8_DisplayVSyncConfigure(int enabled);
    extern void xbox_FramebufferWindowConfigure(unsigned width,unsigned height,float aspect,int fixed_aspect);
    d3d8_DisplayConfigure(s_display.render_scale,smt9_game_aspect());
    d3d8_DisplayVSyncConfigure(s_display.vsync);
    xbox_FramebufferWindowConfigure(s_display.window_width,s_display.window_height,smt9_game_aspect(),s_display.fixed_aspect);
    if(_putenv_s("RECOMP_GPU_BACKEND",backend)!=0)return 1;
    fprintf(stderr,"[DISPLAY] backend=%s render_scale=%u window=%ux%u aspect=%u:%u refresh_hz=%u vsync=%u\n",backend,s_display.render_scale,s_display.window_width,s_display.window_height,s_display.aspect_width,s_display.aspect_height,s_display.refresh_hz,s_display.vsync);
    fprintf(stderr,"[LANGUAGE] %ls\n",s_display.language);
    if(!nine_title_assets_init(smt9_game_aspect()))return 1;
    if(smt9_is_chinese() && !nine_font_assets_init(s_display.render_scale))return 1;
    /* Unsupported state after successful device creation remains strict. */
    if (_putenv_s("RECOMP_GPU_D3D11", "1") != 0 ||
        _putenv_s("RECOMP_FB_WINDOW", "1") != 0 ||
        _putenv_s("RECOMP_HEAP_RECLAIM", "1") != 0 ||
        (!getenv("RECOMP_KEYBOARD") && _putenv_s("RECOMP_KEYBOARD", "1") != 0)) {
        fprintf(stderr, "Failed to configure native game runtime.\n");
        return 1;
    }
    if ((!getenv("RECOMP_TRACE_BUDGET") && _putenv_s("RECOMP_TRACE_BUDGET", "0") != 0) ||
        (!getenv("RECOMP_KERNEL_LOG_BUDGET") && _putenv_s("RECOMP_KERNEL_LOG_BUDGET", "0") != 0)) {
        fprintf(stderr, "Failed to configure diagnostic logging.\n");
        return 1;
    }

    printf("=== Shin Megami Tensei NINE - Static Recompilation ===\n");
    printf("Loading XBE...\n");

    /* Install VEH handler (first handler in chain) */
    /* Load symbols up front rather than from inside the handler: at fault
     * time the process is already in a bad way, and SymInitialize
     * allocates. Failure is not fatal -- the handler prints no name. */
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    AddVectoredExceptionHandler(1, veh_handler);
    ULONG stack_guarantee = 65536;
    SetThreadStackGuarantee(&stack_guarantee);

    /* Step 1: Load XBE */
    char language_image[MAX_PATH];
    if(s_language.external && !WideCharToMultiByte(CP_UTF8,0,s_language.image,-1,language_image,MAX_PATH,NULL,NULL))return 1;
    if (!load_xbe(s_language.external?language_image:(smt9_is_japanese()?SMT9_JP_XBE_PATH:YOUR_GAME_XBE_PATH), &xbe_data, &xbe_size)) {
        fprintf(stderr, "Failed to load %s\n", YOUR_GAME_XBE_PATH);
        return 1;
    }
    if(!xbox_CheckpointSetGameImage(xbe_data,xbe_size)) {
        fprintf(stderr,"[STOP] checkpoint executable/image identity initialization failed\n");return 5;
    }
    printf("XBE loaded: %zu bytes\n", xbe_size);

    /* Step 2: Initialize Xbox memory layout */
    printf("Initializing Xbox memory layout...\n");
    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        fprintf(stderr, "Failed to initialize Xbox memory layout.\n");
        free(xbe_data);
        return 1;
    }

    g_xbox_mem_offset = xbox_GetMemoryOffset();
    printf("Xbox memory mapped. Offset: 0x%llX\n", (unsigned long long)g_xbox_mem_offset);
    if (!xbox_ConnectNv2a()) {
        fprintf(stderr, "[STOP] failed to connect NV2A to guest memory\n");
        return 5;
    }
    nv2a_get_state()->pfifo.method_consumer = nv2a_pb_exec_method;
    extern const FramebufferNativeRenderer *nv2a_pb_native_renderer(void);
    xbox_FramebufferWindowSetRenderer(nv2a_pb_native_renderer());

    if (!xbox_ConnectApu()) {
        fprintf(stderr, "[STOP] failed to connect APU/AC97 to guest memory\n");
        return 5;
    }

    /* Step 3: Initialize Xbox kernel */
    printf("Initializing Xbox kernel replacement...\n");
    xbox_kernel_init();
    if (!nv2a_fifo_start(nv2a_get_state(), s_display.multithreaded_graphics)) {
        fprintf(stderr, "[STOP] graphics consumer startup failed\n");
        return 5;
    }
    if(!nine_saves_prepare(s_language.id,config_error,256)) {
        MessageBoxW(NULL,config_error,L"NINE",MB_OK|MB_ICONERROR);return 1;
    }

    /* Step 4: Set game directory for file I/O path translation */
    {
        extern void xbox_path_init(const char *game_dir, const char *save_dir);
        extern BOOL xbox_path_set_user_directory(const char *directory);
        xbox_path_init(YOUR_GAME_DIR, SMT9_SAVE_DIR);
        if(!xbox_path_set_user_directory(SMT9_USER_DIR))return 1;
        char media_root[MAX_PATH],cache_id[64];
        extern void xbox_path_set_language_media(const char *language,const char *media_root);
        if(s_language.external) {
            if(!WideCharToMultiByte(CP_UTF8,0,s_language.directory,-1,media_root,MAX_PATH,NULL,NULL) ||
               !WideCharToMultiByte(CP_UTF8,0,wcsrchr(s_language.directory,L'/')+1,-1,cache_id,64,NULL,NULL))return 1;
            xbox_path_set_language_media(cache_id,media_root);
        }else xbox_path_set_language_media(smt9_is_japanese()?"Japanese":"",SMT9_JP_GAME_DIR);
    }

    /* Step 5: Initialize kernel bridge (thunk table in Xbox memory) */
    printf("Initializing kernel bridge...\n");
    xbox_kernel_bridge_init();

    /* Step 6: Initialize stack */
    g_esp = XBOX_STACK_TOP;

    /*
     * TODO: Pre-initialize CRT globals if needed.
     *
     * Many Xbox games use the MSVC CRT. The CRT's __heap_init sets up a
     * heap descriptor at a game-specific address. You may need to
     * pre-initialize __active_heap to avoid small-block heap issues:
     *
     *   uint32_t *active_heap = (uint32_t *)((uint8_t *)g_xbox_mem_offset + ACTIVE_HEAP_VA);
     *   *active_heap = 1;  // 1 = system heap (HeapAlloc), avoids SBH init
     *
     * Find ACTIVE_HEAP_VA by searching the disassembly for __heap_init
     * or by looking for the CRT's __active_heap global in the data section.
     */

    printf("\n=== Initialization complete ===\n");
    printf("Entry point: 0x%08X\n", YOUR_GAME_ENTRY_POINT);
    printf("ESP: 0x%08X\n", g_esp);

    /* Require the existing flat dispatch table before the guest runs. */
    if (!recomp_dispatch_init()) {
        fprintf(stderr, "[BOOT] dispatch allocation failed\n");
        return 1;
    }

    /* Original XAPI drives OHCI directly; the linked USB model and host pad
     * backend provide its descriptors, reports and connected interrupt. */
    if (_putenv_s("RECOMP_USB", "1") != 0) {
        fprintf(stderr, "Failed to enable the USB controller model.\n");
        return 1;
    }
    xbox_InputInit();
    xbox_OhciInit();
    if (!xbox_OhciOwnsAddress(XBOX_OHCI0_BASE) ||
        !xbox_OhciOwnsAddress(XBOX_OHCI1_BASE)) {
        fprintf(stderr, "[STOP] USB controller MMIO connection failed.\n");
        return 5;
    }

    /* Arm the hang watchdog. Does nothing unless RECOMP_WATCHDOG_SECS is set,
     * and must be called from this thread -- the guest registers it samples are
     * thread-local, so it has to be handed the copies belonging to the thread
     * that runs guest code.
     *
     * Not optional boilerplate: without this call RECOMP_WATCHDOG_SECS is
     * silently inert, and the one diagnostic that tells a hang from slowness
     * does nothing while appearing to be set. */
    xbox_WatchdogStart();

    /* Step 7: Call the recompiled entry point */
    printf("\nStarting game...\n");
    fflush(stdout);

#ifdef SMT9_MACHINE_CHECKPOINT
    unsigned interface_language=!wcscmp(s_display.interface_language,L"Japanese")?1:!wcscmp(s_display.interface_language,L"English")?2:0;
    xbox_FramebufferWindowSetControls(SMT9_SCREENSHOTS_DIR_W,interface_language);
    xbox_FramebufferSetCheckpointHandler(instant_checkpoint_control);
    if(s_cold_checkpoint) {
        guest_cpu_part();
        FILE *checkpoint=nine_fopen(s_cold_checkpoint,"rb");
        int restored=checkpoint && xbox_CheckpointLoadMachine(checkpoint,1,10000);
        if(checkpoint)fclose(checkpoint);
        if(!restored) {
            fprintf(stderr,"[CHECKPOINT] cold load rejected: %s\n",s_cold_checkpoint);
            return 5;
        }
        fprintf(stderr,"[CHECKPOINT] cold load complete: %s\n",s_cold_checkpoint);
        while(xbox_CheckpointGuestRootCount())Sleep(16);
    } else xbox_RunGuestRoot(xbe_entry_point);
    xbox_FramebufferSetCheckpointHandler(NULL);
#else
    xbox_RunGuestRoot(xbe_entry_point);
#endif

    printf("\nGame returned. Cleaning up...\n");

    /* Cleanup */
    nv2a_fifo_stop(nv2a_get_state());
    xbox_kernel_shutdown();
    nv2a_pb_release_memory_views();
    xbox_MemoryLayoutShutdown();
    free(xbe_data);

    return 0;
}

/* ── XBE Loading ───────────────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size)
{
    FILE *f = nine_fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open XBE: %s\n", path);
        return FALSE;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return FALSE;
    }

    void *data = malloc((size_t)size);
    if (!data) {
        fclose(f);
        return FALSE;
    }

    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return FALSE;
    }

    fclose(f);
    *out_data = data;
    *out_size = (size_t)size;
    return TRUE;
}

/* Console entry point (for debugging -- lets you see printf output) */
int main(int argc, char **argv)
{
    if(!nine_paths_init()) {
        MessageBoxW(NULL,L"无法确定程序所在目录。 / Cannot resolve the executable directory.",L"NINE",MB_OK|MB_ICONERROR);return 1;
    }
    int gui_startup=GetFileType(GetStdHandle(STD_ERROR_HANDLE))==FILE_TYPE_UNKNOWN;
    if(gui_startup) {
        wchar_t path[MAX_PATH];FILE *stream=NULL;
        if(swprintf_s(path,MAX_PATH,L"%s/startup.log",nine_paths.runtime_w)<0 ||
           _wfreopen_s(&stream,path,L"w",stderr)!=0 ||
           _putenv_s("RECOMP_GPU_ERROR_DIALOG","1")!=0) {
            MessageBoxW(NULL,L"无法写入 runtime/startup.log，请检查游戏目录的写入权限。 / Cannot write runtime/startup.log.",L"NINE",MB_OK|MB_ICONERROR);return 1;
        }
        setvbuf(stderr,NULL,_IONBF,0);
    }
    if(!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        fprintf(stderr,"[DISPLAY] per-monitor pixel sizing failed: %lu\n",GetLastError());return 1;
    }
#ifdef SMT9_MACHINE_CHECKPOINT
    if(argc==3 && !strcmp(argv[1],"--load-checkpoint"))s_cold_checkpoint=argv[2];
    else if(argc!=1) {
        fprintf(stderr,"Usage: %s [--load-checkpoint file]\n",argv[0]);return 1;
    }
#else
    (void)argc;
    (void)argv;
#endif
    int result=WinMain(GetModuleHandle(NULL), NULL, GetCommandLineA(), SW_SHOW);
    if(result && gui_startup)MessageBoxW(NULL,L"游戏启动失败，详细信息见 runtime/startup.log。 / Startup failed. See runtime/startup.log.",L"NINE",MB_OK|MB_ICONERROR);
    return result;
}
