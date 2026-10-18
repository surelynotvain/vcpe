"""A small humanoid rig with locomotion, a one-shot, an additive and a turning clip, for tests/animator."""
import math
import os
import sys

V = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
sys.path.insert(0, os.path.join(V, 'tools'))
import vcpe_rig as R  # noqa: E402
import numpy as np  # noqa: E402


def qaxis(axis, deg):
    a = np.asarray(axis, dtype=np.float64)
    a = a / np.linalg.norm(a)
    h = math.radians(deg) / 2
    return np.array([*(a * math.sin(h)), math.cos(h)])


bones = [R.Bone('root', -1, R.Xf()),
         R.Bone('pelvis', 0, R.Xf(t=(0, 1.0, 0))),
         R.Bone('thigh.L', 1, R.Xf(t=(0.1, 0, 0))), R.Bone('calf.L', 2, R.Xf(t=(0, -0.45, 0))),
         R.Bone('foot.L', 3, R.Xf(t=(0, -0.45, 0))),
         R.Bone('thigh.R', 1, R.Xf(t=(-0.1, 0, 0))), R.Bone('calf.R', 5, R.Xf(t=(0, -0.45, 0))),
         R.Bone('foot.R', 6, R.Xf(t=(0, -0.45, 0))),
         R.Bone('spine', 1, R.Xf(t=(0, 0.3, 0))),
         R.Bone('arm', 8, R.Xf(t=(0.2, 0.2, 0)))]
NB = len(bones)


def keys(frames, fn):
    out = []
    for f in range(frames):
        u = f / (frames - 1) if frames > 1 else 0.0
        row = []
        for b in range(NB):
            x = fn(u, b)
            row.append(x if x is not None else R.Xf(bones[b].bind.r, bones[b].bind.t, bones[b].bind.s))
        out.append(row)
    return out


def stride(dist, swing):
    def fn(u, b):
        if b == 0:
            return R.Xf(t=(0, 0, -dist * u))
        if b in (2, 5):
            return R.Xf(qaxis((1, 0, 0), swing * math.sin(2 * math.pi * u) * (1 if b == 2 else -1)), bones[b].bind.t)
        if b == 8:
            return R.Xf(qaxis((0, 1, 0), 5 * math.sin(2 * math.pi * u)), bones[8].bind.t)
        return None
    return fn


clips = [
    R.Clip('idle', 1.0, keys(31, lambda u, b: R.Xf(qaxis((0, 0, 1), 3 * math.sin(2 * math.pi * u)), bones[8].bind.t) if b == 8 else None),
           R.LOOP),
    R.Clip('walk', 1.0, keys(31, stride(1.4, 25)), R.LOOP | R.MOTION_XZ | R.MOTION_YAW,
           [(0.0, 'foot', 0), (0.5, 'foot', 1)]),
    R.Clip('run', 0.6, keys(19, stride(3.0, 45)), R.LOOP | R.MOTION_XZ | R.MOTION_YAW,
           [(0.0, 'foot', 0), (0.3, 'foot', 1)]),
    R.Clip('swing', 0.5, keys(16, lambda u, b: R.Xf(qaxis((0, 0, 1), 90 * math.sin(math.pi * u)), bones[9].bind.t) if b == 9 else
                         (R.Xf(qaxis((0, 1, 0), 30 * math.sin(math.pi * u)), bones[8].bind.t) if b == 8 else None)), 0,
           [(0.25, 'hit', 42)]),
    R.Clip('breathe', 2.0, keys(21, lambda u, b: R.Xf(qaxis((1, 0, 0), 4 * math.sin(2 * math.pi * u)), bones[8].bind.t) if b == 8 else None),
           R.LOOP | R.ADDITIVE),
    R.Clip('turn', 1.0, keys(31, lambda u, b: R.Xf(qaxis((0, 1, 0), 90 * u)) if b == 0 else None), R.MOTION_YAW),
]
open(sys.argv[1], 'wb').write(R.build_rig(bones, clips, motion_bone=0))
