"""Nodes moved or changed by any AnimationPlayer track in a parsed Godot text scene (tscn.parse)."""
import re, posixpath


def node_path(a):
    parent = a.get("parent")
    return a["name"] if parent in (None, ".") else parent + "/" + a["name"]


def animated_nodes(sc):
    """-> set of absolute node paths (relative to the scene root, like node_path) that some track targets"""
    libs = {}
    for sid, (t, p) in sc["sub"].items():
        if t == "AnimationLibrary" and "_data" in p:
            libs[sid] = re.findall(r'SubResource\("([^"]+)"\)', p["_data"])
    tracks = {}
    for sid, (t, p) in sc["sub"].items():
        if t == "Animation":
            tracks[sid] = [v.split(":")[0] for k, v in ((k, re.search(r'NodePath\("([^"]*)"\)', v).group(1))
                           for k, v in p.items() if re.match(r'tracks/\d+/path$', k) and 'NodePath("' in v)]
    out = set()
    for a, p in sc["nodes"]:
        if a.get("type") != "AnimationPlayer":
            continue
        me = node_path(a)
        root = re.search(r'NodePath\("([^"]*)"\)', p.get("root_node", 'NodePath("..")')).group(1)
        base = posixpath.normpath(posixpath.join(me, root)) if root else me
        for lib in re.findall(r'SubResource\("([^"]+)"\)', p.get("libraries", "")):
            for anim in libs.get(lib, []):
                for tp in tracks.get(anim, []):
                    if not tp:
                        continue
                    full = posixpath.normpath(posixpath.join(base, tp))
                    out.add("" if full == "." else full)
    return out


def is_under(path, roots):
    while path:
        if path in roots:
            return True
        path = path.rsplit("/", 1)[0] if "/" in path else ""
    return False
