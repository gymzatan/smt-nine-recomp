"""Exercise the shipped installer on an existing play directory."""
from pathlib import Path
import hashlib
import json
import shutil
import subprocess
import sys
import pytest

ROOT=Path(__file__).resolve().parents[1]


def test_update_preserves_data_and_checks_entire_payload(tmp_path):
    shell=shutil.which('powershell.exe');assert shell
    bundle=tmp_path/'bundle';target=tmp_path/'game'
    bundle.mkdir();target.mkdir()
    shutil.copyfile(ROOT/'Apply-Update.ps1',bundle/'Apply-Update.ps1')
    def write(root,name,data):
        path=root/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(data);return path
    write(target,'smt9_recomp.exe',b'old program')
    write(target,'Config.exe',b'old config program')
    protected=[write(target,name,b'keep '+name.encode()) for name in
               ('runtime/NINE.ini','data/japanese/Media/resource.bin','data/chinese/Media/resource.bin','data/japanese/UDATA/save.bin','runtime/languages/English.ini','save/UDATA/ordinary.sav','save/states/Japanese/instant.slot1.state')]
    before={p:(p.read_bytes(),p.stat().st_mtime_ns) for p in protected}
    identity=write(target,'runtime/native_game_jp.xbe',b'compatible game')
    rows=[]
    for name,data in [('smt9_recomp.exe',b'new program'),('Config.exe',b'new config program'),('Updater.exe',b'new updater')]:
        write(bundle/'payload',name,data)
        rows.append({'path':name,'sha256':hashlib.sha256(data).hexdigest()})
    manifest={'format':'smt-nine-program-update','required_files':[{'path':'runtime/native_game_jp.xbe','sha256':hashlib.sha256(identity.read_bytes()).hexdigest()}],'files':rows}
    manifest_path=bundle/'UPDATE_MANIFEST.json'
    def run(*extra):
        manifest_path.write_text(json.dumps(manifest))
        return subprocess.run([shell,'-NoProfile','-ExecutionPolicy','Bypass','-File',str(bundle/'Apply-Update.ps1'),'-Target',str(target),*extra],capture_output=True)
    write(bundle/'payload','Config.exe',b'broken')
    assert run().returncode!=0
    assert (target/'smt9_recomp.exe').read_bytes()==b'old program'
    write(bundle/'payload','Config.exe',b'new config program')
    manifest['files'].append({'path':'../outside','sha256':'0'*64})
    assert run().returncode!=0
    manifest['files'].pop()
    manifest['forbidden_files']=['runtime/native_game.xbe']
    marker=write(target,'runtime/native_game.xbe',b'Chinese-enabled installation')
    assert run().returncode!=0
    assert (target/'smt9_recomp.exe').read_bytes()==b'old program'
    marker.unlink()
    manifest['forbidden_files']=[]
    manifest['language_resources']='JapaneseChinese'
    assert run().returncode!=0
    assert (target/'smt9_recomp.exe').read_bytes()==b'old program'
    write(target,'runtime/native_game.xbe',b'Installed Chinese text may differ')
    assert run('-CheckOnly').returncode==0
    assert (target/'smt9_recomp.exe').read_bytes()==b'old program'
    assert not (target/'Updater.exe').exists()
    assert run().returncode==0
    assert (target/'smt9_recomp.exe').read_bytes()==b'new program'
    stamps={p:p.stat().st_mtime_ns for p in (target/'smt9_recomp.exe',target/'Config.exe')}
    assert run().returncode==0
    assert all(p.stat().st_mtime_ns==stamp for p,stamp in stamps.items())
    assert all((p.read_bytes(),p.stat().st_mtime_ns)==record for p,record in before.items())


def test_unchanged_generated_output_keeps_timestamp(tmp_path):
    sys.path.insert(0,str(ROOT/'tools'))
    from common import write_if_changed
    path=tmp_path/'generated.c'
    write_if_changed(path,'accepted\n')
    timestamp=path.stat().st_mtime_ns
    write_if_changed(path,'accepted\n')
    assert path.stat().st_mtime_ns==timestamp
    write_if_changed(path,'changed\n')
    assert path.read_text()=='changed\n'
