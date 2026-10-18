#!/usr/bin/env python3
"""Reversible, offline upgrades for VCPE demakes. Start with a read-only plan.

  python3 tools/vcpe_upgrade.py plan /path/to/demake
  python3 tools/vcpe_upgrade.py apply /path/to/demake [--savedata]
  python3 tools/vcpe_upgrade.py status /path/to/demake
  python3 tools/vcpe_upgrade.py rollback /path/to/demake

Only the project's Makefile and .vcpe/ are written. Each engine is a private pinned
snapshot: upgrading one demake never changes another. No downloads or builds run.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
START = '# BEGIN VCPE EASY UPGRADE'
END = '# END VCPE EASY UPGRADE'
BASE = 'vcpe_sys vcpe_pak vcpe_gfx vcpe_test vcpe_hud vcpe_math vcpe_mem vcpe_load vcpe_log'.split()
DEPS = {'vcpe_savedata': ['vcpe_save'], 'vcpe_prefetch': ['vcpe_pack', 'vcpe_jobs'],
        'vcpe_pack': ['vcpe_jobs'], 'vcpe_node3d': ['vcpe_3d'], 'vcpe_3d': ['vcpe_lod', 'vcpe_cull'],
        'vcpe_stream': ['vcpe_audio'], 'vcpe_spr': ['vcpe_math'],
        'vcpe_perspective': ['vcpe_math'], 'vcpe_raycast': ['vcpe_math'],
        'vcpe_perspective_draw': ['vcpe_perspective', 'vcpe_gfx'], 'vcpe_skin': ['vcpe_math'],
        'vcpe_path': ['vcpe_math'], 'vcpe_vehicle': ['vcpe_path', 'vcpe_math'], 'vcpe_backdrop': ['vcpe_3d'],
        'vcpe_canvas': ['vcpe_spr', 'vcpe_gfx'],
        'vcpe_rig': ['vcpe_skin'], 'vcpe_animator': ['vcpe_rig'],
        'vcpe_terrain_draw': ['vcpe_terrain', 'vcpe_3d'], 'vcpe_sky_draw': ['vcpe_sky', 'vcpe_3d']}

def atomic(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(dir=path.parent, prefix='.upgrade-')
    try:
        with os.fdopen(fd, 'wb') as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.replace(name, path)
    finally:
        if os.path.exists(name): os.unlink(name)

def files(source):
    result = []
    for folder in ('src', 'runtime', 'cook', 'rpg', 'tools', 'docs', 'engineassets'):
        for p in sorted((source / folder).rglob('*')):
            if p.is_file() and '__pycache__' not in p.parts and p.suffix not in ('.o', '.d', '.pyc'):
                if p.is_symlink(): raise ValueError(f'engine symlink is not supported: {p}')
                result.append(p)
    for name in ('README.md', 'CHANGELOG.md'):
        if (source / name).is_file(): result.append(source / name)
    return result

def inspect(project, source, savedata):
    project = project.resolve(); source = source.resolve()
    make = project / 'port/Makefile'
    if not make.is_file(): make = project / 'Makefile'
    if not make.is_file(): raise ValueError('expected port/Makefile or Makefile')
    if make.is_symlink() or (project / '.vcpe').is_symlink(): raise ValueError('refusing symlinked build state')
    if any(c.isspace() or c in '#$:%' for c in str(project)):
        raise ValueError('PSP Makefile paths must not contain whitespace or # $ : %')
    text = make.read_text()
    clean = re.sub(r'\n?' + re.escape(START) + r'.*?' + re.escape(END) + r'\n?', '', text, flags=re.S)
    matches = list(re.finditer(r'^VCPE_MODULES[ \t]*[:?+]?=(?:[^\n]*\\\n)*[^\n]*', clean, re.M))
    if not matches or not re.search(r'^VCPE\s*[:?+]?=', clean, re.M):
        raise ValueError('unsupported Makefile: requires VCPE and VCPE_MODULES declarations')
    pos = matches[-1].end()
    if '$(VCPE)' in clean[:matches[0].start()] and re.search(r'^OBJS.*\$\(VCPE', clean[:pos], re.M):
        raise ValueError('VCPE objects are evaluated before VCPE_MODULES; move that declaration first')
    selected = set(re.findall(r'\bvcpe_[a-z0-9_]+\b', '\n'.join(m[0] for m in matches))) | set(BASE)
    if savedata: selected.add('vcpe_savedata')
    while True:
        expanded = selected | {d for m in selected for d in DEPS.get(m, [])}
        if expanded == selected: break
        selected = expanded
    missing = [m for m in selected if not (source / 'src' / (m + '.c')).is_file()]
    if missing: raise ValueError('engine lacks modules: ' + ', '.join(sorted(missing)))
    version_header = source / 'src/vcpe_version.h'
    if not version_header.exists(): version_header = source / 'src/vcpe.h'
    version = re.search(r'#define VCPE_VERSION "([\d.]+)"', version_header.read_text()).group(1)
    manifest = {str(p.relative_to(source)): hashlib.sha256(p.read_bytes()).hexdigest() for p in files(source)}
    digest = hashlib.sha256(json.dumps(manifest, sort_keys=True).encode()).hexdigest()
    return project, make, clean, pos, sorted(selected), version, manifest, digest

def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('action', choices=['plan', 'apply', 'status', 'rollback'])
    parser.add_argument('project', type=Path)
    parser.add_argument('--engine', type=Path, default=ROOT)
    parser.add_argument('--savedata', action='store_true', help='also link native PSP savedata and its utility library')
    args = parser.parse_args()
    project = args.project.resolve(); state = project / '.vcpe'
    active = state / 'active.json'
    if args.action in ('status', 'rollback'):
        if not active.exists(): raise ValueError('no active easy upgrade')
        record = json.loads(active.read_text())
        if args.action == 'status':
            print(json.dumps(record, indent=2)); return
        make = project / record['makefile']; backup = state / 'backups' / record['backup']
        if hashlib.sha256(make.read_bytes()).hexdigest() != record['make_sha256']:
            raise ValueError('Makefile changed since upgrade; preserve your edits and review the backup before rollback')
        atomic(make, (backup / 'Makefile').read_bytes())
        previous = backup / 'active.json'
        if previous.exists(): atomic(active, previous.read_bytes())
        else: active.unlink()
        # Rebuild game and engine objects with the restored headers. Preserve caches in the backup.
        for cache in project.rglob('*.o'):
            if '.vcpe' in cache.relative_to(project).parts or '.git' in cache.parts: continue
            target = backup / 'objects' / cache.relative_to(project)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.move(str(cache), str(target))
        print(f'Rolled back {make}. Pinned snapshots and backups retained in {state}.'); return
    project, make, clean, pos, modules, version, manifest, digest = inspect(project, args.engine, args.savedata)
    print(f'VCPE {version} -> {project}\nModules: {" ".join(modules)}')
    print('Pins an engine snapshot, adds module/header dependencies, preserves game code and save files.')
    print('After building: compare gameplay recordings and adopt vcpe_loading_begin/end at content switches.')
    if args.action == 'plan': return
    key = version + '-' + digest[:12]
    snapshot = state / 'engines' / key
    snapshot.parent.mkdir(parents=True, exist_ok=True)
    if not snapshot.exists():
        stage = Path(tempfile.mkdtemp(dir=snapshot.parent, prefix='.staging-'))
        try:
            for rel, sha in manifest.items():
                src = args.engine / rel; dst = stage / rel
                dst.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(src, dst)
                if hashlib.sha256(dst.read_bytes()).hexdigest() != sha: raise ValueError('engine changed during snapshot; retry')
            (stage / '.vcpe-version').write_text(version + '\n' + digest + '\n')
            (stage / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
            stage.rename(snapshot)
        finally:
            if stage.exists(): shutil.rmtree(stage)
    else:
        for rel, sha in manifest.items():
            if hashlib.sha256((snapshot / rel).read_bytes()).hexdigest() != sha:
                raise ValueError(f'pinned snapshot was modified: {rel}')
    relative = os.path.relpath(state, make.parent)
    # Inline fragments avoid mutable shared config: a rollback only needs the original Makefile.
    setup = (f'\n{START}\n'
             f'override VCPE := $(abspath {relative}/engines/{key})\n'
             f'VCPE_MODULES := $(sort $(VCPE_MODULES) {" ".join(modules)})\n{END}\n')
    tail = (f'\n{START}\n'
            f'$(filter %.o,$(OBJS)): $(VCPE)/.vcpe-version $(wildcard $(VCPE)/src/*.h $(VCPE)/runtime/*.h)\n'
            'CFLAGS += -MMD -MP\n-include $(OBJS:.o=.d)\n')
    if 'vcpe_savedata' in modules: tail += 'LIBS += -lpsputility\n'
    tail += END + '\n'
    patched = (clean[:pos] + setup + clean[pos:] + tail).encode()
    if active.exists():
        current = json.loads(active.read_text())
        if current['digest'] == digest and make.read_bytes() == patched:
            print('Already pinned to this engine; nothing changed.'); return
    backup_id = time.strftime('%Y%m%d-%H%M%S') + '-' + str(time.time_ns())[-9:]
    backup = state / 'backups' / backup_id; backup.mkdir(parents=True)
    shutil.copy2(make, backup / 'Makefile')
    if active.exists(): shutil.copy2(active, backup / 'active.json')
    record = dict(version=version, digest=digest, snapshot=str(snapshot.relative_to(project)),
                  makefile=str(make.relative_to(project)), make_sha256=hashlib.sha256(patched).hexdigest(),
                  backup=backup_id, modules=modules)
    try:
        atomic(make, patched)
        atomic(active, (json.dumps(record, indent=2) + '\n').encode())
    except BaseException:
        atomic(make, (backup / 'Makefile').read_bytes())
        raise
    print(f'Applied. Backup: {backup}\nBuild with the existing build.sh. Rollback: {Path(__file__)} rollback {project}')

if __name__ == '__main__':
    try: main()
    except (ValueError, OSError) as e: raise SystemExit(f'Upgrade stopped: {e}')
