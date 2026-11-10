"""Unit tests for install_kits.py on a small synthetic SFZ kit laid out as
Naked Drums is: its keymap and macros, a kick on two close mics, the
overheads and the close room, two snares on two summed top mics, and a
tuned tom. Needs ffmpeg and ffprobe."""

import array
import math
import shutil
import tempfile
import unittest
import wave
from pathlib import Path

import install_kits as kits

RATE = 44_100
SECONDS = 0.25
FRAMES = int(RATE * SECONDS)
TONE_HZ = 220.0
DECAY_SECONDS = 0.05
PEAK = 0.5
INT16_FULL = 32_767
STEREO = 2
ROUND_ROBINS = 2
TOM_CENTS = -200
SNARE_KEY = 38
SECOND_SNARE_KEY = 40
KICK_KEY = 36
TOM_KEY = 45
TRIM_DB = 6
EXPECTED = {
    ("kick", "hit"): ROUND_ROBINS,
    ("snare", "hit"): ROUND_ROBINS,
    ("snare", "alt"): ROUND_ROBINS,
    ("tom3", "hit"): ROUND_ROBINS,
}
ROOT = "Wilkinson Audio/Naked Drums"

KEYMAP = f"""// keymap
#define $KEY_KICK {KICK_KEY}
#define $KEY_SNR1 {SNARE_KEY}
#define $KEY_SNR2 {SECOND_SNARE_KEY}
#define $KEY_TOM3 {TOM_KEY}
#define $SW_NON 12
"""
VELTRACK = 99
# Velocity squared, in dB: full tracking.
SQUARED_DB = 40.0
MACRO = f"""#define $VELTRACK {VELTRACK}
#define $dir ../Samples
#define $ext wav
#define $TRIM_OH_KICK {TRIM_DB}
#define $TUN_TOM3 {TOM_CENTS}
#define $center Center
<control>
default_path=$dir/
"""
# Two velocity layers of one round robin, recorded alike: the SFZ's
# velocity tracking alone sets them apart.
SOFT_TOP = 63
LOUD_BOTTOM = SOFT_TOP + 1
REGIONS = f"""#define $POS 1
#define $RR one
<region> lovel=1 hivel={SOFT_TOP} seq_position=$POS sample=$MIC_$ART_$RR.$ext
#define $RR two
<region> lovel={LOUD_BOTTOM} hivel=127 seq_position=$POS sample=$MIC_$ART_$RR.$ext
"""
# Powers agree within dither.
POWER_TOLERANCE_DB = 0.05
# Each group file: its master opcodes, then a group per key (name, key define).
GROUPS = {
    "kick_in": ("", [("KickIn", "Kick", "$KEY_KICK")]),
    "kick_sub": ("", [("KickSub", "Kick", "$KEY_KICK")]),
    "kick_oh": ("master_volume=$TRIM_OH_KICK", [("KickOH", "Kick", "$KEY_KICK")]),
    "kick_cr": ("", [("KickCR", "Kick", "$KEY_KICK")]),
    "snare_top1": ("", [("Top1", "A", "$KEY_SNR1"), ("Top1", "B", "$KEY_SNR2")]),
    "snare_top2": ("", [("Top2", "A", "$KEY_SNR1"), ("Top2", "B", "$KEY_SNR2")]),
    "tom3_di": ("tune=$TUN_TOM3", [("Tom3", "D3", "$KEY_TOM3")]),
}
# The mic files the instrument includes: stem -> group files.
MIC_FILES = {
    "DI_Kick_In": ["kick_in"],
    "DI_Kick_Sub": ["kick_sub"],
    "OH_Kick": ["kick_oh"],
    "CR_Kick": ["kick_cr"],
    "DI_Snare_Tops": ["snare_top1", "snare_top2"],
    "DI_Tom3": ["tom3_di"],
}
STEREO_MICS = {"KickOH"}


