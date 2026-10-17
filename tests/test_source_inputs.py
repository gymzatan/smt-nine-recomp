from pathlib import Path
import hashlib
import sys
import pytest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from prepare_resources import hydrate
from audit_source import source_files


def test_serialized_reference_reads_only_its_original_source():
    original=b'\x02\x01\x00'+b'\x00'*13+b'HELLO'
    payload={'parts':[{'offset':16,'length':5,'source_sha256':hashlib.sha256(b'HELLO').hexdigest(),
                      'bytecode_offset':0,'bytecode_length':3}], 'pages':[]}
    restored=hydrate(payload,original)
    assert restored['storage_jp_lines']==['HELLO']
    assert restored['parts'][0]['bytecode_raw']=='020100'
    assert 'raw_hex' not in payload['parts'][0]
    with pytest.raises(ValueError,match='identity'):
        hydrate(payload,original[:-1]+b'X')


def test_source_selection_excludes_local_builds_and_rejects_game_files(tmp_path):
    (tmp_path/'runtime').mkdir()
    (tmp_path/'runtime/local.xbe').write_bytes(b'private')
    (tmp_path/'README.md').write_text('Project source')
    assert [p.name for p in source_files(tmp_path)]==['README.md']
    (tmp_path/'disc.iso').write_bytes(b'private')
    with pytest.raises(ValueError,match='Game/build'):
        list(source_files(tmp_path))
