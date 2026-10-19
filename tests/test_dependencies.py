"""Check production dependency preparation and failed-download cache handling."""
from pathlib import Path
import hashlib
import importlib.util
import io
import json
import sys
import urllib.error
import zipfile
import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture
def dependency(tmp_path, monkeypatch):
    sys.path.insert(0, str(ROOT/'tools'))
    spec = importlib.util.spec_from_file_location('gpu_dependencies', ROOT/'tools/dependencies.py')
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    monkeypatch.setattr(module, 'ROOT', tmp_path)
    files = {'d3d11.dll': b'prefixdxgi.dll\0suffix', 'dxgi.dll': b'fixture dxgi',
             'wined3d.dll': b'fixture wined3d'}
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, 'w') as archive:
        for name, content in files.items(): archive.writestr(name, content)
    payload = stream.getvalue()
    entry = {'name': 'wined3d', 'archive': 'wined3d.zip', 'url': 'https://example.invalid/wined3d.zip',
             'sha256': hashlib.sha256(payload).hexdigest(),
             'files': {name: hashlib.sha256(content).hexdigest() for name, content in files.items()},
             'backend': 'opengl', 'dxgi_import_offset': 6, 'import_name': 'nogi.dll',
             'prepared_sha256': hashlib.sha256(b'prefixnogi.dll\0suffix').hexdigest()}
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
    assert (tmp_path/'runtime/backends/opengl/d3d11.dll').read_bytes() == b'prefixnogi.dll\0suffix'
    assert (tmp_path/'runtime/backends/opengl/nogi.dll').read_bytes() == b'fixture dxgi'


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
    assert 'Remove this cached file' in str(error.value)
    assert cached.read_bytes() == b'old incomplete download'
