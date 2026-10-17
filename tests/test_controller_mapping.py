"""Exercise the production Windows controller mapping with distinct inputs."""
from pathlib import Path
import shutil
import subprocess


def test_controller_shoulders_and_triggers(tmp_path):
    root = Path(__file__).resolve().parents[1]
    compiler = shutil.which('cl')
    assert compiler, 'requires the existing MSVC environment'
    program = r'''
#include <windows.h>
#include <xinput.h>
#include <assert.h>
static XINPUT_STATE host;
static DWORD test_get_state(DWORD port, XINPUT_STATE *state) {*state=host;return ERROR_SUCCESS;}
static HWND test_foreground(void) {return (HWND)1;}
static DWORD test_owner(HWND window, LPDWORD owner) {*owner=GetCurrentProcessId();return 1;}
#define XInputGetState test_get_state
#define GetForegroundWindow test_foreground
#define GetWindowThreadProcessId test_owner
#include "xinput_device.c"
int xbox_FramebufferKeyDown(int key) {return 0;}
static void check(WORD buttons,BYTE left,BYTE right,BYTE l,BYTE r,BYTE white,BYTE black) {
    memset(&host,0,sizeof host);host.Gamepad.wButtons=buttons;
    host.Gamepad.bLeftTrigger=left;host.Gamepad.bRightTrigger=right;
    host.Gamepad.sThumbLX=-12345;host.Gamepad.sThumbRY=23456;
    XBOX_INPUT_STATE actual;assert(xbox_InputGetState(0,&actual)==ERROR_SUCCESS);
    assert(actual.Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER]==l);
    assert(actual.Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER]==r);
    assert(actual.Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE]==white);
    assert(actual.Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK]==black);
    assert(actual.Gamepad.sThumbLX==-12345 && actual.Gamepad.sThumbRY==23456);
    assert(actual.Gamepad.bAnalogButtons[XBOX_BUTTON_A]==((buttons&XINPUT_GAMEPAD_A)?255:0));
}
int main(void) {
    _putenv_s("RECOMP_KEYBOARD","0");
    check(XINPUT_GAMEPAD_LEFT_SHOULDER,0,0,255,0,0,0);
    check(XINPUT_GAMEPAD_RIGHT_SHOULDER,0,0,0,255,0,0);
    check(0,73,0,0,0,73,0);check(0,0,181,0,0,0,181);
    check(XINPUT_GAMEPAD_A|XINPUT_GAMEPAD_LEFT_SHOULDER|XINPUT_GAMEPAD_RIGHT_SHOULDER,31,219,255,255,31,219);
    check(0,0,0,0,0,0,0);
    return 0;
}
'''
    source=tmp_path/'mapping.c'
    source.write_text(program,encoding='utf-8')
    exe=tmp_path/'mapping.exe'
    subprocess.run([compiler,'/nologo','/utf-8','/O2','/I'+str(root/'vendor/xboxrecomp/src/input'),'/I'+str(root/'vendor/xboxrecomp/src'),str(source),'/Fe:'+str(exe),'/link','user32.lib'],cwd=tmp_path,check=True,capture_output=True)
    subprocess.run([str(exe)],check=True,capture_output=True)
