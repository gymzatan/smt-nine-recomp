#ifndef RECOMP_FPU_STATE_H
#define RECOMP_FPU_STATE_H
#include <stdint.h>
#include <string.h>
#include <math.h>
typedef struct RecompFpMetadata {
    uint32_t instruction_pointer;
    uint16_t opcode, code_selector;
    uint32_t data_pointer;
    uint16_t data_selector;
} RecompFpMetadata;
#define g_fp_instruction_pointer (g_fp_metadata.instruction_pointer)
#define g_fp_opcode (g_fp_metadata.opcode)
#define g_fp_code_selector (g_fp_metadata.code_selector)
#define g_fp_data_pointer (g_fp_metadata.data_pointer)
#define g_fp_data_selector (g_fp_metadata.data_selector)
/* Encode the existing double-backed x87 value as an architectural 80-bit
 * register. Byte copies permit the unaligned save buffers used by the CRT. */
static inline void recomp_fp80_store(void *destination, double value) {
    uint64_t bits, significand; uint16_t exponent;
    unsigned raw_exp; int shift=0;
    memcpy(&bits,&value,8); raw_exp=(unsigned)((bits>>52)&0x7ffu);
    significand=bits&UINT64_C(0xfffffffffffff);
    if(raw_exp==0x7ffu) {
        exponent=0x7fffu; significand=(significand<<11)|UINT64_C(0x8000000000000000);
    } else if(raw_exp) {
        exponent=(uint16_t)(raw_exp+15360u);
        significand=(significand|UINT64_C(0x10000000000000))<<11;
    } else if(significand) {
        while(!(significand&UINT64_C(0x10000000000000))) {significand<<=1; ++shift;}
        exponent=(uint16_t)(15361-shift); significand<<=11;
    } else exponent=0;
    exponent|=(uint16_t)((bits>>48)&0x8000u);
    memcpy(destination,&significand,8); memcpy((uint8_t*)destination+8,&exponent,2);
}
/* Synchronizing aliases must not change architectural tags (FLDENV/FNINIT). */
static inline void recomp_fp_sync_slots(uint8_t *dirty,const double stack[8],uint8_t raw[8][10],
                                       uint64_t *const mm[8],unsigned mask) {
    unsigned pending=*dirty & mask;
    for(unsigned i=0;pending;i++,pending>>=1)if(pending&1u) {
        recomp_fp80_store(raw[i],stack[i]);memcpy(mm[i],raw[i],8);
        *dirty&=(uint8_t)~(1u<<i);
    }
}
static inline double recomp_fp80_load(const void *source) {
    uint64_t significand,bits; uint16_t exponent; double value;
    memcpy(&significand,source,8);memcpy(&exponent,(const uint8_t*)source+8,2);
    if((exponent&0x7fffu)==0x7fffu) {
        bits=UINT64_C(0x7ff0000000000000)|((significand&UINT64_C(0x7fffffffffffffff))>>11);
        if((significand&UINT64_C(0x7fffffffffffffff)) && !(bits&UINT64_C(0xfffffffffffff)))bits|=1;
        bits|=(uint64_t)(exponent&0x8000u)<<48;memcpy(&value,&bits,8);return value;
    }
    value=ldexp((double)significand,((exponent&0x7fffu)?(exponent&0x7fffu):1)-16383-63);
    return (exponent&0x8000u)?-value:value;
}

/* A value view of the one runtime state, used by guest save instructions and
 * kernel exception CONTEXT serialization. Stack indices are physical. */
