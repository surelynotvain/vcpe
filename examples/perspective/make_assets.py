#!/usr/bin/env python3
"""Deterministic hand-drawn engine-demo textures; contains no external game art."""
from pathlib import Path
from PIL import Image, ImageDraw
import random
root=Path(__file__).resolve().parent
out=root/'generated';out.mkdir(exist_ok=True)
rng=random.Random(11)
pages=[]
for floor in (False,True):
 im=Image.new('RGBA',(64,64)); d=ImageDraw.Draw(im)
 for y in range(64):
  for x in range(64):
   v=rng.randint(-9,9);base=(85,82,70) if floor else (119,110,89)
   im.putpixel((x,y),tuple(max(0,min(255,c+v)) for c in base)+(255,))
 for y in range(0,64,16):
  d.line((0,y,63,y),fill=(37,35,30,255),width=2)
  for x in range(-16 if y%32 else 0,64,32):
   d.line((x,y,x,y+15),fill=(42,39,32,255),width=2)
   d.line((x+2,y+2,x+30,y+2),fill=(143,130,101,255))
 pages.append(im);im.save(out/('floor.png' if floor else 'wall.png'))
im=Image.new('RGBA',(128,128));d=ImageDraw.Draw(im)
for i in range(8):
 x=(i%4)*32;y=(i//4)*64
 # Flat guard cutout: eight hand-authored facing variants in a tiny atlas.
 body=(90+abs(i-4)*9,100,106,255)
 d.ellipse((x+11,y+3,x+21,y+15),fill=(187,154,111,255))
 d.polygon([(x+10,y+17),(x+23,y+17),(x+27,y+42),(x+6,y+42)],fill=body)
 d.rectangle((x+8,y+40,x+14,y+60),fill=(51,43,35,255))
 d.rectangle((x+18,y+40,x+24,y+60),fill=(51,43,35,255))
 if i in (0,1,7):
  d.rectangle((x+12,y+8,x+14,y+9),fill=(25,22,18,255));d.rectangle((x+19,y+8,x+21,y+9),fill=(25,22,18,255))
 if i in (2,3,4):d.rectangle((x+24,y+18,x+28,y+51),fill=(180,171,148,255))
 else:d.rectangle((x+3,y+18,x+7,y+51),fill=(180,171,148,255))
 d.rectangle((x+9,y+26,x+23,y+29),fill=(170,148,93,255))
pages.append(im);im.save(out/'views.png')
atlas=Image.new('RGBA',(256,128));atlas.paste(pages[0],(0,0));atlas.paste(pages[1],(64,0));atlas.paste(pages[2],(128,0))
q=atlas.quantize(colors=256,method=Image.Quantize.FASTOCTREE,dither=Image.Dither.NONE)
palette=q.getpalette('RGBA');palette=(palette+[0]*1024)[:1024]
pixels=bytes(q.getdata())
# T8 swizzle: 16-byte by 8-row blocks, keeping the tiny atlas in the GE cache.
raw=bytearray()
for y in range(0,128,8):
 for x in range(0,256,16):
  for row in range(8):raw+=pixels[(y+row)*256+x:(y+row)*256+x+16]
raw+=bytes(palette)
q.convert('RGBA').save(out/'atlas.png')
(root/'src/assets.h').write_text('/* Generated original procedural art, shared swizzled T8 atlas. */\n'
 +'static unsigned char texture_data[] __attribute__((aligned(64))) = {\n'
 +'\n'.join(','.join(str(x) for x in raw[i:i+32])+',' for i in range(0,len(raw),32))+'\n};\n')
print('Shared T8 atlas:',len(raw),'bytes; all scene cards use one texture bind')
