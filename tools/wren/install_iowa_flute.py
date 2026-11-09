#!/usr/bin/env python3
"""Download the University of Iowa MIS flute and install it as Wren's library.

Wren (ADR-0089) plays one sampled concert flute: the pre-2012 University of
Iowa Musical Instrument Samples session, 22 AIFF recordings (16-bit, 44.1 kHz,
mono), each a chromatic run played note by note at one dynamic (pp, mf, ff),
with or without vibrato. This script:

1. downloads the 22 files from the MIS site, or takes copies already
   downloaded with --source, and checks each against the SHA-256 pinned below
   (there is no git commit to pin, so the hashes are the integrity check);
2. resamples each run to 48 kHz with ffmpeg's soxr resampler and cuts it into
   notes on the 48 kHz data: a 10 ms RMS envelope, a note starting 18 dB above
   the file's 10th-percentile floor and ending when it falls within 10 dB of
   it, notes of 0.3 s or more, with 10 ms of pre-roll;
3. assigns keys by position in the run (the run's first key is the one its
   file name gives, confirmed by the median of measured key minus position
   being within 0.75 semitone of it),
   measures each note's pitch 150-450 ms after its onset (autocorrelation,
   200-2500 Hz) and drops a note more than 0.75 semitone from its key (one ff
   non-vibrato note cracked to its twelfth); `cents` is the measured offset of
   the played pitch from the key;
4. keeps one note per (kind, key, layer): the pp non-vibrato runs overlap on
   B4, and a note inside its run is preferred to one at a run's end, then the
   note from the lower run;
5. trims the silence before the attack to 2 ms (-40 dB re the note's peak)
   with a 2 ms fade-in, and fades the last 5 ms out;
6. marks the attack end: where the 10 ms RMS envelope first reaches 90 % of
   the note's steady level (the median over 40-85 % of the note);
7. finds a loop in the steady part (40-85 % of the note, and at least a
   crossfade after the attack end, since a few notes swell for over a
   second; widened toward 25-92 % when that holds less than 0.5 s):
   - 5 ms level and pitch tracks are matched over a crossfade before and half
     a crossfade after both points, trading match against length (a loop
     may shrink to 0.5 s, so a note that decays or swells across its steady
     part loops where its ends agree rather than pumping);
   - a vibrato note's loop spans a whole number of vibrato cycles, the rate
     measured from its pitch track (or its level track when the pitch shows
     no clear cycle); a vibrato recording holding a note straight (under 2
     cents deep) is looped like a plain note;
   - the points are then refined to rising zero crossings whose crossfade
     passages correlate best;
   a loop shorter than 0.5 s is left only when the widened part cannot hold
   it;
8. bakes the loop crossfade into the sample: the frames just before loop_end
   are crossfaded into the frames just before loop_start, so a player that
   jumps from loop_end to loop_start continues seamlessly, with no crossfade
   of its own. The fade is 120 ms, equal power corrected for how alike the
   passages are (gains normalised by sqrt(g_out^2 + g_in^2 + 2 r g_out g_in),
   r their correlation in 10 ms blocks). Where the passages drift out of phase
   over 120 ms (high vibrato notes) and the fade's level would depart from
   the expected power by more than 1 dB in any 10 ms block, it
   is shortened (90, 60, 40 or 20 ms) and placed within those 120 ms where the
   passages agree. The frames from loop_end on are untouched, so a release
   that plays on past loop_end meets the unbaked splice there;
9. peak-normalises and TPDF-dithers (seeded, so the pack is reproducible) to
   16-bit mono, recording the note's peak before normalising as `peak_db`;
10. writes `wren.pack` (little-endian int16 mono, each sample aligned to
    4 KiB) and `wren.manifest`, with ATTRIBUTION.txt, into the library
    directory.

The manifest's first line is `oscillate wren library 1`, then `name`,
`attribution`, `rate 48000`, `channels 1`, `pack wren.pack <bytes>`, then one
line per sample:

    sample <vibrato|plain> <key> <layer> <offset> <frames> <attack_end>
           <loop_start> <loop_end> <cents> <scale> <peak_db>

`offset` is the sample's first frame in the pack; `frames`, `attack_end`,
`loop_start` and `loop_end` are in frames, the last three relative to the
sample's own first frame (loop_end is exclusive: the frame after the loop's
last). `layer` is 0 pp, 1 mf, 2 ff. `scale` turns an int16 value into the
recording's level (1 / (gain * 32768)), and `peak_db` is the note's peak in
the recording, before normalising.

The library directory is $OSCILLATE_WREN_LIBRARY when set, otherwise
$XDG_DATA_HOME/oscillate/instruments/wren/iowa-flute (~/.local/share when
XDG_DATA_HOME is unset); Wren looks in the same place. The new library
replaces an old one only once it is complete.

Requires ffmpeg built with libsoxr. Run with
`python3 -I tools/wren/install_iowa_flute.py`.
"""

from __future__ import annotations

import argparse
import array
import concurrent.futures
import hashlib
import itertools
import math
import os
import random
import re
import shutil
import statistics
import subprocess
import sys
import urllib.parse
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path

