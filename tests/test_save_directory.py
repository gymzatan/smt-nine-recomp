"""Use the production path/migration code on real existing save directories."""
from pathlib import Path
import shutil
import subprocess
import pytest

ROOT=Path(__file__).resolve().parents[1]

@pytest.fixture
def probe(tmp_path):
    folder=tmp_path/'保存目录';folder.mkdir()
    header=(ROOT/'game/src/game_paths.h.in').read_text(encoding='utf-8')
    header=header.replace('#cmakedefine01 SMT9_WITH_CHINESE','#define SMT9_WITH_CHINESE 1')
    for key,value in {'SMT9_ROOT_FROM_EXE':'.','SMT9_IMAGE_RELATIVE':'runtime/native_game.xbe',
                      'SMT9_CHECKPOINT_RELATIVE':'runtime/instant.state','SMT9_IMPORTED_LANGUAGE_ID':''}.items():
        header=header.replace('@'+key+'@',value)
    (folder/'game_paths.h').write_text(header,encoding='utf-8')
    code=folder/'probe.c';code.write_text(r'''
#include "game_paths.h"
#include <assert.h>
int wmain(int argc,wchar_t **argv) {
    _set_error_mode(_OUT_TO_STDERR);_set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
    assert(argc==2 && nine_paths_init());wchar_t error[512];
    if(!nine_saves_prepare(argv[1],error,512)){fwprintf(stderr,L"%s\n",error);return 2;}
    printf("%s\n",nine_paths.checkpoint);return 0;
}
''',encoding='utf-8')
    result=subprocess.run([shutil.which('cl'),'/nologo','/O2','/MD','/utf-8','/I'+str(folder),'/I'+str(ROOT/'game/src'),
        str(code),str(ROOT/'game/src/game_paths.c'),str(ROOT/'game/src/language.c'),'/Fe:'+str(folder/'probe.exe'),
        '/link','shell32.lib'],cwd=folder,capture_output=True)
    assert result.returncode==0,result.stdout+result.stderr
    return folder

def write(root,name,data):
    p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(data);return p

def run(root,language='Chinese'):
    return subprocess.run([str(root/'probe.exe'),language],capture_output=True)

def test_migrate_once_preserves_originals_and_separates_states(probe):
    names={'data/chinese/UDATA/title/slot/megaten9.sav':b'ordinary',
           'data/chinese/TDATA/title/profile.bin':b'profile','runtime/save/UserData/other.sav':b'user',
           'runtime/instant.slot3.state':b'built-in exact image state',
           'runtime/languages/packages/English-disc/instant.slot2.state':b'import exact image state',
           'runtime/save/Cache/asset.bin':b'disposable cache'}
    originals={write(probe,n,b):(b,None) for n,b in names.items()}
    originals={p:(b,p.stat().st_mtime_ns) for p,(b,_) in originals.items()}
    result=run(probe);assert result.returncode==0,result.stderr
    target=probe/'save'
    expected={'UDATA/title/slot/megaten9.sav':b'ordinary','TDATA/title/profile.bin':b'profile',
              'UserData/other.sav':b'user','states/legacy-built-in/instant.slot3.state':b'built-in exact image state',
              'states/legacy-imports/English-disc/instant.slot2.state':b'import exact image state'}
    assert {p.relative_to(target).as_posix():p.read_bytes() for p in target.rglob('*') if p.is_file()}==expected
    assert not (probe/'save.migrating').exists()
    assert all((p.read_bytes(),p.stat().st_mtime_ns)==before for p,before in originals.items())
    write(target,'UDATA/title/slot/megaten9.sav',b'new progress')
    write(probe,'data/chinese/UDATA/title/slot/megaten9.sav',b'stale legacy')
    outputs=[]
    for language in ['Chinese','Japanese','English']:
        result=run(probe,language);assert result.returncode==0,result.stderr
        outputs.append(Path(result.stdout.decode('utf-8').strip()))
        assert outputs[-1]==target/'states'/language/'instant.state'
        assert (target/'UDATA/title/slot/megaten9.sav').read_bytes()==b'new progress'
    assert len(set(outputs))==3

def test_existing_save_is_authoritative(probe):
    write(probe,'save/UDATA/progress.sav',b'current')
    write(probe,'data/chinese/UDATA/progress.sav',b'legacy')
    assert run(probe,'Japanese').returncode==0
    assert (probe/'save/UserData').is_dir()
    assert (probe/'save/UDATA/progress.sav').read_bytes()==b'current'
    assert (probe/'data/chinese/UDATA/progress.sav').read_bytes()==b'legacy'

def test_failed_copy_does_not_publish_empty_save_or_remove_source(probe):
    write(probe,'data/chinese/UDATA',b'not a directory')
    result=run(probe);assert result.returncode!=0
    assert not (probe/'save').exists()
    assert (probe/'data/chinese/UDATA').read_bytes()==b'not a directory'
    assert (probe/'save.migrating').is_dir()
    assert run(probe).returncode!=0

def test_new_install_and_invalid_language(probe):
    assert run(probe,'../elsewhere').returncode!=0
    assert not (probe/'save').exists()
    assert run(probe,'Japanese').returncode==0
    assert (probe/'save/states/Japanese').is_dir()
