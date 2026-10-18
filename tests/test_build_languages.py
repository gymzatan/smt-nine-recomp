"""Check optional resource dependencies and the production language consumers."""
from pathlib import Path
import importlib.util
import json
import shutil
import subprocess
import sys
from types import SimpleNamespace
import pytest

ROOT=Path(__file__).resolve().parents[1]


@pytest.mark.parametrize('chinese',[False,True])
def test_build_language_dependencies_and_resume(tmp_path,monkeypatch,chinese):
    sys.path.insert(0,str(ROOT/'tools'))
    spec=importlib.util.spec_from_file_location('nine_build',ROOT/'tools/build.py')
    build=importlib.util.module_from_spec(spec);spec.loader.exec_module(build)
    monkeypatch.setattr(build,'ROOT',tmp_path)
    calls=[]
    def write(relative):
        path=tmp_path/relative;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(b'fixture')
        return path
    iso=write('disc.iso')
    def tool(name):
        calls.append(name)
        assert chinese or name not in ('ffmpeg','ffprobe')
        return name
    monkeypatch.setattr(build,'required_program',tool)
    monkeypatch.setattr(build,'visual_studio',lambda:('cmake','ninja',{}))
    def run(command,**kwargs):
        text=' '.join(map(str,command));calls.append(text)
        if 'build_native_fonts.py' in text:
            assert chinese;write('runtime/fonts/atlas.bin')
        elif 'build_subtitles.py' in text:
            assert chinese
            for n in (27,28,33,35,60):write(f'data/chinese/Media/Videos/video_{n}.xmv')
        elif command[0]=='rustup':write('build_dsp_jit/x86_64-pc-windows-msvc/release/dsp56300_emu_ffi.lib')
        elif '--build' in command:write('smt9_recomp.exe');write('Config.exe')
    monkeypatch.setattr(build,'run',run)
    def extract(*args):
        calls.append('retail');write('data/japanese/default.xbe')
    def fonts():
        assert chinese;calls.append('fonts');write('data/chinese/Media/SysFont/font.xpr')
    def scripts(*args):
        assert chinese
        for directory in ('Script','Scriptf'):write('data/chinese/Media/'+directory+'/script.mgp')
    def images(with_chinese):
        assert with_chinese==chinese;calls.append('images')
        write('runtime/native_game_jp.xbe')
        write('runtime/native_game.xbe' if chinese else 'build/localization/native_codegen.xbe')
    def generate(image,destination):
        assert image.is_file();calls.append('generation');write('build/generated/shared.c')
    modules={
        'extract_iso':SimpleNamespace(extract=extract,verify_existing=lambda destination:{'iso_sha256':build.digest(iso)}),
        'prepare_fonts':SimpleNamespace(prepare=fonts),
        'prepare_resources':SimpleNamespace(scripts=scripts,
            rts=lambda *args:write('data/chinese/Media/RTS/Scripts/RTSScript.bin'),
            staff=lambda *args:write('data/chinese/Media/staff/staff-list.txt')),
        'prepare_images':SimpleNamespace(prepare=images),
        'prepare_title':SimpleNamespace(prepare=lambda:(write('runtime/title_layers.bin'),write('game/src/app_icon.ico'))),
        'generate':SimpleNamespace(generate=generate),
        'dependencies':SimpleNamespace(prepare=lambda:write('runtime/backends/backend.dll')),
    }
    for name,module in modules.items():monkeypatch.setitem(sys.modules,name,module)
    build.build(iso,2,with_chinese=chinese)
    assert (tmp_path/'data/chinese').exists()==chinese
    assert (tmp_path/'runtime/fonts').exists()==chinese
    assert (tmp_path/'runtime/native_game.xbe').exists()==chinese
    assert any('-DSMT9_WITH_CHINESE='+('ON' if chinese else 'OFF') in call for call in calls)
    receipt=json.loads((tmp_path/'build/completion.json').read_text())
    assert receipt['with_chinese']==chinese
    assert ('ffmpeg' in receipt)==chinese
    build.build(iso,2,resume=True,with_chinese=chinese)
    assert calls.count('retail')==calls.count('generation')==calls.count('images')==1
    protected={p:p.read_bytes() for p in (tmp_path/'data').rglob('*') if p.is_file()}
    previous_calls=len(calls)
    build.build(None,2,update=True)
    assert calls.count('retail')==calls.count('images')==1
    assert calls.count('generation')==2
    assert not {'ffmpeg','ffprobe'} & set(calls[previous_calls:])
    assert all(p.read_bytes()==content for p,content in protected.items())
    assert json.loads((tmp_path/'build/completion.json').read_text())['with_chinese']==chinese
    with pytest.raises(ValueError,match='same --with-chinese'):
        build.build(iso,2,resume=True,with_chinese=not chinese)
    progress_path=tmp_path/'build/progress.json'
    progress=json.loads(progress_path.read_text());del progress['with_chinese']
    progress_path.write_text(json.dumps(progress))
    with pytest.raises(ValueError,match='did not record its language selection'):
        build.build(iso,2,resume=True,with_chinese=chinese)


