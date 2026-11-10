#!/usr/bin/env python3
"""Download Naked Drums and DrumGizmo's DRSKit and install them as Stick's kits.

Stick (ADR-0114, ADR-0121) plays two multi-mic acoustic kits, each under the
Creative Commons Attribution 4.0 International licence:

- Naked Drums (the default): Wilkinson Audio's Yamaha Recording Custom,
  through Neve and API preamps, in kinwie's SFZ conversion
  (github.com/sfzinstruments/WilkinsonAudio.NakedDrums). Five velocity
  layers and ten round robins on the kick and both snares; five toms; on 16
  channels of mics.
- DRS Studio: DRSKit 2.1, a DRSDrums maple kit by Jes Eiler, recorded by the
  DrumGizmo team on 13 mics, with brushes (whiskers) as well as sticks.

For each kit this script:

1. fetches it: Naked Drums with git at a pinned commit (git checks every
   object's hash; the commit and its tree are checked against the pins), the
   DRSKit zip from drumgizmo.org (resuming a partial download) checked against
   the MD5 its page publishes;
2. reads its mapping as data (Naked's SFZ text with its #define and #include,
   DRSKit's drumkit and instrument XML; nothing in either is run) and maps the
   hits Stick plays onto its pieces, articulations and strikers, and the
   kit's mics onto its mic groups (tables below). Naked's per-mic trims and
   tunings (its toms 3 and 4) are applied as its SFZ applies them; its two
   snare-top mics are summed, and its five tom mics share one channel, since
   each tom was recorded on its own mic only;
3. decodes every hit with ffmpeg, resampled to 48 kHz (SoX resampler when
   ffmpeg has it), every mic in the kit's channel order;
4. measures each hit on its own close mics (the overheads for a cymbal): a
   hit with no stroke in it (its peak under -70 dBFS) or two strokes (its
   attack envelope, the steepest slope per 5 ms held over 15 ms, rising
   12 dB over the dip after the first stroke, within 24 dB of its peak) is
   dropped; its power is the RMS of its first 50 ms;
5. trims the silence before the stroke to 1 ms with a fade-in, and the tail
   where every channel has fallen 60 dB under the hit's peak (or to twice
   the recording's noise floor), at most 20 s, with a 20 ms fade-out;
6. keeps at most 64 hits per stroke, spread evenly over their powers;
7. normalises each hit to its peak, dithers it to 16-bit and writes
   `stick.pack` (little-endian int16, every mic interleaved, each hit aligned
   to 4 KiB), `stick.manifest`, `sources.tsv` (each hit's source files),
   ATTRIBUTION.txt and the licence's link into the kit's folder.

The manifest's first line is `oscillate stick library 1`, then `name`,
`attribution`, `rate 48000`, `channels <mics>`, `pack stick.pack <bytes>`,
one line per mic and one per hit:

    mic <channel> <group> <piece[+piece...]|-> <mono|left|right|centre>
    hit <piece> <articulation> <stick|brush> <offset> <frames> <scale> <power_db>

Groups are kick_in, kick_out, kick_sub, snare_top, snare_bottom, toms, hihat,
ride, overheads, room, far_room, mid_side and china_oh. Pieces are kick,
snare, hihat, tom1-tom5, crash1, crash2, ride, splash and china;
articulations hit, alt, sidestick, bell, closed, half, open and pedal. A
close mic names the pieces recorded on it and is mono (placed by its
piece's pan); the other mics are the left or right of a pair, or a single
mic in the centre (Naked's close room). `offset` is the hit's first frame
in the pack, `scale` turns an int16 back into the recorded level and
`power_db` is the hit's power at it.

The kits are installed under $OSCILLATE_STICK_LIBRARY when set, otherwise
$XDG_DATA_HOME/oscillate/instruments/stick (~/.local/share when XDG_DATA_HOME
is unset), each in its own folder; Stick looks in the same place. A kit
replaces an old one only once it is complete. The downloads are kept in
`downloads/` there unless --remove-downloads is given.

Requires git and ffmpeg. Run with `python3 -I tools/stick/install_kits.py [--kit naked]`.
"""

from __future__ import annotations

import argparse
import array
import concurrent.futures
import hashlib
import math
import operator
import os
import random
import re
import shutil
import subprocess
import sys
import urllib.request
import xml.etree.ElementTree as ElementTree
import zipfile
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

# ---- The installed format -----------------------------------------------------------------------

FORMAT_LINE = "oscillate stick library 1"
MANIFEST = "stick.manifest"
PACK = "stick.pack"
SOURCES = "sources.tsv"
ATTRIBUTION_FILE = "ATTRIBUTION.txt"
RATE = 48_000
ALIGN_BYTES = 4_096
BYTES_PER_SAMPLE = 2
BYTES_PER_DOUBLE = 8
INT16_MAX = 32_767
INT16_MIN = -32_768
# Each hit is normalised to this share of full scale before dithering.
PEAK_SHARE = 0.98
LICENCE = "Creative Commons Attribution 4.0 International"
LICENCE_SHORT = "CC BY 4.0"
LICENCE_URL = "https://creativecommons.org/licenses/by/4.0/"

# ---- Measuring and trimming ---------------------------------------------------------------------

MILLISECOND = RATE // 1_000
DECIBELS = 20.0
POWER_DECIBELS = 10.0
TINY_POWER = 1e-30
CENTS_PER_OCTAVE = 1_200.0
# A hit whose own mics never reach this has no stroke in it.
EMPTY_PEAK_DB = -70.0
# The stroke starts where its own mics first reach this far under their peak.
ONSET_DB = -30.0
PRE_ROLL_FRAMES = 1 * MILLISECOND
# A second stroke: the attack envelope (the peak of the signal's slope, so a
# kick's slow beating is not taken for strokes, held over 15 ms) rises this
# far over the dip after the first stroke, within this far of its peak.
ONSET_BLOCK_FRAMES = 5 * MILLISECOND
HOLD_BLOCKS = 3
DOUBLE_RISE_DB = 12.0
DOUBLE_FLOOR_DB = -24.0
# The first stroke peaks within this many blocks of reaching that floor.
ATTACK_BLOCKS = 4
# Power: the RMS of the hit's own mics over its first 50 ms.
POWER_FRAMES = 50 * MILLISECOND
# The tail ends where every channel's 10 ms peak has fallen this far under
# the hit's peak, or to twice the recording's noise floor (its last 100 ms).
TAIL_BLOCK_FRAMES = 10 * MILLISECOND
TAIL_DB = -60.0
NOISE_FRAMES = 100 * MILLISECOND
NOISE_MARGIN = 2.0
FADE_OUT_FRAMES = 20 * MILLISECOND
MAX_HIT_SECONDS = 20
MAX_HIT_FRAMES = MAX_HIT_SECONDS * RATE
MAX_HITS_PER_STROKE = 64
# Bounds on the untrusted kit data.
MAX_XML_BYTES = 4 << 20
MAX_SFZ_BYTES = 1 << 20
MAX_AUDIO_BYTES = 512 << 20
MAX_INCLUDE_DEPTH = 8
MAX_SUBSTITUTION_PASSES = 4

