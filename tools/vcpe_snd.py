"""VCPE audio helpers for the converters: build the ADPCM encoder (adpcm_enc.c, the format
vcpe_audio.c plays), measure and encode WAVs (ffmpeg -> s16le -> ADPCM)."""
import os, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))


# The encoder is a small C program (adpcm_enc.c), compiled on demand with the host's gcc.
def adpcm_tool(build_dir):
    """Compile adpcm_enc into build_dir when missing or older than its source; returns the path."""
    exe = os.path.join(build_dir, "adpcm_enc")
    src = os.path.join(HERE, "adpcm_enc.c")
    if not os.path.exists(exe) or os.path.getmtime(exe) < os.path.getmtime(src):
        subprocess.check_call(["gcc", "-O2", "-o", exe, src])
    return exe


# Length of an audio file in seconds, from ffprobe.
def duration(wav):
    out = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", wav],
                         capture_output=True, text=True).stdout
    return float(out.strip() or 0)


# Decode any audio file with ffmpeg to raw 16-bit PCM at the wanted rate and channel count (22050 mono for sound
# effects, 44100 stereo for music), then pipe it through the ADPCM encoder.
def encode(adpcm, wav, rate, ch):
    """-> (samples, ADPCM bytes)"""
    pcm = subprocess.run(["ffmpeg", "-v", "error", "-i", wav, "-ac", str(ch), "-ar", str(rate),
                          "-f", "s16le", "-"], capture_output=True, check=True).stdout
    nsamp = len(pcm) // (2 * ch)
    enc = subprocess.run([adpcm, str(ch)], input=pcm, capture_output=True, check=True).stdout
    return nsamp, enc
