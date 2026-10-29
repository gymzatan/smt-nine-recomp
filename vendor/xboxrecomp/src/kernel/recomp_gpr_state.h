#ifndef RECOMP_GPR_STATE_H
#define RECOMP_GPR_STATE_H
#include <stdint.h>
typedef struct RecompGprState {
    uint32_t r_eax,r_ecx,r_edx,r_esp,r_ebx,r_esi,r_edi,r_eflags;
    int r_df;
} RecompGprState;
#define g_eax (g_gpr.r_eax)
#define g_ecx (g_gpr.r_ecx)
#define g_edx (g_gpr.r_edx)
#define g_esp (g_gpr.r_esp)
#define g_ebx (g_gpr.r_ebx)
#define g_esi (g_gpr.r_esi)
#define g_edi (g_gpr.r_edi)
#define g_eflags (g_gpr.r_eflags)
#define g_df (g_gpr.r_df)
#endif
