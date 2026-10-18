"""VCPE rigs and skinned meshes (src/vcpe_rig.c): skeletons, compressed animation clips and skinned meshes from glTF 2.0.

    from vcpe_rig import build_rig, read_gltf, convert
    rig_blob, mesh_blob, report = convert('npc.glb', fps=30, loop=['*walk*', '*idle*'], motion=['*walk*'])
    # or from your own data: build_rig(bones, clips, motion_bone=0) -> VRG1 bytes (see Bone / Clip below)

Every glTF exporter (Blender, Maya, 3ds Max, Godot, NifSkope through Blender...) writes what this reads: one skin
(its joints become the bones), the animations that move those joints (each becomes a clip, resampled at --fps), and
the meshes skinned by it (one VSM1 mesh, one submesh per material). Character space is the glTF scene space (Y up,
forward -Z); non-joint parents of the skeleton (an armature object, an export scale) are baked into the root bones.

Rig blob "VRG1" (little endian, tables 16-byte aligned):
    header   "VRG1", u16 bones, u16 clips, u32 total size, u32 bone table offset, u32 clip table offset,
             i16 motion bone (-1 none), u16 0, 8 bytes 0                                                   (32 bytes)
    bone     u32 name hash (FNV-1a), i16 parent (-1, always before the bone), u16 0, float t[3], r[4] (x y z w), s,
             float inverse bind [12] (3x4 row-major, character space)                                      (88 bytes)
    clip     u32 name hash, u16 frames, u16 flags (1 loop, 2 root motion XZ, 4 root motion yaw, 8 additive),
             float duration, u32 track table offset, u32 event offset, u16 events, u16 0, 8 bytes 0         (32 bytes)
    track    per bone: u8 rotation, translation, scale mode (0 bind pose, 1 one value, 2 a key per frame), u8 0,
             u32 data offset (4-aligned). Data: rotation keys, translation, scale, each part padded to 4 bytes:
               rotation     6 bytes a key: smallest-three quaternion, 15 bits per kept component
                            ((c + 1/sqrt2) / sqrt2 x 32766); bit 15 of words 0 and 1 = index of the dropped one
               translation  one value: float[3]; keys: float min[3], float step[3], then u16[3] a key
               scale        float a key
    event    float time, u32 name hash, i32 argument (12 bytes, sorted by time)

Mesh blob "VSM1": header {"VSM1", u16 vertices, u16 submeshes, u16 levels, u16 bones used, u32 total size,
u32 vertex offset, u32 bind offset, u32 submesh offset, u32 level offset, float centre[3], float radius} (48 bytes),
levels {u16 vertices used, u16 0, float error} (8 bytes each, level 0 = full), submeshes {i16 page, u16 0,
u32 index offset[4], u32 index count[4]} (36 bytes each), VcpeVtx[vertices] (float u, v, u32 ABGR, float x, y, z: the
colour/UV template the skinning output starts from), VcpeSkinVertex[vertices] (float xyz[3], u16 bone[4], float
weight[4]: 36 bytes), then the u16 index lists, each 16-byte aligned. With --lod, vertices are ordered so that every
level uses a prefix of them (tools/vcpe_lod.py): skin only `vertices used` of the level drawn.

Command line:
  vcpe_rig.py IN.gltf|IN.glb OUT.vrg [--mesh OUT.vsm] [--fps 30] [--skin N] [--clips PATTERN]
              [--loop PATTERN]... [--motion PATTERN]... [--additive PATTERN]... [--motion-bone NAME]
              [--events EVENTS.json] [--lod LEVELS] [--page MATERIAL=N]..."""
import base64
import fnmatch
import json
import math
import os
import struct
import sys

import numpy as np

MAX_BONES = 192
LOOP, MOTION_XZ, MOTION_YAW, ADDITIVE = 1, 2, 4, 8
BIND, CONST, KEYS = 0, 1, 2
MESH_LOD = 4
ROT_TOL = 1e-9        # 1 - |dot| below this: the same rotation (about 4.5e-5 rad)
POS_TOL = 1e-5        # metres
SCL_TOL = 1e-5


def fnv1a(s):
    h = 2166136261
    for c in s.encode('utf-8'):
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return h


def _pad(b, n=16):
    return b + b'\0' * (-len(b) % n)


