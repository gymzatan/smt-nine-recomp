"""Exercise the real fatal writer in child processes, without running the game."""
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope='module')
def error_probe(tmp_path_factory):
    work = tmp_path_factory.mktemp('error-log')
    source = r'''
#include "error_log.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
static int invalid_context;
static void context(EXCEPTION_POINTERS *exception) {
    if(invalid_context)RaiseException(0xe0004567,0,0,NULL);
    fprintf(stderr,"EXISTING GUEST CONTEXT %s\n",exception?"exception":"stop");
}
int main(int argc,char **argv) {
    assert(argc==2);SetErrorMode(SEM_NOGPFAULTERRORBOX|SEM_FAILCRITICALERRORS);
    _set_error_mode(_OUT_TO_STDERR);_set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
    wchar_t root[MAX_PATH];assert(GetCurrentDirectoryW(MAX_PATH,root));
    FILE *stream;assert(!_wfreopen_s(&stream,L"startup.log",L"w+",stderr));
    setvbuf(stderr,NULL,_IONBF,0);
    assert(nine_error_log_init(root,context));
    fprintf(stderr,"GPU ADAPTER AND SETTINGS\n");
    if(!strcmp(argv[1],"normal"))return nine_error_log_finish(0);
    if(!strcmp(argv[1],"handled")) {
        __try {RaiseException(0xe0424242,0,0,NULL);}
        __except(EXCEPTION_EXECUTE_HANDLER) {}
        return nine_error_log_finish(0);
    }
    if(!strcmp(argv[1],"exception"))RaiseException(0xe0424242,EXCEPTION_NONCONTINUABLE,0,NULL);
    if(!strcmp(argv[1],"abort"))abort();
    if(!strcmp(argv[1],"blocked")) {
        HANDLE held=CreateFileW(L"err.log",GENERIC_READ,0,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
        assert(held!=INVALID_HANDLE_VALUE);fprintf(stderr,"[STOP] locked error destination\n");
        exit(5);
    }
    if(!strcmp(argv[1],"large")) {
        for(unsigned i=0;i<100000;i++)fprintf(stderr,"ordinary diagnostic middle %u\n",i);
        fprintf(stderr,"[STOP] ACTUAL FATAL REASON\n");exit(5);
    }
    if(!strcmp(argv[1],"callback")) {
        fprintf(stderr,"[STOP] guest exception DEADBEEF\n");
        nine_error_log_fatal("Unhandled guest exception",0xdeadbeef);ExitProcess(5);
    }
    if(!strcmp(argv[1],"context-fault")) {
        /* The writer must preserve its error record if guest memory is gone. */
        invalid_context=1;
        nine_error_log_fatal("Native diagnostic test",7);ExitProcess(7);
    }
    fprintf(stderr,"[STOP] ACTUAL FATAL REASON\n");exit(5);
}
'''
    (work/'probe.c').write_text(source, encoding='utf-8')
    compiler = shutil.which('cl')
    assert compiler
    result = subprocess.run([compiler, '/nologo', '/O2', '/utf-8', '/I'+str(ROOT/'game/src'),
                             str(work/'probe.c'), str(ROOT/'game/src/error_log.c'),
                             '/Fe:'+str(work/'probe.exe')], cwd=work, capture_output=True, text=True)
    assert result.returncode == 0, result.stdout+result.stderr
    return work/'probe.exe'


def run(error_probe, tmp_path, mode, previous=True):
    if previous:
        (tmp_path/'err.log').write_bytes(b'PREVIOUS FATAL LOG')
    before = (tmp_path/'err.log').stat().st_mtime_ns if previous else None
    result = subprocess.run([str(error_probe), mode], cwd=tmp_path, capture_output=True, timeout=15)
    path = tmp_path/'err.log'
    return result, path, before


@pytest.mark.parametrize('mode', ['normal', 'handled'])
def test_normal_and_handled_exception_keep_previous_report(error_probe, tmp_path, mode):
    result, path, before = run(error_probe, tmp_path, mode)
    assert result.returncode == 0
    assert path.read_bytes() == b'PREVIOUS FATAL LOG' and path.stat().st_mtime_ns == before


def test_success_creates_no_error_file(error_probe, tmp_path):
    result, path, _ = run(error_probe, tmp_path, 'normal', previous=False)
    assert result.returncode == 0 and not path.exists()


