"""The real wait wrapper must accept a completed final timeout interval."""
from pathlib import Path
import shutil
import subprocess

import pytest


@pytest.fixture(scope='module')
def wait_timeout_probe(tmp_path_factory):
    root = Path(__file__).resolve().parents[2]
    source = (root / 'src/kernel/kernel_sync.c').read_text(encoding='utf-8')
    body = source[source.index('DWORD xbox_checkpoint_wait('):source.index('NTSTATUS __stdcall xbox_NtWaitForSingleObject(')]
    program = r'''
#include <windows.h>
#include <mmsystem.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
static int simulated=1,tracked=1,pause_once;
static DWORD calls,last_slice,forced_result=WAIT_TIMEOUT,restored=INFINITE;
static ULONGLONG wall,waited;
static ULONGLONG observed_tick(void) {return simulated?wall/16*16:GetTickCount64();}
static DWORD observed_sleep(DWORD ms,BOOL alertable) {
    ++calls;last_slice=ms;
    if(!simulated)return SleepEx(ms,alertable);
    assert(ms!=INFINITE && calls<1000);wall+=ms;waited+=ms;
    return forced_result==WAIT_TIMEOUT?0:forced_result;
}
static DWORD observed_objects(DWORD count,const HANDLE *objects,BOOL all,DWORD ms,BOOL alertable) {
    assert(count && objects && all==(count==2));
    DWORD result=observed_sleep(ms,alertable);
    return result?result:WAIT_TIMEOUT;
}
int xbox_kernel_wait_tracked(void) {return tracked;}
uint32_t xbox_kernel_wait_begin(uint32_t ms) {return restored==INFINITE?ms:restored;}
void xbox_kernel_wait_boundary(uint32_t remaining) {
    (void)remaining;
    if(pause_once){wall+=1600;pause_once=0;}
}
#define GetTickCount64 observed_tick
#define SleepEx observed_sleep
#define WaitForMultipleObjectsEx observed_objects
''' + body + r'''
#undef GetTickCount64
#undef SleepEx
#undef WaitForMultipleObjectsEx
int main(int argc,char **argv) {
    assert(argc==2);
    if(!strcmp(argv[1],"native")) {
        simulated=0;LARGE_INTEGER frequency,begin,end;QueryPerformanceFrequency(&frequency);
        for(int period=0;period<2;period++) {
            if(period)assert(timeBeginPeriod(1)==TIMERR_NOERROR);
            for(tracked=0;tracked<2;tracked++) {
                calls=0;QueryPerformanceCounter(&begin);
                for(int i=0;i<20;i++)assert(xbox_checkpoint_wait(0,NULL,FALSE,FALSE,2)==WAIT_TIMEOUT);
                QueryPerformanceCounter(&end);
                printf("period=%d tracked=%d calls=%lu elapsed_ms=%.6f\n",period,tracked,calls,(double)(end.QuadPart-begin.QuadPart)*1000/frequency.QuadPart);
            }
            if(period)assert(timeEndPeriod(1)==TIMERR_NOERROR);
        }
        return 0;
    }
    DWORD count=(DWORD)atoi(argv[1]);assert(count<=2);HANDLE objects[2]={(HANDLE)1,(HANDLE)2};
    for(DWORD ms=0;ms<=10;ms++) {
        for(int pause=0;pause<2;pause++) {
            calls=0;wall=waited=0;pause_once=pause;
            DWORD result=xbox_checkpoint_wait(count,objects,count==2,FALSE,ms);
            fprintf(stderr,"count=%lu request=%lu pause=%d calls=%lu waited=%llu result=%lu\n",count,ms,pause,calls,waited,result);
            assert(result==WAIT_TIMEOUT && calls==1 && waited==ms);
        }
    }
    calls=0;wall=waited=0;restored=2;
    assert(xbox_checkpoint_wait(count,objects,count==2,FALSE,1000)==WAIT_TIMEOUT);
    assert(calls==1 && waited==2);restored=INFINITE;
    calls=0;wall=waited=0;forced_result=WAIT_IO_COMPLETION;
    assert(xbox_checkpoint_wait(count,objects,count==2,TRUE,INFINITE)==WAIT_IO_COMPLETION);
    assert(calls==1 && last_slice==10);
    calls=0;wall=waited=0;forced_result=WAIT_FAILED;
    assert(xbox_checkpoint_wait(count,objects,count==2,FALSE,1000)==WAIT_FAILED);
    assert(calls==1 && last_slice==10);
    return 0;
}
'''
    work=tmp_path_factory.mktemp('checkpoint_wait_timeout')
    (work/'probe.c').write_text(program,encoding='utf-8')
    compiler=shutil.which('cl');assert compiler
    result=subprocess.run([compiler,'/nologo','/O2','/UNDEBUG','probe.c','/Fe:probe.exe','/link','winmm.lib'],cwd=work,capture_output=True,text=True)
    assert result.returncode==0,result.stdout+result.stderr
    return work/'probe.exe'


@pytest.mark.parametrize('count',[0,1,2])
def test_completed_final_timeout_is_not_repeated(wait_timeout_probe,count):
    result=subprocess.run([str(wait_timeout_probe),str(count)],capture_output=True,text=True,timeout=5)
    assert result.returncode==0,result.stdout+result.stderr


def test_native_short_wait_observation(wait_timeout_probe):
    result=subprocess.run([str(wait_timeout_probe),'native'],capture_output=True,text=True,timeout=10)
    assert result.returncode==0,result.stdout+result.stderr
    print(result.stdout)
