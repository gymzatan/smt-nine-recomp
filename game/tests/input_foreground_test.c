#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../vendor/xboxrecomp/src/input/xinput_xbox.h"
#include <xinput.h>

static DWORD foreground_owner;
static HWND foreground_window;
static unsigned state_calls, capability_calls, vibration_calls, keyboard_calls;
static unsigned char held_keys[256];
static WORD host_buttons=XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_START;

static HWND WINAPI test_foreground_window(void) { return foreground_window; }
static DWORD WINAPI test_window_owner(HWND window, LPDWORD owner)
{
    assert(window == foreground_window);
    *owner = foreground_owner;
    return 1;
}
static DWORD WINAPI test_process_id(void) { return 42; }
static DWORD WINAPI test_get_state(DWORD port, XINPUT_STATE *state)
{
    assert(port < 4);
    ++state_calls;
    memset(state, 0, sizeof(*state));
    state->dwPacketNumber = 9;
    state->Gamepad.wButtons = host_buttons;
    state->Gamepad.sThumbLX = 123;
    return ERROR_SUCCESS;
}
static DWORD WINAPI test_set_state(DWORD port, XINPUT_VIBRATION *vibration)
{
    assert(port == 0);
    assert(vibration->wLeftMotorSpeed == 100);
    assert(vibration->wRightMotorSpeed == 200);
    ++vibration_calls;
    return ERROR_SUCCESS;
}
static DWORD WINAPI test_get_capabilities(DWORD port, DWORD flags,
                                         XINPUT_CAPABILITIES *caps)
{
    assert(port == 0 && flags == 0);
    ++capability_calls;
    memset(caps, 0, sizeof(*caps));
    caps->Type = 1;
    caps->SubType = 1;
    return ERROR_SUCCESS;
}
int xbox_FramebufferKeyDown(int key)
{
    ++keyboard_calls;
    return held_keys[key];
}

#define GetForegroundWindow test_foreground_window
#define GetWindowThreadProcessId test_window_owner
#define GetCurrentProcessId test_process_id
#define XInputGetState test_get_state
#define XInputSetState test_set_state
#define XInputGetCapabilities test_get_capabilities
#include "../../vendor/xboxrecomp/src/input/xinput_device.c"

static void check_background(void)
{
    unsigned before_state = state_calls, before_caps = capability_calls;
    unsigned before_vibration = vibration_calls, before_keyboard = keyboard_calls;
    XBOX_INPUT_STATE state, zero = {0};
    XBOX_INPUT_CAPABILITIES caps;
    XBOX_VIBRATION vibration = {100, 200};
    xbox_InputInit();
    memset(&state, 0xFF, sizeof(state));
    assert(xbox_InputGetState(0, &state) == ERROR_DEVICE_NOT_CONNECTED);
    assert(!memcmp(&state, &zero, sizeof(state)));
    assert(xbox_InputGetCapabilities(0, 0, &caps) == ERROR_DEVICE_NOT_CONNECTED);
    assert(xbox_InputSetState(0, &vibration) == ERROR_DEVICE_NOT_CONNECTED);
    assert(!xbox_InputIsConnected(0));
    assert(state_calls == before_state && capability_calls == before_caps);
    assert(vibration_calls == before_vibration && keyboard_calls == before_keyboard);
}

static void check_cxbx_keyboard(void)
{
    XBOX_INPUT_STATE state;
    host_buttons=0;
    held_keys['Q']=1;
    assert(xbox_InputGetState(0,&state)==ERROR_SUCCESS);
    assert(state.Gamepad.wButtons==XBOX_GAMEPAD_LEFT_THUMB);
    assert(state.Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER]==255);
    assert(state.Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE]==0);
    memset(held_keys,0,sizeof held_keys);
    held_keys[VK_LSHIFT]=1;
    assert(xbox_InputGetState(0,&state)==ERROR_SUCCESS && state.Gamepad.wButtons==0);
    held_keys[VK_RSHIFT]=1;
    assert(xbox_InputGetState(0,&state)==ERROR_SUCCESS && state.Gamepad.wButtons==XBOX_GAMEPAD_START);
    memset(held_keys,0,sizeof held_keys);
    held_keys['A']=held_keys['W']=held_keys['J']=held_keys['I']=1;
    held_keys[VK_OEM_PERIOD]=held_keys[VK_OEM_2]=held_keys[VK_OEM_1]=held_keys[VK_OEM_7]=1;
    held_keys['1']=held_keys['2']=held_keys['E']=held_keys['O']=held_keys[VK_BACK]=held_keys[VK_UP]=1;
    assert(xbox_InputGetState(0,&state)==ERROR_SUCCESS);
    assert(state.Gamepad.sThumbLX==-32767 && state.Gamepad.sThumbLY==32767);
    assert(state.Gamepad.sThumbRX==-32767 && state.Gamepad.sThumbRY==32767);
    assert(state.Gamepad.wButtons==(XBOX_GAMEPAD_BACK|XBOX_GAMEPAD_RIGHT_THUMB|XBOX_GAMEPAD_DPAD_UP));
    for(unsigned button=0;button<8;button++)
        assert(state.Gamepad.bAnalogButtons[button]==(button==XBOX_BUTTON_LTRIGGER?0:255));
    held_keys['D']=held_keys['S']=held_keys['L']=held_keys['K']=1;
    assert(xbox_InputGetState(0,&state)==ERROR_SUCCESS);
    assert(state.Gamepad.sThumbLX==123 && state.Gamepad.sThumbLY==0 && state.Gamepad.sThumbRX==0 && state.Gamepad.sThumbRY==0);
    assert(xbox_InputGetState(1,&state)==ERROR_SUCCESS);
    assert(state.Gamepad.wButtons==0 && state.Gamepad.bAnalogButtons[XBOX_BUTTON_A]==0);
    foreground_owner=99;
    check_background();
    memset(held_keys,0,sizeof held_keys);
}

int main(void)
{
    XBOX_INPUT_STATE state;
    XBOX_INPUT_CAPABILITIES caps;
    XBOX_VIBRATION vibration = {100, 200};
    _putenv_s("RECOMP_KEYBOARD", "1");
    foreground_window = (HWND)(uintptr_t)1;
    foreground_owner = 99;
    check_background();
    foreground_window = NULL;
    check_background();

    foreground_window = (HWND)(uintptr_t)1;
    foreground_owner = 42;
    xbox_InputInit();
    assert(state_calls == 4);
    assert(xbox_InputIsConnected(0));
    assert(xbox_InputGetState(0, &state) == ERROR_SUCCESS);
    assert(state_calls == 5 && state.Gamepad.wButtons == XBOX_GAMEPAD_START);
    assert(state.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255);
    assert(state.Gamepad.sThumbLX == 123);
    assert(xbox_InputGetCapabilities(0, 0, &caps) == ERROR_SUCCESS);
    assert(capability_calls == 1 && caps.Type == 1 && caps.SubType == 1);
    assert(xbox_InputSetState(0, &vibration) == ERROR_SUCCESS);
    assert(vibration_calls == 1);

    check_cxbx_keyboard();
    foreground_owner = 99;
    check_background();
    puts("Production input: background host XInput/keyboard calls=0; foreground init/state/capabilities/vibration preserved; focus loss clears input.");
    puts("Cxbx keyboard: Q dual binding, distinct Shift keys, punctuation buttons, both sticks, opposite keys and port0 foreground merge passed.");
    return 0;
}
