#!/usr/bin/env python3
"""Measures kicks in reference intros and in Anvil loops, so presets can be
fitted to real records (ADR-0060).

Commands:
  decode CORPUS WORK       decode each MP3's first 40 s into WORK/full and a
                           700 Hz low-passed copy into WORK/low, named
                           <id>_<tagged BPM>.wav (ffmpeg and ffprobe needed)
  shape FOLDER             pitch fall and decay of each WAV's kicks
  target WORK [IDS...]     the reference: the median of each track's median
                           kick, from the tracks named by id (all when none)
  compare WORK LOOPS...    Anvil loops (from oscillate_anvil_loops) against
                           the reference

Kicks are found on the tagged tempo grid of the low-passed signal. Pick the
tracks whose intros are kick only: the body's upper bands and the attack's top
otherwise carry pads and hats.
"""

import glob
import json
import os
import subprocess
import sys
import wave

import numpy as np

RATE = 48000
BANDS = [(20, 70), (70, 150), (150, 400), (400, 1500), (1500, 6000), (6000, 16000)]
NAMES = ["20-70", "70-150", "150-400", "400-1.5k", "1.5-6k", "6-16k"]


def load(path):
    """Mono float samples of a 16-bit, 24-bit or float WAV."""
    with wave.open(path) as w:
        raw = w.readframes(w.getnframes())
        width, channels = w.getsampwidth(), w.getnchannels()
    if width == 2:
        x = np.frombuffer(raw, dtype=np.int16).astype(np.float64) / 32768
    elif width == 3:
        b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        x = np.where(v >= 1 << 23, v - (1 << 24), v).astype(np.float64) / 8388607
    else:
        x = np.frombuffer(raw, dtype=np.float32).astype(np.float64)
    return x.reshape(-1, channels).mean(axis=1)


def bpm_of(path):
    tail = os.path.basename(path)[:-4].rsplit("_", 1)[-1]
    return float(tail) if tail.isdigit() else 124.0


def hits(low, bpm):
    """Kick starts on the tempo grid: the phase with the most onset energy."""
    beat = 60 / bpm * RATE
    env = np.convolve(np.abs(low), np.ones(240) / 240, mode="same")
    rise = np.maximum(0, np.diff(env, prepend=0))
    usable = int(min(len(low), 32 * beat))
    best, phase = -1.0, 0
    for p in range(0, int(beat), 48):
        idx = (p + np.arange(0, usable - p, beat)).astype(int)
        score = rise[idx].sum()
        if score > best:
            best, phase = score, p
    starts = []
    for k in np.arange(phase, usable - beat, beat):
        a, b = int(k - 480), int(k + 480)
        if a < 480:
            continue
        on = a + int(np.argmax(rise[a:b]))
        before = env[on - 480 : on] > env[on] * 0.1
        starts.append(on - 480 + int(np.argmax(before)) if before.any() else on)
    return starts, beat


def segments(full, low, bpm):
    starts, beat = hits(low, bpm)
    return [
        full[s : s + int(beat)]
        for s in starts
        if s + beat <= len(full) and np.max(np.abs(full[s : s + 960])) > 0.05
    ]


def loop_segments(full):
    """An Anvil loop: sixteen hits from frame zero; the first is skipped."""
    beat = 60 / 124 * RATE
    return [full[int(round(k * beat)) : int(round(k * beat)) + int(beat)] for k in range(1, 15)]


def spectrum(seg):
    n = 1 << 15
    mag = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), n)) ** 2
    return np.fft.rfftfreq(n, 1 / RATE), mag


def level_db(seg, ms, peak):
    a = int(ms * 48)
    return 20 * np.log10(max(np.max(np.abs(seg[a : a + 960])), 1e-9) / peak)


def features(seg):
    """Attack and body band balance (dB of the window), the body's harmonics
    at 2-5 times its fundamental (dB), and the level after the hit (dB)."""
    out = []
    for a, b in ((0, 30), (30, 200)):
        f, m = spectrum(seg[int(a * 48) : int(b * 48)])
        total = m[(f >= 20) & (f < 16000)].sum()
        out += [
            10 * np.log10(max(m[(f >= lo) & (f < hi)].sum() / total, 1e-12)) for lo, hi in BANDS
        ]
    f, m = spectrum(seg[int(40 * 48) : int(160 * 48)])
    low = (f >= 35) & (f <= 80)
    f0 = f[low][np.argmax(m[low])]

    def at(h):
        return m[(f > h * f0 - 4) & (f < h * f0 + 4)].max()

    out += [10 * np.log10(max(at(h) / at(1), 1e-12)) for h in (2, 3, 4, 5)]
    peak = np.max(np.abs(seg[:960]))
    out += [level_db(seg, ms, peak) for ms in (50, 100, 150, 200)]
    return np.array(out)


