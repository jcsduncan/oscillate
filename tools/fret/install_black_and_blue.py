#!/usr/bin/env python3
"""Download the Black and Blue Basses and install them as Fret's library.

Fret (ADR-0088) plays one sampled bass: the "darkblack" fingered bass of
Karoryfer Samples' Black and Blue Basses, CC0 1.0, in the sfzinstruments
44.1 kHz / 24-bit mono WAV edition. This script:

1. fetches the pinned commit with git (git checks every object's hash, so
   the pinned commit and tree ids are the integrity check), or takes an
   already-extracted copy with --source, and verifies the file set;
2. reads the darkblack SFZ maps (Programs/maps/darkblack_*_map.sfz) for each
   sample's articulation, key, velocity layer and round robin;
3. converts each WAV with ffmpeg: resampled to 48 kHz with the soxr
   resampler, the silence before the attack trimmed to 2 ms with a 2 ms
   fade-in, peak-normalised and TPDF-dithered to 16-bit mono;
4. writes `fret.pack` (little-endian int16 mono, each sample aligned to
   4 KiB) and `fret.manifest` (one line per sample), with the licence and the
   attribution, into the library directory.

The SFZ maps every sample one octave above where it sounds (key 35 plays the
low B string, B0 = MIDI 23); the manifest stores the sounding key.

The library directory is $OSCILLATE_FRET_LIBRARY when set, otherwise
$XDG_DATA_HOME/oscillate/instruments/fret/black-and-blue-darkblack
(~/.local/share when XDG_DATA_HOME is unset); Fret looks in the same place.
The new library replaces an old one only once it is complete.

Requires git (unless --source names an extracted copy) and ffmpeg built with
libsoxr. Run with `python3 -I tools/fret/install_black_and_blue.py`.
"""

from __future__ import annotations

import argparse
import array
import concurrent.futures
import math
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path, PureWindowsPath

REPOSITORY = "https://github.com/sfzinstruments/karoryfer.black-and-blue-basses.git"
PINNED_COMMIT = "6e7d674cdb41be7a54dbccb15472401ad01099b9"
PINNED_TREE = "a5509d29a3a7070a447458bd140de64e72b23b75"

NAME = "Black and Blue Basses (darkblack, fingered)"
ATTRIBUTION = "Black and Blue Basses by Karoryfer Samples, CC0 1.0"
SOURCE_URL = "https://github.com/sfzinstruments/karoryfer.black-and-blue-basses"
FORMAT_LINE = "oscillate fret library 1"
MANIFEST = "fret.manifest"
PACK = "fret.pack"
LICENCE_FILE = "license"
INSTALLED_LICENCE = "LICENSE"
DEFAULT_SOURCE_NAME = ".black-and-blue-source"

PROGRAMS = "Programs"
MAPS = "Programs/maps"
SAMPLES = "Samples"
# The darkblack sample directories the maps below use; btb_extra (the
# open-string bridge noises) is not part of Fret.
SAMPLE_DIRECTORIES = (
    "darkblack/reg",
    "darkblack/ghost",
    "darkblack/stac",
    "darkblack/btb",
    "darkblack/rel",
)
SAMPLE_SUFFIX = ".wav"

# Articulation -> its map files, one per velocity layer, softest first.
ARTICULATIONS: dict[str, tuple[str, ...]] = {
    "pluck": (
        "darkblack_reg_p_map.sfz",
        "darkblack_reg_mp_map.sfz",
        "darkblack_reg_mf_map.sfz",
        "darkblack_reg_f_map.sfz",
    ),
    "ghost": ("darkblack_ghost_p_map.sfz", "darkblack_ghost_f_map.sfz"),
    "staccato": ("darkblack_stac_map.sfz",),
    "bridge": ("darkblack_btb_map.sfz",),
    "release": ("darkblack_rel_map.sfz",),
}

# Sounding keys: B0 (the low B string) to E4.
FIRST_KEY = 23
LAST_KEY = 64
# The SFZ maps one octave above the sounding pitch.
SFZ_KEY_OFFSET = 12
# SFZ defaults for opcodes a region may leave out.
SFZ_DEFAULT_KEYCENTER = 60
SFZ_FIRST_SEQ_POSITION = 1