INDEX_URL = "https://theremin.music.uiowa.edu/MISflute.html"
SITE = "https://theremin.music.uiowa.edu/"
SOURCE_FOLDER = "sound files/MIS/Woodwinds/flute/"
# The pre-2012 session's 22 runs and their SHA-256.
SOURCES: dict[str, str] = {
    "Flute.nonvib.ff.B3B4.aiff": "6e756cdc885254d1949f43dc514302553d593dadfc39d21c5268393cbafc0c0d",
    "Flute.nonvib.ff.C5B5.aiff": "6f93ab76ea67466de6b1a4d4152e4178b62131e79f726bfb6948951cf62bb5a2",
    "Flute.nonvib.ff.C6B6.aiff": "ae2f97a73fab989542c14751cc9b7ce4021c1f8f593f758f09c5acf3975152e2",
    "Flute.nonvib.ff.C7Db7.aiff": "7a84c0e1312e07bd80e4121bf29df91e527aa969694e6321392f9a4f4ad74179",  # noqa: E501
    "Flute.nonvib.mf.B3B4.aiff": "8f6e07a5b3988960c333397cac94b13d9ee5fe782a5824aee7efabce96e5ece4",
    "Flute.nonvib.mf.C5B5.aiff": "43d8d6c620fe9c508c30b568b023e54fc226960adcb8455467e511a22866066c",
    "Flute.nonvib.mf.C6B6.aiff": "c1d7096eda92b9fd4fdc37b4c710b623a3c952e7d893b8d7ab48e93da4c2d60e",
    "Flute.nonvib.mf.C7.aiff": "0f5a5eda635cba60b640c271c1207abd6ec0d61208f5758815d5c293046759e2",
    "Flute.nonvib.pp.B3B4.aiff": "663f133c3f260c47ebae6cc7e5c4e7bacec047664cf68f76ab2a4bcf109dc2a3",
    "Flute.nonvib.pp.B4Bb5.aiff": "c0090e167a4480a0782c1891691bcf9619b5af659cedd1199dc0947381213b17",  # noqa: E501
    "Flute.nonvib.pp.C6Bb6.aiff": "de135099e0a5d25f183955eaf9ea9031a0dc1e824a618e8467e17c9027feab7f",  # noqa: E501
    "Flute.vib.ff.B3B4.aiff": "a02f479a6c3eba61cac463426088b3dbd71c07c11cea486dd5b3a574161f1d68",
    "Flute.vib.ff.C5B5.aiff": "edac84c8944349ca21fcb82cd79caf72d819fdcbe21cb90ce4463ec248202bd9",
    "Flute.vib.ff.C6B6.aiff": "12acb2fa637715a45d89e07ac2fa997a4b4288c4ea9157a81f38938e8a2c7a10",
    "Flute.vib.ff.Db7.aiff": "d67bc1eb53bbfebd29fecd93f66ff53e60d5880874f7c2c329444db6774b4c3c",
    "Flute.vib.mf.B3B4.aiff": "31bb6b948f8b4cec925d101f1a5f4f231b117e6ee47ba6f4d8278cb4ca356800",
    "Flute.vib.mf.C5B5.aiff": "8eab8bebf49e1a23614097df78a08b6cc8990e8af32cd1c734349ddf6e2909e7",
    "Flute.vib.mf.C6B6.aiff": "4a673f80dba639593578789844c6b85a1d1edf52648ffe582b44c3b2c379feac",
    "Flute.vib.mf.C7Db7.aiff": "d42406c8d17f4eb4800090e99ece64caa0b1c9918779d5a148d7fa32edd287f2",
    "Flute.vib.pp.B3B4.aiff": "965460b174df3dca02e9b89a7ea039adbee40a7ec9bb2950eee85aecde1e6f5e",
    "Flute.vib.pp.C5B5.aiff": "3e74ab860764d8bb7115b1b64c660be47dfe4a6d75d7b121c5c6962fb1847031",
    "Flute.vib.pp.C6B6.aiff": "a3b6ddb3aca647e0bfa2666dee42b69408af1271bec974c092622806c1bca82c",
}
DOWNLOAD_TIMEOUT_SECONDS = 120
HASH_CHUNK_BYTES = 1 << 20

NAME = "University of Iowa MIS flute"
ATTRIBUTION = "University of Iowa Musical Instrument Samples (Lawrence Fritts)"
CREDIT = (
    "University of Iowa Musical Instrument Samples, recorded by Lawrence Fritts, "
    "Electronic Music Studios"
)
TERMS = "may be downloaded and used for any projects, without restrictions"
FORMAT_LINE = "oscillate wren library 1"
MANIFEST = "wren.manifest"
PACK = "wren.pack"
ATTRIBUTION_FILE = "ATTRIBUTION.txt"
DEFAULT_SOURCE_NAME = ".iowa-flute-source"

# File names: Flute.<vib|nonvib>.<pp|mf|ff>.<first note>[<last note>].aiff
FILE_NAME = re.compile(r"Flute\.(vib|nonvib)\.(pp|mf|ff)\.([A-G]b?\d)([A-G]b?\d)?\.aiff")
NOTE_NAME = re.compile(r"([A-G])(b?)(\d)")
PITCH_CLASSES = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}
FLAT = 1
KINDS = {"vib": "vibrato", "nonvib": "plain"}
KIND_ORDER = ("vibrato", "plain")
LAYERS = {"pp": 0, "mf": 1, "ff": 2}
FIRST_KEY = 59  # B3
LAST_KEY = 97  # Db7

A4_HZ = 440.0
A4_KEY = 69
SEMITONES_PER_OCTAVE = 12
CENTS_PER_SEMITONE = 100.0
CENTS_PER_OCTAVE = CENTS_PER_SEMITONE * SEMITONES_PER_OCTAVE
DECIBELS_PER_DECADE = 20.0
POWER_DECIBELS_PER_DECADE = 10.0
HALF_STEP = 0.5
# Added to mean squares before taking logarithms of silence.
TINY_POWER = 1e-20

SOURCE_RATE = 44_100
RATE = 48_000
CHANNELS = 1
BYTES_PER_SAMPLE = 2
BYTES_PER_FRAME = CHANNELS * BYTES_PER_SAMPLE
INT16_FULL_SCALE = 32_768.0
INT16_MIN = -32_768
INT16_MAX = 32_767
ALIGN_FRAMES = 2_048  # 4 KiB of int16 mono.
# soxr's precision in bits (ffmpeg's range is 15 to 33; 28 is soxr's "very high").
SOXR_PRECISION = 28
MILLISECONDS_PER_SECOND = 1_000


def frames_of(milliseconds: float) -> int:
    return round(milliseconds * RATE / MILLISECONDS_PER_SECOND)


# ---- Cutting the runs ---------------------------------------------------------------
CUT_HOP_FRAMES = frames_of(10)
ONSET_DB_ABOVE_FLOOR = 18.0
OFFSET_DB_ABOVE_FLOOR = 10.0
FLOOR_PERCENTILE = 10.0
PERCENT = 100.0
MIN_NOTE_FRAMES = frames_of(300)
CUT_PRE_ROLL_FRAMES = frames_of(10)

# ---- Pitch -----------------------------------------------------------------------------
PITCH_SKIP_FRAMES = frames_of(150)
PITCH_WINDOW_FRAMES = frames_of(300)
HANN_HALF = 0.5
PITCH_LOW_HZ = 200.0
PITCH_HIGH_HZ = 2_500.0
# The shortest lag whose peak is within this fraction of the highest wins,
# which avoids reading an octave low.
NEAR_MAXIMUM = 0.9
# A note further than this from its key (in semitones) is not that note.
MAX_KEY_ERROR = 0.75

# ---- Trim -----------------------------------------------------------------------------
# The attack starts where the signal first exceeds this fraction of the
# note's peak (-40 dB); 2 ms before it are kept and faded in.
ONSET_FRACTION = 0.01
TRIM_PRE_ROLL_FRAMES = frames_of(2)
END_FADE_FRAMES = frames_of(5)

