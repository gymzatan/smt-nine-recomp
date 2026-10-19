"""Run the actual download/verify/install workflow against local release fixtures."""
from pathlib import Path
import hashlib, json, shutil, subprocess, zipfile
import pytest

ROOT=Path(__file__).resolve().parents[1]
def sha(data): return hashlib.sha256(data).hexdigest()
def quoted(path): return "'"+str(path).replace("'","''")+"'"

@pytest.mark.parametrize('language,encoding', [
    ('Chinese','utf-16'), ('Japanese','utf-16'), ('English','utf-16'),
    ('Japanese','ascii'), ('"English"','ascii'),
])
def test_interface_language_matches_config(tmp_path, language, encoding):
    (tmp_path/'runtime').mkdir()
    (tmp_path/'smt9_recomp.exe').write_bytes(b'game')
    (tmp_path/'runtime/native_game_jp.xbe').write_bytes(b'Japanese')
    ini=tmp_path/'runtime/NINE.ini'
    content=f'[Interface]\nLanguage={language}\n\n[Display]\nLanguage=Chinese\n'
    ini.write_text(content,encoding=encoding)
    before=ini.read_bytes()
    script=f'''
. {quoted(ROOT/'tools/updater.ps1')} -Installer {quoted(ROOT/'Apply-Update.ps1')} -LibraryOnly
[Threading.Thread]::CurrentThread.CurrentUICulture = [Globalization.CultureInfo]::GetCultureInfo('de-DE')
$script:interfaceLanguage=Get-InterfaceLanguage {quoted(tmp_path)}
$value=Text 'english-text' 'chinese-text' 'japanese-text'
Write-Output ($script:interfaceLanguage+'|'+$value)
'''
    result=subprocess.run([shutil.which('powershell.exe'),'-NoProfile','-ExecutionPolicy','Bypass','-Command',script],capture_output=True)
    expected=language.strip('"')
    assert result.returncode==0,result.stderr
    assert result.stdout.decode().strip()==expected+'|'+expected.lower()+'-text'
    assert ini.read_bytes()==before

@pytest.mark.parametrize('variant,expected', [('japanese','English'),('japanese-chinese','Chinese')])
def test_interface_language_uses_config_build_default(tmp_path,variant,expected):
    (tmp_path/'runtime').mkdir()
    (tmp_path/'smt9_recomp.exe').write_bytes(b'game')
    (tmp_path/'runtime/native_game_jp.xbe').write_bytes(b'Japanese')
    if variant=='japanese-chinese':(tmp_path/'runtime/native_game.xbe').write_bytes(b'Chinese')
    script=f'''
. {quoted(ROOT/'tools/updater.ps1')} -Installer {quoted(ROOT/'Apply-Update.ps1')} -LibraryOnly
[Threading.Thread]::CurrentThread.CurrentUICulture = [Globalization.CultureInfo]::GetCultureInfo('ja-JP')
Get-InterfaceLanguage {quoted(tmp_path)}
'''
    result=subprocess.run([shutil.which('powershell.exe'),'-NoProfile','-ExecutionPolicy','Bypass','-Command',script],capture_output=True)
    assert result.returncode==0,result.stderr
    assert result.stdout.decode().strip()==expected
    assert not (tmp_path/'runtime/NINE.ini').exists()

