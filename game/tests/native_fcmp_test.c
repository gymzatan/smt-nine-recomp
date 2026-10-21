#include "recomp_types.h"
#include <assert.h>
#include <stdio.h>
#include <xmmintrin.h>

static unsigned reads[2];
static double operands[2];
static double operand(unsigned i) { ++reads[i];return operands[i]; }
static int original_compare(volatile double *a, volatile double *b) {
    return ((*a)!=(*a) || (*b)!=(*b)) ? 2 : (*a)<(*b) ? -1 : (*a)>(*b) ? 1 : 0;
}
static double from_bits(uint64_t bits) { double result;memcpy(&result,&bits,8);return result; }

int main(void) {
    const uint64_t bits[]={0,0x8000000000000000ULL,1,0x8000000000000001ULL,
        0x0010000000000000ULL,0x8010000000000000ULL,0x3ff0000000000000ULL,
        0xbff0000000000000ULL,0x7fefffffffffffffULL,0xffefffffffffffffULL,
        0x7ff0000000000000ULL,0xfff0000000000000ULL,0x7ff8000000000001ULL,
        0xfff8000000000001ULL,0x7ff0000000000001ULL,0xfff0000000000001ULL};
    unsigned cases=0;
    for(unsigned mode=0;mode<16;mode++) for(unsigned i=0;i<16;i++) for(unsigned j=0;j<16;j++) {
        volatile double a=from_bits(bits[i]),b=from_bits(bits[j]);
        unsigned control=0x1f80|((mode&3)<<13)|((mode&4)?0x40:0)|((mode&8)?0x8000:0);
        _mm_setcsr(control);int expected=original_compare(&a,&b);unsigned flags=_mm_getcsr();
        _mm_setcsr(control);int actual=RECOMP_FCMP(a,b);
        assert(actual==expected);assert(_mm_getcsr()==flags);
        uint16_t status=RECOMP_FCMP_CC(actual);
        assert(status==(expected==2?0x4500:expected<0?0x100:expected>0?0:0x4000));
        for(unsigned top=0;top<8;top++) {
            unsigned word=(top<<11)|status;
            assert(((word>>8)&0x45)==(expected==2?0x45:expected<0?1:expected>0?0:0x40));
        }
        operands[0]=a;operands[1]=b;reads[0]=reads[1]=0;
        assert(RECOMP_FCMP(operand(0),operand(1))==expected);
        if(reads[0]!=1 || reads[1]!=1) return 11;
        cases++;
    }
    _mm_setcsr(0x1f80);
    uint64_t rng=0xa4510c9e2134d6b7ULL;
    for(unsigned i=0;i<100000;i++) {
        rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;volatile double a=from_bits(rng);
        rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;volatile double b=from_bits(rng);
        assert(RECOMP_FCMP(a,b)==original_compare(&a,&b));cases++;
    }
    printf("%u FCMP operand, result and status comparisons passed.\n",cases);
    return 0;
}