DEFAULT_JOBS = 4
# What Stick keeps in memory of each hit (stick_library.hpp), for the report.
RESIDENT_FRAMES = 4_096
DOWNLOAD_CHUNK = 1 << 20
# A stalled download gives up after this long without data.
DOWNLOAD_TIMEOUT_SECONDS = 60
DOWNLOADS = "downloads"

STICK = "stick"
BRUSH = "brush"

# ---- Mics ---------------------------------------------------------------------------------------


@dataclass(frozen=True)
class Mic:
    group: str
    pieces: tuple[str, ...]  # The pieces a close mic hears; none for a pair.
    side: str


def close(group: str, *pieces: str) -> Mic:
    return Mic(group, pieces, "mono")


def left(group: str) -> Mic:
    return Mic(group, (), "left")


def right(group: str) -> Mic:
    return Mic(group, (), "right")


def centre(group: str) -> Mic:
    return Mic(group, (), "centre")


def mic_line(index: int, mic: Mic) -> str:
    return f"mic {index} {mic.group} {'+'.join(mic.pieces) or '-'} {mic.side}"


# ---- Naked Drums (SFZ, git) ---------------------------------------------------------------------

NAKED_REPOSITORY = "https://github.com/sfzinstruments/WilkinsonAudio.NakedDrums.git"
NAKED_SOURCE_URL = "https://github.com/sfzinstruments/WilkinsonAudio.NakedDrums"
NAKED_COMMIT = "407732a548606ac715ad8f3fbac63d9f4df77c0d"
NAKED_TREE = "1cabc9919668f4e957cb45f02e83ed052071286b"
# The commit that made the repository's licence CC BY 4.0, removing a
# NonCommercial term "as we have confirmation from Wilkinson that this is OK".
NAKED_LICENCE_COMMIT = "51a7032d4913529dac06bb485fc0768cc3d11da3"
NAKED_ROOT = "Wilkinson Audio/Naked Drums"
NAKED_INSTRUMENT = f"{NAKED_ROOT}/Stereo/Naked Drums.sfz"
NAKED_EXPECTED_SAMPLES = 3_840
NAKED_LICENCE_FIRST_LINE = "Attribution 4.0 International"
NAKED_NOTICE = (
    "Naked Drums — audio samples from WilkinsonAudio.NakedDrums. Creator: Wilkinson Audio. "
    f"Source: {NAKED_SOURCE_URL}. Licence: {LICENCE} ({LICENCE_SHORT}), {LICENCE_URL}. "
    f"The samples are redistributed under {LICENCE_SHORT} and may have been modified for use "
    "in Stick (trimming, format conversion, resampling, level normalisation, processing). "
    "They remain under their original licence."
)

# Naked's channels: its mics, the two snare-top mics summed and the five tom
# mics on one channel (each tom was recorded on its own mic only); its close
# room is one mic.
NAKED_MICS: list[Mic] = [
    close("kick_in", "kick"),
    close("kick_sub", "kick"),
    close("snare_top", "snare"),
    close("snare_bottom", "snare"),
    close("toms", "tom1", "tom2", "tom3", "tom4", "tom5"),
    close("hihat", "hihat"),
    close("ride", "ride"),
    left("overheads"),
    right("overheads"),
    centre("room"),
    left("far_room"),
    right("far_room"),
    left("mid_side"),
    right("mid_side"),
    left("china_oh"),
    right("china_oh"),
]
# Each of the SFZ's mic files (Data/stereo/<layer>_<part>.txt) plays these
# channels: the overheads' and rooms' by their layer, a close mic's by its
# part.
NAKED_FAR = {"OH": (7, 8), "CR": (9,), "FR": (10, 11), "MS": (12, 13), "CH": (14, 15)}
NAKED_CLOSE = {
    "Kick_In": (0,),
    "Kick_Sub": (1,),
    "Snare_Tops": (2,),
    "Snare_Bot": (3,),
    "Tom1": (4,),
    "Tom2": (4,),
    "Tom3": (4,),
    "Tom4": (4,),
    "Tom5": (4,),
    "Hats": (5,),
    "Ride": (6,),
}
# A close mic file's kit piece, as the pairs' files name it.
NAKED_CLOSE_PART = {
    "Kick_In": "Kick",
    "Kick_Sub": "Kick",
    "Snare_Tops": "Snare",
    "Snare_Bot": "Snare",
}
# (part, group label, key define) -> (piece, articulation); None leaves it
# out. A key define of None matches any key.
NAKED_STROKES: dict[tuple[str, str, str | None], tuple[str, str] | None] = {
    ("Kick", "Center", None): ("kick", "hit"),
    ("Snare", "Sidestick", None): ("snare", "sidestick"),
    ("Snare", "Center", "$KEY_SNR1"): ("snare", "hit"),
    # The second snare (key 40 in the SFZ): Stick's snare `alt`.
    ("Snare", "Center", "$KEY_SNR2"): ("snare", "alt"),
    ("Tom1", "Center", None): ("tom1", "hit"),
    ("Tom2", "Center", None): ("tom2", "hit"),
    ("Tom3", "Center", None): ("tom3", "hit"),
    ("Tom4", "Center", None): ("tom4", "hit"),
    ("Tom5", "Center", None): ("tom5", "hit"),
    ("Hats", "Pedal", None): ("hihat", "pedal"),
    # Tight (bow) and closed (edge) are both closed hats: one pool.
    ("Hats", "Tight", None): ("hihat", "closed"),
    ("Hats", "Closed", None): ("hihat", "closed"),
    ("Hats", "Half", None): ("hihat", "half"),
    ("Hats", "Open", None): ("hihat", "open"),
    ("Ride", "Bow", None): ("ride", "hit"),
    ("Ride", "Bell", None): ("ride", "bell"),
    ("Crash1", "Edge", None): ("crash1", "hit"),
    ("Crash2", "Edge", None): ("crash2", "hit"),
    ("Splash1", "Edge", None): ("splash", "hit"),
    ("China1", "Edge", None): ("china", "hit"),
    # No General MIDI key: the crash bells, the second splash and china.
    ("Crash1", "Bell", None): None,
    ("Crash2", "Bell", None): None,
    ("Splash2", "Edge", None): None,
    ("China2", "Edge", None): None,
}
NAKED_SNARE_KEYS = ("$KEY_SNR1", "$KEY_SNR2")
# Hits per stroke the pinned SFZ maps (a check that it was read right).
NAKED_EXPECTED_HITS = {
    ("kick", "hit"): 50,
    ("snare", "hit"): 50,
    ("snare", "alt"): 50,
    ("snare", "sidestick"): 10,
    ("tom1", "hit"): 30,
    ("tom2", "hit"): 30,
    ("tom3", "hit"): 30,
    ("tom4", "hit"): 30,
    ("tom5", "hit"): 30,
    ("hihat", "pedal"): 10,
    ("hihat", "closed"): 60,
    ("hihat", "half"): 30,
    ("hihat", "open"): 30,
    ("ride", "hit"): 30,
    ("ride", "bell"): 10,
    ("crash1", "hit"): 30,
    ("crash2", "hit"): 30,
    ("splash", "hit"): 20,
    ("china", "hit"): 30,
}
# The SFZ's hi-hat keyswitch: Stick plays the non-variable mapping (the
# variable one maps the same samples to the hat pedal controller).
NAKED_KEYSWITCH = "$SW_NON"