@pytest.mark.parametrize('variant', ['japanese','japanese-chinese'])
@pytest.mark.parametrize('case', ['valid','checksum','traversal','missing','tampered'])
def test_online_update(tmp_path,variant,case):
    target=tmp_path/'installed';target.mkdir();(target/'runtime').mkdir()
    (target/'smt9_recomp.exe').write_bytes(b'old game')
    (target/'Config.exe').write_bytes(b'old config')
    (target/'runtime/native_game_jp.xbe').write_bytes(b'Japanese')
    (target/'runtime/NINE.ini').write_bytes(b'keep settings')
    (target/'runtime/instant.slot1.state').write_bytes(b'keep checkpoint')
    (target/'data').mkdir();(target/'data/save.bin').write_bytes(b'keep save')
    identities=[{'path':'runtime/native_game_jp.xbe','sha256':sha(b'Japanese')}]
    if variant=='japanese-chinese':
        (target/'runtime/native_game.xbe').write_bytes(b'Chinese')
        identities.append({'path':'runtime/native_game.xbe','sha256':sha(b'Chinese')})
    rows=[{'path':n,'sha256':sha(data)} for n,data in [('smt9_recomp.exe',b'new game'),('Config.exe',b'new config'),('Updater.exe',b'new updater')]]
    manifest={'format':'smt-nine-program-update','source_commit':'a'*40,'required_files':identities,'files':rows}
    name='SMT-NINE-recomp-v8.7.6-update-'+variant+'.zip'
    archive=tmp_path/name
    with zipfile.ZipFile(archive,'w') as z:
        for row in rows:z.writestr('payload/'+row['path'],{'smt9_recomp.exe':b'new game','Config.exe':b'new config','Updater.exe':b'new updater'}[row['path']])
        z.writestr('UPDATE_MANIFEST.json',json.dumps(manifest))
        if case=='traversal':z.writestr('../outside.txt',b'unsafe')
    sums=tmp_path/'SHA256SUMS.txt';sums.write_text(sha(archive.read_bytes())+'  '+name+'\n')
    def asset(path):return {'name':path.name,'size':path.stat().st_size,'digest':'sha256:'+sha(path.read_bytes()),'browser_download_url':'https://github.com/gymzatan/smt-nine-recomp/releases/download/v8.7.6/'+path.name}
    release={'tag_name':'v8.7.6','draft':False,'prerelease':False,'assets':[asset(archive),asset(sums)]}
    if case=='missing':release['assets'].pop(0)
    if case=='checksum':archive.write_bytes(archive.read_bytes()+b'corruption')
    (tmp_path/'release.json').write_text(json.dumps(release))
    work=tmp_path/'work';work.mkdir()
    script=tmp_path/'check.ps1'
    script.write_text(f'''
$ErrorActionPreference='Stop'
. {quoted(ROOT/'tools/updater.ps1')} -Target {quoted(target)} -Installer {quoted(ROOT/'Apply-Update.ps1')} -LibraryOnly
function Get-ReleaseMetadata {{ Get-Content -Raw -LiteralPath {quoted(tmp_path/'release.json')} | ConvertFrom-Json }}
function Invoke-WebRequest {{ param($Uri,$Headers,[switch]$UseBasicParsing,$OutFile,$TimeoutSec)
    Copy-Item -LiteralPath (Join-Path {quoted(tmp_path)} ([Uri]$Uri).Segments[-1]) -Destination $OutFile
}}
$plan=Get-UpdatePlan {quoted(target)} {quoted(work)} {quoted(ROOT/'Apply-Update.ps1')}
if ($plan.ChangedFiles -ne 3) {{ throw 'Wrong pending count' }}
if (([IO.File]::ReadAllText({quoted(target/'smt9_recomp.exe')})) -ne 'old game') {{ throw 'Check wrote installed files' }}
{("[IO.File]::WriteAllText("+quoted(work/'bundle/payload/Config.exe')+",'tampered')") if case=='tampered' else ''}
Install-UpdatePlan {quoted(target)} {quoted(work)} {quoted(ROOT/'Apply-Update.ps1')} | ConvertTo-Json
$again=& {quoted(ROOT/'Apply-Update.ps1')} -Target {quoted(target)} -Bundle {quoted(work/'bundle')} -CheckOnly
if ($again.ChangedFiles -ne 0) {{ throw 'Not current after update' }}
''',encoding='utf-8-sig')
    result=subprocess.run([shutil.which('powershell.exe'),'-NoProfile','-ExecutionPolicy','Bypass','-File',str(script)],capture_output=True)
    assert (result.returncode==0)==(case=='valid'),result.stdout+result.stderr
    if case!='valid':
        expected={'checksum':b'checksum differs','traversal':b'Unsafe update path','missing':b'unique update','tampered':b'payload is missing or damaged'}[case]
        assert expected in result.stderr,result.stderr
    assert (target/'smt9_recomp.exe').read_bytes()==(b'new game' if case=='valid' else b'old game')
    assert (target/'runtime/NINE.ini').read_bytes()==b'keep settings'
    assert (target/'runtime/instant.slot1.state').read_bytes()==b'keep checkpoint'
    assert (target/'data/save.bin').read_bytes()==b'keep save'
    assert not (tmp_path/'outside.txt').exists()
