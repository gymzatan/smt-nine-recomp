"""Pack reviewed title layers for the existing native texture-asset consumer.

ImageGen supplies only the local reconstruction and copyright alpha matte.
Foreground RGB is always copied from the original extracted texture.
"""
from pathlib import Path
import hashlib, json, re, struct, sys
import numpy as np
from PIL import Image, ImageFilter
from scipy.ndimage import binary_fill_holes, binary_dilation
from expand_font import parse_xpr_descriptors

root=Path(__file__).resolve().parent.parent
title_origin=float(re.search(r'#define NINE_TITLE_UI_ORIGIN ([0-9.]+)f',
    (root/'game/src/font_assets.h').read_text()).group(1))
out=root/'build/title'
source=root/'build/title'
original=np.asarray(Image.open(source/'Title_00_512x512_A8R8G8B8.png').convert('RGBA')).copy()
repair=np.asarray(Image.open(root/'patches/title/background_repair.png').convert('RGBA'))
matte=Image.open(root/'patches/title/copyright_guide.png').convert('L')
background=original.copy()
background[90:156,60:270]=[0,0,0,255]
# Bound the generated repair to the genuinely occluded copyright region.
x0,y0,x1,y1=40,388,204,438
yy,xx=np.mgrid[y0:y1,x0:x1]
blend=np.clip(np.minimum.reduce([xx-x0,x1-1-xx,yy-y0,y1-1-yy])/8,0,1)[...,None]
background[y0:y1,x0:x1,:3]=np.rint(original[y0:y1,x0:x1,:3]*(1-blend)+repair[:,:,:3]*blend).astype(np.uint8)
foreground=np.zeros_like(original)
foreground[90:156,60:270,:3]=original[90:156,60:270,:3]
foreground[90:156,60:270,3]=np.any(original[90:156,60:270,:3]!=0,axis=2)*255
region=original[392:433,44:200,:3].astype(np.int16)
# The generated matte is a region guide; its redrawn glyph contours are not
# authoritative. Original neutral outlines close around the coloured glyphs.
outline=(region.max(2)-region.min(2)<45)&(region.min(2)>65)
red=(region[:,:,0]>100)&(region[:,:,0]>region[:,:,1]*1.4)&(region[:,:,0]>region[:,:,2]*1.4)
blue=(region[:,:,2]-region[:,:,1]>45)&(region[:,:,1]-region[:,:,0]>12)&(region[:,:,1]-region[:,:,0]<45)
blue[:,111:]=False
mask=binary_fill_holes(outline|red|blue)
edge=binary_dilation(mask)&((region.max(2)-region.min(2)<65)&(region.max(2)>24))
mask=(mask|edge)&(np.asarray(matte.filter(ImageFilter.MaxFilter(5)))[2:43,2:158]>16)
alpha=(mask*255).astype(np.uint8)
# The original mesh shares the S's blue hue. Use the reviewed clean S alpha,
# aligned to its native cap height; all foreground RGB still comes from XPR.
s_alpha=Image.open(root/'patches/title/copyright_s_alpha.png').convert('L')
alpha[:,91:112]=0
alpha[7:32,91:111]=np.asarray(s_alpha)
foreground[392:433,44:200,:3]=original[392:433,44:200,:3]
foreground[392:433,44:200,3]=alpha
foreground[foreground[:,:,3]==0,:3]=0
assert np.array_equal(foreground[foreground[:,:,3]>0,:3],original[foreground[:,:,3]>0,:3])
allowed=np.zeros((512,512),bool);allowed[90:156,60:270]=True;allowed[y0:y1,x0:x1]=True
assert np.array_equal(background[~allowed],original[~allowed])
bg=Image.fromarray(background);fg=Image.fromarray(foreground)
bg.save(out/'background_tile.png');fg.save(out/'logos.png')
fg.crop((60,90,270,156)).save(out/'nine_logo.png')
fg.crop((44,392,200,433)).save(out/'atlus_copyright.png')
right=Image.open(source/'Title_01_512x512_A8R8G8B8.png').convert('RGBA').crop((0,0,128,480))
full=Image.new('RGBA',(640,480));full.paste(bg.crop((0,0,512,480)));full.paste(right,(512,0));full.save(out/'background.png')
logo_canvas=Image.new('RGBA',(640,480));logo_canvas.paste(fg.crop((0,0,512,480)))
original_full=Image.new('RGBA',(640,480));original_full.paste(Image.fromarray(original).crop((0,0,512,480)));original_full.paste(right,(512,0))
for name,width,height in [('4_3',720,540),('16_9',960,540),('16_10',864,540),('21_9',1260,540)]:
    stretch=original_full.resize((width,height),Image.Resampling.BILINEAR)
    corrected=full.resize((width,height),Image.Resampling.BILINEAR)
    layer=logo_canvas.resize((720,height),Image.Resampling.BILINEAR)
    corrected.alpha_composite(layer,(round((width-720)*title_origin/640),0))
    if name=='4_3':corrected=stretch.copy()
    stretch.convert('RGB').save(out/f'original_{name}.png');corrected.convert('RGB').save(out/f'preview_{name}.png')
    if name=='16_9':
        compare=Image.new('RGB',(width,height*2+36),(25,25,25));compare.paste(stretch,(0,0));compare.paste(corrected,(0,height+36));compare.save(out/'comparison.png')
xpr=(root/'data/japanese/Media/Textures/net/Title.xpr').read_bytes()
assert hashlib.sha256(xpr).hexdigest()=='79376784d379c5c2b215721eedff6f23efdac14623fc53c04c9553966fe9ee8c'
desc=parse_xpr_descriptors(xpr[:2048]);assert len(desc)==3 and [d[1] for d in desc]==[0,1048576,2097152]
raw=xpr[2048:2048+1048576]
bgra=lambda a:a[:,:,[2,1,0,3]].tobytes()
bundle=root/'runtime/title_layers.bin'
bundle.write_bytes(b'N9TITLE\0'+struct.pack('<III',512,512,len(raw))+raw+bgra(background)+bgra(foreground))
receipt={'source_xpr_sha256':hashlib.sha256(xpr).hexdigest(),'raw_texture_bytes':len(raw),'foreground_rgb_exact_original_at_every_visible_texel':True,'background_exact_outside_edit_rectangles':True,'local_background_repair_box':[x0,y0,x1,y1],'title_black_cleanup_box':[60,90,270,156],'copyright_alpha_source':'ImageGen region guide; original neutral outline, red and blue fills retain original glyph contours and exclude cyan mesh; RGB strictly from original source.','bundle':str(bundle.relative_to(root)),'bundle_sha256':hashlib.sha256(bundle.read_bytes()).hexdigest(),'preview_only_not_runtime_acceptance':True,'generated_background':'generated_background.png','generated_matte':'generated_matte.png','tool':'Built-in ImageGen; deterministic original-texel packing for native consumer.'}
receipt['copyright_s_alpha']={'reference':'atlus_clean_reference.png','reference_crop':[1338,195,1599,524],'native_crop_in_copyright':[91,7,111,32],'original_rgb_retained':True}
(out/'assets.json').write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8')
print(json.dumps(receipt))
