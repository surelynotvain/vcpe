"""Reader for GameMaker: Studio 1.4 data files (data.win / game.unx, bytecode 14-16).

Parses every chunk the PSP runtime needs into plain Python objects. Pointers in the file are absolute offsets.
"""
import struct


class R:
    def __init__(self, b):
        self.b = b

    def u8(self, o): return self.b[o]
    def u16(self, o): return struct.unpack_from('<H', self.b, o)[0]
    def i16(self, o): return struct.unpack_from('<h', self.b, o)[0]
    def u32(self, o): return struct.unpack_from('<I', self.b, o)[0]
    def i32(self, o): return struct.unpack_from('<i', self.b, o)[0]
    def f32(self, o): return struct.unpack_from('<f', self.b, o)[0]
    def f64(self, o): return struct.unpack_from('<d', self.b, o)[0]

    def s(self, o):
        """GM string pointer: points at the characters, length u32 sits 4 bytes before"""
        if o == 0:
            return None
        n = self.u32(o - 4)
        return self.b[o:o + n].decode('utf-8', 'replace')

    def plist(self, o):
        n = self.u32(o)
        return [self.u32(o + 4 + 4 * i) for i in range(n)]


class Obj:
    def __init__(self, **kw):
        self.__dict__.update(kw)

    def __repr__(self):
        return 'Obj(%s)' % ', '.join('%s=%r' % kv for kv in self.__dict__.items() if not isinstance(kv[1], (list, bytes)))


EVENT_NAMES = ['Create', 'Destroy', 'Alarm', 'Step', 'Collision', 'Keyboard', 'Mouse', 'Other', 'Draw', 'KeyPress',
               'KeyRelease', 'Trigger', 'CleanUp', 'Gesture', 'PreCreate']


