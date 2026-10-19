#include "config_controls.h"
#include <xinput.h>
#include <stdio.h>

static const wchar_t *const actions[][3]={
    {L"十字键 ↑",L"十字キー ↑",L"D-pad Up"},{L"十字键 ↓",L"十字キー ↓",L"D-pad Down"},
    {L"十字键 ←",L"十字キー ←",L"D-pad Left"},{L"十字键 →",L"十字キー →",L"D-pad Right"},
    {L"Start",L"Start",L"Start"},{L"Back",L"Back",L"Back"},
    {L"左摇杆按下",L"左スティック押下",L"Left stick click"},{L"右摇杆按下",L"右スティック押下",L"Right stick click"},
    {L"A",L"A",L"A"},{L"B",L"B",L"B"},{L"X",L"X",L"X"},{L"Y",L"Y",L"Y"},
    {L"白",L"白",L"White"},{L"黑",L"黒",L"Black"},{L"L",L"L",L"L"},{L"R",L"R",L"R"},
    {L"左摇杆 ←",L"左スティック ←",L"Left stick Left"},{L"左摇杆 →",L"左スティック →",L"Left stick Right"},
    {L"左摇杆 ↓",L"左スティック ↓",L"Left stick Down"},{L"左摇杆 ↑",L"左スティック ↑",L"Left stick Up"},
    {L"右摇杆 ←",L"右スティック ←",L"Right stick Left"},{L"右摇杆 →",L"右スティック →",L"Right stick Right"},
    {L"右摇杆 ↓",L"右スティック ↓",L"Right stick Down"},{L"右摇杆 ↑",L"右スティック ↑",L"Right stick Up"}
};
static const wchar_t *const pad_names[]={L"",L"D-pad ↑",L"D-pad ↓",L"D-pad ←",L"D-pad →",L"Start",L"Back",L"LS click",L"RS click",L"A",L"B",L"X",L"Y",L"LT",L"RT",L"LB",L"RB",L"LS ←",L"LS →",L"LS ↓",L"LS ↑",L"RS ←",L"RS →",L"RS ↓",L"RS ↑"};
enum {CT_TITLE,CT_ACTION,CT_KEYBOARD,CT_GAMEPAD,CT_NONE,CT_DEFAULTS,CT_APPLY,CT_CANCEL,CT_HINT,CT_CAPTURE,CT_LISTEN,CT_STOP};
static const wchar_t *const labels[][3]={
    {L"按键设置",L"入力設定",L"Control bindings"},{L"游戏动作",L"ゲーム操作",L"Game action"},
    {L"键盘",L"キーボード",L"Keyboard"},{L"手柄（XInput）",L"ゲームパッド（XInput）",L"Controller (XInput)"},
    {L"不绑定",L"割り当てなし",L"Unbound"},{L"恢复默认",L"初期設定",L"Restore defaults"},
    {L"应用",L"適用",L"Apply"},{L"取消",L"キャンセル",L"Cancel"},
    {L"选中动作后选择按键，或点击“按键捕获”。\nLS/RS 为左/右摇杆；LB/RB 为肩键，LT/RT 为扳机。\nF1～F5、F8、F9 为保留快捷键。返回主窗口后保存，下次启动生效。",L"操作を選び、入力を選択するか「入力検出」を押します。\nLS/RS：スティック、LB/RB：バンパー、LT/RT：トリガー。\nF1～F5、F8、F9 は予約済み。メイン画面で保存後、次回起動時に反映。",L"Select an action, then choose an input or use Capture.\nLS/RS: sticks; LB/RB: shoulders; LT/RT: triggers.\nF1–F5, F8 and F9 are reserved. Save in the main window; applies next launch."},
    {L"按键捕获",L"入力検出",L"Capture"},{L"请按键／移动摇杆…",L"入力してください…",L"Press an input / move a stick…"},
    {L"停止捕获",L"検出を停止",L"Stop capture"}
};
typedef struct ControlsDialog {
    XboxInputBindings working;
    XboxInputBindings *destination;
    unsigned language,action,capture;
    BYTE held[256],pad_held[4][XBOX_BINDING_COUNT];
} ControlsDialog;
static const wchar_t *label(ControlsDialog *c,unsigned id){return labels[id][c->language];}
static void select_value(HWND w,int id,unsigned value) {
    SendDlgItemMessageW(w,id,CB_SETCURSEL,(WPARAM)-1,0);
    LRESULT count=SendDlgItemMessageW(w,id,CB_GETCOUNT,0,0);
    for(LRESULT i=0;i<count;i++)if((unsigned)SendDlgItemMessageW(w,id,CB_GETITEMDATA,i,0)==value){SendDlgItemMessageW(w,id,CB_SETCURSEL,i,0);return;}
}
static void stop_capture(HWND w,ControlsDialog *c) {
    c->capture=0;KillTimer(w,1);SetDlgItemTextW(w,406,label(c,CT_CAPTURE));SetDlgItemTextW(w,407,label(c,CT_CAPTURE));
    SetDlgItemTextW(w,408,L"");EnableWindow(GetDlgItem(w,401),TRUE);
}
static void show_action(HWND w,ControlsDialog *c) {
    select_value(w,402,c->working.keyboard[c->action]);select_value(w,403,c->working.gamepad[c->action]);
}
static void add_choice(HWND w,int id,const wchar_t *text,unsigned value) {
    LRESULT index=SendDlgItemMessageW(w,id,CB_ADDSTRING,0,(LPARAM)text);
    SendDlgItemMessageW(w,id,CB_SETITEMDATA,index,value);
}
static void start_capture(HWND w,ControlsDialog *c,unsigned mode) {
    stop_capture(w,c);c->capture=mode;
    for(unsigned key=0;key<256;key++)c->held[key]=!!(GetAsyncKeyState(key)&0x8000);
    for(unsigned slot=0;slot<4;slot++) {
        XINPUT_STATE state={0};XInputGetState(slot,&state);
        unsigned strengths[XBOX_BINDING_COUNT];xbox_input_host_strengths(&state.Gamepad,strengths);
        for(unsigned i=0;i<XBOX_BINDING_COUNT;i++)c->pad_held[slot][i]=(BYTE)(strengths[i]>20000);
    }
    SetDlgItemTextW(w,mode==1?406:407,label(c,CT_STOP));SetDlgItemTextW(w,408,label(c,CT_LISTEN));
    EnableWindow(GetDlgItem(w,401),FALSE);SetTimer(w,1,16,NULL);
}
static INT_PTR CALLBACK dialog(HWND w,UINT m,WPARAM p,LPARAM l) {
    ControlsDialog *c=(ControlsDialog *)GetWindowLongPtrW(w,DWLP_USER);
    if(m==WM_INITDIALOG) {
        c=(ControlsDialog *)l;SetWindowLongPtrW(w,DWLP_USER,l);SetWindowTextW(w,label(c,CT_TITLE));
        const struct{int id;unsigned text;} controls[]={{410,CT_ACTION},{411,CT_KEYBOARD},{412,CT_GAMEPAD},{413,CT_HINT},{405,CT_DEFAULTS},{IDOK,CT_APPLY},{IDCANCEL,CT_CANCEL},{406,CT_CAPTURE},{407,CT_CAPTURE}};
        for(unsigned i=0;i<sizeof controls/sizeof controls[0];i++)SetDlgItemTextW(w,controls[i].id,label(c,controls[i].text));
        for(unsigned i=0;i<XBOX_BINDING_COUNT;i++)SendDlgItemMessageW(w,401,LB_ADDSTRING,0,(LPARAM)actions[i][c->language]);
        SendDlgItemMessageW(w,401,LB_SETCURSEL,0,0);
        add_choice(w,402,label(c,CT_NONE),0);
        for(unsigned key=8;key<256;key++) {
            if(!xbox_binding_key_valid(key) || key==VK_SHIFT || key==VK_CONTROL || key==VK_MENU)continue;
            wchar_t name[80];UINT scan=MapVirtualKeyW(key,MAPVK_VK_TO_VSC_EX);
            LONG parameter=(scan&255)<<16;
            /* Navigation keys share scan codes with the numeric keypad. */
            if((scan&0xff00) || (key>=VK_PRIOR && key<=VK_DOWN) ||
               key==VK_INSERT || key==VK_DELETE || key==VK_NUMLOCK)parameter|=1<<24;
            if(!GetKeyNameTextW(parameter,name,80))continue;
            add_choice(w,402,name,key);
        }
        for(unsigned i=0;i<=XBOX_BINDING_COUNT;i++)add_choice(w,403,i?pad_names[i]:label(c,CT_NONE),i);
        show_action(w,c);return TRUE;
    }
    if(!c)return FALSE;
    if(m==WM_ACTIVATE && LOWORD(p)==WA_INACTIVE && c->capture){stop_capture(w,c);return TRUE;}
    if(m==WM_TIMER && c->capture && GetForegroundWindow()==w) {
        if(c->capture==1) {
            for(unsigned key=8;key<256;key++) {
                int down=!!(GetAsyncKeyState(key)&0x8000),edge=down&&!c->held[key];c->held[key]=(BYTE)down;
                if(edge && xbox_binding_key_valid(key) && key!=VK_SHIFT && key!=VK_CONTROL && key!=VK_MENU) {
                    c->working.keyboard[c->action]=key;stop_capture(w,c);show_action(w,c);break;
                }
            }
        } else for(unsigned slot=0;slot<4 && c->capture;slot++) {
            XINPUT_STATE state={0};if(XInputGetState(slot,&state)!=ERROR_SUCCESS)continue;
            unsigned strengths[XBOX_BINDING_COUNT];xbox_input_host_strengths(&state.Gamepad,strengths);
            for(unsigned i=0;i<XBOX_BINDING_COUNT;i++) {
                int down=strengths[i]>20000,edge=down&&!c->pad_held[slot][i];c->pad_held[slot][i]=(BYTE)down;
                if(edge){c->working.gamepad[c->action]=i+1;stop_capture(w,c);show_action(w,c);break;}
            }
        }
        return TRUE;
    }
    if(m==WM_CLOSE || (m==WM_COMMAND && LOWORD(p)==IDCANCEL)) {
        if(c->capture){stop_capture(w,c);return TRUE;}EndDialog(w,0);return TRUE;
    }
    if(m==WM_COMMAND) {
        unsigned id=LOWORD(p),event=HIWORD(p);
        if(id==IDOK){stop_capture(w,c);*c->destination=c->working;EndDialog(w,1);return TRUE;}
        if(id==405){stop_capture(w,c);c->working=(XboxInputBindings)XBOX_DEFAULT_BINDINGS;show_action(w,c);return TRUE;}
        if(id==406 || id==407){unsigned mode=id==406?1:2;if(c->capture==mode)stop_capture(w,c);else start_capture(w,c,mode);return TRUE;}
        if(id==401 && event==LBN_SELCHANGE){c->action=(unsigned)SendDlgItemMessageW(w,401,LB_GETCURSEL,0,0);show_action(w,c);return TRUE;}
        if((id==402 || id==403) && event==CBN_SELCHANGE) {
            stop_capture(w,c);LRESULT selected=SendDlgItemMessageW(w,id,CB_GETCURSEL,0,0);
            if(selected>=0){unsigned value=(unsigned)SendDlgItemMessageW(w,id,CB_GETITEMDATA,selected,0);if(id==402)c->working.keyboard[c->action]=value;else c->working.gamepad[c->action]=value;}
            return TRUE;
        }
    }
    return FALSE;
}
void nine_controls_dialog(HWND parent,XboxInputBindings *bindings,unsigned language) {
    ControlsDialog context={0};context.working=*bindings;context.destination=bindings;context.language=language;
    DialogBoxParamW(GetModuleHandleW(NULL),MAKEINTRESOURCEW(400),parent,dialog,(LPARAM)&context);
}
