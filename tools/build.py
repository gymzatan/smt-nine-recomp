"""Build Japanese NINE, optionally with Chinese, from a supported retail ISO."""
from pathlib import Path
import argparse
import configparser
import json
import os
import shutil
import sys
import time
from common import ROOT, digest, required_program, run, visual_studio


def build(iso, jobs, resume=False, with_chinese=False, update=False):
    start = time.monotonic()
    os.environ['PYTHONUTF8']='1'
    if os.name != 'nt':
        raise RuntimeError('The native release currently requires Windows x64.')
    japanese,chinese = ROOT/'data/japanese',ROOT/'data/chinese'
    progress_path=ROOT/'build/progress.json'
    if update:
        from extract_iso import verify_existing
        verify_existing(japanese)
        progress=json.loads(progress_path.read_text())
        previous=json.loads((ROOT/'build/completion.json').read_text())
        recorded=previous.get('with_chinese',progress.get('with_chinese'))
        if recorded is None:
            cache=(ROOT/'build/native/CMakeCache.txt').read_text()
            if 'SMT9_WITH_CHINESE:BOOL=ON' in cache: recorded=True
            elif 'SMT9_WITH_CHINESE:BOOL=OFF' in cache: recorded=False
            else: raise ValueError('Cannot identify installed language selection')
        if with_chinese and not recorded:
            raise ValueError('--update preserves installed languages; use a full build to add Chinese')
        with_chinese=recorded
        progress['with_chinese']=with_chinese
        for name in ('generation','gpu_dependencies','dsp'):
            progress['completed'].pop(name,None)
    elif resume:
        progress=json.loads(progress_path.read_text())
        if progress['iso_sha256']!=digest(iso):
            raise ValueError('Resume ISO differs from the original build input')
        if 'with_chinese' not in progress:
            raise ValueError('This build did not record its language selection; use a clean checkout')
        if progress['with_chinese']!=with_chinese:
            raise ValueError('Resume must use the same --with-chinese selection as the original build')
    else:
        progress={'iso_sha256':digest(iso),'with_chinese':with_chinese,'completed':{}}
    if not resume and not update and (japanese.exists() or chinese.exists()):
        raise ValueError('A full source build needs an empty data directory; use a clean checkout or preserve and move your existing data first.')
    subtitle_tools = {name:str(required_program(name)) for name in ('ffmpeg','ffprobe')} if with_chinese and not update else {}
    rustup = required_program('rustup')
    cmake,ninja,environment = visual_studio()
    def phase(name,action,outputs):
        completed=progress['completed'].get(name)
        if update and name not in ('generation','gpu_dependencies','dsp') and completed is None:
            raise ValueError('Missing installed resource receipt for '+name+'; finish the original build first')
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
    if update:
        resources=['base_resources','native_fonts','native_atlases','scripts','rts','staff','subtitles'] if with_chinese else []
        for name in resources+['images','title']:
            phase(name,None,[])
    if with_chinese and not update:
        phase('base_resources',lambda:shutil.copytree(japanese,chinese,dirs_exist_ok=True),[])
        from prepare_fonts import prepare as fonts
        phase('native_fonts',fonts,lambda:list((chinese/'Media/SysFont').iterdir()))
        phase('native_atlases',lambda:run([sys.executable,ROOT/'font_pipeline/build_native_fonts.py'],cwd=ROOT),lambda:list((ROOT/'runtime/fonts').glob('*.bin')))
        from prepare_resources import scripts,rts,staff
        phase('scripts',lambda:scripts(japanese,chinese),lambda:[p for directory in ['Script','Scriptf'] for p in (chinese/'Media'/directory).iterdir() if p.is_file()])
        phase('rts',lambda:rts(japanese,chinese),[chinese/'Media/RTS/Scripts/RTSScript.bin'])
        phase('staff',lambda:staff(japanese,chinese),[chinese/'Media/staff/staff-list.txt'])
    codegen_image=ROOT/('runtime/native_game.xbe' if with_chinese else 'build/localization/native_codegen.xbe')
    if not update:
        from prepare_images import prepare as images
        phase('images',lambda:images(with_chinese=with_chinese),[codegen_image,ROOT/'runtime/native_game_jp.xbe'])
        from prepare_title import prepare as title
        phase('title',title,[ROOT/'runtime/title_layers.bin',ROOT/'game/src/app_icon.ico'])
    if with_chinese and not update:
        phase('subtitles',lambda:run([sys.executable,ROOT/'localization/ending_subtitles/build_subtitles.py'],cwd=ROOT),
              [chinese/'Media/Videos'/f'video_{n}.xmv' for n in [27,28,33,35,60]])
    from generate import generate
    phase('generation',lambda:generate(codegen_image,ROOT/'build/generated'),lambda:list((ROOT/'build/generated').glob('*')))
    from dependencies import prepare as dependencies
    phase('gpu_dependencies',dependencies,lambda:[p for p in (ROOT/'runtime/backends').rglob('*.dll')])
    dsp_environment=dict(environment)
    dsp_environment['CARGO_TARGET_DIR']=str(ROOT/'build_dsp_jit')
    phase('dsp',lambda:run([rustup,'run','1.96.0','cargo','build','--manifest-path',ROOT/'reference/dsp56300_release/dsp56300-0.1.3/Cargo.toml',
         '--locked','--release','--target','x86_64-pc-windows-msvc','-p','dsp56300-emu-ffi','--jobs',jobs],env=dsp_environment,cwd=ROOT),
         [ROOT/'build_dsp_jit/x86_64-pc-windows-msvc/release/dsp56300_emu_ffi.lib'])
    run([cmake,'-S',ROOT/'game','-B',ROOT/'build/native','-G','Ninja','-DCMAKE_MAKE_PROGRAM='+str(ninja),
         '-DCMAKE_BUILD_TYPE=Release','-DSMT9_MACHINE_CHECKPOINT=ON','-DSMT9_WITH_CHINESE='+('ON' if with_chinese else 'OFF'),
         '-DCMAKE_RUNTIME_OUTPUT_DIRECTORY='+str(ROOT)],env=environment)
    # CMake/Ninja owns the dependency graph. Translation units and Cargo crates
    # have separate outputs; preparation and code generation remain sequential.
    run([cmake,'--build',ROOT/'build/native','--target','smt9_recomp','Config','--parallel',jobs],env=environment)
    if update:
        for registration in sorted((ROOT/'runtime/languages').glob('*.ini')):
            language=configparser.ConfigParser()
            language.read(registration,encoding='utf-16')
            if language.get('Language','State',fallback='')!='Ready': continue
            package_name=language['Language']['Package']
            if Path(package_name).name!=package_name or package_name in ('.','..'):
                raise ValueError('Unsafe language package registration')
            package=ROOT/'runtime/languages/packages'/package_name
            if not (package/'build/CMakeCache.txt').is_file():
                raise ValueError('Imported language needs its original source build directory: '+package_name)
            run([cmake,'--build',package/'build','--target','smt9_recomp','--parallel',jobs],env=environment)
            language_receipt=package/'receipt.json'
            record=json.loads(language_receipt.read_text(encoding='utf-8'))
            record['executable_sha256']=digest(package/'bin/smt9_recomp.exe')
            language_receipt.write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8')
    receipt={'iso_sha256':progress['iso_sha256'],'seconds':time.monotonic()-start,'executable_sha256':digest(ROOT/'smt9_recomp.exe'),
             'config_sha256':digest(ROOT/'Config.exe'),'source':'Retail ISO plus repository source/translation inputs and declared build dependencies',
             'with_chinese':with_chinese,'program_update':update,**subtitle_tools,'python':sys.version,'gameplay_validation':'Build completion is not a full-playthrough claim.'}
    (ROOT/'build/completion.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print('Build complete. Open Config.exe, then smt9_recomp.exe.',flush=True)


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso',type=Path)
    parser.add_argument('--jobs',type=int,default=4)
    parser.add_argument('--resume',action='store_true',help='Resume an interrupted build in the same unchanged checkout')
    parser.add_argument('--update',action='store_true',help='Rebuild programs in place using verified installed resources; no ISO or resource regeneration')
    parser.add_argument('--with-chinese',action='store_true',help='Also build Chinese game resources, fonts and subtitled movies (requires SimSun, SimHei and FFmpeg/FFprobe)')
    args=parser.parse_args()
    if not 1<=args.jobs<=32:
        parser.error('--jobs must be between 1 and 32')
    if args.update and (args.iso or args.resume):
        parser.error('--update does not use --iso or --resume')
    if not args.update and not args.iso:
        parser.error('--iso is required for a full build or --resume')
    build(args.iso.resolve() if args.iso else None,args.jobs,args.resume,args.with_chinese,args.update)
