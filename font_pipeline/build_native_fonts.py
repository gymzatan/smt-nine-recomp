"""Export native-resolution atlases using the active NINE font engines and maps."""
from pathlib import Path
import argparse,csv,hashlib,json,struct
import numpy as np
from PIL import Image,ImageFont
import build_smt_nine_f24 as f24
import build_smt_nine_f18 as f18

ROOT=Path(__file__).resolve().parent
GAME=ROOT.parent
FONT_SOURCES=GAME/'build/font_sources'
p=argparse.ArgumentParser()
p.add_argument('--scales',nargs='+',type=int,choices=[2,3,4],default=[2,3,4])
p.add_argument('--output',type=Path,default=GAME/'runtime/fonts')
p.add_argument('--font-directory',type=Path,default=GAME/'data/chinese/Media/SysFont')
p.add_argument('--font-extension-receipt',type=Path)
args=p.parse_args()
args.output.mkdir(parents=True,exist_ok=True)
mapping=json.loads((ROOT/'final_game_f24.json').read_text(encoding='utf-8'))
font_directory=args.font_directory.resolve()
extension={}
codes18,codes24=f18.read_sysfont_tbl(font_directory/'sysfont.tbl')
rows24=list(csv.DictReader((ROOT/'final_game_f24_full.csv').open(encoding='utf-8')))
rows18=list(csv.DictReader((ROOT/'final_game_f18.csv').open(encoding='utf-8')))
cn18={int(row['sjis_hex'],16):row['char'] for row in rows18}
extension_glyphs24=[]
extension_characters=json.loads((GAME/'config/font_extension.json').read_text(encoding='utf-8'))
code_to_slot24={code:slot for slot,code in enumerate(codes24)}
for char,code in extension_characters.items():
    slot=code_to_slot24[code];page=slot//(21*21);local=slot%(21*21)
    extension_glyphs24.append((char,page,(local%21)*24,(local//21)*24))
    cn18[code]=char
size18=f18.find_max_font_size_global_bbox(FONT_SOURCES/'simhei.ttf',list(mapping),16,17,size_max=24,size_min=10)
font18=ImageFont.truetype(str(FONT_SOURCES/'simhei.ttf'),size18)
receipt={'source_f24_map':str(ROOT/'final_game_f24.json'),'font_directory':str(font_directory),'f18_font_size':size18,'scales':[]}
if extension:
    receipt['font_extension_receipt']={'path':str(args.font_extension_receipt.resolve()),'sha256':hashlib.sha256(args.font_extension_receipt.read_bytes()).hexdigest(),'characters':extension_characters,'resources':extension['outputs']}
for scale in args.scales:
    output=args.output/f'font{scale}.bin';records=[];page_metadata=[]
    font24=ImageFont.truetype(str(FONT_SOURCES/'SimSun.ttf'),int(24*scale*8*.92))
    for kind,cell in [('f24',24),('f18',18)]:
        path=font_directory/f'sys_{kind}.xpr';data=path.read_bytes();header=struct.unpack_from('<I',data,8)[0];native_pages=[data[i:i+131072] for i in range(header,len(data),131072)]
        assert all(len(page)==131072 for page in native_pages)
        canvases=[np.array(Image.frombytes('RGBA',(512,512),page,'bcn',1).resize((512*scale,512*scale),Image.Resampling.NEAREST)) for page in native_pages]
        if kind=='f24':
            glyphs=[(row['char'],int(row['page']),int(row['pixel_x']),int(row['pixel_y'])) for row in rows24 if row['source']=='new_cn']+extension_glyphs24
        else:
            glyphs=[]
            for slot,code in enumerate(code for code in codes18 if f18.f18_code_consumes_slot(code)):
                page=slot//(28*28);local=slot%(28*28)
                if page==8 or code not in cn18:continue
                glyphs.append((cn18[code],page,(local%28)*18,(local//28)*18))
        for char,page,x,y in glyphs:
            pixels=np.zeros((cell*scale,cell*scale,4),dtype=np.uint8)
            if kind=='f24':
                fill,edge,outline=f24.render_char_masks(char,font24,oversample=8,cell_px=24*scale,outline_width=scale)
                pixels[outline]=[0,0,0,255];pixels[edge]=[90,91,102,255];pixels[fill]=[180,182,205,255]
            else:
                states=f18.build_f18_cell_states(char,font18,mode='cov',cov_font_path=FONT_SOURCES/'simhei.ttf',cov_oversample=4,cov_thr_fill=80,cov_thr_edge=20,cov_hyst_iters=4,native_scale=scale)
                pixels[states==1]=[74,77,82,255];pixels[states==3]=[135,137,148,255];pixels[states==2]=[197,198,214,255]
            canvases[page][y*scale:(y+cell)*scale,x*scale:(x+cell)*scale]=pixels
        for index,(base,canvas) in enumerate(zip(native_pages,canvases)):
            bgra=canvas[:,:,[2,1,0,3]].tobytes();records.append(struct.pack('<IIII',512*scale,512*scale,len(base),len(bgra))+base+bgra);page_metadata.append({'font':kind,'page':index,'base_sha256':hashlib.sha256(base).hexdigest(),'pixels_sha256':hashlib.sha256(bgra).hexdigest()})
        # Keep one concrete high-resolution Chinese glyph example per font.
        sample=next(item for item in glyphs if item[0]=='游')
        char,page,x,y=sample;Image.fromarray(canvases[page][y*scale:(y+cell)*scale,x*scale:(x+cell)*scale]).save(args.output/f'{kind}_{scale}x.png')
        if extension_characters:
            extension_samples=[]
            for char in extension_characters:
                _,page,x,y=next(item for item in glyphs if item[0]==char)
                extension_samples.append(canvases[page][y*scale:(y+cell)*scale,x*scale:(x+cell)*scale])
            Image.fromarray(np.concatenate(extension_samples,axis=1)).save(args.output/f'{kind}_{scale}x_extension.png')
        print(kind,scale,'glyphs',len(glyphs),'pages',len(native_pages),flush=True)
    output.write_bytes(b'N9FONT\0\0'+struct.pack('<II',scale,len(records))+b''.join(records))
    receipt['scales'].append({'scale':scale,'path':str(output),'bytes':output.stat().st_size,'sha256':hashlib.sha256(output.read_bytes()).hexdigest(),'pages':page_metadata})
(args.output/'receipt.json').write_text(json.dumps(receipt,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
