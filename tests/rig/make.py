"""Test data for tests/rig: a synthetic rig written by build_rig, a glTF file converted by the command-line tool,
and reference values computed independently here (float64, glTF semantics) for the C test to compare against."""
import base64
import json
import math
import os
import struct
import subprocess
import sys

import numpy as np

V = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
sys.path.insert(0, os.path.join(V, 'tools'))
import vcpe_rig as R  # noqa: E402

out = sys.argv[1]
lines = []


def qaxis(axis, deg):
    a = np.asarray(axis, dtype=np.float64)
    a = a / np.linalg.norm(a)
    h = math.radians(deg) / 2
    return np.array([*(a * math.sin(h)), math.cos(h)])


def emit(kind, *vals):
    lines.append(' '.join([kind] + ['%.9g' % v if isinstance(v, float) else str(v) for v in vals]))


# ------------------------------------------------------------- synthetic rig
bones = [R.Bone('root', -1, R.Xf()),
         R.Bone('hip', 0, R.Xf(t=(0, 1.0, 0))),
         R.Bone('thigh', 1, R.Xf(t=(0.1, 0, 0))),
         R.Bone('calf', 2, R.Xf(t=(0, -0.45, 0))),
         R.Bone('foot', 3, R.Xf(t=(0, -0.45, 0))),
         R.Bone('spine', 1, R.Xf(t=(0, 0.3, 0))),
         R.Bone('head', 5, R.Xf(qaxis((1, 0, 0), 10), (0, 0.4, 0)))]
NB = len(bones)


def pose_keys(fn, frames):
    return [[fn(f, b) for b in range(NB)] for f in range(frames)]


def bind_of(b):
    return R.Xf(bones[b].bind.r, bones[b].bind.t, bones[b].bind.s)


SPIN_F = 31


def spin(f, b):
    u = f / (SPIN_F - 1)
    if b == 0:      # turns a full circle while walking 2 m forward (-Z) and bobbing
        return R.Xf(qaxis((0, 1, 0), 360 * u), (0, 0.05 * math.sin(2 * math.pi * u), -2.0 * u))
    if b == 2:
        return R.Xf(qaxis((1, 0, 0), 30 * math.sin(2 * math.pi * u)), bones[2].bind.t)
    return bind_of(b)


WAVE_F = 21


def wave(f, b):
    u = f / (WAVE_F - 1)
    if b == 5:
        return R.Xf(qaxis((1, 0, 0), 40 * math.sin(math.pi * u)), (0, 0.3, 0))
    if b == 6:
        return R.Xf(bones[6].bind.r, (0, 0.4, 0), 1 + 0.5 * u)          # scale keys
    if b == 1:
        return R.Xf(bones[1].bind.r, (0.2, 1.0, 0))                      # one constant translation
    return bind_of(b)


def lean(f, b):
    if b == 5 and f == 1:
        return R.Xf(qaxis((0, 0, 1), 20), (0, 0.3, 0.1))
    return bind_of(b)


clips = [R.Clip('spin', 1.0, pose_keys(spin, SPIN_F), R.LOOP | R.MOTION_XZ | R.MOTION_YAW),
         R.Clip('wave', 2.0, pose_keys(wave, WAVE_F), 0, [(0.0, 'start', 1), (1.0, 'mid', 2), (2.0, 'end', 3)]),
         R.Clip('still', 0.5, pose_keys(lambda f, b: bind_of(b) if b != 3 else R.Xf(qaxis((1, 0, 0), 45), (0, -0.45, 0)), 1)),
         R.Clip('lean', 1.0, pose_keys(lean, 2), R.ADDITIVE)]
blob = R.build_rig(bones, clips, motion_bone=0)
open(os.path.join(out, 'synth.vrg'), 'wb').write(blob)


def ref_sample(clip, t, loop):
    """what vcpe_rig_sample must return, from the unquantised keys"""
    keys, d, F = clip.keys, clip.duration, len(clip.keys)
    if loop:
        t = math.fmod(t, d)
        t = t + d if t < 0 else t
    else:
        t = min(max(t, 0), d)
    fl = t * (F - 1) / d
    a = min(int(fl), F - 1)
    b = min(a + 1, F - 1)
    k = fl - a if a < F - 1 else 0.0
    res = []
    for i in range(NB):
        x, y = keys[a][i], keys[b][i]
        res.append(R.Xf(R.nlerp(x.r, y.r, k), x.t + (y.t - x.t) * k, x.s + (y.s - x.s) * k))
    if clip.flags & (R.MOTION_XZ | R.MOTION_YAW):
        first = keys[0][0]
        m = res[0]
        t0 = m.t.copy()
        t0[0], t0[2] = first.t[0], first.t[2]
        yaw = lambda q: 2 * math.atan2(q[1], q[3])
        dy = (yaw(m.r) - yaw(first.r) + math.pi) % (2 * math.pi) - math.pi
        res[0] = R.Xf(R.qmul(qaxis((0, 1, 0), -math.degrees(dy)), m.r), t0, m.s)
    return res


