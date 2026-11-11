"""Evidence contract for the reference-project runner."""

import argparse
import unittest

from qualify_reference_projects import (
    RunFailed,
    gate,
    markdown,
    percentile,
    summarise,
    trace_tables,
)

WORKLOAD = (
    "workload reference=heavy frames=64 measured_seconds=20 warmup_blocks=100 audio_tracks=16 "
    "native_synths=15 native_inserts=62 vst3_effects=1 vst3_instruments=1 notes=900 master=4 "
    "groups=2 returns=2"
)
FAKE = (
    "result mode=fake frames=64 blocks=15000 mean_us=200 p50_us=180 p99_us=400 max_us=900 "
    "over_period=0 workers=3 period_us=1333.333 render_failures=0 missing_frames=0 worker_failed=0 "
    "audible_blocks=15100 callback_allocations=0 process_cpu_percent=85.5 cpu_window_p50=80 "
    "cpu_window_p99=120 cpu_window_max=130 cpu_windows=200 max_rss_kib=409600 cores=16"
)
LIVE = (
    "result mode=live frames=128 observed_frames=128 graph_rate=48000 completed_frames=969600 "
    "target_frames=969600 render_failures=0 missing_frames=0 worker_failed=0 callback_blocks=7575 "
    "over_period=0 workers=3 max_us=1200 xruns=unavailable period_us=2666.667 "
    "process_cpu_percent=90 "
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
        text = markdown(
            {
                "host": "h",
                "cores": 16,
                "mode": "fake",
                "master": "default",
                "seconds": 20,
                "runs": [run],
            }
        )
        self.assertIn(
            "| heavy | 64 | 1 | 3 | 180 | 400 | 900 | 30.0% | 0 |  |  |  |  |  "
            "| 85.5 | 120 | 400.0 |",
            text,
        )
        self.assertIn("| Nova | 2 | 50.0 |", text)


# The master node on the callback, once a block (ADR-0125).
MASTER_TRACE = {
    "traceEvents": TRACE["traceEvents"]
    + [
        span(1, "graph.node", 0, 100, n=7, name="Master"),
        span(1, "graph.node", 1000, 300, n=7, name="Master"),
        span(2, "graph.node", 1000, 900, n=3, name="Group A"),
    ]
}
MASTER_MEAN_US = 200
MASTER_PERIOD_PERCENT = 15.0


def run_with(trace) -> dict:
    summary = summarise(WORKLOAD + "\n" + FAKE, trace)
    return {"reference": "light", "frames": 64, "repetition": 1, **summary}


def limits(master=None, p99=None, exposed=None, late=None):
    return argparse.Namespace(
        gate_master_percent=master,
        gate_p99_percent=p99,
        gate_exposed_percent=exposed,
        gate_late_blocks=late,
    )


# A block in parts (ADR-0127): the master's two spans add up, and only the
# part after every other node has finished is exposed.
PARTS_TRACE = {
    "traceEvents": [
        {"ph": "M", "tid": 1, "args": {"name": "audio callback"}},
        {"ph": "M", "tid": 2, "args": {"name": "audio worker"}},
        span(1, "audio.render", 0, 1000),
        span(1, "graph.wavefront", 0, 1000, n=4),
        span(2, "graph.source", 0, 300, n=1, name="Track 1 (source)"),
        span(2, "graph.node", 300, 400, n=1, name="Track 1"),
        span(1, "graph.node", 500, 300, n=7, name="Master"),
        span(1, "graph.node", 800, 150, n=7, name="Master"),
        span(1, "audio.render", 2000, 500),
        span(1, "graph.node", 2000, 100, n=1, name="Track 1"),
        span(1, "graph.node", 2100, 200, n=7, name="Master"),
    ]
}
PARTS_MASTER_MEAN_US = 325
# Block one: the master ends at 950, the track at 700; block two: 2300 and 2100.
PARTS_EXPOSED_MEAN_US = 225
# 225 µs of the fake run's 1333.333 µs period.
PARTS_EXPOSED_PERCENT = 16.88
# One block in four parts, one whole.
PARTS_MEAN = 2.5
# The master after the warm-up mark.
WARMED_MASTER_US = 100


class Master(unittest.TestCase):
    def test_the_master_node_is_timed_apart_from_devices(self):
        tables = trace_tables(MASTER_TRACE)
        self.assertEqual(tables["master"]["blocks"], 2)
        self.assertEqual(tables["master"]["mean_us"], MASTER_MEAN_US)
        self.assertNotIn("graph.node", [row["device"] for row in tables["devices"]])
        result = run_with(MASTER_TRACE)["result"]
        self.assertEqual(result["master_period_percent"], MASTER_PERIOD_PERCENT)

    def test_the_gate_names_each_run_over_a_limit(self):
        run = run_with(MASTER_TRACE)
        self.assertEqual(gate(limits(master=MASTER_PERIOD_PERCENT, p99=30.0), [run]), [])
        failures = gate(limits(master=10.0, p99=20.0), [run])
        self.assertEqual(len(failures), 2)
        self.assertIn("master takes 15.0% of the period", failures[0])
        self.assertIn("p99 is 30.0% of the period", failures[1])

    def test_a_p99_limit_needs_a_p99(self):
        run = run_with(MASTER_TRACE)
        del run["result"]["p99_period_percent"]
        self.assertIn("no callback p99", gate(limits(p99=30.0), [run])[0])

    def test_a_master_limit_needs_a_trace(self):
        self.assertIn("no master spans", gate(limits(master=10.0), [run_with(None)])[0])

    def test_a_master_in_parts_sums_its_spans_and_times_its_exposed_tail(self):
        tables = trace_tables(PARTS_TRACE)
        self.assertEqual(tables["master"]["mean_us"], PARTS_MASTER_MEAN_US)
        self.assertEqual(tables["master"]["exposed_mean_us"], PARTS_EXPOSED_MEAN_US)
        self.assertEqual(tables["mean_parts"], PARTS_MEAN)
        self.assertNotIn("graph.wavefront", [row["device"] for row in tables["devices"]])

    def test_warm_up_blocks_are_left_out(self):
        warmed = {
            "traceEvents": [
                {"ph": "M", "tid": 1, "args": {"name": "audio callback"}},
                span(1, "audio.render", 0, 900),
                span(1, "graph.node", 0, 800, n=7, name="Master"),
                {"ph": "i", "tid": 1, "name": "qualification.measured", "ts": 1000, "args": {}},
                span(1, "audio.render", 1000, 200),
                span(1, "graph.node", 1000, 100, n=7, name="Master"),
            ]
        }
        tables = trace_tables(warmed)
        self.assertEqual(tables["master"]["blocks"], 1)
        self.assertEqual(tables["master"]["mean_us"], WARMED_MASTER_US)

    def test_a_trace_that_dropped_events_fails_the_master_gates(self):
        dropped = dict(PARTS_TRACE, otherData={"dropped_events": 3})
        run = run_with(dropped)
        failures = gate(limits(exposed=PARTS_EXPOSED_PERCENT), [run])
        self.assertIn("dropped 3 events", failures[0])

    def test_the_exposed_and_late_limits(self):
        run = run_with(PARTS_TRACE)
        exposed = run["result"]["master_exposed_percent"]
        self.assertEqual(exposed, PARTS_EXPOSED_PERCENT)
        self.assertEqual(gate(limits(exposed=exposed, late=0), [run]), [])
        failures = gate(limits(exposed=exposed / 2), [run])
        self.assertIn("runs alone", failures[0])
        run["result"]["over_period"] = 3
        self.assertIn("3 blocks passed the period", gate(limits(late=2), [run])[0])


if __name__ == "__main__":
    unittest.main()
