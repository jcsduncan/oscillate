"""Evidence contract for the VST3 compatibility matrix runner."""

import json
import os
import signal
import sys
import tempfile
import unittest
from pathlib import Path

from qualify_vst3_matrix import (
    DEFAULT_MANIFEST,
    STEPS,
    HarnessError,
    complete_steps,
    describe_exit,
    find_bundle,
    load_manifest,
    markdown,
    run_probe,
    select_classes,
    tally,
)

PASS = {"status": "pass", "detail": "ok", "metrics": {}}


class Manifest(unittest.TestCase):
    def test_the_pinned_manifest_is_valid(self):
        plugins = load_manifest(DEFAULT_MANIFEST)
        self.assertIn("Surge XT", [plugin["name"] for plugin in plugins])

    def test_rejects_a_plugin_without_a_pin(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "manifest.json"
            path.write_text(
                json.dumps(
                    {
                        "plugins": [
                            {"name": "X", "url": "https://example.invalid/x.zip", "archive": "zip"}
                        ]
                    }
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(HarnessError, "SHA-256"):
                load_manifest(path)


class Steps(unittest.TestCase):
    def test_a_crash_marks_the_next_step_and_skips_the_rest(self):
        evidence = {"steps": {"scan": PASS, "load": PASS}}
        steps = complete_steps(evidence, "crashed with SIGSEGV")
        self.assertEqual(list(steps), list(STEPS))
        self.assertEqual(steps["load"]["status"], "pass")
        self.assertEqual(steps["render_64"]["status"], "crash")
        self.assertEqual(steps["export"]["status"], "not-run")

    def test_a_clean_exit_leaves_unrecorded_steps_not_run(self):
        steps = complete_steps({"steps": {"scan": PASS}}, "")
        self.assertEqual(steps["load"]["status"], "not-run")

    def test_a_timed_out_probe_keeps_its_last_line(self):
        sleeper = "import time; print('loading', flush=True); time.sleep(5)"
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "out.json"
            evidence, outcome = run_probe(
                [sys.executable, "-c", sleeper], output, 1, dict(os.environ)
            )
        self.assertIsNone(evidence)
        self.assertEqual(outcome, "timed out after 1 s: loading")

    def test_names_the_signal(self):
        self.assertEqual(describe_exit(-signal.SIGSEGV), "crashed with SIGSEGV")
        self.assertEqual(describe_exit(3), "exited with status 3")


class Classes(unittest.TestCase):
    LISTING = {
        "classes": [
            {"id": "a", "name": "Synth", "category": "Audio Module Class", "kind": "instrument"},
            {"id": "b", "name": "Controller", "category": "Component Controller Class"},
        ]
    }

    def test_keeps_only_audio_modules(self):
        selected, missing = select_classes(self.LISTING, None)
        self.assertEqual([klass["id"] for klass in selected], ["a"])
        self.assertEqual(missing, [])

    def test_reports_wanted_classes_the_bundle_lacks(self):
        selected, missing = select_classes(self.LISTING, ["Synth", "Reverb"])
        self.assertEqual([klass["id"] for klass in selected], ["a"])
        self.assertEqual(missing, ["Reverb"])


class Report(unittest.TestCase):
    def test_finds_a_nested_bundle(self):
        with tempfile.TemporaryDirectory() as directory:
            nested = Path(directory) / "usr" / "lib" / "vst3" / "Thing.vst3"
            nested.mkdir(parents=True)
            self.assertEqual(find_bundle(Path(directory), "Thing.vst3"), nested)
            self.assertIsNone(find_bundle(Path(directory), "Other.vst3"))

    def test_one_row_per_class_and_a_row_for_unavailable_plugins(self):
        steps = complete_steps({"steps": {step: PASS for step in STEPS}}, "")
        steps["load"] = {
            "status": "pass",
            "detail": "",
            "metrics": {
                "latency_samples": 0,
                "latency_check": "stable",
                "tail_samples": None,
                "parameters": 12,
            },
        }
        matrix = {
            "plugins": [
                {
                    "name": "Synth",
                    "version": "1",
                    "status": "qualified",
                    "bundles": [
                        {
                            "bundle": "Synth.vst3",
                            "scan": {"status": "pass"},
                            "classes": [{"name": "Synth", "kind": "instrument", "steps": steps}],
                        }
                    ],
                },
                {
                    "name": "Closed",
                    "version": "2",
                    "status": "unavailable",
                    "detail": "account only",
                    "bundles": [],
                },
            ]
        }
        table = markdown(matrix).splitlines()
        self.assertEqual(len(table), 4)
        self.assertIn("tail infinite/unknown, 12 parameters", table[2])
        self.assertIn("unavailable: account only", table[3])
        self.assertEqual(table[2].count("|"), table[0].count("|"))
        self.assertEqual(table[3].count("|"), table[0].count("|"))
        self.assertEqual(tally(matrix), {"pass": len(STEPS)})


if __name__ == "__main__":
    unittest.main()