# ---- DRSKit (DrumGizmo, zip) --------------------------------------------------------------------

DRS_URL = "https://drumgizmo.org/kits/DRSKit/DRSKit2_1.zip"
DRS_MD5 = "8c4d4b61ad9d354b3b845edd5da9c133"
DRS_SIZE = 2_803_397_710
DRS_DRUMKIT = "DRSKit/DRSKit_full.xml"
# The kit's channels by DrumGizmo name.
DRS_MICS: dict[str, Mic] = {
    "AmbL": left("room"),
    "AmbR": right("room"),
    "Kdrum_back": close("kick_out", "kick"),
    "Kdrum_front": close("kick_in", "kick"),
    "Hihat": close("hihat", "hihat"),
    "OHL": left("overheads"),
    "OHR": right("overheads"),
    "Ride": close("ride", "ride"),
    "Snare_bottom": close("snare_bottom", "snare"),
    "Snare_top": close("snare_top", "snare"),
    "Tom1": close("toms", "tom1"),
    "Tom2": close("toms", "tom2"),
    "Tom3": close("toms", "tom3"),
}
# DrumGizmo instrument -> (piece, articulation, striker). Instruments
# recorded twice for one stroke (tip and shank) join one pool.
DRS_INSTRUMENTS: dict[str, tuple[str, str, str]] = {
    "Kdrum_with_contact": ("kick", "hit", STICK),
    "Kdrum_without_contact": ("kick", "alt", STICK),
    "Snare": ("snare", "hit", STICK),
    "Snare_rim": ("snare", "sidestick", STICK),
    "Snare_whisker": ("snare", "hit", BRUSH),
    "Hihat_closed": ("hihat", "closed", STICK),
    "Hihat_closed_shank": ("hihat", "closed", STICK),
    "Hihat_semi_open": ("hihat", "half", STICK),
    "Hihat_open": ("hihat", "open", STICK),
    "Hihat_open_tip": ("hihat", "open", STICK),
    "Hihat_foot": ("hihat", "pedal", STICK),
    "Hihat_closed_whisker": ("hihat", "closed", BRUSH),
    "Hihat_open_whisker": ("hihat", "open", BRUSH),
    "Tom1": ("tom1", "hit", STICK),
    "Tom2": ("tom2", "hit", STICK),
    "Tom3": ("tom3", "hit", STICK),
    "Tom1_whisker": ("tom1", "hit", BRUSH),
    "Tom2_whisker": ("tom2", "hit", BRUSH),
    "Tom3_whisker": ("tom3", "hit", BRUSH),
    "Crash_left_tip": ("crash1", "hit", STICK),
    "Crash_left_shank": ("crash1", "hit", STICK),
    "Crash_left_whisker": ("crash1", "hit", BRUSH),
    "Crash_right_tip": ("crash2", "hit", STICK),
    "Crash_right_shank": ("crash2", "hit", STICK),
    "Crash_right_whisker": ("crash2", "hit", BRUSH),
    "Ride_tip": ("ride", "hit", STICK),
    "Ride_shank": ("ride", "hit", STICK),
    "Ride_tip_bell": ("ride", "bell", STICK),
    "Ride_shank_bell": ("ride", "bell", STICK),
    "Ride_whisker": ("ride", "hit", BRUSH),
}


@dataclass(frozen=True)
class Kit:
    flag: str
    folder: str
    name: str
    attribution: str  # The manifest's line: the library popup shows it.
    notice: str  # ATTRIBUTION.txt's opening.
    source: str  # Where it comes from and its licence, for ATTRIBUTION.txt.
    changes: tuple[str, ...]  # What the conversion changes, for ATTRIBUTION.txt.


COMMON_CHANGES = (
    "resampled to 48 kHz",
    "each hit trimmed to 1 ms before its stroke and to its tail, with fades",
    "normalised per hit and dithered to 16-bit",
    "hits with no stroke or two strokes left out",
)
DRS_ATTRIBUTION = (
    "DRSKit 2.1 by Jes Eiler (DRSDrums) and the DrumGizmo team, "
    f"drumgizmo.org, {LICENCE} ({LICENCE_URL})"
)