class GMData:
    def __init__(self, path):
        self.path = path
        b = open(path, 'rb').read()
        self.b = b
        r = self.r = R(b)
        assert b[:4] == b'FORM'
        self.chunks = {}
        p = 8
        end = 8 + r.u32(4)
        while p < end:
            name = b[p:p + 4].decode()
            ln = r.u32(p + 4)
            self.chunks[name] = (p + 8, ln)
            p += 8 + ln
        self.gen8()
        self.strings()
        self.textures()
        self.tpag()
        self.sprites()
        self.backgrounds()
        self.sounds()
        self.fonts()
        self.objects()
        self.rooms()
        self.scripts()
        self.code()

    def chunk(self, n):
        return self.chunks.get(n, (0, 0))

    # ---- general
    def gen8(self):
        r, (o, _) = self.r, self.chunk('GEN8')
        self.debug_disabled = r.u8(o)
        self.bytecode = r.u8(o + 1)
        self.filename = r.s(r.u32(o + 4))
        self.config = r.s(r.u32(o + 8))
        self.last_obj = r.u32(o + 12)
        self.last_tile = r.u32(o + 16)
        self.game_id = r.u32(o + 20)
        self.name = r.s(r.u32(o + 40))
        self.major, self.minor, self.release, self.build = struct.unpack_from('<4I', self.b, o + 44)
        self.def_w, self.def_h = r.u32(o + 60), r.u32(o + 64)
        self.info = r.u32(o + 68)
        # rooms order list sits after license/timestamp/display name/active targets/function classifications/steam id
        p = o + 128   # after licence md5, crc, timestamp, display name, targets, function classes, steam id, debugger port
        self.display_name = r.s(r.u32(o + 72 + 16 + 4 + 8))
        n = r.u32(p)
        self.room_order = [r.u32(p + 4 + 4 * i) for i in range(n)]

    def strings(self):
        r, (o, _) = self.r, self.chunk('STRG')
        self.strg = [r.s(p + 4) for p in r.plist(o)]
        self.strg_index = {}
        for i, p in enumerate(r.plist(o)):
            self.strg_index[p + 4] = i

    def textures(self):
        r, (o, _) = self.r, self.chunk('TXTR')
        self.txtr = []
        ptrs = r.plist(o)
        for i, p in enumerate(ptrs):
            scaled = r.u32(p)
            data = r.u32(p + 4)
            # PNG length: find IEND
            e = self.b.index(b'IEND', data) + 8
            self.txtr.append(Obj(scaled=scaled, off=data, png=self.b[data:e]))

    def tpag(self):
        r, (o, _) = self.r, self.chunk('TPAG')
        self.tpags = {}
        for p in r.plist(o):
            v = struct.unpack_from('<11H', self.b, p)
            self.tpags[p] = Obj(sx=v[0], sy=v[1], sw=v[2], sh=v[3], tx=v[4], ty=v[5], tw=v[6], th=v[7], bw=v[8],
                                bh=v[9], page=v[10])

    def sprites(self):
        r, (o, _) = self.r, self.chunk('SPRT')
        self.sprt = []
        for p in r.plist(o):
            name = r.s(r.u32(p))
            w, h, ml, mr, mb, mt = (r.i32(p + 4 + 4 * i) for i in range(6))
            transparent, smooth, preload, bboxmode, sepmasks, ox, oy = (r.i32(p + 28 + 4 * i) for i in range(7))
            q = p + 56
            if r.i32(q) == -1:
                raise NotImplementedError('GMS2 sprite format')
            tex = r.plist(q)
            q += 4 + 4 * len(tex)
            nmask = r.u32(q)
            q += 4
            masks = []
            ln = ((w + 7) // 8) * h
            for i in range(nmask):
                masks.append(self.b[q:q + ln])
                q += ln
            self.sprt.append(Obj(name=name, w=w, h=h, left=ml, right=mr, bottom=mb, top=mt, transparent=transparent,
                                 smooth=smooth, bboxmode=bboxmode, sepmasks=sepmasks, ox=ox, oy=oy,
                                 frames=[self.tpags.get(t) for t in tex], masks=masks))

    def backgrounds(self):
        r, (o, _) = self.r, self.chunk('BGND')
        self.bgnd = []
        for p in r.plist(o):
            self.bgnd.append(Obj(name=r.s(r.u32(p)), transparent=r.u32(p + 4), smooth=r.u32(p + 8),
                                 tex=self.tpags.get(r.u32(p + 16))))

    def sounds(self):
        r, (o, _) = self.r, self.chunk('SOND')
        self.sond = []
        for p in r.plist(o):
            self.sond.append(Obj(name=r.s(r.u32(p)), flags=r.u32(p + 4), type=r.s(r.u32(p + 8)),
                                 file=r.s(r.u32(p + 12)), effects=r.u32(p + 16), volume=r.f32(p + 20),
                                 pitch=r.f32(p + 24), group=r.i32(p + 28), audio=r.i32(p + 32)))
        o, _ = self.chunk('AUDO')
        self.audo = []
        for p in r.plist(o):
            n = r.u32(p)
            self.audo.append(self.b[p + 4:p + 4 + n])

    def fonts(self):
        r, (o, _) = self.r, self.chunk('FONT')
        self.font = []
        for p in r.plist(o):
            name = r.s(r.u32(p))
            disp = r.s(r.u32(p + 4))
            size = r.i32(p + 8)
            bold, italic = r.u32(p + 12), r.u32(p + 16)
            rs = r.u16(p + 20)
            rend = r.u32(p + 24)
            tex = self.tpags.get(r.u32(p + 28))
            sx, sy = r.f32(p + 32), r.f32(p + 36)
            glyphs = []
            for g in r.plist(p + 40):
                ch, x, y, w, h = struct.unpack_from('<5H', self.b, g)
                shift, off = struct.unpack_from('<2h', self.b, g + 10)
                glyphs.append(Obj(ch=ch, x=x, y=y, w=w, h=h, shift=shift, off=off))
            self.font.append(Obj(name=name, disp=disp, size=size, bold=bold, italic=italic, first=rs, last=rend,
                                 tex=tex, sx=sx, sy=sy, glyphs=glyphs))

    def objects(self):
        r, (o, _) = self.r, self.chunk('OBJT')
        self.objt = []
        for p in r.plist(o):
            name = r.s(r.u32(p))
            sprite, visible, solid, depth, persistent, parent, mask = (r.i32(p + 4 + 4 * i) for i in range(7))
            nverts = r.i32(p + 4 * 16)
            q = p + 4 * 20 + nverts * 8
            events = []
            for et in r.plist(q):
                lst = []
                for ep in r.plist(et):
                    sub = r.u32(ep)
                    codes = []
                    for ap in r.plist(ep + 4):
                        codes.append(r.i32(ap + 32))
                    lst.append((sub, codes))
                events.append(lst)
            self.objt.append(Obj(name=name, sprite=sprite, visible=visible, solid=solid, depth=depth,
                                 persistent=persistent, parent=parent, mask=mask, events=events))

    def rooms(self):
        r, (o, _) = self.r, self.chunk('ROOM')
        self.room = []
        for p in r.plist(o):
            name = r.s(r.u32(p))
            caption = r.s(r.u32(p + 4))
            w, h, speed, persistent, colour, drawcol, ccode, flags = (r.i32(p + 8 + 4 * i) for i in range(8))
            bgs = []
            for b in r.plist(r.u32(p + 40)):
                v = struct.unpack_from('<9i', self.b, b)
                bgs.append(Obj(enabled=v[0], fore=v[1], bg=v[2], x=v[3], y=v[4], tilex=v[5], tiley=v[6], hspeed=v[7],
                               vspeed=v[8], stretch=r.i32(b + 36)))
            views = []
            for v_ in r.plist(r.u32(p + 44)):
                v = struct.unpack_from('<14i', self.b, v_)
                views.append(Obj(enabled=v[0], x=v[1], y=v[2], w=v[3], h=v[4], px=v[5], py=v[6], pw=v[7], ph=v[8],
                                 bx=v[9], by=v[10], sx=v[11], sy=v[12], obj=v[13]))
            insts = []
            for g in r.plist(r.u32(p + 48)):
                x, y, obj, iid, cc = struct.unpack_from('<5i', self.b, g)
                sx, sy = struct.unpack_from('<2f', self.b, g + 20)
                col = r.u32(g + 28)
                rot = r.f32(g + 32)
                insts.append(Obj(x=x, y=y, obj=obj, id=iid, code=cc, sx=sx, sy=sy, color=col, rot=rot))
            tiles = []
            for t in r.plist(r.u32(p + 52)):
                v = struct.unpack_from('<9i', self.b, t)
                sx, sy = struct.unpack_from('<2f', self.b, t + 36)
                col = r.u32(t + 44)
                tiles.append(Obj(x=v[0], y=v[1], bg=v[2], sx_=v[3], sy_=v[4], w=v[5], h=v[6], depth=v[7], id=v[8],
                                 scx=sx, scy=sy, color=col))
            self.room.append(Obj(name=name, caption=caption, w=w, h=h, speed=speed, persistent=persistent,
                                 colour=colour, drawcol=drawcol, code=ccode, flags=flags, bgs=bgs, views=views,
                                 insts=insts, tiles=tiles))

    def scripts(self):
        r, (o, _) = self.r, self.chunk('SCPT')
        self.scpt = [Obj(name=r.s(r.u32(p)), code=r.i32(p + 4)) for p in r.plist(o)]

    def code(self):
        """CODE / VARI / FUNC. Empty in YYC builds."""
        r, (o, ln) = self.r, self.chunk('CODE')
        self.codes = []
        self.vari = []
        self.func = []
        if ln == 0:
            return
        for p in r.plist(o):
            name = r.s(r.u32(p))
            length = r.u32(p + 4)
            if self.bytecode <= 14:
                self.codes.append(Obj(name=name, length=length, start=p + 8, locals=0, args=0))
                continue
            nloc, nargs = r.u16(p + 8), r.u16(p + 10)
            rel = r.i32(p + 12)
            start = p + 12 + rel
            boff = r.u32(p + 16)
            self.codes.append(Obj(name=name, length=length, start=start, locals=nloc, args=nargs & 0x7fff,
                                  boff=boff))
        o, ln = self.chunk('VARI')
        if self.bytecode <= 14:
            q, step = o, 12
        else:
            self.vari_counts = (r.u32(o), r.u32(o + 4), r.u32(o + 8))
            q, step = o + 12, 20
        while q + step <= o + ln:
            if self.bytecode <= 14:
                self.vari.append(Obj(name=r.s(r.u32(q)), itype=-6, vid=-6, n=r.u32(q + 4), first=r.i32(q + 8)))
            else:
                self.vari.append(Obj(name=r.s(r.u32(q)), itype=r.i32(q + 4), vid=r.i32(q + 8), n=r.u32(q + 12),
                                     first=r.i32(q + 16)))
            q += step
        o, ln = self.chunk('FUNC')
        if self.bytecode <= 14:
            q, end = o, o + ln
            while q + 12 <= end:
                self.func.append(Obj(name=r.s(r.u32(q)), n=r.u32(q + 4), first=r.i32(q + 8)))
                q += 12
        else:
            n = r.u32(o)
            for i in range(n):
                q = o + 4 + 12 * i
                self.func.append(Obj(name=r.s(r.u32(q)), n=r.u32(q + 4), first=r.i32(q + 8)))
            q = o + 4 + 12 * n
            n = r.u32(q)
            q += 4
            self.code_locals = {}
            for i in range(n):
                cnt = r.u32(q)
                cname = r.s(r.u32(q + 4))
                q += 8
                lst = []
                for j in range(cnt):
                    lst.append((r.u32(q), r.s(r.u32(q + 4))))
                    q += 8
                self.code_locals[cname] = lst


if __name__ == '__main__':
    import sys
    d = GMData(sys.argv[1])
    print('name', d.name, d.display_name, 'bytecode', d.bytecode, 'ver', d.major, d.minor, d.release, d.build,
          'size', d.def_w, d.def_h)
    for k, (o, ln) in d.chunks.items():
        print(' ', k, ln)
    print('textures', len(d.txtr), 'tpag', len(d.tpags), 'sprites', len(d.sprt), 'bgs', len(d.bgnd), 'sounds',
          len(d.sond), 'audo', len(d.audo), 'fonts', len(d.font), 'objects', len(d.objt), 'rooms', len(d.room),
          'scripts', len(d.scpt), 'codes', len(d.codes), 'vari', len(d.vari), 'func', len(d.func))
    print('room order', [d.room[i].name for i in d.room_order][:20])
