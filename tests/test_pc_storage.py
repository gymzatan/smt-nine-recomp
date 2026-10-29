"""Exercise the production PC path and file APIs without disk-image backing."""
from pathlib import Path
import shutil
import subprocess
import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize('chinese', [False, True])
def test_directory_storage_and_save_roundtrip(tmp_path, chinese):
    compiler = shutil.which('cl')
    assert compiler, 'requires the existing MSVC environment'
    header=(ROOT/'game/src/game_paths.h.in').read_text(encoding='utf-8')
    header=header.replace('#cmakedefine01 SMT9_WITH_CHINESE',
                          '#define SMT9_WITH_CHINESE '+str(int(chinese)))
    for key,value in {'SMT9_ROOT_FROM_EXE':'.','SMT9_IMAGE_RELATIVE':'runtime/native_game.xbe',
                      'SMT9_CHECKPOINT_RELATIVE':'runtime/instant.state','SMT9_IMPORTED_LANGUAGE_ID':''}.items():
        header=header.replace('@'+key+'@',value)
    (tmp_path/'game_paths.h').write_text(header,encoding='utf-8')
    for name,body in {'data/japanese/Media/language.fixture':b'Japanese',
                      'runtime/languages/packages/English-test/Media/language.fixture':b'English',
                      'runtime/languages/packages/English-test/native_game.xbe':b'fixture',
                      'runtime/languages/packages/English-test/bin/smt9_recomp.exe':b'fixture'}.items():
        p=tmp_path/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(body)
    (tmp_path/'runtime/languages/English.ini').write_text(
        '[Language]\nState=Ready\nPackage=English-test\nName=English\nFontPages=9\n',encoding='utf-16')
    main=(ROOT/'game/src/main.c').read_text(encoding='utf-8')
    start=main.index('    /* Step 4: Set game directory for file I/O path translation */')
    end=main.index('    /* Step 5: Initialize kernel bridge',start)
    startup=main[start:end]
    source = tmp_path/'probe.c'
    source.write_text(r'''
#include "kernel.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shlobj.h>
#include "game_paths.h"
#include "language.h"
#define YOUR_GAME_DIR SMT9_GAME_DIR
static NineLanguage s_language;
static int smt9_is_japanese(void) { return !wcscmp(s_language.id,L"Japanese"); }
void xbox_log(int level, const char *category, const char *format, ...) {}
void xbox_path_set_language_media(const char *, const char *);
const char *xbox_LookupSymbolicLink(const char *name) { return NULL; }
int xbox_SymbolicLinkPath(const char *name,const char *rest,wchar_t *path,DWORD capacity) { return 0; }
/* Object ownership is exercised by the integrated kernel fixture. */
extern NTSTATUS NTAPI NtClose(HANDLE);
NTSTATUS xbox_kernel_close_handle(HANDLE handle) { return NtClose(handle); }
NTSTATUS xbox_kernel_prepare_file_open(PXBOX_OBJECT_ATTRIBUTES oa,WCHAR *path,DWORD size,uint32_t *device,int *volume) {
    *device=0;*volume=0;return xbox_FileObjectPath(oa,path,size)?0:STATUS_OBJECT_PATH_NOT_FOUND;
}
void xbox_kernel_release_file_device(uint32_t device) { assert(!device); }
NTSTATUS xbox_kernel_create_driver_file(PHANDLE a,ACCESS_MASK b,PXBOX_OBJECT_ATTRIBUTES c,PXBOX_IO_STATUS_BLOCK d,PLARGE_INTEGER e,ULONG f,ULONG g,ULONG h,ULONG i,int *handled) {*handled=0;return 0;}
NTSTATUS xbox_kernel_file_volume_status(HANDLE handle) { return 0; }
NTSTATUS xbox_kernel_dismount_file_volume(HANDLE handle) { return STATUS_INVALID_DEVICE_REQUEST; }


/* This isolated filesystem fixture observes the renderer write boundary;
 * real GPU ownership and invalidation are checked by the renderer suite. */
static void *expected_write;static size_t expected_bytes;static unsigned write_open,write_calls;
int nv2a_pb_host_write_begin(void *address,size_t bytes) {
    assert(address==expected_write && bytes==expected_bytes && !write_open);
    write_open=1;++write_calls;return 0;
}
void nv2a_pb_host_write_end(int held) { assert(!held && write_open);write_open=0; }

static int initialize_selected_paths(void) {
@GAME_PATH_SETUP@
    return 0;
}
static void check_selected_paths(const char *root,const char *body) {
    const char *names[]={"\\Device\\CdRom0","D:\\"};
    XBOX_ANSI_STRING name={0};XBOX_OBJECT_ATTRIBUTES oa={0};oa.ObjectName=&name;
    HANDLE handle;XBOX_IO_STATUS_BLOCK ios;WCHAR actual[MAX_PATH],expected[MAX_PATH],joined[MAX_PATH];
    for(unsigned i=0;i<2;i++) {
        name.Buffer=(PCHAR)names[i];name.Length=(USHORT)strlen(names[i]);name.MaximumLength=name.Length+1;
        assert(NT_SUCCESS(xbox_NtCreateFile(&handle,XBOX_GENERIC_READ,&oa,&ios,NULL,0,3,XBOX_FILE_OPEN,XBOX_FILE_DIRECTORY_FILE)));
        xbox_FileHandleClosed(handle);CloseHandle(handle);
        assert(xbox_translate_path(names[i],joined,MAX_PATH));
        assert(GetFullPathNameW(joined,MAX_PATH,actual,NULL));
        assert(MultiByteToWideChar(CP_UTF8,0,root,-1,joined,MAX_PATH));
        assert(GetFullPathNameW(joined,MAX_PATH,expected,NULL));assert(!_wcsicmp(actual,expected));
    }
    const char *file="D:\\Media\\language.fixture";
    name.Buffer=(PCHAR)file;name.Length=(USHORT)strlen(file);name.MaximumLength=name.Length+1;
    assert(NT_SUCCESS(xbox_NtCreateFile(&handle,XBOX_GENERIC_READ,&oa,&ios,NULL,0,3,XBOX_FILE_OPEN,0)));
    char contents[16]={0};LARGE_INTEGER offset={0};expected_write=contents;expected_bytes=strlen(body);
    assert(NT_SUCCESS(xbox_NtReadFile(handle,NULL,NULL,NULL,&ios,contents,(ULONG)expected_bytes,&offset)));
    assert(ios.Information==strlen(body)&&!memcmp(contents,body,strlen(body)));
    xbox_FileHandleClosed(handle);CloseHandle(handle);
}

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
    /* Original XCreateSaveGame first uses CreateDirectory(FILE_CREATE), then
     * writes SaveMeta.xbx. A collision must reach its existing-name check. */
    const char *container="\\Device\\Harddisk0\\partition1\\UDATA\\new-slot";
    name.Buffer=(PCHAR)container;name.Length=(USHORT)strlen(container);name.MaximumLength=name.Length+1;
    assert(NT_SUCCESS(xbox_NtCreateFile(&handle,0x00100001,&oa,&ios,NULL,0x80,3,XBOX_FILE_CREATE,0x4021)));
    xbox_FileHandleClosed(handle);CloseHandle(handle);
    assert(xbox_NtCreateFile(&handle,0x00100001,&oa,&ios,NULL,0x80,3,XBOX_FILE_CREATE,0x4021)==STATUS_OBJECT_NAME_COLLISION);
    const char *metadata="\\Device\\Harddisk0\\partition1\\UDATA\\new-slot\\SaveMeta.xbx";
    name.Buffer=(PCHAR)metadata;name.Length=(USHORT)strlen(metadata);name.MaximumLength=name.Length+1;
    LARGE_INTEGER allocation={.QuadPart=1},metadata_offset={0};
    assert(NT_SUCCESS(xbox_NtCreateFile(&handle,0xC0100000,&oa,&ios,&allocation,4,0,XBOX_FILE_OPEN_IF,0x22)));
    const wchar_t description[]=L"\xFEFFName=PLAY DATA 16\r\n";
    assert(NT_SUCCESS(xbox_NtWriteFile(handle,NULL,NULL,NULL,&ios,(PVOID)description,sizeof description,&metadata_offset)));
    assert(ios.Information==sizeof description);
    xbox_FileHandleClosed(handle);CloseHandle(handle);
    assert(xbox_translate_path("U:\\user.sav",target,MAX_PATH));
    swprintf_s(expected,MAX_PATH,L"%hs\\UserData\\user.sav",user);assert(!_wcsicmp(target,expected));
    xbox_path_set_language_media("English","external/MediaRoot");
    assert(xbox_translate_path("\\Device\\Harddisk0\\partition1\\UDATA",target,MAX_PATH));
    swprintf_s(expected,MAX_PATH,L"%hs\\UDATA",user);assert(!_wcsicmp(target,expected));
    assert(xbox_translate_path("\\Device\\Harddisk0\\Partition5\\cache.tmp",target,MAX_PATH));
    WCHAR normalized[MAX_PATH];assert(GetFullPathNameW(target,MAX_PATH,normalized,NULL));
    swprintf_s(expected,MAX_PATH,L"%hs\\CacheEnglish\\cache.tmp",save);assert(!_wcsicmp(normalized,expected));
    assert(nine_paths_init());wchar_t error[256],chinese_dir[MAX_PATH];
    swprintf_s(chinese_dir,MAX_PATH,L"%s/data/chinese",nine_paths.root_w);
    assert(GetFileAttributesW(chinese_dir)==INVALID_FILE_ATTRIBUTES);
    assert(nine_language_resolve(L"Japanese",&s_language,error,256));
    assert(!initialize_selected_paths());check_selected_paths(SMT9_JP_GAME_DIR,"Japanese");
    assert(GetFileAttributesW(chinese_dir)==INVALID_FILE_ATTRIBUTES);
    assert(nine_language_resolve(L"English",&s_language,error,256));
    char imported[MAX_PATH*4];assert(WideCharToMultiByte(CP_UTF8,0,s_language.directory,-1,imported,sizeof imported,NULL,NULL));
    assert(!initialize_selected_paths());check_selected_paths(imported,"English");
    assert(GetFileAttributesW(chinese_dir)==INVALID_FILE_ATTRIBUTES);
    if(SMT9_WITH_CHINESE) {
        wchar_t media[MAX_PATH],file[MAX_PATH];swprintf_s(media,MAX_PATH,L"%s/Media",chinese_dir);
        assert(GetFullPathNameW(media,MAX_PATH,normalized,NULL));
        assert(SHCreateDirectoryExW(NULL,normalized,NULL)==ERROR_SUCCESS);
        swprintf_s(file,MAX_PATH,L"%s/language.fixture",media);FILE *f=_wfopen(file,L"wb");assert(f);
        assert(fwrite("Chinese",1,7,f)==7);assert(!fclose(f));
        assert(nine_language_resolve(L"Chinese",&s_language,error,256));
        assert(!initialize_selected_paths());check_selected_paths(SMT9_GAME_DIR,"Chinese");
        assert(nine_language_resolve(L"Japanese",&s_language,error,256));
        assert(!initialize_selected_paths());check_selected_paths(SMT9_JP_GAME_DIR,"Japanese");
    } else assert(!nine_language_resolve(L"Chinese",&s_language,error,256));
    return 0;
}
'''.replace('@GAME_PATH_SETUP@',startup), encoding='utf-8')
    kernel = ROOT/'vendor/xboxrecomp/src/kernel'
    built = subprocess.run([compiler,'/nologo','/O2','/MD','/Gy','/utf-8',
        '/I'+str(kernel),'/I'+str(ROOT/'vendor/xboxrecomp/src'),'/I'+str(ROOT/'vendor/xboxrecomp/include'),
        '/I'+str(tmp_path),'/I'+str(ROOT/'game/src'),str(source),
        str(kernel/'kernel_path.c'),str(kernel/'kernel_file.c'),
        str(ROOT/'game/src/game_paths.c'),str(ROOT/'game/src/language.c'),'/Fe:'+str(tmp_path/'probe.exe'),
        '/link','/OPT:REF','shell32.lib','ntdll.lib'],cwd=tmp_path,capture_output=True,text=True)
    assert built.returncode == 0, built.stdout+built.stderr
    checked = subprocess.run([str(tmp_path/'probe.exe')],cwd=tmp_path,capture_output=True,text=True)
    assert checked.returncode == 0, checked.stdout+checked.stderr
