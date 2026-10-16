"""Godot 2D scene compositor (Godot 3 and 4 text scenes): flattens a scene, or a part of one, into a still RGBA
image the way the engine would draw it at rest - for converters that bake backgrounds, props and the rest poses of
cut-out characters into PSP pictures.

Handles: instanced scenes (with overrides of their nodes), Node2D transforms (position, rotation/rotation_degrees,
scale), Control rects (anchors + margins or rect_position, rect_scale, rect_rotation, pivot), Sprite (centered,
offset, flip, region, hframes/vframes/frame), TextureRect (stretch scale/tile/keep), TextureButton (normal
texture), NinePatchRect, ColorRect, AtlasTexture sub-resources, visible, modulate/self_modulate, z_index and
z_as_relative, show_behind_parent. Ignored: labels, particles, lights, shaders, physics and animation (rest pose).

    from godot.scene2d import Scene2D
    sc = Scene2D(project_root)                     # the folder res:// points to
    nodes = sc.load('res://Core/Main.tscn')        # flattened node list with global transforms
    img = sc.render(nodes, size=(1920, 1080), root='Shop', exclude=['Shop/ShopOffer1'])
    img.resize((480, 270), Image.LANCZOS)
"""
import math
import re
from pathlib import Path

import numpy as np
from PIL import Image

HDR = re.compile(r'^\[(\w+)(.*)\]\s*$')
ATTR = re.compile(r'(\w+)=("(?:[^"\\]|\\.)*"|\S+)')
NUM = r'[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?|[-+]?inf|nan'


def _attrs(s):
    return {k: v.strip('"') for k, v in ATTR.findall(s)}


def parse(path):
    """-> dict(ext={id: attrs}, sub={id: (type, props)}, nodes=[(attrs, props)]) with raw value strings"""
    ext, sub, nodes = {}, {}, []
    cur = None
    key, buf, depth = None, [], 0
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            if depth == 0:
                m = HDR.match(line)
                if m and not line.startswith('[{') and not line.startswith('[ '):
                    kind, a = m.group(1), _attrs(m.group(2))
                    inst = re.search(r'instance=ExtResource\(\s*"?([\w\-]+)"?\s*\)', m.group(2))
                    if inst:
                        a['instance'] = inst.group(1)   # Godot 3 writes ExtResource( 1 ) with spaces
                    props = {}
                    if kind == 'ext_resource':
                        ext[a['id']] = a
                        cur = None
                    elif kind == 'sub_resource':
                        sub[a['id']] = (a['type'], props)
                        cur = props
                    elif kind == 'node':
                        nodes.append((a, props))
                        cur = props
                    else:
                        cur = props
                    continue
                if cur is None or '=' not in line:
                    continue
                key, val = line.split('=', 1)
                key, val = key.strip(), val.strip()
                depth = val.count('{') + val.count('[') + val.count('(') - val.count('}') - val.count(']') - val.count(')')
                if depth > 0:
                    buf = [val]
                    continue
                cur[key] = val
            else:
                buf.append(line.rstrip('\n'))
                depth += line.count('{') + line.count('[') + line.count('(') - line.count('}') - line.count(']') - line.count(')')
                if depth <= 0:
                    depth = 0
                    cur[key] = '\n'.join(buf)
    return dict(ext=ext, sub=sub, nodes=nodes)


def nums(s):
    """the numbers of a value like Vector2( 1, -2.5e-3 ) or Rect2( ... ) (the type name is skipped)"""
    s = s or ''
    if '(' in s:
        s = s[s.index('(') + 1:]
    return [float(x) for x in re.findall(NUM, s)]


def vec2(s, d=(0.0, 0.0)):
    v = nums(s)
    return (v[0], v[1]) if len(v) >= 2 else d


def color(s, d=(1.0, 1.0, 1.0, 1.0)):
    v = nums(s)
    if len(v) == 4:
        return tuple(v)
    if len(v) == 3:
        return tuple(v) + (1.0,)
    return d


def boolean(s, d=False):
    return d if s is None else s.strip() == 'true'


