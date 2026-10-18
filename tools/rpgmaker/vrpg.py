"""RPG Maker VX Ace data -> VCPE RPG documents ("VRPG"), the format rpg/vcpe_rpg_data.c reads.

    from vrpg import convert_db, convert_map
    db_bytes = convert_db('Game/Data')              # Actors .. MapInfos, System, CommonEvents in one document
    map_bytes = convert_map('Game/Data/Map001.rvdata2')

A document is:
    header   'VRPG', u32 version, u32 ntables, u32 strings_off, u32 strings_size, u32 i16_off, u32 i16_count, u32 0
    tables   ntables x {u32 off, u32 count, u32 slots}   (RPG_T_* order, vrpg_schema.TABLES)
    data     each table: count records of slots x 32-bit little-endian values, 4-byte aligned
    strings  NUL-terminated UTF-8, offset 0 is ""
    i16      the int16 pool of every Ruby Table (map tiles, tileset flags, class params, animation cells)

Database tables keep RPG Maker's ids as record indices (record 0 is the unused nil entry). Event command parameters
are flattened into the Param table in their original order: nil/false 0, true 1, integers as is, floats truncated,
strings as string offsets, arrays as a count followed by the items, audio files as name/volume/pitch, Tone/Color as 4
ints, a MoveRoute as its index in the Route table. The runtime knows each command's parameter layout (it is VX Ace's).
Nothing here is game-specific: any VX Ace project converts the same way."""
import os
import struct
import glob

from vrpg_schema import SCHEMA, TABLES, DB_ROOTS, fields, record_slots, field_slots

import importlib.util as _ilu
_spec = _ilu.spec_from_file_location('rpg_marshal', os.path.join(os.path.dirname(__file__), 'marshal.py'))
_m = _ilu.module_from_spec(_spec)
_spec.loader.exec_module(_m)
RubyObject, load = _m.RubyObject, _m.load

VERSION = 1


class Doc:
    def __init__(self):
        self.tables = {n: [] for n in TABLES}
        self.strings = bytearray(b'\0')
        self.string_ids = {b'': 0}
        self.i16 = []

    # --- pools
    def string(self, v):
        if v is None:
            return 0
        if isinstance(v, str):
            v = v.encode('utf-8')
        if not isinstance(v, (bytes, bytearray)):
            v = str(v).encode()
        v = bytes(v).replace(b'\0', b'')
        o = self.string_ids.get(v)
        if o is None:
            o = len(self.strings)
            self.strings += v + b'\0'
            self.string_ids[v] = o
        return o

    def table16(self, t):
        """Ruby Table -> (first, count, x, y, z) in the int16 pool."""
        if t is None:
            return [0, 0, 0, 0, 0]
        b = t.attrs['__bytes__']
        _dim, x, y, z, n = struct.unpack_from('<5i', b)
        vals = struct.unpack_from('<%dh' % n, b, 20)
        first = len(self.i16)
        self.i16.extend(vals)
        return [first, n, x, y, z]

    # --- records
    def add(self, table, obj):
        """Encode obj (RubyObject / dict / None) as one record of `table`; returns its index."""
        slots = self.encode(table, obj)
        self.tables[table].append(slots)
        return len(self.tables[table]) - 1

    def reserve(self, table, n):
        first = len(self.tables[table])
        self.tables[table].extend([None] * n)
        return first

    def encode(self, table, obj):
        out = []
        for f, kind in fields(table):
            v = None
            if isinstance(obj, RubyObject):
                v = obj.attrs.get('@' + f)
            elif isinstance(obj, dict):
                v = obj.get(f)
            out.extend(self.value(kind, v))
        assert len(out) == record_slots(table), table
        return out

    def value(self, kind, v):
        if kind == 'i':
            return [as_int(v)]
        if kind == 'f':
            return [('f', float(v or 0))]
        if kind == 's':
            return [self.string(v)]
        if kind == 'a':
            if v is None:
                return [0, 100, 100]
            return [self.string(v['name']), as_int(v['volume']), as_int(v['pitch'])]
        if kind == 'tone':
            if v is None:
                return [('f', 0.0)] * 4
            return [('f', x) for x in struct.unpack('<4d', v.attrs['__bytes__'])]
        if kind == 't16':
            return self.table16(v)
        if kind == 'route':
            return [self.route(v)]
        if kind == 'list':
            return self.cmd_list(v)
        if kind.startswith('sub:'):
            return self.encode(kind[4:], v)
        if kind.startswith('r:'):
            t = kind[2:]
            items = list(v or [])
            first = self.reserve(t, len(items))
            for i, it in enumerate(items):
                if t == 'Int':
                    rec = [as_int(it)]
                elif t == 'Str':
                    rec = [self.string(it)]
                else:
                    rec = self.encode(t, it)
                self.tables[t][first + i] = rec
            return [first, len(items)]
        if '*' in kind:
            t, n = kind.split('*')
            n = int(n)
            items = list(v or [])[:n]
            if t == 's':
                vals = [self.string(x) for x in items]
            else:
                vals = [as_int(x) for x in items]
            return vals + [0] * (n - len(vals))
        raise ValueError(kind)

    # --- event commands
    def params(self, values):
        flat = []
        for v in values:
            self.flatten(v, flat)
        first = len(self.tables['Param'])
        self.tables['Param'].extend([[x] for x in flat])
        return [first, len(flat)]

    def flatten(self, v, out):
        if v is None or v is False:
            out.append(0)
        elif v is True:
            out.append(1)
        elif isinstance(v, int):
            out.append(as_int(v))
        elif isinstance(v, float):
            out.append(int(v))
        elif isinstance(v, (bytes, bytearray, str)):
            out.append(self.string(v))
        elif isinstance(v, list):
            out.append(len(v))
            for x in v:
                self.flatten(x, out)
        elif isinstance(v, RubyObject):
            n = v.name
            if n in ('RPG::AudioFile', 'RPG::BGM', 'RPG::BGS', 'RPG::ME', 'RPG::SE'):
                out += [self.string(v['name']), as_int(v['volume']), as_int(v['pitch'])]
            elif n in ('Tone', 'Color'):
                out += [int(x) for x in struct.unpack('<4d', v.attrs['__bytes__'])]
            elif n == 'RPG::MoveRoute':
                out.append(self.route(v))
            elif n == 'RPG::MoveCommand':
                out.append(as_int(v['code']))   # 505 lines only echo the route for the editor
            else:
                raise ValueError('unknown parameter object ' + str(n))
        else:
            raise ValueError('unknown parameter ' + repr(v))

    def cmd_list(self, lst):
        lst = lst or []
        first = self.reserve('Cmd', len(lst))
        for i, c in enumerate(lst):
            p = self.params(c['parameters'] or [])
            self.tables['Cmd'][first + i] = [as_int(c['code']), as_int(c['indent'])] + p
        return [first, len(lst)]

    def route(self, r):
        if r is None:
            return -1
        cmds = r['list'] or []
        idx = self.reserve('Route', 1)
        first = self.reserve('MoveCmd', len(cmds))
        for i, c in enumerate(cmds):
            self.tables['MoveCmd'][first + i] = [as_int(c['code'])] + self.params(c['parameters'] or [])
        self.tables['Route'][idx] = [as_int(r['repeat']), as_int(r['skippable']), as_int(r['wait']), first, len(cmds)]
        return idx

    # --- output
    def bytes(self):
        n = len(TABLES)
        head = 32 + 12 * n
        body = bytearray()
        dirs = []
        for t in TABLES:
            rows = self.tables[t]
            slots = record_slots(t)
            off = head + len(body)
            for r in rows:
                if r is None:
                    r = [0] * slots
                for x in r:
                    if isinstance(x, tuple):
                        body += struct.pack('<f', x[1])
                    else:
                        body += struct.pack('<i', x)
            dirs.append((off, len(rows), slots))
        while len(body) % 4:
            body += b'\0'
        so = head + len(body)
        body += self.strings
        while len(body) % 4:
            body += b'\0'
        io = head + len(body)
        body += struct.pack('<%dh' % len(self.i16), *self.i16)
        out = bytearray(b'VRPG' + struct.pack('<7I', VERSION, n, so, len(self.strings), io, len(self.i16), 0))
        for d in dirs:
            out += struct.pack('<3I', *d)
        return bytes(out + body)