def write_tone(path: Path, channels: int, delay: int = 0) -> None:
    """A decaying tone, `delay` frames of silence before it."""
    values = array.array("h")
    for i in range(FRAMES):
        t = max(0, i - delay) / RATE
        x = PEAK * math.exp(-t / DECAY_SECONDS) * math.sin(2 * math.pi * TONE_HZ * t)
        values.extend([round(x * INT16_FULL) if i >= delay else 0] * channels)
    with wave.open(str(path), "wb") as file:
        file.setnchannels(channels)
        file.setsampwidth(2)
        file.setframerate(RATE)
        file.writeframes(values.tobytes())


def write_kit(base: Path) -> Path:
    """The synthetic kit; returns the source folder."""
    root = base / ROOT
    data = root / "Data"
    for folder in ("Data/stereo", "Data/group", "Data/region", "Stereo", "Samples"):
        (root / folder).mkdir(parents=True)
    (data / "keymap.txt").write_text(KEYMAP)
    (data / "macro.txt").write_text(MACRO)
    (data / "region" / "v1.txt").write_text(REGIONS)
    for name, (master, groups) in GROUPS.items():
        lines = [master]
        for mic, articulation, key in groups:
            lines += [
                "<group>",
                f"key={key}",
                "group_label=$center",
                f"#define $MIC {mic}",
                f"#define $ART {articulation}",
                '#include "../Data/region/v1.txt"',
            ]
            for robin in ("one", "two"):
                write_tone(
                    root / "Samples" / f"{mic}_{articulation}_{robin}.wav",
                    STEREO if mic in STEREO_MICS else 1,
                )
        (data / "group" / f"{name}.txt").write_text("\n".join(lines) + "\n")
    for stem, groups in MIC_FILES.items():
        text = "\n".join(f'<master>\n#include "../Data/group/{g}.txt"' for g in groups)
        (data / "stereo" / f"{stem}.txt").write_text(text + "\n")
    includes = ['#include "../Data/keymap.txt"', '#include "../Data/macro.txt"']
    includes += [f'#include "../Data/stereo/{stem}.txt"' for stem in MIC_FILES]
    (root / "Stereo" / "Naked Drums.sfz").write_text("\n".join(includes) + "\n")
    return base


class SfzTests(unittest.TestCase):
    """The SFZ reader: defines, includes, headers."""

    def setUp(self):
        """A fresh folder per test."""
        self.base = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.base)

    def test_defines_apply_in_order_longest_first(self):
        """Defines apply in order longest first."""
        sfz = self.base / "a.sfz"
        sfz.write_text(
            "#define $A one\n#define $AB two\nx=$AB_$A\n#define $A three\ny=$A\n"
            "// a comment\n/* a\nblock */z=1\n"
        )
        lines = kits.SfzText(self.base, self.base).lines(sfz)
        self.assertEqual(lines, ["x=two_one", "y=three", " z=1"])

    def test_includes_outside_the_source_are_refused(self):
        """Includes outside the source are refused."""
        sfz = self.base / "kit" / "a.sfz"
        sfz.parent.mkdir()
        (self.base / "outside.txt").write_text("x=1\n")
        sfz.write_text('#include "../outside.txt"\n')
        with self.assertRaises(SystemExit):
            kits.SfzText(sfz.parent, sfz.parent).lines(sfz)

    def test_regions_inherit_master_and_group_opcodes(self):
        """Regions inherit master and group opcodes."""
        lines = [
            "<master> master_volume=3",
            "<group> key=36",
            "<region> sample=a.wav volume=1",
            "<group> key=38",
            "<region> sample=b.wav",
            "<master>",
            "<region> sample=c.wav",
        ]
        regions = kits.sfz_regions(lines)
        self.assertEqual(
            regions[0], {"master_volume": "3", "key": "36", "sample": "a.wav", "volume": "1"}
        )
        self.assertEqual(regions[1], {"master_volume": "3", "key": "38", "sample": "b.wav"})
        self.assertEqual(regions[2], {"sample": "c.wav"})

    def test_velocity_tracking_follows_the_sfz(self):
        """Velocity tracking follows the SFZ."""
        soft_velocity = 27
        full_tracking = 100
        self.assertEqual(kits.velocity_db(kits.MAX_VELOCITY, VELTRACK), 0.0)
        self.assertAlmostEqual(
            kits.velocity_db(soft_velocity, VELTRACK),
            VELTRACK / kits.PERCENT * SQUARED_DB * math.log10(soft_velocity / kits.MAX_VELOCITY),
        )
        self.assertAlmostEqual(
            kits.velocity_db(soft_velocity, full_tracking),
            SQUARED_DB * math.log10(soft_velocity / kits.MAX_VELOCITY),
        )

    def test_opcode_values_may_hold_spaces(self):
        """Opcode values may hold spaces."""
        self.assertEqual(
            kits.sfz_opcodes("region_label=10 one sample=a b.wav"),
            {"region_label": "10 one", "sample": "a b.wav"},
        )


