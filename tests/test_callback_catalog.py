"""Keep recovered original entry points connected to the emitted dispatcher."""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


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