SOURCE_RATE = 44_100
RATE = 48_000
CHANNELS = 1
BYTES_PER_SAMPLE = 2
BYTES_PER_FRAME = CHANNELS * BYTES_PER_SAMPLE
INT16_FULL_SCALE = 32_768.0
ALIGN_FRAMES = 2_048  # 4 KiB of int16 mono.
# soxr's precision in bits (ffmpeg's range is 15 to 33; 28 is soxr's "very high").
SOXR_PRECISION = 28
# The attack starts where the signal first exceeds this fraction of the
# sample's peak (-40 dB); 2 ms before it are kept and faded in.
ONSET_FRACTION = 0.01
PRE_ROLL_FRAMES = 96  # 2 ms at 48 kHz.
ONSET_SEARCH_FRAMES = RATE
# Normalised peak, a little under full scale so dither never clips.
NORMALISED_PEAK = 0.98
DECIBELS_PER_DECADE = 20.0
PROGRESS_INTERVAL = 64
DEFAULT_JOBS = 4


@dataclass
class Sample:
    articulation: str
    file: Path
    key: int
    layer: int
    layers: int = 0
    round_robin: int = 0
    # Filled by conversion.
    start: int = 0
    frames: int = 0
    peak: float = 0.0
    offset: int = 0
    scale: float = 0.0


def data_directory() -> Path:
    override = os.environ.get("OSCILLATE_FRET_LIBRARY")
    if override:
        return Path(override)
    base = os.environ.get("XDG_DATA_HOME") or str(Path.home() / ".local" / "share")
    return Path(base) / "oscillate" / "instruments" / "fret" / "black-and-blue-darkblack"