class NakedTests(unittest.TestCase):
    """Naked's mapping and install, on the synthetic kit."""

    @classmethod
    def setUpClass(cls):
        """Writes the synthetic kit and reads its hits once."""
        cls.base = Path(tempfile.mkdtemp())
        cls.source = write_kit(cls.base / "source")
        cls.hits = kits.naked_hits(cls.source, EXPECTED)

    @classmethod
    def tearDownClass(cls):
        """Removes the synthetic kit."""
        shutil.rmtree(cls.base)

    def hit(self, piece, articulation, layer=LOUD_BOTTOM):
        """A stroke's hit on the velocity layer from `layer` up."""
        return next(
            h
            for h in self.hits
            if (h.piece, h.articulation) == (piece, articulation)
            and h.name.split("/")[4] == str(layer)
        )

    def test_every_mic_of_a_hit_joins_it(self):
        """Every mic of a hit joins it."""
        kick = self.hit("kick", "hit")
        played = [len(parts) for parts in kick.channels]
        oh_left, oh_right = kits.NAKED_FAR["OH"]
        (room,) = kits.NAKED_FAR["CR"]
        kick_in, kick_sub = kits.NAKED_CLOSE["Kick_In"][0], kits.NAKED_CLOSE["Kick_Sub"][0]
        recorded = (kick_in, kick_sub, oh_left, oh_right, room)
        for channel in recorded:
            self.assertEqual(played[channel], 1)
        self.assertEqual(sum(played), len(recorded))
        self.assertEqual(
            [p.channel for p in kick.channels[oh_left] + kick.channels[oh_right]], [0, 1]
        )
        self.assertEqual(kick.channels[oh_left][0].gain_db, TRIM_DB)

    def test_the_second_snare_is_its_alt_and_the_tops_sum(self):
        """The second snare is its alt and the tops sum."""
        snare = self.hit("snare", "hit")
        second = self.hit("snare", "alt")
        top = kits.NAKED_CLOSE["Snare_Tops"][0]
        self.assertEqual(len(snare.channels[top]), 2)
        self.assertTrue(all("_A_" in p.path for p in snare.channels[top]))
        self.assertTrue(all("_B_" in p.path for p in second.channels[top]))

    def test_toms_share_a_channel_and_keep_their_tuning(self):
        """Toms share a channel and keep their tuning."""
        tom = self.hit("tom3", "hit")
        (channel,) = kits.NAKED_CLOSE["Tom3"]
        self.assertEqual(kits.NAKED_MICS[channel].pieces, ("tom1", "tom2", "tom3", "tom4", "tom5"))
        self.assertEqual(tom.channels[channel][0].cents, TOM_CENTS)

    def test_unexpected_counts_are_refused(self):
        """Unexpected counts are refused."""
        with self.assertRaises(SystemExit):
            kits.naked_hits(self.source, {("kick", "hit"): ROUND_ROBINS})

    def test_an_unknown_stroke_is_refused(self):
        """An unknown stroke is refused."""
        with self.assertRaises(SystemExit):
            kits.naked_stroke("Cowbell", "Center", "56", {"$KEY_SNR1": "38", "$KEY_SNR2": "40"})

    def test_install_writes_the_kit_its_sources_and_its_credit(self):
        """Install writes the kit, its sources and its credit."""
        destination = self.base / "installed"
        target = kits.install(kits.KITS["naked"], kits.NAKED_MICS, self.hits, destination, 1)
        manifest = (target / kits.MANIFEST).read_text().splitlines()
        self.assertEqual(manifest[0], kits.FORMAT_LINE)
        self.assertIn(f"attribution {kits.NAKED_NOTICE}", manifest)
        self.assertIn(f"channels {len(kits.NAKED_MICS)}", manifest)
        (toms,) = kits.NAKED_CLOSE["Tom1"]
        (room,) = kits.NAKED_FAR["CR"]
        self.assertIn(f"mic {toms} toms tom1+tom2+tom3+tom4+tom5 mono", manifest)
        self.assertIn(f"mic {room} room - centre", manifest)
        hit_lines = [line for line in manifest if line.startswith("hit ")]
        self.assertEqual(len(hit_lines), sum(EXPECTED.values()))
        pack_line = next(line for line in manifest if line.startswith("pack "))
        self.assertEqual(int(pack_line.split()[2]), (target / kits.PACK).stat().st_size)
        sources = (target / kits.SOURCES).read_text().splitlines()[1:]
        self.assertEqual(len(sources), len(hit_lines))
        offsets = {line.split()[4] for line in hit_lines}
        self.assertEqual({line.split("\t")[0] for line in sources}, offsets)
        for line in sources:
            for entry in line.split("\t")[4:]:
                path = entry.split(":", 1)[1].rsplit("#", 1)[0]
                self.assertTrue((self.source / path).is_file(), path)
        credit = (target / kits.ATTRIBUTION_FILE).read_text()
        self.assertTrue(credit.startswith(kits.NAKED_NOTICE))
        for fact in ("kinwie", kits.NAKED_COMMIT, kits.NAKED_LICENCE_COMMIT, kits.LICENCE_URL):
            self.assertIn(fact, credit)

    def test_a_soft_layer_measures_as_the_sfz_plays_it(self):
        """A soft layer measures as the SFZ plays it."""
        work = self.base / "layers"
        work.mkdir(exist_ok=True)
        filters = kits.resampler()
        soft = kits.measure((self.hit("kick", "hit", 1), kits.NAKED_MICS, filters, str(work)))
        loud = kits.measure((self.hit("kick", "hit"), kits.NAKED_MICS, filters, str(work)))
        self.assertAlmostEqual(
            soft.power_db - loud.power_db,
            kits.velocity_db(SOFT_TOP, VELTRACK),
            delta=POWER_TOLERANCE_DB,
        )

    def test_a_silent_mic_stays_undithered(self):
        """A silent mic stays undithered."""
        work = self.base / "work"
        work.mkdir(exist_ok=True)
        kick = self.hit("kick", "hit")
        measured = kits.measure((kick, kits.NAKED_MICS, kits.resampler(), str(work)))
        self.assertFalse(measured.dropped)
        pcm = array.array("h")
        pcm.frombytes(Path(measured.work).read_bytes())
        channels = len(kits.NAKED_MICS)
        snare_top = kits.NAKED_CLOSE["Snare_Tops"][0]
        self.assertFalse(any(pcm[snare_top::channels]))
        self.assertTrue(any(pcm[0::channels]))

    def test_a_file_with_more_channels_than_the_kit_plays_is_refused(self):
        """A file with more channels than the kit plays is refused."""
        kick = self.hit("kick", "hit")
        (room,) = kits.NAKED_FAR["CR"]
        stereo_room = kick.channels[room][0].path
        decoded = kits.read_parts(kick, kits.resampler())
        key = (stereo_room, 0.0)
        decoded[key] = decoded[key] * STEREO
        with self.assertRaises(SystemExit):
            kits.mix_channels(kick, decoded)


if __name__ == "__main__":
    unittest.main()
