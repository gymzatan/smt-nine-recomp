"""Exercise retail extraction through the existing XDVDFS reader."""
from pathlib import Path
import hashlib
import importlib.util
import json
import sys
import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture
def retail(tmp_path, monkeypatch):
    sys.path.insert(0, str(ROOT/'tools'))
    spec = importlib.util.spec_from_file_location('retail_extract', ROOT/'tools/extract_iso.py')
    extractor = importlib.util.module_from_spec(spec); spec.loader.exec_module(extractor)
    spec = importlib.util.spec_from_file_location('xdvdfs_fixture', ROOT/'vendor/xboxrecomp/tools/xiso/test_xdvdfs.py')
    fixture = importlib.util.module_from_spec(spec); spec.loader.exec_module(fixture)
    files = {'default.xbe': b'XBEH-original', 'Media/test.bin': b'original game resource'}
    image = fixture._build_image(files=(('default.xbe', files['default.xbe']),),
                                 subdir=('Media', (('test.bin', files['Media/test.bin']),)))
    identity = {'iso_sha256': hashlib.sha256(image).hexdigest(), 'bytes': len(image),
                'files': [{'path': name, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
                          for name, data in files.items()]}
    (tmp_path/'config').mkdir()
    (tmp_path/'config/retail.json').write_text(json.dumps(identity))
    monkeypatch.setattr(extractor, 'ROOT', tmp_path)
    return extractor, fixture, files, image, identity


@pytest.mark.parametrize('base', [0, 0x18300000])
def test_detect_layout_and_verify_identical_game_files(tmp_path, retail, base, capsys):
    extractor, _, files, image, identity = retail
    disc = tmp_path/'disc.iso'
    with disc.open('wb') as stream:
        stream.seek(base); stream.write(image + b'different container padding')
    assert extractor.digest(disc) != identity['iso_sha256']
    destination = tmp_path/'extracted'
    assert extractor.extract(disc, destination) == identity
    assert {p.relative_to(destination).as_posix(): p.read_bytes()
            for p in destination.rglob('*') if p.is_file()} == files
    assert extractor.verify_existing(destination) == identity
    assert ('0x%X' % base) in capsys.readouterr().out
    (destination/'Media/test.bin').write_bytes(b'modified')
    with pytest.raises(ValueError, match='Existing retail resource identity differs'):
        extractor.verify_existing(destination)


@pytest.mark.parametrize('change', ['modified', 'missing', 'extra'])
def test_reject_changed_game_contents(tmp_path, retail, change):
    extractor, fixture, files, _, _ = retail
    payload = b'XBEH-modified' if change == 'modified' else files['default.xbe']
    root = [('default.xbe', payload)]
    if change == 'extra': root.append(('extra.bin', b'extra'))
    subdir = None if change == 'missing' else ('Media', (('test.bin', files['Media/test.bin']),))
    disc = tmp_path/'disc.xiso.iso'
    disc.write_bytes(fixture._build_image(files=root, subdir=subdir))
    with pytest.raises(ValueError, match={'modified': 'ISO resource identity differs',
                                         'missing': 'complete retail file set',
                                         'extra': 'Unexpected or repeated ISO entry'}[change]):
        extractor.extract(disc, tmp_path/'extracted')
