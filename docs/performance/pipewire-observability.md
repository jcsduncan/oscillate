# PipeWire playback observability

The output adapter reports three different kinds of evidence. They must not be
combined into one underrun count.

| Observation | Meaning | Source |
| --- | --- | --- |
| `missing_frames`, render failures | Managed-source starvation or a failed application render | Routed source and output adapter |
| `measured_callback_blocks`, `callback_over_period_blocks`, `max_callback_duration_ns` | Wall time from entry through buffer queueing for a valid PipeWire process callback; over-period means the measured duration exceeds that block's `frames / project_rate` period | `steady_clock` samples inside `PipeWireOutput::on_process` |
| `xruns` | Backend xrun count attributable to this stream | Unavailable for this PipeWire adapter; remains `std::nullopt` |

Timing includes callbacks that emit intentional silence before the playback
permission opens. It omits invalid or oversized buffers, which have their own
counters. It measures the callback body, not the scheduler wait before entry,
device delivery after exit or the graph's complete cycle. A callback that takes
longer than one nominal period is a processing-budget warning. A shorter
callback does **not** prove that its graph cycle met the deadline, and a longer
one does **not** identify a device xrun. The opt-in live runner prints these
values separately and checks that timing observations actually exist; it does
not turn a zero over-period count into a zero-xrun claim.

The [PipeWire stream event API](https://docs.pipewire.org/structpw__stream__events.html)
offers process, state and buffer events but no xrun event. The
[stream time API](https://docs.pipewire.org/structpw__time.html) says driver
ticks can reveal timeline discontinuities caused by xruns, but a discontinuity
is not an exact per-stream event count. The
[SPA clock](https://docs.pipewire.org/structspa__io__clock.html) has an
*estimated accumulated xrun duration* for the graph driver; its position
clock is the driver's, not this output stream's own loss counter. The
[node recovery flag](https://docs.pipewire.org/devel/group__spa__node.html)
indicates that a node's previous process missed its deadline when delivered to
that node; it is not a durable history and does not count missed callbacks that
never ran. The [`pw-top` ERR column](https://docs.pipewire.org/page_man_pw-top_1.html)
totals node or driver xruns **and errors**, so it is useful as separate,
externally sampled qualification evidence, not a pure stream-underflow count.

For G7, capture `pw-top` node and driver ERR deltas over a controlled
physical-device run, with stable node identity and complete start/end samples.
Keep application source starvation, callback timing, PipeWire node/driver ERR,
and any device-specific xrun log separate. The disposable null-sink runner
tests routing and callback operation but cannot qualify physical-device
underruns.

## Local check, 15 October 2026

On an Intel i7-10700K (16 logical CPUs), Ubuntu kernel 7.0.0-38 and GCC 15.2
Release build, the disposable PipeWire 1.6.2 null-sink runner completed one
32-track run at each requested 48 kHz buffer size. The 64-frame run observed
2,138 valid callbacks, zero callbacks over their nominal period and a maximum
callback body of 237,986 ns. The 128-frame run observed 2,134, zero and
316,532 ns respectively. Both had zero reported source/render/buffer failures
and finite audible monitor captures. Background load was uncontrolled and the
sink was virtual; these values are a diagnostic smoke check, not G7 release
performance evidence. The focused telemetry contract passed under GCC, Clang,
ASan/UBSan and TSan; coverage of `telemetry.cpp` was 100/100 executable lines.