KITS: dict[str, Kit] = {
    "naked": Kit(
        flag="naked",
        folder="naked-drums",
        name="Naked Drums",
        attribution=NAKED_NOTICE,
        notice=(
            f"{NAKED_NOTICE}\n\n"
            "Recorded by Wilkinson Audio (Yamaha Recording Custom; Neve 1073 and 1081, BAE 1073 "
            "and API 512 preamps). SFZ conversion by kinwie."
        ),
        source=(
            f"Source: {NAKED_SOURCE_URL}, commit {NAKED_COMMIT} (tree {NAKED_TREE}).\n"
            f"Licence: the repository's LICENSE, {LICENCE}, {LICENCE_URL}, as made in its commit "
            f"{NAKED_LICENCE_COMMIT} (2020-07-01), which removed a NonCommercial term "
            '"as we have confirmation from Wilkinson that this is OK".'
        ),
        changes=(
            *COMMON_CHANGES,
            "the SFZ's per-mic trims and tom tunings applied",
            "the two snare-top mics summed",
            "the five tom mics carried on one channel",
            "the crash bells and chokes, the second splash and the second china left out",
        ),
    ),
    "drs": Kit(
        flag="drs",
        folder="drs-studio",
        name="DRS Studio (DRSKit 2.1)",
        attribution=DRS_ATTRIBUTION,
        notice=DRS_ATTRIBUTION,
        source=f"Source: {DRS_URL} (MD5 {DRS_MD5}).\nLicence: {LICENCE}, {LICENCE_URL}",
        changes=COMMON_CHANGES,
    ),
}


# ---- Locations and fetching ---------------------------------------------------------------------


def data_directory() -> Path:
    override = os.environ.get("OSCILLATE_STICK_LIBRARY")
    if override:
        return Path(override)
    base = os.environ.get("XDG_DATA_HOME") or str(Path.home() / ".local" / "share")
    return Path(base) / "oscillate" / "instruments" / "stick"


def run(command: list[str], cwd: Path | None = None) -> str:
    result = subprocess.run(command, cwd=cwd, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise SystemExit(f"{' '.join(command)} failed:\n{result.stderr.strip()}")
    return result.stdout.strip()


def fetch_naked(source: Path) -> None:
    """The SFZ repository at its pinned commit, checked."""
    if not (source / ".git").is_dir():
        source.mkdir(parents=True, exist_ok=True)
        print(f"fetching {NAKED_REPOSITORY} at {NAKED_COMMIT[:12]} (about 1.3 GB)")
        run(["git", "init", "-q"], source)
        run(["git", "remote", "add", "origin", NAKED_REPOSITORY], source)
        run(["git", "fetch", "-q", "--depth", "1", "origin", NAKED_COMMIT], source)
        run(["git", "-c", "advice.detachedHead=false", "checkout", "-q", "FETCH_HEAD"], source)
    if run(["git", "rev-parse", "HEAD"], source) != NAKED_COMMIT:
        raise SystemExit("the Naked Drums checkout is not the pinned commit")
    if run(["git", "rev-parse", "HEAD^{tree}"], source) != NAKED_TREE:
        raise SystemExit("the Naked Drums checkout's tree is not the pinned tree")
    if run(["git", "status", "--porcelain", "--untracked-files=no"], source):
        raise SystemExit("the Naked Drums checkout has modified files")
    samples = list((source / NAKED_ROOT / "Samples").glob("*.flac"))
    if len(samples) != NAKED_EXPECTED_SAMPLES:
        raise SystemExit(f"expected {NAKED_EXPECTED_SAMPLES} samples, found {len(samples)}")
    licence = (source / "LICENSE").read_text(errors="replace").splitlines()
    if not licence or licence[0].strip() != NAKED_LICENCE_FIRST_LINE:
        raise SystemExit("the Naked Drums licence is not CC BY 4.0")


def md5_of(path: Path) -> str:
    digest = hashlib.md5(usedforsecurity=False)
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(DOWNLOAD_CHUNK), b""):
            digest.update(chunk)
    return digest.hexdigest()


def download_drs(folder: Path) -> Path:
    """DRSKit's zip, downloaded (or resumed) and checked against its pinned MD5."""
    folder.mkdir(parents=True, exist_ok=True)
    target = folder / PurePosixPath(DRS_URL).name
    have = target.stat().st_size if target.exists() else 0
    if have < DRS_SIZE:
        request = urllib.request.Request(DRS_URL, headers={"Range": f"bytes={have}-"})
        print(f"downloading {DRS_URL} ({DRS_SIZE / 2**20:.0f} MiB) to {target}")
        with (
            urllib.request.urlopen(request, timeout=DOWNLOAD_TIMEOUT_SECONDS) as response,
            target.open("ab") as file,
        ):
            if have and response.status != 206:
                file.truncate(0)
                have = 0
            reported = 0
            while chunk := response.read(DOWNLOAD_CHUNK):
                file.write(chunk)
                have += len(chunk)
                percent = have * 100 // DRS_SIZE
                if percent >= reported + 10:
                    reported = percent - percent % 10
                    print(f"  {reported} %")
    if target.stat().st_size != DRS_SIZE or md5_of(target) != DRS_MD5:
        target.unlink()
        raise SystemExit(
            f"{target.name} does not match its published MD5: downloaded again next time"
        )
    return target


# ---- Hits ---------------------------------------------------------------------------------------


@dataclass(frozen=True)
class Part:
    """One recorded channel that plays into a kit channel."""

    path: str  # A zip member, or a file under the source folder.
    channel: int  # In that file, from 0.
    gain_db: float = 0.0
    cents: float = 0.0


@dataclass
class Hit:
    kit: str
    source: str  # The zip, or the folder the paths are under.
    zipped: bool
    name: str  # Unique in the kit: seeds its dither.
    piece: str
    articulation: str
    striker: str
    # Per kit channel: what plays into it (nothing: silence).
    channels: list[list[Part]]
    work: str = ""
    frames: int = 0
    scale: float = 0.0
    power_db: float = 0.0
    dropped: str = ""
    offset: int = 0


# ---- DRSKit's XML -------------------------------------------------------------------------------


def member(archive: zipfile.ZipFile, base: PurePosixPath, relative: str) -> str:
    """A path the kit names, resolved inside the zip; anything outside it is refused."""
    parts: list[str] = []
    for part in (base / relative).parts:
        if part in ("", "."):
            continue
        if part == "..":
            if not parts:
                raise SystemExit(f"the kit names a file outside it: {relative}")
            parts.pop()
        else:
            parts.append(part)
    name = "/".join(parts)
    if name not in archive.NameToInfo:
        raise SystemExit(f"the kit names a file it does not hold: {name}")
    return name


def read_xml(archive: zipfile.ZipFile, name: str) -> ElementTree.Element:
    """A kit XML file, bounded in size; one declaring a document type or
    entities (which no kit needs, and which could expand without bound) is
    refused."""
    info = archive.getinfo(name)
    if info.file_size > MAX_XML_BYTES:
        raise SystemExit(f"{name} is too large")
    data = archive.read(info)
    if b"<!DOCTYPE" in data or b"<!ENTITY" in data:
        raise SystemExit(f"{name} declares a document type")
    return ElementTree.fromstring(data)


