"""Small parser and gate checks for the opt-in physical output runner."""

import subprocess
import sys
import unittest
from unittest.mock import patch

from qualify_physical_streaming_32track import (
    checked_result,
    player_node,
    pw_top_errors,
    read_ready,
    workload_plan,
)


class PhysicalQualificationTests(unittest.TestCase):
    def test_long_workload_plan_bounds_sources_and_preserves_short_default(self) -> None:
        self.assertEqual(workload_plan(64, None), (4_500, 8))
        self.assertEqual(workload_plan(128, None), (2_200, 8))
        for frames in (64, 128):
            blocks, source_seconds = workload_plan(frames, 60)
            self.assertGreaterEqual(blocks * frames, 60 * 48_000)
            self.assertGreaterEqual(source_seconds * 48_000, (blocks + 100) * frames)
            self.assertLess(source_seconds, 120)
            self.assertLessEqual(workload_plan(frames, 90)[1], 120)
        for seconds in (0, 59, 91, 1_000_000):
            with self.assertRaisesRegex(ValueError, "60 to 90"):
                workload_plan(64, seconds)
        with self.assertRaisesRegex(ValueError, "frame size"):
            workload_plan(256, 60)

    def test_player_node_follows_link_to_process_client(self) -> None:
        state = [
            {
                "type": "PipeWire:Interface:Client",
                "id": 21,
                "info": {"props": {"application.process.id": 456}},
            },
            {"type": "PipeWire:Interface:Node", "id": 31, "info": {"props": {"client.id": 21}}},
            {
                "type": "PipeWire:Interface:Link",
                "info": {"input-node-id": 30, "output-node-id": 31, "state": "active"},
            },
        ]
        with patch("qualify_physical_streaming_32track.objects", return_value=state):
            self.assertEqual(player_node(30, 456)["id"], 31)
            with self.assertRaisesRegex(RuntimeError, "one linked"):
                player_node(30, 999)

    def test_ready_line_after_buffered_probe_lines(self) -> None:
        player = subprocess.Popen(
            [
                sys.executable,
                "-c",
                "import sys; sys.stdout.write('probe=a\\nprobe=b\\nlive_ready run=1\\n'); "
                "sys.stdout.flush()",
            ],
            stdout=subprocess.PIPE,
            text=True,
        )
        try:
            self.assertIn("live_ready run=1", read_ready(player, 2))
            self.assertEqual(player.wait(timeout=2), 0)
        finally:
            if player.poll() is None:
                player.kill()
                player.wait()
            if player.stdout is not None:
                player.stdout.close()

    def test_pw_top_uses_last_complete_sample_and_err_column(self) -> None:
        output = (
            "S   ID  QUANT RATE WAIT BUSY W/Q B/Q ERR FORMAT NAME\n"
            "C 45 64 48000 --- --- --- --- 7 F32LE sink\n"
            "C 91 64 48000 --- --- --- --- 4 F32LE player\n"
            "S   ID  QUANT RATE WAIT BUSY W/Q B/Q ERR FORMAT NAME\n"
            "S 45 64 48000 1.0 2.0 0.1 0.2 8 F32LE sink\n"
            "S 91 64 48000 1.0 2.0 0.1 0.2 5 F32LE player\n"
        )
        self.assertEqual(pw_top_errors(output, {45, 91}), {45: 8, 91: 5})

    def test_pw_top_rejects_missing_node(self) -> None:
        output = (
            "S   ID  QUANT RATE WAIT BUSY W/Q B/Q ERR FORMAT NAME\n"
            "S 45 64 48000 --- --- --- --- 0 F32LE sink\n"
        )
        with self.assertRaisesRegex(RuntimeError, "omitted a complete sample"):
            pw_top_errors(output, {45, 91})

    def test_result_separates_callback_budget_from_backend_xruns(self) -> None:
        fields = {
            "requested_frames": 64,
            "observed_frames": 64,
            "source_seconds": 8,
            "client_rate": 48000,
            "graph_rate": 48000,
            "target_frames": 294400,
            "completed_frames": 294464,
            "render_failures": 0,
            "missing_frames": 0,
            "worker_failed": 0,
            "oversized_blocks": 0,
            "invalid_buffers": 0,
            "vst3_insert": 1,
            "vst3_instrument": 1,
            "vst3_fault": 0,
            "measured_callback_blocks": 4600,
            "max_callback_duration_ns": 200000,
            "callback_over_period_blocks": 0,
            "graph_xrun_recovery_events": 0,
            "xruns": "unavailable",
        }
        result = "live_result " + " ".join(f"{key}={value}" for key, value in fields.items())
        checked = checked_result(result, 64, 4500, 100)
        self.assertEqual(checked["xruns"], "unavailable")
        self.assertEqual(checked["graph_xrun_recovery_events"], "0")
        self.assertEqual(
            checked_result(
                result.replace(
                    "graph_xrun_recovery_events=0", "graph_xrun_recovery_events=unavailable"
                ),
                64,
                4500,
                100,
            )["graph_xrun_recovery_events"],
            "unavailable",
        )
        with self.assertRaisesRegex(RuntimeError, "node xrun recovery"):
            checked_result(
                result.replace("graph_xrun_recovery_events=0", "graph_xrun_recovery_events=1"),
                64,
                4500,
                100,
            )
        with self.assertRaisesRegex(RuntimeError, "recovery evidence"):
            checked_result(
                result.replace("graph_xrun_recovery_events=0", "graph_xrun_recovery_events=bad"),
                64,
                4500,
                100,
            )
        with self.assertRaisesRegex(RuntimeError, "recovery evidence"):
            checked_result(result.replace("graph_xrun_recovery_events=0 ", ""), 64, 4500, 100)
        with self.assertRaisesRegex(RuntimeError, "callback exceeded"):
            checked_result(
                result.replace("callback_over_period_blocks=0", "callback_over_period_blocks=1"),
                64,
                4500,
                100,
            )
        with self.assertRaisesRegex(RuntimeError, "stream xrun"):
            checked_result(result.replace("xruns=unavailable", "xruns=1"), 64, 4500, 100)
        with self.assertRaisesRegex(RuntimeError, "source_seconds"):
            checked_result(result, 64, 4500, 100, 61)
        long_result = (
            result.replace("source_seconds=8", "source_seconds=61")
            .replace("target_frames=294400", "target_frames=2886400")
            .replace("completed_frames=294464", "completed_frames=2886464")
            .replace("measured_callback_blocks=4600", "measured_callback_blocks=45100")
        )
        self.assertEqual(
            checked_result(long_result, 64, 45000, 100, 61)["target_frames"], "2886400"
        )


if __name__ == "__main__":
    unittest.main()
