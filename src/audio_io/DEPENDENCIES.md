# Audio I/O dependency decision

This first adapter dynamically links the Ubuntu 26.04 `libpipewire-0.3` shared
library and uses its SPA headers. CMake discovers the development package through
`pkg-config`; no PipeWire code or binary is copied into this repository. The
[PipeWire licence](https://github.com/PipeWire/pipewire/blob/master/LICENSE)
identifies its source as MIT, with exceptions for other plugins that this target
does not link. Packaging still needs a transitive dependency and licence inventory.

The [PipeWire stream API](https://docs.pipewire.org/group__pw__stream.html) marks
`pw_stream_dequeue_buffer`, `pw_stream_queue_buffer` and `pw_stream_get_time_n`
as real-time safe. Its
[stream guide](https://docs.pipewire.org/devel/page_streams.html) documents
dequeue/fill/queue in the process callback. The adapter uses the documented
`PW_STREAM_FLAG_RT_PROCESS` path, with preallocated mono scratch and bounded
stereo output. A buffer larger than 8192 frames is queued empty and counted.

Negotiated client format/rate comes from the stream's format event. Graph rate
is observed from `pw_time.rate` when it has a whole-sample-rate representation;
otherwise it remains unknown. The adapter does not infer whether conversion
occurred. The stream API has no reliable per-stream xrun counter, so status
reports xruns as unknown. These are acceptance limits, not a completed ER-05
qualification: automatic conversion and physical disconnect/reselect still need
reference-device tests, and JACK remains a separate adapter.

The adapter also observes the transient `SPA_IO_CLOCK_FLAG_XRUN_RECOVER` flag
from `SPA_IO_Position` once per process callback. It reports node recovery
notices only when that IO area was available for every callback in the stream
session. This is narrower than device xrun evidence; an observed zero never
fills the unknown `xruns` field. See the [node recovery observation](../../docs/performance/pipewire-node-recovery-observation-2026-10-19.md).

Both PipeWire and JACK expose a bounded callback timing distribution. Each
valid callback increments one of eight bins by its wall time as a fraction of
that block's nominal period: at most 25, 50, 75, 90, 100, 125, or 150%, then
above 150%. A control thread can subtract two snapshots with the same epoch,
sample rate and frame count to estimate recent p95/p99 bins and threshold
crossings. A rate or block-size change starts a new epoch. Sampling performs
two clock reads and fixed-size atomic bookkeeping on the callback, with no
allocation or lock; a snapshot may be temporarily unavailable during a
concurrent update. These bins measure process-callback wall time, including
adapter work, and cannot establish backend xruns or device underruns. JACK's
xrun notifications are a separate count. For JACK, `DeviceTelemetry::snapshot()`
reports that count as both `xruns` and `client_xrun_notifications`, and
`JackOutput::status()` as `xrun_notifications`; each starts from a known zero
and resets on each output start. PipeWire leaves both snapshot fields unknown;
its graph recoveries are `graph_xrun_recovery_events`. These JACK callbacks
report a server/client notification; they do not identify the affected output
port or prove a DAW render underrun.

`start` requests PipeWire node latency through `node.latency` in frames/rate;
this is a hint, not a guarantee of callback quantum. A local 48 kHz smoke run
observed 1024 frames before this request was wired, then 128 after requesting
128. Both target buffer sizes still require controlled qualification.

For a local server smoke, configure with
`-DOSCILLATE_AUDIO_IO_LIVE_TESTS=ON` and run the `audio_io_pipewire_live`
CTest case. It checks both 64- and 128-frame requests, independently observed
client/graph rates, callback size, and clean stop under a bounded deadline.
This test is opt-in because its result depends on a running PipeWire server and
host policy. It does not replace disconnect, conversion or xrun qualification.
On the local PipeWire 1.6.2 server, the live smoke covered 128/162 (79%)
executable lines in the device-specific adapter. The remaining branches are
mostly server failure, malformed buffer and renegotiation paths; they require
fault injection or device tests. The deterministic stereo/telemetry seam is
fully covered. The broad default coverage gate needs a narrow device-glue
exception until those tests exist; no other production file needs exclusion.

## Stop and callback lifetime

`PipeWireOutput::stop()` is synchronous. It sets a lock-free stop request before
acquiring PipeWire's thread-loop lock, so later callbacks that observe it queue
silence without invoking the renderer or advancing transport. A callback already
past that check may still finish one render. The control thread then deactivates,
disconnects and destroys the stream under the loop lock, unlocks, and calls
`pw_thread_loop_stop()` before freeing the loop and callback-owned state. The
[PipeWire thread-loop contract](https://docs.pipewire.org/page_thread_loop.html)
requires `pw_thread_loop_stop()` without the lock held and says its return means
the loop thread has stopped; it states no maximum duration. PipeWire offers no
timed loop-lock acquisition or timed stop/join. The stream's real-time process
callback runs on a separate data thread, and in-process renderers may themselves
take unbounded time. An atomic request cannot preempt an in-flight renderer.

Consequently, stop has **no hard deadline**. Callers must not free or reuse the
render source, transport, graph, rings or output object until stop returns.
Detaching a stalled teardown thread or cancelling the PipeWire data thread would
violate that lifetime contract. UI callers should schedule shutdown off the UI
thread, show a stopping/stalled state if it takes too long, and retain live state
until join completes; a persistently hung in-process renderer may require process
restart. This adapter does not yet provide a nonblocking shutdown handle or a
watchdog escalation policy. Ordinary CTest uses a fake renderer to verify that
a requested stop suppresses subsequent work and holds the transport boundary;
the opt-in live smoke checks a clean join under normal server conditions. These
tests do not prove a worst-case stop time.

## Direct ALSA and card reservation

The direct ALSA output ([ADR-0130](../../docs/decisions/0130-direct-alsa-backend.md))
dynamically links the Ubuntu 26.04 `libasound2t64` library (alsa-lib,
LGPL-2.1-or-later), found through `pkg-config` (`alsa`, from
`libasound2-dev`). PipeWire's client library does not link it, so it is a new
dependency. The `.deb` depends on it and the AppImage bundles it, like the
PipeWire and JACK client libraries. The I/O thread uses `snd_pcm_wait`, `snd_pcm_avail_update`,
`snd_pcm_delay`, `snd_pcm_writei` and `snd_pcm_recover`; on a hardware PCM
these are system calls on preallocated buffers. Device enumeration reads the
control interface only (`snd_ctl_pcm_info`), never opening a PCM.

Card reservation speaks the freedesktop device reservation protocol
(`org.freedesktop.ReserveDevice1`) through `libdbus-1-3` (AFL-2.1 or
GPL-2.0-or-later), found through `pkg-config` (`dbus-1`, from
`libdbus-1-dev`). The library is already loaded by PipeWire's client; the
development package is new. Reservation runs on control threads only.

The `alsa_output_contract` cases drive the output against ALSA's `null` and
`file` PCMs, which need only `libasound2-data`'s `alsa.conf`, so they run in
CI without a sound card. A real card, the sound server's release and
reacquisition of it, and the real-time grant need a manual check on hardware.