# ---------------------------------------------------------------- quaternions (x, y, z, w), float64
def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return np.array([aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
                     aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz])


def qrot(q, v):
    u, w = np.asarray(q[:3]), q[3]
    c = np.cross(u, v)
    return np.asarray(v) + 2 * (w * c + np.cross(u, c))


def qnorm(q):
    q = np.asarray(q, dtype=np.float64)
    n = np.linalg.norm(q)
    return np.array([0, 0, 0, 1.0]) if n < 1e-20 else q / n


def qconj(q):
    return np.array([-q[0], -q[1], -q[2], q[3]])


def qmat(q, s=1.0):
    x, y, z, w = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
                     [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
                     [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)]]) * s


def mat_quat(m):
    """rotation matrix (orthonormal 3x3) -> unit quaternion"""
    t = m[0, 0] + m[1, 1] + m[2, 2]
    if t > 0:
        s = math.sqrt(t + 1) * 2
        q = [(m[2, 1] - m[1, 2]) / s, (m[0, 2] - m[2, 0]) / s, (m[1, 0] - m[0, 1]) / s, s / 4]
    elif m[0, 0] > m[1, 1] and m[0, 0] > m[2, 2]:
        s = math.sqrt(1 + m[0, 0] - m[1, 1] - m[2, 2]) * 2
        q = [s / 4, (m[0, 1] + m[1, 0]) / s, (m[0, 2] + m[2, 0]) / s, (m[2, 1] - m[1, 2]) / s]
    elif m[1, 1] > m[2, 2]:
        s = math.sqrt(1 + m[1, 1] - m[0, 0] - m[2, 2]) * 2
        q = [(m[0, 1] + m[1, 0]) / s, s / 4, (m[1, 2] + m[2, 1]) / s, (m[0, 2] - m[2, 0]) / s]
    else:
        s = math.sqrt(1 + m[2, 2] - m[0, 0] - m[1, 1]) * 2
        q = [(m[0, 2] + m[2, 0]) / s, (m[1, 2] + m[2, 1]) / s, s / 4, (m[1, 0] - m[0, 1]) / s]
    return qnorm(q)


def nlerp(a, b, t):
    if np.dot(a, b) < 0:
        b = -b
    return qnorm(a * (1 - t) + b * t)


def slerp(a, b, t):
    d = float(np.dot(a, b))
    if d < 0:
        b, d = -b, -d
    if d > 0.9995:
        return nlerp(a, b, t)
    th = math.acos(min(1.0, d))
    return qnorm((math.sin((1 - t) * th) * a + math.sin(t * th) * b) / math.sin(th))


class Xf:
    """rotation quaternion, translation, uniform scale"""
    __slots__ = ('r', 't', 's')

    def __init__(self, r=(0, 0, 0, 1), t=(0, 0, 0), s=1.0):
        self.r, self.t, self.s = qnorm(r), np.asarray(t, dtype=np.float64), float(s)

    def then(self, child):
        """self as the parent of child"""
        return Xf(qmul(self.r, child.r), self.t + qrot(self.r, child.t * self.s), self.s * child.s)

    def matrix(self):
        m = np.zeros((3, 4))
        m[:, :3] = qmat(self.r, self.s)
        m[:, 3] = self.t
        return m


def affine_inverse(m):
    a = np.linalg.inv(m[:, :3])
    out = np.zeros((3, 4))
    out[:, :3] = a
    out[:, 3] = -a @ m[:, 3]
    return out


def decompose(m):
    """3x4 or 4x4 -> Xf (uniform scale: the mean column length; a warning-worthy residual is returned too)"""
    a = np.asarray(m, dtype=np.float64)[:3, :3]
    cols = np.linalg.norm(a, axis=0)
    s = float(cols.mean())
    r = a / cols
    if np.linalg.det(r) < 0:          # a mirror cannot be a rotation: drop it (the caller reports the skew)
        r[:, 0] = -r[:, 0]
    skew = float(np.abs(cols - s).max() / max(s, 1e-12))
    return Xf(mat_quat(r), np.asarray(m, dtype=np.float64)[:3, 3], s), skew