def run(command: list[str], cwd: Path | None = None) -> str:
    result = subprocess.run(command, cwd=cwd, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise SystemExit(f"{' '.join(command)} failed:\n{result.stderr.strip()}")
    return result.stdout.strip()


def fetch(source: Path) -> None:
    """Fetch the pinned commit into `source` (or reuse a checkout there) and verify it."""
    if (source / ".git").is_dir():
        print(f"using the checkout in {source}")
    else:
        source.mkdir(parents=True, exist_ok=True)
        print(f"fetching {REPOSITORY} at {PINNED_COMMIT[:12]} (about 1.1 GB)")
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


def check_required_files(source: Path) -> None:
    required = [LICENCE_FILE] + [
        f"{MAPS}/{name}" for maps in ARTICULATIONS.values() for name in maps
    ]
    for name in required:
        if not (source / name).is_file():
            raise SystemExit(f"missing {name}")


# ---- A minimal SFZ reader for the map files ------------------------------------------


def sfz_regions(path: Path) -> list[dict[str, str]]:
    """The regions of one map file, each with its opcodes (one per line in these maps)."""
    regions: list[dict[str, str]] = []
    current: dict[str, str] | None = None
    for raw in path.read_text(encoding="ascii").splitlines():
        line = raw.split("//", 1)[0].strip()
        if not line:
            continue
        if line == "<region>":
            current = {}
            regions.append(current)
        elif line.startswith("<") or line.startswith("#"):
            raise SystemExit(f"{path.name}: unexpected {line!r}")
        elif "=" in line:
            if current is None:
                raise SystemExit(f"{path.name}: opcode outside a region: {line!r}")
            key, value = line.split("=", 1)
            current[key.strip()] = value.strip()
        else:
            raise SystemExit(f"{path.name}: cannot read {line!r}")
    return regions


def sample_path(source: Path, value: str, map_name: str) -> Path:
    """The WAV a region names, relative to Programs, kept inside Samples."""
    relative = Path(*PureWindowsPath(value).parts)
    path = (source / PROGRAMS / relative).resolve()
    samples = (source / SAMPLES).resolve()
    if samples not in path.parents or path.suffix != SAMPLE_SUFFIX:
        raise SystemExit(f"{map_name}: sample outside {SAMPLES}: {value!r}")
    return path


def integer(region: dict[str, str], opcode: str, map_name: str, default: int | None = None) -> int:
    value = region.get(opcode)
    if value is None:
        if default is None:
            raise SystemExit(f"{map_name}: a region has no {opcode}")
        return default
    try:
        return int(value)
    except ValueError:
        raise SystemExit(f"{map_name}: {opcode}={value!r} is not a whole number") from None


def mapping(source: Path) -> list[Sample]:
    samples: list[Sample] = []
    seen: set[Path] = set()
    for articulation, maps in ARTICULATIONS.items():
        hits: dict[tuple[int, int], list[int]] = {}
        for layer, map_name in enumerate(maps):
            for region in sfz_regions(source / MAPS / map_name):
                low = integer(region, "lokey", map_name)
                high = integer(region, "hikey", map_name)
                centre = integer(region, "pitch_keycenter", map_name, SFZ_DEFAULT_KEYCENTER)
                if not low == high == centre:
                    raise SystemExit(
                        f"{map_name}: region {region.get('sample')} spans keys "
                        f"{low}-{high} about {centre}; Fret expects one key each"
                    )
                key = centre - SFZ_KEY_OFFSET
                if not FIRST_KEY <= key <= LAST_KEY:
                    raise SystemExit(
                        f"{map_name}: key {centre} sounds outside {FIRST_KEY}-{LAST_KEY}"
                    )
                position = integer(region, "seq_position", map_name, SFZ_FIRST_SEQ_POSITION)
                round_robin = position - SFZ_FIRST_SEQ_POSITION
                if "sample" not in region:
                    raise SystemExit(f"{map_name}: a region has no sample")
                path = sample_path(source, region["sample"], map_name)
                if path in seen:
                    raise SystemExit(f"{map_name}: {path.name} is mapped twice")
                seen.add(path)
                hits.setdefault((key, layer), []).append(round_robin)
                samples.append(Sample(articulation, path, key, layer, len(maps), round_robin))
        # Every key must have every layer, each with the same round robins 0..n-1.
        counts = {len(robins) for robins in hits.values()}
        if len(counts) != 1:
            raise SystemExit(f"{articulation}: round robin counts differ between keys: {counts}")
        robins = counts.pop()
        for (key, layer), positions in sorted(hits.items()):
            if sorted(positions) != list(range(robins)):
                raise SystemExit(
                    f"{articulation} key {key} layer {layer}: round robins "
                    f"{sorted(positions)}, expected 0..{robins - 1}"
                )
        expected = {
            (key, layer) for key in range(FIRST_KEY, LAST_KEY + 1) for layer in range(len(maps))
        }
        if set(hits) != expected:
            missing = sorted(expected - set(hits))
            raise SystemExit(f"{articulation}: keys or layers missing: {missing[:8]}")
        print(
            f"  {articulation}: keys {FIRST_KEY}-{LAST_KEY}, {len(maps)} layer(s) x "
            f"{robins} round robin(s)"
        )
    on_disk = {
        path.resolve()
        for directory in SAMPLE_DIRECTORIES
        for path in (source / SAMPLES / directory).glob(f"*{SAMPLE_SUFFIX}")
    }
    if seen != on_disk:
        difference = sorted(path.name for path in seen ^ on_disk)
        raise SystemExit(f"the maps and the samples disagree: {difference[:8]}")
    samples.sort(
        key=lambda s: (list(ARTICULATIONS).index(s.articulation), s.key, s.layer, s.round_robin)
    )
    return samples


# ---- Conversion -------------------------------------------------------------------------


def resample_filter() -> str:
    return f"aformat=sample_fmts=dbl,aresample={RATE}:resampler=soxr:precision={SOXR_PRECISION}"


def analyse(path: Path) -> tuple[float, int, int]:
    """Peak (linear) at 48 kHz, the trimmed start frame and the frame count."""
    raw = subprocess.run(
        [
            "ffmpeg",
            "-nostdin",
            "-v",
            "error",
            "-i",
            str(path),
            "-af",
            resample_filter(),
            "-ac",
            str(CHANNELS),
            "-ar",
            str(RATE),
            "-f",
            "f32le",
            "-c:a",
            "pcm_f32le",
            "-",
        ],
        capture_output=True,
        check=True,
    ).stdout
    values = array.array("f")
    values.frombytes(raw)
    if sys.byteorder != "little":
        values.byteswap()
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
        f"{resample_filter()},atrim=start_sample={start},asetpts=PTS-STARTPTS,"
        f"afade=t=in:ns={PRE_ROLL_FRAMES},volume=volume={gain:.9f}:precision=double,"
        "aresample=osf=s16:dither_method=triangular"
    )
    return subprocess.run(
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
            "s16le",
            "-c:a",
            "pcm_s16le",
            "-",
        ],
        capture_output=True,
        check=True,
    ).stdout


