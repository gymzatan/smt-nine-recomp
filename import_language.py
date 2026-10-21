"""Import a translated NINE ISO using the existing Xiso reader and recompiler."""
from pathlib import Path, PurePosixPath
import argparse
import bisect
import contextlib
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import traceback
import uuid

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / 'vendor/xboxrecomp'))
from tools.xiso.xdvdfs import Xiso
from tools.xbe_parser.xbe_parser import XBEParser
from tools.recomp import config
from tools.recomp.translator import BatchTranslator
from game_patches import _prepare_original_layout, prepare_battle_camera


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def register_imported_helpers(batch, dispatch, additions):
    text = dispatch.read_text(encoding='utf-8')
    entries = [(int(address,16),name,'') for address,name in re.findall(
        r'\{ 0x([0-9A-F]{8})u, \(recomp_func_t\)(\w+) \}',text)]
    existing = {address for address,_,_ in entries}
    if len(existing)!=len(entries) or existing.intersection(additions):
        raise ValueError('Imported helper duplicates an existing dispatch identity.')
    entries.extend((address,f'sub_{address:08X}','') for address in additions)
    # Retain the accepted extra forward declarations; the original writer
    # owns table ordering, cardinality and the full flat-address span.
    anchor = '} recomp_entry_t;'
    declarations = text[text.index(anchor)+len(anchor):text.index('static const recomp_entry_t g_recomp_table[]')]
    batch._write_dispatch_table(sorted(entries),str(dispatch),'recomp_funcs.h')
    emitted = dispatch.read_text(encoding='utf-8')
    emitted = emitted.replace(anchor+'\n\n',anchor+declarations,1)
    dispatch.write_text(emitted,encoding='utf-8')


