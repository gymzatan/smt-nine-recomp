#include "config.h"
#include "game_paths.h"
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

typedef enum ConfigError {CE_UI,CE_REFRESH,CE_SCALE,CE_SIZE,CE_ASPECT,CE_BACKEND,CE_NUMBERS,CE_GAME,CE_PATH,CE_WRITE,CE_SAVE,CE_FIXED} ConfigError;
static int config_error(const NineConfig *c,wchar_t *error,size_t count,ConfigError code)
{
    static const wchar_t *const messages[][3]={
        {L"界面语言 / 表示言語 / UI language: Chinese, Japanese, English",L"界面语言 / 表示言語 / UI language: Chinese, Japanese, English",L"界面语言 / 表示言語 / UI language: Chinese, Japanese, English"},
        {L"画面刷新率必须为 30 或 60 Hz。",L"リフレッシュレートは 30 または 60 Hz を選択してください。",L"Refresh rate must be 30 or 60 Hz."},
        {L"内部渲染倍率必须为 1～4。",L"内部解像度倍率は 1～4 を選択してください。",L"Internal resolution scale must be between 1 and 4."},
        {L"窗口宽度须为 320～7680，高度须为 240～4320。",L"ウィンドウの幅は 320～7680、高さは 240～4320 で指定してください。",L"Window width must be 320–7680 and height must be 240–4320."},
        {L"请选择 4:3、16:9、16:10 或 21:9 游戏比例。",L"画面比率は 4:3、16:9、16:10、21:9 を選択してください。",L"Choose an aspect ratio of 4:3, 16:9, 16:10 or 21:9."},
        {L"渲染后端名称无效。",L"描画バックエンド名が無効です。",L"Invalid renderer name."},
        {L"NINE.ini 中的显示参数不是有效整数。",L"NINE.ini の表示設定に無効な整数が含まれています。",L"NINE.ini contains an invalid integer display setting."},
        {L"游戏语言标识无效或过长。",L"ゲーム言語 ID が無効、または長すぎます。",L"The game language ID is invalid or too long."},
        {L"配置文件路径过长。",L"設定ファイルのパスが長すぎます。",L"The settings file path is too long."},
        {L"无法写入配置文件。",L"設定ファイルに書き込めません。",L"Could not write the settings file."},
        {L"无法保存配置文件。",L"設定ファイルを保存できません。",L"Could not save the settings file."},
        {L"FixedAspectRatio 必须为 0 或 1。",L"FixedAspectRatio は 0 または 1 で指定してください。",L"FixedAspectRatio must be 0 or 1."}
    };
    unsigned language=!wcscmp(c->interface_language,L"Chinese")?0:!wcscmp(c->interface_language,L"Japanese")?1:2;
    if(error && count)swprintf_s(error,count,L"%s",messages[code][language]);
    return 0;
}
int nine_config_validate(const NineConfig *c,wchar_t *error,size_t count)
{
    if(wcscmp(c->interface_language,L"Chinese") && wcscmp(c->interface_language,L"Japanese") && wcscmp(c->interface_language,L"English"))
        return config_error(c,error,count,CE_UI);
    if(c->refresh_hz!=30 && c->refresh_hz!=60)
        return config_error(c,error,count,CE_REFRESH);
    if(c->render_scale<1 || c->render_scale>4)
        return config_error(c,error,count,CE_SCALE);
    if(c->window_width<320 || c->window_width>7680 || c->window_height<240 || c->window_height>4320)
        return config_error(c,error,count,CE_SIZE);
    if(!((c->aspect_width==4 && c->aspect_height==3) ||
         (c->aspect_width==16 && c->aspect_height==9) ||
         (c->aspect_width==16 && c->aspect_height==10) ||
         (c->aspect_width==21 && c->aspect_height==9)))
        return config_error(c,error,count,CE_ASPECT);
    if(wcscmp(c->backend,L"Auto") && wcscmp(c->backend,L"Vulkan") &&
       wcscmp(c->backend,L"OpenGL") && wcscmp(c->backend,L"D3D12") && wcscmp(c->backend,L"D3D11"))
        return config_error(c,error,count,CE_BACKEND);
    if(!nine_language_id_valid(c->language))return config_error(c,error,count,CE_GAME);
    if(c->fixed_aspect>1)return config_error(c,error,count,CE_FIXED);
    return 1;
}
static int read_number(const wchar_t *key,unsigned initial,unsigned *value)
{
    wchar_t text[32],def[32],*end;
    swprintf_s(def,32,L"%u",initial);
    DWORD length=GetPrivateProfileStringW(L"Display",key,def,text,32,SMT9_CONFIG_PATH_W);
    unsigned long number=wcstoul(text,&end,10);
    if(!length || length>=31 || *end || text[0]==L'-' || number>UINT_MAX) return 0;
    *value=(unsigned)number;return 1;
}
int nine_config_read(NineConfig *c,wchar_t *error,size_t count)
{
    *c=(NineConfig){1,960,720,4,3,L"Auto",SMT9_DEFAULT_GAME_LANGUAGE_W,30,SMT9_DEFAULT_UI_LANGUAGE_W,0};
    if(GetPrivateProfileStringW(L"Interface",L"Language",SMT9_DEFAULT_UI_LANGUAGE_W,c->interface_language,16,SMT9_CONFIG_PATH_W)>=15)
        return config_error(c,error,count,CE_UI);
    DWORD length=GetPrivateProfileStringW(L"Display",L"Backend",L"Auto",c->backend,16,SMT9_CONFIG_PATH_W);
    if(length>=15 || !read_number(L"RenderScale",1,&c->render_scale) ||
       !read_number(L"WindowWidth",960,&c->window_width) ||
       !read_number(L"WindowHeight",720,&c->window_height) ||
       !read_number(L"AspectWidth",4,&c->aspect_width) || !read_number(L"AspectHeight",3,&c->aspect_height) ||
       !read_number(L"RefreshHz",30,&c->refresh_hz) ||
       !read_number(L"FixedAspectRatio",0,&c->fixed_aspect))
        return config_error(c,error,count,CE_NUMBERS);
    wchar_t language_id[NINE_LANGUAGE_ID+1];
    if(GetPrivateProfileStringW(L"Display",L"Language",SMT9_DEFAULT_GAME_LANGUAGE_W,language_id,NINE_LANGUAGE_ID+1,SMT9_CONFIG_PATH_W)>=NINE_LANGUAGE_ID)
        return config_error(c,error,count,CE_GAME);
    wcscpy_s(c->language,NINE_LANGUAGE_ID,language_id);
    return nine_config_validate(c,error,count);
}
int nine_config_write(const NineConfig *c,wchar_t *error,size_t count)
{
    if(!nine_config_validate(c,error,count))return 0;
    wchar_t temporary[MAX_PATH],content[512];
    if(swprintf_s(temporary,MAX_PATH,L"%s.tmp",SMT9_CONFIG_PATH_W)<0)
        return config_error(c,error,count,CE_PATH);
    int length=swprintf_s(content,512,L"[Interface]\r\nLanguage=%s\r\n\r\n[Display]\r\nBackend=%s\r\nLanguage=%s\r\nRenderScale=%u\r\nWindowWidth=%u\r\nWindowHeight=%u\r\nAspectWidth=%u\r\nAspectHeight=%u\r\nRefreshHz=%u\r\nFixedAspectRatio=%u\r\n",
        c->interface_language,c->backend,c->language,c->render_scale,c->window_width,c->window_height,c->aspect_width,c->aspect_height,c->refresh_hz,c->fixed_aspect);
    HANDLE file=CreateFileW(temporary,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file==INVALID_HANDLE_VALUE)return config_error(c,error,count,CE_WRITE);
    WORD bom=0xfeff;DWORD written;DWORD bytes=(DWORD)length*sizeof(wchar_t);
    int ok=WriteFile(file,&bom,2,&written,NULL) && written==2 &&
        WriteFile(file,content,bytes,&written,NULL) && written==bytes && FlushFileBuffers(file);
    if(!CloseHandle(file))ok=0;
    if(ok)ok=MoveFileExW(temporary,SMT9_CONFIG_PATH_W,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
    if(!ok){DeleteFileW(temporary);return config_error(c,error,count,CE_SAVE);}
    WritePrivateProfileStringW(NULL,NULL,NULL,SMT9_CONFIG_PATH_W);
    return 1;
}
