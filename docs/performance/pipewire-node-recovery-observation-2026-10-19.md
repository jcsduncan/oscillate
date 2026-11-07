# PipeWire node recovery observation, 19 October 2026

The PipeWire stream event API does not expose an exact stream xrun count. The
`SPA_IO_Position` area does, however, carry the graph driver's
`SPA_IO_CLOCK_FLAG_XRUN_RECOVER` flag to a node whose previous process call
missed its deadline. The adapter samples that flag once at the start of
every process callback and reports `graph_xrun_recovery_events` separately
from `xruns`. A missing or undersized position area in any callback makes the
recovery observation unavailable for the entire stream session. A new stream
resets the count and coverage state.

This notices *this node's* recovery callback. It is not an exact count of
device underruns, other nodes' xruns, or graph-driver failures. The flag is
transient and only observable when PipeWire invokes this process callback;
lost callbacks without a later recovery call remain invisible. `xruns` thus
remains unknown, even when the recovery count is zero. The G7 release gate
remains open. The physical 32-track runner's sampled `pw-top` ERR delta remains
a separate node/driver error observation.

The contract fixture covers zero, one and unavailable recovery observations,
their reset on a new stream, and separation from callback timing and backend
xruns. The opt-in `oscillate_audio_io_live_smoke --target-serial SERIAL` renders
silence at 48 kHz with 64- and 128-frame requests to one selected sink without
changing its volume or the default sink. It prints the recovery field and
keeps stream xruns unavailable. This smoke checks that PipeWire supplies the
position area on this host; it does not exercise the production graph or close
the 32-track gate.

The focused `audio_io_contract` passed. The opt-in smoke targeted the
user-selected GO Work USB Analogue Stereo sink by object serial 8732 on this
host. At 48 kHz, both requests observed the exact 64/128-frame block size,
zero invalid buffers and render failures, and zero node recovery notices.
Stream xruns stayed unavailable. This short, silent check did not change the
sink volume or default output and does not replace the six-run 32-track
physical workload.

PipeWire references: [stream events and IO callback](https://docs.pipewire.org/structpw__stream__events.html),
[SPA IO position](https://pvir.pages.freedesktop.org/pipewire/structspa__io__position.html),
and [xrun recovery flag semantics](https://pipewire.pages.freedesktop.org/pipewire/group__spa__node.html).
