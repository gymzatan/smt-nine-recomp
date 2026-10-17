push ebp
mov ebp, esp
sub esp, 0x30
push ebx
push esi
push edi
mov edi, dword ptr [ebp+8]
imul ecx, edi, 0x4d60
xor esi, esi
lea eax, [ebp-0x2e]
add ecx, 0x6a9af4
party_loop:
mov edx, dword ptr [ecx-0xc]
cmp edx, -1
je enemy
imul edx, edx, 0x1d0
mov bx, word ptr [edx+0x6a7a30]
mov dx, word ptr [edx+0x6a7a28]
mov word ptr [eax-2], bx
lea ebx, [esi+esi]
mov word ptr [eax], bx
mov word ptr [eax+2], dx
add eax, 8
enemy:
mov edx, dword ptr [ecx]
cmp edx, -1
je next_party
imul edx, edx, 0x1d0
mov bx, word ptr [edx+0x6a7a30]
mov dx, word ptr [edx+0x6a7a28]
mov word ptr [eax-2], bx
lea ebx, [esi+esi+1]
mov word ptr [eax], bx
mov word ptr [eax+2], dx
add eax, 8
next_party:
inc esi
add ecx, 4
cmp esi, 3
jl party_loop
xor ebx, ebx
push ebx
push edi
call 0x1de353
push ebx
push edi
mov esi, eax
call 0x1de179
push edi
add esi, eax
push esi
lea eax, [ebp-0x30]
push eax
push ebx
push ebx
push ebx
call 0x166c6d
fld dword ptr [0x31779c]
add esp, 0x24
fstp dword ptr [esp]
push 7
mov dword ptr [0x699c1c], edi
call 0x1eae8e
push 5
call 0x1efee8
add esp, 0xc
pop edi
pop esi
pop ebx
leave
ret