# ---------------------------------------------------------------- encoding
def enc_quat(q):
    q = qnorm(q)
    i = int(np.argmax(np.abs(q)))
    if q[i] < 0:
        q = -q
    rest = [q[k] for k in range(4) if k != i]
    v = [max(0, min(32766, int(round((c + 0.7071067811865476) / 1.4142135623730951 * 32766)))) for c in rest]
    return struct.pack('<3H', v[0] | (i >> 1) << 15, v[1] | (i & 1) << 15, v[2])


def dec_quat(b):
    """the runtime's decoder (src/vcpe_rig.c key_rot), for tests and error reports"""
    a, bb, c = struct.unpack('<3H', b)
    i = (a >> 15) << 1 | (bb >> 15)
    k, o = 1.4142135623730951 / 32766, 0.7071067811865476
    v = [(a & 0x7fff) * k - o, (bb & 0x7fff) * k - o, (c & 0x7fff) * k - o]
    w = math.sqrt(max(0.0, 1 - sum(x * x for x in v)))
    out, j = [], 0
    for n in range(4):
        if n == i:
            out.append(w)
        else:
            out.append(v[j])
            j += 1
    return np.array(out)


def _same_rot(keys, ref):
    return all(1 - abs(float(np.dot(k, ref))) < ROT_TOL for k in keys)


def _track(rots, poss, scls, bind):
    """-> (modes, data bytes) for one bone of one clip"""
    modes, data = [], b''
    if _same_rot(rots, bind.r):
        modes.append(BIND)
    elif _same_rot(rots, rots[0]):
        modes.append(CONST)
        data += _pad(enc_quat(rots[0]), 4)
    else:
        modes.append(KEYS)
        data += _pad(b''.join(enc_quat(q) for q in rots), 4)
    P = np.asarray(poss)
    if np.abs(P - bind.t).max() < POS_TOL:
        modes.append(BIND)
    elif np.abs(P - P[0]).max() < POS_TOL:
        modes.append(CONST)
        data += struct.pack('<3f', *P[0])
    else:
        modes.append(KEYS)
        lo, hi = P.min(0), P.max(0)
        step = (hi - lo) / 65535.0
        q = np.where(step > 0, np.round((P - lo) / np.where(step > 0, step, 1)), 0).clip(0, 65535).astype(np.uint16)
        data += _pad(struct.pack('<6f', *lo, *step) + q.astype('<u2').tobytes(), 4)
    S = np.asarray(scls, dtype=np.float64)
    if np.abs(S - bind.s).max() < SCL_TOL:
        modes.append(BIND)
    elif np.abs(S - S[0]).max() < SCL_TOL:
        modes.append(CONST)
        data += struct.pack('<f', S[0])
    else:
        modes.append(KEYS)
        data += S.astype('<f4').tobytes()
    return modes, data


class Bone:
    def __init__(self, name, parent, bind, inv_bind=None):
        self.name, self.parent, self.bind = name, parent, bind   # bind: Xf (local)
        self.inv_bind = inv_bind                                 # 3x4 or None (computed from the bind pose)


class Clip:
    """keys[f][b] = Xf (local) for every frame f and bone b; events: [(time, name, arg)]"""
    def __init__(self, name, duration, keys, flags=0, events=()):
        self.name, self.duration, self.keys, self.flags, self.events = name, float(duration), keys, flags, list(events)


def bind_models(bones):
    model = []
    for b in bones:
        model.append(b.bind if b.parent < 0 else model[b.parent].then(b.bind))
    return model


