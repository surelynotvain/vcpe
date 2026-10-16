"""Tracks for vcpe_stream (src/vcpe_stream.h): any audio file -> an intro segment and a loop segment of IMA-ADPCM
blocks (1024 samples, the vcpe_audio block layout), each starting on a block boundary.

    from vcpe_stream import StreamPak
    sp = StreamPak(adpcm_tool(build_dir))
    i = sp.add('Audio/BGM/Town', 'Town.ogg', rate=32000, channels=2, loop=True)   # loop tags are honoured
    sp.write('MUSIC.PAK', header=b'...')    # then sp.c_table('name') for the VcpeStreamTrack array

Loop points: the LOOPSTART / LOOPLENGTH comments (samples at the file's own rate, as RPG Maker and many engines use
them) or loop=(start_s, length_s) in seconds; loop=True without tags loops the whole file, loop=False plays it once."""
import json
import subprocess

BLOCK = 1024


def probe(path):
    out = subprocess.run(['ffprobe', '-v', 'error', '-show_entries',
                          'stream=sample_rate,channels:stream_tags:format_tags', '-of', 'json', path],
                         capture_output=True, text=True).stdout
    j = json.loads(out or '{}')
    tags = {}
    for s in j.get('streams', []):
        tags.update({k.upper(): v for k, v in (s.get('tags') or {}).items()})
    tags.update({k.upper(): v for k, v in ((j.get('format') or {}).get('tags') or {}).items()})
    st = (j.get('streams') or [{}])[0]
    return int(st.get('sample_rate') or 44100), tags


def pcm(path, rate, ch):
    return subprocess.run(['ffmpeg', '-v', 'error', '-i', path, '-ac', str(ch), '-ar', str(rate), '-f', 's16le', '-'],
                          capture_output=True, check=True).stdout


def encode_blocks(adpcm, raw, ch):
    """PCM bytes -> ADPCM blocks, padded with silence to whole blocks; returns (bytes, samples)."""
    n = len(raw) // (2 * ch)
    pad = (-n) % BLOCK
    raw = raw + b'\0' * (pad * 2 * ch)
    enc = subprocess.run([adpcm, str(ch)], input=raw, capture_output=True, check=True).stdout
    assert len(enc) == (n + pad) // BLOCK * ch * (4 + BLOCK // 2), 'encoder block size'
    return enc, n


def encode_track(adpcm, path, rate, ch, loop):
    """-> (intro_bytes, intro_samples, loop_bytes, loop_samples)"""
    src_rate, tags = probe(path)
    raw = pcm(path, rate, ch)
    total = len(raw) // (2 * ch)
    start = length = None
    if loop is True or loop is None:
        if 'LOOPSTART' in tags:
            try:
                start = int(int(tags['LOOPSTART']) * rate / src_rate)
                if 'LOOPLENGTH' in tags:
                    length = int(int(tags['LOOPLENGTH']) * rate / src_rate)
            except ValueError:
                start = None
        if start is None:
            start = 0
    elif isinstance(loop, tuple):
        start, length = int(loop[0] * rate), (int(loop[1] * rate) if loop[1] else None)
    if start is None:   # no loop
        b, n = encode_blocks(adpcm, raw, ch)
        return b, n, b'', 0
    start = max(0, min(start, total))
    end = total if length is None else max(start + 1, min(total, start + length))
    fs = 2 * ch
    ib, isamp = encode_blocks(adpcm, raw[:start * fs], ch) if start else (b'', 0)
    lb, lsamp = encode_blocks(adpcm, raw[start * fs:end * fs], ch)
    return ib, isamp, lb, lsamp


class StreamPak:
    def __init__(self, adpcm, base=64):
        self.adpcm, self.data, self.tracks, self.names, self.base = adpcm, bytearray(), [], [], base

    def add(self, name, path, rate=32000, channels=2, loop=True, cached=None):
        """cached: (intro, isamp, loopbytes, lsamp) from an earlier run, to skip encoding"""
        ib, isamp, lb, lsamp = cached or encode_track(self.adpcm, path, rate, channels, loop)
        io = self.base + len(self.data)
        self.data += ib
        lo = self.base + len(self.data)
        self.data += lb
        self.tracks.append((io, isamp, lo, lsamp, rate, channels))
        self.names.append(name)
        return len(self.tracks) - 1

    def c_rows(self):
        return ['    {%du, %du, %du, %du, %d, %d, 0},' % t for t in self.tracks]
