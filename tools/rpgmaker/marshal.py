"""Ruby Marshal 4.8 reader for RPG Maker data files (.rvdata2 / .rxdata / .rvdata). Plain Python, no Ruby needed.
Objects become RubyObject(name, attrs) with '@'-prefixed attribute names; user-defined dumps (Table, Color, Tone)
keep their raw bytes in attrs['__bytes__']. Strings stay bytes (RPG Maker stores UTF-8)."""


class RubyObject:
    def __init__(self, name, attrs=None):
        self.name, self.attrs = name, attrs or {}

    def __getitem__(self, k):
        return self.attrs.get('@' + k)

    def get(self, k, d=None):
        return self.attrs.get('@' + k, d)

    def __repr__(self):
        return f'{self.name}({self.attrs!r})'


class MarshalReader:
    def __init__(self, data):
        if data[:2] != b'\x04\x08':
            raise ValueError('not a Ruby Marshal 4.8 file')
        self.data, self.pos, self.symbols, self.objects = data, 2, [], []

    def take(self, n):
        b = self.data[self.pos:self.pos + n]
        if len(b) != n:
            raise ValueError('truncated Marshal data')
        self.pos += n
        return b

    def integer(self):
        n = int.from_bytes(self.take(1), 'little', signed=True)
        if n == 0:
            return 0
        if n > 4:
            return n - 5
        if n < -4:
            return n + 5
        b = self.take(abs(n))
        v = int.from_bytes(b, 'little')
        return v - (1 << (8 * abs(n))) if n < 0 else v

    def string(self):
        return self.take(self.integer())

    def track(self, x):
        self.objects.append(x)
        return x

    def read(self):
        t = self.take(1)
        if t == b'0':
            return None
        if t == b'T':
            return True
        if t == b'F':
            return False
        if t == b'i':
            return self.integer()
        if t == b':':
            s = self.string().decode('utf-8', 'replace')
            self.symbols.append(s)
            return s
        if t == b';':
            return self.symbols[self.integer()]
        if t == b'@':
            return self.objects[self.integer()]
        if t == b'"':
            return self.track(self.string())
        if t == b'[':
            a = self.track([])
            for _ in range(self.integer()):
                a.append(self.read())
            return a
        if t in (b'{', b'}'):
            h = self.track({})
            for _ in range(self.integer()):
                k = self.read()
                h[k] = self.read()
            if t == b'}':
                self.read()   # default value
            return h
        if t == b'o':
            o = self.track(RubyObject(None))
            o.name = self.read()
            for _ in range(self.integer()):
                k = self.read()
                o.attrs[k] = self.read()
            return o
        if t == b'u':
            name = self.read()
            return self.track(RubyObject(name, {'__bytes__': self.string()}))
        if t == b'U':
            o = self.track(RubyObject(None))
            o.name = self.read()
            o.attrs['__marshal__'] = self.read()
            return o
        if t == b'I':
            v = self.read()
            for _ in range(self.integer()):
                self.read()
                self.read()
            return v
        if t == b'f':
            s = self.string().split(b'\0')[0]
            return self.track(float({b'inf': 'inf', b'-inf': '-inf', b'nan': 'nan'}.get(s, s)))
        if t == b'l':
            sign = self.take(1)
            v = int.from_bytes(self.take(self.integer() * 2), 'little')
            return self.track(-v if sign == b'-' else v)
        if t == b'C':
            self.read()
            return self.read()
        if t == b'e':
            self.read()
            return self.read()
        raise ValueError(f'unsupported Marshal type {t!r} at {self.pos - 1}')


def load(path):
    with open(path, 'rb') as f:
        return MarshalReader(f.read()).read()
