#!/usr/bin/env python3
"""Download DrumGizmo's DRSKit, MuldjordKit and ShittyKit and install them as Stick's kits.

Stick (ADR-0114) plays three multi-mic acoustic kits from DrumGizmo.org, each
under the Creative Commons Attribution 4.0 International licence:

- DRS Studio: DRSKit 2.1, a DRSDrums maple kit by Jes Eiler, recorded by the
  DrumGizmo team on 13 mics, with brushes (whiskers) as well as sticks.
- Muldjord: MuldjordKit 3, Lars Muldjord's Tama Superstar on 15 mics (its
  16th channel, the kick drums' trigger signals, is dropped).
- Garage: ShittyKit 1.2, Sardonicus's small Stagg kit on 8 mics.

For each kit this script:

1. downloads its zip from drumgizmo.org (resuming a partial download) and
   checks it against the MD5 the kit's page publishes, pinned below;
2. reads the kit's drumkit XML and each instrument's XML from the zip (they
   are parsed as data; nothing in the zip is run), and maps the instruments
   Stick plays onto its pieces, articulations and strikers, and the kit's
   channels onto its mic groups (tables below);
3. decodes every hit of those instruments with ffmpeg, resampled to 48 kHz,
   every channel in the kit's order: Muldjord's snare bottom is inverted (the
   kit's page asks for it) and Garage's overhead and room pairs are swapped,
   so every kit is heard from the drummer's seat;
4. measures each hit on its own close mics (the overheads for a cymbal): a
   hit with no stroke in it (its peak under -70 dBFS) or two strokes (its
   attack envelope, the steepest slope per 5 ms held over 15 ms, rising
   12 dB over the dip after the first stroke, within 24 dB of its peak) is
   dropped; its power is the RMS of its first 50 ms;
5. trims the silence before the stroke to 1 ms with a fade-in, and the tail
   where every channel has fallen 60 dB under the hit's peak (or to twice
   the recording's noise floor), at most 20 s, with a 20 ms fade-out;
6. keeps at most 48 hits per stroke, spread evenly over their powers;
7. normalises each hit to its peak, dithers it to 16-bit and writes
   `stick.pack` (little-endian int16, every mic interleaved, each hit aligned
   to 4 KiB), `stick.manifest`, ATTRIBUTION.txt and the licence's link into
   the kit's folder.

The manifest's first line is `oscillate stick library 1`, then `name`,
`attribution`, `rate 48000`, `channels <mics>`, `pack stick.pack <bytes>`,
one line per mic and one per hit:

    mic <channel> <kick_in|kick_out|snare_top|snare_bottom|toms|hihat|ride|
        overheads|room> <piece|-> <mono|left|right>
    hit <piece> <articulation> <stick|brush> <offset> <frames> <scale> <power_db>

Pieces are kick, snare, hihat, tom1-tom4, crash1, crash2, ride, ride2 and
china; articulations hit, alt, rimshot, sidestick, bell, closed, half, open
and pedal. `offset` is the hit's first frame in the pack, `scale` turns an
int16 back into the recorded level and `power_db` is the hit's power at it.

The kits are installed under $OSCILLATE_STICK_LIBRARY when set, otherwise
$XDG_DATA_HOME/oscillate/instruments/stick (~/.local/share when XDG_DATA_HOME
is unset), each in its own folder; Stick looks in the same place. A kit
replaces an old one only once it is complete. The downloads are kept in
`downloads/` there unless --remove-downloads is given.

Requires ffmpeg. Run with `python3 -I tools/stick/install_kits.py [--kit drs]`.
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
import shutil
import subprocess
import sys
import urllib.request
import xml.etree.ElementTree as ElementTree
import zipfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

# ---- The installed format -----------------------------------------------------------------------

FORMAT_LINE = "oscillate stick library 1"
MANIFEST = "stick.manifest"
PACK = "stick.pack"
ATTRIBUTION_FILE = "ATTRIBUTION.txt"
RATE = 48_000
ALIGN_BYTES = 4_096
BYTES_PER_SAMPLE = 2
INT16_MAX = 32_767
INT16_MIN = -32_768
# Each hit is normalised to this share of full scale before dithering.
PEAK_SHARE = 0.98
LICENCE = "Creative Commons Attribution 4.0 International"
LICENCE_URL = "https://creativecommons.org/licenses/by/4.0/"

# ---- Measuring and trimming ---------------------------------------------------------------------

MILLISECOND = RATE // 1_000
DECIBELS = 20.0
POWER_DECIBELS = 10.0
TINY_POWER = 1e-30
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
MAX_HITS_PER_STROKE = 48
# Bounds on the untrusted kit XML.
MAX_XML_BYTES = 4 << 20
MAX_WAV_BYTES = 512 << 20

DEFAULT_JOBS = 4
# What Stick keeps in memory of each hit (stick_library.hpp), for the report.
RESIDENT_FRAMES = 4_096
DOWNLOAD_CHUNK = 1 << 20
# A stalled download gives up after this long without data.
DOWNLOAD_TIMEOUT_SECONDS = 60
DOWNLOADS = "downloads"

# ---- The kits -----------------------------------------------------------------------------------


@dataclass(frozen=True)
class Mic:
    group: str
    piece: str | None
    side: str


def close(group: str, piece: str) -> Mic:
    return Mic(group, piece, "mono")


def left(group: str) -> Mic:
    return Mic(group, None, "left")


def right(group: str) -> Mic:
    return Mic(group, None, "right")


@dataclass(frozen=True)
class Kit:
    flag: str
    folder: str
    name: str
    url: str
    md5: str
    size: int
    drumkit: str  # The drumkit XML inside the zip.
    attribution: str
    # The kit's channels by DrumGizmo name; None drops one.
    mics: dict[str, Mic | None]
    inverted: frozenset[str]
    # DrumGizmo instrument -> (piece, articulation, striker).
    instruments: dict[str, tuple[str, str, str]]


STICK = "stick"
BRUSH = "brush"

KITS: dict[str, Kit] = {
    "drs": Kit(
        flag="drs",
        folder="drs-studio",
        name="DRS Studio (DRSKit 2.1)",
        url="https://drumgizmo.org/kits/DRSKit/DRSKit2_1.zip",
        md5="8c4d4b61ad9d354b3b845edd5da9c133",
        size=2_803_397_710,
        drumkit="DRSKit/DRSKit_full.xml",
        attribution=(
            "DRSKit 2.1 by Jes Eiler (DRSDrums) and the DrumGizmo team, "
            f"drumgizmo.org, {LICENCE} ({LICENCE_URL})"
        ),
        mics={
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
        },
        inverted=frozenset(),
        instruments={
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
        },
    ),
    "muldjord": Kit(
        flag="muldjord",
        folder="muldjord",
        name="Muldjord (MuldjordKit 3)",
        url="https://drumgizmo.org/kits/MuldjordKit/MuldjordKit3.zip",
        md5="8a66a3e90bbf15687b2d34fd355024f2",
        size=2_536_576_789,
        drumkit="MuldjordKit3/MuldjordKit3.xml",
        attribution=(
            "MuldjordKit 3 by Lars Muldjord. Drum samples provided by DrumGizmo.org. "
            f"{LICENCE} ({LICENCE_URL})"
        ),
        mics={
            "AmbL": left("room"),
            "AmbR": right("room"),
            "Hihat": close("hihat", "hihat"),
            "KdrumL": close("kick_in", "kick"),
            "KdrumR": close("kick_in", "kick"),
            "OHL": left("overheads"),
            "OHR": right("overheads"),
            "RideL": close("ride", "ride2"),
            "RideR": close("ride", "ride"),
            "Snare_bottom": close("snare_bottom", "snare"),
            "Snare_top": close("snare_top", "snare"),
            "Tom1": close("toms", "tom1"),
            "Tom2": close("toms", "tom2"),
            "Tom3": close("toms", "tom3"),
            "Tom4": close("toms", "tom4"),
            # The kick drums' triggers, not a microphone.
            "Trigger": None,
        },
        # The kit's page: the snare's bottom mic is out of phase with its top.
        inverted=frozenset({"Snare_bottom"}),
        instruments={
            "KdrumR": ("kick", "hit", STICK),
            "KdrumL": ("kick", "alt", STICK),
            "Snare": ("snare", "hit", STICK),
            "SnareRest": ("snare", "sidestick", STICK),
            "HihatClosed": ("hihat", "closed", STICK),
            "HihatOpen": ("hihat", "open", STICK),
            "Tom1": ("tom1", "hit", STICK),
            "Tom2": ("tom2", "hit", STICK),
            "Tom3": ("tom3", "hit", STICK),
            "Tom4": ("tom4", "hit", STICK),
            "CrashL": ("crash1", "hit", STICK),
            "CrashR": ("crash2", "hit", STICK),
            "RideR": ("ride", "hit", STICK),
            "RideRBell": ("ride", "bell", STICK),
            "RideL": ("ride2", "hit", STICK),
            "RideLBell": ("ride2", "bell", STICK),
            "China": ("china", "hit", STICK),
        },
    ),
    "garage": Kit(
        flag="garage",
        folder="garage",
        name="Garage (ShittyKit 1.2)",
        url="https://drumgizmo.org/kits/ShittyKit/ShittyKit1_2.zip",
        md5="cbab008a3a4413c6e85b1439d36fe63f",
        size=369_655_148,
        drumkit="ShittyKit/ShittyKit.xml",
        attribution=(
            f"ShittyKit 1.2 by Sardonicus for DrumGizmo, drumgizmo.org, {LICENCE} ({LICENCE_URL})"
        ),
        # Its pairs are heard from the audience: swapped to the drummer's seat.
        mics={
            "kick": close("kick_in", "kick"),
            "snare": close("snare_top", "snare"),
            "Tom": close("toms", "tom1"),
            "Floor": close("toms", "tom2"),
            "OH-L": right("overheads"),
            "OH-R": left("overheads"),
            "Room-L": right("room"),
            "Room-R": left("room"),
        },
        inverted=frozenset(),
        instruments={
            "kick": ("kick", "hit", STICK),
            "snare-Dyn-LH": ("snare", "hit", STICK),
            "snare-Dyn-RH": ("snare", "hit", STICK),
            "snare-Hrd-LH": ("snare", "hit", STICK),
            "snare-RmClk": ("snare", "sidestick", STICK),
            "Tom-LH": ("tom1", "hit", STICK),
            "Tom-RH": ("tom1", "hit", STICK),
            "Floor-LH": ("tom2", "hit", STICK),
            "Floor-RH": ("tom2", "hit", STICK),
            "HH-closed": ("hihat", "closed", STICK),
            "HH-open": ("hihat", "open", STICK),
            "HH-pdl": ("hihat", "pedal", STICK),
            "ride": ("ride", "hit", STICK),
            "ride-bell": ("ride", "bell", STICK),
            "Crash1": ("crash1", "hit", STICK),
            "Crash2": ("crash2", "hit", STICK),
        },
    ),
}


# ---- Locations and downloads --------------------------------------------------------------------


def data_directory() -> Path:
    override = os.environ.get("OSCILLATE_STICK_LIBRARY")
    if override:
        return Path(override)
    base = os.environ.get("XDG_DATA_HOME") or str(Path.home() / ".local" / "share")
    return Path(base) / "oscillate" / "instruments" / "stick"


def md5_of(path: Path) -> str:
    digest = hashlib.md5(usedforsecurity=False)
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(DOWNLOAD_CHUNK), b""):
            digest.update(chunk)
    return digest.hexdigest()


def download(kit: Kit, folder: Path) -> Path:
    """The kit's zip, downloaded (or resumed) and checked against its pinned MD5."""
    folder.mkdir(parents=True, exist_ok=True)
    target = folder / PurePosixPath(kit.url).name
    have = target.stat().st_size if target.exists() else 0
    if have < kit.size:
        request = urllib.request.Request(kit.url, headers={"Range": f"bytes={have}-"})
        print(f"downloading {kit.url} ({kit.size / 2**20:.0f} MiB) to {target}")
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
                percent = have * 100 // kit.size
                if percent >= reported + 10:
                    reported = percent - percent % 10
                    print(f"  {reported} %")
    if target.stat().st_size != kit.size or md5_of(target) != kit.md5:
        target.unlink()
        raise SystemExit(
            f"{target.name} does not match its published MD5: downloaded again next time"
        )
    return target