def drs_hits(archive_path: Path) -> tuple[list[Mic], list[Hit]]:
    """DRSKit's mics in its channel order and every hit Stick plays, from its XML."""
    with zipfile.ZipFile(archive_path) as archive:
        drumkit = read_xml(archive, DRS_DRUMKIT)
        base = PurePosixPath(DRS_DRUMKIT).parent
        order = [c.get("name", "") for c in drumkit.iter("channel")]
        if set(order) != set(DRS_MICS):
            raise SystemExit(f"DRSKit: unexpected channels {order}")
        hits: list[Hit] = []
        seen: set[str] = set()
        for instrument in drumkit.iter("instrument"):
            name = instrument.get("name", "")
            if name not in DRS_INSTRUMENTS or not instrument.get("file"):
                continue
            seen.add(name)
            piece, articulation, striker = DRS_INSTRUMENTS[name]
            file = member(archive, base, instrument.get("file", ""))
            # An instrument's own map names the kit's channel each of its
            # file channels plays (DRSKit maps 1:1).
            mapped = {m.get("in", ""): m.get("out", "") for m in instrument.iter("channelmap")}
            folder = PurePosixPath(file).parent
            for sample in read_xml(archive, file).iter("sample"):
                files: dict[str, Part] = {}
                for audio in sample.iter("audiofile"):
                    channel = mapped.get(audio.get("channel", ""), audio.get("channel", ""))
                    index = int(audio.get("filechannel", "1")) - 1
                    if index < 0:
                        raise SystemExit(f"{name}: a file channel below 1")
                    files[channel] = Part(member(archive, folder, audio.get("file", "")), index)
                if not all(c in files for c in order):
                    raise SystemExit(f"{name} {sample.get('name')}: a mic is missing")
                hits.append(
                    Hit(
                        kit="drs",
                        source=str(archive_path),
                        zipped=True,
                        name=f"{name}/{sample.get('name', '')}",
                        piece=piece,
                        articulation=articulation,
                        striker=striker,
                        channels=[[files[c]] for c in order],
                    )
                )
        missing = set(DRS_INSTRUMENTS) - seen
        if missing:
            raise SystemExit(f"DRSKit: instruments not found: {sorted(missing)}")
    return [DRS_MICS[c] for c in order], hits


# ---- Naked's SFZ --------------------------------------------------------------------------------


class SfzText:
    """SFZ text as data: comments removed, #define applied in order and
    #include followed (inside the source folder, relative to the
    instrument's folder, as SFZ players resolve them)."""

    def __init__(self, root: Path, instrument_folder: Path) -> None:
        self.root = root.resolve()
        self.folder = instrument_folder
        self.defines: dict[str, str] = {}
        self._pattern: re.Pattern[str] | None = None

    def resolve(self, relative: str) -> Path:
        path = (self.folder / relative).resolve()
        if not path.is_relative_to(self.root) or not path.is_file():
            raise SystemExit(f"the SFZ names a file outside it or missing: {relative}")
        return path

    def substitute(self, text: str) -> str:
        """Defines replaced, the longest name first (SFZ names run together:
        $MIC_$ART_$VEL1)."""
        if "$" not in text or not self.defines:
            return text
        if self._pattern is None:
            names = sorted(self.defines, key=len, reverse=True)
            self._pattern = re.compile("|".join(re.escape(n) for n in names))
        for _ in range(MAX_SUBSTITUTION_PASSES):
            replaced = self._pattern.sub(lambda m: self.defines[m.group(0)], text)
            if replaced == text:
                break
            text = replaced
        return text

    def lines(self, path: Path, depth: int = 0) -> list[str]:
        if depth > MAX_INCLUDE_DEPTH:
            raise SystemExit("the SFZ includes too deeply")
        if path.stat().st_size > MAX_SFZ_BYTES:
            raise SystemExit(f"{path.name} is too large")
        text = path.read_text(encoding="utf-8", errors="replace")
        text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
        out: list[str] = []
        for raw in text.splitlines():
            line = raw.split("//", 1)[0]
            definitions = re.findall(r"#define\s+(\$\w+)\s+(\S+)", line)
            if definitions:
                for name, value in definitions:
                    if name not in self.defines:
                        self._pattern = None
                    self.defines[name] = value
                continue
            include = re.match(r'\s*#include\s+"([^"]+)"', line)
            if include:
                out.extend(self.lines(self.resolve(self.substitute(include.group(1))), depth + 1))
                continue
            if line.strip():
                out.append(self.substitute(line))
        return out


def sfz_opcodes(text: str) -> dict[str, str]:
    """A header's opcodes (a value may hold spaces: region_label=10 one)."""
    out: dict[str, str] = {}
    parts = re.split(r"(?:^|\s+)([A-Za-z_]\w*)=", " " + text.strip())
    for j in range(1, len(parts), 2):
        out[parts[j]] = parts[j + 1].strip()
    return out


def sfz_regions(lines: list[str]) -> list[dict[str, str]]:
    """Every region with the opcodes it inherits (global, master, group)."""
    order = ["global", "master", "group"]
    levels: dict[str, dict[str, str]] = {level: {} for level in order}
    tokens = re.split(r"<(\w+)>", "\n".join(lines))
    regions: list[dict[str, str]] = []
    for i in range(1, len(tokens), 2):
        header, body = tokens[i], sfz_opcodes(tokens[i + 1])
        if header in order:
            levels[header] = body
            for lower in order[order.index(header) + 1 :]:
                levels[lower] = {}
        elif header == "region":
            region: dict[str, str] = {}
            for level in order:
                region.update(levels[level])
            region.update(body)
            regions.append(region)
        # <control>, <curve>, <effect> and the like play no hit.
    return regions


def sfz_number(value: str, what: str) -> float:
    try:
        return float(value)
    except ValueError as error:
        raise SystemExit(f"the SFZ's {what} is not a number: {value}") from error