@pytest.mark.parametrize('mode,reason', [('stop', 'ACTUAL FATAL REASON'),
                                      ('exception', 'Unhandled native exception'),
                                      ('abort', 'C runtime abort'),
                                      ('callback', 'Unhandled guest exception')])
def test_fatal_endpoints_record_current_output_and_context(error_probe, tmp_path, mode, reason):
    result, path, _ = run(error_probe, tmp_path, mode)
    assert result.returncode != 0
    content = path.read_text(encoding='utf-8')
    assert 'NINE fatal error' in content and reason in content
    assert 'GPU ADAPTER AND SETTINGS' in content
    assert 'Native termination stack' in content and 'PE timestamp' in content
    assert 'EXISTING GUEST CONTEXT' in content
    assert 'PREVIOUS FATAL LOG' not in content
    if mode == 'exception':
        assert 'E0424242' in content and 'RIP=' in content
    if mode == 'callback':
        assert 'DEADBEEF' in content
    else:
        assert reason in (tmp_path/'startup.log').read_text() or mode in ('abort', 'exception')


def test_long_log_keeps_settings_and_fatal_tail(error_probe, tmp_path):
    result, path, _ = run(error_probe, tmp_path, 'large')
    assert result.returncode == 5
    content = path.read_text()
    assert 'GPU ADAPTER AND SETTINGS' in content and 'ACTUAL FATAL REASON' in content
    assert 'Middle omitted' in content and path.stat().st_size < 300000
    assert (tmp_path/'startup.log').stat().st_size > 1000000


def test_unwritable_error_log_preserves_original_termination(error_probe, tmp_path):
    result, path, _ = run(error_probe, tmp_path, 'blocked')
    assert result.returncode == 5 and path.read_bytes() == b'PREVIOUS FATAL LOG'
    assert 'Cannot create err.log' in (tmp_path/'startup.log').read_text()


def test_unreadable_guest_context_keeps_the_fatal_record(error_probe, tmp_path):
    result, path, _ = run(error_probe, tmp_path, 'context-fault')
    assert result.returncode == 7
    content = path.read_text()
    assert 'Native diagnostic test' in content and 'Guest diagnostic context was unreadable' in content


@pytest.fixture(scope='module')
def guest_context_probe(tmp_path_factory):
    work = tmp_path_factory.mktemp('guest-error-context')
    original = (ROOT/'game/src/main.c').read_text(encoding='utf-8')
    begin = original.index('static void print_guest_context(')
    end = original.index('static void error_game_context(', begin)
    source = r'''
#include <windows.h>
#include <dbghelp.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
static ptrdiff_t g_xbox_mem_offset;
static uint32_t g_esp,g_xbox_code_lo=0x11000,g_xbox_code_hi=0x20000;
volatile uint32_t g_icall_trace[16],g_icall_trace_idx;
''' + original[begin:end] + r'''
int main(int argc,char **argv) {
    assert(argc==2);SetErrorMode(SEM_NOGPFAULTERRORBOX);
    uint32_t *pages=VirtualAlloc(NULL,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);assert(pages);
    pages[0]=0x12345;pages[1023]=0x12346;
    g_xbox_mem_offset=(ptrdiff_t)pages-0x10000;
    if(!strcmp(argv[1],"full"))g_esp=0x10000;
    else if(!strcmp(argv[1],"partial")) {
        DWORD old;assert(VirtualProtect(pages+1024,4096,PAGE_NOACCESS,&old));g_esp=0x10ffc;
    } else g_esp=0xffffffff;
    print_guest_context(NULL);
    VirtualFree(pages,0,MEM_RELEASE);return 0;
}
'''
    (work/'context.c').write_text(source, encoding='utf-8')
    result = subprocess.run([shutil.which('cl'), '/nologo', '/O2', '/utf-8', str(work/'context.c'),
                             '/Fe:'+str(work/'context.exe'), '/link', 'dbghelp.lib'],
                            cwd=work, capture_output=True, text=True)
    assert result.returncode == 0, result.stdout+result.stderr
    return work/'context.exe'


@pytest.mark.parametrize('mode', ['full', 'partial', 'unmapped'])
def test_original_guest_context_reader_stays_inside_readable_memory(guest_context_probe, mode):
    result = subprocess.run([str(guest_context_probe), mode], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stdout+result.stderr
    if mode == 'full':
        assert '0x00012345' in result.stderr
    else:
        assert 'unreadable' in result.stderr or '0x00012346' in result.stderr
