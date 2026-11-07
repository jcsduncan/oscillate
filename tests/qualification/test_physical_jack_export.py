"""Offline evidence gates for the explicitly opt-in JACK runner."""

import struct
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from qualify_physical_jack_export import parse_result, run_once, wav_summary


def result(**updates: str) -> str:
    fields = {
        "published": "durable",
        "rate": "48000",
        "requested_frames": "64",
        "server_frames": "64",
        "expected_frames": "2",
        "captured_frames": "2",
        "tail_frames": "0",
        "latency_frames": "0",
        "callback_blocks": "2",
        "over_period_blocks": "0",
        "xruns": "0",
        "render_failures": "0",
        "invalid_buffers": "0",
        "oversized_blocks": "0",
        "source_health": "passed",
        "named_connections": "passed",
    }
    fields.update(updates)
    return "jack_export_result " + " ".join(f"{key}={value}" for key, value in fields.items())


def wav(samples: tuple[float, ...]) -> bytes:
    payload = struct.pack("<" + "f" * len(samples), *samples)
    fmt = struct.pack("<HHIIHH", 3, 2, 48_000, 384_000, 8, 32)
    return (
        b"RIFF"
        + struct.pack("<I", 4 + 8 + len(fmt) + 8 + len(payload))
        + b"WAVEfmt "
        + struct.pack("<I", len(fmt))
        + fmt
        + b"data"
        + struct.pack("<I", len(payload))
        + payload
    )


class PhysicalJackExportEvidence(unittest.TestCase):
    def test_accepts_complete_native_capture_and_rejects_gaps(self) -> None:
        self.assertEqual(parse_result(result(), 64)["expected_frames"], 2)
        for changed, reason in (
            ({"xruns": "1"}, "xruns"),
            ({"over_period_blocks": "1"}, "over_period"),
            ({"server_frames": "128"}, "server_frames"),
            ({"captured_frames": "1"}, "incomplete"),
            ({"callback_blocks": "0"}, "callback"),
            ({"published": "durability_uncertain"}, "published"),
        ):
            with self.subTest(changed=changed), self.assertRaisesRegex(ValueError, reason):
                parse_result(result(**changed), 64)

    def test_wav_checks_header_frames_and_finite_audio(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "out.wav"
            path.write_bytes(wav((0.25, -0.25, 0.1, -0.1)))
            checked = wav_summary(path, 2, True)
            self.assertEqual(checked["frames"], 2)
            self.assertAlmostEqual(checked["peak"], 0.25)
            with self.assertRaisesRegex(ValueError, "wrong frame count"):
                wav_summary(path, 3, False)
            path.write_bytes(wav((float("nan"), 0.0)))
            with self.assertRaisesRegex(ValueError, "non-finite"):
                wav_summary(path, 1, False)

    def test_failed_process_must_preserve_previous_target(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            parent = Path(folder)
            target = parent / "previous.wav"
            target.write_bytes(b"previous render")
            failed = type("Result", (), {"returncode": 1, "stdout": "injected xrun"})()
            with (
                patch("qualify_physical_jack_export.subprocess.run", return_value=failed),
                self.assertRaisesRegex(RuntimeError, "without replacing target"),
            ):
                run_once(
                    parent / "tool",
                    parent / "project",
                    target,
                    "system:playback_1",
                    "system:playback_2",
                    64,
                    1,
                    parent,
                    False,
                )
            self.assertEqual(target.read_bytes(), b"previous render")

            def corrupt(*_args, **_kwargs):
                target.write_bytes(b"changed")
                return failed

            with (
                patch("qualify_physical_jack_export.subprocess.run", side_effect=corrupt),
                self.assertRaisesRegex(RuntimeError, "changed the prior target"),
            ):
                run_once(
                    parent / "tool",
                    parent / "project",
                    target,
                    "system:playback_1",
                    "system:playback_2",
                    64,
                    1,
                    parent,
                    False,
                )


if __name__ == "__main__":
    unittest.main()