def prepare(sample: Sample) -> Sample:
    sample.peak, sample.start, total = analyse(sample.file)
    sample.frames = total - sample.start
    return sample


def write(sample: Sample, pack: int) -> None:
    gain = NORMALISED_PEAK / sample.peak
    pcm = convert(sample.file, sample.start, gain)
    if len(pcm) != sample.frames * BYTES_PER_FRAME:
        raise SystemExit(
            f"{sample.file.name}: converted {len(pcm)} bytes, expected "
            f"{sample.frames * BYTES_PER_FRAME}"
        )
    if sys.byteorder != "little":
        swapped = array.array("h")
        swapped.frombytes(pcm)
        swapped.byteswap()
        pcm = swapped.tobytes()
    os.pwrite(pack, pcm, sample.offset * BYTES_PER_FRAME)
    sample.scale = 1.0 / (gain * INT16_FULL_SCALE)


def build(source: Path, target: Path, jobs: int) -> None:
    print("reading the darkblack maps")
    samples = mapping(source)
    print(f"analysing {len(samples)} samples")
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        samples = list(pool.map(prepare, samples))
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
            written = pool.map(lambda s: write(s, descriptor), samples)
            for done, _ in enumerate(written, start=1):
                if done % PROGRESS_INTERVAL == 0 or done == len(samples):
                    print(f"  {done}/{len(samples)}")
        os.fsync(descriptor)
    finally:
        os.close(descriptor)
    lines = [
        FORMAT_LINE,
        f"name {NAME}",
        f"attribution {ATTRIBUTION}",
        f"rate {RATE}",
        f"channels {CHANNELS}",
        f"pack {PACK} {pack_bytes}",
    ]
    for s in samples:
        lines.append(
            " ".join(
                str(v)
                for v in (
                    "sample",
                    s.articulation,
                    s.key,
                    s.layer,
                    s.layers,
                    s.round_robin,
                    s.offset,
                    s.frames,
                    f"{s.scale:.9e}",
                    f"{DECIBELS_PER_DECADE * math.log10(s.peak):.3f}",
                )
            )
        )
    (target / MANIFEST).write_text("\n".join(lines) + "\n")
    shutil.copyfile(source / LICENCE_FILE, target / INSTALLED_LICENCE)
    (target / "ATTRIBUTION.txt").write_text(
        f"{ATTRIBUTION}\n\n{NAME}, recorded and released by Karoryfer Samples\n"
        f"under the Creative Commons CC0 1.0 Universal dedication (see {INSTALLED_LICENCE}).\n"
        f"SFZ edition: {SOURCE_URL} (commit {PINNED_COMMIT}).\n"
        f"Converted for oscillate's Fret: resampled from {SOURCE_RATE} Hz to {RATE} Hz, attack\n"
        "pre-roll trimmed to 2 ms, peak-normalised and dithered to 16-bit; the sound is\n"
        "otherwise unchanged.\n"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument(
        "--destination",
        type=Path,
        default=data_directory(),
        help="where Fret reads its library (default: %(default)s)",
    )
    parser.add_argument(
        "--source",
        type=Path,
        help="an already-extracted copy or git checkout of the library to use "
        "instead of fetching it",
    )
    parser.add_argument("--keep-source", action="store_true", help="keep the fetched checkout")
    parser.add_argument(
        "--jobs", type=int, default=DEFAULT_JOBS, help="parallel ffmpeg conversions"
    )
    arguments = parser.parse_args()
    tools = ["ffmpeg"] if arguments.source is not None else ["git", "ffmpeg"]
    for tool in tools:
        if shutil.which(tool) is None:
            raise SystemExit(f"{tool} is required")
    library = arguments.destination.resolve()
    library.parent.mkdir(parents=True, exist_ok=True)
    if arguments.source is not None:
        source = arguments.source.resolve()
        if not source.is_dir():
            raise SystemExit(f"{source} is not a directory")
        if (source / ".git").is_dir():
            if shutil.which("git") is None:
                raise SystemExit("git is required to verify the checkout")
            fetch(source)
        else:
            print(
                f"using the extracted copy in {source}: its file set is checked against "
                f"the maps, but its tree id could not be checked against {PINNED_TREE[:12]}"
            )
    else:
        source = (library.parent / DEFAULT_SOURCE_NAME).resolve()
        fetch(source)
    check_required_files(source)
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
