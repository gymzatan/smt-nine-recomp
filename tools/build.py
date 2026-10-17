"""Build Japanese/Chinese NINE locally from a supported retail ISO."""
from pathlib import Path
import argparse
import json
import os
import shutil
import sys
import time
from common import ROOT, digest, required_program, run, visual_studio


def build(iso, jobs, resume=False):
    start = time.monotonic()
    os.environ['PYTHONUTF8']='1'
    if os.name != 'nt':
        raise RuntimeError('The native release currently requires Windows x64.')
    ffmpeg, ffprobe = required_program('ffmpeg'),required_program('ffprobe')
    rustup = required_program('rustup')
    cmake,ninja,environment = visual_studio()
    japanese,chinese = ROOT/'data/japanese',ROOT/'data/chinese'
    progress_path=ROOT/'build/progress.json'
    if resume:
        progress=json.loads(progress_path.read_text())
        if progress['iso_sha256']!=digest(iso):
            raise ValueError('Resume ISO differs from the original build input')
    else:
        progress={'iso_sha256':digest(iso),'completed':{}}
    if not resume and (japanese.exists() or chinese.exists()):
        raise ValueError('A full source build needs an empty data directory; use a clean checkout or preserve and move your existing data first.')
    def phase(name,action,outputs):
        completed=progress['completed'].get(name)
        if completed is not None:
            for path,expected in completed.items():
                if digest(ROOT/path)!=expected:
                    raise ValueError('Completed build output changed: '+path)
            print('Reusing completed phase:',name,flush=True)
            return
        print('Preparing phase:',name,flush=True)
        action()
        paths=outputs() if callable(outputs) else outputs
        progress['completed'][name]={p.relative_to(ROOT).as_posix():digest(p) for p in paths}
        progress_path.parent.mkdir(parents=True,exist_ok=True)
        progress_path.write_text(json.dumps(progress,indent=2)+'\n')
    from extract_iso import extract
    phase('retail',lambda:extract(iso,japanese),lambda:[p for p in japanese.rglob('*') if p.is_file()])
    phase('base_resources',lambda:shutil.copytree(japanese,chinese,dirs_exist_ok=True),[])
    from prepare_fonts import prepare as fonts
    phase('native_fonts',fonts,lambda:list((chinese/'Media/SysFont').iterdir()))
    phase('native_atlases',lambda:run([sys.executable,ROOT/'font_pipeline/build_native_fonts.py'],cwd=ROOT),lambda:list((ROOT/'runtime/fonts').glob('*.bin')))
    from prepare_resources import scripts,rts,staff
    phase('scripts',lambda:scripts(japanese,chinese),lambda:[p for directory in ['Script','Scriptf'] for p in (chinese/'Media'/directory).iterdir() if p.is_file()])
    phase('rts',lambda:rts(japanese,chinese),[chinese/'Media/RTS/Scripts/RTSScript.bin'])
    phase('staff',lambda:staff(japanese,chinese),[chinese/'Media/staff/staff-list.txt'])
    from prepare_images import prepare as images
    phase('images',images,[ROOT/'runtime/native_game.xbe',ROOT/'runtime/native_game_jp.xbe'])
    from prepare_title import prepare as title
    phase('title',title,[ROOT/'runtime/title_layers.bin',ROOT/'game/src/app_icon.ico'])
    phase('subtitles',lambda:run([sys.executable,ROOT/'localization/ending_subtitles/build_subtitles.py'],cwd=ROOT),
          [chinese/'Media/Videos'/f'video_{n}.xmv' for n in [27,28,33,35,60]])
    from generate import generate
    phase('generation',lambda:generate(ROOT/'runtime/native_game.xbe',ROOT/'build/generated'),lambda:list((ROOT/'build/generated').glob('*')))
    from dependencies import prepare as dependencies
    phase('gpu_dependencies',dependencies,lambda:[p for p in (ROOT/'runtime/backends').rglob('*.dll')])
    dsp_environment=dict(environment)
    dsp_environment['CARGO_TARGET_DIR']=str(ROOT/'build_dsp_jit')
    phase('dsp',lambda:run([rustup,'run','1.96.0','cargo','build','--manifest-path',ROOT/'reference/dsp56300_release/dsp56300-0.1.3/Cargo.toml',
         '--locked','--release','--target','x86_64-pc-windows-msvc','-p','dsp56300-emu-ffi','--jobs',jobs],env=dsp_environment,cwd=ROOT),
         [ROOT/'build_dsp_jit/x86_64-pc-windows-msvc/release/dsp56300_emu_ffi.lib'])
    run([cmake,'-S',ROOT/'game','-B',ROOT/'build/native','-G','Ninja','-DCMAKE_MAKE_PROGRAM='+str(ninja),
         '-DCMAKE_BUILD_TYPE=Release','-DSMT9_MACHINE_CHECKPOINT=ON','-DCMAKE_RUNTIME_OUTPUT_DIRECTORY='+str(ROOT)],env=environment)
    # CMake/Ninja owns the dependency graph. Translation units and Cargo crates
    # have separate outputs; preparation and code generation remain sequential.
    run([cmake,'--build',ROOT/'build/native','--target','smt9_recomp','Config','--parallel',jobs],env=environment)
    receipt={'iso_sha256':digest(iso),'seconds':time.monotonic()-start,'executable_sha256':digest(ROOT/'smt9_recomp.exe'),
             'config_sha256':digest(ROOT/'Config.exe'),'source':'Retail ISO plus repository source/translation inputs and declared build dependencies',
             'ffmpeg':str(ffmpeg),'ffprobe':str(ffprobe),'python':sys.version,'gameplay_validation':'Build completion is not a full-playthrough claim.'}
    (ROOT/'build/completion.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print('Build complete. Open Config.exe, then smt9_recomp.exe.',flush=True)


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso',required=True,type=Path)
    parser.add_argument('--jobs',type=int,default=4)
    parser.add_argument('--resume',action='store_true',help='Resume an interrupted build in the same unchanged checkout')
    args=parser.parse_args()
    if not 1<=args.jobs<=32:
        parser.error('--jobs must be between 1 and 32')
    build(args.iso.resolve(),args.jobs,args.resume)
