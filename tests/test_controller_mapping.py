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
static unsigned char keys[256];
static DWORD test_get_state(DWORD port, XINPUT_STATE *state) {*state=host;return ERROR_SUCCESS;}
static HWND test_foreground(void) {return (HWND)1;}
static DWORD test_owner(HWND window, LPDWORD owner) {*owner=GetCurrentProcessId();return 1;}
#define XInputGetState test_get_state
#define GetForegroundWindow test_foreground
#define GetWindowThreadProcessId test_owner
#include "xinput_device.c"
int xbox_FramebufferKeyDown(int key) {return key>=0 && key<256 && keys[key];}
static void keyboard_key(int key,WORD digital,int analog) {
    memset(keys,0,sizeof keys);keys[key]=1;
    XBOX_INPUT_STATE actual;keyboard_state(&actual);
    assert(actual.Gamepad.wButtons==digital);
    for(int i=0;i<8;i++)assert(actual.Gamepad.bAnalogButtons[i]==(i==analog?255:0));
    assert(!actual.Gamepad.sThumbLX && !actual.Gamepad.sThumbLY &&
           !actual.Gamepad.sThumbRX && !actual.Gamepad.sThumbRY);
}
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
    keyboard_key('1',XBOX_GAMEPAD_LEFT_THUMB,-1);
    keyboard_key('2',XBOX_GAMEPAD_RIGHT_THUMB,-1);
    keyboard_key(VK_OEM_4,0,XBOX_BUTTON_WHITE);
    keyboard_key(VK_OEM_6,0,XBOX_BUTTON_BLACK);
    keyboard_key('Q',0,XBOX_BUTTON_LTRIGGER);
    keyboard_key('E',0,XBOX_BUTTON_RTRIGGER);
    keyboard_key('O',0,-1);
    for(unsigned value=0;value<256;value++)check(0,(BYTE)value,(BYTE)(255-value),0,0,(BYTE)value,(BYTE)(255-value));
    XboxInputBindings custom=XBOX_DEFAULT_BINDINGS;
    custom.keyboard[8]='Z';custom.gamepad[8]=10;custom.gamepad[9]=0;
    custom.gamepad[16]=18;custom.gamepad[17]=17;
    assert(xbox_InputConfigure(&custom));
    keyboard_key('Z',0,XBOX_BUTTON_A);keyboard_key(VK_OEM_PERIOD,0,-1);
    memset(&host,0,sizeof host);host.Gamepad.wButtons=XINPUT_GAMEPAD_B;host.Gamepad.sThumbLX=-32768;
    XBOX_INPUT_STATE actual;assert(xbox_InputGetState(0,&actual)==ERROR_SUCCESS);
    assert(actual.Gamepad.bAnalogButtons[XBOX_BUTTON_A]==255 && !actual.Gamepad.bAnalogButtons[XBOX_BUTTON_B]);
    assert(actual.Gamepad.sThumbLX==32767);
    host.Gamepad.sThumbLX=32767;assert(xbox_InputGetState(0,&actual)==ERROR_SUCCESS);
    assert(actual.Gamepad.sThumbLX==-32767);
    custom.keyboard[8]=0;assert(xbox_InputConfigure(&custom));keyboard_key('Z',0,-1);
    custom.gamepad[8]=25;assert(!xbox_InputConfigure(&custom));
    custom.gamepad[8]=10;custom.keyboard[8]=VK_F1;assert(!xbox_InputConfigure(&custom));
    custom=(XboxInputBindings)XBOX_DEFAULT_BINDINGS;assert(xbox_InputConfigure(&custom));
    keyboard_key(VK_OEM_PERIOD,0,XBOX_BUTTON_A);
    return 0;
}
'''
    source=tmp_path/'mapping.c'
    source.write_text(program,encoding='utf-8')
    exe=tmp_path/'mapping.exe'
    subprocess.run([compiler,'/nologo','/utf-8','/O2','/I'+str(root/'vendor/xboxrecomp/src/input'),'/I'+str(root/'vendor/xboxrecomp/src'),str(source),'/Fe:'+str(exe),'/link','user32.lib'],cwd=tmp_path,check=True,capture_output=True)
    subprocess.run([str(exe)],check=True,capture_output=True)
