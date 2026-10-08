import importlib.util
import struct
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('vcpe_tga', Path(__file__).resolve().parents[1] / 'tools/vcpe_tga.py')
tga = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tga)


def header(kind, width=2, height=1, bits=8, origin=32, cmap=0, first=0, count=0, palbits=0):
    return struct.pack('<BBBHHBHHHHBB', 0, cmap, kind, first, count, palbits, 0, 0, width, height, bits, origin)


class TgaTests(unittest.TestCase):
    def read(self, data):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'test.tga'
            path.write_bytes(data)
            return tga.load(path)

    def test_rgba_palette_rle_and_nonzero_base(self):
        data = header(9, cmap=1, first=5, count=2, palbits=32)
        data += bytes((30, 20, 10, 0, 60, 50, 40, 255, 1, 5, 6))
        self.assertEqual(self.read(data).tobytes(), bytes((10, 20, 30, 0, 40, 50, 60, 255)))

    def test_all_origins(self):
        for origin in (0, 16, 32, 48):
            pixels = [1, 2, 3, 4]
            if not origin & 32: pixels = pixels[2:] + pixels[:2]
            if origin & 16: pixels = [pixels[1], pixels[0], pixels[3], pixels[2]]
            image = self.read(header(3, 2, 2, origin=origin) + bytes(pixels))
            self.assertEqual(list(image.tobytes()[::4]), [1, 2, 3, 4])

    def test_true_colour_rle(self):
        image = self.read(header(10, bits=24) + bytes((129, 3, 2, 1)))
        self.assertEqual(image.tobytes(), bytes((1, 2, 3, 255)) * 2)

    def test_sixteen_bit_indices_and_palette(self):
        data = header(1, bits=16, cmap=1, first=300, count=2, palbits=16)
        data += struct.pack('<4H', 0xfc00, 0x03e0, 300, 301)
        self.assertEqual(self.read(data).tobytes(), bytes((255, 0, 0, 255, 0, 255, 0, 0)))

    def test_uncompressed_colour_and_alpha(self):
        data = header(2, bits=32, origin=40) + bytes((3, 2, 1, 64, 6, 5, 4, 255))
        self.assertEqual(self.read(data).tobytes(), bytes((1, 2, 3, 64, 4, 5, 6, 255)))
        data = header(2, bits=16, origin=33) + struct.pack('<2H', 0x801f, 0x7c00)
        self.assertEqual(self.read(data).tobytes(), bytes((0, 0, 255, 255, 255, 0, 0, 0)))

    def test_greyscale_rle_and_alpha(self):
        data = header(11, bits=16) + bytes((129, 75, 128))
        self.assertEqual(self.read(data).tobytes(), bytes((75, 75, 75, 128)) * 2)

    def test_invalid_headers(self):
        for data in (header(1), header(2, bits=8), header(3, width=0),
                     header(3, origin=96), header(1, cmap=1, count=1, palbits=8)):
            with self.assertRaises(ValueError): self.read(data)

    def test_bad_packets(self):
        for data in (b'', header(10, bits=24) + bytes((129, 3)),
                     header(10, bits=24) + bytes((130, 3, 2, 1)),
                     header(1, cmap=1, count=1, palbits=24) + bytes((0,0,0,1,1))):
            with self.assertRaises(ValueError): self.read(data)


if __name__ == '__main__': unittest.main()