# The 2D affine matrix used everywhere below. Products compose transforms: (parent * child) maps child space to the
# parent's parent space.
class Mat:
    """2D affine: x' = a x + c y + e, y' = b x + d y + f (Godot Transform2D columns)"""

    def __init__(self, a=1.0, b=0.0, c=0.0, d=1.0, e=0.0, f=0.0):
        self.m = (a, b, c, d, e, f)

    @staticmethod
    def trs(pos, rot, scale):
        cs, sn = math.cos(rot), math.sin(rot)
        return Mat(cs * scale[0], sn * scale[0], -sn * scale[1], cs * scale[1], pos[0], pos[1])

    def __mul__(self, o):
        a, b, c, d, e, f = self.m
        A, B, C, D, E, F = o.m
        return Mat(a * A + c * B, b * A + d * B, a * C + c * D, b * C + d * D, a * E + c * F + e, b * E + d * F + f)

    def apply(self, x, y):
        a, b, c, d, e, f = self.m
        return a * x + c * y + e, b * x + d * y + f

    def inverse(self):
        a, b, c, d, e, f = self.m
        det = a * d - b * c
        if abs(det) < 1e-12:
            return None
        ia, ib, ic, id_ = d / det, -b / det, -c / det, a / det
        return Mat(ia, ib, ic, id_, -(ia * e + ic * f), -(ib * e + id_ * f))


