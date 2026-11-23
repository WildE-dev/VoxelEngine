"""Turns generated sound effects (MP3, from Scenario's ElevenLabs Sound Effects model)
into the game's sounds: 16-bit mono WAV in Resources/Sounds, named as the game plays
them ("step_soft_2", "hum_drill"...).

One-shot sounds lose the silence before and after them, are cut to at most their
`max` length with a short fade, and are brought to a common peak so they sit together.
Loops ("hum_*", "rain_loop") are kept whole, so their seamless loop point survives,
and only levelled.

Needs ffmpeg (to decode the MP3s). Run from the repository root:
    python tools/convert_sounds.py <folder of name.mp3 files>
"""
import array
import os
import subprocess
import sys
import wave

RATE = 44100
OUT = os.path.join(os.path.dirname(__file__), "..", "Resources", "Sounds")

# Longest a one-shot may run, in seconds (by name prefix; the first match wins).
MAX_LENGTH = [
    ("step_", 0.28), ("dig_", 0.25), ("place", 0.3), ("pickup", 0.3), ("hit", 0.3),
    ("hurt", 0.45), ("bow", 0.45), ("break_", 0.6), ("grunt", 0.6), ("eat", 0.6),
    ("craft", 0.7), ("piston", 0.6), ("splash", 0.8), ("break_glass", 0.8),
]
# Peak level for each kind (0..1): footsteps and digging a little quieter than the rest.
PEAK = [("step_", 0.55), ("dig_", 0.6), ("hum_", 0.7), ("rain_loop", 0.6), ("crickets", 0.6), ("birds", 0.6)]
DEFAULT_PEAK = 0.8


def decode(path):
    """The file's samples as 16-bit mono at RATE."""
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-f", "s16le", "-ac", "1", "-ar", str(RATE), "-"],
                         check=True, capture_output=True).stdout
    samples = array.array("h")
    samples.frombytes(raw)
    return samples


def lookup(table, name, default):
    for prefix, value in table:
        if name.startswith(prefix):
            return value
    return default


def trim(samples, start_threshold, end_threshold):
    """From just before the first sample above `start_threshold` to the last above
    `end_threshold` (lower, so quiet tails such as a chime's ring or thunder's rumble stay)."""
    first = next((i for i, s in enumerate(samples) if abs(s) > start_threshold), 0)
    last = next((i for i in range(len(samples) - 1, -1, -1) if abs(samples[i]) > end_threshold), len(samples) - 1)
    return samples[max(0, first - int(0.003 * RATE)):last + 1]


def process(name, samples):
    loop = name.startswith("hum_") or name == "rain_loop"
    floats = [s / 32768.0 for s in samples]
    if not loop:
        peak = max((abs(s) for s in floats), default=0.0)
        kept = trim(samples, peak * 32768.0 * 0.06, peak * 32768.0 * 0.01)
        floats = [s / 32768.0 for s in kept]
        limit = lookup(MAX_LENGTH, name, None)
        if limit is not None:
            floats = floats[:int(limit * RATE)]
        # A 2 ms fade in (no click at the cut) and a fade out over the last fifth (at most 60 ms).
        fade_in = min(len(floats), int(0.002 * RATE))
        for i in range(fade_in):
            floats[i] *= i / fade_in
        fade_out = min(len(floats), max(1, min(int(0.06 * RATE), len(floats) // 5)))
        for i in range(fade_out):
            floats[len(floats) - 1 - i] *= i / fade_out
    peak = max((abs(s) for s in floats), default=0.0)
    gain = lookup(PEAK, name, DEFAULT_PEAK) / peak if peak > 0 else 1.0
    return array.array("h", (max(-32767, min(32767, int(s * gain * 32767))) for s in floats))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    source = sys.argv[1]
    os.makedirs(OUT, exist_ok=True)
    for file in sorted(os.listdir(source)):
        if not file.endswith(".mp3"):
            continue
        name = file[:-4]
        samples = process(name, decode(os.path.join(source, file)))
        with wave.open(os.path.join(OUT, name + ".wav"), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(RATE)
            w.writeframes(samples.tobytes())
        print(f"{name}: {len(samples) / RATE:.2f} s")
    print("sounds written to", os.path.normpath(OUT))


if __name__ == "__main__":
    main()
