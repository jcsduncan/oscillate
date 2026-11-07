"""Unit tests for ctest_failures.py's failure summary."""

import unittest

import ctest_failures

REPORT = """<?xml version="1.0" encoding="UTF-8"?>
<testsuite name="Linux" tests="3" failures="2">
  <testcase name="engine_graph_contract" classname="engine_graph_contract" status="run">
    <system-out>ok</system-out>
  </testcase>
  <testcase name="native_halo_contract" classname="native_halo_contract" status="fail">
    <failure message="Timeout"/>
    <system-out>start
render
level 3 dB above the window | check</system-out>
  </testcase>
  <testcase name="quick_shell_smoke" classname="quick_shell_smoke" status="notrun">
    <skipped message="SKIP_RETURN_CODE=77"/>
  </testcase>
</testsuite>
"""


class FailureTests(unittest.TestCase):
    def test_failed_tests_are_listed_with_their_last_output_but_skipped_ones_are_not(self):
        self.assertEqual(
            ctest_failures.failures(REPORT),
            [
                (
                    "native_halo_contract",
                    "Timeout",
                    "start / render / level 3 dB above the window | check",
                ),
            ],
        )

    def test_summary_escapes_table_cells_and_names_the_shard(self):
        text = ctest_failures.summary({"shard-2": REPORT})
        self.assertIn("1 failing test(s)", text)
        self.assertIn(
            "| shard-2 | `native_halo_contract` | Timeout | "
            "start / render / level 3 dB above the window \\| check |",
            text,
        )

    def test_missing_report_is_listed(self):
        self.assertIn(
            "| shard-3 | `(no report)` | missing |", ctest_failures.summary({"shard-3": None})
        )

    def test_clean_reports_say_so(self):
        clean = REPORT.split('  <testcase name="native_halo', maxsplit=1)[0] + "</testsuite>\n"
        self.assertEqual(ctest_failures.summary({"shard-1": clean}), "No test failed.\n")


if __name__ == "__main__":
    unittest.main()