def build_rig(bones, clips, motion_bone=-1):
    """-> VRG1 bytes"""
    nb = len(bones)
    if not 1 <= nb <= MAX_BONES:
        raise ValueError('a rig has 1..%d bones' % MAX_BONES)
    for i, b in enumerate(bones):
        if not -1 <= b.parent < i:
            raise ValueError('bone %s: parents must come first' % b.name)
    model = bind_models(bones)
    bone_off = 32
    clip_off = bone_off + 88 * nb
    clip_off += -clip_off % 16
    btab = b''
    for i, b in enumerate(bones):
        inv = b.inv_bind if b.inv_bind is not None else affine_inverse(model[i].matrix())
        btab += struct.pack('<IhH3f4ff12f', fnv1a(b.name), b.parent, 0, *b.bind.t, *b.bind.r, b.bind.s,
                            *np.asarray(inv, dtype=np.float64).reshape(12))
    data_off = clip_off + 32 * len(clips)
    data_off += -data_off % 16
    ctab, body = b'', b''
    for c in clips:
        frames = len(c.keys)
        if frames < 1 or frames > 65535 or not c.duration > 0:
            raise ValueError('clip %s: needs 1..65535 frames and a positive duration' % c.name)
        tracks_at = data_off + len(body)
        tracks, tdata = b'', b''
        tdata_at = tracks_at + 8 * nb
        tdata_at += -tdata_at % 4
        for i, b in enumerate(bones):
            modes, d = _track([k[i].r for k in c.keys], [k[i].t for k in c.keys], [k[i].s for k in c.keys], b.bind)
            tracks += struct.pack('<4BI', modes[0], modes[1], modes[2], 0, tdata_at + len(tdata) if d else 0)
            tdata += d
        body += tracks + b'\0' * (tdata_at - tracks_at - len(tracks)) + tdata
        body += b'\0' * (-(data_off + len(body)) % 4)
        events = sorted(c.events, key=lambda e: e[0])
        ev_at = data_off + len(body)
        for t, name, arg in events:
            body += struct.pack('<fIi', t, fnv1a(name) if isinstance(name, str) else name, arg)
        body += b'\0' * (-(data_off + len(body)) % 16)
        ctab += struct.pack('<IHHfIIHH2I', fnv1a(c.name), frames, c.flags, c.duration, tracks_at, ev_at,
                            len(events), 0, 0, 0)
    total = data_off + len(body)
    head = struct.pack('<4sHHIIIhH8x', b'VRG1', nb, len(clips), total, bone_off, clip_off, motion_bone, 0)
    blob = head + btab + b'\0' * (clip_off - bone_off - len(btab)) + ctab
    blob += b'\0' * (data_off - len(blob)) + body
    assert len(blob) == total
    return blob


# ---------------------------------------------------------------- glTF
COMP = {5120: np.int8, 5121: np.uint8, 5122: np.int16, 5123: np.uint16, 5125: np.uint32, 5126: np.float32}
NCOMP = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}
NORM = {5120: 127.0, 5121: 255.0, 5122: 32767.0, 5123: 65535.0}


class Gltf:
    def __init__(self, path):
        raw = open(path, 'rb').read()
        self.base = os.path.dirname(os.path.abspath(path))
        self.bin = None
        if raw[:4] == b'glTF':
            _, length = struct.unpack('<II', raw[4:12])
            pos, self.json = 12, None
            while pos < length:
                clen, ctype = struct.unpack('<II', raw[pos:pos + 8])
                chunk = raw[pos + 8:pos + 8 + clen]
                if ctype == 0x4E4F534A:
                    self.json = json.loads(chunk.decode('utf-8'))
                elif ctype == 0x004E4942:
                    self.bin = chunk
                pos += 8 + clen
        else:
            self.json = json.loads(raw.decode('utf-8'))
        self.buffers = []
        for i, b in enumerate(self.json.get('buffers', [])):
            uri = b.get('uri')
            if uri is None:
                self.buffers.append(self.bin)
            elif uri.startswith('data:'):
                self.buffers.append(base64.b64decode(uri.split(',', 1)[1]))
            else:
                self.buffers.append(open(os.path.join(self.base, uri), 'rb').read())

    def accessor(self, i, normalize=True):
        a = self.json['accessors'][i]
        if 'sparse' in a:
            raise ValueError('sparse accessors are not supported (re-export without sparse data)')
        n, comps, dt = a['count'], NCOMP[a['type']], COMP[a['componentType']]
        if 'bufferView' not in a:
            out = np.zeros((n, comps), dtype=np.float64)
        else:
            v = self.json['bufferViews'][a['bufferView']]
            buf = self.buffers[v['buffer']]
            off = v.get('byteOffset', 0) + a.get('byteOffset', 0)
            item = np.dtype(dt).itemsize * comps
            stride = v.get('byteStride', item)
            rows = [np.frombuffer(buf, dtype=dt, count=comps, offset=off + k * stride) for k in range(n)] \
                if stride != item else [np.frombuffer(buf, dtype=dt, count=n * comps, offset=off)]
            out = np.concatenate(rows).reshape(n, comps)
        if normalize and a.get('normalized') and a['componentType'] in NORM:
            out = np.maximum(out.astype(np.float64) / NORM[a['componentType']], -1.0)
        return out

    def node_xf(self, i):
        nd = self.json['nodes'][i]
        if 'matrix' in nd:
            m = np.array(nd['matrix'], dtype=np.float64).reshape(4, 4).T      # column-major
            xf, _ = decompose(m)
            return xf, nd
        s = nd.get('scale', [1, 1, 1])
        return Xf(nd.get('rotation', [0, 0, 0, 1]), nd.get('translation', [0, 0, 0]), sum(s) / 3.0), nd


