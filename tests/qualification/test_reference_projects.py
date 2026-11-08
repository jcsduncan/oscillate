"""Evidence contract for the reference-project runner."""

import unittest

from qualify_reference_projects import RunFailed, markdown, percentile, summarise, trace_tables

WORKLOAD = (
    "workload reference=heavy frames=64 measured_seconds=20 warmup_blocks=100 audio_tracks=16 "
    "native_synths=15 native_inserts=62 vst3_effects=1 vst3_instruments=1 notes=900 master=4 "
    "groups=2 returns=2"
)
FAKE = (
    "result mode=fake frames=64 blocks=15000 mean_us=200 p50_us=180 p99_us=400 max_us=900 "
    "over_period=0 period_us=1333.333 render_failures=0 missing_frames=0 worker_failed=0 "
    "audible_blocks=15100 callback_allocations=0 process_cpu_percent=85.5 cpu_window_p50=80 "
    "cpu_window_p99=120 cpu_window_max=130 cpu_windows=200 max_rss_kib=409600 cores=16"
)
LIVE = (
    "result mode=live frames=128 observed_frames=128 graph_rate=48000 completed_frames=969600 "
    "target_frames=969600 render_failures=0 missing_frames=0 worker_failed=0 callback_blocks=7575 "
    "over_period=0 max_us=1200 xruns=unavailable period_us=2666.667 process_cpu_percent=90 "
    "cpu_window_p50=88 cpu_window_p99=110 cpu_window_max=115 cpu_windows=200 max_rss_kib=1024 "
    "cores=16"
)


def span(tid, kind, ts, dur, n=1, name=None):
    return {
        "ph": "X",
        "tid": tid,
        "name": name or kind,
        "ts": ts,
        "dur": dur,
        "args": {"kind": kind, "n": n},
    }


TRACE = {
    "traceEvents": [
        {"ph": "M", "tid": 1, "args": {"name": "audio callback"}},
        {"ph": "M", "tid": 2, "args": {"name": "audio worker"}},
        {"ph": "M", "tid": 3, "args": {"name": "ui"}},
        span(1, "audio.render", 0, 400),
        span(1, "audio.render", 1000, 600),
        span(1, "Nova", 0, 300, n=1, name="Lead"),
        span(2, "Nova", 1000, 500, n=2, name="Pad"),
        span(2, "Shape", 1500, 100),
        span(3, "Nova", 0, 9999),
        {"ph": "i", "tid": 1, "name": "audio.xrun", "ts": 5, "args": {"n": 0}},
    ]
}


class Summary(unittest.TestCase):
    def test_fake_runs_report_period_headroom(self):
        summary = summarise(WORKLOAD + "\n" + FAKE + "\n", None)
        self.assertEqual(summary["result"]["p99_period_percent"], 30.0)
        self.assertEqual(summary["workload"]["native_synths"], 15)
        self.assertEqual(summary["result"]["process_cpu_percent"], 85.5)

    def test_live_runs_take_callback_timing_from_the_trace(self):
        summary = summarise(WORKLOAD + "\n" + LIVE + "\n", TRACE)
        self.assertEqual(summary["result"]["p99_us"], 600)
        self.assertEqual(summary["result"]["xruns"], "unavailable")
        self.assertEqual(summary["trace"]["xrun_events"], 1)

    def test_needs_exactly_one_result(self):
        with self.assertRaises(RunFailed):
            summarise(WORKLOAD + "\n", None)


class Devices(unittest.TestCase):
    def test_devices_by_kind_on_audio_threads_only(self):
        tables = trace_tables(TRACE)
        nova = tables["devices"][0]
        self.assertEqual(nova["device"], "Nova")
        self.assertEqual(nova["instances"], 2)
        self.assertEqual(nova["core_percent"], 50.0)
        self.assertEqual(nova["max_us"], 500)
        self.assertNotIn("audio.render", [row["device"] for row in tables["devices"]])
        self.assertEqual(tables["callbacks"]["blocks"], 2)

    def test_percentile_takes_the_ceiling_rank(self):
        self.assertEqual(percentile([1, 2, 3, 4], 50), 2)
        self.assertEqual(percentile([1, 2, 3, 4], 99), 4)
        self.assertEqual(percentile([], 99), 0.0)


class Report(unittest.TestCase):
    def test_one_row_per_run_and_a_device_table(self):
        run = {
            "reference": "heavy",
            "frames": 64,
            "repetition": 1,
            **summarise(WORKLOAD + "\n" + FAKE, TRACE),
        }
        text = markdown({"host": "h", "cores": 16, "mode": "fake", "seconds": 20, "runs": [run]})
        self.assertIn("| heavy | 64 | 1 | 180 | 400 | 900 | 30.0% | 0 | 85.5 | 120 | 400.0 |", text)
        self.assertIn("| Nova | 2 | 50.0 |", text)


if __name__ == "__main__":
    unittest.main()
