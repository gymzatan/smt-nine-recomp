#include "language.h"
#include "game_paths.h"
#include <shellapi.h>
#include <stdio.h>
#include <wchar.h>

static int failure(wchar_t *error,size_t count,const wchar_t *message)
{
    if(error && count)swprintf_s(error,count,L"%s",message);
    return 0;
}
int nine_language_id_valid(const wchar_t *id)
{
    size_t n=wcslen(id);
    if(!n || n>=NINE_LANGUAGE_ID)return 0;
    for(size_t i=0;i<n;i++)
        if(!((id[i]>=L'A'&&id[i]<=L'Z')||(id[i]>=L'a'&&id[i]<=L'z')||
             (i&&((id[i]>=L'0'&&id[i]<=L'9')||id[i]==L'_'||id[i]==L'-'))))return 0;
    return 1;
}
int nine_language_resolve(const wchar_t *id,NineLanguage *language,wchar_t *error,size_t count)
{
    *language=(NineLanguage){0};
    if(!nine_language_id_valid(id))return failure(error,count,L"语言标识须以英文字母开头，最多31位，仅含字母、数字、下划线或连字符。");
    wcscpy_s(language->id,NINE_LANGUAGE_ID,id);
    if(!wcscmp(id,L"Chinese")||!wcscmp(id,L"Japanese")) {
        if(!wcscmp(id,L"Chinese") && !SMT9_WITH_CHINESE)
            return failure(error,count,L"Chinese game support was not built. Build with --with-chinese to enable it.");
        wcscpy_s(language->name,80,!wcscmp(id,L"Chinese")?L"中文":L"日本語");
        language->font_pages=!wcscmp(id,L"Japanese")?9:13;
        return 1;
    }
    wchar_t ini[MAX_PATH],package[100],state[16];
    if(swprintf_s(ini,MAX_PATH,L"%s/%s.ini",SMT9_LANGUAGES_DIR_W,id)<0)return failure(error,count,L"语言路径过长。");
    WritePrivateProfileStringW(NULL,NULL,NULL,ini);
    GetPrivateProfileStringW(L"Language",L"State",L"",state,16,ini);
    GetPrivateProfileStringW(L"Language",L"Package",L"",package,100,ini);
    if(wcscmp(state,L"Ready")||!package[0])return failure(error,count,L"尚未导入该语言，或其原生构建未完成。请先在 Config 中导入语言 ISO。");
    for(const wchar_t *p=package;*p;p++)if(!((*p>=L'A'&&*p<=L'Z')||(*p>=L'a'&&*p<=L'z')||(*p>=L'0'&&*p<=L'9')||*p==L'-'||*p==L'_'))return failure(error,count,L"语言包目录无效。");
    GetPrivateProfileStringW(L"Language",L"Name",id,language->name,80,ini);
    language->font_pages=GetPrivateProfileIntW(L"Language",L"FontPages",0,ini);
    if(!language->font_pages||language->font_pages>128)return failure(error,count,L"语言包字体页数无效。");
    if(swprintf_s(language->directory,MAX_PATH,L"%s/packages/%s",SMT9_LANGUAGES_DIR_W,package)<0 ||
       swprintf_s(language->image,MAX_PATH,L"%s/native_game.xbe",language->directory)<0 ||
       swprintf_s(language->executable,MAX_PATH,L"%s/bin/smt9_recomp.exe",language->directory)<0)
        return failure(error,count,L"语言包路径过长。");
    if(GetFileAttributesW(language->image)==INVALID_FILE_ATTRIBUTES || GetFileAttributesW(language->executable)==INVALID_FILE_ATTRIBUTES)
        return failure(error,count,L"语言包缺少原生镜像或程序，请重新导入。");
    language->external=1;return 1;
}
unsigned nine_language_list(NineLanguage *languages,unsigned capacity)
{
    unsigned count=0;NineLanguage item;
    for(unsigned i=0;i<2&&count<capacity;i++)if(nine_language_resolve(i?L"Japanese":L"Chinese",&item,NULL,0))languages[count++]=item;
    wchar_t pattern[MAX_PATH];WIN32_FIND_DATAW entry;
    swprintf_s(pattern,MAX_PATH,L"%s/*.ini",SMT9_LANGUAGES_DIR_W);
    HANDLE search=FindFirstFileW(pattern,&entry);
    if(search==INVALID_HANDLE_VALUE)return count;
    do {
        if(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)continue;
        wchar_t id[NINE_LANGUAGE_ID];size_t n=wcslen(entry.cFileName);
        if(n<5||n-4>=NINE_LANGUAGE_ID)continue;
        wcsncpy_s(id,NINE_LANGUAGE_ID,entry.cFileName,n-4);
        if(!wcscmp(id,L"Chinese")||!wcscmp(id,L"Japanese"))continue;
        if(count<capacity&&nine_language_resolve(id,&item,NULL,0))languages[count++]=item;
    }while(FindNextFileW(search,&entry));
    FindClose(search);return count;
}
int nine_language_enter(const NineLanguage *language,wchar_t *error,size_t count)
{
    int matching=language->external?!_wcsicmp(language->id,SMT9_COMPILED_LANGUAGE_W):!SMT9_COMPILED_LANGUAGE_W[0];
    wchar_t own[MAX_PATH],selected[MAX_PATH];GetModuleFileNameW(NULL,own,MAX_PATH);
    if(language->external&&!GetFullPathNameW(language->executable,MAX_PATH,selected,NULL))return -1;
    if(matching && (!language->external||!_wcsicmp(own,selected)))return 1;
    const wchar_t *exe=language->external?language->executable:SMT9_BUILTIN_EXECUTABLE_W;
    int argc=0;wchar_t **argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(!argv)return -1;
    wchar_t command[32768];int used=swprintf_s(command,32768,L"\"%s\"",exe);
    for(int i=1;i<argc;i++) {
        if(wcschr(argv[i],L'"')||used<0){LocalFree(argv);failure(error,count,L"启动参数无效。");return -1;}
        int n=swprintf_s(command+used,32768-used,L" \"%s\"",argv[i]);
        if(n<0){LocalFree(argv);failure(error,count,L"启动参数过长。");return -1;}used+=n;
    }
    LocalFree(argv);STARTUPINFOW si={sizeof si};PROCESS_INFORMATION process;
    if(!CreateProcessW(exe,command,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&si,&process)) {
        failure(error,count,L"无法启动所选语言的原生程序，请重新导入语言 ISO。");return -1;
    }
    CloseHandle(process.hThread);CloseHandle(process.hProcess);return 0;
}
