/* Inputs and expected bits were captured from the original x86 function.
 * This fixture checks the native kernel, without another matrix algorithm. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <xmmintrin.h>
extern uint32_t smt9_matrix_rows(const void *, const void *, const volatile uint32_t *, void *);
typedef struct Input { uint32_t mode,xmm[8][4],matrix[2][16]; } Input;
typedef struct Expected { uint32_t regs[8],xmm[8][4],matrix[3][16],mxcsr; } Expected;
typedef struct Record { Input input; Expected expected; } Record;
extern void smt9_matrix_transpose_rows(const void *, void *);
static void check_transpose(void) {
    /* Original final XMM lane identities after the four UNPCK operations. */
    static const unsigned lanes[6][4]={{0,4,1,5},{2,6,3,7},{4,5,6,7},
                                      {8,12,9,13},{10,14,11,15},{12,13,14,15}};
    static const int offsets[]={-31,-16,-1,0,1,16,31,96};
    unsigned cases=0;
    for(unsigned seed=0;seed<64;seed++)for(unsigned align=0;align<16;align++)
    for(unsigned mode=0;mode<sizeof offsets/sizeof offsets[0];mode++) {
        unsigned char buffer[384],expected[384];uint32_t values[16],rows[6][4];
        uint32_t rng=seed*0x1234567u+1;
        for(unsigned i=0;i<sizeof buffer;i++){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;buffer[i]=(unsigned char)rng;}
        unsigned source=64+align,target=source+offsets[mode];
        memcpy(values,buffer+source,sizeof values);memcpy(expected,buffer,sizeof buffer);
        for(unsigned i=0;i<6;i++)for(unsigned j=0;j<4;j++)rows[i][j]=values[lanes[i][j]];
        memcpy(expected+target,rows,sizeof rows);
        uint32_t csr=0x1f80|(seed&63);_mm_setcsr(csr);
        smt9_matrix_transpose_rows(buffer+source,buffer+target);
        assert(_mm_getcsr()==csr && !memcmp(buffer,expected,sizeof buffer));cases++;
    }
    printf("%u transpose bit permutations, unaligned addresses and aliases matched.\n",cases);
}
int main(int argc,char **argv) {
    check_transpose();
    assert(argc==2 && sizeof(Record)==616);
    FILE *stream=fopen(argv[1],"rb");assert(stream);
    Record record;unsigned cases=0;
    while(fread(&record,sizeof record,1,stream)==1) {
        uint32_t matrices[3][16],rows[6][4];
        memcpy(matrices,record.input.matrix,128);memset(matrices[2],0,64);
        unsigned target=record.input.mode==1?0:record.input.mode==2?1:2;
        uint32_t address=target==0?0x2000:target==1?0x2080:0x3000;
        _mm_setcsr(0x1f80);
        uint32_t result=smt9_matrix_rows(matrices[0],matrices[record.input.mode==3?0:1],&address,rows);
        uint32_t mxcsr=_mm_getcsr();
        memcpy(matrices[target],rows[2],64);
        assert(result==record.expected.regs[1]);
        assert(!memcmp(rows,record.expected.xmm,sizeof rows));
        assert(!memcmp(matrices,record.expected.matrix,sizeof matrices));
        assert(mxcsr==record.expected.mxcsr);
        cases++;
    }
    assert(feof(stream) && !ferror(stream) && cases==264);fclose(stream);
    printf("%u original x86 matrix results, alias cases and MXCSR values matched.\n",cases);
    return 0;
}