def naked_stroke(part: str, label: str, key: str, keys: dict[str, str]) -> tuple[str, str] | None:
    """The piece and articulation a region of the SFZ records."""
    for define in NAKED_SNARE_KEYS:
        if (part, label, define) in NAKED_STROKES and keys[define] == key:
            return NAKED_STROKES[(part, label, define)]
    if (part, label, None) not in NAKED_STROKES:
        raise SystemExit(f"Naked Drums: unexpected stroke {part} {label} (key {key})")
    return NAKED_STROKES[(part, label, None)]


def naked_hits(source: Path, expected: dict[tuple[str, str], int] | None = None) -> list[Hit]:
    """Every hit Stick plays from the Naked Drums SFZ, each with its mics;
    the hits per stroke must be `expected` (the pinned SFZ's by default)."""
    instrument = source / NAKED_INSTRUMENT
    reader = SfzText(source, instrument.parent)
    main = reader.lines(instrument)  # Every define, from its keymap and macros.
    default_path = ""
    for chunk in re.split(r"<\w+>", "\n".join(main)):
        default_path = sfz_opcodes(chunk).get("default_path", default_path)
    keyswitch = reader.defines[NAKED_KEYSWITCH]
    keys = {define: reader.defines[define] for define in NAKED_SNARE_KEYS}
    mic_files = re.findall(
        r'#include\s+"(\.\./Data/stereo/(\w+)\.txt)"', instrument.read_text(errors="replace")
    )
    hits: dict[tuple[str, ...], Hit] = {}
    used: set[tuple[str, Path]] = set()
    for relative, stem in mic_files:
        layer, _, part = stem.partition("_")
        if layer == "DI":
            channels = NAKED_CLOSE[part]
            part = NAKED_CLOSE_PART.get(part, part)
        else:
            channels = NAKED_FAR[layer]
        for region in sfz_regions(reader.lines(reader.resolve(relative))):
            sample = region.get("sample", "")
            if sample.startswith("*") or region.get("trigger") == "release":
                continue  # Silence and the cymbal chokes.
            if region.get("sw_last", keyswitch) != keyswitch:
                continue  # The variable hi-hat mode: the same samples again.
            label = region.get("group_label", "")
            key = region.get("key", "")
            stroke = naked_stroke(part, label, key, keys)
            if stroke is None:
                continue
            path = reader.resolve(default_path + sample)
            if (stem, path) in used:
                continue
            used.add((stem, path))
            gain_db = sum(
                sfz_number(region.get(name, "0"), name)
                for name in ("master_volume", "group_volume", "volume")
            )
            cents = sfz_number(region.get("tune", "0"), "tune")
            lovel = region.get("lovel", "")
            identity = (*stroke, label, key, lovel, region.get("seq_position", ""))
            hit = hits.get(identity)
            if hit is None:
                hit = Hit(
                    kit="naked",
                    source=str(reader.root),
                    zipped=False,
                    name="/".join(identity),
                    piece=stroke[0],
                    articulation=stroke[1],
                    striker=STICK,
                    channels=[[] for _ in NAKED_MICS],
                )
                hits[identity] = hit
            relative_path = path.relative_to(reader.root).as_posix()
            for index, channel in enumerate(channels):
                hit.channels[channel].append(Part(relative_path, index, gain_db, cents))
    counts: dict[tuple[str, str], int] = {}
    for hit in hits.values():
        counts[(hit.piece, hit.articulation)] = counts.get((hit.piece, hit.articulation), 0) + 1
        if len({p.cents for parts in hit.channels for p in parts}) != 1:
            raise SystemExit(f"Naked Drums: {hit.name} is tuned differently on its mics")
    if counts != (NAKED_EXPECTED_HITS if expected is None else expected):
        raise SystemExit(f"Naked Drums: unexpected hits {sorted(counts.items())}")
    return list(hits.values())


# ---- Decoding and measuring ---------------------------------------------------------------------


