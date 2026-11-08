"""Unit tests for tidy_changed.py's choice of translation units."""

import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import tidy_changed

CONFIGURED = {
    "src/engine/graph.cpp",
    "src/foundation/log.cpp",
    "apps/daw_quick/src/main_window.cpp",
    "tests/engine/graph_tests.cpp",
}


class TranslationUnitTests(unittest.TestCase):
    def test_changed_source_is_checked(self):
        self.assertEqual(
            tidy_changed.translation_units(["src/engine/graph.cpp"], CONFIGURED),
            ["src/engine/graph.cpp"],
        )

    def test_public_header_is_checked_through_its_source(self):
        self.assertEqual(
            tidy_changed.translation_units(["include/oscillate/foundation/log.hpp"], CONFIGURED),
            ["src/foundation/log.cpp"],
        )

    def test_private_header_is_checked_through_the_source_beside_it(self):
        self.assertEqual(
            tidy_changed.translation_units(["apps/daw_quick/src/main_window.hpp"], CONFIGURED),
            ["apps/daw_quick/src/main_window.cpp"],
        )

    def test_unconfigured_and_non_cpp_files_are_skipped(self):
        self.assertEqual(
            tidy_changed.translation_units(
                [
                    "src/plugins/vst3_only.cpp",
                    "docs/HLD.md",
                    "tools/bench/main.cpp",
                    "src/engine/CMakeLists.txt",
                ],
                CONFIGURED,
            ),
            [],
        )

    def test_each_source_is_checked_once(self):
        self.assertEqual(
            tidy_changed.translation_units(
                ["src/foundation/log.cpp", "include/oscillate/foundation/log.hpp"], CONFIGURED
            ),
            ["src/foundation/log.cpp"],
        )


DIFF = """\
diff --git a/src/engine/graph.cpp b/src/engine/graph.cpp
--- a/src/engine/graph.cpp
+++ b/src/engine/graph.cpp
@@ -4 +4 @@ void a() {
@@ -10,0 +11,3 @@ void b() {
@@ -20,2 +23,0 @@ void c() {
diff --git a/docs/HLD.md b/docs/HLD.md
--- a/docs/HLD.md
+++ b/docs/HLD.md
@@ -1,2 +1,2 @@
"""


class AddedLinesTests(unittest.TestCase):
    def test_hunks_become_inclusive_ranges_and_deletions_are_skipped(self):
        self.assertEqual(
            tidy_changed.added_lines(DIFF),
            {"src/engine/graph.cpp": [[4, 4], [11, 13]], "docs/HLD.md": [[1, 2]]},
        )


class LineFilterTests(unittest.TestCase):
    def test_filter_names_changed_cpp_files_and_their_lines(self):
        self.assertEqual(
            json.loads(tidy_changed.line_filter(tidy_changed.added_lines(DIFF))),
            [{"name": "src/engine/graph.cpp", "lines": [[4, 4], [11, 13]]}],
        )


class ChangedFindingsTests(unittest.TestCase):
    ROOT = "/work/oscillate"
    GRAPH = {"src/engine/graph.cpp": [[4, 4], [11, 13]]}

    def test_keeps_findings_on_changed_lines_only(self):
        output = "\n".join(
            [
                "/work/oscillate/src/engine/graph.cpp:4:2: error: bad [bugprone-x]",
                "/work/oscillate/src/engine/graph.cpp:8:2: error: older [bugprone-x]",
                "/work/oscillate/src/engine/graph.cpp:13:1: error: edge [bugprone-x]",
                "/work/oscillate/src/engine/graph.cpp:12:1: note: Calling implicit constructor",
            ]
        )
        found = tidy_changed.changed_findings(output, self.GRAPH, self.ROOT)
        self.assertEqual(len(found), 2)
        self.assertIn("graph.cpp:4:2", found[0])
        self.assertIn("graph.cpp:13:1", found[1])

    def test_finding_in_an_unchanged_header_is_dropped(self):
        output = (
            "/work/oscillate/src/engine/../../include/oscillate/foundation/error.hpp:27:8: "
            "error: uninitialised [clang-analyzer-core.uninitialized.Assign,-warnings-as-errors]"
        )
        self.assertEqual(tidy_changed.changed_findings(output, self.GRAPH, self.ROOT), [])

    def test_changed_header_line_reached_through_a_relative_path_is_kept(self):
        output = (
            "/work/oscillate/src/engine/../../include/oscillate/engine/graph.hpp:3:1: "
            "error: bad [performance-x,-warnings-as-errors]"
        )
        lines = {"include/oscillate/engine/graph.hpp": [[2, 5]]}
        self.assertEqual(len(tidy_changed.changed_findings(output, lines, self.ROOT)), 1)


class ConfiguredSourcesTests(unittest.TestCase):
    PRESET = "quick"

    def test_relative_file_is_resolved_against_its_directory(self):
        with tempfile.TemporaryDirectory() as root:
            root = os.path.realpath(root)
            build = Path(root) / "build" / self.PRESET
            build.mkdir(parents=True)
            entries = [
                {"directory": str(build), "file": "../../src/engine/graph.cpp"},
                {"directory": str(build), "file": f"{root}/src/foundation/log.cpp"},
            ]
            (build / "compile_commands.json").write_text(json.dumps(entries))
            with mock.patch.object(tidy_changed, "ROOT", Path(root)):
                self.assertEqual(
                    tidy_changed.configured_sources(self.PRESET),
                    {"src/engine/graph.cpp", "src/foundation/log.cpp"},
                )


class MovedBaseTests(unittest.TestCase):
    """GitHub checks out the pull request merged into the current base tip."""

    def setUp(self):
        # A Git hook's GIT_DIR and friends would point these commands at its repository.
        scrubbed = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
        scrubbed["GIT_CONFIG_GLOBAL"] = os.devnull
        scrubbed["GIT_CONFIG_SYSTEM"] = os.devnull
        patcher = mock.patch.dict(os.environ, scrubbed, clear=True)
        patcher.start()
        self.addCleanup(patcher.stop)

    def git(self, *arguments):
        return subprocess.run(
            ["git", *arguments], check=True, capture_output=True, text=True, cwd=self.root
        ).stdout.strip()

    def write(self, name, lines):
        (Path(self.root) / name).write_text("".join(f"{line}\n" for line in lines))

    def test_commits_that_reach_the_base_later_are_not_the_change(self):
        with tempfile.TemporaryDirectory() as root:
            self.root = root
            self.git("init", "-q", "-b", "main")
            self.git("config", "user.email", "ci@example.invalid")
            self.git("config", "user.name", "CI")
            self.write("a.cpp", ["a1", "a2", "a3"])
            self.write("b.cpp", ["b1", "b2"])
            self.git("add", ".")
            self.git("commit", "-q", "-m", "base")
            recorded_base = self.git("rev-parse", "HEAD")
            self.git("checkout", "-q", "-b", "change")
            self.write("a.cpp", ["a1", "changed", "a3"])
            self.git("commit", "-q", "-am", "the change")
            self.git("checkout", "-q", "main")
            self.write("b.cpp", ["b1", "b2", "landed later"])
            self.git("commit", "-q", "-am", "another pull request")
            self.git("merge", "-q", "--no-ff", "-m", "merge", "change")
            self.assertEqual(tidy_changed.changed_lines("HEAD^1", cwd=root), {"a.cpp": [[2, 2]]})
            # The event's recorded base would also count the other pull request.
            self.assertIn("b.cpp", tidy_changed.changed_lines(recorded_base, cwd=root))


if __name__ == "__main__":
    unittest.main()
