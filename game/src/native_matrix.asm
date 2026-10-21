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
END
