; Original matrix SSE operand order; RCX/RDX inputs, R8 output address slot,
; R9 final architectural XMM values. XMM6 is only a preserved load scratch.
.code
smt9_matrix_rows PROC FRAME
    sub rsp, 24
    .allocstack 24
    movaps XMMWORD PTR [rsp], xmm6
    .savexmm128 xmm6, 0
    .endprolog
    movups xmm2, XMMWORD PTR [rcx]
    shufps xmm2, xmm2, 0
    movups xmm6, XMMWORD PTR [rdx]
    mulps xmm2, xmm6
    movups xmm1, XMMWORD PTR [rcx]
    shufps xmm1, xmm1, 85
    movups xmm6, XMMWORD PTR [rdx + 16]
    mulps xmm1, xmm6
    movups xmm0, XMMWORD PTR [rcx]
    shufps xmm0, xmm0, 170
    movups xmm6, XMMWORD PTR [rdx + 32]
    mulps xmm0, xmm6
    addps xmm2, xmm1
    movups xmm1, XMMWORD PTR [rcx]
    shufps xmm1, xmm1, 255
    movups xmm6, XMMWORD PTR [rdx + 48]
    mulps xmm1, xmm6
    addps xmm2, xmm0
    movups xmm3, XMMWORD PTR [rcx + 16]
    shufps xmm3, xmm3, 0
    addps xmm2, xmm1
    movups xmm6, XMMWORD PTR [rdx]
    mulps xmm3, xmm6
    movups xmm1, XMMWORD PTR [rcx + 16]
    shufps xmm1, xmm1, 85
    movups xmm6, XMMWORD PTR [rdx + 16]
    mulps xmm1, xmm6
    movups xmm0, XMMWORD PTR [rcx + 16]
    shufps xmm0, xmm0, 170
    movups xmm6, XMMWORD PTR [rdx + 32]
    mulps xmm0, xmm6
    addps xmm3, xmm1
    movups xmm1, XMMWORD PTR [rcx + 16]
    shufps xmm1, xmm1, 255
    movups xmm6, XMMWORD PTR [rdx + 48]
    mulps xmm1, xmm6
    addps xmm3, xmm0
    movups xmm4, XMMWORD PTR [rcx + 32]
    shufps xmm4, xmm4, 0
    addps xmm3, xmm1
    movups xmm6, XMMWORD PTR [rdx]
    mulps xmm4, xmm6
    movups xmm1, XMMWORD PTR [rcx + 32]
    shufps xmm1, xmm1, 85
    movups xmm6, XMMWORD PTR [rdx + 16]
    mulps xmm1, xmm6
    movups xmm0, XMMWORD PTR [rcx + 32]
    shufps xmm0, xmm0, 170
    movups xmm6, XMMWORD PTR [rdx + 32]
    mulps xmm0, xmm6
    addps xmm4, xmm1
    movups xmm1, XMMWORD PTR [rcx + 32]
    shufps xmm1, xmm1, 255
    movups xmm6, XMMWORD PTR [rdx + 48]
    mulps xmm1, xmm6
    addps xmm4, xmm0
    movups xmm5, XMMWORD PTR [rcx + 48]
    shufps xmm5, xmm5, 0
    addps xmm4, xmm1
    movups xmm6, XMMWORD PTR [rdx]
    mulps xmm5, xmm6
    movups xmm1, XMMWORD PTR [rcx + 48]
    shufps xmm1, xmm1, 85
    movups xmm6, XMMWORD PTR [rdx + 16]
    mulps xmm1, xmm6
    movups xmm0, XMMWORD PTR [rcx + 48]
    shufps xmm0, xmm0, 170
    movups xmm6, XMMWORD PTR [rdx + 32]
    mulps xmm0, xmm6
    addps xmm5, xmm1
    movups xmm1, XMMWORD PTR [rcx + 48]
    shufps xmm1, xmm1, 255
    movups xmm6, XMMWORD PTR [rdx + 48]
    mulps xmm1, xmm6
    mov eax, DWORD PTR [r8]
    addps xmm5, xmm0
    addps xmm5, xmm1
    movups XMMWORD PTR [r9 + 0], xmm0
    movups XMMWORD PTR [r9 + 16], xmm1
    movups XMMWORD PTR [r9 + 32], xmm2
    movups XMMWORD PTR [r9 + 48], xmm3
    movups XMMWORD PTR [r9 + 64], xmm4
    movups XMMWORD PTR [r9 + 80], xmm5
    movaps xmm6, XMMWORD PTR [rsp]
    add rsp, 24
    ret
