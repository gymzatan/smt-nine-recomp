"""Prepare retail and Chinese images with the accepted patch producers."""
import json
import os
import sys
import tempfile
from pathlib import Path
from common import ROOT, digest

sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT/'localization'))
sys.path.insert(0, str(ROOT/'writers'))
from game_patches import _prepare_original_layout
from apply_xbe import refresh_section_digests
from build_popup_ui import build as build_popup
from prepare_resources import fixed_fields


def prepare():
    runtime = ROOT/'runtime'
    runtime.mkdir(exist_ok=True)
    work = ROOT/'build/localization'
    work.mkdir(parents=True,exist_ok=True)
    japanese = _prepare_original_layout(ROOT/'data/japanese/default.xbe',runtime/'native_game_jp.xbe')
    chinese = work/'fields.xbe'
    fixed_fields(japanese,chinese)
    image = bytearray(chinese.read_bytes())
    if image[0x2AE1] != 0x7A:
        raise ValueError('Original startup texture instruction differs')
    image[0x140CB2] = 13
    refresh_section_digests(image)
    chinese.write_bytes(image)
    with tempfile.TemporaryDirectory(prefix='popup-',dir=work) as directory:
        prepared = Path(directory)/'native_game.xbe'
        layout = build_popup(chinese,prepared)
        os.replace(prepared,runtime/'native_game.xbe')
    receipt = {'japanese_sha256':digest(japanese),'chinese_sha256':digest(runtime/'native_game.xbe'),
               'popup_layout':layout,'source':'Original layout, fixed-field writer and popup assembly APIs'}
    (work/'images.json').write_text(json.dumps(receipt,indent=2)+'\n')
    return receipt


if __name__ == '__main__':
    prepare()
