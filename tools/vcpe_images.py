"""Engine-independent PNG/JPEG -> VSPR sprite section. Original dimensions retained.
Single RGBA frames use the same palette/page builder as GameMaker converters.
images: [(name, PIL_RGBA), ...]; returns section bytes and name-to-image-id map.
No game-specific paths, source formats, generated tables or runtime dependency."""
import struct
import numpy as np
from gm.gmtex import TexBuilder

def build(images):
    class Empty:
        txtr=[]
    b=TexBuilder(Empty());metrics=[];names={}
    for name,image in images:
        image=image.convert('RGBA');names[name]=b.add_frames(name,[(0,0,np.asarray(image))]);metrics.append(image.size)
    blobs,ims=b.finish();out=bytearray(64)
    def append(data,align=64):
        out.extend(b'\0'*(-len(out)%align));o=len(out);out.extend(data);return o
    imrows=[];frrows=[];pcrows=[];pal=bytearray()
    for im,(w,h) in zip(ims,metrics):
        pal.extend(b'\0'*(-len(pal)%64));po=len(pal);pal.extend(im['pal'])
        imrows.append(struct.pack('<HHhhhhhhHBBII',w,h,0,0,0,0,w-1,h-1,len(im['frames']),im['bpp'],0,len(frrows),po))
        for fr in im['frames']:
            frrows.append(struct.pack('<IHH',len(pcrows),len(fr),0))
            pcrows.extend(struct.pack('<HBBHHhh',*pc) for pc in fr)
    pages=[];data=bytearray()
    for bpp,raw in blobs:
        data.extend(b'\0'*(-len(data)%64));pages.append(struct.pack('<IB3x',len(data),bpp));data.extend(raw)
    offsets=[append(b''.join(imrows)),append(b''.join(frrows)),append(b''.join(pcrows)),append(b''.join(pages)),0,0,append(pal),len(pal),append(data)]
    struct.pack_into('<4s15I',out,0,b'VSPR',len(ims),len(frrows),len(pcrows),len(blobs),0,0,*offsets)
    return bytes(out),names
