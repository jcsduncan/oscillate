"""Unit tests for tidy_changed.py's choice of translation units."""

import json
import unittest

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


if __name__ == "__main__":
    unittest.main()