def pitch_track(seg):
    zc = np.where((seg[:-1] < 0) & (seg[1:] >= 0))[0]
    t = (zc[:-1] + zc[1:]) / 2 / 48
    f = RATE / np.diff(zc)

    def f_at(ms):
        near = np.abs(t - ms) < max(3, ms * 0.15)
        return float(np.median(f[near])) if near.any() else float("nan")

    return [f_at(ms) for ms in (3, 8, 15, 30, 60, 120)]


def decode(corpus, work):
    for sub in ("full", "low"):
        os.makedirs(os.path.join(work, sub), exist_ok=True)
    for mp3 in sorted(glob.glob(os.path.join(corpus, "*.mp3"))):
        bpm = subprocess.run(
            [
                "ffprobe",
                "-v",
                "error",
                "-show_entries",
                "format_tags=TBPM",
                "-of",
                "default=nw=1:nk=1",
                mp3,
            ],
            capture_output=True,
            text=True,
            check=False,
        ).stdout.strip()
        if not bpm.isdigit():
            continue
        name = os.path.basename(mp3).split("_", 1)[0] + "_" + bpm + ".wav"
        common = [
            "ffmpeg",
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-t",
            "40",
            "-i",
            mp3,
            "-ac",
            "1",
            "-ar",
            str(RATE),
        ]
        subprocess.run(common + ["-c:a", "pcm_s16le", os.path.join(work, "full", name)], check=True)
        subprocess.run(
            common
            + [
                "-af",
                "lowpass=f=700,lowpass=f=700",
                "-c:a",
                "pcm_s16le",
                os.path.join(work, "low", name),
            ],
            check=True,
        )


def shape(folder):
    for path in sorted(glob.glob(os.path.join(folder, "*.wav"))):
        x = load(path)
        segs = segments(x, x, bpm_of(path))
        if not segs:
            continue
        hz = np.nanmedian([pitch_track(s) for s in segs], axis=0)
        db = np.median(
            [
                [level_db(s, ms, np.max(np.abs(s[:960]))) for ms in (50, 100, 150, 200, 300)]
                for s in segs
            ],
            axis=0,
        )
        print(
            f"{os.path.basename(path):30s} Hz@3/8/15/30/60/120 {' '.join(f'{v:5.0f}' for v in hz)}"
            f" | dB@50/100/150/200/300 {' '.join(f'{v:6.1f}' for v in db)}"
        )


def target(work, ids):
    tracks = []
    for path in sorted(glob.glob(os.path.join(work, "full", "*.wav"))):
        name = os.path.basename(path)
        if ids and name.split("_", 1)[0] not in ids:
            continue
        segs = segments(load(path), load(os.path.join(work, "low", name)), bpm_of(path))
        if segs:
            tracks.append(np.median([features(s) for s in segs], axis=0))
    reference = np.median(np.array(tracks), axis=0)
    with open(os.path.join(work, "target.json"), "w") as out:
        json.dump(reference.tolist(), out)
    print(f"reference from {len(tracks)} tracks written to {work}/target.json")


def report(name, v):
    print(
        f"{name:30s} attack 150-400 {v[2]:6.1f} 400-1.5k {v[3]:6.1f} 1.5-6k {v[4]:6.1f}"
        f" | H3 {v[13]:6.1f} | dB@50/100/150/200 {' '.join(f'{d:5.1f}' for d in v[16:20])}"
    )


def compare(work, loops):
    with open(os.path.join(work, "target.json")) as f:
        report("REFERENCE", np.array(json.load(f)))
    for path in loops:
        report(
            os.path.basename(path),
            np.median([features(s) for s in loop_segments(load(path))], axis=0),
        )


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    command = argv[1]
    if command == "decode" and len(argv) == 4:
        decode(argv[2], argv[3])
    elif command == "shape":
        shape(argv[2])
    elif command == "target":
        target(argv[2], set(argv[3:]))
    elif command == "compare":
        compare(argv[2], argv[3:])
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
