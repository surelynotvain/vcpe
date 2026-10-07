"""World transforms and bounding boxes of a parsed Godot text scene (tscn.parse)."""
import re, math
import numpy as np


def nums(s):
    """numbers inside the outermost parentheses (so "Vector3(" or "Transform3D(" add nothing)"""
    if "(" in s:
        s = s[s.index("(") + 1:s.rindex(")")]
    return [float(x) for x in re.findall(r'-?\d+(?:\.\d+)?(?:e[-+]?\d+)?', s)]


def xform(s):
    """Transform3D(basis rows 9, origin 3) -> 4x4"""
    v = nums(s)
    m = np.eye(4)
    m[:3, :3] = np.array(v[:9]).reshape(3, 3)   # written row by row (Basis.rows[i][j])
    m[:3, 3] = v[9:12]
    return m


def world_transforms(sc):
    """node path ('.'-relative, like Godot's parent paths) -> 4x4 world matrix"""
    W = {}
    for a, p in sc["nodes"]:
        parent = a.get("parent")
        path = a["name"] if parent is None else (a["name"] if parent == "." else parent + "/" + a["name"])
        local = xform(p["transform"]) if "transform" in p else np.eye(4)
        if parent is None:
            W["."] = local
            continue
        W[path] = W.get(parent, np.eye(4)) @ local
    return W


def euler_deg_to_basis(rx, ry, rz):
    """Godot YXZ euler (degrees) -> 3x3"""
    x, y, z = map(math.radians, (rx, ry, rz))
    cx, sx, cy, sy, cz, sz = math.cos(x), math.sin(x), math.cos(y), math.sin(y), math.cos(z), math.sin(z)
    Rx = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]])
    Ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    Rz = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]])
    return Ry @ Rx @ Rz
