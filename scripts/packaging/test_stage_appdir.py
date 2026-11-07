#!/usr/bin/env python3
"""The AppDir refuses to bundle libraries whose redistribution is undecided."""

import importlib.util
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "stage-appdir.py"
SPEC = importlib.util.spec_from_file_location("stage_appdir", SCRIPT)
assert SPEC and SPEC.loader
STAGE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(STAGE)


class BlockedLibraryTests(unittest.TestCase):
    def test_mp3_encoder_is_refused(self):
        with self.assertRaisesRegex(ValueError, r"libmp3lame\.so\.0: .*#93"):
            STAGE.check_not_blocked({"libQt6Core.so.6", "libmp3lame.so.0"})

    def test_other_libraries_pass(self):
        STAGE.check_not_blocked({"libQt6Core.so.6", "libjack.so.0", "libsndfile.so.1"})


if __name__ == "__main__":
    unittest.main()
