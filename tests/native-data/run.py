#!/usr/bin/env python3
"""Host contract for immutable data, including malformed/truncated buffers."""
from pathlib import Path
import sys,tempfile,subprocess
root=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root/'tools'))
from vcpe_data import pack
with tempfile.TemporaryDirectory(prefix='vcpe-data-') as tmp:
    tmp=Path(tmp);fixture=tmp/'fixture.bin'
    fixture.write_bytes(pack({'array':[0,1.25,None],'negative':-123,'unicode':'A あ','blob':b'\0\1\2'}))
    subprocess.run(['cc','-std=c99','-O2','-Wall','-Wextra','-I'+str(root/'src'),str(Path(__file__).with_name('test.c')),str(root/'src/vcpe_data.c'),'-o',str(tmp/'test')],check=True)
    subprocess.run([str(tmp/'test'),str(fixture)],check=True)
