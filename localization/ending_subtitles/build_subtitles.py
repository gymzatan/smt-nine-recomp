"""Render editable Chinese captions and produce native XMV assets with original audio."""
import argparse
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path
from PIL import ImageFont
from write_xmv import write,ROOT,BASE


def ass_time(seconds):
    n=round(seconds*100);return f'{n//360000}:{n//6000%60:02}:{n//100%60:02}.{n%100:02}'


def srt_time(seconds):
    n=round(seconds*1000);return f'{n//3600000:02}:{n//60000%60:02}:{n//1000%60:02},{n%1000:03}'


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--only',type=int,nargs='+')
    parser.add_argument('--export-only',action='store_true')
    args=parser.parse_args()
    data=json.loads((BASE/'captions.json').read_text(encoding='utf-8'))
    fonts=ROOT/'build/font_sources'
    font=ImageFont.truetype(str(fonts/'simhei.ttf'),24)
    font_assets=ROOT/'build/subtitles/fonts'
    font_assets.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(fonts/'simhei.ttf',font_assets/'simhei.ttf')
    assert hashlib.sha256((fonts/'simhei.ttf').read_bytes()).digest()==hashlib.sha256((font_assets/'simhei.ttf').read_bytes()).digest()
    header='''[Script Info]
ScriptType: v4.00+
PlayResX: 640
PlayResY: 480
WrapStyle: 2
ScaledBorderAndShadow: yes

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Chinese,SimHei,24,&H00FFFFFF,&H00FFFFFF,&H00000000,&H80000000,0,0,0,0,100,100,0,0,1,2,1,2,32,32,44,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
'''
    subtitles=ROOT/'build/subtitles/text';subtitles.mkdir(parents=True,exist_ok=True)
    encoded=ROOT/'build/subtitles/encoded';encoded.mkdir(parents=True,exist_ok=True)
    out=ROOT/'data/chinese/Media/Videos';out.mkdir(parents=True,exist_ok=True)
    for video in data['videos']:
        number=video['number']
        if args.only and number not in args.only: continue
        ass=header;srt=[];last=0
        for i,cue in enumerate(video['cues']):
            assert cue['start']>=last and cue['end']>cue['start'];last=cue['end']
            lines=cue['chinese'].split('\n');assert 1<=len(lines)<=2
            assert all(font.getlength(line)<=576 for line in lines),(number,i,lines)
            assert not any(c in cue['chinese'] for c in '{}\\')
            text=cue['chinese'].replace('\n',r'\N')
            ass+=f"Dialogue: 0,{ass_time(cue['start'])},{ass_time(cue['end'])},Chinese,,0,0,0,,{{\\fad(150,150)}}{text}\n"
            srt.append(f"{i+1}\n{srt_time(cue['start'])} --> {srt_time(cue['end'])}\n{cue['chinese']}\n")
        ass_path=subtitles/f'video_{number}.ass';ass_path.write_text(ass,encoding='utf-8-sig')
        (subtitles/f'video_{number}.srt').write_text('\n'.join(srt),encoding='utf-8-sig')
        if args.export_only: continue
        target=encoded/f'video_{number}.nut'
        vf=f"subtitles={ass_path.relative_to(ROOT).as_posix()}:fontsdir={font_assets.relative_to(ROOT).as_posix()}"
        command=['ffmpeg','-v','warning','-i',str(ROOT/'data/japanese/Media/Videos'/f'video_{number}.xmv'),
            '-an','-vf',vf,'-c:v','wmv2','-b:v','1100k','-maxrate:v','1600k','-bufsize:v','256k','-g','90','-pix_fmt','yuv420p',
            '-fps_mode','passthrough','-enc_time_base','1:30','-y',str(target)]
        process=subprocess.run(command,cwd=ROOT,capture_output=True)
        (encoded/f'video_{number}.encode.log').write_bytes(process.stderr)
        assert process.returncode==0,process.stderr.decode(errors='replace')
        write(ROOT/'data/japanese/Media/Videos'/f'video_{number}.xmv',target,out/f'video_{number}.xmv')
        print('Built Chinese subtitles',number,flush=True)

if __name__=='__main__': main()