smt9_matrix_rows ENDP
; Original data-only matrix transpose; RCX source, RDX final XMM values.
smt9_matrix_transpose_rows PROC
    movups xmm0, XMMWORD PTR [rcx]
    movups xmm2, XMMWORD PTR [rcx + 16]
    movups xmm3, XMMWORD PTR [rcx + 32]
    movups xmm5, XMMWORD PTR [rcx + 48]
    movaps xmm1, xmm0
    movaps xmm4, xmm3
    unpcklps xmm0, xmm2
    unpckhps xmm1, xmm2
    unpcklps xmm3, xmm5
    unpckhps xmm4, xmm5
    movups XMMWORD PTR [rdx + 0], xmm0
    movups XMMWORD PTR [rdx + 16], xmm1
    movups XMMWORD PTR [rdx + 32], xmm2
    movups XMMWORD PTR [rdx + 48], xmm3
    movups XMMWORD PTR [rdx + 64], xmm4
    movups XMMWORD PTR [rdx + 80], xmm5
    ret
smt9_matrix_transpose_rows ENDP

; Exact arithmetic status bits for the original completion comparison.
SMT9_COMPARE_STATUS MACRO lhsReg, rhsReg
    xor eax, eax
    cmp lhsReg, rhsReg
    IFDIF <rhsReg>, <0>
    seto dl
    ENDIF
    lahf
    movzx eax, ah
    IFDIF <rhsReg>, <0>
    movzx edx, dl
    shl edx, 11
    or eax, edx
    ENDIF
    ret
ENDM

smt9_cmp32_flags PROC
    SMT9_COMPARE_STATUS ecx, edx
smt9_cmp32_flags ENDP

smt9_cmp8_flags PROC
    SMT9_COMPARE_STATUS cl, dl
smt9_cmp8_flags ENDP

smt9_cmp16_flags PROC
    SMT9_COMPARE_STATUS cx, dx
smt9_cmp16_flags ENDP

smt9_cmp8_zero_flags PROC
    SMT9_COMPARE_STATUS cl, 0
smt9_cmp8_zero_flags ENDP

smt9_cmp32_zero_flags PROC
    SMT9_COMPARE_STATUS ecx, 0
smt9_cmp32_zero_flags ENDP
; Original point SSE order; RCX source, RDX matrix, R8 output slot,
; R9 final XMM1/XMM2/XMM3. The output address loads at its original boundary.
smt9_point_rows PROC
    movss xmm2, DWORD PTR [rcx]
    shufps xmm2, xmm2, 0
    movups xmm0, XMMWORD PTR [rdx]
    mulps xmm2, xmm0
    movss xmm1, DWORD PTR [rcx + 4]
    shufps xmm1, xmm1, 0
    movups xmm0, XMMWORD PTR [rdx + 16]
    mulps xmm1, xmm0
    movss xmm3, DWORD PTR [rcx + 8]
    shufps xmm3, xmm3, 0
    movups xmm0, XMMWORD PTR [rdx + 32]
    mulps xmm3, xmm0
    movups xmm0, XMMWORD PTR [rdx + 48]
    addps xmm2, xmm0
    mov eax, DWORD PTR [r8]
    addps xmm2, xmm1
    addps xmm2, xmm3
    movups XMMWORD PTR [r9], xmm1
    movups XMMWORD PTR [r9 + 16], xmm2
    movups XMMWORD PTR [r9 + 32], xmm3
    ret
smt9_point_rows ENDP
; Original interpolation over authoritative XMM state; RCX XMM1,
; RDX XMM2, R8 weight, R9 output slot, stack argument5 XMM0.
smt9_lerp_rows PROC
    mov r10, QWORD PTR [rsp + 40]
    movups xmm1, XMMWORD PTR [rcx]
    movups xmm2, XMMWORD PTR [rdx]
    subps xmm2, xmm1
    movups XMMWORD PTR [rdx], xmm2
    movss xmm0, DWORD PTR [r8]
    shufps xmm0, xmm0, 0
    movups XMMWORD PTR [r10], xmm0
    mulps xmm0, xmm2
    movups XMMWORD PTR [r10], xmm0
    addps xmm0, xmm1
    movups XMMWORD PTR [r10], xmm0
    mov eax, DWORD PTR [r9]
    ret
smt9_lerp_rows ENDP
END
