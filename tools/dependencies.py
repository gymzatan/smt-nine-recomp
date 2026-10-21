"""Fetch declared GPU archives and prepare their existing runtime DLL layout."""
from pathlib import Path
import json
import shutil
import tempfile
import urllib.error
import urllib.request
import zipfile
import pefile
from common import ROOT


def use_wined3d_events(content):
    """Select WineD3D's existing event wakeups on native Windows.

    Direct thread alerts can interrupt an unrelated Windows critical-section
    wait and leave its stack wait block queued. Disable both optional procedure
    probes together so WineD3D creates and uses its own command-stream event.
    """
    with pefile.PE(data=content, fast_load=True) as pe:
        locations = []
        for name in (b'NtAlertThreadByThreadId\0', b'NtWaitForAlertByThreadId\0'):
            if content.count(name) != 1:
                raise ValueError('WineD3D native-alert procedure layout is unsupported')
            offset = content.index(name)
            section = next((part for part in pe.sections
                            if part.PointerToRawData <= offset and
                            offset + len(name) <= part.PointerToRawData + part.SizeOfRawData), None)
            if section is None or not section.Characteristics & 0x40000000 or section.Characteristics & 0x20000000:
                raise ValueError('WineD3D native-alert probe must be in readable data')
            locations.append(offset)
    prepared = bytearray(content)
    for offset in locations:
        prepared[offset] = 0
    return bytes(prepared)


def read_dependency(archive, entry):
    files = {}
    with zipfile.ZipFile(archive) as bundle:
        damaged=bundle.testzip()
        if damaged is not None:
            raise ValueError('ZIP member is damaged: '+damaged)
        for filename in entry['files']:
            folder=entry.get('archive_directory')
            members=[name for name in bundle.namelist()
                     if name==folder+'/'+filename] if folder else [
                         name for name in bundle.namelist() if Path(name).name==filename]
            if len(members)!=1:
                raise ValueError(entry['name']+' archive must contain one '+filename)
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
    if 'wined3d.dll' in files:
        files['wined3d.dll'] = use_wined3d_events(files['wined3d.dll'])
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
            except (ValueError,zipfile.BadZipFile,pefile.PEFormatError) as error:
                raise ValueError(f'Invalid cached dependency: {archive}: {error}. Replace this cached archive. '+recovery) from error
        else:
            print('Downloading dependency:',entry['url'],flush=True)
            with tempfile.TemporaryDirectory(prefix=archive.name+'.',dir=cache) as directory:
                pending = Path(directory)/archive.name
                try:
                    urllib.request.urlretrieve(entry['url'],pending)
                    files,patched=read_dependency(pending,entry)
                except (ValueError,zipfile.BadZipFile,pefile.PEFormatError,urllib.error.URLError) as error:
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
        if entry['name']=='dxc':
            # These generated artifacts belong to the retired translation path.
            for name in ('d3d11.dll','nvgi.dll'):
                (output/name).unlink(missing_ok=True)
        if patched is not None:
            (output/'d3d11.dll').write_bytes(patched)
            shutil.copyfile(target/'dxgi.dll',output/entry['import_name'])
            shutil.copyfile(target/'wined3d.dll',output/'wined3d.dll')
        else:
            for filename in entry['files']:shutil.copyfile(target/filename,output/filename)
    for name in ('DXC_LICENSE_LLVM', 'DXC_LICENSE_MS', 'WINED3D_COPYING.LIB'):
        shutil.copyfile(ROOT/'LICENSES'/name,reference/name)
    return lock


if __name__ == '__main__':
    prepare()
