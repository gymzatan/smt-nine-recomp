"""Select public source files and reject game/build content before publication."""
from pathlib import Path
import hashlib
import json
import re
from common import ROOT

IGNORED = {'.git','.venv','__pycache__','.pytest_cache','data','runtime','bin','build','build_dsp_jit'}
FORBIDDEN = {'.iso','.xbe','.xpr','.xmv','.sav','.state','.ttf','.ttc','.exe','.dll','.obj','.pdb','.lib','.exp','.log','.pyc'}


def source_files(root=ROOT):
    for path in sorted(Path(root).rglob('*')):
        relative=path.relative_to(root)
        if not path.is_file() or relative.parts[0] in IGNORED or any(
                part in {'__pycache__','.pytest_cache'} for part in relative.parts):
            continue
        if relative.parts[:2] in [('writers','output'),('writers','translated')]:
            continue
        if relative.parts[:2] == ('reference','gpu_runtime'):
            continue
        if len(relative.parts)==1 and (path.name in {'smt9_recomp.exe','Config.exe','Updater.exe','smt9_recomp.pdb','Config.pdb','Updater.pdb'} or path.name=='backends'):
            continue
        if relative.parts[0]=='backends':
            continue
        if relative.as_posix()=='game/src/app_icon.ico':
            continue
        license_text = relative.parts[0]=='LICENSES' and path.name=='WINED3D_COPYING.LIB'
        if path.suffix.lower() in FORBIDDEN and not license_text:
            raise ValueError('Game/build content in source selection: '+relative.as_posix())
        if re.fullmatch(r'recomp_\d+\.c',path.name) or path.name in {'recomp_dispatch.c','recomp_funcs.h'}:
            raise ValueError('Generated game code in source selection: '+relative.as_posix())
        if path.suffix.lower() in {'.py','.c','.h','.in','.json','.md','.cmd','.yml','.yaml'}:
            text=path.read_text(encoding='utf-8')
            if re.search(r'[A-Za-z]:[/\\]Others[/\\]Emulators',text):
                raise ValueError('Development-machine path in source: '+relative.as_posix())
        yield path


def manifest(root=ROOT):
    return [{'path':p.relative_to(root).as_posix(),'bytes':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in source_files(root)]


if __name__=='__main__':
    rows=manifest()
    print(json.dumps({'source_files':len(rows),'source_bytes':sum(r['bytes'] for r in rows),'game_and_build_content':'excluded'},indent=2))