def _sample(g, sampler, path, times):
    """a glTF animation sampler evaluated at times -> array (len(times), components)"""
    inp = g.accessor(sampler['input'])[:, 0].astype(np.float64)
    out = g.accessor(sampler['output']).astype(np.float64)
    interp = sampler.get('interpolation', 'LINEAR')
    comps = out.shape[1]
    if interp == 'CUBICSPLINE':
        out = out.reshape(len(inp), 3, comps)          # in-tangent, value, out-tangent
    res = np.zeros((len(times), comps))
    for k, t in enumerate(times):
        if t <= inp[0] or len(inp) == 1:
            j, u = 0, 0.0
        elif t >= inp[-1]:
            j, u = len(inp) - 1, 0.0
        else:
            j = int(np.searchsorted(inp, t, side='right')) - 1
            u = (t - inp[j]) / (inp[j + 1] - inp[j])
        if interp == 'CUBICSPLINE':
            if u == 0.0:
                v = out[j, 1]
            else:
                dt = inp[j + 1] - inp[j]
                p0, m0, p1, m1 = out[j, 1], out[j, 2] * dt, out[j + 1, 1], out[j + 1, 0] * dt
                u2, u3 = u * u, u * u * u
                v = (2 * u3 - 3 * u2 + 1) * p0 + (u3 - 2 * u2 + u) * m0 + (-2 * u3 + 3 * u2) * p1 + (u3 - u2) * m1
            res[k] = qnorm(v) if path == 'rotation' else v
        elif interp == 'STEP' or u == 0.0:
            res[k] = out[j]
        elif path == 'rotation':
            res[k] = slerp(qnorm(out[j]), qnorm(out[j + 1]), u)
        else:
            res[k] = out[j] * (1 - u) + out[j + 1] * u
    return res


