loc_00086E3D: ;
    _fb = (uint32_t)(0x14) & 0xFFFFFFFFu; _fbs = (int32_t)(int32_t)(_fb); /* add source, before the write */
    _cf = (int)((((uint64_t)(esp) + (uint64_t)(0x14)) >> 32) & 1);
    esp = esp + 0x14;
    _fa = (uint32_t)(esp) & 0xFFFFFFFFu; _fas = (int32_t)(int32_t)(_fa); /* add result */
    _fa = (uint32_t)(eax) & 0xFFFFFFFFu; _fb = (uint32_t)(0) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* cmp eax, 0 (32-bit) */
    _cf = 0; /* nothing borrows from zero */
    if (CMP_GE(_fas, _fbs)) goto loc_00086E57; /* jge: greater or equal (signed >=) */

loc_00086E44: ;
    _cf = 0; /* logical op clears CF */
    MEM32(esp + 0xC) = MEM32(esp + 0xC) | 0xFFFFFFFFu;
    _fa = (uint32_t)(MEM32(esp + 0xC)) & 0xFFFFFFFFu; _fas = (int32_t)(int32_t)(_fa); /* or result */
    PUSH32(esp, 0x3575A4);
    eax = esp + 0x10;
    goto loc_00087264;

loc_00086E57: ;
    _fa = (uint32_t)(MEM32(ebx + 0x34)) & 0xFFFFFFFFu; _fb = (uint32_t)(edi) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* cmp MEM32(ebx + 0x34), edi (32-bit) */
    _cf = (int)(_fa < _fb);
    if (CMP_EQ(_fa, _fb)) goto loc_00087256; /* je: equal / zero */

loc_00086E60: ;
    _fa = (uint32_t)(MEM32(ebx + 0x38)) & 0xFFFFFFFFu; _fb = (uint32_t)(edi) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* cmp MEM32(ebx + 0x38), edi (32-bit) */
    _cf = (int)(_fa < _fb);
    if (CMP_EQ(_fa, _fb)) goto loc_00087256; /* je: equal / zero */

loc_00086E69: ;
    eax = MEM32(esp + 0x94);
    _fa = (uint32_t)(MEM32(ebp + 0x14)) & 0xFFFFFFFFu; _fb = (uint32_t)(edi) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* cmp MEM32(ebp + 0x14), edi (32-bit) */
    _cf = (int)(_fa < _fb);
    MEM32(ebx + 0x34) = eax;
    eax = MEM32(esp + 0x98);
    MEM32(ebx + 0x38) = eax;
    esi = esp + 0x14;
    edi = ebx + 4;
    if (CMP_NE(_fa, _fb)) goto loc_00086EA1; /* jne: not equal / not zero */

loc_00086E89: ;
