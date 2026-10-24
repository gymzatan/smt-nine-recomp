"""Execute the width-sensitive flag producers through the production lifter."""
from .test_lifter_architectural_flags import PRELUDE, instruction, szp
from .test_lifter_pushal import _build_and_run


def test_multiply_full_result_and_overflow():
    functions=[];checks=[];old=0x246fd7
    for width,reg in ((8,'cl'),(16,'cx'),(32,'ecx')):
        mask=(1<<width)-1;sign=1<<(width-1)
        for mnemonic in ('mul','imul'):
            name=f'run_{mnemonic}_{width}'
            functions.append(f'''static void {name}(uint32_t a,uint32_t b,uint32_t *out) {{
 uint32_t eax=a,ecx=b,edx=0x98760000u;g_eflags=0x{old:x}u;
 {instruction(mnemonic,[reg])}
 out[0]=eax;out[1]=edx;out[2]=g_eflags;
}}''')
            for a in (0,1,2,sign-1,sign,mask):
                for b in (0,1,2,sign-1,sign,mask):
                    aa=a if mnemonic=='mul' or a<sign else a-(1<<width)
                    bb=b if mnemonic=='mul' or b<sign else b-(1<<width)
                    product=aa*bb
                    overflow=product>mask if mnemonic=='mul' else not -sign<=product<sign
                    eax=(0x76540000|(product&65535)) if width<=16 else product&mask
                    edx=0x98760000 if width==8 else (0x98760000|((product>>16)&mask)) if width==16 else (product>>32)&mask
                    flags=(old&~0x801)|(0x801 if overflow else 0)
                    checks.append(f'{name}(0x{a|0x76540000 if width<32 else a:x}u,0x{b:x}u,out);if(out[0]!=0x{eax:x}u||out[1]!=0x{edx:x}u||out[2]!=0x{flags:x}u)return {len(checks)+1};')
    result=_build_and_run(PRELUDE+'\n'.join(functions)+'\nint main(void){uint32_t out[3];'+'\n'.join(checks)+'return 0;}')
    assert result.returncode==0,(result.returncode,result.stdout,result.stderr)


def test_rotate_counts_carry_and_preserved_flags():
    functions=[];checks=[];old=0x246fd7
    for width,reg in ((8,'al'),(16,'ax'),(32,'eax')):
        mask=(1<<width)-1
        for mnemonic in ('rol','ror','rcl','rcr'):
            name=f'run_{mnemonic}_{width}'
            functions.append(f'''static void {name}(uint32_t a,uint32_t n,uint32_t f,uint32_t *out) {{
 uint32_t eax=a,ecx=n;int _cf=0;g_eflags=f;
 {instruction(mnemonic,[reg,'cl'])}
 out[0]=eax&0x{mask:x}u;out[1]=g_eflags;
}}''')
            for a in (0,1,1<<(width-1),mask):
                for count in (0,1,2,8,9,16,17,31,32,33,255):
                    for carry in (0,1):
                        incoming=(old&~1)|carry;c=count&31;r=a;cf=carry
                        effective=c%(width+1) if mnemonic in ('rcl','rcr') else c
                        for _ in range(effective):
                            if mnemonic in ('rol','rcl'):
                                r,cf=((r<<1)|(cf if mnemonic=='rcl' else r>>(width-1)))&mask,r>>(width-1)
                            else:r,cf=(r>>1)|((cf if mnemonic=='rcr' else r&1)<<(width-1)),r&1
                        flags=(incoming&~1)|cf if effective else incoming
                        if c==1:
                            of=((r>>(width-1))^cf)&1 if mnemonic in ('rol','rcl') else ((r>>(width-1))^(r>>(width-2)))&1
                            flags=(flags&~0x800)|(of<<11)
                        checks.append(f'{name}(0x{a:x}u,{count},0x{incoming:x}u,out);if(out[0]!=0x{r:x}u||out[1]!=0x{flags:x}u)return {len(checks)+1};')
    result=_build_and_run(PRELUDE+'\n'.join(functions)+'\nint main(void){uint32_t out[2];'+'\n'.join(checks)+'return 0;}')
    assert result.returncode==0,(result.returncode,result.stdout,result.stderr)


def test_exchange_add_and_compare_exchange_register_aliases():
    functions=[];checks=[];old=0x246fd7
    for width,acc,reg in ((8,'al','cl'),(16,'ax','cx'),(32,'eax','ecx')):
        mask=(1<<width)-1;sign=1<<(width-1)
        for mnemonic in ('xadd','cmpxchg'):
            for alias in (False,True):
                dst=acc;src=acc if alias else reg;name=f'run_{mnemonic}_{width}_{int(alias)}'
                functions.append(f'''static void {name}(uint32_t a,uint32_t b,uint32_t *out) {{
 uint32_t eax=a,ecx=b,_fa=0,_fb=0;int32_t _fas=0,_fbs=0;int _cf=0;g_eflags=0x{old:x}u;
 {instruction(mnemonic,[dst,src])}
 out[0]=eax;out[1]=ecx;out[2]=g_eflags;
}}''')
                for a in (0,1,sign-1,sign,mask):
                    for b in (0,1,sign-1,sign,mask):
                        if mnemonic=='xadd':
                            rhs=a if alias else b;wide=a+rhs;r=wide&mask
                            flags=(old&~0x8d5)|szp(r,width)|(wide>mask)|(((~(a^rhs)&(a^r)&sign)!=0)<<11)|((a^rhs^r)&16)
                            ea=(0x76540000&~mask)|r;ec=(0x98760000|b) if alias else (0x98760000&~mask)|a
                        else:
                            ea=(0x76540000&~mask)|(a if alias else b);ec=0x98760000|b
                            flags=(old&~0x8d5)|0x44
                        ain=(0x76540000&~mask)|a;bin=(0x98760000&~mask)|b
                        if width==32:ec=b if alias or mnemonic=='cmpxchg' else a
                        checks.append(f'{name}(0x{ain:x}u,0x{bin:x}u,out);if(out[0]!=0x{ea:x}u||out[1]!=0x{ec:x}u||out[2]!=0x{flags:x}u)return {len(checks)+1};')
    result=_build_and_run(PRELUDE+'\n'.join(functions)+'\nint main(void){uint32_t out[3];'+'\n'.join(checks)+'return 0;}')
    assert result.returncode==0,(result.returncode,result.stdout,result.stderr)
