#include "game_paths.h"
#include <wchar.h>
#include <shlobj.h>
#include "language.h"
NinePaths nine_paths;
int nine_paths_init(void)
{
    wchar_t module[MAX_PATH],joined[MAX_PATH],root[MAX_PATH];
    DWORD length=GetModuleFileNameW(NULL,module,MAX_PATH);
    if(!length || length>=MAX_PATH)return 0;
    wchar_t *end=wcsrchr(module,L'\\');
    if(!end)return 0;
    *end=0;
    if(swprintf_s(joined,MAX_PATH,L"%s/%s",module,SMT9_ROOT_FROM_EXE_W)<0)return 0;
    length=GetFullPathNameW(joined,MAX_PATH,root,NULL);
    if(!length || length>=MAX_PATH)return 0;
    wcscpy_s(nine_paths.root_w,MAX_PATH,root);
    const struct {const wchar_t *relative;wchar_t *wide;char *utf8;} paths[]={
        {L"data/chinese",NULL,nine_paths.game_dir},
        {SMT9_IMAGE_RELATIVE_W,NULL,nine_paths.xbe},
        {L"runtime/save",NULL,nine_paths.save},
        {L"save",nine_paths.user_w,nine_paths.user},
        {SMT9_CHECKPOINT_RELATIVE_W,NULL,nine_paths.checkpoint},
        {L"runtime/NINE.ini",nine_paths.config,NULL},
        {L"runtime/native_game_jp.xbe",NULL,nine_paths.jp_xbe},
        {L"data/japanese",NULL,nine_paths.jp_game_dir},
        {L"runtime",nine_paths.runtime_w,nine_paths.runtime},
        {L"runtime/Screenshots",nine_paths.screenshots,NULL},
        {L"runtime/languages",nine_paths.languages,NULL},
        {L"smt9_recomp.exe",nine_paths.builtin,NULL},
        {L".venv/Scripts/python.exe",nine_paths.python,NULL},
        {L"import_language.py",nine_paths.importer,NULL}
    };
    for(unsigned i=0;i<sizeof paths/sizeof paths[0];i++) {
        if(swprintf_s(joined,MAX_PATH,L"%s/%s",root,paths[i].relative)<0)return 0;
        if(paths[i].wide)wcscpy_s(paths[i].wide,MAX_PATH,joined);
        if(paths[i].utf8 && !WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,joined,-1,paths[i].utf8,MAX_PATH*4,NULL,NULL))return 0;
    }
    return 1;
}
static int save_error(wchar_t *error,size_t count,const wchar_t *path)
{
    DWORD code=GetLastError();
    if(error&&count)swprintf_s(error,count,L"存档目录处理失败 / Save folder operation failed (%lu): %s",code,path);
    return 0;
}
static int save_join(wchar_t *result,const wchar_t *base,const wchar_t *name)
{
    if(swprintf_s(result,MAX_PATH,L"%s/%s",base,name)>=0)return 1;
    SetLastError(ERROR_FILENAME_EXCED_RANGE);return 0;
}
static int save_directory(const wchar_t *path)
{
    DWORD attributes=GetFileAttributesW(path);
    if(attributes!=INVALID_FILE_ATTRIBUTES) {
        if((attributes&FILE_ATTRIBUTE_DIRECTORY)&&!(attributes&FILE_ATTRIBUTE_REPARSE_POINT))return 1;
        SetLastError(ERROR_INVALID_DATA);return 0;
    }
    DWORD code=GetLastError();if(code!=ERROR_FILE_NOT_FOUND&&code!=ERROR_PATH_NOT_FOUND)return 0;
    wchar_t full[MAX_PATH];DWORD length=GetFullPathNameW(path,MAX_PATH,full,NULL);
    if(!length)return 0;
    if(length>=MAX_PATH){SetLastError(ERROR_FILENAME_EXCED_RANGE);return 0;}
    int result=SHCreateDirectoryExW(NULL,full,NULL);
    if(result==ERROR_SUCCESS)return 1;
    SetLastError(result);return 0;
}
static int save_copy_tree(const wchar_t *from,const wchar_t *to,const wchar_t *pattern)
{
    DWORD attributes=GetFileAttributesW(from);
    if(attributes==INVALID_FILE_ATTRIBUTES) {
        DWORD code=GetLastError();return code==ERROR_FILE_NOT_FOUND||code==ERROR_PATH_NOT_FOUND;
    }
    if(!(attributes&FILE_ATTRIBUTE_DIRECTORY)||(attributes&FILE_ATTRIBUTE_REPARSE_POINT)) {SetLastError(ERROR_INVALID_DATA);return 0;}
    if(!save_directory(to))return 0;
    wchar_t search[MAX_PATH];if(!save_join(search,from,pattern))return 0;
    WIN32_FIND_DATAW item;HANDLE scan=FindFirstFileW(search,&item);
    if(scan==INVALID_HANDLE_VALUE)return GetLastError()==ERROR_FILE_NOT_FOUND;
    int ok=1;DWORD code=ERROR_SUCCESS;
    do {
        if(!wcscmp(item.cFileName,L".")||!wcscmp(item.cFileName,L".."))continue;
        if(item.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT){ok=0;SetLastError(ERROR_INVALID_DATA);break;}
        wchar_t input[MAX_PATH],output[MAX_PATH];
        if(!save_join(input,from,item.cFileName)||!save_join(output,to,item.cFileName)){ok=0;break;}
        if(item.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)ok=save_copy_tree(input,output,L"*");
        else ok=CopyFileW(input,output,TRUE)!=0;
        if(!ok)break;
    }while(FindNextFileW(scan,&item));
    code=GetLastError();FindClose(scan);
    if(ok&&code!=ERROR_NO_MORE_FILES){ok=0;}
    if(!ok)SetLastError(code);
    return ok;
}
int nine_saves_prepare(const wchar_t *language,wchar_t *error,size_t count)
{
    if(language&&!nine_language_id_valid(language)){SetLastError(ERROR_INVALID_NAME);return save_error(error,count,language);}
    DWORD attributes=GetFileAttributesW(nine_paths.user_w);
    if(attributes==INVALID_FILE_ATTRIBUTES) {
        DWORD code=GetLastError();
        if(code!=ERROR_FILE_NOT_FOUND&&code!=ERROR_PATH_NOT_FOUND)return save_error(error,count,nine_paths.user_w);
        wchar_t staging[MAX_PATH],from[MAX_PATH],to[MAX_PATH];
        if(!save_join(staging,nine_paths.root_w,L"save.migrating")||!CreateDirectoryW(staging,NULL))return save_error(error,count,L"save.migrating");
        const struct {const wchar_t *old_path,*new_path;} copies[]={
            {L"data/chinese/UDATA",L"UDATA"},{L"data/chinese/TDATA",L"TDATA"},
            {L"runtime/save/UserData",L"UserData"}
        };
        for(unsigned i=0;i<sizeof copies/sizeof copies[0];i++) {
            if(!save_join(from,nine_paths.root_w,copies[i].old_path)||!save_join(to,staging,copies[i].new_path)||
               !save_copy_tree(from,to,L"*"))return save_error(error,count,copies[i].old_path);
        }
        if(!save_join(to,staging,L"states/legacy-built-in")||!save_copy_tree(nine_paths.runtime_w,to,L"instant*.state"))return save_error(error,count,L"runtime instant states");
        wchar_t packages[MAX_PATH],search[MAX_PATH];
        if(!save_join(packages,nine_paths.languages,L"packages")||!save_join(search,packages,L"*"))return save_error(error,count,L"language packages");
        WIN32_FIND_DATAW item;HANDLE scan=FindFirstFileW(search,&item);
        if(scan!=INVALID_HANDLE_VALUE) {
            int ok=1;DWORD failure=ERROR_SUCCESS;
            do {
                if(!wcscmp(item.cFileName,L".")||!wcscmp(item.cFileName,L"..")||!(item.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))continue;
                if(item.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT){ok=0;SetLastError(ERROR_INVALID_DATA);break;}
                wchar_t archive[MAX_PATH];
                if(!save_join(from,packages,item.cFileName)||!save_join(archive,staging,L"states/legacy-imports")||
                   !save_join(to,archive,item.cFileName)||!save_copy_tree(from,to,L"instant*.state")){ok=0;break;}
            }while(FindNextFileW(scan,&item));
            failure=GetLastError();FindClose(scan);
            if(!ok||failure!=ERROR_NO_MORE_FILES){SetLastError(failure);return save_error(error,count,packages);}
        } else {DWORD code=GetLastError();if(code!=ERROR_FILE_NOT_FOUND&&code!=ERROR_PATH_NOT_FOUND)return save_error(error,count,packages);}
        if(!MoveFileExW(staging,nine_paths.user_w,MOVEFILE_WRITE_THROUGH))return save_error(error,count,nine_paths.user_w);
    } else if(!save_directory(nine_paths.user_w))return save_error(error,count,nine_paths.user_w);
    wchar_t user_data[MAX_PATH];
    if(!save_join(user_data,nine_paths.user_w,L"UserData")||!save_directory(user_data))return save_error(error,count,nine_paths.user_w);
    if(language) {
        wchar_t states[MAX_PATH],directory[MAX_PATH],file[MAX_PATH];
        if(!save_join(states,nine_paths.user_w,L"states")||!save_join(directory,states,language)||!save_directory(directory)||
           !save_join(file,directory,L"instant.state")||
           !WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,file,-1,nine_paths.checkpoint,sizeof nine_paths.checkpoint,NULL,NULL))return save_error(error,count,nine_paths.user_w);
    }
    return 1;
}
FILE *nine_fopen(const char *path,const char *mode)
{
    wchar_t wide[MAX_PATH],wide_mode[16];
    if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,wide,MAX_PATH) ||
       !MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,mode,-1,wide_mode,16))return NULL;
    return _wfopen(wide,wide_mode);
}
