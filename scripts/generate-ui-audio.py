#!/usr/bin/env python3
"""Synthesize the original, soft glass UI cues using only Python's standard library."""
import argparse
import math
from pathlib import Path
import random
import struct
import wave


ROOT = Path(__file__).resolve().parents[1]
SAMPLE_RATE = 48000


def glass(samples, start, frequency, duration, gain, brightness=0.25, bend=0.0):
    """A rounded mallet attack, fading FM shimmer and a quiet detuned partial."""
    offset = round(start * SAMPLE_RATE)
    length = round(duration * SAMPLE_RATE)
    for index in range(min(length, len(samples) - offset)):
        time = index / SAMPLE_RATE
        progress = index / max(1, length - 1)
        attack = 1.0 - math.exp(-time / 0.003)
        release = (0.5 + 0.5 * math.cos(math.pi * progress)) ** 2
        envelope = attack * math.exp(-time / (duration * 0.30)) * release
        # Integrate the pitch glide so its phase remains continuous.
        phase = math.tau * frequency * (time + bend * time * time / (2 * duration))
        shimmer = brightness * math.exp(-time / 0.025) * math.sin(phase * 2.01)
        tone = math.sin(phase + shimmer)
        tone += 0.12 * math.sin(phase * 2.003) * math.exp(-time / 0.045)
        tone += 0.06 * math.sin(phase * 3.997) * math.exp(-time / 0.018)
        samples[offset + index] += gain * envelope * tone


def air(samples, duration, gain, seed):
    """A restrained filtered breath under transitions, rather than a sharp click."""
    rng = random.Random(seed)
    low = 0.0
    previous = 0.0
    length = min(len(samples), round(duration * SAMPLE_RATE))
    for index in range(length):
        low += 0.12 * (rng.uniform(-1.0, 1.0) - low)
        breath = low - previous
        previous += 0.025 * (low - previous)
        envelope = math.sin(math.pi * index / max(1, length - 1)) ** 2
        samples[index] += gain * breath * envelope


def finish(samples, peak, room):
    dry = samples[:]
    # Sparse early reflections keep the timbre airy without a long reverb tail.
    for delay, gain in ((0.019, room), (0.037, room * 0.55), (0.061, room * 0.25)):
        offset = round(delay * SAMPLE_RATE)
        for index in range(offset, len(samples)):
            samples[index] += dry[index - offset] * gain
    fade = round(0.008 * SAMPLE_RATE)
    for index in range(fade):
        samples[index] *= 0.5 - 0.5 * math.cos(math.pi * index / (fade - 1))
        samples[-1 - index] *= 0.5 - 0.5 * math.cos(math.pi * index / (fade - 1))
    scale = peak / max(abs(value) for value in samples)
    return b"".join(struct.pack("<h", round(value * scale * 32767)) for value in samples)


def render_cues():
    # Notes share a D/A/E palette. Frequent actions stay short and quieter;
    # rising/falling contours distinguish confirmation from dismissal.
    # Voice fields: onset, frequency, duration, gain, brightness, pitch bend.
    cues = (
        ("01-navegacion-alternativa.wav", 0.085, 0.20, 0.06, 0.010, (
            (0.000, 880.00, 0.075, 1.00, 0.22, -0.035),
            (0.002, 440.00, 0.060, 0.18, 0.08, 0.000),
        )),
        ("02-cambio-tab.wav", 0.165, 0.23, 0.10, 0.055, (
            (0.000, 587.33, 0.135, 0.80, 0.16, 0.055),
            (0.025, 880.00, 0.125, 0.32, 0.20, 0.015),
        )),
        ("03-entrar.wav", 0.260, 0.29, 0.12, 0.018, (
            (0.000, 587.33, 0.170, 0.72, 0.30, 0.000),
            (0.055, 880.00, 0.190, 1.00, 0.25, 0.000),
            (0.058, 1174.66, 0.170, 0.15, 0.12, 0.000),
        )),
        ("04-volver-salir.wav", 0.195, 0.23, 0.08, 0.024, (
            (0.000, 659.25, 0.120, 0.68, 0.12, -0.025),
            (0.045, 440.00, 0.140, 0.85, 0.10, -0.035),
        )),
        ("05-iniciar-descarga.wav", 0.320, 0.30, 0.13, 0.035, (
            (0.000, 440.00, 0.170, 0.65, 0.16, 0.000),
            (0.055, 659.25, 0.190, 0.78, 0.20, 0.000),
            (0.110, 880.00, 0.195, 0.90, 0.24, 0.000),
        )),
        ("06-descarga-completa.wav", 0.560, 0.36, 0.16, 0.012, (
            (0.000, 587.33, 0.300, 0.65, 0.24, 0.000),
            (0.070, 880.00, 0.320, 0.74, 0.22, 0.000),
            (0.140, 1174.66, 0.390, 0.85, 0.28, 0.000),
            (0.145, 739.99, 0.365, 0.30, 0.12, 0.000),
        )),
    )
    for seed, (filename, duration, peak, room, breath, voices) in enumerate(cues):
        samples = [0.0] * round(duration * SAMPLE_RATE)
        for voice in voices:
            glass(samples, *voice)
        air(samples, min(duration * 0.7, 0.15), breath, seed)
        yield filename, finish(samples, peak, room)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=ROOT / "assets/audio")
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    for filename, pcm in render_cues():
        path = args.output_dir / filename
        with wave.open(str(path), "wb") as output:
            output.setnchannels(1)
            output.setsampwidth(2)
            output.setframerate(SAMPLE_RATE)
            output.writeframes(pcm)
        print(f"{filename}: {len(pcm) / (2 * SAMPLE_RATE):.3f}s, mono PCM16, {SAMPLE_RATE} Hz")


if __name__ == "__main__":
    main()
