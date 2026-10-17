"""Fetch the pinned GPU archives and prepare the existing runtime DLL layout."""
from pathlib import Path
import hashlib
import json
import shutil
import tarfile
import urllib.request
import zipfile
from common import ROOT, digest


def prepare():
    lock = json.loads((ROOT/'config/dependencies.json').read_text())
    cache = ROOT/'build/downloads'
    cache.mkdir(parents=True,exist_ok=True)
    reference = ROOT/'reference/gpu_runtime'
    runtime = ROOT/'runtime/backends'
    for entry in lock['gpu']:
        archive = cache/entry['archive']
        if archive.exists():
            if digest(archive) != entry['sha256']:
                raise ValueError('Pinned archive checksum differs: '+archive.name)
        else:
            print('Downloading pinned dependency:',entry['url'],flush=True)
            urllib.request.urlretrieve(entry['url'],archive)
            if digest(archive) != entry['sha256']:
                raise ValueError('Downloaded dependency checksum differs: '+archive.name)
        target = reference/entry['name']
        target.mkdir(parents=True,exist_ok=True)
        if entry['name']=='dxvk':
            with tarfile.open(archive,'r:gz') as bundle:
                for filename in ['d3d11.dll','dxgi.dll']:
                    members=[m for m in bundle.getmembers() if m.name.endswith('/x64/'+filename)]
                    if len(members)!=1:
                        raise ValueError('Pinned DXVK archive layout differs')
                    (target/filename).write_bytes(bundle.extractfile(members[0]).read())
        else:
            with zipfile.ZipFile(archive) as bundle:
                for filename in ['d3d11.dll','dxgi.dll','wined3d.dll']:
                    members=[name for name in bundle.namelist() if Path(name).name==filename]
                    if len(members)!=1:
                        raise ValueError('Pinned WineD3D archive layout differs')
                    (target/filename).write_bytes(bundle.read(members[0]))
        for filename,expected in entry['files'].items():
            if digest(target/filename)!=expected:
                raise ValueError('Pinned GPU DLL identity differs: '+filename)
        output = runtime/entry['backend']
        output.mkdir(parents=True,exist_ok=True)
        raw = bytearray((target/'d3d11.dll').read_bytes())
        offset = entry['dxgi_import_offset']
        if raw[offset:offset+9]!=b'dxgi.dll\0':
            raise ValueError('Original D3D11 import module differs')
        raw[offset:offset+8]=entry['import_name'].encode('ascii')
        if hashlib.sha256(raw).hexdigest()!=entry['prepared_sha256']:
            raise ValueError('Prepared D3D11 import edit differs')
        (output/'d3d11.dll').write_bytes(raw)
        shutil.copyfile(target/'dxgi.dll',output/entry['import_name'])
    shutil.copyfile(ROOT/'LICENSES/DXVK_LICENSE',reference/'DXVK_LICENSE')
    shutil.copyfile(ROOT/'LICENSES/WINED3D_COPYING.LIB',reference/'WINED3D_COPYING.LIB')
    return lock


if __name__ == '__main__':
    prepare()
