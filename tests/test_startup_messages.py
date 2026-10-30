"""Exercise the production entry before game initialization, with real log opens."""
from pathlib import Path
import re
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope='module', params=[0, 1], ids=['japanese-build', 'chinese-build'])
def startup_probe(tmp_path_factory, request):
    work = tmp_path_factory.mktemp('startup-messages')
    header = (ROOT/'game/src/game_paths.h.in').read_text(encoding='utf-8')
    header = header.replace('#cmakedefine01 SMT9_WITH_CHINESE', '#define SMT9_WITH_CHINESE '+str(request.param))
    for key, value in {'SMT9_ROOT_FROM_EXE': '.', 'SMT9_IMAGE_RELATIVE': 'runtime/native_game.xbe',
                       'SMT9_CHECKPOINT_RELATIVE': 'runtime/instant.state', 'SMT9_IMPORTED_LANGUAGE_ID': ''}.items():
        header = header.replace('@'+key+'@', value)
    (work/'game_paths.h').write_text(header, encoding='utf-8')
    original = (ROOT/'game/src/main.c').read_text(encoding='utf-8')
    display = re.search(r'^static NineConfig s_display[^\n]*;', original, re.M).group()
    entry = original[original.index('/* Console entry point'):]
    source = r'''
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <errno.h>
#include <assert.h>
#include "config.h"
#include "game_paths.h"
''' + display + r'''
static void error_game_context(EXCEPTION_POINTERS *exception) {(void)exception;}
static const char *scenario;
static unsigned dialog_calls, game_calls;
static wchar_t observed[2048];
static int observed_message(HWND owner,LPCWSTR message,LPCWSTR title,UINT flags) {
    assert(!wcscmp(title,L"NINE") && (flags&MB_ICONERROR));
    ++dialog_calls;wcscpy_s(observed,2048,message);return IDOK;
}
static int startup_environment(const char *name,const char *value) {
    if(!strcmp(scenario,"environment"))return ENOMEM;
    return _putenv_s(name,value);
}
int WINAPI WinMain(HINSTANCE a,HINSTANCE b,LPSTR command,int show) {
    ++game_calls;
    return !strcmp(scenario,"game-fail");
}
#define MessageBoxW observed_message
#define _putenv_s startup_environment
#define main production_main
''' + entry + r'''
#undef main
#undef _putenv_s
#undef MessageBoxW
int main(int argc,char **argv) {
    assert(argc==2);scenario=argv[1];
    _set_error_mode(_OUT_TO_STDERR);_set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
    assert(nine_paths_init());
    HANDLE held=INVALID_HANDLE_VALUE;
    wchar_t log[MAX_PATH];assert(swprintf_s(log,MAX_PATH,L"%s/startup.log",nine_paths.runtime_w)>=0);
    if(!strcmp(scenario,"locked") || !strcmp(scenario,"readonly")) {
        held=CreateFileW(log,GENERIC_WRITE,0,NULL,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
        assert(held!=INVALID_HANDLE_VALUE);
        if(!strcmp(scenario,"readonly")) {CloseHandle(held);held=INVALID_HANDLE_VALUE;assert(SetFileAttributesW(log,FILE_ATTRIBUTE_READONLY));}
    }
    if(strcmp(scenario,"redirected"))assert(SetStdHandle(STD_ERROR_HANDLE,INVALID_HANDLE_VALUE));
    char *arguments[]={argv[0],NULL};
    int result=production_main(1,arguments);
    if(held!=INVALID_HANDLE_VALUE)CloseHandle(held);
    if(!strcmp(scenario,"readonly"))assert(SetFileAttributesW(log,FILE_ATTRIBUTE_NORMAL));
    FILE *out=fopen("result.txt","wb");assert(out);
    fprintf(out,"%d %u %u",result,dialog_calls,game_calls);fclose(out);
    out=fopen("message.bin","wb");assert(out);
    assert(fwrite(observed,sizeof(wchar_t),wcslen(observed),out)==wcslen(observed));fclose(out);
    return 0;
}
'''
    (work/'probe.c').write_text(source, encoding='utf-8')
    compiler = shutil.which('cl')
    assert compiler
    subprocess.run([compiler, '/nologo', '/O2', '/utf-8', '/I'+str(work), '/I'+str(ROOT/'game/src'),
                    str(work/'probe.c'), *[str(ROOT/'game/src'/name) for name in ('config.c', 'language.c', 'game_paths.c', 'error_log.c')],
                    '/Fe:'+str(work/'probe.exe'), '/link', 'shell32.lib', 'user32.lib'],
                   cwd=work, check=True, capture_output=True)
    return work/'probe.exe', 'Chinese' if request.param else 'English'


