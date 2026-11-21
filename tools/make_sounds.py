#!/usr/bin/env python3
"""Synthesises the game's sound effects into Resources/Sounds (16-bit mono WAV,
22050 Hz). Everything is made from noise and tones here, so there are no
third-party recordings. Run from the repository root: python3 tools/make_sounds.py
Variants of a sound are name_1.wav, name_2.wav, ...; the game picks one at random."""
import math, os, random, struct, wave

RATE = 22050
OUT = os.path.join(os.path.dirname(__file__), "..", "Resources", "Sounds")


def save(name, samples):
    os.makedirs(OUT, exist_ok=True)
    peak = max(1e-6, max(abs(s) for s in samples))
    scale = 0.85 / peak if peak > 0.85 else 1.0
    with wave.open(os.path.join(OUT, name + ".wav"), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(max(-1.0, min(1.0, s * scale)) * 32767)) for s in samples))


def noise(n, rng):
    return [rng.uniform(-1.0, 1.0) for _ in range(n)]


def lowpass(x, cutoff):
    # One pole: a softer, duller sound the lower the cutoff (Hz).
    a = math.exp(-2.0 * math.pi * cutoff / RATE)
    y, out = 0.0, []
    for s in x:
        y = (1 - a) * s + a * y
        out.append(y)
    return out


def highpass(x, cutoff):
    lp = lowpass(x, cutoff)
    return [s - l for s, l in zip(x, lp)]


def envelope(x, attack, decay, curve=3.0):
    n = len(x)
    out = []
    for i, s in enumerate(x):
        t = i / RATE
        e = min(1.0, t / attack) if attack > 0 else 1.0
        e *= max(0.0, 1.0 - t / decay) ** curve if t < decay else 0.0
        out.append(s * e)
    return out


def secs(t):
    return int(t * RATE)


def mix(*parts):
    n = max(len(p) for p in parts)
    return [sum(p[i] for p in parts if i < len(p)) for i in range(n)]


def tone(freq_from, freq_to, length, wave_shape="sine"):
    out, phase = [], 0.0
    n = secs(length)
    for i in range(n):
        f = freq_from + (freq_to - freq_from) * i / n
        phase += 2 * math.pi * f / RATE
        s = math.sin(phase)
        if wave_shape == "saw":
            s = 2.0 * ((phase / (2 * math.pi)) % 1.0) - 1.0
        out.append(s)
    return out


