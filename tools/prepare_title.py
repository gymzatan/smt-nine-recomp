"""Read original title/icon pixels from the ISO with existing format consumers."""
import ctypes
import json
import shutil
import struct
import sys
from pathlib import Path
from PIL import Image
from common import ROOT, visual_studio, run, digest

sys.path.insert(0, str(ROOT/'vendor/xboxrecomp'))
from tools.xbe_parser.xbe_parser import XBEParser


def prepare():
    work = ROOT/'build/title'
    work.mkdir(parents=True, exist_ok=True)
    cmake, ninja, environment = visual_studio()
    compiler = shutil.which('cl.exe', path=environment['PATH'])
    if compiler is None:
        raise RuntimeError('Visual Studio environment does not contain cl.exe')
    run([compiler,'/nologo','/O2','/LD',ROOT/'tools/asset_pixels.c', '/Fe:'+str(work/'asset_pixels.dll')], cwd=work, env=environment)
    decoder = ctypes.CDLL(str(work/'asset_pixels.dll'))
    decoder.nine_unswizzle.argtypes = [ctypes.c_void_p,ctypes.c_void_p,ctypes.c_uint,ctypes.c_uint]
    data = (ROOT/'data/japanese/Media/Textures/net/Title.xpr').read_bytes()
    if struct.unpack_from('<III',data) != (0x30525058, len(data), 2048):
        raise ValueError('Original title texture header differs')
    for index in range(2):
        raw = ctypes.create_string_buffer(data[2048+index*1048576:2048+(index+1)*1048576])
        pixels = ctypes.create_string_buffer(1048576)
        decoder.nine_unswizzle(pixels,raw,512,512)
        Image.frombytes('RGBA',(512,512),pixels.raw,'raw','BGRA').save(work/f'Title_{index:02}_512x512_A8R8G8B8.png')
    image_path = ROOT/'data/japanese/default.xbe'
    image = XBEParser(str(image_path)).parse()
    section = next(s for s in image.sections if s.name=='$$XTIMAGE')
    raw = image_path.read_bytes()[section.raw_addr:section.raw_addr+section.raw_size]
    if raw[:4] != b'XPR0' or len(raw) != 10240:
        raise ValueError('Original title icon format differs')
    icon = Image.frombytes('RGBA',(128,128),raw[2048:],'bcn',1)
    icon.save(ROOT/'game/src/app_icon.ico',sizes=[(16,16),(24,24),(32,32),(48,48),(64,64),(128,128)])
    run([sys.executable,ROOT/'font_pipeline/build_native_title.py'])
    (work/'receipt.json').write_text(json.dumps({'title_layers_sha256':digest(ROOT/'runtime/title_layers.bin'),
        'icon_source':'Original retail XBE $$XTIMAGE', 'texture_decoder':'Runtime xbox_unswizzle_rect'},indent=2)+'\n')


if __name__ == '__main__':
    prepare()