class Scene2D:
    def __init__(self, root):
        self.root = Path(root)
        self.tex_cache = {}
        self.scene_cache = {}

    def path(self, res):
        return self.root / res.replace('res://', '')

    # ------------------------------------------------------------ textures
    def texture(self, res):
        """an image file or a Godot resource; None if missing"""
        if res in self.tex_cache:
            return self.tex_cache[res]
        im = None
        p = self.path(res)
        if p.suffix.lower() in ('.png', '.jpg', '.jpeg', '.webp', '.bmp', '.tga') and p.exists():
            im = Image.open(p).convert('RGBA')
        self.tex_cache[res] = im
        return im

    def tex_value(self, v, sc):
        """a texture property value -> (PIL image or None, region (x, y, w, h) or None)"""
        if not v:
            return None, None
        m = re.match(r'ExtResource\(\s*"?([\w\-]+)"?\s*\)', v)
        if m:
            e = sc['ext'].get(m.group(1))
            if e and e.get('path'):
                return self.texture(e['path']), None
            return None, None
        m = re.match(r'SubResource\(\s*"?([\w\-]+)"?\s*\)', v)
        if m and m.group(1) in sc['sub']:
            typ, props = sc['sub'][m.group(1)]
            if typ == 'AtlasTexture':
                img, _ = self.tex_value(props.get('atlas'), sc)
                r = nums(props.get('region'))
                return img, tuple(r[:4]) if len(r) >= 4 else None
        return None, None

    # ------------------------------------------------------------ scene flattening
    def load(self, res):
        """-> list of node dicts in tree order: path, type, props, sc (the scene the props refer to)"""
        if res in self.scene_cache:
            return [dict(n) for n in self.scene_cache[res]]
        sc = parse(self.path(res))
        out = []
        index = {}
        for a, props in sc['nodes']:
            name, parent = a.get('name'), a.get('parent')
            if parent is None:
                path = '.'
            elif parent == '.':
                path = name
            else:
                path = parent + '/' + name
            if a.get('instance') in sc['ext']:
                child = self.load(sc['ext'][a['instance']]['path'])
                base = path
                for n in child:
                    n['path'] = base if n['path'] == '.' else (n['path'] if base == '.' else base + '/' + n['path'])
                    index[n['path']] = n
                    out.append(n)
                root = index[base]
                for k, v in props.items():
                    root['props'] = dict(root['props'], **{k: v})
                    root['scs'] = dict(root.get('scs', {}), **{k: sc})
                if a.get('type'):
                    root['type'] = a['type']
                continue
            if path in index and 'type' not in a:
                n = index[path]   # an override of an instanced node
                n['props'] = dict(n['props'], **props)
                n['scs'] = dict(n.get('scs', {}), **{k: sc for k in props})
                continue
            n = dict(path=path, type=a.get('type', 'Node'), props=dict(props), sc=sc, scs={})
            index[path] = n
            out.append(n)
        self.scene_cache[res] = out
        return [dict(n) for n in out]

    def prop_sc(self, n, k):
        return n.get('scs', {}).get(k, n['sc'])

    # ------------------------------------------------------------ layout
    def local(self, n, parent_size):
        p, t = n['props'], n['type']
        if t in CONTROL_TYPES:
            size = vec2(p.get('rect_size'), None) if 'rect_size' in p else None
            al, at, ar, ab = (float(p.get(k, '0') or 0) for k in ('anchor_left', 'anchor_top', 'anchor_right', 'anchor_bottom'))
            ml, mt, mr, mb = (float(p.get(k, '0') or 0) for k in ('margin_left', 'margin_top', 'margin_right', 'margin_bottom'))
            pw, ph = parent_size
            x0, y0 = al * pw + ml, at * ph + mt
            x1, y1 = ar * pw + mr, ab * ph + mb
            if 'rect_position' in p:
                x0, y0 = vec2(p['rect_position'])
            if size is None:
                size = (max(0.0, x1 - x0), max(0.0, y1 - y0))
            sc = vec2(p.get('rect_scale'), (1.0, 1.0))
            rot = math.radians(float(p.get('rect_rotation', '0') or 0))
            piv = vec2(p.get('rect_pivot_offset'))
            m = Mat.trs((x0 + piv[0], y0 + piv[1]), rot, sc) * Mat(1, 0, 0, 1, -piv[0], -piv[1])
            return m, size
        pos = vec2(p.get('position'))
        if 'rotation_degrees' in p:
            rot = math.radians(float(p['rotation_degrees']))
        else:
            rot = float(p.get('rotation', '0') or 0)
        sc = vec2(p.get('scale'), (1.0, 1.0))
        return Mat.trs(pos, rot, sc), (0.0, 0.0)

    # Compute every node's global transform, size, z order, visibility and colour: each is its parent's combined with
    # its own (children inherit modulate and visibility; z adds up when z_as_relative).
    def resolve(self, nodes):
        """adds gm (global Mat), size, gz (global z), vis (visible), mod (modulate RGBA), order"""
        by = {}
        for i, n in enumerate(nodes):
            par = n['path'].rsplit('/', 1)[0] if '/' in n['path'] else ('.' if n['path'] != '.' else None)
            pn = by.get(par)
            m, size = self.local(n, pn['size'] if pn else (0.0, 0.0))
            p = n['props']
            n['gm'] = (pn['gm'] * m) if pn else m
            n['size'] = size
            z = int(float(p.get('z_index', '0') or 0))
            rel = boolean(p.get('z_as_relative'), True)
            n['gz'] = (pn['gz'] + z if pn and rel else z)
            n['vis'] = boolean(p.get('visible'), True) and (pn['vis'] if pn else True)
            mod = color(p.get('modulate'))
            pm = pn['mod'] if pn else (1, 1, 1, 1)
            n['mod'] = tuple(a * b for a, b in zip(mod, pm))
            n['smod'] = color(p.get('self_modulate'))
            n['behind'] = boolean(p.get('show_behind_parent'))
            n['order'] = i
            by[n['path']] = n
        return nodes

    # ------------------------------------------------------------ drawing
    # What a node draws, as images with the matrix that places each one (sprites with region/frames/flip/offset,
    # texture rects, nine-patches, colour rects).
    def quads(self, n):
        """-> list of (PIL image, local Mat placing the image's pixel grid)"""
        p, t = n['props'], n['type']
        out = []
        if t == 'Sprite' or t == 'Sprite2D':
            img, region = self.tex_value(p.get('texture'), self.prop_sc(n, 'texture'))
            if img is None:
                return out
            if boolean(p.get('region_enabled')):
                r = nums(p.get('region_rect'))
                if len(r) >= 4:
                    region = (r[0], r[1], r[2], r[3])
            if region:
                x, y, w, h = (int(round(v)) for v in region)
                img = img.crop((x, y, x + w, y + h))
            hf, vf = int(float(p.get('hframes', '1') or 1)), int(float(p.get('vframes', '1') or 1))
            if hf > 1 or vf > 1:
                fw, fh = img.width // hf, img.height // vf
                fr = int(float(p.get('frame', '0') or 0))
                img = img.crop(((fr % hf) * fw, (fr // hf) * fh, (fr % hf + 1) * fw, (fr // hf + 1) * fh))
            if boolean(p.get('flip_h')):
                img = img.transpose(Image.FLIP_LEFT_RIGHT)
            if boolean(p.get('flip_v')):
                img = img.transpose(Image.FLIP_TOP_BOTTOM)
            off = vec2(p.get('offset'))
            x, y = off
            if boolean(p.get('centered'), True):
                x -= img.width / 2.0
                y -= img.height / 2.0
            out.append((img, Mat(1, 0, 0, 1, x, y)))
        elif t in ('TextureRect', 'TextureButton'):
            key = 'texture' if t == 'TextureRect' else 'texture_normal'
            img, region = self.tex_value(p.get(key), self.prop_sc(n, key))
            if img is None:
                return out
            if region:
                x, y, w, h = (int(round(v)) for v in region)
                img = img.crop((x, y, x + w, y + h))
            w, h = n['size']
            if w <= 0 or h <= 0:
                w, h = img.width, img.height
            stretch = int(float(p.get('stretch_mode', '0') or 0))
            expand = boolean(p.get('expand'))
            if t == 'TextureButton':
                if boolean(p.get('expand')):
                    out.append((img, Mat(w / img.width, 0, 0, h / img.height, 0, 0)))
                else:
                    out.append((img, Mat()))
            elif stretch == 0 and expand or stretch == 1:          # scale on expand, or scale
                out.append((img, Mat(w / img.width, 0, 0, h / img.height, 0, 0)))
            elif stretch == 2:                                     # tile
                for ty in range(0, int(math.ceil(h)), img.height):
                    for tx in range(0, int(math.ceil(w)), img.width):
                        out.append((img.crop((0, 0, min(img.width, int(w) - tx), min(img.height, int(h) - ty))),
                                    Mat(1, 0, 0, 1, tx, ty)))
            elif stretch in (4, 5, 6):                             # keep aspect (centered / covered)
                s = (min if stretch != 6 else max)(w / img.width, h / img.height)
                out.append((img, Mat(s, 0, 0, s, (w - img.width * s) / 2, (h - img.height * s) / 2)))
            elif stretch == 3:                                     # keep (top-left, no scale)
                out.append((img, Mat()))
            else:                                                  # keep centered
                out.append((img, Mat(1, 0, 0, 1, (w - img.width) / 2, (h - img.height) / 2)))
        elif t == 'NinePatchRect':
            img, region = self.tex_value(p.get('texture'), self.prop_sc(n, 'texture'))
            if img is None:
                return out
            r = nums(p.get('region_rect'))
            if len(r) >= 4 and r[2] > 0:
                img = img.crop((int(r[0]), int(r[1]), int(r[0] + r[2]), int(r[1] + r[3])))
            elif region:
                x, y, w, h = (int(round(v)) for v in region)
                img = img.crop((x, y, x + w, y + h))
            w, h = n['size']
            l, t_, r_, b = (int(float(p.get(k, '0') or 0)) for k in ('patch_margin_left', 'patch_margin_top',
                                                                    'patch_margin_right', 'patch_margin_bottom'))
            out.append((ninepatch(img, int(round(w)), int(round(h)), l, t_, r_, b), Mat()))
        elif t == 'ColorRect':
            w, h = n['size']
            c = color(p.get('color'))
            if w > 0 and h > 0:
                im = Image.new('RGBA', (max(1, int(round(w))), max(1, int(round(h)))),
                               tuple(int(round(v * 255)) for v in c))
                out.append((im, Mat()))
        return out

    # Draw the visible nodes in Godot's order (z, then tree order, with show_behind_parent) onto a float RGBA canvas.
    def render(self, nodes, size=(1920, 1080), root=None, exclude=(), view=None, only_types=None):
        """draw the visible nodes under root (a node path; None = all) into an RGBA image of `size`.
        view: a Mat applied after the global transforms (e.g. to move a part to the image centre)."""
        nodes = self.resolve(nodes)
        canvas = np.zeros((size[1], size[0], 4), np.float32)   # straight RGBA 0..1
        excl = tuple(exclude)

        def inside(path):
            if root is None:
                return True
            return path == root or path.startswith(root + '/')

        draw = []
        for n in nodes:
            if not n['vis'] or not inside(n['path']):
                continue
            if any(n['path'] == e or n['path'].startswith(e + '/') for e in excl):
                continue
            if only_types and n['type'] not in only_types:
                continue
            for img, lm in self.quads(n):
                key = (n['gz'], 0 if n['behind'] else 1, n['order'])
                draw.append((key, n, img, lm))
        draw.sort(key=lambda d: d[0])
        for _, n, img, lm in draw:
            m = n['gm'] * lm
            if view is not None:
                m = view * m
            mod = tuple(a * b for a, b in zip(n['mod'], n['smod']))
            blit(canvas, img, m, mod)
        return Image.fromarray(np.clip(canvas * 255 + 0.5, 0, 255).astype(np.uint8), 'RGBA')


CONTROL_TYPES = {'Control', 'TextureRect', 'TextureButton', 'NinePatchRect', 'ColorRect', 'Panel', 'Button', 'Label',
                 'RichTextLabel', 'HBoxContainer', 'VBoxContainer', 'MarginContainer', 'CenterContainer',
                 'GridContainer', 'ScrollContainer', 'PanelContainer', 'HSlider', 'VSlider', 'ProgressBar',
                 'TextureProgress', 'LineEdit', 'OptionButton', 'CheckBox', 'CheckButton', 'MenuButton'}


# Nine-patch: the corners keep their size, the edges stretch along one axis, the centre along both.
def ninepatch(img, w, h, l, t, r, b):
    w, h = max(1, w), max(1, h)
    out = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    iw, ih = img.size
    xs = [(0, l, 0, l), (l, iw - r, l, w - r), (iw - r, iw, w - r, w)]
    ys = [(0, t, 0, t), (t, ih - b, t, h - b), (ih - b, ih, h - b, h)]
    for sx0, sx1, dx0, dx1 in xs:
        for sy0, sy1, dy0, dy1 in ys:
            if sx1 <= sx0 or sy1 <= sy0 or dx1 <= dx0 or dy1 <= dy0:
                continue
            out.paste(img.crop((sx0, sy0, sx1, sy1)).resize((dx1 - dx0, dy1 - dy0), Image.BILINEAR), (dx0, dy0))
    return out


# Composite an image through a matrix: find its bounding box on the canvas, sample the image through the inverse
# matrix (bilinear, premultiplied alpha so edges don't darken), then alpha-blend it over the canvas.
def blit(canvas, img, m, mod):
    """alpha-composite img placed by Mat m (image pixel space -> canvas) with modulate colour mod;
    canvas: float32 HxWx4 straight RGBA in 0..1, changed in place"""
    if mod[3] <= 0:
        return
    a = np.asarray(img, np.float32) / 255.0
    if mod != (1, 1, 1, 1):
        a = a * np.array(mod, np.float32)
    # bounding box on the canvas
    corners = [m.apply(x, y) for x, y in ((0, 0), (img.width, 0), (0, img.height), (img.width, img.height))]
    x0 = max(0, int(math.floor(min(c[0] for c in corners))))
    y0 = max(0, int(math.floor(min(c[1] for c in corners))))
    x1 = min(canvas.shape[1], int(math.ceil(max(c[0] for c in corners))))
    y1 = min(canvas.shape[0], int(math.ceil(max(c[1] for c in corners))))
    if x1 <= x0 or y1 <= y0:
        return
    inv = m.inverse()
    if inv is None:
        return
    # inverse map of the box, sampled bilinearly from premultiplied colour
    src = Image.fromarray(np.clip(np.concatenate([a[..., :3] * a[..., 3:4], a[..., 3:4]], 2) * 255, 0, 255)
                          .astype(np.uint8), 'RGBA')
    t = inv * Mat(1, 0, 0, 1, x0, y0)
    ia, ib, ic, id_, ie, if_ = t.m
    piece = src.transform((x1 - x0, y1 - y0), Image.AFFINE, (ia, ic, ie, ib, id_, if_), resample=Image.BILINEAR)
    p = np.asarray(piece, np.float32) / 255.0
    region = canvas[y0:y1, x0:x1]
    # premultiplied over: out = src + dst * (1 - src_a), kept straight in the canvas
    da = region[..., 3:4]
    dc = region[..., :3] * da
    oa = p[..., 3:4] + da * (1 - p[..., 3:4])
    oc = p[..., :3] + dc * (1 - p[..., 3:4])
    with np.errstate(invalid='ignore', divide='ignore'):
        straight = np.where(oa > 0, oc / oa, 0)
    canvas[y0:y1, x0:x1] = np.concatenate([straight, oa], 2)
