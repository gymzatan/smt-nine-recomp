"""Fetch declared GPU archives and prepare their existing runtime DLL layout."""
from pathlib import Path
import json
import shutil
import tarfile
import tempfile
import urllib.error
import urllib.request
import zipfile
import pefile
from common import ROOT


def read_dependency(archive, entry):
    files = {}
    if entry['name']=='dxvk':
        with tarfile.open(archive,'r:gz') as bundle:
            for filename in entry['files']:
                members=[m for m in bundle.getmembers() if m.isfile() and m.name.endswith('/x64/'+filename)]
                if len(members)!=1:
                    raise ValueError('DXVK archive must contain one x64/'+filename)
                files[filename]=bundle.extractfile(members[0]).read()
    else:
        with zipfile.ZipFile(archive) as bundle:
            damaged=bundle.testzip()
            if damaged is not None:
                raise ValueError('ZIP member is damaged: '+damaged)
            for filename in entry['files']:
                members=[name for name in bundle.namelist() if Path(name).name==filename]
                if len(members)!=1:
                    raise ValueError('WineD3D archive must contain one '+filename)
                files[filename]=bundle.read(members[0])
    patched = None
    for filename,content in files.items():
        with pefile.PE(data=content,fast_load=True) as pe:
            if pe.FILE_HEADER.Machine!=0x8664 or pe.OPTIONAL_HEADER.Magic!=0x20b or not pe.FILE_HEADER.Characteristics & 0x2000:
                raise ValueError('Dependency must be an x64 DLL: '+filename)
            if filename=='d3d11.dll':
                pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']])
                imports=[item for item in getattr(pe,'DIRECTORY_ENTRY_IMPORT',[]) if item.dll.lower()==b'dxgi.dll']
                if len(imports)!=1:
                    raise ValueError('D3D11 must have exactly one DXGI import module')
                offset=pe.get_offset_from_rva(imports[0].struct.Name)
                replacement=entry['import_name'].encode('ascii')
                if len(replacement)!=8 or content[offset:offset+9].lower()!=b'dxgi.dll\0':
                    raise ValueError('D3D11 import module cannot be renamed in place')
                patched=content[:offset]+replacement+content[offset+8:]
    return files,patched


def prepare():
    lock = json.loads((ROOT/'config/dependencies.json').read_text())
    cache = ROOT/'build/downloads'
    cache.mkdir(parents=True,exist_ok=True)
    reference = ROOT/'reference/gpu_runtime'
    runtime = ROOT/'runtime/backends'
    for entry in lock['gpu']:
        archive = cache/entry['archive']
        recovery=f"Download {entry['url']} in your browser, save the archive without extracting it as {archive}, then rerun the build."
        if archive.exists():
            try:
                files,patched=read_dependency(archive,entry)
            except (ValueError,zipfile.BadZipFile,tarfile.TarError,pefile.PEFormatError) as error:
                raise ValueError(f'Invalid cached dependency: {archive}: {error}. Replace this cached archive. '+recovery) from error
        else:
            print('Downloading dependency:',entry['url'],flush=True)
            with tempfile.TemporaryDirectory(prefix=archive.name+'.',dir=cache) as directory:
                pending = Path(directory)/archive.name
                try:
                    urllib.request.urlretrieve(entry['url'],pending)
                    files,patched=read_dependency(pending,entry)
                except (ValueError,zipfile.BadZipFile,tarfile.TarError,pefile.PEFormatError,urllib.error.URLError) as error:
                    size=pending.stat().st_size if pending.exists() else 0
                    raise ValueError(f'Downloaded dependency is unusable: {archive.name} ({size} bytes): {error}. '
                                     'The failed download is discarded. '+recovery) from error
                pending.replace(archive)
        target = reference/entry['name']
        target.mkdir(parents=True,exist_ok=True)
        for filename,content in files.items():
            (target/filename).write_bytes(content)
        output = runtime/entry['backend']
        output.mkdir(parents=True,exist_ok=True)
        (output/'d3d11.dll').write_bytes(patched)
        shutil.copyfile(target/'dxgi.dll',output/entry['import_name'])
    shutil.copyfile(ROOT/'LICENSES/DXVK_LICENSE',reference/'DXVK_LICENSE')
    shutil.copyfile(ROOT/'LICENSES/WINED3D_COPYING.LIB',reference/'WINED3D_COPYING.LIB')
    return lock


if __name__ == '__main__':
    prepare()