def read_gltf(path, fps=30.0, skin=0, clips='*', loop=(), motion=(), additive=(), events=None, motion_bone=None,
              log=print):
    """-> (bones, clips, motion bone index, Gltf, joint node -> bone index)"""
    g = Gltf(path)
    j = g.json
    if not j.get('skins'):
        raise ValueError('%s has no skin' % path)
    sk = j['skins'][skin]
    joints = list(sk['joints'])
    nodes = j['nodes']
    parent = {}
    for i, nd in enumerate(nodes):
        for c in nd.get('children', []):
            parent[c] = i
    jset = set(joints)

    def depth(n):
        d = 0
        while n in parent and parent[n] in jset:
            n, d = parent[n], d + 1
        return d
    order = sorted(joints, key=lambda n: (depth(n), joints.index(n)))
    # parents first, keeping the source order among bones of one depth
    order = _parents_first(order, parent, jset)
    if len(order) > MAX_BONES:
        raise ValueError('skin has %d joints; at most %d' % (len(order), MAX_BONES))
    index = {n: k for k, n in enumerate(order)}

    def above(n):
        """world transform of the non-joint nodes above a root joint (baked into it)"""
        xf = Xf()
        chain = []
        while n in parent:
            n = parent[n]
            chain.append(n)
        for a in reversed(chain):
            axf, _ = g.node_xf(a)
            xf = xf.then(axf)
        return xf
    ibm = g.accessor(sk['inverseBindMatrices']) if 'inverseBindMatrices' in sk else None
    restxf = {n: g.node_xf(n)[0] for n in order}
    bones, base = [], {}
    for n in order:
        xf, nd = g.node_xf(n)
        name = nd.get('name', 'joint%d' % n)
        s = nd.get('scale', [1, 1, 1])
        if max(s) - min(s) > 0.01 * max(abs(max(s)), 1e-9):
            log('note: %s has non-uniform scale %s; using the mean' % (name, s))
        p = index[parent[n]] if n in parent and parent[n] in jset else -1
        if p < 0:
            base[n] = above(n)
            xf = base[n].then(xf)
        inv = None
        if ibm is not None:
            m = ibm[joints.index(n)].reshape(4, 4).T
            inv = m[:3, :]
        bones.append(Bone(name, p, xf, inv))
    mb = -1
    if motion_bone is not None:
        names = [b.name for b in bones]
        if motion_bone not in names:
            raise ValueError('motion bone %s is not a joint of the skin' % motion_bone)
        mb = names.index(motion_bone)
    elif motion:
        mb = 0
    ev = {}
    if events:
        raw = json.load(open(events)) if isinstance(events, str) else events
        for k, lst in raw.items():
            ev[k] = [(e['t'], e['name'], e.get('arg', 0)) if isinstance(e, dict) else (e[0], e[1], e[2] if len(e) > 2 else 0)
                     for e in lst]
    out = []
    for ai, an in enumerate(j.get('animations', [])):
        name = an.get('name', 'clip%d' % ai)
        if not any(fnmatch.fnmatchcase(name, p) for p in ([clips] if isinstance(clips, str) else clips)):
            continue
        chans = [c for c in an['channels'] if c['target'].get('node') in jset and c['target']['path'] != 'weights']
        if not chans:
            continue
        lo = min(g.accessor(an['samplers'][c['sampler']]['input'])[:, 0].min() for c in chans)
        hi = max(g.accessor(an['samplers'][c['sampler']]['input'])[:, 0].max() for c in chans)
        frames = max(1, int(round((hi - lo) * fps)) + 1)
        times = lo + np.arange(frames) / fps if frames > 1 else np.array([lo])
        duration = (frames - 1) / fps if frames > 1 else 1.0 / fps
        per = {}
        for c in chans:
            per.setdefault(c['target']['node'], {})[c['target']['path']] = \
                _sample(g, an['samplers'][c['sampler']], c['target']['path'], times)
        flags = 0
        if any(fnmatch.fnmatchcase(name, p) for p in loop):
            flags |= LOOP
        if any(fnmatch.fnmatchcase(name, p) for p in motion):
            flags |= MOTION_XZ | MOTION_YAW
        if any(fnmatch.fnmatchcase(name, p) for p in additive):
            flags |= ADDITIVE
        keys = []
        for f in range(frames):
            row = []
            for n in order:
                xf0 = restxf[n]
                ch = per.get(n, {})
                r = ch['rotation'][f] if 'rotation' in ch else xf0.r
                t = ch['translation'][f] if 'translation' in ch else xf0.t
                s = float(ch['scale'][f].mean()) if 'scale' in ch else xf0.s
                xf = Xf(r, t, s)
                if n in base:
                    xf = base[n].then(xf)
                row.append(xf)
            keys.append(row)
        if flags & LOOP and frames > 1 and not _same_pose(keys[0], keys[-1]):
            keys.append(keys[0])
            duration += 1.0 / fps
            log('note: %s loops; appended its first frame so the cycle closes' % name)
        out.append(Clip(name, duration, keys, flags, ev.get(name, ())))
    return bones, out, mb, g, index


def _parents_first(order, parent, jset):
    done, res = set(), []

    def visit(n):
        if n in done:
            return
        if n in parent and parent[n] in jset:
            visit(parent[n])
        done.add(n)
        res.append(n)
    for n in order:
        visit(n)
    return res


def _same_pose(a, b):
    return all(1 - abs(float(np.dot(x.r, y.r))) < ROT_TOL and np.abs(x.t - y.t).max() < POS_TOL and
               abs(x.s - y.s) < SCL_TOL for x, y in zip(a, b))


# ---------------------------------------------------------------- skinned meshes
def _abgr(c):
    c = [max(0, min(255, int(round(float(v) * 255)))) for v in (list(c) + [1.0] * 4)[:4]]
    return c[3] << 24 | c[2] << 16 | c[1] << 8 | c[0]


