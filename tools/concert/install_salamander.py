#!/usr/bin/env python3
"""Download the Salamander Grand Piano and install it as Concert's library.

Concert (ADR-0071) plays one sampled piano: the Salamander Grand Piano V3
(Yamaha C5) by Alexander Holm, CC-BY 3.0, in the sfzinstruments 48 kHz /
24-bit FLAC edition. This script:

1. fetches the pinned commit with git (git checks every object's hash, so
   the pinned commit id is the integrity check) and verifies the file set;
2. reads the SFZ mapping (Data/*.txt) for the note, release, resonance and
   pedal samples;
3. converts each FLAC with ffmpeg: the silence before the attack trimmed to
   2 ms with a 2 ms fade-in, peak-normalised and TPDF-dithered to 16-bit;
4. writes `concert.pack` (interleaved little-endian int16 stereo, each sample
   aligned to 4 KiB) and `concert.manifest` (one line per sample), with the
   licence, the README and the attribution, into the library directory.

The library directory is $OSCILLATE_CONCERT_LIBRARY when set, otherwise
$XDG_DATA_HOME/oscillate/instruments/concert/salamander-grand-v3
(~/.local/share when XDG_DATA_HOME is unset); Concert looks in the same
place (concert::library::default_directory). The new library replaces an
old one only once it is complete.

Requires git and ffmpeg. Run with `python3 -I tools/concert/install_salamander.py`.
"""

from __future__ import annotations

import argparse
import array
import concurrent.futures
import math
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPOSITORY = "https://github.com/sfzinstruments/SalamanderGrandPiano.git"
PINNED_COMMIT = "3382bf9496bba2486f5ab0de55a264d1dfc38404"
PINNED_TREE = "77a68329fec9a01d1cdb965f02709bb188d1dbae"
EXPECTED_SAMPLES = 641

NAME = "Salamander Grand Piano V3"
INSTRUMENT = "Yamaha C5"
ATTRIBUTION = "Salamander Grand Piano by Alexander Holm, CC-BY 3.0"
LICENCE = "CC-BY-3.0"
SOURCE_URL = "https://github.com/sfzinstruments/SalamanderGrandPiano"
FORMAT_LINE = "oscillate concert library 1"
MANIFEST = "concert.manifest"
PACK = "concert.pack"

RATE = 48_000
CHANNELS = 2
BYTES_PER_FRAME = CHANNELS * 2
ALIGN_FRAMES = 1_024  # 4 KiB.
# The attack starts where the signal first exceeds this fraction of the
# sample's peak (-40 dB); 2 ms before it are kept and faded in.
ONSET_FRACTION = 0.01
PRE_ROLL_FRAMES = 96
ONSET_SEARCH_FRAMES = RATE
# Normalised peak, a little under full scale so dither never clips.
NORMALISED_PEAK = 0.98
# Keys the dampers cover: the regions the SFZ maps without its long
# ampeg_release ("Notes without dampers" start at F#6, key 89).
LAST_DAMPED_KEY = 88


@dataclass
class Sample:
    kind: str
    file: str
    first_key: int
    last_key: int
    root_key: int
    low_velocity: int
    high_velocity: int
    gain_db: float = 0.0
    velocity_track: float = 0.0
    decay_db_per_second: float = 0.0
    # Filled by conversion.
    start: int = 0
    frames: int = 0
    peak: float = 0.0
    offset: int = 0
    extra: dict = field(default_factory=dict)


def data_directory() -> Path:
    override = os.environ.get("OSCILLATE_CONCERT_LIBRARY")
    if override:
        return Path(override)
    base = os.environ.get("XDG_DATA_HOME") or str(Path.home() / ".local" / "share")
    return Path(base) / "oscillate" / "instruments" / "concert" / "salamander-grand-v3"