# ---- Attack end -------------------------------------------------------------------------
ENVELOPE_WINDOW_FRAMES = frames_of(10)
ENVELOPE_STEP_FRAMES = frames_of(1)
ATTACK_LEVEL_FRACTION = 0.9
STEADY_START_FRACTION = 0.40
STEADY_END_FRACTION = 0.85

# ---- Loop ---------------------------------------------------------------------------------
CROSSFADE_FRAMES = frames_of(120)
MIN_LOOP_FRAMES = frames_of(500)
# When 40-85 % of the note holds less than MIN_LOOP_FRAMES, the loop may use
# up to this much of it (never starting within a crossfade of the attack end).
WIDE_START_FRACTION = 0.25
WIDE_END_FRACTION = 0.92
# Level and pitch tracks for matching the loop points.
TRACK_HOP_FRAMES = frames_of(5)
TRACK_WINDOW_FRAMES = frames_of(40)
# The pitch track follows the autocorrelation peak nearest this lag (a
# multiple of the period), which resolves a high note's pitch finely.
TRACK_TARGET_LAG_FRAMES = frames_of(5)
TRACK_SEARCH_FRACTION = 0.04
# Vibrato is looked for between these rates; a track whose normalised
# autocorrelation peak is below VIBRATO_MIN_STRENGTH shows no clear cycle.
VIBRATO_LOW_HZ = 3.0
VIBRATO_HIGH_HZ = 9.0
VIBRATO_MIN_STRENGTH = 0.5
# Shallower than this (RMS about the trend) is no vibrato: the pp vibrato
# runs are 3-5 cents deep in pitch, and hold a few high notes straight.
VIBRATO_MIN_DEPTH_CENTS = 2.0
VIBRATO_MIN_DEPTH_DB = 0.5
# A vibrato loop may differ from a whole number of measured cycles by this
# fraction of a cycle, so the match can follow the cycle actually played.
VIBRATO_SLACK_CYCLES = 0.2
# A loop may be as short as MIN_LOOP_FRAMES (or the fewest whole vibrato
# cycles reaching it) to match better; shortening it to nothing would cost
# this much match score (1 = LEVEL_TOLERANCE_DB of level difference).
SHORTER_LOOP_COST = 2.0
# Track matching: the level and pitch over a crossfade before each point and
# half a crossfade after it, weighted by these tolerances.
MATCH_AFTER_FRACTION = 0.5
LEVEL_TOLERANCE_DB = 0.5
PITCH_TOLERANCE_CENTS = 5.0
# Waveform refinement: rising zero crossings within this distance of each
# coarse point (at least FINE_SEARCH_PERIODS periods), scored by the
# correlation of the crossfade passages before them, weighted toward the
# passages' middle where both are heard equally.
FINE_SEARCH_FRAMES = frames_of(3)
FINE_SEARCH_PERIODS = 1.5
# The crossfade's correlation is measured in blocks of this size; a block
# more out of phase than this is corrected only this far (the gain stays
# bounded where the passages would cancel).
CROSSFADE_BLOCK_FRAMES = frames_of(10)
LEAST_CORRECTED_CORRELATION = -0.5
# Fade lengths tried, longest first, and the steps they are placed in.
FADE_LENGTHS = tuple(frames_of(milliseconds) for milliseconds in (120, 90, 60, 40, 20))
FADE_PLACEMENT_STEP = frames_of(5)
MAX_FADE_DEVIATION_DB = 1.0
QUARTER_TURN = math.pi / 2.0

# Normalised peak, a little under full scale so dither never clips.
NORMALISED_PEAK = 0.98
DEFAULT_JOBS = 4


@dataclass
class Note:
    kind: str
    layer: int
    key: int
    file: str
    index: int  # Position in the run.
    run_length: int
    run_first_key: int
    measured_hz: float
    cents: float
    # Filled when the note is converted.
    frames: int = 0
    attack_end: int = 0
    loop_start: int = 0
    loop_end: int = 0
    crossfade: int = 0
    fade_start: int = 0
    fade_dip: float = 0.0  # The fade's worst level departure (dB, 0 or less).
    loop_match: float = 0.0
    vibrato_hz: float = 0.0
    vibrato_source: str = ""
    widened: bool = False
    peak: float = 0.0
    scale: float = 0.0
    pcm: bytes = b""
    offset: int = 0

    @property
    def interior(self) -> bool:
        return 0 < self.index < self.run_length - 1


@dataclass
class RunResult:
    file: str
    kind: str
    layer: int
    first_key: int
    kept: list[Note] = field(default_factory=list)
    dropped: list[Note] = field(default_factory=list)


def data_directory() -> Path:
    override = os.environ.get("OSCILLATE_WREN_LIBRARY")
    if override:
        return Path(override)
    base = os.environ.get("XDG_DATA_HOME") or str(Path.home() / ".local" / "share")
    return Path(base) / "oscillate" / "instruments" / "wren" / "iowa-flute"


# ---- Fetching and checking ------------------------------------------------------------


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(HASH_CHUNK_BYTES):
            digest.update(chunk)
    return digest.hexdigest()


def source_url(name: str) -> str:
    return SITE + urllib.parse.quote(SOURCE_FOLDER + name)


def fetch(source: Path) -> None:
    """Download any of the 22 files `source` lacks (or holds with the wrong hash)."""
    source.mkdir(parents=True, exist_ok=True)
    for name, expected in SOURCES.items():
        path = source / name
        if path.is_file() and sha256(path) == expected:
            continue
        print(f"  downloading {name}")
        partial = path.with_name(name + ".partial")
        with (
            urllib.request.urlopen(source_url(name), timeout=DOWNLOAD_TIMEOUT_SECONDS) as reply,
            partial.open("wb") as output,
        ):
            shutil.copyfileobj(reply, output)
        if sha256(partial) != expected:
            partial.unlink()
            raise SystemExit(f"{name}: the download does not match its pinned SHA-256")
        partial.rename(path)


def verify(source: Path) -> None:
    for name, expected in SOURCES.items():
        path = source / name
        if not path.is_file():
            raise SystemExit(f"missing {name} in {source}")
        if sha256(path) != expected:
            raise SystemExit(f"{name} does not match its pinned SHA-256")


def key_of(note_name: str) -> int:
    match = NOTE_NAME.fullmatch(note_name)
    if match is None:
        raise SystemExit(f"cannot read the note name {note_name!r}")
    letter, flat, octave = match.groups()
    return (int(octave) + 1) * SEMITONES_PER_OCTAVE + PITCH_CLASSES[letter] - (FLAT if flat else 0)


# ---- Signal helpers -----------------------------------------------------------------------