def read_meshes(g, skin, index, pages=None):
    """every primitive skinned by `skin` -> (verts [(u, v, abgr, x, y, z)], skin [(xyz, bones, weights)],
    [(page, [indices])])"""
    j = g.json
    joints = j['skins'][skin]['joints']
    verts, sv, subs = [], [], []
    mats = j.get('materials', [])
    for ni, nd in enumerate(j['nodes']):
        if nd.get('skin') != skin or 'mesh' not in nd:
            continue
        for prim in j['meshes'][nd['mesh']]['primitives']:
            if prim.get('mode', 4) != 4:
                continue
            at = prim['attributes']
            if 'JOINTS_0' not in at or 'WEIGHTS_0' not in at:
                continue
            P = g.accessor(at['POSITION'])
            n = len(P)
            T = g.accessor(at['TEXCOORD_0']) if 'TEXCOORD_0' in at else np.zeros((n, 2))
            C = g.accessor(at['COLOR_0']) if 'COLOR_0' in at else np.ones((n, 4))
            J = g.accessor(at['JOINTS_0'], normalize=False).astype(np.int64)
            W = g.accessor(at['WEIGHTS_0']).astype(np.float64)
            mi = prim.get('material', -1)
            factor = mats[mi].get('pbrMetallicRoughness', {}).get('baseColorFactor', [1, 1, 1, 1]) if mi >= 0 else [1] * 4
            mname = mats[mi].get('name', 'material%d' % mi) if mi >= 0 else ''
            page = (pages or {}).get(mname, mi)
            first = len(verts)
            for k in range(n):
                col = [C[k][c] * factor[c] if c < C.shape[1] else factor[c] for c in range(4)]
                verts.append((float(T[k][0]), float(T[k][1]), _abgr(col), *map(float, P[k])))
                w = np.where(W[k] > 0, W[k], 0)
                tot = w.sum()
                w = w / tot if tot > 0 else np.array([1.0, 0, 0, 0])
                b = [index[joints[int(x)]] if wv > 0 else 0 for x, wv in zip(J[k], w)]
                sv.append((tuple(map(float, P[k])), b, list(map(float, w))))
            idx = g.accessor(prim['indices'], normalize=False)[:, 0].astype(np.int64) if 'indices' in prim \
                else np.arange(n)
            subs.append((page, [first + int(i) for i in idx]))
    return verts, sv, subs