def models(local):
    mo = []
    for i, b in enumerate(bones):
        mo.append(local[i] if b.parent < 0 else mo[b.parent].then(local[i]))
    return mo


for ci, (clip, times) in enumerate(zip(clips, [(0, 0.1, 0.37, 0.999, 1.25, -0.2), (0, 0.5, 1.33, 2.0, 2.5),
                                              (0, 0.3), (0.5,)])):
    loop = 1 if clip.flags & R.LOOP else 0
    for t in times:
        p = ref_sample(clip, t, loop)
        mo = models(p)
        for b in range(NB):
            emit('pose', 'synth.vrg', ci, float(t), loop, b, *map(float, p[b].r), *map(float, p[b].t), float(p[b].s))
            emit('model', 'synth.vrg', ci, float(t), loop, b, *map(float, mo[b].t))

# root motion over a full cycle of "spin": 2 m forward in the frame at the start, a full turn
emit('motion', 'synth.vrg', 0, 0.0, 1.0, 1, 0.0, 0.0, -2.0, 2 * math.pi)

# ------------------------------------------------------------- glTF file through the command line
arm_xf = R.Xf(qaxis((0, 1, 0), 90), (3, 0, 0), 0.5)
root_rest = R.Xf(t=(0, 1, 0))
arm_rest = R.Xf(t=(0, 1, 0))
w_root = arm_xf.then(root_rest)
w_arm = w_root.then(arm_rest)


def m4(xf):
    m = np.eye(4)
    m[:3, :] = xf.matrix()
    return m


ibm_root = np.linalg.inv(m4(w_root))
ibm_arm = np.linalg.inv(m4(w_arm))
pos = np.array([[0, 1, 0], [1, 1.5, 0], [0, 2.5, 0], [-1, 2, 0]], dtype=np.float32) * 0.5 + np.array([3, 0, 0], np.float32)
uv = np.array([[0, 0], [1, 0], [1, 1], [0, 1]], dtype=np.float32)
col = np.array([[1, 0, 0, 1], [0, 1, 0, 1], [0, 0, 1, 1], [1, 1, 1, 0.5]], dtype=np.float32)
jts = np.array([[1, 0, 0, 0], [1, 0, 0, 0], [0, 1, 0, 0], [0, 1, 0, 0]], dtype=np.uint8)   # skin joint order: arm, root
wts = np.array([[1, 0, 0, 0], [0.5, 0.5, 0, 0], [1, 0, 0, 0], [0.25, 0.75, 0, 0]], dtype=np.float32)
idx = np.array([0, 1, 2, 0, 2, 3], dtype=np.uint16)
t_in = np.array([0, 0.5, 1.0], dtype=np.float32)
rot_out = np.array([[0, 0, 0, 1], qaxis((0, 0, 1), 90), [0, 0, 0, 1]], dtype=np.float32)
tr_in = np.array([0, 0.5], dtype=np.float32)
tr_out = np.array([[0, 1, 0], [0, 1.5, 0]], dtype=np.float32)
ibms = np.stack([ibm_arm.T.reshape(16), ibm_root.T.reshape(16)]).astype(np.float32)
parts = [pos, uv, col, jts, wts, idx, t_in, rot_out, tr_in, tr_out, ibms]
buf, views = b'', []
for p in parts:
    buf += b'\0' * (-len(buf) % 4)
    views.append((len(buf), p.nbytes))
    buf += p.tobytes()
acc = [(5126, 'VEC3', 4), (5126, 'VEC2', 4), (5126, 'VEC4', 4), (5121, 'VEC4', 4), (5126, 'VEC4', 4),
       (5123, 'SCALAR', 6), (5126, 'SCALAR', 3), (5126, 'VEC4', 3), (5126, 'SCALAR', 2), (5126, 'VEC3', 2),
       (5126, 'MAT4', 2)]