def run(command: list[str], cwd: Path | None = None) -> str:
    result = subprocess.run(command, cwd=cwd, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise SystemExit(f"{' '.join(command)} failed:\n{result.stderr.strip()}")
    return result.stdout.strip()


def fetch(source: Path) -> None:
    if (source / ".git").is_dir():
        print(f"using the checkout in {source}")
    else:
        source.mkdir(parents=True, exist_ok=True)
        print(f"fetching {REPOSITORY} at {PINNED_COMMIT[:12]} (about 750 MB)")
        run(["git", "init", "-q"], source)
        run(["git", "remote", "add", "origin", REPOSITORY], source)
        run(["git", "fetch", "-q", "--depth", "1", "origin", PINNED_COMMIT], source)
        run(["git", "-c", "advice.detachedHead=false", "checkout", "-q", "FETCH_HEAD"], source)
    if run(["git", "rev-parse", "HEAD"], source) != PINNED_COMMIT:
        raise SystemExit("the checkout is not the pinned commit")
    if run(["git", "rev-parse", "HEAD^{tree}"], source) != PINNED_TREE:
        raise SystemExit("the checkout's tree is not the pinned tree")
    if run(["git", "status", "--porcelain", "--untracked-files=no"], source):
        raise SystemExit("the checkout has modified files")
    samples = list((source / "Samples").glob("*.flac"))
    if len(samples) != EXPECTED_SAMPLES:
        raise SystemExit(f"expected {EXPECTED_SAMPLES} samples, found {len(samples)}")
    for name in (
        "LICENSE",
        "README.md",
        "Data/notes.txt",
        "Data/region.txt",
        "Data/hammer.txt",
        "Data/str_res.txt",
        "Data/pedal.txt",
    ):
        if not (source / name).is_file():
            raise SystemExit(f"missing {name}")


# ---- A minimal SFZ reader for the mapping files --------------------------------------


def sfz_regions(path: Path, substitutions: dict[str, str]) -> list[dict[str, str]]:
    """Regions of one SFZ fragment with their group's (and the file's) opcodes."""
    base: dict[str, str] = {}
    group: dict[str, str] | None = None
    regions: list[dict[str, str]] = []
    for raw in path.read_text().splitlines():
        line = raw.split("//", 1)[0].strip()
        if not line or line.startswith("#"):
            continue
        for key, value in substitutions.items():
            line = line.replace(key, value)
        current: dict[str, str] | None = None
        for token in line.split():
            if token == "<group>":
                group = dict(base)
                current = group
            elif token == "<region>":
                current = dict(group if group is not None else base)
                regions.append(current)
            elif "=" in token:
                key, value = token.split("=", 1)
                target = current if current is not None else (group if group is not None else base)
                target[key] = value
    return regions


def defines(path: Path) -> dict[str, str]:
    values = {}
    for line in path.read_text().splitlines():
        match = re.match(r"#define\s+(\$\w+)\s+(\S+)", line.strip())
        if match:
            values[match.group(1)] = match.group(2)
    return values


def mapping(source: Path) -> list[Sample]:
    data = source / "Data"
    samples: list[Sample] = []
    # Notes: notes.txt names one velocity file and range per layer, and
    # region.txt maps the keys for each.
    layers = re.findall(
        r'#include "Data/(vel_\d+\.txt)"\s+lovel=(\d+)\s+hivel=(\d+)',
        (data / "notes.txt").read_text(),
    )
    if len(layers) != 16:
        raise SystemExit("expected 16 velocity layers")
    for velocity_file, low, high in layers:
        substitutions = defines(data / velocity_file)
        substitutions["$EXT"] = "flac"
        for region in sfz_regions(data / "region.txt", substitutions):
            samples.append(
                Sample(
                    "note",
                    region["sample"],
                    int(region["lokey"]),
                    int(region["hikey"]),
                    int(region["pitch_keycenter"]),
                    int(low),
                    int(high),
                )
            )
    # Key-release (hammer and key mechanism) noises, one per key.
    for region in sfz_regions(data / "hammer.txt", {"$EXT": "flac"}):
        key = int(region["key"])
        samples.append(
            Sample(
                "release",
                region["sample"],
                key,
                key,
                key,
                1,
                127,
                float(region.get("volume", 0)),
                float(region.get("amp_veltrack", 0)) / 100.0,
                float(region.get("rt_decay", 0)),
            )
        )
    # String resonance releases, by register and loud/soft layer.
    for region in sfz_regions(data / "str_res.txt", {"$EXT": "flac"}):
        samples.append(
            Sample(
                "resonance",
                region["sample"],
                int(region["lokey"]),
                int(region["hikey"]),
                int(region["pitch_keycenter"]),
                int(region.get("lovel", 1)),
                int(region.get("hivel", 127)),
                float(region.get("volume", 0)),
                float(region.get("amp_veltrack", 0)) / 100.0,
                float(region.get("rt_decay", 0)),
            )
        )
    # The sustain pedal's mechanism, down and up, two of each.
    for region in sfz_regions(data / "pedal.txt", {"$EXT": "flac"}):
        label = region.get("group_label", "")
        kind = {"pedalD": "pedal_down", "pedalU": "pedal_up"}.get(label)
        if kind is None:
            raise SystemExit(f"unknown pedal group {label!r}")
        samples.append(
            Sample(kind, region["sample"], 0, 0, 0, 1, 127, float(region.get("volume", 0)))
        )
    names = {sample.file for sample in samples}
    on_disk = {path.name for path in (source / "Samples").glob("*.flac")}
    if names != on_disk:
        raise SystemExit(f"the mapping and the samples disagree: {sorted(names ^ on_disk)[:8]}")
    return samples


# ---- Conversion -------------------------------------------------------------------------


def analyse(path: Path) -> tuple[float, int, int]:
    """Peak (linear), the trimmed start frame and the frame count."""
    raw = subprocess.run(
        [
            "ffmpeg",
            "-v",
            "error",
            "-i",
            str(path),
            "-f",
            "f32le",
            "-ac",
            "2",
            "-ar",
            str(RATE),
            "-",
        ],
        capture_output=True,
        check=True,
    ).stdout
    values = array.array("f")
    values.frombytes(raw)
    frames = len(values) // CHANNELS
    peak = max(max(values), -min(values)) if values else 0.0
    if peak <= 0.0:
        raise SystemExit(f"{path.name} is silent")
    threshold = peak * ONSET_FRACTION
    onset = 0
    for index in range(min(len(values), ONSET_SEARCH_FRAMES * CHANNELS)):
        if abs(values[index]) > threshold:
            onset = index // CHANNELS
            break
    return peak, max(0, onset - PRE_ROLL_FRAMES), frames


def convert(path: Path, start: int, gain: float) -> bytes:
    chain = (
        f"aformat=sample_fmts=dbl,atrim=start_sample={start},asetpts=PTS-STARTPTS,"
        f"afade=t=in:ns={PRE_ROLL_FRAMES},volume=volume={gain:.9f}:precision=double,"
        "aresample=osf=s16:dither_method=triangular"
    )
    return subprocess.run(
        [
            "ffmpeg",
            "-v",
            "error",
            "-i",
            str(path),
            "-af",
            chain,
            "-ac",
            "2",
            "-ar",
            str(RATE),
            "-f",
            "s16le",
            "-c:a",
            "pcm_s16le",
            "-",
        ],
        capture_output=True,
        check=True,
    ).stdout


def prepare(source: Path, sample: Sample) -> Sample:
    path = source / "Samples" / sample.file
    sample.peak, sample.start, total = analyse(path)
    sample.frames = total - sample.start
    return sample


def write(source: Path, sample: Sample, pack: int) -> None:
    gain = NORMALISED_PEAK / sample.peak
    pcm = convert(source / "Samples" / sample.file, sample.start, gain)
    if len(pcm) != sample.frames * BYTES_PER_FRAME:
        raise SystemExit(
            f"{sample.file}: converted {len(pcm)} bytes, expected {sample.frames * BYTES_PER_FRAME}"
        )
    if sys.byteorder != "little":
        swapped = array.array("h")
        swapped.frombytes(pcm)
        swapped.byteswap()
        pcm = swapped.tobytes()
    os.pwrite(pack, pcm, sample.offset * BYTES_PER_FRAME)
    sample.extra["scale"] = 1.0 / (gain * 32_768.0)


def build(source: Path, target: Path, jobs: int) -> None:
    samples = mapping(source)
    print(f"analysing {len(samples)} samples")
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        samples = list(pool.map(lambda s: prepare(source, s), samples))
    offset = 0
    for sample in samples:
        sample.offset = offset
        offset += (sample.frames + ALIGN_FRAMES - 1) // ALIGN_FRAMES * ALIGN_FRAMES
    pack_bytes = offset * BYTES_PER_FRAME
    print(f"converting to a {pack_bytes / 2**20:.0f} MiB pack")
    target.mkdir(parents=True)
    descriptor = os.open(target / PACK, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
    try:
        os.ftruncate(descriptor, pack_bytes)
        with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
            written = pool.map(lambda s: write(source, s, descriptor), samples)
            for done, _ in enumerate(written, start=1):
                if done % 64 == 0 or done == len(samples):
                    print(f"  {done}/{len(samples)}")
        os.fsync(descriptor)
    finally:
        os.close(descriptor)
    lines = [
        FORMAT_LINE,
        f"name {NAME}",
        f"instrument {INSTRUMENT}",
        f"attribution {ATTRIBUTION}",
        f"licence {LICENCE}",
        f"source {SOURCE_URL}",
        f"commit {PINNED_COMMIT}",
        f"rate {RATE}",
        f"pack {PACK} {pack_bytes}",
        f"last_damped_key {LAST_DAMPED_KEY}",
        "# sample kind first_key last_key root_key low_velocity high_velocity offset frames"
        " scale peak_db gain_db velocity_track decay_db_per_second file",
    ]
    for s in samples:
        lines.append(
            " ".join(
                str(v)
                for v in (
                    "sample",
                    s.kind,
                    s.first_key,
                    s.last_key,
                    s.root_key,
                    s.low_velocity,
                    s.high_velocity,
                    s.offset,
                    s.frames,
                    f"{s.extra['scale']:.9e}",
                    f"{20.0 * math.log10(s.peak):.3f}",
                    f"{s.gain_db:g}",
                    f"{s.velocity_track:g}",
                    f"{s.decay_db_per_second:g}",
                    s.file,
                )
            )
        )
    (target / MANIFEST).write_text("\n".join(lines) + "\n")
    shutil.copyfile(source / "LICENSE", target / "LICENSE")
    shutil.copyfile(source / "README.md", target / "README.md")
    (target / "ATTRIBUTION.txt").write_text(
        f"{ATTRIBUTION}\n\n{NAME} ({INSTRUMENT}), recorded and released by Alexander Holm\n"
        f"under the Creative Commons Attribution 3.0 Unported licence (see LICENSE).\n"
        f"SFZ mapping by kinwie: {SOURCE_URL} (commit {PINNED_COMMIT}).\n"
        "Converted for oscillate's Concert: attack pre-roll trimmed to 2 ms, peak-normalised\n"
        "and dithered to 16-bit; the sound is otherwise unchanged.\n"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument(
        "--library",
        type=Path,
        default=data_directory(),
        help="where Concert reads its library (default: %(default)s)",
    )
    parser.add_argument(
        "--source",
        type=Path,
        help="an existing checkout of the pinned commit, or where to fetch it",
    )
    parser.add_argument(
        "--keep-source", action="store_true", help="keep the downloaded FLAC checkout"
    )
    parser.add_argument("--jobs", type=int, default=4, help="parallel ffmpeg conversions")
    arguments = parser.parse_args()
    for tool in ("git", "ffmpeg"):
        if shutil.which(tool) is None:
            raise SystemExit(f"{tool} is required")
    library = arguments.library.resolve()
    library.parent.mkdir(parents=True, exist_ok=True)
    source = (arguments.source or library.parent / ".salamander-source").resolve()
    fetch(source)
    partial = library.with_name(library.name + ".partial")
    if partial.exists():
        shutil.rmtree(partial)
    build(source, partial, max(1, arguments.jobs))
    previous = library.with_name(library.name + ".previous")
    if previous.exists():
        shutil.rmtree(previous)
    # Only an earlier install (or an empty directory) is replaced: a library
    # path that holds anything else is left alone.
    if library.exists() and any(library.iterdir()) and not (library / MANIFEST).is_file():
        shutil.rmtree(partial)
        raise SystemExit(f"{library} is not empty and holds no {MANIFEST}; refusing to replace it")
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
