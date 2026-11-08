"""Unit tests for ctest_shards.py's selection and balancing."""

import unittest

import ctest_shards

BUDGETS = """# comment
set(OSCILLATE_TEST_BUDGET_slow_render 90)
set(OSCILLATE_TEST_BUDGET_clay_dsp.piano.c_open_held 30)
set(OSCILLATE_TEST_BUDGET_medium 12)
"""
SLOW_RENDER_SECONDS = 30.0
PIANO_SECONDS = 10.0
MEDIUM_SECONDS = 4.0
QUARANTINE_ISSUE = 227
HALO_ISSUE = 12


def ctest_test(name, *labels):
    return {"name": name, "properties": [{"name": "LABELS", "value": list(labels)}]}


class BudgetTests(unittest.TestCase):
    def test_measured_time_is_a_third_of_the_budget(self):
        self.assertEqual(
            ctest_shards.measured_seconds(BUDGETS),
            {
                "slow_render": SLOW_RENDER_SECONDS,
                "clay_dsp.piano.c_open_held": PIANO_SECONDS,
                "medium": MEDIUM_SECONDS,
            },
        )


class SelectionTests(unittest.TestCase):
    def test_live_and_parity_tests_are_left_out(self):
        listing = {
            "tests": [
                ctest_test("engine_graph_contract", "fast", "engine"),
                ctest_test("audio_io_pipewire_live", "live", "audio_io"),
                ctest_test("application_synth_export_parity", "slow", "application", "parity"),
                ctest_test("unlabelled"),
            ]
        }
        self.assertEqual(
            ctest_shards.selected_tests(listing), ["engine_graph_contract", "unlabelled"]
        )


class QuarantineTests(unittest.TestCase):
    def test_entries_map_tests_to_their_issue(self):
        text = "# comment\n\nquick_shell_smoke 227\nnative_halo_contract   12\n"
        self.assertEqual(
            ctest_shards.quarantined(text),
            {"quick_shell_smoke": QUARANTINE_ISSUE, "native_halo_contract": HALO_ISSUE},
        )

    def test_an_entry_without_an_issue_is_refused(self):
        with self.assertRaises(ValueError):
            ctest_shards.quarantined("quick_shell_smoke\n")

    def test_quarantined_tests_are_left_out(self):
        listing = {"tests": [ctest_test("quick_shell_smoke", "slow"), ctest_test("kept", "fast")]}
        self.assertEqual(
            ctest_shards.selected_tests(listing, {"quick_shell_smoke": QUARANTINE_ISSUE}),
            ["kept"],
        )

    def test_the_checked_in_list_parses(self):
        ctest_shards.quarantined(ctest_shards.QUARANTINE.read_text(encoding="utf-8"))


class SplitTests(unittest.TestCase):
    def test_every_test_lands_in_exactly_one_shard(self):
        names = ["slow_render", "medium", "a", "b", "c"]
        shards, _ = ctest_shards.split(names, ctest_shards.measured_seconds(BUDGETS), 2)
        self.assertEqual(sorted(name for shard in shards for name in shard), sorted(names))

    def test_heavy_tests_are_spread_across_shards(self):
        seconds = {"heavy_one": 20.0, "heavy_two": 20.0, "light": 1.0}
        shards, loads = ctest_shards.split(["light", "heavy_one", "heavy_two"], seconds, 2)
        self.assertEqual(shards, [["heavy_one", "light"], ["heavy_two"]])
        self.assertEqual(loads, [21.0, 20.0])

    def test_unlisted_tests_weigh_the_fast_default(self):
        _, loads = ctest_shards.split(["a", "b"], {}, 1)
        self.assertEqual(loads, [2 * ctest_shards.UNLISTED_SECONDS])

    def test_zero_shards_are_refused(self):
        with self.assertRaises(ValueError):
            ctest_shards.split(["a"], {}, 0)


if __name__ == "__main__":
    unittest.main()