def resampler() -> str:
    """ffmpeg's SoX resampler when it has one, else its own at a long filter."""
    filters = subprocess.run(
        ["ffmpeg", "-hide_banner", "-h", "filter=aresample"],
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    if "soxr" in filters:
        return f"aresample={RATE}:resampler=soxr:precision=28"
    return f"aresample={RATE}:filter_size=64:cutoff=0.97"


def probe(data: bytes | None, path: Path | None) -> tuple[int, int]:
    """An audio file's channels and sample rate."""
    command = ["ffprobe", "-v", "error", "-show_entries", "stream=sample_rate,channels"]
    command += ["-of", "csv=p=0", "-i", "pipe:0" if path is None else str(path)]
    out = subprocess.run(command, input=data, capture_output=True, check=True).stdout.decode()
    rate, channels = out.split()[0].split(",")
    return int(channels), int(rate)


def decode(data: bytes | None, path: Path | None, filters: str, cents: float) -> list[array.array]:
    """An audio file's channels as doubles at 48 kHz, tuned by `cents`
    (played faster or slower, as an SFZ player tunes)."""
    channels, rate = probe(data, path)
    chain = filters
    if cents:
        chain = f"asetrate={round(rate * 2.0 ** (cents / CENTS_PER_OCTAVE))},{filters}"
    command = ["ffmpeg", "-nostdin", "-v", "error", "-i"]
    command += ["pipe:0" if path is None else str(path), "-af", chain]
    command += ["-f", "f64le", "-c:a", "pcm_f64le", "-"]
    raw = subprocess.run(command, input=data, capture_output=True, check=True).stdout
    values = array.array("d")
    values.frombytes(raw)
    if sys.byteorder != "little":
        values.byteswap()
    return [values[c::channels] for c in range(channels)]


def read_parts(hit: Hit, filters: str) -> dict[tuple[str, float], list[array.array]]:
    """Every file a hit plays, decoded once."""
    decoded: dict[tuple[str, float], list[array.array]] = {}
    wanted = sorted({(p.path, p.cents) for parts in hit.channels for p in parts})
    if hit.zipped:
        with zipfile.ZipFile(hit.source) as archive:
            for name, cents in wanted:
                info = archive.getinfo(name)
                if info.file_size > MAX_AUDIO_BYTES:
                    raise SystemExit(f"{name} is too large")
                decoded[(name, cents)] = decode(archive.read(info), None, filters, cents)
    else:
        root = Path(hit.source)
        for name, cents in wanted:
            path = (root / name).resolve()
            if not path.is_relative_to(root) or path.stat().st_size > MAX_AUDIO_BYTES:
                raise SystemExit(f"{name} is outside the kit or too large")
            decoded[(name, cents)] = decode(None, path, filters, cents)
    return decoded


def mix_channels(
    hit: Hit, decoded: dict[tuple[str, float], list[array.array]]
) -> list[array.array]:
    """Each kit channel: its parts summed at their gains (silence for none),
    all as long as the longest. Every channel of every file plays."""
    for key, file_channels in decoded.items():
        used = {p.channel for parts in hit.channels for p in parts if (p.path, p.cents) == key}
        if used != set(range(len(file_channels))):
            raise SystemExit(
                f"{key[0]} has {len(file_channels)} channels, the kit plays {sorted(used)}"
            )
    frames = max(len(decoded[(p.path, p.cents)][p.channel]) for c in hit.channels for p in c)
    out = []
    for parts in hit.channels:
        channel = array.array("d", bytes(BYTES_PER_DOUBLE * frames))
        for part in parts:
            source = decoded[(part.path, part.cents)][part.channel]
            gain = 10.0 ** (part.gain_db / DECIBELS)
            mixed = map(operator.add, channel[: len(source)], (x * gain for x in source))
            channel[: len(source)] = array.array("d", mixed)
        out.append(channel)
    return out


def block_peaks(channels: list[array.array], block: int) -> list[float]:
    frames = len(channels[0])
    peaks = []
    for start in range(0, frames, block):
        peak = 0.0
        for channel in channels:
            part = channel[start : start + block]
            peak = max(peak, max(part), -min(part))
        peaks.append(peak)
    return peaks


def attack_envelope(channels: list[array.array]) -> list[float]:
    """Each block's steepest slope over the channels, held over HOLD_BLOCKS."""
    slopes = [array.array("d", map(operator.sub, c[1:], c[:-1])) for c in channels]
    peaks = block_peaks(slopes, ONSET_BLOCK_FRAMES)
    return [max(peaks[max(0, i - HOLD_BLOCKS + 1) : i + 1]) for i in range(len(peaks))]


def two_strokes(peaks: list[float]) -> bool:
    """A second onset: past the first stroke's peak (the loudest of its first
    ATTACK_BLOCKS), a rise of DOUBLE_RISE_DB over the dip since, within
    DOUBLE_FLOOR_DB of the hit's peak."""
    rise = 10.0 ** (DOUBLE_RISE_DB / DECIBELS)
    floor = max(peaks, default=0.0) * 10.0 ** (DOUBLE_FLOOR_DB / DECIBELS)
    struck = next((i for i, value in enumerate(peaks) if value >= floor), len(peaks))
    attack = peaks[struck : struck + ATTACK_BLOCKS]
    if not attack:
        return False
    first_peak = struck + attack.index(max(attack))
    dip = peaks[first_peak]
    for value in peaks[first_peak + 1 :]:
        dip = min(dip, value)
        if value >= floor and value >= dip * rise:
            return True
    return False


def own_channels(hit: Hit, mics: list[Mic], recorded: list[bool]) -> list[int]:
    """The channels a hit is measured on: its close mics that recorded it
    (Naked's hat mic is silent on its pedal), else the overheads."""
    own = [i for i, m in enumerate(mics) if hit.piece in m.pieces and recorded[i]]
    return own or [i for i, m in enumerate(mics) if m.group == "overheads"]


def dithered(part: array.array, gain: float, uniform: Callable[[], float]) -> array.array:
    """A channel scaled to int16 with TPDF dither and the hit's fades."""
    length = len(part)
    fade_out_from = length - FADE_OUT_FRAMES
    return array.array(
        "h",
        (
            min(
                INT16_MAX,
                max(
                    INT16_MIN,
                    round(
                        value * gain * min(1.0, i / PRE_ROLL_FRAMES, (length - i) / FADE_OUT_FRAMES)
                        + uniform()
                        - uniform()
                    ),
                ),
            )
            if i < PRE_ROLL_FRAMES or i >= fade_out_from
            else min(INT16_MAX, max(INT16_MIN, round(value * gain + uniform() - uniform())))
            for i, value in enumerate(part)
        ),
    )


def measure(job: tuple[Hit, list[Mic], str, str]) -> Hit:
    hit, mics, filters, work = job
    channels = mix_channels(hit, read_parts(hit, filters))
    frames = len(channels[0])
    # A mic that did not record the hit (or recorded silence) stays silent, undithered.
    recorded = [bool(parts) and any(c) for c, parts in zip(channels, hit.channels, strict=True)]
    main = [channels[i] for i in own_channels(hit, mics, recorded)]
    onset_peaks = block_peaks(main, MILLISECOND)
    top = max(onset_peaks)
    if top < 10.0 ** (EMPTY_PEAK_DB / DECIBELS):
        hit.dropped = "no stroke"
        return hit
    if two_strokes(attack_envelope(main)):
        hit.dropped = "two strokes"
        return hit
    threshold = top * 10.0 ** (ONSET_DB / DECIBELS)
    onset = next(i for i, value in enumerate(onset_peaks) if value >= threshold) * MILLISECOND
    start = max(0, onset - PRE_ROLL_FRAMES)
    sounding = [c for c, heard in zip(channels, recorded, strict=True) if heard]
    every = block_peaks(sounding, TAIL_BLOCK_FRAMES)
    loudest = max(every)
    noise = max(block_peaks([c[-NOISE_FRAMES:] for c in sounding], NOISE_FRAMES))
    floor = max(loudest * 10.0 ** (TAIL_DB / DECIBELS), noise * NOISE_MARGIN)
    last = max((i for i, value in enumerate(every) if value >= floor), default=0)
    end = min(frames, (last + 1) * TAIL_BLOCK_FRAMES + FADE_OUT_FRAMES, start + MAX_HIT_FRAMES)
    length = end - start
    if length <= PRE_ROLL_FRAMES + FADE_OUT_FRAMES:
        hit.dropped = "too short"
        return hit
    square = sum(x * x for c in main for x in c[onset : onset + POWER_FRAMES])
    hit.power_db = POWER_DECIBELS * math.log10(square / (len(main) * POWER_FRAMES) + TINY_POWER)
    peak = max(max(max(c[start:end]), -min(c[start:end])) for c in sounding)
    gain = PEAK_SHARE * INT16_MAX / peak
    hit.scale = 1.0 / gain
    uniform = random.Random(f"{hit.kit}/{hit.name}").random
    pcm = array.array("h", bytes(BYTES_PER_SAMPLE * length * len(channels)))
    for index, (channel, heard) in enumerate(zip(channels, recorded, strict=True)):
        if heard:
            pcm[index :: len(channels)] = dithered(channel[start:end], gain, uniform)
    if sys.byteorder != "little":
        pcm.byteswap()
    hit.frames = length
    name = hashlib.sha1(hit.name.encode(), usedforsecurity=False)
    hit.work = str(Path(work) / f"{name.hexdigest()}.pcm")
    Path(hit.work).write_bytes(pcm.tobytes())
    return hit


# ---- Choosing and writing -----------------------------------------------------------------------


def choose(hits: list[Hit]) -> list[Hit]:
    """At most MAX_HITS_PER_STROKE per stroke, spread evenly over their powers."""
    strokes: dict[tuple[str, str, str], list[Hit]] = {}
    for hit in hits:
        strokes.setdefault((hit.piece, hit.articulation, hit.striker), []).append(hit)
    kept = []
    for members in strokes.values():
        members.sort(key=lambda h: h.power_db)
        if len(members) > MAX_HITS_PER_STROKE:
            step = (len(members) - 1) / (MAX_HITS_PER_STROKE - 1)
            members = [members[round(i * step)] for i in range(MAX_HITS_PER_STROKE)]
        kept.extend(members)
    return kept


def source_line(hit: Hit) -> str:
    """A hit's place in the pack and every file it was made from, by channel."""
    files = [
        f"{channel}:{part.path}#{part.channel}"
        for channel, parts in enumerate(hit.channels)
        for part in parts
    ]
    return "\t".join([str(hit.offset), hit.piece, hit.articulation, hit.striker, *files])


def write_kit(kit: Kit, mics: list[Mic], hits: list[Hit], target: Path) -> int:
    channels = len(mics)
    frame_bytes = BYTES_PER_SAMPLE * channels
    # Each hit starts on a whole frame and a 4 KiB boundary.
    align = math.lcm(frame_bytes, ALIGN_BYTES)
    offset_bytes = 0
    for hit in hits:
        hit.offset = offset_bytes // frame_bytes
        span = hit.frames * frame_bytes
        offset_bytes += (span + align - 1) // align * align
    with (target / PACK).open("wb") as pack:
        for hit in hits:
            pack.seek(hit.offset * frame_bytes)
            pack.write(Path(hit.work).read_bytes())
        pack.truncate(offset_bytes)
        pack.flush()
        os.fsync(pack.fileno())
    lines = [
        FORMAT_LINE,
        f"name {kit.name}",
        f"attribution {kit.attribution}",
        f"rate {RATE}",
        f"channels {channels}",
        f"pack {PACK} {offset_bytes}",
    ]
    lines += [mic_line(index, mic) for index, mic in enumerate(mics)]
    for hit in hits:
        lines.append(
            f"hit {hit.piece} {hit.articulation} {hit.striker} {hit.offset} {hit.frames} "
            f"{hit.scale:.9e} {hit.power_db:.2f}"
        )
    (target / MANIFEST).write_text("\n".join(lines) + "\n")
    header = "# offset\tpiece\tarticulation\tstriker\tchannel:source file#file channel ..."
    (target / SOURCES).write_text("\n".join([header, *map(source_line, hits)]) + "\n")
    (target / ATTRIBUTION_FILE).write_text(
        f"{kit.notice}\n\n{kit.source}\n\n"
        "Converted for Oscillate's Stick on the machine it was installed on: "
        + "; ".join(kit.changes)
        + f". {SOURCES} names the source files of every hit. "
        "The recordings are otherwise unchanged.\n"
    )
    return offset_bytes


def install(kit: Kit, mics: list[Mic], hits: list[Hit], base: Path, jobs: int) -> Path:
    target = base / kit.folder
    partial = target.with_name(target.name + ".partial")
    if partial.exists():
        shutil.rmtree(partial)
    work = partial / "work"
    work.mkdir(parents=True)
    filters = resampler()
    print(f"{kit.name}: {len(mics)} channels, measuring {len(hits)} hits")
    with concurrent.futures.ProcessPoolExecutor(max(1, jobs)) as pool:
        measured = list(
            pool.map(measure, [(h, mics, filters, str(work)) for h in hits], chunksize=1)
        )
    dropped = [h for h in measured if h.dropped]
    for hit in dropped:
        print(f"  left out {hit.name}: {hit.dropped}")
    chosen = choose([h for h in measured if not h.dropped])
    size = write_kit(kit, mics, chosen, partial)
    shutil.rmtree(work)
    previous = target.with_name(target.name + ".previous")
    if previous.exists():
        shutil.rmtree(previous)
    if target.exists():
        target.rename(previous)
    partial.rename(target)
    if previous.exists():
        shutil.rmtree(previous)
    resident = len(chosen) * len(mics) * RESIDENT_FRAMES * BYTES_PER_SAMPLE
    print(
        f"installed {kit.name} in {target}: {len(chosen)} hits, pack {size / 2**20:.0f} MiB, "
        f"{resident / 2**20:.0f} MiB resident, {len(dropped)} left out"
    )
    return target


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument(
        "--kit",
        choices=[*KITS, "all"],
        default="all",
        help="the kit to install (default: both)",
    )
    parser.add_argument(
        "--destination",
        type=Path,
        default=data_directory(),
        help="the folder Stick reads its kits from (default: %(default)s)",
    )
    parser.add_argument(
        "--remove-downloads", action="store_true", help="delete each download once it is installed"
    )
    parser.add_argument("--jobs", type=int, default=DEFAULT_JOBS, help="hits converted in parallel")
    arguments = parser.parse_args()
    for tool in ("git", "ffmpeg", "ffprobe"):
        if shutil.which(tool) is None:
            raise SystemExit(f"{tool} is required")
    base = arguments.destination.resolve()
    base.mkdir(parents=True, exist_ok=True)
    chosen = list(KITS) if arguments.kit == "all" else [arguments.kit]
    for flag in chosen:
        kit = KITS[flag]
        if flag == "naked":
            download = base / DOWNLOADS / kit.folder
            fetch_naked(download)
            install(kit, NAKED_MICS, naked_hits(download), base, arguments.jobs)
        else:
            download = download_drs(base / DOWNLOADS)
            mics, hits = drs_hits(download)
            install(kit, mics, hits, base, arguments.jobs)
        if arguments.remove_downloads:
            if download.is_dir():
                shutil.rmtree(download)
            else:
                download.unlink()
    print(f"Stick's kits are in {base}. Credit them where the music is credited.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