gl = {
    'asset': {'version': '2.0'},
    'scene': 0, 'scenes': [{'nodes': [0, 3]}],
    'nodes': [
        {'name': 'Armature', 'rotation': list(map(float, arm_xf.r)), 'translation': [3, 0, 0], 'scale': [0.5] * 3,
         'children': [1]},
        {'name': 'root', 'translation': [0, 1, 0], 'children': [2]},
        {'name': 'arm', 'translation': [0, 1, 0]},
        {'name': 'body', 'mesh': 0, 'skin': 0}],
    'skins': [{'joints': [2, 1], 'inverseBindMatrices': 10}],
    'meshes': [{'primitives': [{'attributes': {'POSITION': 0, 'TEXCOORD_0': 1, 'COLOR_0': 2, 'JOINTS_0': 3,
                                               'WEIGHTS_0': 4}, 'indices': 5, 'material': 0}]}],
    'materials': [{'name': 'skin', 'pbrMetallicRoughness': {'baseColorFactor': [1, 1, 1, 1]}}],
    'animations': [{'name': 'Wave', 'samplers': [{'input': 6, 'output': 7, 'interpolation': 'LINEAR'},
                                                 {'input': 8, 'output': 9, 'interpolation': 'STEP'}],
                    'channels': [{'sampler': 0, 'target': {'node': 2, 'path': 'rotation'}},
                                 {'sampler': 1, 'target': {'node': 1, 'path': 'translation'}}]}],
    'buffers': [{'byteLength': len(buf), 'uri': 'data:application/octet-stream;base64,' + base64.b64encode(buf).decode()}],
    'bufferViews': [{'buffer': 0, 'byteOffset': o, 'byteLength': n} for o, n in views],
    'accessors': [{'bufferView': i, 'componentType': c, 'type': ty, 'count': n} for i, (c, ty, n) in enumerate(acc)],
}
gl['accessors'][3]['componentType'] = 5121
json.dump(gl, open(os.path.join(out, 'model.gltf'), 'w'))
ev = {'Wave': [{'t': 0.5, 'name': 'peak', 'arg': 7}]}
json.dump(ev, open(os.path.join(out, 'events.json'), 'w'))
subprocess.check_call([sys.executable, os.path.join(V, 'tools', 'vcpe_rig.py'), os.path.join(out, 'model.gltf'),
                       os.path.join(out, 'model.vrg'), '--mesh', os.path.join(out, 'model.vsm'), '--loop', 'Wa*',
                       '--events', os.path.join(out, 'events.json'), '--page', 'skin=5'], stdout=subprocess.DEVNULL)


def gltf_at(t):
    """skinned vertex positions at t straight from the glTF data (slerp, STEP), no resampling"""
    k = t / 0.5
    if k <= 1:
        r = R.slerp(np.array([0, 0, 0, 1.0]), qaxis((0, 0, 1), 90), k)
    else:
        r = R.slerp(qaxis((0, 0, 1), 90), np.array([0, 0, 0, 1.0]), k - 1)
    root = R.Xf(t=(0, 1, 0) if t < 0.5 else (0, 1.5, 0))
    wr = arm_xf.then(root)
    wa = wr.then(R.Xf(r, (0, 1, 0)))
    mats = {0: m4(wa) @ ibm_arm, 1: m4(wr) @ ibm_root}      # skin joint order
    res = []
    for v in range(4):
        p = np.array([*pos[v], 1.0])
        q = sum(wts[v][k] * (mats[jts[v][k]] @ p) for k in range(4) if wts[v][k] > 0)
        res.append(q[:3])
    return res, wr, wa


for t in (0.0, 0.25, 0.75):
    vs, wr, wa = gltf_at(t)
    for v in range(4):
        emit('skinv', 'model.vrg', 'model.vsm', 0, float(t), v, *map(float, vs[v]))
    emit('model', 'model.vrg', 0, float(t), 1, 0, *map(float, wr.t))      # bone 0 = root after reordering
    emit('model', 'model.vrg', 0, float(t), 1, 1, *map(float, wa.t))
open(os.path.join(out, 'expect.txt'), 'w').write('\n'.join(lines) + '\n')

# ------------------------------------------------------------- a bending tube with LOD levels (build_mesh directly)
tverts, tskin, tidx = [], [], []
RINGS, SEGS = 12, 16
for i in range(RINGS + 1):
    y = i / RINGS
    for j in range(SEGS + 1):
        a = 2 * math.pi * j / SEGS
        rr = 0.2 + 0.1 * math.sin(math.pi * y)          # a bulge, so joining rings has a measurable error
        p = (rr * math.cos(a), y, rr * math.sin(a))
        tverts.append((j / SEGS, y, 0xffffffff, *p))
        w = min(1.0, max(0.0, (y - 0.3) / 0.4))
        tskin.append((p, [0, 1, 0, 0], [1 - w, w, 0, 0]))
for i in range(RINGS):
    for j in range(SEGS):
        a, b = i * (SEGS + 1) + j, i * (SEGS + 1) + j + 1
        c, d = a + SEGS + 1, b + SEGS + 1
        tidx += [a, c, b, b, c, d]
half = len(tidx) // 2 // 3 * 3
mesh, info = R.build_mesh(tverts, tskin, [(1, tidx[:half]), (2, tidx[half:])], lod=3)
open(os.path.join(out, 'tube.vsm'), 'wb').write(mesh)
emit('tube', len(info), *[x for t, v, e in info for x in (t, v)])
open(os.path.join(out, 'expect.txt'), 'w').write('\n'.join(lines) + '\n')
