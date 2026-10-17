"""Rebuild accepted font maps through the existing NINE glyph renderers."""
from pathlib import Path
import csv
import json
import os
import shutil
import struct
import sys
from common import ROOT, digest

sys.path.insert(0, str(ROOT/'font_pipeline'))
from fontTools.ttLib import TTFont
from expand_font import expand_xpr
from append_missing_tail_chars import patch_f24_tail
from build_smt_nine_f18 import patch_f18_preserve


def prepare():
    sources = ROOT/'build/font_sources'
    sources.mkdir(parents=True, exist_ok=True)
    system_fonts = Path(os.environ['WINDIR'])/'Fonts'
    simsun, simhei = system_fonts/'simsun.ttc', system_fonts/'simhei.ttf'
    if not simsun.is_file() or not simhei.is_file():
        raise RuntimeError('Install Windows Simplified Chinese supplemental fonts (SimSun and SimHei).')
    font = TTFont(str(simsun), fontNumber=0)
    font.save(sources/'SimSun.ttf')
    shutil.copyfile(simhei, sources/'simhei.ttf')
    layout = json.loads((ROOT/'config/font_layout.json').read_text())
    extension = json.loads((ROOT/'config/font_extension.json').read_text())
    directory = ROOT/'data/chinese/Media/SysFont'
    directory.mkdir(parents=True, exist_ok=True)
    for name in ['sys_f18.xpr', 'sys_f24.xpr']:
        shutil.copyfile(ROOT/'data/japanese/Media/SysFont'/name, directory/name)
        expand_xpr(directory/name, 4)
    codes18, codes24 = layout['codes18'], layout['codes24']
    table = struct.pack('<I', 4+2*len(codes18))+b''.join(struct.pack('>H', n) for n in codes18)
    table += bytes.fromhex(layout['f24_header'])+b''.join(struct.pack('>H', n) for n in codes24)
    (directory/'sysfont.tbl').write_bytes(table)
    mapping = json.loads((ROOT/'font_pipeline/final_game_f24.json').read_text(encoding='utf-8'))
    chars24 = [(int(code,16),char) for char,code in mapping.items()]+[(code,char) for char,code in extension.items()]
    rows18 = list(csv.DictReader((ROOT/'font_pipeline/final_game_f18.csv').open(encoding='utf-8')))
    chars18 = {int(row['sjis_hex'],16):row['char'] for row in rows18}
    chars18.update({code:char for char,code in extension.items()})
    for path, wanted in [(sources/'SimSun.ttf',[char for _,char in chars24]), (sources/'simhei.ttf',list(chars18.values()))]:
        cmap = TTFont(str(path)).getBestCmap()
        missing = sorted({char for char in wanted if ord(char) not in cmap})
        if missing:
            raise ValueError(f'Required glyphs are absent from {path.name}: '+''.join(missing))
    patch_f24_tail(directory/'sysfont.tbl', directory/'sys_f24.xpr', directory/'sys_f24.xpr', sources/'SimSun.ttf', characters=chars24)
    patch_f18_preserve(directory/'sys_f18.xpr', directory/'sys_f18.xpr', chars18, codes18,
        sources/'simhei.ttf', 18, 'cov', 4, 80., 20., 4, (197,198,214), (74,77,82))
    receipt = {'font_inputs':{str(path):digest(path) for path in [simsun,simhei]},
               'font_resources':{p.name:digest(p) for p in directory.iterdir()},
               'font_pages':13, 'source':'Existing expand_xpr, patch_f24_tail and patch_f18_preserve APIs'}
    (ROOT/'build/font_receipt.json').write_text(json.dumps(receipt, indent=2)+'\n')
    return receipt


if __name__ == '__main__':
    prepare()
