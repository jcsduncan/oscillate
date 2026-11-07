#!/usr/bin/env python3
"""Build reproducible stereo audition material from the repository starter sounds.

The sounds are procedural one-shots, so these arrangements are generated
composites rather than natural recordings or evidence of listening quality.
"""

import argparse
import array
import sys
import wave
from pathlib import Path

RATE = 48_000
FRAMES = 8 * RATE


def read_one_shot(path: Path) -> list[float]:
    with wave.open(str(path), "rb") as audio:
        if (audio.getnchannels(), audio.getsampwidth(), audio.getframerate()) != (1, 2, RATE):
            raise ValueError(f"{path}: expected 48 kHz mono PCM16")
        samples = array.array("h")
        samples.frombytes(audio.readframes(audio.getnframes()))
    if sys.byteorder != "little":
        samples.byteswap()
    return [sample / 32768.0 for sample in samples]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kind", choices=("drums", "mix"), required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2] / "assets" / "starter"
    sounds = {
        name: read_one_shot(root / f"{name}.wav") for name in ("kick", "snare", "hat", "pluck")
    }
    left = [0.0] * FRAMES
    right = [0.0] * FRAMES

    def place(name: str, frame: int, gain: float, pan: float = 0.0) -> None:
        for offset, sample in enumerate(sounds[name]):
            if frame + offset >= FRAMES:
                break
            left[frame + offset] += sample * gain * (1.0 - max(0.0, pan))
            right[frame + offset] += sample * gain * (1.0 + min(0.0, pan))

    # Four bars at 120 BPM. The offset hats and stereo accents exercise onset
    # and channel-coherence paths without pulling in external source material.
    for beat in range(16):
        if beat % 4 in (0, 2):
            place("kick", beat * RATE // 2, 0.62)
        if beat % 4 in (1, 3):
            place("snare", beat * RATE // 2, 0.52)
        place("hat", beat * RATE // 2, 0.27, -0.35 if beat % 2 else 0.35)
        place("hat", beat * RATE // 2 + RATE // 4, 0.21, 0.35 if beat % 2 else -0.35)
        if args.kind == "mix":
            place("pluck", beat * RATE // 2 + RATE // 16, 0.34, -0.2 if beat % 2 else 0.2)

    peak = max(max(map(abs, left)), max(map(abs, right)))
    if peak >= 1.0:
        raise ValueError(f"fixture would clip at {peak:.3f}")
    interleaved = array.array("h")
    for l_value, r_value in zip(left, right, strict=False):
        interleaved.extend((round(l_value * 32767), round(r_value * 32767)))
    if sys.byteorder != "little":
        interleaved.byteswap()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(args.output), "wb") as audio:
        audio.setnchannels(2)
        audio.setsampwidth(2)
        audio.setframerate(RATE)
        audio.writeframes(interleaved.tobytes())
    print(f"{args.kind}: {FRAMES} frames, peak {peak:.6f}, {args.output}")


if __name__ == "__main__":
    main()