def generate(package):
    """Retain accepted native bodies; lift only changed code and reachable caves."""
    image = package / 'native_game.xbe'
    original = ROOT / 'runtime/native_game_jp.xbe'
    source, baseline = image.read_bytes(), original.read_bytes()
    parsed, retail = XBEParser(str(image)).parse(), XBEParser(str(original)).parse()
    layout = lambda x: [(s.name, s.virtual_addr, s.raw_addr, s.raw_size) for s in x.sections]
    if layout(parsed) != layout(retail):
        raise ValueError('Imported executable section layout differs from the supported NINE build.')
    functions = json.loads((ROOT / 'config/functions.json').read_text(encoding='utf-8'))
    changed = set()
    changed_addresses = []
    for section in parsed.sections:
        if not section.flags & 4 or section.name in ('.data', '.rdata', '.data1'):
            continue
        offsets = [i for i in range(section.raw_size)
                   if source[section.raw_addr+i] != baseline[section.raw_addr+i]]
        if not offsets:
            continue
        for f in functions:
            start, end = int(f['start'], 16), int(f['end'], 16)
            lo = bisect.bisect_left(offsets, start-section.virtual_addr)
            if lo < len(offsets) and section.virtual_addr+offsets[lo] < end:
                changed.add(start)
        changed_addresses.extend(section.virtual_addr+offset for offset in offsets)
    generated = package / 'generated'
    shutil.copytree(ROOT / 'build/generated', generated)
    metadata = ROOT / 'config'
    config.configure_from_xbe(str(image))
    batch = BatchTranslator(str(image), str(metadata / 'functions.json'),
        str(metadata / 'labels.json'), str(metadata / 'identified_functions.json'),
        str(metadata / 'abi_functions.json'), continuations=True)
    # Boundary repair belongs to BatchTranslator; consume its final identities.
    uncovered=[]
    for va in changed_addresses:
        owners=[start for start,info in batch.func_db.items() if start<=va<info['end']]
        if not owners: uncovered.append(va)
        changed.update(owners)
    # Bytes with no existing compiled consumer remain in the loaded ISO image.
    # They do not justify inventing new function boundaries or a second engine.
    changed.difference_update(batch.translator.owned_function_starts)
    # The canonical CFG decoder follows the translation patch's direct branch
    # caves. Keep those instructions in their owner so flags/frame survive.
    virtual_end = max(s.virtual_addr+s.raw_size for s in parsed.sections)
    virtual = bytearray(virtual_end)
    for section in parsed.sections:
        virtual[section.virtual_addr:section.virtual_addr+section.raw_size] = source[section.raw_addr:section.raw_addr+section.raw_size]
    additions = {}
    for address in sorted(changed):
        info = batch.func_db[address]
        end = info['end']
        instructions, _ = batch.translator.decode_function(address, end)
        external = [i.jump_target for i in instructions if i.is_jump and i.jump_target is not None
                    and not address <= i.jump_target < end and i.jump_target not in batch.func_db]
        if external:
            recovered = batch.translator.disasm.disassemble_cfg(bytes(virtual[address:]), address,
                virtual_end, [address], stop_mnemonics=('int3','ud2','hlt','iret','iretd'))
            for instruction in recovered:
                target = instruction.jump_target
                if target is not None and not (address <= target < end) and target in batch.func_db:
                    raise ValueError(f'Noncontiguous patch branches into another function at {target:X}; explicit integration required.')
            recovered_end = max(i.end_address for i in recovered)
            batch.translator._recovered_cfg[address] = {'end': recovered_end, 'instructions': recovered, 'jump_tables': {}}
            instructions = recovered
        for instruction in instructions:
            target = instruction.call_target
            if target is not None and target not in batch.func_db:
                if not 0 < target < virtual_end:
                    raise ValueError(f'Imported call target is not file backed: {target:X}')
                body = batch.translator.disasm.disassemble_cfg(bytes(virtual[target:]), target,
                    virtual_end, [target], stop_mnemonics=('int3','ud2','hlt','iret','iretd'))
                if not body or not (body[-1].is_ret or body[-1].is_jump):
                    raise ValueError(f'New imported helper has no bounded return or tail: {target:X}')
                helper_end = max(i.end_address for i in body)
                additions[target] = {'start':f'0x{target:08X}', 'end':helper_end,
                    'name':f'sub_{target:08X}', 'size':helper_end-target}
                batch.func_db[target] = additions[target]
                batch.translator._recovered_cfg[target] = {'end':helper_end,'instructions':body,'jump_tables':{}}
    texts = {path: path.read_text(encoding='utf-8') for path in generated.glob('recomp_*.c')}
    records = []
    for address in sorted(changed):
        pattern = re.compile(rf'^void sub_{address:08X}(?P<suffix>_gen)?\(void\)\n\{{.*?^\}}', re.M|re.S)
        matches = [(path, match) for path, text in texts.items() for match in pattern.finditer(text)]
        if len(matches) != 1:
            raise ValueError(f'Changed function {address:X} has {len(matches)} native consumers; explicit integration required.')
        path, match = matches[0]
        body = batch.translate_single(address)
        if not body or 'TODO' in body or 'UNSUPPORTED' in body:
            raise ValueError(f'Original translator cannot lift changed function {address:X}.')
        if match['suffix']:
            body = body.replace(f'void sub_{address:08X}(void)',f'void sub_{address:08X}_gen(void)',1)
        definition = body.index('void sub_')
        texts[path] = texts[path][:match.start()] + body[definition:] + texts[path][match.end():]
        records.append(f'{address:08X}')
    for path, text in texts.items():
        path.write_text(text, encoding='utf-8')
    if additions:
        declarations = '\n'.join(f'void sub_{a:08X}(void);' for a in sorted(additions))+'\n'
        header = generated / 'recomp_funcs.h'
        header.write_text(header.read_text(encoding='utf-8')+'\n'+declarations,encoding='utf-8')
        bodies = []
        for address in sorted(additions):
            body = batch.translate_single(address)
            if not body or 'TODO' in body or 'UNSUPPORTED' in body:
                raise ValueError(f'Original translator cannot lift new helper {address:X}.')
            bodies.append(body)
        (generated/'recomp_imported_helpers.c').write_text('#define RECOMP_GENERATED_CODE\n#include "recomp_funcs.h"\n#include "kernel/recomp_continuation.h"\n#include <math.h>\n'+'\n'.join(bodies),encoding='utf-8')
        dispatch = generated/'recomp_dispatch.c'
        register_imported_helpers(batch,dispatch,additions)
    prepare_battle_camera(generated)
    return {'changed_functions':records,'new_helpers':[f'{a:08X}' for a in sorted(additions)],
            'changed_bytes_without_existing_function':[f'{a:08X}' for a in uncovered]}


