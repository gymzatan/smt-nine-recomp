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
    helper = source[source.index('void smt9_game_frame_begin(void)'):source.index('extern int smt9_is_japanese(void);')]
    program = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>
typedef unsigned long DWORD;
static uint32_t previous_words[2];
static uint32_t movie;
static float original_interval;
#define MEM32(address) ((address)==0x005548B0u ? movie : previous_words[((address)-0x00557228u)/4])
#define MEMF(address) original_interval
static uint64_t now;
static unsigned hz,parts,joins,sleeps;
static int held=1;
unsigned smt9_game_refresh_hz(void) { return hz; }
uint64_t xbox_ReadTimeStampCounter(void) { uint64_t value=now;now+=7333;return value; }
void guest_cpu_part(void) { assert(held);held=0;parts++; }
void guest_cpu_join(void) { assert(!held);now+=366667;held=1;joins++; }
void Sleep(DWORD milliseconds) { assert(!held);now+=(uint64_t)milliseconds*733333333ull/1000+70000;sleeps++; }
'''+helper+r'''
static void timestamp_original_caller(void) {
    previous_words[0]=(uint32_t)now;previous_words[1]=(uint32_t)(now>>32);
}
static void frame(uint64_t work) {
    now+=work;
    uint32_t lo=previous_words[0],hi=previous_words[1];
    unsigned p=parts,j=joins;
    smt9_game_frame_begin();
    assert(held && parts-p==joins-j && parts-p<=1);
    assert(previous_words[0]==lo && previous_words[1]==hi);
    timestamp_original_caller();
}
int main(int argc,char **argv) {
    assert(argc==3);hz=(unsigned)atoi(argv[2]);assert(hz==30 || hz==60);
    now=(1ull<<32)-3000000;timestamp_original_caller();
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
        assert(parts==600 && joins==600 && sleeps>=600);
    } else if(!strcmp(argv[1],"overload")) {
        uint64_t start=now;
        for(unsigned i=0;i<120;i++)frame(36666667); /* 50ms work cannot fit either rate. */
        assert(!parts && !joins && !sleeps);
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
