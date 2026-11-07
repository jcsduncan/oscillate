"""Focused checks for the opt-in recorded-material listening pack."""

import array
import hashlib
import sys
import tempfile
import unittest
import wave
from pathlib import Path

from listening_pack import gain_for, prepare
from recorded_oracle import measure


class ListeningPackTests(unittest.TestCase):
    def test_gain_matches_rms_or_respects_peak_ceiling(self):
        self.assertAlmostEqual(gain_for(0.1, 0.5, 0.05), 1.8)
        self.assertAlmostEqual(gain_for(0.1, 0.8, 0.05), 1.125)
        self.assertEqual(gain_for(0.1, 0, 0), 1)
        with self.assertRaises(ValueError):
            gain_for(float("nan"), 0.5, 0.1)

    def test_pack_checks_digest_and_writes_safe_float_audio(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.wav"
            samples = array.array("h", [20000, -20000] * 4800)
            if sys.byteorder != "little":
                samples.byteswap()
            with wave.open(str(source), "wb") as stream:
                stream.setnchannels(2)
                stream.setsampwidth(2)
                stream.setframerate(48000)
                stream.writeframes(samples.tobytes())
            digest = hashlib.sha256(source.read_bytes()).hexdigest()
            render_dir = root / "renders"
            render_dir.mkdir()
            # This test checks the pack contract; the audition engine is tested
            # separately. Reusing the source also makes exact RMS predictable.
            (render_dir / "repitch_identity.wav").write_bytes(source.read_bytes())
            (render_dir / "manifest.tsv").write_text(
                f"# input_sha256\t{digest}\n# input_frames\t4800\n"
                "file\tmode\tcase\tstatus\texpected_frames\toutput_frames\n"
                "repitch_identity.wav\trepitch\tidentity\tok\t4800\t4800\n"
            )
            output = root / "pack"
            rows = prepare(source, render_dir, output, ("repitch_identity",))
            self.assertEqual([row["case"] for row in rows], ["source", "repitch_identity"])
            self.assertAlmostEqual(float(rows[1]["rms_db_vs_reference"]), 0, places=3)
            self.assertTrue((output / "index.tsv").is_file())
            for row in rows:
                frames, peak, clipped, _ = measure(output / row["file"])
                self.assertEqual(frames, 4800)
                self.assertLessEqual(peak, 0.9)
                self.assertEqual(clipped, 0)
            with self.assertRaisesRegex(ValueError, "empty"):
                prepare(source, render_dir, output, ("repitch_identity",))
            (render_dir / "manifest.tsv").write_text(
                (render_dir / "manifest.tsv").read_text().replace(digest, "0" * 64)
            )
            with self.assertRaisesRegex(ValueError, "digest"):
                prepare(source, render_dir, root / "other", ("repitch_identity",))


if __name__ == "__main__":
    unittest.main()