def as_int(v):
    if v is None or v is False:
        return 0
    if v is True:
        return 1
    if isinstance(v, float):
        return int(v)
    v = int(v)
    return max(-0x80000000, min(0x7fffffff, v))


def convert_db(data_dir):
    """The database document: every RPG:: database file of a VX Ace project."""
    doc = Doc()
    files = {'Actor': 'Actors', 'Class': 'Classes', 'Skill': 'Skills', 'Item': 'Items', 'Weapon': 'Weapons',
             'Armor': 'Armors', 'Enemy': 'Enemies', 'State': 'States', 'Troop': 'Troops', 'Animation': 'Animations',
             'Tileset': 'Tilesets', 'CommonEvent': 'CommonEvents'}
    for t in DB_ROOTS:
        if t in files:
            arr = load(os.path.join(data_dir, files[t] + '.rvdata2'))
            first = doc.reserve(t, len(arr))
            for i, o in enumerate(arr):
                doc.tables[t][first + i] = doc.encode(t, o)
        elif t == 'System':
            sysobj = load(os.path.join(data_dir, 'System.rvdata2'))
            doc.add('System', sysobj)
        elif t == 'MapInfo':
            infos = load(os.path.join(data_dir, 'MapInfos.rvdata2'))
            top = max(infos) if infos else 0
            first = doc.reserve('MapInfo', top + 1)
            for k, o in infos.items():
                rec = doc.encode('MapInfo', o)
                rec[0] = k
                doc.tables['MapInfo'][first + k] = rec
    return doc.bytes()


def convert_map(path):
    """One map document: the Map record (index 0) and its events sorted by id."""
    m = load(path)
    doc = Doc()
    evs = m['events'] or {}
    obj = {f: m[f] for f, _ in fields('Map')}
    obj['events'] = [evs[k] for k in sorted(evs)]
    doc.add('Map', obj)
    return doc.bytes()


def map_files(data_dir):
    return sorted(glob.glob(os.path.join(data_dir, 'Map[0-9][0-9][0-9].rvdata2')))


if __name__ == '__main__':
    import sys
    d = sys.argv[1]
    b = convert_db(d)
    print('database', len(b), 'bytes')
    tot = 0
    for p in map_files(d):
        tot += len(convert_map(p))
    print('maps', len(map_files(d)), tot, 'bytes')
