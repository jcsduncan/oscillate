"""Focused evidence contract for the opt-in fake-callback CPU rehearsal."""

import unittest

from qualify_mixed_cpu import parse_result

OUTPUT = """vst3_probe wet_energy=2 dry_energy=1 ratio=2
vst3_instrument_probe silent_audio_input_energy=632
send_automation_probe start_energy=1 next_bar_energy=1.3 ratio=1.33
workload=managed-wav-32-routed rate=48000 frames=64 source_tracks=32 group_buses=2 return_buses=2 sends=33 master=eq+compressor+reverb+limiter vst3_inserts=1 vst3_instruments=1 native_instruments=0 automated_sends=1 processed_clips=0 repitch_clips=0 pitched_clips=0 warmup_blocks=100 measured_blocks=2000 repetitions=1 period_us=1333.333 pace_multiplier=1
run=1 mean_us=100 p50_us=90 p95_us=130 p99_us=180 max_us=250 over_period=0 callback_allocations=0 render_failures=0 missing_frames=0 worker_failed=0 audible_blocks=2000 synth_voice_blocks=0 processed_clips=0 reduced_quality_clips=0 quality_step_downs=0 process_cpu_us=100000 wall_us=3000000 vst3_fault=0
"""  # noqa: E501 - verbatim tool output


class MixedCpuEvidence(unittest.TestCase):
    def test_reports_period_headroom_for_verified_mixed_graph(self):
        result = parse_result(OUTPUT, 64, 2000, 100, 1)
        self.assertEqual(result[0]["p99_period_percent"], 13.5)
        self.assertEqual(result[0]["worst_period_percent"], 18.75)
        self.assertEqual(result[0]["process_cpu_percent"], 3.33)

    def test_rejects_fault_or_missing_plugin_oracle(self):
        with self.assertRaisesRegex(RuntimeError, "callback or audible-output"):
            parse_result(
                OUTPUT.replace("callback_allocations=0", "callback_allocations=1"), 64, 2000, 100, 1
            )
        with self.assertRaisesRegex(RuntimeError, "instrument audio oracle"):
            parse_result(OUTPUT.replace("vst3_instrument_probe", "missing_probe"), 64, 2000, 100, 1)

    def test_rejects_wrong_workload_and_retains_over_period_outliers(self):
        with self.assertRaisesRegex(RuntimeError, "wrong workload"):
            parse_result(OUTPUT.replace("source_tracks=32", "source_tracks=31"), 64, 2000, 100, 1)
        observed = parse_result(
            OUTPUT.replace("max_us=250", "max_us=1500").replace("over_period=0", "over_period=1"),
            64,
            2000,
            100,
            1,
        )
        self.assertEqual(observed[0]["over_period"], 1)
        self.assertGreater(observed[0]["worst_period_percent"], 100)

    def test_processed_variant_requires_saved_workload_and_full_quality(self):
        processed = OUTPUT.replace(
            "processed_clips=0 repitch_clips=0 pitched_clips=0",
            "processed_clips=4 repitch_clips=2 pitched_clips=2",
        ).replace(
            "synth_voice_blocks=0 processed_clips=0", "synth_voice_blocks=0 processed_clips=4"
        )
        result = parse_result(processed, 64, 2000, 100, 1, 4)
        self.assertEqual(result[0]["processed_clips"], 4)
        with self.assertRaisesRegex(RuntimeError, "wrong workload"):
            parse_result(OUTPUT, 64, 2000, 100, 1, 4)
        with self.assertRaisesRegex(RuntimeError, "callback or audible-output"):
            parse_result(
                processed.replace("quality_step_downs=0", "quality_step_downs=1"),
                64,
                2000,
                100,
                1,
                4,
            )

    def test_native_synth_variant_requires_both_tracks_and_active_voices(self):
        native = (
            OUTPUT.replace("workload=managed-wav-32-routed", "workload=native-synth-32-routed")
            .replace("source_tracks=32", "source_tracks=30")
            .replace("sends=33", "sends=32")
            .replace("vst3_instruments=1", "vst3_instruments=0")
            .replace("native_instruments=0", "native_instruments=2")
            .replace("synth_voice_blocks=0", "synth_voice_blocks=150")
        )
        self.assertEqual(
            parse_result(native, 64, 2000, 100, 1, native_synths=True)[0]["synth_voice_blocks"], 150
        )
        with self.assertRaisesRegex(RuntimeError, "wrong workload"):
            parse_result(OUTPUT, 64, 2000, 100, 1, native_synths=True)
        with self.assertRaisesRegex(RuntimeError, "callback or audible-output"):
            parse_result(
                native.replace("synth_voice_blocks=150", "synth_voice_blocks=0"),
                64,
                2000,
                100,
                1,
                native_synths=True,
            )


if __name__ == "__main__":
    unittest.main()