def decode(path: Path) -> array.array:
    """The recording at 48 kHz (soxr), mono doubles at the recording's level."""
    chain = f"aformat=sample_fmts=dbl,aresample={RATE}:resampler=soxr:precision={SOXR_PRECISION}"
    raw = subprocess.run(
        [
            "ffmpeg",
            "-nostdin",
            "-v",
            "error",
            "-i",
            str(path),
            "-af",
            chain,
            "-ac",
            str(CHANNELS),
            "-ar",
            str(RATE),
            "-f",
            "f64le",
            "-c:a",
            "pcm_f64le",
            "-",
        ],
        capture_output=True,
        check=True,
    ).stdout
    values = array.array("d")
    values.frombytes(raw)
    if sys.byteorder != "little":
        values.byteswap()
    return values


def power_db(mean_square: float) -> float:
    return POWER_DECIBELS_PER_DECADE * math.log10(mean_square + TINY_POWER)


def percentile(values: list[float], percent: float) -> float:
    """Linear-interpolated percentile (as numpy's default)."""
    ordered = sorted(values)
    position = percent / PERCENT * (len(ordered) - 1)
    low = math.floor(position)
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


def parabolic(left: float, centre: float, right: float) -> float:
    """Offset of a parabola's vertex through three equally spaced points, within half a step."""
    curvature = left - 2.0 * centre + right
    offset = 0.5 * (left - right) / curvature if curvature != 0.0 else 0.0
    return max(-HALF_STEP, min(HALF_STEP, offset))


def key_of_hz(hz: float) -> float:
    return A4_KEY + SEMITONES_PER_OCTAVE * math.log2(hz / A4_HZ)


def prefix_squares(signal) -> list[float]:
    return list(itertools.accumulate((value * value for value in signal), initial=0.0))


def correlation(signal, a: int, b: int, length: int) -> float:
    """Normalised correlation of signal[a:a+length] with signal[b:b+length]."""
    view = memoryview(signal)
    first, second = view[a : a + length], view[b : b + length]
    cross = math.sumprod(first, second)
    energy = math.sumprod(first, first) * math.sumprod(second, second)
    return cross / math.sqrt(energy) if energy > 0.0 else 0.0


def fundamental(segment) -> float:
    """Autocorrelation pitch (Hann window, parabolic refinement), 200-2500 Hz."""
    count = len(segment)
    mean = math.fsum(segment) / count
    windowed = [
        (value - mean) * (HANN_HALF - HANN_HALF * math.cos(math.tau * index / (count - 1)))
        for index, value in enumerate(segment)
    ]
    shortest = int(RATE / PITCH_HIGH_HZ)
    longest = int(RATE / PITCH_LOW_HZ)
    lags = range(shortest, longest)
    ac = {
        lag: math.sumprod(windowed[: count - lag], windowed[lag:])
        for lag in range(shortest - 1, longest + 1)
    }
    best = max(lags, key=ac.__getitem__)
    chosen = best
    for lag in lags:
        if ac[lag] > NEAR_MAXIMUM * ac[best] and ac[lag] >= ac[lag - 1] and ac[lag] >= ac[lag + 1]:
            chosen = lag
            break
    return RATE / (chosen + parabolic(ac[chosen - 1], ac[chosen], ac[chosen + 1]))


# ---- Cutting ----------------------------------------------------------------------------


def cut(signal: array.array) -> list[tuple[int, int]]:
    """(start, end) frames of each note in a run."""
    view = memoryview(signal)
    hops = len(signal) // CUT_HOP_FRAMES
    levels = []
    for hop in range(hops):
        chunk = view[hop * CUT_HOP_FRAMES : (hop + 1) * CUT_HOP_FRAMES]
        levels.append(power_db(math.sumprod(chunk, chunk) / CUT_HOP_FRAMES))
    floor = percentile(levels, FLOOR_PERCENTILE)
    spans: list[tuple[int, int]] = []
    sounding, first = False, 0
    for hop, level in enumerate(levels):
        if not sounding and level > floor + ONSET_DB_ABOVE_FLOOR:
            sounding, first = True, hop
        elif sounding and level < floor + OFFSET_DB_ABOVE_FLOOR:
            sounding = False
            if (hop - first) * CUT_HOP_FRAMES >= MIN_NOTE_FRAMES:
                spans.append((first, hop))
    if sounding and (hops - first) * CUT_HOP_FRAMES >= MIN_NOTE_FRAMES:
        spans.append((first, hops))
    return [
        (max(0, a * CUT_HOP_FRAMES - CUT_PRE_ROLL_FRAMES), b * CUT_HOP_FRAMES) for a, b in spans
    ]


# ---- Marking and looping -------------------------------------------------------------------


def trim(note: list[float]) -> list[float]:
    """The note from 2 ms before its attack, faded in over those 2 ms and out over its last 5 ms."""
    peak = max(abs(value) for value in note)
    threshold = peak * ONSET_FRACTION
    onset = next(index for index, value in enumerate(note) if abs(value) > threshold)
    trimmed = note[max(0, onset - TRIM_PRE_ROLL_FRAMES) :]
    for index in range(min(TRIM_PRE_ROLL_FRAMES, len(trimmed))):
        trimmed[index] *= index / TRIM_PRE_ROLL_FRAMES
    for index in range(min(END_FADE_FRAMES, len(trimmed))):
        trimmed[-1 - index] *= index / END_FADE_FRAMES
    return trimmed


def window_rms(squares: list[float], start: int, length: int) -> float:
    return math.sqrt(max(0.0, squares[start + length] - squares[start]) / length)


def attack_end_of(squares: list[float], frames: int) -> int:
    """The frame (window centre) where the 10 ms RMS first reaches 90 % of the steady level."""
    starts = range(0, frames - ENVELOPE_WINDOW_FRAMES, ENVELOPE_STEP_FRAMES)
    levels = [window_rms(squares, start, ENVELOPE_WINDOW_FRAMES) for start in starts]
    steady_first = STEADY_START_FRACTION * frames
    steady_last = STEADY_END_FRACTION * frames - ENVELOPE_WINDOW_FRAMES
    steady = statistics.median(
        level
        for start, level in zip(starts, levels, strict=False)
        if steady_first <= start <= steady_last
    )
    for start, level in zip(starts, levels, strict=False):
        if level >= ATTACK_LEVEL_FRACTION * steady:
            return start + ENVELOPE_WINDOW_FRAMES // 2
    raise SystemExit("a note never reaches its steady level")


@dataclass
class Tracks:
    """Level (dB) and pitch (cents re the note's measured pitch) every TRACK_HOP_FRAMES."""

    first_centre: int
    level: list[float]
    pitch: list[float]

    def index(self, frame: int) -> int:
        return round((frame - self.first_centre) / TRACK_HOP_FRAMES)

    def centre(self, index: int) -> int:
        return self.first_centre + index * TRACK_HOP_FRAMES


