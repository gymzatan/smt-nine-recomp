#include "config.h"
#include "game_paths.h"
#include <commdlg.h>
#include <stdio.h>
#include "config_ui.h"

static NineConfig settings;
static NineLanguage languages[64];
static unsigned language_count;
static PROCESS_INFORMATION importing;
static unsigned ui_language;
static UiText status_text=UI_IMPORT_HELP;
static const wchar_t *tr(UiText text){return ui_text[text][ui_language];}
static void apply_ui(HWND window);
static wchar_t import_id[NINE_LANGUAGE_ID]=L"English",import_name[80]=L"English";
static INT_PTR CALLBACK controls_dialog(HWND w,UINT m,WPARAM p,LPARAM l) {
    (void)l;
    if(m==WM_INITDIALOG){SetWindowTextW(w,tr(UI_CONTROLS));SetDlgItemTextW(w,301,tr(UI_CONTROLS_BODY));SetDlgItemTextW(w,IDOK,tr(UI_CLOSE));SetFocus(GetDlgItem(w,IDOK));return FALSE;}
    if(m==WM_CLOSE || (m==WM_COMMAND && (LOWORD(p)==IDOK || LOWORD(p)==IDCANCEL))){EndDialog(w,0);return TRUE;}
    return FALSE;
}
static INT_PTR CALLBACK import_dialog(HWND w,UINT m,WPARAM p,LPARAM l)
{
    (void)l;
    if(m==WM_INITDIALOG){
        SendDlgItemMessageW(w,201,EM_SETLIMITTEXT,NINE_LANGUAGE_ID-1,0);SendDlgItemMessageW(w,202,EM_SETLIMITTEXT,79,0);
        SetWindowTextW(w,tr(UI_IMPORT_TITLE));SetDlgItemTextW(w,203,tr(UI_ID));SetDlgItemTextW(w,204,tr(UI_NAME));
        SetDlgItemTextW(w,205,tr(UI_ID_HINT));SetDlgItemTextW(w,IDOK,tr(UI_CHOOSE));SetDlgItemTextW(w,IDCANCEL,tr(UI_CANCEL));
        SetDlgItemTextW(w,201,import_id);SetDlgItemTextW(w,202,import_name);return TRUE;
    }
    if(m==WM_COMMAND && LOWORD(p)==IDCANCEL){EndDialog(w,0);return TRUE;}
    if(m==WM_COMMAND && LOWORD(p)==IDOK){
        GetDlgItemTextW(w,201,import_id,NINE_LANGUAGE_ID);GetDlgItemTextW(w,202,import_name,80);
        if(!nine_language_id_valid(import_id)||!wcscmp(import_id,L"Chinese")||!wcscmp(import_id,L"Japanese")||!import_name[0]||wcschr(import_name,L'"')||wcschr(import_name,L'\n')||wcschr(import_name,L'\r')){
            MessageBoxW(w,tr(UI_ID_ERROR),tr(UI_IMPORT_TITLE),MB_OK|MB_ICONERROR);return TRUE;
        }
        EndDialog(w,1);return TRUE;
    }return FALSE;
}
static void refresh_languages(HWND w,const wchar_t *selected)
{
    SendDlgItemMessageW(w,106,CB_RESETCONTENT,0,0);
    language_count=nine_language_list(languages,64);
    for(unsigned i=0;i<language_count;i++){
        SendDlgItemMessageW(w,106,CB_ADDSTRING,0,(LPARAM)languages[i].name);
        if(!wcscmp(languages[i].id,selected))SendDlgItemMessageW(w,106,CB_SETCURSEL,i,0);
    }
}
static const unsigned aspect[][2]={{4,3},{16,9},{16,10},{21,9}};
static const wchar_t *backends[]={L"Auto",L"Vulkan",L"OpenGL",L"D3D11"};
static void combo(HWND window,int id,const wchar_t *label)
{
    SendDlgItemMessageW(window,id,CB_ADDSTRING,0,(LPARAM)label);
}
static void apply_ui(HWND window)
{
    const struct {int id;UiText text;} labels[]={
        {121,UI_BACKEND},{122,UI_SCALE},{125,UI_ASPECT},{126,UI_GAME},
        {127,UI_REFRESH},{128,UI_SIZE},{129,UI_HINT},{108,UI_IMPORT},
        {111,UI_CONTROLS},{IDOK,UI_SAVE},{IDCANCEL,UI_CANCEL}};
    SetWindowTextW(window,tr(UI_TITLE));
    for(unsigned i=0;i<sizeof labels/sizeof labels[0];i++)SetDlgItemTextW(window,labels[i].id,tr(labels[i].text));
    SetDlgItemTextW(window,109,tr(status_text));
    LRESULT backend=SendDlgItemMessageW(window,101,CB_GETCURSEL,0,0);
    SendDlgItemMessageW(window,101,CB_DELETESTRING,0,0);SendDlgItemMessageW(window,101,CB_INSERTSTRING,0,(LPARAM)tr(UI_AUTO));
    SendDlgItemMessageW(window,101,CB_SETCURSEL,backend,0);
    LRESULT ratio=SendDlgItemMessageW(window,105,CB_GETCURSEL,0,0);
    SendDlgItemMessageW(window,105,CB_DELETESTRING,1,0);SendDlgItemMessageW(window,105,CB_INSERTSTRING,1,(LPARAM)tr(UI_WIDE));
    SendDlgItemMessageW(window,105,CB_DELETESTRING,3,0);SendDlgItemMessageW(window,105,CB_INSERTSTRING,3,(LPARAM)tr(UI_ULTRAWIDE));
    SendDlgItemMessageW(window,105,CB_SETCURSEL,ratio,0);
}
static INT_PTR CALLBACK config_dialog(HWND window,UINT message,WPARAM wparam,LPARAM lparam)
{
    (void)lparam;
    if(message==WM_INITDIALOG) {
        combo(window,110,L"中文");combo(window,110,L"日本語");combo(window,110,L"English");
        for(unsigned i=0;i<3;i++)if(!wcscmp(settings.interface_language,ui_ids[i]))ui_language=i;
        SendDlgItemMessageW(window,110,CB_SETCURSEL,ui_language,0);
        SendMessageW(window,WM_SETICON,ICON_BIG,(LPARAM)LoadIconW(GetModuleHandleW(NULL),MAKEINTRESOURCEW(1)));
        SendMessageW(window,WM_SETICON,ICON_SMALL,(LPARAM)LoadIconW(GetModuleHandleW(NULL),MAKEINTRESOURCEW(1)));
        combo(window,101,L"自动（Vulkan → OpenGL → D3D11）");
        for(unsigned i=1;i<4;i++)combo(window,101,backends[i]);
        for(unsigned i=0;i<4;i++)if(!wcscmp(settings.backend,backends[i]))SendDlgItemMessageW(window,101,CB_SETCURSEL,i,0);
        combo(window,102,L"1×");combo(window,102,L"2×");combo(window,102,L"3×");combo(window,102,L"4×");
        SendDlgItemMessageW(window,102,CB_SETCURSEL,settings.render_scale-1,0);
        combo(window,105,L"4:3");combo(window,105,L"16:9（宽屏）");combo(window,105,L"16:10");combo(window,105,L"21:9（超宽屏）");
        for(unsigned i=0;i<4;i++)if(settings.aspect_width==aspect[i][0] && settings.aspect_height==aspect[i][1])SendDlgItemMessageW(window,105,CB_SETCURSEL,i,0);
        refresh_languages(window,settings.language);
        combo(window,107,L"30 Hz");combo(window,107,L"60 Hz");
        SendDlgItemMessageW(window,107,CB_SETCURSEL,settings.refresh_hz==60,0);
        SetDlgItemInt(window,103,settings.window_width,FALSE);SetDlgItemInt(window,104,settings.window_height,FALSE);
        apply_ui(window);
        return TRUE;
    }
    if(message==WM_COMMAND) {
        if(importing.hProcess && (LOWORD(wparam)==IDOK || LOWORD(wparam)==IDCANCEL))return TRUE;
        if(LOWORD(wparam)==110 && HIWORD(wparam)==CBN_SELCHANGE){
            LRESULT selected=SendDlgItemMessageW(window,110,CB_GETCURSEL,0,0);
            if(selected>=0 && selected<3){ui_language=(unsigned)selected;apply_ui(window);}
            return TRUE;
        }
        if(LOWORD(wparam)==111){DialogBoxParamW(GetModuleHandleW(NULL),MAKEINTRESOURCEW(300),window,controls_dialog,0);return TRUE;}
        if(LOWORD(wparam)==108){
            if(importing.hProcess)return TRUE;
            INT_PTR dialog=DialogBoxParamW(GetModuleHandleW(NULL),MAKEINTRESOURCEW(200),window,import_dialog,0);
            if(dialog==-1){MessageBoxW(window,tr(UI_DIALOG_ERROR),tr(UI_IMPORT_TITLE),MB_OK|MB_ICONERROR);return TRUE;}
            if(dialog!=1)return TRUE;
            wchar_t iso[MAX_PATH]=L"";
            OPENFILENAMEW picker={sizeof picker};picker.hwndOwner=window;
            picker.lpstrFilter=L"Xbox ISO (*.iso)\0*.iso\0";picker.lpstrTitle=tr(UI_CHOOSE);picker.lpstrFile=iso;picker.nMaxFile=MAX_PATH;picker.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
            if(!GetOpenFileNameW(&picker))return TRUE;
            wchar_t command[2048];
            swprintf_s(command,2048,L"\"%s\" \"%s\" --iso \"%s\" --id %s --name \"%s\"",SMT9_PYTHON_W,SMT9_IMPORT_SCRIPT_W,iso,import_id,import_name);
            STARTUPINFOW si={sizeof si};
            if(!CreateProcessW(SMT9_PYTHON_W,command,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&si,&importing)){
                MessageBoxW(window,tr(UI_START_ERROR),tr(UI_IMPORT_TITLE),MB_OK|MB_ICONERROR);return TRUE;
            }
            CloseHandle(importing.hThread);EnableWindow(GetDlgItem(window,108),FALSE);EnableWindow(GetDlgItem(window,IDOK),FALSE);EnableWindow(GetDlgItem(window,IDCANCEL),FALSE);
            status_text=UI_BUSY;SetDlgItemTextW(window,109,tr(status_text));SetTimer(window,1,1000,NULL);return TRUE;
        }
        if(LOWORD(wparam)==IDCANCEL){EndDialog(window,0);return TRUE;}
        if(LOWORD(wparam)==IDOK) {
            BOOL width_ok,height_ok;wchar_t error[256];NineConfig chosen=settings;
            LRESULT backend=SendDlgItemMessageW(window,101,CB_GETCURSEL,0,0),scale=SendDlgItemMessageW(window,102,CB_GETCURSEL,0,0),ratio=SendDlgItemMessageW(window,105,CB_GETCURSEL,0,0);
            if(backend<0 || backend>3 || scale<0 || scale>3 || ratio<0 || ratio>3)return TRUE;
            chosen.window_width=GetDlgItemInt(window,103,&width_ok,FALSE);chosen.window_height=GetDlgItemInt(window,104,&height_ok,FALSE);
            if(!width_ok || !height_ok){MessageBoxW(window,tr(UI_INTEGER_ERROR),tr(UI_TITLE),MB_OK|MB_ICONERROR);return TRUE;}
            LRESULT language=SendDlgItemMessageW(window,106,CB_GETCURSEL,0,0);
            if(language<0 || language>=(LRESULT)language_count)return TRUE;
            LRESULT refresh=SendDlgItemMessageW(window,107,CB_GETCURSEL,0,0);
            if(refresh<0 || refresh>1)return TRUE;
            chosen.refresh_hz=refresh?60:30;
            wcscpy_s(chosen.interface_language,16,ui_ids[ui_language]);
            wcscpy_s(chosen.language,NINE_LANGUAGE_ID,languages[language].id);
            wcscpy_s(chosen.backend,16,backends[backend]);chosen.render_scale=(unsigned)scale+1;
            chosen.aspect_width=aspect[ratio][0];chosen.aspect_height=aspect[ratio][1];
            if(!nine_config_write(&chosen,error,256)){MessageBoxW(window,error,tr(UI_TITLE),MB_OK|MB_ICONERROR);return TRUE;}
            EndDialog(window,1);return TRUE;
        }
    }
    if(message==WM_TIMER && importing.hProcess){
        DWORD result;
        if(GetExitCodeProcess(importing.hProcess,&result)&&result!=STILL_ACTIVE){
            KillTimer(window,1);CloseHandle(importing.hProcess);importing.hProcess=NULL;
            EnableWindow(GetDlgItem(window,108),TRUE);EnableWindow(GetDlgItem(window,IDOK),TRUE);EnableWindow(GetDlgItem(window,IDCANCEL),TRUE);
            if(!result){refresh_languages(window,import_id);status_text=UI_READY;SetDlgItemTextW(window,109,tr(status_text));}
            else {
                wchar_t log[MAX_PATH],error[700];swprintf_s(log,MAX_PATH,L"%s/%s.error.txt",SMT9_LANGUAGES_DIR_W,import_id);
                GetPrivateProfileStringW(L"Import",L"Error",tr(UI_LOG_ERROR),error,700,log);
                status_text=UI_FAILED;SetDlgItemTextW(window,109,tr(status_text));MessageBoxW(window,error,tr(UI_IMPORT_TITLE),MB_OK|MB_ICONERROR);
            }
        }return TRUE;
    }
    if(message==WM_CLOSE){if(!importing.hProcess)EndDialog(window,0);return TRUE;}
    return FALSE;
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE previous,LPWSTR command,int show)
{
    if(!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))return 1;
    (void)previous;(void)show;wchar_t error[256];
    if(!nine_config_read(&settings,error,256)){MessageBoxW(NULL,error,L"NINE 设置",MB_OK|MB_ICONERROR);return 1;}
    if(!wcscmp(command,L"--validate"))return 0;
    return DialogBoxParamW(instance,MAKEINTRESOURCEW(100),NULL,config_dialog,0)==-1?1:0;
}