def run_probe(startup_probe, tmp_path, scenario, language=None, invalid=False):
    executable, default = startup_probe
    shutil.copy2(executable, tmp_path/'probe.exe')
    if scenario != 'missing-runtime':
        runtime = tmp_path/'runtime'
        runtime.mkdir()
        if language:
            (runtime/'NINE.ini').write_text('[Interface]\nLanguage='+language+'\n[Display]\nLanguage=Japanese\n'+
                ('RefreshHz=invalid\n' if invalid else ''), encoding='utf-16')
    subprocess.run([str(tmp_path/'probe.exe'), scenario], cwd=tmp_path, check=True, capture_output=True, timeout=10)
    return tuple(map(int, (tmp_path/'result.txt').read_text().split())), (tmp_path/'message.bin').read_bytes().decode('utf-16-le'), language or default


@pytest.mark.parametrize('language', ['Chinese', 'Japanese', 'English'])
def test_log_open_error_follows_saved_config_interface(startup_probe, tmp_path, language):
    result, message, _ = run_probe(startup_probe, tmp_path, 'locked', language)
    assert result == (1, 1, 0)
    expected = {'Chinese': '无法打开启动日志', 'Japanese': '起動ログを開けません', 'English': 'Cannot open the startup log'}
    assert message.startswith(expected[language])
    assert str(tmp_path) in message and 'startup.log' in message and '32' in message
    assert ' / Cannot' not in message


@pytest.mark.parametrize('scenario,code', [('readonly', '5'), ('missing-runtime', '3')])
def test_log_error_retains_real_system_code_and_build_default(startup_probe, tmp_path, scenario, code):
    result, message, language = run_probe(startup_probe, tmp_path, scenario)
    assert result == (1, 1, 0)
    assert message.startswith('无法打开启动日志' if language == 'Chinese' else 'Cannot open the startup log')
    assert 'startup.log' in message and code in message


@pytest.mark.parametrize('language', ['Chinese', 'Japanese', 'English'])
def test_later_startup_error_uses_saved_interface(startup_probe, tmp_path, language):
    result, message, _ = run_probe(startup_probe, tmp_path, 'game-fail', language)
    assert result == (1, 1, 1)
    assert message.startswith({'Chinese': '游戏启动失败', 'Japanese': 'ゲームを起動できません', 'English': 'Startup failed'}[language])
    assert ' / Startup' not in message


@pytest.mark.parametrize('scenario', ['success', 'redirected'])
def test_success_and_redirected_diagnostics_keep_existing_destination(startup_probe, tmp_path, scenario):
    result, message, _ = run_probe(startup_probe, tmp_path, scenario, 'Japanese')
    assert result == (0, 0, 1) and not message
    assert (tmp_path/'runtime/startup.log').exists() == (scenario == 'success')


def test_environment_failure_is_not_reported_as_log_permission(startup_probe, tmp_path):
    result, message, _ = run_probe(startup_probe, tmp_path, 'environment', 'English')
    assert result == (1, 1, 0)
    assert message.startswith('Cannot initialize startup diagnostics')
    assert 'Cannot open' not in message


@pytest.mark.parametrize('language', ['Chinese', 'Japanese', 'English'])
def test_invalid_display_config_keeps_its_interface_error(startup_probe, tmp_path, language):
    result, message, _ = run_probe(startup_probe, tmp_path, 'success', language, invalid=True)
    assert result == (1, 1, 0)
    assert message.startswith({'Chinese': 'NINE.ini 中', 'Japanese': 'NINE.ini の', 'English': 'NINE.ini contains'}[language])
    assert not (tmp_path/'runtime/startup.log').exists()
