"""Exercise the production dialog, resources and capture state machine."""
from pathlib import Path
import shutil
import subprocess

ROOT=Path(__file__).resolve().parents[1]


def test_controls_dialog_capture_apply_cancel_and_defaults(tmp_path):
    compiler=shutil.which('cl');resource_compiler=shutil.which('rc')
    assert compiler and resource_compiler
    resource=tmp_path/'controls.rc'
    resource.write_text((ROOT/'game/src/config.rc').read_text(encoding='utf-8').replace('#include "app.rc"',''),encoding='utf-8')
    subprocess.run([resource_compiler,'/nologo','/fo',str(tmp_path/'controls.res'),str(resource)],check=True,capture_output=True)
    source=tmp_path/'dialog.c'
    source.write_text(r'''
#include <windows.h>
#include <xinput.h>
#include <assert.h>
#include <string.h>
static HWND active;
static unsigned char pressed[256];
static XINPUT_STATE host;
static unsigned mode;
static SHORT WINAPI test_key(int key){return pressed[key]?(SHORT)0x8000:0;}
static HWND WINAPI test_foreground(void){return active;}
static DWORD WINAPI test_pad(DWORD port,XINPUT_STATE *state){*state=host;return port?ERROR_DEVICE_NOT_CONNECTED:ERROR_SUCCESS;}
static INT_PTR WINAPI test_dialog(HINSTANCE,LPCWSTR,HWND,DLGPROC,LPARAM);
#define GetAsyncKeyState test_key
#define GetForegroundWindow test_foreground
#define XInputGetState test_pad
#define DialogBoxParamW test_dialog
#include "config_controls.c"
static void command(HWND w,unsigned id,unsigned notification) {
    SendMessageW(w,WM_COMMAND,MAKEWPARAM(id,notification),(LPARAM)GetDlgItem(w,id));
}
static INT_PTR WINAPI test_dialog(HINSTANCE instance,LPCWSTR resource,HWND parent,DLGPROC callback,LPARAM context) {
    HWND w=CreateDialogParamW(instance,resource,parent,callback,context);assert(w);active=w;
    ControlsDialog *c=(ControlsDialog *)GetWindowLongPtrW(w,DWLP_USER);assert(c);
    assert(SendDlgItemMessageW(w,401,LB_GETCOUNT,0,0)==24);
    LRESULT count=SendDlgItemMessageW(w,402,CB_GETCOUNT,0,0);assert(count>60);
    for(LRESULT i=0;i<count;i++) {
        unsigned key=(unsigned)SendDlgItemMessageW(w,402,CB_GETITEMDATA,i,0);
        assert(xbox_binding_key_valid(key));assert(key!=VK_F8 && key!=VK_F9);
    }
    SendDlgItemMessageW(w,401,LB_SETCURSEL,8,0);command(w,401,LBN_SELCHANGE);
    command(w,406,BN_CLICKED);assert(c->capture==1);
    SendMessageW(w,WM_ACTIVATE,WA_INACTIVE,0);assert(!c->capture);
    pressed['Z']=1;command(w,406,BN_CLICKED);SendMessageW(w,WM_TIMER,1,0);assert(c->capture==1);
    pressed['Z']=0;SendMessageW(w,WM_TIMER,1,0);
    pressed[VK_F8]=1;SendMessageW(w,WM_TIMER,1,0);assert(c->capture==1);
    pressed['Z']=1;SendMessageW(w,WM_TIMER,1,0);
    assert(!c->capture && c->working.keyboard[8]=='Z');
    command(w,407,BN_CLICKED);assert(c->capture==2);
    host.Gamepad.wButtons=XINPUT_GAMEPAD_B;SendMessageW(w,WM_TIMER,1,0);
    assert(!c->capture && c->working.gamepad[8]==10);
    if(mode==2){command(w,405,BN_CLICKED);XboxInputBindings defaults=XBOX_DEFAULT_BINDINGS;assert(!memcmp(&defaults,&c->working,sizeof defaults));}
    command(w,mode==1?IDCANCEL:IDOK,BN_CLICKED);
    DestroyWindow(w);active=NULL;return mode!=1;
}
int main(void) {
    _set_error_mode(_OUT_TO_STDERR);_set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
    for(mode=0;mode<3;mode++) {
        memset(pressed,0,sizeof pressed);memset(&host,0,sizeof host);
        XboxInputBindings bindings=XBOX_DEFAULT_BINDINGS;
        nine_controls_dialog(NULL,&bindings,mode);
        if(!mode)assert(bindings.keyboard[8]=='Z' && bindings.gamepad[8]==10);
        else {XboxInputBindings defaults=XBOX_DEFAULT_BINDINGS;assert(!memcmp(&defaults,&bindings,sizeof defaults));}
    }
    return 0;
}
''',encoding='utf-8')
    exe=tmp_path/'dialog.exe'
    subprocess.run([compiler,'/nologo','/utf-8','/O2','/I'+str(ROOT/'game/src'),str(source),str(tmp_path/'controls.res'),'/Fe:'+str(exe),'/link','user32.lib'],cwd=tmp_path,check=True,capture_output=True)
    subprocess.run([str(exe)],check=True,capture_output=True,timeout=20)
