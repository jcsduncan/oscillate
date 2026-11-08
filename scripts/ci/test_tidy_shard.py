"""Unit tests for tidy_shard.py's split of the translation units and its findings."""

import unittest

import tidy_shard

CONFIGURED = {
    "apps/daw_quick/src/main_window.cpp",
    "src/engine/graph.cpp",
    "src/foundation/log.cpp",
    "tests/engine/graph_tests.cpp",
    "build/quick/apps/daw_quick/mocs_compilation.cpp",
    "third_party/pffft/pffft.c",
}
SHARD_COUNT = 3
ROOT = "/__w/oscillate/oscillate"
BUILD = f"{ROOT}/build/quick"
FINDINGS = "findings/shard-0.txt"
WARNING = ":12:3: warning: use auto [modernize-use-auto]"
ERROR = ":7:1: error: unknown type name 'Foo' [clang-diagnostic-error]"


class ShardTests(unittest.TestCase):
    def test_shards_cover_every_project_source_once(self):
        shards = [
            tidy_shard.shard_sources(CONFIGURED, shard, SHARD_COUNT) for shard in range(SHARD_COUNT)
        ]
        merged = sorted(source for shard in shards for source in shard)
        self.assertEqual(
            merged,
            [
                "apps/daw_quick/src/main_window.cpp",
                "src/engine/graph.cpp",
                "src/foundation/log.cpp",
                "tests/engine/graph_tests.cpp",
            ],
        )

    def test_sources_are_dealt_round_robin_in_sorted_order(self):
        self.assertEqual(
            tidy_shard.shard_sources(CONFIGURED, 0, SHARD_COUNT),
            ["apps/daw_quick/src/main_window.cpp", "tests/engine/graph_tests.cpp"],
        )

    def test_generated_and_third_party_sources_are_skipped(self):
        self.assertEqual(
            tidy_shard.shard_sources(CONFIGURED, 0, 1),
            [
                "apps/daw_quick/src/main_window.cpp",
                "src/engine/graph.cpp",
                "src/foundation/log.cpp",
                "tests/engine/graph_tests.cpp",
            ],
        )

    def test_shard_outside_the_count_is_refused(self):
        with self.assertRaises(ValueError):
            tidy_shard.shard_sources(CONFIGURED, SHARD_COUNT, SHARD_COUNT)


class ArgumentTests(unittest.TestCase):
    def test_valid_arguments_are_parsed(self):
        self.assertEqual(
            tidy_shard.parse_arguments(["quick", "1", str(SHARD_COUNT), FINDINGS]),
            ("quick", 1, SHARD_COUNT, FINDINGS),
        )

    def test_wrong_argument_count_is_refused(self):
        with self.assertRaisesRegex(ValueError, "expected 4 arguments"):
            tidy_shard.parse_arguments(["quick", "1", str(SHARD_COUNT)])

    def test_non_numeric_shard_or_count_is_refused(self):
        with self.assertRaisesRegex(ValueError, "must be integers.*'one'"):
            tidy_shard.parse_arguments(["quick", "one", str(SHARD_COUNT), FINDINGS])
        with self.assertRaisesRegex(ValueError, "must be integers.*'eight'"):
            tidy_shard.parse_arguments(["quick", "1", "eight", FINDINGS])

    def test_shard_outside_the_count_is_refused(self):
        with self.assertRaisesRegex(ValueError, "outside"):
            tidy_shard.parse_arguments(["quick", str(SHARD_COUNT), str(SHARD_COUNT), FINDINGS])
        with self.assertRaisesRegex(ValueError, "outside"):
            tidy_shard.parse_arguments(["quick", "0", "0", FINDINGS])


class DatabaseTests(unittest.TestCase):
    def test_relative_file_is_resolved_against_its_directory(self):
        entries = [
            {"directory": BUILD, "file": "../../src/engine/graph.cpp"},
            {"directory": BUILD, "file": f"{ROOT}/src/foundation/log.cpp"},
        ]
        self.assertEqual(
            tidy_shard.database_sources(entries, ROOT),
            {"src/engine/graph.cpp", "src/foundation/log.cpp"},
        )


class FindingTests(unittest.TestCase):
    def test_path_in_the_tree_is_made_relative(self):
        self.assertEqual(
            tidy_shard.normalised_finding(f"{ROOT}/src/engine/graph.cpp{WARNING}\n", ROOT),
            f"src/engine/graph.cpp{WARNING}",
        )

    def test_parent_segments_fold_inside_the_tree(self):
        self.assertEqual(
            tidy_shard.normalised_finding(f"{BUILD}/../../src/a/../engine/graph.hpp{ERROR}", ROOT),
            f"src/engine/graph.hpp{ERROR}",
        )

    def test_leading_parent_segments_are_kept(self):
        self.assertEqual(
            tidy_shard.normalised_finding(f"../../x/graph.hpp{WARNING}", ROOT),
            f"../../x/graph.hpp{WARNING}",
        )
        self.assertEqual(
            tidy_shard.normalised_finding(f"a/../../x/graph.hpp{WARNING}", ROOT),
            f"../x/graph.hpp{WARNING}",
        )

    def test_path_outside_the_tree_stays_absolute(self):
        self.assertEqual(
            tidy_shard.normalised_finding(f"/usr/include/c++/v1/../vector{WARNING}", ROOT),
            f"/usr/include/c++/vector{WARNING}",
        )
        self.assertEqual(
            tidy_shard.normalised_finding(f"{ROOT}-other/src/graph.cpp{WARNING}", ROOT),
            f"{ROOT}-other/src/graph.cpp{WARNING}",
        )

    def test_other_lines_are_not_findings(self):
        for line in (
            f"{ROOT}/src/engine/graph.cpp:12:3: note: expanded from here",
            "12 warnings generated.",
            "Suppressed 40 warnings (40 in non-user code).",
        ):
            self.assertIsNone(tidy_shard.normalised_finding(line, ROOT))

    def test_repeated_findings_are_kept_once_in_order(self):
        lines = [
            f"{ROOT}/src/engine/graph.hpp{WARNING}\n",
            "1 warning generated.\n",
            f"{BUILD}/../../src/engine/graph.hpp{WARNING}\n",
            f"{ROOT}/src/engine/graph.cpp{ERROR}\n",
        ]
        self.assertEqual(
            tidy_shard.distinct_findings(lines, ROOT),
            [f"src/engine/graph.cpp{ERROR}", f"src/engine/graph.hpp{WARNING}"],
        )


if __name__ == "__main__":
    unittest.main()
