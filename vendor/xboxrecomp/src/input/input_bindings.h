#pragma once
#include <wchar.h>

enum { XBOX_BINDING_COUNT=24 };
typedef struct XboxInputBindings {
    unsigned keyboard[XBOX_BINDING_COUNT];
    unsigned gamepad[XBOX_BINDING_COUNT];
} XboxInputBindings;

/* Eight digital buttons, eight pressure buttons, then both sticks' L/R/D/U.
 * Host source zero disables a binding; sources 1..24 use the same order,
 * with LT/RT for White/Black and LB/RB for the original Xbox L/R. */
#define XBOX_DEFAULT_BINDINGS { \
    {0x26,0x28,0x25,0x27,0xA1,0x08,'1','2',0xBE,0xBF,0xBA,0xDE,0xDB,0xDD,'Q','E','A','D','S','W','J','L','K','I'}, \
    {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24} }
static const wchar_t *const xbox_binding_ids[XBOX_BINDING_COUNT]={
    L"DpadUp",L"DpadDown",L"DpadLeft",L"DpadRight",L"Start",L"Back",L"LeftClick",L"RightClick",
    L"A",L"B",L"X",L"Y",L"White",L"Black",L"L",L"R",
    L"LeftStickLeft",L"LeftStickRight",L"LeftStickDown",L"LeftStickUp",
    L"RightStickLeft",L"RightStickRight",L"RightStickDown",L"RightStickUp"
};
static inline int xbox_binding_key_valid(unsigned key) {
    return (key==0 || (key>=8 && key<=255)) && !(key>=0x70 && key<=0x74) && key!=0x77 && key!=0x78;
}
static inline int xbox_bindings_valid(const XboxInputBindings *bindings) {
    for(unsigned i=0;i<XBOX_BINDING_COUNT;i++)
        if(!xbox_binding_key_valid(bindings->keyboard[i]) || bindings->gamepad[i]>XBOX_BINDING_COUNT)return 0;
    return 1;
}
#ifdef _WIN32
#include <windows.h>
#include <xinput.h>
static inline unsigned xbox_input_magnitude(int value,int positive) {
    return positive?(value>0?(unsigned)value:0):(value<0?(unsigned)-value:0);
}
static inline void xbox_input_host_strengths(const XINPUT_GAMEPAD *pad,unsigned values[XBOX_BINDING_COUNT]) {
    for(unsigned i=0;i<8;i++)values[i]=(pad->wButtons&(1u<<i))?32767:0;
    for(unsigned i=0;i<4;i++)values[8+i]=(pad->wButtons&(XINPUT_GAMEPAD_A<<i))?32767:0;
    values[12]=(unsigned)pad->bLeftTrigger*32767/255;
    values[13]=(unsigned)pad->bRightTrigger*32767/255;
    values[14]=(pad->wButtons&XINPUT_GAMEPAD_LEFT_SHOULDER)?32767:0;
    values[15]=(pad->wButtons&XINPUT_GAMEPAD_RIGHT_SHOULDER)?32767:0;
    const SHORT axes[]={pad->sThumbLX,pad->sThumbLY,pad->sThumbRX,pad->sThumbRY};
    for(unsigned i=0;i<4;i++) {
        values[16+i*2]=xbox_input_magnitude(axes[i],0);
        values[17+i*2]=xbox_input_magnitude(axes[i],1);
    }
}
#endif