@pytest.mark.parametrize('chinese',[False,True])
def test_native_language_selection_and_defaults(tmp_path,chinese):
    compiler=shutil.which('cl');assert compiler,'requires the existing MSVC environment'
    header=(ROOT/'game/src/game_paths.h.in').read_text(encoding='utf-8')
    header=header.replace('#cmakedefine01 SMT9_WITH_CHINESE','#define SMT9_WITH_CHINESE '+str(int(chinese)))
    for key,value in {'SMT9_ROOT_FROM_EXE':'.','SMT9_IMAGE_RELATIVE':'runtime/native_game.xbe',
                      'SMT9_CHECKPOINT_RELATIVE':'runtime/instant.state','SMT9_IMPORTED_LANGUAGE_ID':''}.items():
        header=header.replace('@'+key+'@',value)
    (tmp_path/'game_paths.h').write_text(header,encoding='utf-8')
    runtime=tmp_path/'runtime';runtime.mkdir()
    package=runtime/'languages/packages/English-test'
    (package/'bin').mkdir(parents=True)
    (package/'bin/smt9_recomp.exe').write_bytes(b'fixture')
    (package/'native_game.xbe').write_bytes(b'fixture')
    (runtime/'languages/English.ini').write_text('[Language]\nState=Ready\nPackage=English-test\nName=English\nFontPages=9\n',encoding='utf-16')
    source=tmp_path/'probe.c'
    source.write_text(r'''
#include "config.h"
#include "game_paths.h"
#include <assert.h>
#include <stdlib.h>
#include <wchar.h>
int main(void) {
    _set_error_mode(_OUT_TO_STDERR);_set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
    assert(nine_paths_init());
    NineConfig config;wchar_t error[256];assert(nine_config_read(&config,error,256));
    assert(!wcscmp(config.language,SMT9_WITH_CHINESE?L"Chinese":L"Japanese"));
    assert(!wcscmp(config.interface_language,SMT9_WITH_CHINESE?L"Chinese":L"English"));
    NineLanguage item,languages[8];unsigned count=nine_language_list(languages,8);
    assert(count==(SMT9_WITH_CHINESE?3:2));
    assert(nine_language_resolve(L"Chinese",&item,error,256)==SMT9_WITH_CHINESE);
    assert(nine_language_resolve(L"Japanese",&item,error,256)&&item.font_pages==9&&!item.external);
    assert(nine_language_resolve(L"English",&item,error,256)&&item.font_pages==9&&item.external);
    wcscpy_s(config.language,NINE_LANGUAGE_ID,L"English");
    wcscpy_s(config.interface_language,16,L"Japanese");
    assert(nine_config_write(&config,error,256));assert(nine_config_read(&config,error,256));
    assert(!wcscmp(config.language,L"English")&&!wcscmp(config.interface_language,L"Japanese"));
    wcscpy_s(config.backend,16,L"D3D12");assert(nine_config_write(&config,error,256));
    assert(nine_config_read(&config,error,256)&&!wcscmp(config.backend,L"D3D12"));
    wcscpy_s(config.language,NINE_LANGUAGE_ID,L"Chinese");assert(nine_config_write(&config,error,256));
    assert(nine_config_read(&config,error,256)&&!wcscmp(config.language,L"Chinese"));
    assert(nine_language_resolve(config.language,&item,error,256)==SMT9_WITH_CHINESE);
    return 0;
}
''',encoding='utf-8')
    exe=tmp_path/'probe.exe'
    subprocess.run([compiler,'/nologo','/utf-8','/O2','/I'+str(tmp_path),'/I'+str(ROOT/'game/src'),
        str(source),*[str(ROOT/'game/src'/name) for name in ('config.c','language.c','game_paths.c')],
        '/Fe:'+str(exe),'/link','shell32.lib'],cwd=tmp_path,check=True,capture_output=True)
    subprocess.run([str(exe)],cwd=tmp_path,check=True,capture_output=True)
