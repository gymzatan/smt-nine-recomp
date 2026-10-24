"""Keep recovered original entry points connected to the emitted dispatcher."""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def test_resource_callback_preserves_both_paths_and_caller_stack():
    import sys
    sys.path.insert(0, str(ROOT / 'vendor/xboxrecomp'))
    from tools.recomp import config
    from tools.recomp.translator import FunctionTranslator
    from tools.recomp.test_lifter_pushal import _build_and_run
    from tools.recomp.test_lifter_architectural_flags import PRELUDE

    start = 0x1BFF0
    image = bytes.fromhex('807c2408007411681ca85400e8b2defffff7d81bc05940c3ff742404b91ca85400e8c2e5ffffc3')
    rows = json.loads((ROOT / 'config/functions.json').read_text())
    callback = next(r for r in rows if int(r['start'], 16) == start)
    end = int(callback['end'], 16)
    identified = json.loads((ROOT / 'config/identified_functions.json').read_text())
    assert next(r['end'] for r in identified if int(r['start'], 16) == start) == callback['end']
    config._install([config.Section('.text', start, len(image), 0, len(image), True)],
                    entry_point=start, kernel_thunk_addr=start, origin='resource-callback-test')
    db = {start: {**callback, 'end': end},
          0x19EB3: {'end': 0x19EB4, 'size': 1},
          0x1A5D8: {'end': 0x1A5D9, 'size': 1}}
    body = FunctionTranslator(image, db).translate_function(start, db[start])
    assert 'sub_00019EB3' in body and 'sub_0001A5D8' in body
    assert 'sub_0001C000' not in body
    source = PRELUDE + r'''
/* Keep the original architectural flag helpers; only guest RAM/call plumbing
 * is local to this callback fixture. */
#undef MEM8
#undef MEM32
#undef PUSH32
#undef POP32
#undef CMP_EQ
#undef RECOMP_ABI_CALL
#include <stdint.h>
#include <assert.h>
static unsigned char ram[0x2000];
static uint32_t eax,ecx,esp;
RECOMP_TLS uint32_t g_ebp,g_seh_ebp;
static int initialized,loaded,init_result;
#define MEM8(a) (*(uint8_t *)(ram+(a)))
#define MEM32(a) (*(uint32_t *)(ram+(a)))
#define PUSH32(sp,v) do {uint32_t value=(v);(sp)-=4;MEM32(sp)=value;} while(0)
#define POP32(sp,v) do {(v)=MEM32(sp);(sp)+=4;} while(0)
#define CMP_EQ(a,b) ((a)==(b))
#define RECOMP_ABI_CALL(a,f) f()
void sub_00019EB3(void){assert(MEM32(esp+4)==0x54A81C);initialized++;eax=init_result;esp+=4;}
void sub_0001A5D8(void){assert(ecx==0x54A81C);assert(MEM32(esp+4)==123);loaded++;eax=7;esp+=8;}
''' + body + r'''
int main(void){
  for(int mode=0;mode<3;mode++){
    initialized=loaded=0;init_result=mode==2;esp=0x1000;
    MEM32(esp)=0xABCDEF;MEM32(esp+4)=123;MEM32(esp+8)=mode!=0;
    sub_0001BFF0();
    assert(esp==0x1004);assert(MEM32(0x1004)==123);
    assert(initialized==(mode!=0));assert(loaded==(mode==0));
    assert(eax==(mode==0?7:mode==1?1:0));
  }
  return 0;
}
'''
    result = _build_and_run(source)
    assert result.returncode == 0, result.stdout + result.stderr


def test_recovered_entries_have_complete_generated_dispatch_targets():
    config = ROOT / 'config'
    seeds = json.loads((config / 'seed_functions.json').read_text())
    functions = {int(r['start'], 16): r for r in json.loads((config / 'functions.json').read_text())}
    recipe = json.loads((config / 'codegen.json').read_text())
    generated = {r['address']: r for r in recipe['functions']}
    dispatch = {int(address, 16): name for address, name in recipe['dispatch']}
    stops = {int(address, 16) for address in recipe['unresolved_stops']}
    assert len(dispatch) == len(recipe['dispatch'])
    addresses = [int(address, 16) for address, _ in recipe['dispatch']]
    assert addresses == sorted(addresses)
    assert len(generated) == len(recipe['functions'])
    for seed in seeds:
        address = int(seed['start'], 16)
        assert address in functions
        assert address in generated
        assert dispatch[address] == generated[address]['name']
        assert address not in stops
        row = functions[address]
        assert int(row['end'], 16) - address == row['size'] > 0


def test_long_callback_keeps_its_cleanup_tail():
    rows = json.loads((ROOT / 'config/functions.json').read_text())
    functions = {int(r['start'], 16): r for r in rows}
    callback = functions[0x484CE]
    tail = functions[0x485FC]
    assert int(callback['end'], 16) == 0x48618
    assert int(callback['end'], 16) >= int(tail['end'], 16)


def test_conditionally_selected_callback_is_a_complete_entry():
    rows = json.loads((ROOT / 'config/functions.json').read_text())
    functions = {int(r['start'], 16): r for r in rows}
    callback = functions[0x8726B]
    assert int(callback['end'], 16) == 0x8728D
    assert callback['calls_to'] == ['0x00085667']


def test_original_conditional_branches_remain_in_their_owner():
    functions = {int(r['start'], 16): r for r in json.loads((ROOT / 'config/functions.json').read_text())}
    branches = {
        0x8CFC3: (0x8D5AA,), 0xA335C: (0xA3469,),
        0x15FF99: (0x16030D, 0x16031B, 0x160348, 0x16035E, 0x1603C4),
        0x1ABA9D: (0x1ABD6B,),
        0x203DE5: (0x203F8E, 0x204074, 0x204087, 0x20409A, 0x2040AD, 0x2040F2),
        0x240AB4: (0x24106B, 0x241095, 0x24109D, 0x2411E3, 0x2411E8),
        0x27001E: (0x270063,), 0x270042: (0x270095,), 0x288473: (0x2884DB,),
    }
    for owner, targets in branches.items():
        assert all(owner <= target < int(functions[owner]['end'], 16) for target in targets)
