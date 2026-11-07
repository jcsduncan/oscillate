#!/usr/bin/env python3
"""Generate and verify the original, dependency-free starter one-shots."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import math
import struct
import wave
from pathlib import Path

RATE = 48_000
GENERATOR_REVISION = 1
ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "assets" / "starter"
MANIFEST = ASSETS / "manifest.json"

PARAMETERS = {
    "kick": {"duration_ms": 450, "target_peak": 0.65, "seed": 0x1051},
    "snare": {"duration_ms": 280, "target_peak": 0.58, "seed": 0x2052},
    "hat": {"duration_ms": 140, "target_peak": 0.42, "seed": 0x3053},
    "pluck": {"duration_ms": 700, "target_peak": 0.62, "seed": 0x4054},
}


class Noise:
    def __init__(self, seed: int) -> None:
        self.state = seed

    def next(self) -> float:
        self.state = (1_664_525 * self.state + 1_013_904_223) & 0xFFFF_FFFF
        return ((self.state >> 8) & 0xFF_FFFF) / 0x7F_FFFF - 1.0


def fade_end(value: float, index: int, frames: int, fade_frames: int = 480) -> float:
    return value * min(1.0, (frames - 1 - index) / fade_frames)


def kick(frames: int, seed: int) -> list[float]:
    noise = Noise(seed)
    phase = 0.0
    output = []
    for index in range(frames):
        t = index / RATE
        frequency = 47.0 + 153.0 * math.exp(-31.0 * t)
        phase += 2.0 * math.pi * frequency / RATE
        body = math.sin(phase) * math.exp(-10.0 * t)
        click = noise.next() * math.exp(-1_800.0 * t) * 0.12
        output.append(fade_end((body + click) * min(1.0, t * 2_000.0), index, frames))
    return output


def snare(frames: int, seed: int) -> list[float]:
    noise = Noise(seed)
    previous = 0.0
    output = []
    for index in range(frames):
        t = index / RATE
        current = noise.next()
        bright = current - previous * 0.82
        previous = current
        body = 0.27 * math.sin(2.0 * math.pi * 185.0 * t)
        output.append(fade_end((0.78 * bright + body) * math.exp(-17.0 * t), index, frames))
    return output


def hat(frames: int, seed: int) -> list[float]:
    noise = Noise(seed)
    previous = 0.0
    output = []
    for index in range(frames):
        t = index / RATE
        current = noise.next()
        bright = current - previous
        previous = current
        output.append(fade_end(bright * math.exp(-36.0 * t), index, frames, 240))
    return output


def pluck(frames: int, seed: int) -> list[float]:
    noise = Noise(seed)
    delay = [noise.next() for _ in range(round(RATE / 220.0))]
    output = []
    for index in range(frames):
        position = index % len(delay)
        next_position = (position + 1) % len(delay)
        current = delay[position]
        delay[position] = 0.9975 * (current + delay[next_position]) / 2.0
        output.append(fade_end(current, index, frames, 960))
    return output


SYNTHS = {"kick": kick, "snare": snare, "hat": hat, "pluck": pluck}


def wav_bytes(samples: list[float], target_peak: float) -> tuple[bytes, float, float]:
    if not samples or not all(math.isfinite(sample) for sample in samples):
        raise ValueError("generator produced non-finite or empty audio")
    raw_peak = max(abs(sample) for sample in samples)
    if raw_peak <= 0.0:
        raise ValueError("generator produced silence")
    scale = target_peak / raw_peak
    pcm = [max(-32_768, min(32_767, round(sample * scale * 32_767))) for sample in samples]
    decoded = [sample / 32_768.0 for sample in pcm]
    peak = max(abs(sample) for sample in decoded)
    rms = math.sqrt(sum(sample * sample for sample in decoded) / len(decoded))
    if not (0.005 < rms < 0.5 and 0.3 <= peak <= 0.7 and abs(decoded[-1]) <= 1 / 32_768):
        raise ValueError("starter sample fails loudness, peak or tail checks")
    payload = struct.pack(f"<{len(pcm)}h", *pcm)

    buffer = io.BytesIO()
    with wave.open(buffer, "wb") as writer:
        writer.setnchannels(1)
        writer.setsampwidth(2)
        writer.setframerate(RATE)
        writer.writeframes(payload)
    return buffer.getvalue(), peak, rms


def build() -> tuple[dict, dict[str, bytes]]:
    files = {}
    sounds = []
    for name, parameters in PARAMETERS.items():
        frames = RATE * parameters["duration_ms"] // 1_000
        generated = SYNTHS[name](frames, parameters["seed"])
        data, peak, rms = wav_bytes(generated, parameters["target_peak"])
        filename = f"{name}.wav"
        files[filename] = data
        sounds.append(
            {
                "name": name,
                "file": filename,
                "parameters": parameters,
                "format": {
                    "container": "RIFF/WAVE",
                    "encoding": "PCM16",
                    "channels": 1,
                    "sample_rate": RATE,
                    "frames": frames,
                },
                "bytes": len(data),
                "sha256": hashlib.sha256(data).hexdigest(),
                "decoded_peak": round(peak, 6),
                "decoded_rms": round(rms, 6),
            }
        )
    manifest = {
        "schema": 1,
        "generator": "scripts/generate-starter-sounds.py",
        "generator_revision": GENERATOR_REVISION,
        "generator_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "creation": "Original procedural synthesis from this repository's code; no external "
        "recordings, sample packs or sound libraries were used.",
        "sounds": sounds,
    }
    return manifest, files


def validate_wav(data: bytes, expected_frames: int) -> None:
    if len(data) != 44 + 2 * expected_frames or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("invalid WAV size or RIFF header")
    if struct.unpack_from("<I", data, 4)[0] != len(data) - 8:
        raise ValueError("invalid RIFF byte count")
    if data[12:16] != b"fmt " or data[36:40] != b"data":
        raise ValueError("missing canonical PCM chunks")
    if struct.unpack_from("<IHHIIHH", data, 16) != (16, 1, 1, RATE, RATE * 2, 2, 16):
        raise ValueError("unexpected sample format")
    if struct.unpack_from("<I", data, 40)[0] != 2 * expected_frames:
        raise ValueError("invalid data byte count")
    samples = struct.unpack_from(f"<{expected_frames}h", data, 44)
    peak = max(abs(sample) for sample in samples) / 32_768.0
    rms = math.sqrt(sum((sample / 32_768.0) ** 2 for sample in samples) / expected_frames)
    if not (0.3 <= peak <= 0.7 and 0.005 < rms < 0.5 and abs(samples[-1]) <= 1):
        raise ValueError("decoded audio violates headroom, audibility or tail limits")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="verify committed assets and manifest")
    args = parser.parse_args()
    manifest, files = build()
    manifest_bytes = (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode()
    if args.check:
        if MANIFEST.read_bytes() != manifest_bytes:
            raise SystemExit("starter manifest differs from deterministic regeneration")
        actual = {path.name for path in ASSETS.glob("*.wav")}
        if actual != set(files):
            raise SystemExit("starter WAV inventory differs from manifest")
        for sound in manifest["sounds"]:
            data = (ASSETS / sound["file"]).read_bytes()
            if data != files[sound["file"]] or hashlib.sha256(data).hexdigest() != sound["sha256"]:
                raise SystemExit(f"starter sample differs: {sound['file']}")
            validate_wav(data, sound["format"]["frames"])
        print(f"Validated {len(files)} starter sounds ({sum(map(len, files.values()))} bytes)")
        return
    ASSETS.mkdir(parents=True, exist_ok=True)
    for filename, data in files.items():
        (ASSETS / filename).write_bytes(data)
    MANIFEST.write_bytes(manifest_bytes)
    print(f"Generated {len(files)} starter sounds ({sum(map(len, files.values()))} bytes)")


if __name__ == "__main__":
    main()
