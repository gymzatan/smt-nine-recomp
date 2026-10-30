#include "error_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <io.h>
#include <share.h>
#include <signal.h>
#include <errno.h>

static wchar_t error_path[MAX_PATH];
static NineErrorContext error_context;
static LPTOP_LEVEL_EXCEPTION_FILTER previous_exception_filter;
static volatile LONG error_written;
static int normal_return;

static void copy_current_log(FILE *out)
{
    int fd=_fileno(stderr);
    if(fd<0)return;
    intptr_t descriptor=_get_osfhandle(fd);
    if(descriptor==-1 || GetFileType((HANDLE)descriptor)!=FILE_TYPE_DISK)return;
    LARGE_INTEGER size;
    if(!GetFileSizeEx((HANDLE)descriptor,&size) || !size.QuadPart)return;
    /* The GUI opens this original log read/write. Map that same handle:
     * no reopening of its exclusive path and no change to its file cursor. */
    HANDLE mapping=CreateFileMappingW((HANDLE)descriptor,NULL,PAGE_READONLY,0,0,NULL);
    if(!mapping) {fprintf(out,"Current diagnostic log could not be mapped: Windows error %lu\n",GetLastError());return;}
    const char *bytes=MapViewOfFile(mapping,FILE_MAP_READ,0,0,0);
    if(bytes) {
        const size_t head=16384,tail=262144;
        fprintf(out,"\n--- Current run diagnostic log (%lld bytes) ---\n",size.QuadPart);
        if(size.QuadPart<=head+tail)fwrite(bytes,1,(size_t)size.QuadPart,out);
        else {
            fwrite(bytes,1,head,out);
            fprintf(out,"\n--- Middle omitted; original diagnostic log is retained ---\n");
            fwrite(bytes+size.QuadPart-tail,1,tail,out);
        }
        UnmapViewOfFile(bytes);
    } else fprintf(out,"Current diagnostic log view failed: Windows error %lu\n",GetLastError());
    CloseHandle(mapping);
}

static void write_error(const char *reason, unsigned code, int code_known,
                        EXCEPTION_POINTERS *exception)
{
    if(!error_path[0] || InterlockedCompareExchange(&error_written,1,0))return;
    fflush(stderr);
    FILE *out=_wfsopen(error_path,L"wb",_SH_DENYWR);
    if(!out) {
        fprintf(stderr,"[ERROR LOG] Cannot create err.log (C runtime error %d).\n",errno);
        return;
    }
    setvbuf(out,NULL,_IONBF,0);
    SYSTEMTIME now;GetSystemTime(&now);
    fprintf(out,"NINE fatal error\nUTC %04u-%02u-%02u %02u:%02u:%02u\nPID %lu thread %lu\nReason: %s\n",
            now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond,
            GetCurrentProcessId(),GetCurrentThreadId(),reason);
    if(code_known)fprintf(out,"Code: 0x%08X\n",code);
    HMODULE module=GetModuleHandleW(NULL);
    IMAGE_DOS_HEADER *dos=(IMAGE_DOS_HEADER *)module;
    IMAGE_NT_HEADERS *image=(IMAGE_NT_HEADERS *)((BYTE *)module+dos->e_lfanew);
    fprintf(out,"Executable base: %p PE timestamp: %08lX image bytes: %lu\n",
            module,image->FileHeader.TimeDateStamp,image->OptionalHeader.SizeOfImage);
    if(exception) {
        fprintf(out,"Exception: %08lX RIP=%016llX RSP=%016llX\n",
                exception->ExceptionRecord->ExceptionCode,
                (unsigned long long)exception->ContextRecord->Rip,
                (unsigned long long)exception->ContextRecord->Rsp);
        for(DWORD i=0;i<exception->ExceptionRecord->NumberParameters && i<EXCEPTION_MAXIMUM_PARAMETERS;i++)
            fprintf(out,"Exception parameter %lu: %016llX\n",i,
                    (unsigned long long)exception->ExceptionRecord->ExceptionInformation[i]);
    }
    copy_current_log(out);
    fprintf(out,"\n--- Native termination stack ---\n");
    void *frames[64];USHORT count=CaptureStackBackTrace(0,64,frames,NULL);
    for(USHORT i=0;i<count;i++)fprintf(out,"%u: %p\n",i,frames[i]);
    if(error_context) {
        int fd=_fileno(stderr);
        int original=fd>=0?_dup(fd):-1;
        if(original>=0) {
            if(_dup2(_fileno(out),_fileno(stderr))==0) {
                __try {error_context(exception);}
                __except(EXCEPTION_EXECUTE_HANDLER) {fprintf(stderr,"Guest diagnostic context was unreadable.\n");}
                fflush(stderr);
                _dup2(original,_fileno(stderr));
            }
            _close(original);
        }
    }
    fflush(out);_commit(_fileno(out));fclose(out);
}

void nine_error_log_fatal(const char *reason,unsigned code)
{
    write_error(reason,code,1,NULL);
}
void nine_error_log_exception(const char *reason,EXCEPTION_POINTERS *exception)
{
    write_error(reason,exception->ExceptionRecord->ExceptionCode,1,exception);
}
static LONG WINAPI unhandled_error(EXCEPTION_POINTERS *exception)
{
    nine_error_log_exception("Unhandled native exception",exception);
    return previous_exception_filter?previous_exception_filter(exception):EXCEPTION_CONTINUE_SEARCH;
}
static void abort_error(int signal_number)
{
    nine_error_log_fatal("C runtime abort",(unsigned)signal_number);
    signal(SIGABRT,SIG_DFL);raise(SIGABRT);
}
static void exit_error(void)
{
    if(!normal_return)write_error("Runtime terminated before returning normally",0,0,NULL);
}
int nine_error_log_init(const wchar_t *root,NineErrorContext context)
{
    if(swprintf_s(error_path,MAX_PATH,L"%s/err.log",root)<0)return 0;
    error_context=context;
    if(atexit(exit_error)!=0)return 0;
    previous_exception_filter=SetUnhandledExceptionFilter(unhandled_error);
    return signal(SIGABRT,abort_error)!=SIG_ERR;
}
int nine_error_log_finish(int result)
{
    if(result)nine_error_log_fatal("Game startup or execution failed",(unsigned)result);
    normal_return=1;
    return result;
}