def main():
    rng = random.Random(1234)
    # Footsteps: soft (grass, dirt, leaves), hard (stone, bricks), sand (sand, snow), wood.
    for v in range(1, 4):
        save(f"step_soft_{v}", envelope(lowpass(noise(secs(0.12), rng), 700 + 150 * v), 0.004, 0.11, 2.0))
        save(f"step_hard_{v}", envelope(mix(highpass(noise(secs(0.07), rng), 1500), [0.6 * s for s in lowpass(noise(secs(0.07), rng), 400)]), 0.001, 0.06, 4.0))
        save(f"step_sand_{v}", envelope(highpass(lowpass(noise(secs(0.16), rng), 3500), 800), 0.02, 0.15, 1.5))
        save(f"step_wood_{v}", envelope(mix([0.6 * s for s in tone(180 + 20 * v, 140, 0.09)], lowpass(noise(secs(0.09), rng), 1200)), 0.002, 0.08, 3.0))
    # Mining: a tap on the block each fraction of a second.
    for v in range(1, 3):
        save(f"dig_soft_{v}", envelope(lowpass(noise(secs(0.08), rng), 900), 0.002, 0.07, 3.0))
        save(f"dig_hard_{v}", envelope(mix(highpass(noise(secs(0.06), rng), 2500), [0.5 * s for s in tone(900 + 80 * v, 700, 0.06)]), 0.0005, 0.05, 4.0))
    # Breaking: a crumble, a crack, a knock, a shatter.
    save("break_soft", envelope(lowpass(noise(secs(0.35), rng), 600), 0.005, 0.33, 2.0))
    save("break_hard", envelope(mix(highpass(noise(secs(0.3), rng), 1200), lowpass(noise(secs(0.3), rng), 300)), 0.001, 0.28, 3.0))
    save("break_wood", envelope(mix([0.7 * s for s in tone(220, 120, 0.25)], lowpass(noise(secs(0.25), rng), 1500)), 0.001, 0.24, 2.5))
    shards = [0.0] * secs(0.5)
    for _ in range(40):
        start = rng.randint(0, secs(0.35))
        f = rng.uniform(2500, 6000)
        for i, s in enumerate(envelope(tone(f, f * 0.9, 0.05), 0.0, 0.05, 2.0)):
            if start + i < len(shards):
                shards[start + i] += s * rng.uniform(0.2, 0.6)
    save("break_glass", mix(shards, envelope(highpass(noise(secs(0.5), rng), 4000), 0.0, 0.3, 3.0)))
    # Placing: a thud.
    save("place", envelope(mix([0.8 * s for s in tone(120, 70, 0.12)], lowpass(noise(secs(0.12), rng), 500)), 0.002, 0.11, 3.0))
    # Picking up pebbles, crafting.
    save("pickup", envelope(tone(500, 1100, 0.09), 0.003, 0.09, 2.0))
    save("craft", mix(envelope(tone(660, 660, 0.25), 0.005, 0.25, 2.0), [0] * secs(0.08) + envelope(tone(990, 990, 0.3), 0.005, 0.3, 2.0)))
    # Hurt: a low grunt.
    save("hurt", envelope(lowpass(mix(tone(220, 120, 0.25, "saw"), [0.3 * s for s in noise(secs(0.25), rng)]), 900), 0.01, 0.24, 1.5))
    # Hitting a creature.
    save("hit", envelope(mix(tone(160, 60, 0.12), lowpass(noise(secs(0.12), rng), 800)), 0.001, 0.11, 3.0))
    # Splash into water.
    save("splash", envelope(mix(lowpass(noise(secs(0.6), rng), 2500), highpass(noise(secs(0.6), rng), 3000)), 0.01, 0.55, 2.0))
    # A ghoul's groan: a wavering, rasping low tone.
    for v in range(1, 3):
        n = secs(1.1)
        base = 85 + 15 * v
        groan, phase = [], 0.0
        for i in range(n):
            t = i / RATE
            f = base * (1.0 + 0.15 * math.sin(2 * math.pi * 3.0 * t)) * (1.0 - 0.25 * t)
            phase += 2 * math.pi * f / RATE
            groan.append(((phase / (2 * math.pi)) % 1.0) * 2 - 1)
        groan = lowpass(mix(groan, [0.4 * s for s in noise(n, rng)]), 700)
        save(f"groan_{v}", envelope(groan, 0.15, 1.1, 1.2))
    # Rain: two seconds of hiss that loops seamlessly (the tail is crossfaded into the head).
    n, fade = secs(2.0), secs(0.25)
    hiss = highpass(lowpass(noise(n + fade, rng), 5000), 600)
    drops = [0.0] * (n + fade)
    for _ in range(140):
        start = rng.randint(0, n + fade - secs(0.02))
        for i, s in enumerate(envelope(highpass(noise(secs(0.02), rng), 2000), 0.0, 0.02, 3.0)):
            drops[start + i] += s * rng.uniform(0.2, 0.7)
    rain = [0.5 * h + d for h, d in zip(hiss, drops)]
    loop = rain[:n]
    for i in range(fade):
        k = i / fade
        loop[i] = loop[i] * k + rain[n + i] * (1 - k)
    save("rain_loop", loop)

    # Eating: a few soft crunchy bites (its own random stream, so adding it didn't
    # change the sounds above).
    bite_rng = random.Random(51)
    bites = []
    for k in range(3):
        bite = envelope(mix(lowpass(noise(secs(0.07), bite_rng), 1800), [0.4 * s for s in highpass(noise(secs(0.07), bite_rng), 3000)]), 0.002, 0.065, 2.5)
        bites += bite + [0.0] * secs(0.05)
    save("eat", bites)
    print("sounds written to", os.path.normpath(OUT))


if __name__ == "__main__":
    main()
