"""Minimal reader for Godot 4 text scenes (.tscn): sub_resources, ext_resources and nodes with their properties.
Values stay raw strings; helpers decode what the converter needs."""
import re

# A .tscn file is a list of sections: a header line in brackets ([ext_resource ...], [sub_resource ...], [node ...])
# followed by "key = value" lines. A value can span several lines (arrays, dictionaries): bracket depth is counted,
# and lines are collected until it returns to zero.
HDR = re.compile(r'^\[(\w+)(.*)\]\s*$')
ATTR = re.compile(r'(\w+)=("(?:[^"\\]|\\.)*"|\S+)')


def _attrs(s):
    return {k: v.strip('"') for k, v in ATTR.findall(s)}


def parse(path):
    """-> dict(ext={id: attrs}, sub={id: (type, props)}, nodes=[(attrs, props)])"""
    ext, sub, nodes = {}, {}, []
    cur = None
    key, buf, depth = None, [], 0
    with open(path, encoding="utf-8") as f:
        for line in f:
            if depth == 0:
                m = HDR.match(line)
                if m and not line.startswith('[{'):
                    kind, a = m.group(1), _attrs(m.group(2))
                    props = {}
                    if kind == "ext_resource":
                        ext[a["id"]] = a
                        cur = None
                    elif kind == "sub_resource":
                        sub[a["id"]] = (a["type"], props)
                        cur = props
                    elif kind == "node":
                        nodes.append((a, props))
                        cur = props
                    else:
                        cur = props
                    continue
                if cur is None or "=" not in line:
                    continue
                key, val = line.split("=", 1)
                key, val = key.strip(), val.strip()
                depth = val.count("[") + val.count("{") + val.count("(") - val.count("]") - val.count("}") - val.count(")")
                if depth > 0:
                    buf = [val]
                    continue
                cur[key] = val
            else:
                buf.append(line.rstrip("\n"))
                depth += line.count("[") + line.count("{") + line.count("(") - line.count("]") - line.count("}") - line.count(")")
                if depth <= 0:
                    cur[key] = "\n".join(buf)
                    depth = 0
    return {"ext": ext, "sub": sub, "nodes": nodes}


# Split a mesh's _surfaces array into one dict per surface (its blocks are separated by "}, {").
def surfaces(raw):
    """ArrayMesh _surfaces -> list of dicts with format, vertex_count, index_count, primitive, material, name"""
    out = []
    for block in re.split(r'\n\}, \{|\}, \{', raw):
        d = {}
        for k in ("format", "vertex_count", "index_count", "primitive"):
            m = re.search(r'"%s": (\d+)' % k, block)
            d[k] = int(m.group(1)) if m else 0
        m = re.search(r'"material": (\w+)\("([^"]+)"\)', block)
        d["material"] = m.groups() if m else None
        m = re.search(r'"name": "([^"]*)"', block)
        d["name"] = m.group(1) if m else ""
        out.append(d)
    return out
