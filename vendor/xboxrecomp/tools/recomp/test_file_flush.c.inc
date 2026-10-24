#include "kernel.h"
#include <assert.h>
#include <stdio.h>
NTSTATUS xbox_kernel_file_volume_status(HANDLE h){(void)h;return STATUS_SUCCESS;}
int main(int argc,char **argv){
 assert(argc==2); HANDLE h=CreateFileA(argv[1],GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);assert(h!=INVALID_HANDLE_VALUE);
 DWORD wrote=0;assert(WriteFile(h,"flush",5,&wrote,NULL)&&wrote==5);XBOX_IO_STATUS_BLOCK ios;ios.Status=0x12345678;ios.Information=999;
 NTSTATUS status=xbox_NtFlushBuffersFile(h,&ios);assert(status==STATUS_SUCCESS&&ios.Status==STATUS_SUCCESS&&ios.Information==0);CloseHandle(h);
 h=CreateFileA(argv[1],GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);assert(h!=INVALID_HANDLE_VALUE);
 assert(!FlushFileBuffers(h)); DWORD actual=GetLastError();assert(actual==ERROR_ACCESS_DENIED);ios.Status=0;ios.Information=999;status=xbox_NtFlushBuffersFile(h,&ios);
 printf("host_error=%lu status=%08lx ios=%08lx information=%llu\n",actual,(unsigned long)status,(unsigned long)ios.Status,(unsigned long long)ios.Information);
 assert(status==STATUS_UNSUCCESSFUL&&ios.Status==status&&ios.Information==0);assert(xbox_LastFileError()==actual);CloseHandle(h);
 assert(xbox_NtFlushBuffersFile(INVALID_HANDLE_VALUE,NULL)==STATUS_UNSUCCESSFUL);DeleteFileA(argv[1]);puts("flush success/failure pass");return 0;
}
/* Unused module dependencies abort if the focused fixture reaches them. */
BOOL xbox_translate_path(const char*a,xbox_host_char*b,DWORD c){abort();}
NTSTATUS xbox_kernel_close_handle(HANDLE a){abort();}
NTSTATUS xbox_kernel_prepare_file_open(PXBOX_OBJECT_ATTRIBUTES a,WCHAR*b,DWORD c,uint32_t*d,int*e){abort();}
NTSTATUS xbox_kernel_create_driver_file(PHANDLE a,ACCESS_MASK b,PXBOX_OBJECT_ATTRIBUTES c,PXBOX_IO_STATUS_BLOCK d,PLARGE_INTEGER e,ULONG f,ULONG g,ULONG h,ULONG i,int*j){abort();}
void xbox_kernel_release_file_device(uint32_t a){abort();}
void xbox_log(int a,const char*b,const char*c,...){abort();}
int nv2a_pb_host_write_begin(void*a,size_t b){abort();}
void nv2a_pb_host_write_end(int a){abort();}
