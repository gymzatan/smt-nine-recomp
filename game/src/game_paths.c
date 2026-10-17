#include "game_paths.h"
#include <wchar.h>
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
    const struct {const wchar_t *relative;wchar_t *wide;char *utf8;} paths[]={
        {L"data/chinese",NULL,nine_paths.game_dir},
        {SMT9_IMAGE_RELATIVE_W,NULL,nine_paths.xbe},
        {L"runtime/save",NULL,nine_paths.save},
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
FILE *nine_fopen(const char *path,const char *mode)
{
    wchar_t wide[MAX_PATH],wide_mode[16];
    if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,wide,MAX_PATH) ||
       !MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,mode,-1,wide_mode,16))return NULL;
    return _wfopen(wide,wide_mode);
}
