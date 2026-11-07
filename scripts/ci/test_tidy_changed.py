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


class LineFilterTests(unittest.TestCase):
    def test_filter_names_only_changed_cpp_files(self):
        self.assertEqual(
            json.loads(
                tidy_changed.line_filter(
                    ["src/engine/graph.cpp", "include/oscillate/engine/graph.hpp", "docs/HLD.md"]
                )
            ),
            [{"name": "src/engine/graph.cpp"}, {"name": "include/oscillate/engine/graph.hpp"}],
        )


if __name__ == "__main__":
    unittest.main()
