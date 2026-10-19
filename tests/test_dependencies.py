"""Check production dependency preparation and failed-download cache handling."""
from pathlib import Path
import hashlib
import importlib.util
import io
import json
import sys
import struct
import urllib.error
import zipfile
import pytest

ROOT = Path(__file__).resolve().parents[1]


def dll(marker=b'fixture', name_offset=0x280):
    """Small x64 PE with a real DXGI import descriptor and writable padding."""
    raw=bytearray(1024);raw[:2]=b'MZ';struct.pack_into('<I',raw,0x3c,0x80)
    raw[0x80:0x84]=b'PE\0\0';struct.pack_into('<HHIIIHH',raw,0x84,0x8664,1,0,0,0,240,0x2022)
    optional=0x98;struct.pack_into('<H',raw,optional,0x20b)
    struct.pack_into('<Q',raw,optional+24,0x180000000)
    struct.pack_into('<II',raw,optional+32,0x1000,0x200)
    struct.pack_into('<II',raw,optional+56,0x2000,0x200)
    struct.pack_into('<I',raw,optional+108,16)
    struct.pack_into('<II',raw,optional+120,0x1000,40)
    section=optional+240;raw[section:section+8]=b'.rdata\0\0'
    struct.pack_into('<IIII',raw,section+8,512,0x1000,512,512)
    struct.pack_into('<I',raw,section+36,0x40000040)
    struct.pack_into('<IIIII',raw,512,0x10a0,0,0,0x1000+name_offset-512,0x10b0)
    raw[name_offset:name_offset+9]=b'dxgi.dll\0'
    struct.pack_into('<Q',raw,0x2a0,0x10c0);struct.pack_into('<Q',raw,0x2b0,0x10c0)
    raw[0x2c0:0x2c0+20]=b'\0\0CreateDXGIFactory\0\0'
    raw[0x380:0x380+len(marker)]=marker
    return bytes(raw)


def zipped(files):
    stream=io.BytesIO()
    with zipfile.ZipFile(stream,'w') as archive:
        for name,content in files.items():archive.writestr(name,content)
    return stream.getvalue()


@pytest.fixture
def dependency(tmp_path, monkeypatch):
    sys.path.insert(0, str(ROOT/'tools'))
    spec = importlib.util.spec_from_file_location('gpu_dependencies', ROOT/'tools/dependencies.py')
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    monkeypatch.setattr(module, 'ROOT', tmp_path)
    files = {name:dll(name.encode()) for name in ('d3d11.dll','dxgi.dll','wined3d.dll')}
    payload = zipped(files)
    entry = {'name': 'wined3d', 'archive': 'wined3d.zip', 'url': 'https://example.invalid/wined3d.zip',
             'sha256': hashlib.sha256(payload).hexdigest(),
             'files': {name: hashlib.sha256(content).hexdigest() for name, content in files.items()},
             'backend': 'opengl', 'dxgi_import_offset': 0x280, 'import_name': 'nogi.dll',
             'prepared_sha256': hashlib.sha256(files['d3d11.dll'].replace(b'dxgi.dll\0',b'nogi.dll\0')).hexdigest()}
    (tmp_path/'config').mkdir(); (tmp_path/'config/dependencies.json').write_text(json.dumps({'gpu': [entry]}))
    (tmp_path/'LICENSES').mkdir()
    for name in ('DXVK_LICENSE', 'WINED3D_COPYING.LIB'): (tmp_path/'LICENSES'/name).write_text('fixture')
    return module, payload, entry


def test_verified_download_is_cached_and_reused(tmp_path, monkeypatch, dependency):
    module, payload, entry = dependency
    calls = []
    def download(url, target):
        calls.append(url); Path(target).write_bytes(payload)
    monkeypatch.setattr(module.urllib.request, 'urlretrieve', download)
    module.prepare(); module.prepare()
    assert calls == [entry['url']]
    cache = tmp_path/'build/downloads'
    assert list(cache.iterdir()) == [cache/'wined3d.zip']
    assert (cache/'wined3d.zip').read_bytes() == payload
    assert (tmp_path/'runtime/backends/opengl/d3d11.dll').read_bytes() == dll(b'd3d11.dll').replace(b'dxgi.dll\0',b'nogi.dll\0')
    assert (tmp_path/'runtime/backends/opengl/nogi.dll').read_bytes() == dll(b'dxgi.dll')