# ---- The kit's XML ------------------------------------------------------------------------------


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


@dataclass
class Hit:
    kit: str
    archive: str
    instrument: str
    sample: str
    piece: str
    articulation: str
    striker: str
    # Per channel in the kit's order: (zip member, channel in that file, from 0).
    sources: list[tuple[str, int]]
    work: str = ""
    frames: int = 0
    scale: float = 0.0
    power_db: float = 0.0
    dropped: str = ""
    offset: int = 0


def kit_hits(kit: Kit, archive_path: Path) -> tuple[list[str], list[Hit]]:
    """The kit's channels in order and every hit Stick plays, from its XML."""
    with zipfile.ZipFile(archive_path) as archive:
        drumkit = read_xml(archive, kit.drumkit)
        base = PurePosixPath(kit.drumkit).parent
        channels = [c.get("name", "") for c in drumkit.iter("channel")]
        if set(channels) != set(kit.mics):
            raise SystemExit(f"{kit.name}: unexpected channels {channels}")
        kept = [c for c in channels if kit.mics[c] is not None]
        hits: list[Hit] = []
        seen: set[str] = set()
        for instrument in drumkit.iter("instrument"):
            name = instrument.get("name", "")
            if name not in kit.instruments or not instrument.get("file"):
                continue
            seen.add(name)
            piece, articulation, striker = kit.instruments[name]
            file = member(archive, base, instrument.get("file", ""))
            # An instrument's own map, when it has one, names the kit's channel
            # each of its file channels plays (all of the Stick kits map 1:1).
            mapped = {m.get("in", ""): m.get("out", "") for m in instrument.iter("channelmap")}
            folder = PurePosixPath(file).parent
            for sample in read_xml(archive, file).iter("sample"):
                files: dict[str, tuple[str, int]] = {}
                for audio in sample.iter("audiofile"):
                    channel = mapped.get(audio.get("channel", ""), audio.get("channel", ""))
                    index = int(audio.get("filechannel", "1")) - 1
                    if index < 0:
                        raise SystemExit(f"{name}: a file channel below 1")
                    files[channel] = (member(archive, folder, audio.get("file", "")), index)
                if not all(c in files for c in kept):
                    raise SystemExit(f"{name} {sample.get('name')}: a mic is missing")
                hits.append(
                    Hit(
                        kit=kit.flag,
                        archive=str(archive_path),
                        instrument=name,
                        sample=sample.get("name", ""),
                        piece=piece,
                        articulation=articulation,
                        striker=striker,
                        sources=[files[c] for c in kept],
                    )
                )
        missing = set(kit.instruments) - seen
        if missing:
            raise SystemExit(f"{kit.name}: instruments not found: {sorted(missing)}")
    return kept, hits


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


