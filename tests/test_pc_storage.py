"""Exercise the production PC path and file APIs without disk-image backing."""
from pathlib import Path
import shutil
import subprocess
import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_directory_storage_and_save_roundtrip(tmp_path):
    compiler = shutil.which('cl')
    assert compiler, 'requires the existing MSVC environment'
    source = tmp_path/'probe.c'
    source.write_text(r'''
#include "kernel.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void xbox_log(int level, const char *category, const char *format, ...) {}
void xbox_path_set_language_media(const char *, const char *);
const char *xbox_LookupSymbolicLink(const char *name) { return NULL; }
/* This isolated filesystem fixture observes the renderer write boundary;
 * real GPU ownership and invalidation are checked by the renderer suite. */
static void *expected_write;static size_t expected_bytes;static unsigned write_open,write_calls;
int nv2a_pb_host_write_begin(void *address,size_t bytes) {
    assert(address==expected_write && bytes==expected_bytes && !write_open);
    write_open=1;++write_calls;return 0;
}
void nv2a_pb_host_write_end(int held) { assert(!held && write_open);write_open=0; }

int main(void) {
    _set_error_mode(_OUT_TO_STDERR);_set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
    char root[MAX_PATH],game[MAX_PATH],save[MAX_PATH],user[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH,root);
    sprintf_s(game,MAX_PATH,"%s\\game",root);
    sprintf_s(save,MAX_PATH,"%s\\runtime\\save",root);
    CreateDirectoryA(game,NULL);
    xbox_path_init(game,save);
    sprintf_s(user,MAX_PATH,"%s\\save",root);CreateDirectoryA(user,NULL);
    assert(xbox_path_set_user_directory(user));
    WCHAR pattern[MAX_PATH];swprintf_s(pattern,MAX_PATH,L"%hs\\Partition*.img",save);
    WIN32_FIND_DATAW found;HANDLE scan=FindFirstFileW(pattern,&found);
    if(scan!=INVALID_HANDLE_VALUE){FindClose(scan);fprintf(stderr,"unexpected partition image\n");return 2;}
    WCHAR target[MAX_PATH],expected[MAX_PATH];
    assert(xbox_translate_path("\\Device\\Harddisk0\\partition1\\",target,MAX_PATH));
    swprintf_s(expected,MAX_PATH,L"%hs",user);assert(!_wcsicmp(target,expected));
    assert(xbox_translate_path("\\Device\\Harddisk0\\Partition5\\",target,MAX_PATH));
    swprintf_s(expected,MAX_PATH,L"%hs\\Cache",save);assert(!_wcsicmp(target,expected));
    XBOX_ANSI_STRING name={0};XBOX_OBJECT_ATTRIBUTES oa={0};
    const char *directory="\\Device\\Harddisk0\\partition1\\UDATA";
    name.Buffer=(PCHAR)directory;name.Length=(USHORT)strlen(directory);name.MaximumLength=name.Length+1;oa.ObjectName=&name;
    HANDLE handle;XBOX_IO_STATUS_BLOCK ios;
    assert(NT_SUCCESS(xbox_NtCreateFile(&handle,XBOX_GENERIC_READ,&oa,&ios,NULL,0,3,XBOX_FILE_OPEN_IF,XBOX_FILE_DIRECTORY_FILE)));
    XBOX_FILE_FS_SIZE_INFORMATION volume;
    assert(NT_SUCCESS(xbox_NtQueryVolumeInformationFile(handle,&ios,&volume,sizeof(volume),XboxFileFsSizeInformation)));
    assert(volume.BytesPerSector*volume.SectorsPerAllocationUnit==0x4000);
    xbox_FileHandleClosed(handle);CloseHandle(handle);
    const char *file="\\Device\\Harddisk0\\partition1\\UDATA\\roundtrip.sav";
    name.Buffer=(PCHAR)file;name.Length=(USHORT)strlen(file);name.MaximumLength=name.Length+1;
    assert(NT_SUCCESS(xbox_NtCreateFile(&handle,XBOX_GENERIC_READ|XBOX_GENERIC_WRITE,&oa,&ios,NULL,0,3,XBOX_FILE_CREATE,0)));
    LARGE_INTEGER offset={0};const char payload[]="existing-save-path";char actual[sizeof(payload)]={0};
    assert(NT_SUCCESS(xbox_NtWriteFile(handle,NULL,NULL,NULL,&ios,(PVOID)payload,sizeof(payload),&offset)));
    xbox_FileHandleClosed(handle);CloseHandle(handle);
    assert(NT_SUCCESS(xbox_NtCreateFile(&handle,XBOX_GENERIC_READ,&oa,&ios,NULL,0,3,XBOX_FILE_OPEN,0)));
    expected_write=actual;expected_bytes=sizeof(actual);
    assert(NT_SUCCESS(xbox_NtReadFile(handle,NULL,NULL,NULL,&ios,actual,sizeof(actual),&offset)));
    assert(write_calls==1 && !write_open);
    assert(ios.Information==sizeof(payload)&&!memcmp(actual,payload,sizeof(payload)));
    xbox_FileHandleClosed(handle);CloseHandle(handle);
    assert(xbox_translate_path("U:\\user.sav",target,MAX_PATH));
    swprintf_s(expected,MAX_PATH,L"%hs\\UserData\\user.sav",user);assert(!_wcsicmp(target,expected));
    xbox_path_set_language_media("English","external/MediaRoot");
    assert(xbox_translate_path("\\Device\\Harddisk0\\partition1\\UDATA",target,MAX_PATH));
    swprintf_s(expected,MAX_PATH,L"%hs\\UDATA",user);assert(!_wcsicmp(target,expected));
    assert(xbox_translate_path("\\Device\\Harddisk0\\Partition5\\cache.tmp",target,MAX_PATH));
    WCHAR normalized[MAX_PATH];assert(GetFullPathNameW(target,MAX_PATH,normalized,NULL));
    swprintf_s(expected,MAX_PATH,L"%hs\\CacheEnglish\\cache.tmp",save);assert(!_wcsicmp(normalized,expected));
    return 0;
}
''', encoding='utf-8')
    kernel = ROOT/'vendor/xboxrecomp/src/kernel'
    built = subprocess.run([compiler,'/nologo','/O2','/MD','/Gy','/utf-8',
        '/I'+str(kernel),'/I'+str(ROOT/'vendor/xboxrecomp/src'),'/I'+str(ROOT/'vendor/xboxrecomp/include'),str(source),
        str(kernel/'kernel_path.c'),str(kernel/'kernel_file.c'),'/Fe:'+str(tmp_path/'probe.exe'),
        '/link','/OPT:REF','shell32.lib'],cwd=tmp_path,capture_output=True,text=True)
    assert built.returncode == 0, built.stdout+built.stderr
    checked = subprocess.run([str(tmp_path/'probe.exe')],cwd=tmp_path,capture_output=True,text=True)
    assert checked.returncode == 0, checked.stdout+checked.stderr
