"""Unit tests for tidy_shard.py's split of the translation units."""

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


if __name__ == "__main__":
    unittest.main()