def difference(signal, squares: list[float], start: int, lag: int) -> float:
    """Squared difference between a track window and the window `lag` frames later."""
    view = memoryview(signal)
    cross = math.sumprod(
        view[start : start + TRACK_WINDOW_FRAMES],
        view[start + lag : start + lag + TRACK_WINDOW_FRAMES],
    )
    return (
        squares[start + TRACK_WINDOW_FRAMES]
        - squares[start]
        + squares[start + lag + TRACK_WINDOW_FRAMES]
        - squares[start + lag]
        - 2.0 * cross
    )


def tracks(signal: array.array, squares: list[float], first: int, last: int, hz: float) -> Tracks:
    """Level and pitch tracks with window centres from `first` to `last`."""
    period = RATE / hz
    periods = max(1, round(TRACK_TARGET_LAG_FRAMES / period))
    nominal = periods * period
    span = max(1, math.ceil(nominal * TRACK_SEARCH_FRACTION))
    lowest = max(2, math.floor(nominal) - span)
    highest = math.ceil(nominal) + span
    half = TRACK_WINDOW_FRAMES // 2
    first = max(first, half)
    last = min(last, len(signal) - half - highest - 1)
    level, pitch = [], []
    lag = None
    for centre in range(first, last + 1, TRACK_HOP_FRAMES):
        start = centre - half
        level.append(
            DECIBELS_PER_DECADE
            * math.log10(
                window_rms(squares, centre - ENVELOPE_WINDOW_FRAMES // 2, ENVELOPE_WINDOW_FRAMES)
                + math.sqrt(TINY_POWER)
            )
        )
        if lag is None:
            lag = min(
                range(lowest, highest + 1),
                key=lambda candidate: difference(signal, squares, start, candidate),
            )
        # Follow the valley from the previous frame's lag.
        while True:
            here = difference(signal, squares, start, lag)
            below = difference(signal, squares, start, lag - 1) if lag > lowest else math.inf
            above = difference(signal, squares, start, lag + 1) if lag < highest else math.inf
            if below < here and below <= above:
                lag -= 1
            elif above < here:
                lag += 1
            else:
                break
        refined = lag
        if math.isfinite(below) and math.isfinite(above):
            refined = lag + parabolic(below, here, above)
        pitch.append(CENTS_PER_OCTAVE * math.log2(nominal / refined))
    return Tracks(first, level, pitch)


def cycle(values: list[float]) -> tuple[float, float, float]:
    """(period in track hops, strength, RMS depth) of the clearest cycle at vibrato rates."""
    count = len(values)
    if count < 2:
        return 0.0, 0.0, 0.0
    centre = (count - 1) / 2.0
    mean = math.fsum(values) / count
    slope_scale = math.fsum((index - centre) ** 2 for index in range(count))
    slope = (
        math.fsum((index - centre) * value for index, value in enumerate(values)) / slope_scale
        if slope_scale > 0.0
        else 0.0
    )
    detrended = [value - mean - slope * (index - centre) for index, value in enumerate(values)]
    hops_per_second = RATE / TRACK_HOP_FRAMES
    shortest = max(1, math.floor(hops_per_second / VIBRATO_HIGH_HZ))
    longest = math.ceil(hops_per_second / VIBRATO_LOW_HZ)
    depth = math.sqrt(math.sumprod(detrended, detrended) / count)
    if longest + 1 >= count:
        return 0.0, 0.0, depth

    def normalised(lag: int) -> float:
        first, second = detrended[: count - lag], detrended[lag:]
        energy = math.sumprod(first, first) * math.sumprod(second, second)
        return math.sumprod(first, second) / math.sqrt(energy) if energy > 0.0 else 0.0

    scores = {lag: normalised(lag) for lag in range(shortest - 1, longest + 2)}
    lags = range(shortest, longest + 1)
    best = max(lags, key=scores.__getitem__)
    chosen = best
    for lag in lags:
        if (
            scores[lag] > NEAR_MAXIMUM * scores[best]
            and scores[lag] >= scores[lag - 1]
            and scores[lag] >= scores[lag + 1]
        ):
            chosen = lag
            break
    period = chosen + parabolic(scores[chosen - 1], scores[chosen], scores[chosen + 1])
    return period, scores[chosen], depth


def track_mismatch(found: Tracks, a: int, b: int, before: int, after: int) -> float:
    """How differently level and pitch run around track indices a and b."""
    level = found.level[a - before : a + after]
    other_level = found.level[b - before : b + after]
    pitch = found.pitch[a - before : a + after]
    other_pitch = found.pitch[b - before : b + after]
    root_count = math.sqrt(len(level))
    return (
        math.dist(level, other_level) / root_count / LEVEL_TOLERANCE_DB
        + math.dist(pitch, other_pitch) / root_count / PITCH_TOLERANCE_CENTS
    )


def rising_crossings(signal: array.array, first: int, last: int) -> list[int]:
    return [
        index
        for index in range(max(1, first), min(len(signal), last + 1))
        if signal[index - 1] < 0.0 <= signal[index]
    ]


def loop_region(frames: int, attack_end: int, needed: int) -> tuple[int, int, bool]:
    """The part of the note the loop lives in: 40-85 %, widened toward 25-92 % when
    shorter than `needed`, and never within a crossfade of the attack end (a few
    notes swell for over a second)."""
    lowest = max(round(WIDE_START_FRACTION * frames), attack_end + CROSSFADE_FRAMES)
    first = max(round(STEADY_START_FRACTION * frames), attack_end + CROSSFADE_FRAMES)
    last = round(STEADY_END_FRACTION * frames)
    if last - first >= needed:
        return first, last, False
    highest = round(WIDE_END_FRACTION * frames)
    # Grow both ends equally, then whichever end still can.
    missing = needed - (last - first)
    first = max(lowest, first - missing // 2)
    last = min(highest, last + missing - missing // 2)
    missing = needed - (last - first)
    if missing > 0:
        first = max(lowest, first - missing)
        last = min(highest, last + needed - (last - first))
    return first, last, True


def find_loop(signal: array.array, squares: list[float], note: Note) -> None:
    frames = len(signal)
    period = RATE / note.measured_hz
    before = CROSSFADE_FRAMES // TRACK_HOP_FRAMES
    after = round(MATCH_AFTER_FRACTION * CROSSFADE_FRAMES) // TRACK_HOP_FRAMES
    wide_first = max(round(WIDE_START_FRACTION * frames), note.attack_end + CROSSFADE_FRAMES)
    wide_last = round(WIDE_END_FRACTION * frames)
    found = tracks(
        signal,
        squares,
        wide_first - CROSSFADE_FRAMES - TRACK_HOP_FRAMES,
        wide_last + CROSSFADE_FRAMES,
        note.measured_hz,
    )

    cycle_frames = 0.0
    if note.kind == "vibrato":
        first, last, _ = loop_region(frames, note.attack_end, MIN_LOOP_FRAMES)
        segment = slice(max(0, found.index(first)), found.index(last) + 1)
        hops, strength, depth = cycle(found.pitch[segment])
        note.vibrato_source = "pitch"
        if strength < VIBRATO_MIN_STRENGTH or depth < VIBRATO_MIN_DEPTH_CENTS:
            hops, strength, depth = cycle(found.level[segment])
            note.vibrato_source = "level"
            if depth < VIBRATO_MIN_DEPTH_DB:
                strength = 0.0
        if strength >= VIBRATO_MIN_STRENGTH:
            cycle_frames = hops * TRACK_HOP_FRAMES
            note.vibrato_hz = RATE / cycle_frames
        else:
            note.vibrato_source = "none"
    needed = (
        math.ceil(MIN_LOOP_FRAMES / cycle_frames) * cycle_frames
        if cycle_frames
        else MIN_LOOP_FRAMES
    )
    first, last, note.widened = loop_region(frames, note.attack_end, round(needed))
    first_index = max(found.index(first), before)
    last_index = min(found.index(last), len(found.level) - after)
    available = last_index - first_index
    # Refining the points to zero crossings may move each by up to `reach`.
    reach = max(FINE_SEARCH_FRAMES, math.ceil(FINE_SEARCH_PERIODS * period))
    minimum = (MIN_LOOP_FRAMES + 2 * reach) / TRACK_HOP_FRAMES
    if cycle_frames:
        # Whole numbers of cycles, from the fewest reaching MIN_LOOP_FRAMES to the most that fit.
        cycle_hops = cycle_frames / TRACK_HOP_FRAMES
        most = max(1, math.floor(available / cycle_hops))
        fewest = min(most, max(1, math.ceil(minimum / cycle_hops)))
        slack = VIBRATO_SLACK_CYCLES * cycle_hops
        lengths = sorted(
            {
                length
                for cycles in range(fewest, most + 1)
                for length in range(
                    math.ceil(cycles * cycle_hops - slack),
                    math.floor(cycles * cycle_hops + slack) + 1,
                )
                if 0 < length <= available
            }
        )
    else:
        lengths = list(range(min(available, math.ceil(minimum)), available + 1))
    lengths = lengths or [max(1, available)]
    best_score, best = math.inf, (first_index, first_index + lengths[-1])
    for length in lengths:
        shortening = SHORTER_LOOP_COST * (1.0 - length / max(available, 1))
        for start in range(first_index, last_index - length + 1):
            score = track_mismatch(found, start, start + length, before, after) + shortening
            if score < best_score:
                best_score, best = score, (start, start + length)
    coarse_start, coarse_end = found.centre(best[0]), found.centre(best[1])

    # Refine to rising zero crossings whose crossfade passages are most alike.
    starts = rising_crossings(signal, coarse_start - reach, coarse_start + reach) or [coarse_start]
    ends = rising_crossings(signal, coarse_end - reach, coarse_end + reach) or [coarse_end]
    starts = [start for start in starts if start >= CROSSFADE_FRAMES] or [coarse_start]
    weights = [
        math.sqrt(math.sin(2.0 * QUARTER_TURN * (index + 1) / CROSSFADE_FRAMES))
        for index in range(CROSSFADE_FRAMES)
    ]

    def passage(point: int) -> tuple[list[float], float]:
        weighted = [
            weight * value
            for weight, value in zip(
                weights, signal[point - CROSSFADE_FRAMES : point], strict=False
            )
        ]
        return weighted, math.sumprod(weighted, weighted)

    start_passages = {start: passage(start) for start in starts}
    end_passages = {end: passage(end) for end in ends}

    def alike(pair: tuple[int, int]) -> float:
        (first_passage, first_energy), (second_passage, second_energy) = (
            start_passages[pair[0]],
            end_passages[pair[1]],
        )
        energy = first_energy * second_energy
        return (
            math.sumprod(first_passage, second_passage) / math.sqrt(energy) if energy > 0.0 else 0.0
        )

    note.loop_start, note.loop_end = max(
        ((start, end) for start in starts for end in ends), key=alike
    )
    note.loop_match = alike((note.loop_start, note.loop_end))


def blend(signal: array.array, note: Note, fade_start: int, fade_length: int) -> list[float]:
    """The CROSSFADE_FRAMES before loop_end, fading from themselves to the frames
    before loop_start over [fade_start, fade_start + fade_length): equal power,
    corrected for the passages' correlation in blocks."""
    out_start = note.loop_end - CROSSFADE_FRAMES
    in_start = note.loop_start - CROSSFADE_FRAMES
    blocks = max(1, round(fade_length / CROSSFADE_BLOCK_FRAMES))
    block = fade_length / blocks
    alike = []
    for index in range(blocks):
        a = fade_start + round(index * block)
        b = fade_start + round((index + 1) * block)
        alike.append(
            min(
                1.0,
                max(
                    LEAST_CORRECTED_CORRELATION,
                    correlation(signal, out_start + a, in_start + a, b - a),
                ),
            )
        )
    result = list(signal[out_start : out_start + fade_start])
    for index in range(fade_length):
        position = (index + 1) / fade_length
        fade_out = math.cos(QUARTER_TURN * position)
        fade_in = math.sin(QUARTER_TURN * position)
        # The block correlation, interpolated between block centres.
        where = min(max((index + 0.5) / block - 0.5, 0.0), blocks - 1.0)
        low = math.floor(where)
        high = min(low + 1, blocks - 1)
        r = alike[low] + (alike[high] - alike[low]) * (where - low)
        power = math.sqrt(fade_out * fade_out + fade_in * fade_in + 2.0 * r * fade_out * fade_in)
        frame = fade_start + index
        result.append(
            (fade_out * signal[out_start + frame] + fade_in * signal[in_start + frame]) / power
        )
    result.extend(signal[in_start + fade_start + fade_length : note.loop_start])
    return result


def worst_deviation(
    signal: array.array, note: Note, blended: list[float], fade_start: int, fade_length: int
) -> float:
    """The largest departure (dB, as a negative number) of a blend's 10 ms block levels
    over the fade from the power expected of the two passages under equal-power gains."""
    out_start = note.loop_end - CROSSFADE_FRAMES
    in_start = note.loop_start - CROSSFADE_FRAMES
    worst = 0.0
    for first in range(fade_start, fade_start + fade_length, CROSSFADE_BLOCK_FRAMES):
        last = min(first + CROSSFADE_BLOCK_FRAMES, fade_start + fade_length)
        expected = 0.0
        for frame in range(first, last):
            position = (frame - fade_start + 1) / fade_length
            expected += (
                math.cos(QUARTER_TURN * position) ** 2 * signal[out_start + frame] ** 2
                + math.sin(QUARTER_TURN * position) ** 2 * signal[in_start + frame] ** 2
            )
        got = math.sumprod(blended[first:last], blended[first:last])
        if expected > 0.0:
            worst = min(worst, -abs(power_db(got / expected)) if got > 0.0 else -math.inf)
    return worst


def bake_crossfade(signal: array.array, note: Note) -> None:
    """Crossfade the frames before loop_end into those before loop_start.

    The fade is CROSSFADE_FRAMES long when the passages stay alike enough that its
    level departs from the expected power by no more than MAX_FADE_DEVIATION_DB
    in any 10 ms block; otherwise it is the longest of FADE_LENGTHS that can be
    placed within those CROSSFADE_FRAMES without departing more (high vibrato
    notes drift out of phase over a long fade, and their partials cancel). With
    no such placement, the one departing least is used.
    After the fade the frames are those before loop_start, so the jump from
    loop_end to loop_start continues seamlessly."""
    if note.loop_start < CROSSFADE_FRAMES or note.loop_end - note.loop_start < CROSSFADE_FRAMES:
        raise SystemExit(f"{note.file} key {note.key}: the loop leaves no room for its crossfade")
    best = (-math.inf, 0, 0, [])
    for fade_length in FADE_LENGTHS:
        for fade_start in range(CROSSFADE_FRAMES - fade_length, -1, -FADE_PLACEMENT_STEP):
            blended = blend(signal, note, fade_start, fade_length)
            dip = worst_deviation(signal, note, blended, fade_start, fade_length)
            if dip > best[0]:
                best = (dip, fade_start, fade_length, blended)
        if best[0] >= -MAX_FADE_DEVIATION_DB:
            break
    note.fade_dip, fade_start, note.crossfade, blended = best
    note.fade_start = fade_start
    signal[note.loop_end - CROSSFADE_FRAMES : note.loop_end] = array.array("d", blended)


def dither(signal: array.array, gain: float, seed: str) -> bytes:
    generator = random.Random(seed)
    uniform = generator.random
    pcm = array.array(
        "h",
        (
            min(
                INT16_MAX,
                max(INT16_MIN, round(value * gain * INT16_FULL_SCALE + uniform() - uniform())),
            )
            for value in signal
        ),
    )
    if sys.byteorder != "little":
        pcm.byteswap()
    return pcm.tobytes()


def convert(note_frames: list[float], note: Note) -> None:
    signal = array.array("d", trim(note_frames))
    note.frames = len(signal)
    squares = prefix_squares(signal)
    note.attack_end = attack_end_of(squares, note.frames)
    find_loop(signal, squares, note)
    bake_crossfade(signal, note)
    note.peak = max(abs(value) for value in signal)
    gain = NORMALISED_PEAK / note.peak
    note.scale = 1.0 / (gain * INT16_FULL_SCALE)
    note.pcm = dither(signal, gain, f"{note.kind} {note.key} {note.layer}")


# ---- One run ------------------------------------------------------------------------------


def process(path: Path) -> RunResult:
    match = FILE_NAME.fullmatch(path.name)
    if match is None:
        raise SystemExit(f"cannot read the file name {path.name}")
    kind_name, dynamic, first_name, last_name = match.groups()
    first_key = key_of(first_name)
    last_key = key_of(last_name) if last_name else first_key
    result = RunResult(path.name, KINDS[kind_name], LAYERS[dynamic], first_key)
    signal = decode(path)
    spans = cut(signal)
    if len(spans) != last_key - first_key + 1:
        raise SystemExit(
            f"{path.name}: cut {len(spans)} notes, its name spans {last_key - first_key + 1}"
        )
    measured = []
    for start, _ in spans:
        window = signal[start + PITCH_SKIP_FRAMES : start + PITCH_SKIP_FRAMES + PITCH_WINDOW_FRAMES]
        measured.append(fundamental(window))
    # Keys by position: the measured pitches, less their positions, must agree
    # with the first key the name gives (the high register plays up to half a
    # semitone sharp, so the unrounded keys are compared).
    run_first = statistics.median(key_of_hz(hz) - index for index, hz in enumerate(measured))
    if abs(run_first - first_key) > MAX_KEY_ERROR:
        raise SystemExit(
            f"{path.name}: its notes measure as starting on {run_first:.2f}, "
            f"its name says {first_key}"
        )
    for index, ((start, end), hz) in enumerate(zip(spans, measured, strict=False)):
        key = first_key + index
        exact = key_of_hz(hz)
        note = Note(
            result.kind,
            result.layer,
            key,
            path.name,
            index,
            len(spans),
            first_key,
            hz,
            CENTS_PER_SEMITONE * (exact - key),
        )
        if abs(exact - key) > MAX_KEY_ERROR or not FIRST_KEY <= key <= LAST_KEY:
            result.dropped.append(note)
            continue
        convert(signal[start:end].tolist(), note)
        result.kept.append(note)
    return result


# ---- The library ----------------------------------------------------------------------------


def choose(notes: list[Note]) -> tuple[list[Note], list[Note]]:
    """One note per (kind, key, layer): inside its run first, then from the lower run."""
    groups: dict[tuple[str, int, int], list[Note]] = {}
    for note in notes:
        groups.setdefault((note.kind, note.key, note.layer), []).append(note)
    kept, duplicates = [], []
    for group in groups.values():
        group.sort(key=lambda note: (not note.interior, note.run_first_key))
        kept.append(group[0])
        duplicates.extend(group[1:])
    kept.sort(key=lambda note: (KIND_ORDER.index(note.kind), note.key, note.layer))
    return kept, duplicates


def summarise(notes: list[Note], dropped: list[Note], duplicates: list[Note]) -> None:
    for kind in KIND_ORDER:
        for dynamic, layer in LAYERS.items():
            keys = sorted(note.key for note in notes if note.kind == kind and note.layer == layer)
            missing = sorted(set(range(keys[0], keys[-1] + 1)) - set(keys)) if keys else []
            print(
                f"  {kind:7} {dynamic}: {len(keys):2} notes, keys {keys[0]}-{keys[-1]}"
                + (f", missing {missing}" if missing else "")
            )
    for note in dropped:
        print(
            f"  dropped {note.file} note {note.index + 1} (key {note.key}): plays "
            f"{note.cents:+.0f} cents off its key"
        )
    for note in duplicates:
        print(f"  duplicate {note.file} note {note.index + 1} (key {note.key}) not used")
    cents = [abs(note.cents) for note in notes]
    loops = [(note.loop_end - note.loop_start) / RATE for note in notes]
    lengths = [note.frames / RATE for note in notes]
    print(f"  |cents|: median {statistics.median(cents):.1f}, max {max(cents):.1f}")
    print(
        f"  notes {min(lengths):.2f}-{max(lengths):.2f} s; loops {min(loops):.2f}-"
        f"{max(loops):.2f} s (median {statistics.median(loops):.2f}); "
        f"{sum(note.widened for note in notes)} widened, "
        f"{sum(loop < MIN_LOOP_FRAMES / RATE for loop in loops)} under "
        f"{MIN_LOOP_FRAMES / RATE:.1f} s"
    )
    fades = [note.crossfade for note in notes]
    print(
        "  crossfades: "
        + ", ".join(
            f"{length * MILLISECONDS_PER_SECOND // RATE} ms x{fades.count(length)}"
            for length in FADE_LENGTHS
            if fades.count(length)
        )
    )
    print(f"  worst fade level departure {-min(note.fade_dip for note in notes):.2f} dB")
    matches = [note.loop_match for note in notes]
    print(
        f"  crossfade passage correlation: min {min(matches):.2f}, median "
        f"{statistics.median(matches):.2f}"
    )
    vibrato = [note for note in notes if note.kind == "vibrato"]
    rates = [note.vibrato_hz for note in vibrato if note.vibrato_hz]
    sources = {
        source: sum(note.vibrato_source == source for note in vibrato)
        for source in ("pitch", "level", "none")
    }
    if rates:
        print(
            f"  vibrato {min(rates):.2f}-{max(rates):.2f} Hz (median "
            f"{statistics.median(rates):.2f}); measured from {sources}"
        )


def build(source: Path, target: Path, jobs: int) -> None:
    paths = [source / name for name in SOURCES]
    print(f"cutting and converting {len(paths)} runs")
    with concurrent.futures.ProcessPoolExecutor(jobs) as pool:
        runs = list(pool.map(process, paths))
    notes = [note for run in runs for note in run.kept]
    dropped = [note for run in runs for note in run.dropped]
    total = len(notes) + len(dropped)
    notes, duplicates = choose(notes)
    print(
        f"{total} notes cut, {len(dropped)} off their key, {len(duplicates)} duplicate(s), "
        f"{len(notes)} kept"
    )
    offset = 0
    for note in notes:
        note.offset = offset
        offset += (note.frames + ALIGN_FRAMES - 1) // ALIGN_FRAMES * ALIGN_FRAMES
    pack_bytes = offset * BYTES_PER_FRAME
    target.mkdir(parents=True)
    with (target / PACK).open("wb") as pack:
        for note in notes:
            pack.seek(note.offset * BYTES_PER_FRAME)
            pack.write(note.pcm)
        pack.truncate(pack_bytes)
        pack.flush()
        os.fsync(pack.fileno())
    lines = [
        FORMAT_LINE,
        f"name {NAME}",
        f"attribution {ATTRIBUTION}",
        f"rate {RATE}",
        f"channels {CHANNELS}",
        f"pack {PACK} {pack_bytes}",
    ]
    for n in notes:
        lines.append(
            " ".join(
                str(v)
                for v in (
                    "sample",
                    n.kind,
                    n.key,
                    n.layer,
                    n.offset,
                    n.frames,
                    n.attack_end,
                    n.loop_start,
                    n.loop_end,
                    f"{n.cents:.2f}",
                    f"{n.scale:.9e}",
                    f"{DECIBELS_PER_DECADE * math.log10(n.peak):.3f}",
                )
            )
        )
    (target / MANIFEST).write_text("\n".join(lines) + "\n")
    (target / ATTRIBUTION_FILE).write_text(
        f"{CREDIT}.\n\n"
        f"The MIS page ({INDEX_URL}) says the recordings\n"
        f'"{TERMS}".\n'
        "There is no formal licence.\n\n"
        "Source: the 22 pre-2012 flute recordings linked from that page, each checked\n"
        "against a pinned SHA-256.\n"
        f"Converted for Oscillate's Wren: resampled from {SOURCE_RATE} Hz to {RATE} Hz, cut into\n"
        "single notes, attack pre-roll trimmed to 2 ms, a loop crossfade baked into the\n"
        "steady part of each note, peak-normalised and dithered to 16-bit; the sound is\n"
        "otherwise unchanged.\n"
    )
    summarise(notes, dropped, duplicates)
    print(f"pack: {pack_bytes} bytes ({pack_bytes / 2**20:.1f} MiB), {len(notes)} samples")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument(
        "--destination",
        type=Path,
        default=data_directory(),
        help="where Wren reads its library (default: %(default)s)",
    )
    parser.add_argument(
        "--source", type=Path, help="a folder holding the 22 AIFF files, already downloaded"
    )
    parser.add_argument("--keep-source", action="store_true", help="keep the downloaded files")
    parser.add_argument("--jobs", type=int, default=DEFAULT_JOBS, help="runs converted in parallel")
    arguments = parser.parse_args()
    if shutil.which("ffmpeg") is None:
        raise SystemExit("ffmpeg is required")
    library = arguments.destination.resolve()
    library.parent.mkdir(parents=True, exist_ok=True)
    if arguments.source is not None:
        source = arguments.source.resolve()
        if not source.is_dir():
            raise SystemExit(f"{source} is not a directory")
    else:
        source = (library.parent / DEFAULT_SOURCE_NAME).resolve()
        print(f"downloading {len(SOURCES)} recordings from {INDEX_URL}")
        fetch(source)
    verify(source)
    print(f"all {len(SOURCES)} recordings match their pinned SHA-256")
    partial = library.with_name(library.name + ".partial")
    if partial.exists():
        shutil.rmtree(partial)
    build(source, partial, max(1, arguments.jobs))
    previous = library.with_name(library.name + ".previous")
    if previous.exists():
        shutil.rmtree(previous)
    if library.exists():
        library.rename(previous)
    partial.rename(library)
    if previous.exists():
        shutil.rmtree(previous)
    if not arguments.keep_source and arguments.source is None:
        shutil.rmtree(source)
    print(f"installed {NAME} in {library}\n{ATTRIBUTION}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
