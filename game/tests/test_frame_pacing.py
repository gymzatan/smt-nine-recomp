"""Run the actual game pacing helper against elapsed-time requirements."""
from pathlib import Path
import shutil
import subprocess
import tempfile

import pytest


@pytest.fixture(scope='module')
def pacing_probe():
    compiler = shutil.which('cl')
    assert compiler, 'requires the existing MSVC developer environment'
    root = Path(__file__).resolve().parents[2]
    source = (root / 'game/src/recomp_manual.c').read_text(encoding='utf-8')
    helper = source[source.index('static HANDLE s_frame_wait_timer;'):source.index('extern int smt9_is_japanese(void);')]
    program = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>
typedef unsigned long DWORD;
typedef int64_t LONGLONG;
typedef struct { LONGLONG QuadPart; } LARGE_INTEGER;
typedef void *HANDLE;
#define FALSE 0
#define WAIT_OBJECT_0 0
#define WAIT_FAILED 0xffffffffu
#define INFINITE 0xffffffffu
static uint32_t previous_words[2];
static uint32_t movie;
static float original_interval;
#define MEM32(address) ((address)==0x005548B0u ? movie : previous_words[((address)-0x00557228u)/4])
#define MEMF(address) original_interval
static uint64_t now;
static unsigned hz,parts,joins,waits,armed,created,closed,fail_step;
static uint64_t wake_at;
static int timer_object;
static int held=1;
unsigned smt9_game_refresh_hz(void) { return hz; }
uint64_t xbox_ReadTimeStampCounter(void) { uint64_t value=now;now+=7333;return value; }
void guest_cpu_part(void) { assert(held);held=0;parts++; }
void guest_cpu_join(void) { assert(!held);now+=366667;held=1;joins++; }
static DWORD GetLastError(void) { return 8; }
static HANDLE CreateWaitableTimerW(void *security,int manual,const void *name) {
    assert(held && !security && !manual && !name && !created);
    if(fail_step==1)return NULL;
    ++created;return &timer_object;
}
static int CloseHandle(HANDLE timer) {
    assert(held && timer==&timer_object && !closed && !armed);++closed;return 1;
}
static int SetWaitableTimer(HANDLE timer,const LARGE_INTEGER *due,long period,void *completion,void *context,int resume) {
    assert(!held && timer==&timer_object && !closed && !armed);
    assert(due->QuadPart<0 && !period && !completion && !context && !resume);
    if(fail_step==2)return 0;
    uint64_t remaining=(uint64_t)(-due->QuadPart)*733333333ull/10000000ull;
    assert(remaining && remaining<=733333333ull/hz+74);
    wake_at=now+remaining;
    /* Exercise an early wake without letting the pacing helper start early. */
    if(remaining>146666)wake_at-=146666;
    armed=1;return 1;
}
static DWORD WaitForSingleObject(HANDLE timer,DWORD timeout) {
    assert(!held && timer==&timer_object && armed && timeout==INFINITE);
    if(fail_step==3)return WAIT_FAILED;
    now=wake_at;armed=0;++waits;return WAIT_OBJECT_0;
}
static void xbox_ReportFatalError(const char *reason,DWORD error) {
    assert((fail_step==2 || fail_step==3) && error==8 && !strcmp(reason,"Frame timer wait"));
}
static void ExitProcess(unsigned code) { assert(code==5);exit(code); }
'''+helper+r'''
static void timestamp_original_caller(void) {
    previous_words[0]=(uint32_t)now;previous_words[1]=(uint32_t)(now>>32);
}
static void frame(uint64_t work) {
    now+=work;
    uint32_t lo=previous_words[0],hi=previous_words[1];
    unsigned p=parts,j=joins;
    uint64_t previous=(uint64_t)lo|((uint64_t)hi<<32),period=733333333ull/hz;
    uint64_t deadline=previous-previous%period+period;
    smt9_game_frame_begin();
    assert(now>=deadline);
    assert(held && parts-p==joins-j && parts-p<=1);
    assert(previous_words[0]==lo && previous_words[1]==hi);
    timestamp_original_caller();
}
int main(int argc,char **argv) {
    assert(argc==3);hz=(unsigned)atoi(argv[2]);assert(hz==30 || hz==60);
    now=(1ull<<32)-3000000;timestamp_original_caller();
    if(!strcmp(argv[1],"create_failed")) {
        fail_step=1;
        assert(!smt9_frame_wait_init() && !created && !parts && !joins);
        return 0;
    }
    assert(smt9_frame_wait_init() && created==1);
    if(!strcmp(argv[1],"set_failed") || !strcmp(argv[1],"wait_failed")) {
        fail_step=!strcmp(argv[1],"set_failed")?2:3;
        frame(2933333);
        assert(!"fatal wait failure returned");
    }
    assert(smt9_game_frame_interval()==1.0f/hz);
    if(!strcmp(argv[1],"camera")) {
        const float original[4]={0.3f,0.03f,-0.3f,8.0f};
        float actual[4];original_interval=0.3f;
        for(unsigned i=0;i<4;i++)actual[i]=smt9_battle_camera_constant(original[i],i);
        if(hz==30)for(unsigned i=0;i<4;i++)assert(actual[i]==original[i]);
        float position_remaining=1.0f-actual[1]/actual[0];
        float rotation_remaining=1.0f-1.0f/actual[3];
        unsigned frames=hz/30;
        assert(fabsf(powf(position_remaining,(float)frames)-0.9f)<0.000001f);
        assert(fabsf(powf(rotation_remaining,(float)frames)-0.875f)<0.000001f);
        assert(fabsf(actual[0]*hz-9.0f)<0.000001f);
        assert(fabsf(actual[2]*hz+9.0f)<0.000001f);
        assert(original_interval==0.3f);
    } else if(!strcmp(argv[1],"movie")) {
        movie=0x123456;original_interval=1.0f/30;
        float elapsed=0;
        for(unsigned i=0;i<30;i++)elapsed+=smt9_game_frame_interval();
        assert(fabsf(elapsed-1.0f)<0.00001f);
        original_interval=0.041f;
        assert(smt9_game_frame_interval()==original_interval);
        movie=0;
        assert(smt9_game_frame_interval()==1.0f/hz);
    } else if(!strcmp(argv[1],"elapsed")) {
        uint64_t start=now;
        for(unsigned i=0;i<600;i++)frame(2933333); /* 4ms original frame work. */
        double elapsed=(double)(now-start)/733333333.0,expected=600.0/hz;
        fprintf(stderr,"actual helper hz=%u frames=600 elapsed=%g expected=%g parts=%u joins=%u\n",hz,elapsed,expected,parts,joins);
        assert(fabs(elapsed-expected)<1.0/hz);
        assert(parts==600 && joins==600 && waits>600);
    } else if(!strcmp(argv[1],"overload")) {
        uint64_t start=now;
        for(unsigned i=0;i<120;i++)frame(36666667); /* 50ms work cannot fit either rate. */
        assert(!parts && !joins && !waits);
        assert((double)(now-start)/733333333.0>=6.0);
    } else {
        assert(!strcmp(argv[1],"restore"));
        for(unsigned i=0;i<60;i++)frame(2933333);
        uint64_t saved=now;
        uint32_t words[2]={previous_words[0],previous_words[1]};
        for(unsigned i=0;i<60;i++)frame(2933333);
        uint64_t end=now;
        now=saved;previous_words[0]=words[0];previous_words[1]=words[1];
        for(unsigned i=0;i<60;i++)frame(2933333);
        assert(now==end); /* Frozen original clock and caller state fully determine resume. */
    }
    smt9_frame_wait_shutdown();
    assert(closed==1 && !armed);
    return 0;
}
'''
    with tempfile.TemporaryDirectory() as temp:
        work = Path(temp)
        (work / 'pacing.c').write_text(program, encoding='utf-8')
        subprocess.run([compiler, '/nologo', '/O2', '/UNDEBUG', 'pacing.c', '/Fe:pacing.exe'], cwd=work, check=True, capture_output=True)
        yield work / 'pacing.exe'


@pytest.mark.parametrize('hz', [30, 60])
@pytest.mark.parametrize('mode', ['elapsed', 'overload', 'restore', 'movie', 'camera'])
def test_actual_helper_elapsed_time(pacing_probe, hz, mode):
    subprocess.run([str(pacing_probe), mode, str(hz)], check=True, timeout=10)


@pytest.mark.parametrize(('mode', 'expected'), [('create_failed', 0), ('set_failed', 5), ('wait_failed', 5)])
def test_frame_timer_api_failures(pacing_probe, mode, expected):
    run = subprocess.run([str(pacing_probe), mode, '60'], timeout=10)
    assert run.returncode == expected