def build_package(args,package,generation,fonts):
    languages=ROOT/'runtime/languages'
    sys.path.insert(0,str(ROOT/'tools'))
    from common import visual_studio,run as build_run
    cmake,ninja,environment=visual_studio()
    build=package/'build'; binary=package/'bin'
    print('Building imported native executable.',flush=True)
    build_run([cmake,'-S',ROOT/'game','-B',build,'-G','Ninja','-DCMAKE_MAKE_PROGRAM='+str(ninja),
        '-DCMAKE_BUILD_TYPE=Release','-DSMT9_MACHINE_CHECKPOINT=ON','-DSMT9_RECOMP_GEN_DIR='+str(package/'generated'),
        '-DSMT9_IMPORTED_LANGUAGE_ROOT='+package.as_posix(),'-DSMT9_IMPORTED_LANGUAGE_ID='+args.id,
        '-DCMAKE_RUNTIME_OUTPUT_DIRECTORY='+str(binary)],env=environment)
    build_run([cmake,'--build',build,'--target','smt9_recomp','--parallel','4'],env=environment)
    receipt={'language':args.id,'name':args.name,'iso':str(args.iso.resolve()),'iso_sha256':digest(args.iso),
        'source_xbe_sha256':digest(package/'default.xbe'),'native_xbe_sha256':digest(package/'native_game.xbe'),
        'executable_sha256':digest(binary/'smt9_recomp.exe'),'font_pages':fonts[0],**generation}
    (package/'receipt.json').write_text(json.dumps(receipt,indent=2),encoding='utf-8')
    pending=languages/(args.id+'.pending')
    pending.write_text('[Language]\nState=Ready\nPackage='+package.name+'\nName='+args.name+'\nFontPages='+str(fonts[0])+'\n',encoding='utf-16')
    os.replace(pending,languages/(args.id+'.ini'))
    print('Language ready:',package,flush=True)


def run(args):
    if not re.fullmatch(r'[A-Za-z][A-Za-z0-9_-]{0,30}',args.id) or args.id.casefold() in ('chinese','japanese'):
        raise ValueError('Use a new language ID of at most31 ASCII letters/digits/_/-, starting with a letter.')
    if not args.name or len(args.name)>79 or any(c in args.name for c in '\r\n[]"'):
        raise ValueError('Invalid language display name.')
    languages = ROOT/'runtime/languages'
    languages.mkdir(parents=True,exist_ok=True)
    lock = languages/(args.id+'.lock')
    lock.open('x').close()
    try:
        package = languages/'packages'/(args.id+'-'+uuid.uuid4().hex[:8])
        package.mkdir(parents=True)
        print('Extracting language ISO:',args.iso,flush=True)
        with Xiso(str(args.iso)) as iso:
            exe = iso.find('default.xbe')
            if exe is None:
                raise ValueError('ISO contains no default.xbe.')
            (package/'default.xbe').write_bytes(iso.read(exe))
            for directory,entry in iso.walk():
                if '/' in entry.name or '\\' in entry.name or ':' in entry.name:
                    raise ValueError('Unsafe ISO entry name.')
                relative = PurePosixPath(directory.replace('\\','/'))/entry.name
                if not relative.parts or relative.parts[0].casefold()!='media':
                    continue
                if relative.is_absolute() or any(p in ('..','.') or ':' in p for p in relative.parts):
                    raise ValueError('Unsafe ISO path.')
                target = package.joinpath(*relative.parts)
                if entry.is_dir: target.mkdir(parents=True,exist_ok=True)
                else:
                    target.parent.mkdir(parents=True,exist_ok=True)
                    iso.extract(entry,str(target))
        sys.path.insert(0,str(ROOT/'font_pipeline'))
        from expand_font import parse_xpr_descriptors,HEADER_SIZE,BC1_PAGE_BYTES
        fonts=[]
        for name in ('sys_f18.xpr','sys_f24.xpr'):
            data=(package/'Media/SysFont'/name).read_bytes()
            pages=len(parse_xpr_descriptors(data[:HEADER_SIZE]))
            if data[:4]!=b'XPR0' or len(data)!=HEADER_SIZE+pages*BC1_PAGE_BYTES or not 0<pages<128:
                raise ValueError('Imported font texture pages are invalid.')
            fonts.append(pages)
        if fonts[0]!=fonts[1]: raise ValueError('Imported font page counts disagree.')
        _prepare_original_layout(package/'default.xbe',package/'native_game.xbe')
        print('Regenerating changed native functions.',flush=True)
        generation=generate(package)
        build_package(args,package,generation,fonts)
    finally:
        lock.unlink()


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso',type=Path,required=True)
    parser.add_argument('--id',required=True)
    parser.add_argument('--name',required=True)
    args=parser.parse_args()
    logdir=ROOT/'runtime/languages';logdir.mkdir(parents=True,exist_ok=True)
    safe_id=args.id if re.fullmatch(r'[A-Za-z][A-Za-z0-9_-]{0,30}',args.id) else 'Invalid'
    with (logdir/(safe_id+'.import.log')).open('w',encoding='utf-8',buffering=1) as log:
        with contextlib.redirect_stdout(log),contextlib.redirect_stderr(log):
            try: run(args)
            except Exception as error:
                traceback.print_exc()
                (logdir/(safe_id+'.error.txt')).write_text('[Import]\nError='+str(error).replace('\n',' ')+'\n',encoding='utf-16')
                sys.exit(1)