def decode(data: bytes, filters: str) -> list[array.array]:
    """A WAV's channels as doubles at 48 kHz."""
    probe = subprocess.run(
        [
            "ffprobe",
            "-v",
            "error",
            "-show_entries",
            "stream=channels",
            "-of",
            "csv=p=0",
            "-i",
            "pipe:0",
        ],
        input=data,
        capture_output=True,
        check=True,
    )
    channels = int(probe.stdout.decode().split()[0])
    raw = subprocess.run(
        [
            "ffmpeg",
            "-nostdin",
            "-v",
            "error",
            "-i",
            "pipe:0",
            "-af",
            filters,
            "-f",
            "f64le",
            "-c:a",
            "pcm_f64le",
            "-",
        ],
        input=data,
        capture_output=True,
        check=True,
    ).stdout
    values = array.array("d")
    values.frombytes(raw)
    if sys.byteorder != "little":
        values.byteswap()
    return [values[c::channels] for c in range(channels)]


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


def measure(job: tuple[Hit, dict[str, Mic | None], list[str], frozenset[str], str, str]) -> Hit:
    hit, mics, order, inverted, filters, work = job
    with zipfile.ZipFile(hit.archive) as archive:
        decoded: dict[str, list[array.array]] = {}
        for name, _ in hit.sources:
            if name not in decoded:
                info = archive.getinfo(name)
                if info.file_size > MAX_WAV_BYTES:
                    raise SystemExit(f"{name} is too large")
                decoded[name] = decode(archive.read(info), filters)
    channels = []
    for (name, index), dg_name in zip(hit.sources, order, strict=True):
        channel = decoded[name][index]
        if dg_name in inverted:
            channel = array.array("d", (-x for x in channel))
        channels.append(channel)
    frames = min(len(c) for c in channels)
    channels = [c[:frames] for c in channels]
    own = [i for i, n in enumerate(order) if (m := mics[n]) is not None and m.piece == hit.piece]
    if not own:
        own = [
            i for i, n in enumerate(order) if (m := mics[n]) is not None and m.group == "overheads"
        ]
    main = [channels[i] for i in own]
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
    every = block_peaks(channels, TAIL_BLOCK_FRAMES)
    loudest = max(every)
    noise = max(block_peaks([c[-NOISE_FRAMES:] for c in channels], NOISE_FRAMES))
    floor = max(loudest * 10.0 ** (TAIL_DB / DECIBELS), noise * NOISE_MARGIN)
    last = max((i for i, value in enumerate(every) if value >= floor), default=0)
    end = min(frames, (last + 1) * TAIL_BLOCK_FRAMES + FADE_OUT_FRAMES, start + MAX_HIT_FRAMES)
    length = end - start
    if length <= PRE_ROLL_FRAMES + FADE_OUT_FRAMES:
        hit.dropped = "too short"
        return hit
    square = sum(x * x for c in main for x in c[onset : onset + POWER_FRAMES])
    hit.power_db = POWER_DECIBELS * math.log10(square / (len(main) * POWER_FRAMES) + TINY_POWER)
    peak = max(max(max(c[start:end]), -min(c[start:end])) for c in channels)
    gain = PEAK_SHARE * INT16_MAX / peak
    hit.scale = 1.0 / gain
    generator = random.Random(f"{hit.kit}/{hit.instrument}/{hit.sample}")
    uniform = generator.random
    pcm = array.array("h", bytes(BYTES_PER_SAMPLE * length * len(channels)))
    fade_out_from = length - FADE_OUT_FRAMES
    for index, channel in enumerate(channels):
        part = channel[start:end]
        pcm[index :: len(channels)] = array.array(
            "h",
            (
                min(
                    INT16_MAX,
                    max(
                        INT16_MIN,
                        round(
                            value
                            * gain
                            * min(1.0, i / PRE_ROLL_FRAMES, (length - i) / FADE_OUT_FRAMES)
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
    if sys.byteorder != "little":
        pcm.byteswap()
    hit.frames = length
    name = hashlib.sha1(f"{hit.instrument}/{hit.sample}".encode(), usedforsecurity=False)
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


def write_kit(kit: Kit, order: list[str], hits: list[Hit], target: Path) -> int:
    channels = len(order)
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
    for index, dg_name in enumerate(order):
        mic = kit.mics[dg_name]
        assert mic is not None
        lines.append(f"mic {index} {mic.group} {mic.piece or '-'} {mic.side}")
    for hit in hits:
        lines.append(
            f"hit {hit.piece} {hit.articulation} {hit.striker} {hit.offset} {hit.frames} "
            f"{hit.scale:.9e} {hit.power_db:.2f}"
        )
    (target / MANIFEST).write_text("\n".join(lines) + "\n")
    changes = [
        "resampled to 48 kHz",
        "each hit trimmed to 1 ms before its stroke and to its tail, with fades",
        "normalised per hit and dithered to 16-bit",
        "hits with no stroke or two strokes left out",
    ]
    if kit.inverted:
        changes.append(f"{', '.join(sorted(kit.inverted))} inverted, as the kit's page advises")
    dropped = [name for name, mic in kit.mics.items() if mic is None]
    if dropped:
        changes.append(f"{', '.join(dropped)} left out (not a microphone)")
    if kit.flag == "garage":
        changes.append("the overhead and room pairs swapped, to the drummer's seat")
    (target / ATTRIBUTION_FILE).write_text(
        f"{kit.attribution}\n\n"
        f"Source: {kit.url} (MD5 {kit.md5}).\n"
        f"Licence: {LICENCE}, {LICENCE_URL}\n\n"
        "Converted for Oscillate's Stick on the machine it was installed on: "
        + "; ".join(changes)
        + ". The recordings are otherwise unchanged.\n"
    )
    return offset_bytes


def install(kit: Kit, base: Path, archive: Path, jobs: int) -> None:
    order, hits = kit_hits(kit, archive)
    target = base / kit.folder
    partial = target.with_name(target.name + ".partial")
    if partial.exists():
        shutil.rmtree(partial)
    work = partial / "work"
    work.mkdir(parents=True)
    filters = resampler()
    print(f"{kit.name}: {len(order)} mics, measuring {len(hits)} hits")
    with concurrent.futures.ProcessPoolExecutor(max(1, jobs)) as pool:
        measured = list(
            pool.map(
                measure,
                [(h, kit.mics, order, kit.inverted, filters, str(work)) for h in hits],
                chunksize=1,
            )
        )
    dropped = [h for h in measured if h.dropped]
    for hit in dropped:
        print(f"  left out {hit.instrument} {hit.sample}: {hit.dropped}")
    chosen = choose([h for h in measured if not h.dropped])
    size = write_kit(kit, order, chosen, partial)
    shutil.rmtree(work)
    previous = target.with_name(target.name + ".previous")
    if previous.exists():
        shutil.rmtree(previous)
    if target.exists():
        target.rename(previous)
    partial.rename(target)
    if previous.exists():
        shutil.rmtree(previous)
    resident = len(chosen) * len(order) * RESIDENT_FRAMES * BYTES_PER_SAMPLE
    print(
        f"installed {kit.name} in {target}: {len(chosen)} hits, pack {size / 2**20:.0f} MiB, "
        f"{resident / 2**20:.0f} MiB resident, {len(dropped)} left out"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument(
        "--kit",
        choices=[*KITS, "all"],
        default="all",
        help="the kit to install (default: all three)",
    )
    parser.add_argument(
        "--destination",
        type=Path,
        default=data_directory(),
        help="the folder Stick reads its kits from (default: %(default)s)",
    )
    parser.add_argument(
        "--remove-downloads", action="store_true", help="delete each zip once it is installed"
    )
    parser.add_argument("--jobs", type=int, default=DEFAULT_JOBS, help="hits converted in parallel")
    arguments = parser.parse_args()
    for tool in ("ffmpeg", "ffprobe"):
        if shutil.which(tool) is None:
            raise SystemExit(f"{tool} is required")
    base = arguments.destination.resolve()
    base.mkdir(parents=True, exist_ok=True)
    chosen = list(KITS.values()) if arguments.kit == "all" else [KITS[arguments.kit]]
    for kit in chosen:
        archive = download(kit, base / DOWNLOADS)
        install(kit, base, archive, arguments.jobs)
        if arguments.remove_downloads:
            archive.unlink()
    print(f"Stick's kits are in {base}. Credit them where the music is credited.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