@pytest.mark.parametrize('failure', ['interrupted', 'truncated', 'html'])
def test_failed_download_is_not_left_in_cache(tmp_path, monkeypatch, dependency, failure):
    module, payload, _ = dependency
    def download(url, target):
        Path(target).write_bytes(b'<html>download unavailable</html>' if failure == 'html' else payload[:30])
        if failure == 'interrupted': raise urllib.error.ContentTooShortError('interrupted', None)
    monkeypatch.setattr(module.urllib.request, 'urlretrieve', download)
    with pytest.raises((ValueError, urllib.error.ContentTooShortError)):
        module.prepare()
    assert list((tmp_path/'build/downloads').iterdir()) == []
    assert not (tmp_path/'runtime/backends').exists()
    monkeypatch.setattr(module.urllib.request, 'urlretrieve', lambda url, target: Path(target).write_bytes(payload))
    module.prepare()
    assert (tmp_path/'build/downloads/wined3d.zip').read_bytes() == payload


def test_existing_invalid_cache_explains_recovery_without_downloading(tmp_path, monkeypatch, dependency):
    module, _, _ = dependency
    cached = tmp_path/'build/downloads/wined3d.zip'; cached.parent.mkdir(parents=True)
    cached.write_bytes(b'old incomplete download')
    def forbidden(*args): pytest.fail('Existing cache must not silently trigger another download')
    monkeypatch.setattr(module.urllib.request, 'urlretrieve', forbidden)
    with pytest.raises(ValueError) as error: module.prepare()
    assert str(cached) in str(error.value)
    assert 'cached' in str(error.value) and entry_url(module,tmp_path) in str(error.value)
    assert cached.read_bytes() == b'old incomplete download'


def entry_url(module,root):
    return json.loads((root/'config/dependencies.json').read_text())['gpu'][0]['url']


@pytest.mark.parametrize('cached',[False,True])
def test_changed_archive_and_dll_use_current_pe_import_location(tmp_path,monkeypatch,dependency,cached):
    module,_,entry=dependency
    files={name:dll(b'upstream changed '+name.encode(),name_offset=0x290) for name in entry['files']}
    payload=zipped(files)
    if cached:
        path=tmp_path/'build/downloads/wined3d.zip';path.parent.mkdir(parents=True);path.write_bytes(payload)
        monkeypatch.setattr(module.urllib.request,'urlretrieve',lambda *args:pytest.fail('Use the supplied valid cache'))
    else:
        monkeypatch.setattr(module.urllib.request,'urlretrieve',lambda url,target:Path(target).write_bytes(payload))
    module.prepare()
    assert (tmp_path/'runtime/backends/opengl/d3d11.dll').read_bytes()==files['d3d11.dll'].replace(b'dxgi.dll\0',b'nogi.dll\0')


@pytest.mark.parametrize('failure',['missing','wrong_architecture','invalid_dll'])
def test_unusable_dlls_are_not_cached(tmp_path,monkeypatch,dependency,failure):
    module,_,entry=dependency
    files={name:dll() for name in entry['files']}
    if failure=='missing':del files['dxgi.dll']
    elif failure=='wrong_architecture':
        raw=bytearray(files['d3d11.dll']);struct.pack_into('<H',raw,0x84,0x14c);files['d3d11.dll']=bytes(raw)
    else:files['d3d11.dll']=b'<html>not a DLL</html>'
    monkeypatch.setattr(module.urllib.request,'urlretrieve',lambda url,target:Path(target).write_bytes(zipped(files)))
    with pytest.raises(ValueError):module.prepare()
    assert list((tmp_path/'build/downloads').iterdir())==[]
