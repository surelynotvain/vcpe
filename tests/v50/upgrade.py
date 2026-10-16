#!/usr/bin/env python3
"""Exercise real Makefile evaluation and upgrade recovery in disposable projects."""
from pathlib import Path
import hashlib
import json
import subprocess
import tempfile
import shutil

ENGINE = Path(__file__).resolve().parents[2]
TOOL = ENGINE / 'tools/vcpe_upgrade.py'

def run(action, project, *args, ok=True):
    p = subprocess.run(['python3', str(TOOL), action, str(project), *args], capture_output=True, text=True)
    assert (p.returncode == 0) == ok, (p.stdout, p.stderr)
    return p.stdout

with tempfile.TemporaryDirectory(prefix='vcpe-upgrade-test-') as temp:
    project = Path(temp); port = project/'port'; port.mkdir()
    make = port/'Makefile'
    original = b'''VCPE ?= /old/engine
VCPE_MODULES := vcpe_pad \\
 vcpe_prefetch
OBJS := src/main.o $(addprefix vcpe_obj/,$(addsuffix .o,$(VCPE_MODULES)))
.DEFAULT_GOAL := show
show:
\t@echo $(VCPE)
\t@echo $(VCPE_MODULES)
\t@echo $(LIBS)
'''
    make.write_bytes(original)
    (port/'src').mkdir(); (port/'src/main.c').write_text('int main(void){return 0;}')
    (port/'src/main.o').write_bytes(b'cache')
    run('plan', project, '--savedata'); assert not (project/'.vcpe').exists()
    run('apply', project, '--savedata')
    first = make.read_bytes(); state = json.loads((project/'.vcpe/active.json').read_text())
    snapshot = project/state['snapshot']; assert (snapshot/'src/vcpe_savedata.c').is_file()
    process = subprocess.run(['make','-s','show'],cwd=port,capture_output=True,text=True)
    assert process.returncode == 0, (process.stdout, process.stderr, make.read_text())
    result = process.stdout
    assert str(snapshot) in result and 'vcpe_load' in result and 'vcpe_pack' in result and '-lpsputility' in result
    assert 'nothing changed' in run('apply', project, '--savedata')
    assert make.read_bytes() == first
    make.write_bytes(first+b'# new local change\n')
    run('rollback',project,ok=False); assert make.read_bytes().endswith(b'# new local change\n')
    make.write_bytes(first)
    run('rollback',project); assert make.read_bytes() == original
    assert not (port/'src/main.o').exists()
    assert list((project/'.vcpe/backups').rglob('main.o'))[0].read_bytes() == b'cache'
    run('apply',project,'--savedata')
    pinned = snapshot/'src/vcpe_version.h'; pinned.write_text('tampered')
    run('apply',project,'--savedata',ok=False)
    print('PASS easy upgrade: plan, multiline modules/dependencies, Make evaluation, idempotence, edit protection, exact rollback, cache preservation, snapshot tamper detection')