def build_mesh(verts, skin, subs, lod=1, ratio=0.5):
    """-> VSM1 bytes; lod > 1 adds coarser levels (vertex order: every level uses a prefix)"""
    nv = len(verts)
    if not 0 < nv <= 65535:
        raise ValueError('a skinned mesh has 1..65535 vertices')
    lod = max(1, min(MESH_LOD, lod))
    owner = [0] * nv
    for s, (_, il) in enumerate(subs):
        for i in il:
            owner[i] = s
    all_idx = [i for _, il in subs for i in il]
    if lod > 1:
        here = os.path.dirname(os.path.abspath(__file__))
        if here not in sys.path:
            sys.path.insert(0, here)
        from vcpe_lod import simplify
        order, levels = simplify(verts, all_idx, levels=lod, ratio=ratio)
        new = {old: k for k, old in enumerate(order)}
        levels = [(nvu, il, err) for nvu, il, err in levels][:lod]
    else:
        order = list(range(nv))
        new = {k: k for k in range(nv)}
        levels = [(nv, all_idx, 0.0)]
    inv_owner = [owner[o] for o in order]
    # level -> submesh -> indices (a triangle belongs to the submesh of its vertices)
    per = []
    for nvu, il, err in levels:
        lists = [[] for _ in subs]
        for t in range(0, len(il), 3):
            tri = il[t:t + 3] if lod > 1 else [new[i] for i in il[t:t + 3]]
            lists[inv_owner[tri[0]]].extend(tri)
        per.append((nvu if lod > 1 else nv, lists, err))
    P = np.array([v[3:6] for v in verts], dtype=np.float64)
    lo, hi = P.min(0), P.max(0)
    centre = (lo + hi) / 2
    radius = float(np.sqrt(((P - centre) ** 2).sum(1)).max())
    used = max((max(b) for _, b, _ in skin), default=0) + 1
    nl, ns = len(per), len(subs)
    level_off = 48
    sub_off = level_off + 8 * nl
    sub_off += -sub_off % 16
    vtx_off = sub_off + 36 * ns
    vtx_off += -vtx_off % 16
    bind_off = vtx_off + 24 * nv
    bind_off += -bind_off % 16
    idx_at = bind_off + 36 * nv
    idx_at += -idx_at % 16
    ltab = b''.join(struct.pack('<HHf', nvu, 0, err) for nvu, _, err in per)
    vdata = b''.join(struct.pack('<2fI3f', *verts[o]) for o in order)
    bdata = b''
    for o in order:
        xyz, b, w = skin[o]
        bdata += struct.pack('<3f4H4f', *xyz, *b, *w)
    idata, offs = b'', [[(0, 0)] * MESH_LOD for _ in subs]
    for li, (_, lists, _) in enumerate(per):
        for s, il in enumerate(lists):
            idata += b'\0' * (-(idx_at + len(idata)) % 16)
            offs[s][li] = (idx_at + len(idata), len(il))
            idata += struct.pack('<%dH' % len(il), *il)
    stab = b''
    for s, (page, _) in enumerate(subs):
        o = offs[s]
        stab += struct.pack('<hH4I4I', page, 0, *[x[0] for x in o], *[x[1] for x in o])
    total = idx_at + len(idata)
    total += -total % 16
    head = struct.pack('<4sHHHHIIIII4f', b'VSM1', nv, ns, nl, used, total, vtx_off, bind_off, sub_off, level_off,
                       *centre, radius)
    blob = bytearray(total)
    blob[0:48] = head
    blob[level_off:level_off + len(ltab)] = ltab
    blob[sub_off:sub_off + len(stab)] = stab
    blob[vtx_off:vtx_off + len(vdata)] = vdata
    blob[bind_off:bind_off + len(bdata)] = bdata
    blob[idx_at:idx_at + len(idata)] = idata
    return bytes(blob), [(sum(len(il) for il in lists) // 3, nvu, err) for nvu, lists, err in per]


def convert(path, fps=30.0, skin=0, clips='*', loop=(), motion=(), additive=(), events=None, motion_bone=None,
            lod=1, pages=None, log=print):
    """-> (VRG1 bytes, VSM1 bytes or None, report lines)"""
    bones, cl, mb, g, index = read_gltf(path, fps, skin, clips, loop, motion, additive, events, motion_bone, log)
    rig = build_rig(bones, cl, mb)
    report = ['%d bones, %d clips, %d bytes' % (len(bones), len(cl), len(rig))]
    for c in cl:
        report.append('  %-24s %5d frames %6.3f s%s' % (c.name, len(c.keys), c.duration,
                                                     ''.join(' ' + n for f, n in ((LOOP, 'loop'), (MOTION_XZ, 'motion'),
                                                                                 (ADDITIVE, 'additive')) if c.flags & f)))
    verts, sv, subs = read_meshes(g, skin, index, pages)
    mesh = None
    if verts:
        mesh, info = build_mesh(verts, sv, subs, lod)
        report.append('mesh: %d vertices, %d submeshes, %d bytes' % (len(verts), len(subs), len(mesh)))
        for k, (t, v, e) in enumerate(info):
            report.append('  level %d: %6d triangles, %5d vertices, error %.5g' % (k, t, v, e))
    return rig, mesh, report


def main(argv):
    if len(argv) < 2:
        print(__doc__.strip().splitlines()[-3])
        print(__doc__.strip().splitlines()[-2])
        print(__doc__.strip().splitlines()[-1])
        return 2
    opts = {'--fps': 30.0, '--skin': 0, '--clips': '*', '--motion-bone': None, '--events': None, '--lod': 1,
            '--mesh': None}
    multi = {'--loop': [], '--motion': [], '--additive': [], '--page': []}
    rest = []
    it = iter(argv)
    for a in it:
        if a in opts:
            v = next(it)
            opts[a] = type(opts[a])(v) if opts[a] is not None else v
        elif a in multi:
            multi[a].append(next(it))
        else:
            rest.append(a)
    pages = {}
    for p in multi['--page']:
        k, v = p.rsplit('=', 1)
        pages[k] = int(v)
    rig, mesh, report = convert(rest[0], opts['--fps'], opts['--skin'], opts['--clips'], multi['--loop'],
                                multi['--motion'], multi['--additive'], opts['--events'], opts['--motion-bone'],
                                opts['--lod'], pages)
    open(rest[1], 'wb').write(rig)
    if opts['--mesh'] and mesh:
        open(opts['--mesh'], 'wb').write(mesh)
    print('\n'.join(report))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
