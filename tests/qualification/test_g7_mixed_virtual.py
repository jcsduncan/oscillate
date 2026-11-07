"""Only the evidence contract; the live run is deliberately opt-in."""

import unittest

from qualify_g7_mixed_virtual import parse_run


class EvidenceContract(unittest.TestCase):
    def test_accepts_virtual_run_without_claiming_backend_xruns(self):
        output = (
            "live_result requested_frames=64 observed_frames=64 client_rate=48000 "
            "graph_rate=48000 target_frames=134400 completed_frames=134464 "
            "vst3_insert=1 vst3_instrument=1 render_failures=0 missing_frames=0 "
            "worker_failed=0 vst3_fault=0 oversized_blocks=0 invalid_buffers=0 "
            "callback_over_period_blocks=0 "
            "measured_callback_blocks=2101 max_callback_duration_ns=300000 "
            "xruns=unavailable\n"
            "monitor_frames=150000 monitor_peak=0.8900 "
            "active_20ms_windows=140 disposable_sink_cleaned=true\n"
        )
        result = parse_run(output, 64, 2000, 100)
        self.assertIsNone(result["backend_xruns"])
        self.assertTrue(result["null_sink_cleaned"])

    def test_rejects_missing_cleanup_or_incomplete_transport(self):
        output = (
            "live_result requested_frames=64 observed_frames=64 client_rate=48000 "
            "graph_rate=48000 target_frames=134400 completed_frames=130000 "
            "vst3_insert=1 vst3_instrument=1 render_failures=0 missing_frames=0 "
            "worker_failed=0 vst3_fault=0 oversized_blocks=0 invalid_buffers=0 "
            "callback_over_period_blocks=0 "
            "measured_callback_blocks=2101 max_callback_duration_ns=300000 "
            "xruns=unavailable\n"
            "monitor_frames=150000 monitor_peak=0.8900 "
            "active_20ms_windows=140 disposable_sink_cleaned=true\n"
        )
        with self.assertRaisesRegex(RuntimeError, "transport stopped"):
            parse_run(output, 64, 2000, 100)
        with self.assertRaisesRegex(RuntimeError, "cleaned-monitor"):
            parse_run(output.split("monitor_frames", maxsplit=1)[0], 64, 2000, 100)

    def test_rejects_processing_overrun(self):
        output = (
            "live_result requested_frames=64 observed_frames=64 client_rate=48000 "
            "graph_rate=48000 target_frames=134400 completed_frames=134464 "
            "vst3_insert=1 vst3_instrument=1 render_failures=0 missing_frames=0 "
            "worker_failed=0 vst3_fault=0 oversized_blocks=0 invalid_buffers=0 "
            "callback_over_period_blocks=1 measured_callback_blocks=2101 "
            "max_callback_duration_ns=2000000 xruns=unavailable\n"
            "monitor_frames=150000 monitor_peak=0.8900 "
            "active_20ms_windows=140 disposable_sink_cleaned=true\n"
        )
        with self.assertRaisesRegex(RuntimeError, "callback_over_period_blocks"):
            parse_run(output, 64, 2000, 100)


if __name__ == "__main__":
    unittest.main()
