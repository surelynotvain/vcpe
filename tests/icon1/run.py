#!/usr/bin/env python3
"""Round trips and per-AU timestamps across small, split and multi-GOP PES."""
import importlib.util,random,struct
from pathlib import Path
root=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('pmf',root/'tools/mkicon1.py')
pmf=importlib.util.module_from_spec(spec);spec.loader.exec_module(pmf)
def stamp(b):
    return ((b[0]&14)<<29)|(b[1]<<22)|((b[2]&254)<<14)|(b[3]<<7)|(b[4]>>1)
rng=random.Random(19052)
for count in (1,2,30,270):
    sizes=[rng.choice((1,7,23,1900,2048,6000)) for _ in range(count)]
    aus=[(bytes([i%251+1])*size,i%25==0) for i,size in enumerate(sizes)]
    result,_=pmf.mux(aus)
    assert pmf.demux(result)==b''.join(a for a,_ in aus)
    assert len(result)%2048==0
    starts=[];total=0
    for size in sizes:starts.append(total);total+=size
    seen=[];offset=0
    for pack in range(2048,len(result),2048):
        q=pack+14
        while q<pack+2048:
            sid=result[q+3];length=struct.unpack_from('>H',result,q+4)[0]
            if sid==0xe0:
                hlen=result[q+8]
                if result[q+7]&0xc0:
                    i=starts.index(offset)
                    pts=stamp(result[q+9:q+14]);dts=stamp(result[q+14:q+19])
                    assert pts==90000+3003*i and dts==pts-3003
                    seen.append(i)
                offset+=length-3-hlen
            q+=6+length
        assert q==pack+2048
    assert seen==list(range(count)),seen
print('PASS PMF round trips, pack boundaries and one exact PTS/DTS per AU (1/2/30/270 frames)')