typedef struct RecompFpuState {
    double stack[8];
    uint64_t mm[8];uint8_t raw[8][10];
    uint16_t control,status,tag,opcode,code_selector,data_selector;
    uint32_t instruction_pointer,data_pointer,mxcsr;
    uint8_t xmm[8][16];
} RecompFpuState;
static __forceinline unsigned recomp_fp_tag(double value) {
    uint64_t bits;memcpy(&bits,&value,8);
    if(!(bits&UINT64_C(0x7fffffffffffffff)))return 1;
    if((bits&UINT64_C(0x7ff0000000000000))==UINT64_C(0x7ff0000000000000))return 2;
    /* Double denormals normalize when loaded into the extended register. */
    return 0;
}
static inline unsigned recomp_fp_raw_tag(const void *value) {
    const uint8_t *p=(const uint8_t*)value;uint64_t significand;uint16_t exponent;
    memcpy(&significand,p,8);memcpy(&exponent,p+8,2);exponent&=0x7fffu;
    if(!exponent && !significand)return 1;
    return !exponent || exponent==0x7fffu || !(significand>>63)?2:0;
}
static inline int recomp_fp_value_changed(const RecompFpuState *s,unsigned slot) {
    double previous=recomp_fp80_load(s->raw[slot]);uint64_t a,b;
    memcpy(&a,&previous,8);memcpy(&b,&s->stack[slot],8);return a!=b;
}
static inline uint16_t recomp_fp_tags(const RecompFpuState *s) {
    uint16_t tag=s->tag;
    for(unsigned i=0;i<8;i++)if(((tag>>(2*i))&3u)!=3 && recomp_fp_value_changed(s,i))
        tag=(uint16_t)((tag&~(3u<<(2*i)))|(recomp_fp_tag(s->stack[i])<<(2*i)));
    return tag;
}
static inline void recomp_fp_put16(uint8_t *p,uint16_t v) {memcpy(p,&v,2);}
static inline void recomp_fp_put32(uint8_t *p,uint32_t v) {memcpy(p,&v,4);}
static inline uint16_t recomp_fp_get16(const uint8_t *p) {uint16_t v;memcpy(&v,p,2);return v;}
static inline uint32_t recomp_fp_get32(const uint8_t *p) {uint32_t v;memcpy(&v,p,4);return v;}
static inline void recomp_fp_save_environment(void *destination,const RecompFpuState *s,int narrow) {
    uint8_t *p=(uint8_t*)destination;
    memset(p,0,narrow?14:28);
    recomp_fp_put16(p,s->control);recomp_fp_put16(p+(narrow?2:4),s->status);
    recomp_fp_put16(p+(narrow?4:8),recomp_fp_tags(s));
    if(narrow) {
        recomp_fp_put16(p+6,(uint16_t)s->instruction_pointer);recomp_fp_put16(p+8,s->code_selector);
        recomp_fp_put16(p+10,(uint16_t)s->data_pointer);recomp_fp_put16(p+12,s->data_selector);
    } else {
        recomp_fp_put32(p+12,s->instruction_pointer);recomp_fp_put16(p+16,s->code_selector);
        recomp_fp_put16(p+18,s->opcode&0x7ffu);recomp_fp_put32(p+20,s->data_pointer);recomp_fp_put16(p+24,s->data_selector);
    }
}
static inline void recomp_fp_load_environment(const void *source,RecompFpuState *s,int narrow) {
    const uint8_t *p=(const uint8_t*)source;
    s->control=recomp_fp_get16(p);s->status=recomp_fp_get16(p+(narrow?2:4));s->tag=recomp_fp_get16(p+(narrow?4:8));
    if(narrow) {
        s->instruction_pointer=recomp_fp_get16(p+6);s->code_selector=recomp_fp_get16(p+8);
        s->data_pointer=recomp_fp_get16(p+10);s->data_selector=recomp_fp_get16(p+12);
    } else {
        s->instruction_pointer=recomp_fp_get32(p+12);s->code_selector=recomp_fp_get16(p+16);
        s->opcode=recomp_fp_get16(p+18)&0x7ffu;s->data_pointer=recomp_fp_get32(p+20);s->data_selector=recomp_fp_get16(p+24);
    }
}
static inline void recomp_fp_register_store(void *p,const RecompFpuState *s,unsigned slot) {
    if(recomp_fp_value_changed(s,slot))recomp_fp80_store(p,s->stack[slot]);
    else memcpy(p,s->raw[slot],10);
}
static inline void recomp_fp_register_load(const void *p,RecompFpuState *s,unsigned slot) {
    memcpy(s->raw[slot],p,10);s->stack[slot]=recomp_fp80_load(p);memcpy(&s->mm[slot],p,8);
}
static inline void recomp_fp_save_legacy(void *destination,const RecompFpuState *s,int narrow) {
    uint8_t *p=(uint8_t*)destination;unsigned top=(s->status>>11)&7u;
    recomp_fp_save_environment(p,s,narrow);p+=narrow?14:28;
    for(unsigned i=0;i<8;i++)recomp_fp_register_store(p+10*i,s,(top+i)&7u);
}
static inline void recomp_fp_load_legacy(const void *source,RecompFpuState *s,int narrow) {
    const uint8_t *p=(const uint8_t*)source;recomp_fp_load_environment(p,s,narrow);
    unsigned top=(s->status>>11)&7u;p+=narrow?14:28;
    for(unsigned i=0;i<8;i++)recomp_fp_register_load(p+10*i,s,(top+i)&7u);
}
static inline void recomp_fp_save_extended(void *destination,const RecompFpuState *s) {
    uint8_t *p=(uint8_t*)destination;unsigned top=(s->status>>11)&7u;
    memset(p,0,512);recomp_fp_put16(p,s->control);recomp_fp_put16(p+2,s->status);
    for(unsigned i=0;i<8;i++)if(((s->tag>>(2*i))&3u)!=3)p[4]|=(uint8_t)(1u<<i);
    recomp_fp_put16(p+6,s->opcode&0x7ffu);recomp_fp_put32(p+8,s->instruction_pointer);
    recomp_fp_put16(p+12,s->code_selector);recomp_fp_put32(p+16,s->data_pointer);recomp_fp_put16(p+20,s->data_selector);
    recomp_fp_put32(p+24,s->mxcsr);recomp_fp_put32(p+28,0xffbfu);
    for(unsigned i=0;i<8;i++)recomp_fp_register_store(p+32+16*i,s,(top+i)&7u);
    memcpy(p+160,s->xmm,sizeof s->xmm);
}
static inline void recomp_fp_load_extended(const void *source,RecompFpuState *s) {
    const uint8_t *p=(const uint8_t*)source;
    s->control=recomp_fp_get16(p);s->status=recomp_fp_get16(p+2);s->opcode=recomp_fp_get16(p+6)&0x7ffu;
    s->instruction_pointer=recomp_fp_get32(p+8);s->code_selector=recomp_fp_get16(p+12);
    s->data_pointer=recomp_fp_get32(p+16);s->data_selector=recomp_fp_get16(p+20);s->mxcsr=recomp_fp_get32(p+24);
    unsigned top=(s->status>>11)&7u;s->tag=0;
    for(unsigned i=0;i<8;i++) {
        unsigned physical=(top+i)&7u;recomp_fp_register_load(p+32+16*i,s,physical);
        s->tag|=(uint16_t)(((p[4]&(1u<<physical))?recomp_fp_raw_tag(s->raw[physical]):3)<<(2*physical));
    }
    memcpy(s->xmm,p+160,sizeof s->xmm);
}
#endif
