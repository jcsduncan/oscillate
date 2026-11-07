"""Reject MP3 codec or notice changes until the source ledger is reviewed."""

from __future__ import annotations

import copy
import hashlib
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from codec_source_gate import (
    STAGED_LEDGER_PATH,
    load_ledger,
    stage_ledger,
    verify_codec_gate,
    verify_lame_fft_fingerprint,
    verify_staged_ledger,
)

SOURCE_ROOT = Path(__file__).resolve().parents[2]


class CodecSourceGateTests(unittest.TestCase):
    def setUp(self) -> None:
        self.ledger = load_ledger(SOURCE_ROOT)
        components = self.ledger["components"]
        records = [{"path": "usr/bin/oscillate", "needed": ["libsndfile.so.1"]}]
        notices = []
        for name, component in components.items():
            needed = []
            if name == "libsndfile":
                needed = ["libmp3lame.so.0", "libmpg123.so.0"]
            elif name == "libmp3lame":
                needed = ["libmpg123.so.0"]
            records.append(
                {
                    "path": component["path"],
                    "kind": "shared-library",
                    "package": component["binary_package"],
                    "version": component["binary_version"],
                    "sha256": component["binary_sha256"],
                    "needed": needed,
                }
            )
            notices.append(
                {
                    "path": component["path"],
                    "binary_package": component["binary_package"],
                    "binary_version": component["binary_version"],
                    "sha256": component["binary_sha256"],
                    "notice_sha256": component["notice_sha256"],
                }
            )
        self.appdir = {"elf_files": records}
        self.notices = {"copied_elfs": notices}

    def test_known_codec_chain_is_accepted(self) -> None:
        verify_codec_gate(self.appdir, self.notices, self.ledger)

    def test_staged_source_ledger_tamper_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            stage_ledger(root, SOURCE_ROOT)
            self.assertEqual(verify_staged_ledger(root, SOURCE_ROOT)["schema"], 1)
            (root / STAGED_LEDGER_PATH).write_text("{}")
            with self.assertRaisesRegex(ValueError, "differs from reviewed copy"):
                verify_staged_ledger(root, SOURCE_ROOT)

    def test_changed_encoder_or_notice_requires_review(self) -> None:
        changed = copy.deepcopy(self.appdir)
        changed["elf_files"][2]["sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "package or notice drift.*libmp3lame"):
            verify_codec_gate(changed, self.notices, self.ledger)
        changed = copy.deepcopy(self.notices)
        changed["copied_elfs"][1]["notice_sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "package or notice drift.*libmp3lame"):
            verify_codec_gate(self.appdir, changed, self.ledger)

    def test_missing_encoder_or_dependency_requires_review(self) -> None:
        changed = copy.deepcopy(self.appdir)
        changed["elf_files"] = [
            item for item in changed["elf_files"] if item["path"] != "usr/lib/libmp3lame.so.0"
        ]
        with self.assertRaisesRegex(ValueError, "component is missing: libmp3lame"):
            verify_codec_gate(changed, self.notices, self.ledger)
        changed = copy.deepcopy(self.appdir)
        changed["elf_files"][1]["needed"] = []
        with self.assertRaisesRegex(ValueError, "dependency path changed"):
            verify_codec_gate(changed, self.notices, self.ledger)

    def test_fft_classification_cannot_be_silently_changed(self) -> None:
        changed = copy.deepcopy(self.ledger)
        changed["lame_source_evidence"]["declared_fft_c_license"] = "LGPL-2.1"
        with self.assertRaisesRegex(ValueError, "fft.c licence classification changed"):
            verify_codec_gate(self.appdir, self.notices, changed)

    def test_fft_object_fingerprint_detects_changed_encoder_code(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            encoder = root / self.ledger["components"]["libmp3lame"]["path"]
            encoder.parent.mkdir(parents=True)
            encoder.write_bytes(b"prefix matching fft bytes suffix")
            evidence = copy.deepcopy(self.ledger)
            evidence["lame_source_evidence"]["fft_text_match"] = {
                "shared_elf_file_offset": 7,
                "length": 18,
                "sha256": hashlib.sha256(b"matching fft bytes").hexdigest(),
            }
            verify_lame_fft_fingerprint(root, evidence)
            encoder.write_bytes(b"prefix altered  fft bytes suffix")
            with self.assertRaisesRegex(ValueError, "fft.c code fingerprint changed"):
                verify_lame_fft_fingerprint(root, evidence)


if __name__ == "__main__":
    unittest.main()
