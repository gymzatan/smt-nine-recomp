"""Connect retail references and completed Chinese text to the existing writers."""
from pathlib import Path
import argparse
import copy
import hashlib
import json
import shutil
import sys
from common import ROOT, digest

sys.path.insert(0, str(ROOT/'writers'))
import write_script_binaries as writer


def hydrate(payload, source):
    """Read original strings/opcodes from the disc, using exported identities."""
    result = copy.deepcopy(payload)
    lines = []
    for part in result['parts']:
        raw = source[part['offset']:part['offset']+part['length']]
        if hashlib.sha256(raw).hexdigest() != part['source_sha256']:
            raise ValueError('Original text-reference identity differs')
        part['raw_hex'] = raw.hex()
        lines.append(writer.decode_script_bytes(raw))
        if part.get('bytecode_length') is not None:
            offset, length = part['bytecode_offset'], part['bytecode_length']
            part['bytecode_raw'] = source[offset:offset+length].hex()
    for page in result['pages']:
        if page.get('marker_offset') is not None and 'source_sha256' in page:
            offset, length = page['marker_offset'], page['marker_length']
            raw = source[offset:offset+length]
            if hashlib.sha256(raw).hexdigest() != page['source_sha256']:
                raise ValueError('Original page-boundary identity differs')
            page['marker_raw'] = raw.hex()
    result['storage_jp_lines'] = lines
    result['text'] = '\n'.join(lines)
    return result


def scripts(retail, chinese, only=None):
    table = ROOT/'translations/zh-CN/encoding.tbl'
    char_map = writer.load_tbl(table)
    records = []
    for path in sorted((ROOT/'translations/zh-CN/scripts').glob('*.json')):
        name = path.stem
        if only is not None and name not in only:
            continue
        projected = json.loads(path.read_text(encoding='utf-8'))
        for is_female, directory, side in [(False, 'Script', 'script_only'), (True, 'Scriptf', 'scriptf_only')]:
            source = retail/'Media'/directory/name
            raw = source.read_bytes()
            data = {side:[hydrate(payload, raw) for payload in projected[side]]}
            binary, applied = writer.apply_json_to_script_v2(source, data, char_map, is_scriptf=is_female)
            target = chinese/'Media'/directory/name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(binary)
            records.append({'path':target.relative_to(chinese).as_posix(), 'records':applied, 'sha256':digest(target)})
            print(f'Built {directory}/{name}: {applied} translations', flush=True)
    return records


def fixed_fields(source, destination):
    import apply_xbe
    fields = json.loads((ROOT/'translations/zh-CN/xbe_fields.json').read_text(encoding='utf-8'))
    original = (ROOT/'data/japanese/default.xbe').read_bytes()
    by_offset = {}
    for rows in fields['categories'].values():
        for row in rows:
            raw = original[row['offset']:row['offset']+row['length']]
            prefix = bytes.fromhex(row.get('prefix_hex',''))
            row['text'] = raw[len(prefix):].decode('cp932')
            row['raw_hex'] = raw.hex()
            by_offset[str(row['offset'])] = row
    # Keep the completed writer's formatting references where its reviewed
    # input differs from retail. Placeholders retain layout, not source prose.
    for offset, reference in fields.pop('writer_references').items():
        by_offset[offset]['text'] = reference
    work = ROOT/'build/localization'
    work.mkdir(parents=True, exist_ok=True)
    inputs = work/'xbe_fields.json'
    inputs.write_text(json.dumps(fields, ensure_ascii=False)+'\n', encoding='utf-8')
    apply_xbe.TRANSLATED_DIR = work
    apply_xbe.XBE_TRANSLATION_OVERRIDES.clear()
    if not apply_xbe.apply_translations(source, destination, tbl_path=ROOT/'translations/zh-CN/encoding.tbl', translations_path=inputs, patch_code=True):
        raise RuntimeError('Original fixed-field writer did not complete')
    if json.loads((work/'xbe_overflow_report.json').read_text()):
        raise ValueError('Fixed-field writer reported an overflow')


def rts(retail, chinese):
    import extract_rts_scripts
    import apply_rts_scripts
    source = retail/'Media/RTS/Scripts/RTSScript.bin'
    data = extract_rts_scripts.build_index(source)
    translations = json.loads((ROOT/'translations/zh-CN/rts.json').read_text(encoding='utf-8'))
    index = {(row['file'],row['index']):row for row in translations['messages']}
    for file in data['files']:
        for message in file['messages']:
            row = index[file['name'],message['message_index']]
            if hashlib.sha256(message['text'].encode('utf-8')).hexdigest() != row['source_sha256']:
                raise ValueError('RTS message identity differs')
            message['chinese'] = row['chinese']
    if len(data['menu_strings']['strings']) != len(translations['menu']):
        raise ValueError('RTS menu cardinality differs')
    for original, row in zip(data['menu_strings']['strings'],translations['menu']):
        if hashlib.sha256(original['text'].encode('utf-8')).hexdigest() != row['source_sha256']:
            raise ValueError('RTS menu identity differs')
        original['chinese'] = row['chinese']
    inputs = ROOT/'build/localization/rts.json'
    inputs.parent.mkdir(parents=True, exist_ok=True)
    inputs.write_text(json.dumps(data,ensure_ascii=False)+'\n',encoding='utf-8')
    apply_rts_scripts.TBL_CANDIDATES = [ROOT/'translations/zh-CN/encoding.tbl']
    target = chinese/'Media/RTS/Scripts/RTSScript.bin'
    target.parent.mkdir(parents=True,exist_ok=True)
    return apply_rts_scripts.build_output(source,inputs,target,dry_run=False)


def staff(retail, chinese):
    patch = json.loads((ROOT/'patches/staff.json').read_text())
    source = retail/'Media/staff/staff-list.txt'
    raw = source.read_bytes()
    if digest(source) != patch['source_sha256']:
        raise ValueError('Original staff-list identity differs')
    for offset,removed,inserted in reversed(patch['edits']):
        raw = raw[:offset]+bytes.fromhex(inserted)+raw[offset+removed:]
    if hashlib.sha256(raw).hexdigest() != patch['output_sha256']:
        raise ValueError('Staff-list correction identity differs')
    (chinese/'Media/staff/staff-list.txt').write_bytes(raw)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--only', nargs='+')
    args = parser.parse_args()
    result = scripts(ROOT/'data/japanese', ROOT/'data/chinese', args.only)
    receipt = ROOT/'build/localization/scripts.json'
    receipt.parent.mkdir(parents=True, exist_ok=True)
    receipt.write_text(json.dumps(result, indent=2)+'\n')
