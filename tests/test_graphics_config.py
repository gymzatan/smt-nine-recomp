"""Exercise the real settings dialog and its saved graphics mode."""
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_graphics_mode_defaults_languages_save_and_cancel(tmp_path):
    compiler = shutil.which('cl')
    resource_compiler = shutil.which('rc')
    assert compiler and resource_compiler
    header = (ROOT/'game/src/game_paths.h.in').read_text(encoding='utf-8')
    header = header.replace('#cmakedefine01 SMT9_WITH_CHINESE', '#define SMT9_WITH_CHINESE 1')
    for key, value in {'SMT9_ROOT_FROM_EXE': '.', 'SMT9_IMAGE_RELATIVE': 'runtime/native_game.xbe',
                       'SMT9_CHECKPOINT_RELATIVE': 'runtime/instant.state', 'SMT9_IMPORTED_LANGUAGE_ID': ''}.items():
        header = header.replace('@'+key+'@', value)
    (tmp_path/'game_paths.h').write_text(header, encoding='utf-8')
    (tmp_path/'runtime').mkdir()
    resource = tmp_path/'config.rc'
    resource.write_text((ROOT/'game/src/config.rc').read_text(encoding='utf-8').replace('#include "app.rc"', ''), encoding='utf-8')
    subprocess.run([resource_compiler, '/nologo', '/fo', str(tmp_path/'config.res'), str(resource)], check=True, capture_output=True)
    source = tmp_path/'probe.c'
    source.write_text(r'''
#include <assert.h>
#include "config_main.c"
static void choose_item(HWND w,int id,int value) {
    SendDlgItemMessageW(w,id,CB_SETCURSEL,value,0);
    SendMessageW(w,WM_COMMAND,MAKEWPARAM(id,CBN_SELCHANGE),(LPARAM)GetDlgItem(w,id));
}
int main(void) {
    _set_error_mode(_OUT_TO_STDERR);_set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
    assert(nine_paths_init());wchar_t error[256],text[256];
    assert(nine_config_read(&settings,error,256) && settings.multithreaded_graphics==1);
    assert(!settings.vsync && settings.refresh_hz==30);
    for(unsigned language=0;language<3;language++) {
        HWND w=CreateDialogParamW(GetModuleHandleW(NULL),MAKEINTRESOURCEW(100),NULL,config_dialog,0);assert(w);
        assert(SendDlgItemMessageW(w,114,CB_GETCOUNT,0,0)==2);
        assert(SendDlgItemMessageW(w,114,CB_GETCURSEL,0,0)==!settings.multithreaded_graphics);
        assert(IsDlgButtonChecked(w,135)==BST_UNCHECKED);
        CheckDlgButton(w,135,BST_CHECKED);
        choose_item(w,114,1);choose_item(w,110,language);
        assert(SendDlgItemMessageW(w,114,CB_GETCURSEL,0,0)==1);
        GetDlgItemTextW(w,135,text,256);assert(!wcscmp(text,ui_text[UI_VSYNC][language]));
        assert(IsDlgButtonChecked(w,135)==BST_CHECKED);
        GetDlgItemTextW(w,134,text,256);assert(!wcscmp(text,ui_text[UI_GRAPHICS][language]));
        SendDlgItemMessageW(w,114,CB_GETLBTEXT,0,(LPARAM)text);assert(!wcscmp(text,ui_text[UI_MULTI][language]));
        SendDlgItemMessageW(w,114,CB_GETLBTEXT,1,(LPARAM)text);assert(!wcscmp(text,ui_text[UI_SINGLE][language]));
        RECT client,row,next;GetClientRect(w,&client);GetWindowRect(GetDlgItem(w,114),&row);
        MapWindowPoints(NULL,w,(POINT *)&row,2);GetWindowRect(GetDlgItem(w,122),&next);MapWindowPoints(NULL,w,(POINT *)&next,2);
        assert(row.left>=0 && row.right<=client.right && row.bottom<next.top);
        SendMessageW(w,WM_COMMAND,IDOK,0);DestroyWindow(w);
        assert(nine_config_read(&settings,error,256) && !settings.multithreaded_graphics);
        assert(!wcscmp(settings.interface_language,ui_ids[language]));
        assert(settings.vsync==1 && settings.refresh_hz==30);
        w=CreateDialogParamW(GetModuleHandleW(NULL),MAKEINTRESOURCEW(100),NULL,config_dialog,0);assert(w);
        assert(SendDlgItemMessageW(w,114,CB_GETCURSEL,0,0)==1);
        assert(IsDlgButtonChecked(w,135)==BST_CHECKED);
        CheckDlgButton(w,135,BST_UNCHECKED);
        choose_item(w,114,0);SendMessageW(w,WM_COMMAND,IDCANCEL,0);DestroyWindow(w);
        assert(nine_config_read(&settings,error,256) && !settings.multithreaded_graphics);
        assert(settings.vsync==1);
        w=CreateDialogParamW(GetModuleHandleW(NULL),MAKEINTRESOURCEW(100),NULL,config_dialog,0);assert(w);
        CheckDlgButton(w,135,BST_UNCHECKED);
        choose_item(w,114,0);SendMessageW(w,WM_COMMAND,IDOK,0);DestroyWindow(w);
        assert(nine_config_read(&settings,error,256) && settings.multithreaded_graphics==1 && !settings.vsync);
    }
    return 0;
}
''', encoding='utf-8')
    exe = tmp_path/'probe.exe'
    subprocess.run([compiler, '/nologo', '/utf-8', '/O2', '/I'+str(tmp_path), '/I'+str(ROOT/'game/src'),
                    str(source), *[str(ROOT/'game/src'/name) for name in ('config.c', 'config_controls.c', 'language.c', 'game_paths.c')],
                    str(tmp_path/'config.res'), '/Fe:'+str(exe), '/link', 'shell32.lib', 'user32.lib', 'comdlg32.lib', 'xinput.lib'],
                   cwd=tmp_path, check=True, capture_output=True)
    subprocess.run([str(exe)], cwd=tmp_path, check=True, capture_output=True, timeout=20)
