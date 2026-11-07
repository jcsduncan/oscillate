# Direct sidechain detector stems

Processed stem export supports saved native compressor sidechains sent directly
from audio or sampler source tracks. For each stem, all direct detector sources
run through their saved insert chains, channel gain, automation, and aligned
sidechain sends. Audible main and ordinary send edges from every other source
track are zeroed after route gain evaluation. The selected source keeps its
audible routes, and the master path remains the same as for a full render.

The export route accounts for latency on both the detector edge and the direct
source input of a compressor's track. It renders the common project extent and
the bounded downstream effect tails before publishing the staged stem bundle.
Detector tracks do not become audible merely because they drive a compressor.

Dedicated group or return detector buses are supported when they receive
ordinary sends directly from audio or sampler source tracks, send only to
native compressor sidechain inputs on audio source tracks, and have no audible
output route beyond their default master edge. Each stem keeps all direct
inputs to these buses so the detector remains the full project signal, then
zeros the buses' audible master edges. Source-track audible routes are isolated
as before. The buses and their saved insert chains still process the detector
signal, including route gain automation and latency compensation.

A detector bus with a routed main input, another bus as an input, an additional
audible send/output or a bus sidechain target is rejected before creating the
stem bundle. These routes can mix a detector-only signal into an audible stem
path and need a separate detector graph. The master render retains its
existing full-mix behaviour.

The export fixture checks independent compressor samples, no detector bleed,
repeat renders, limiter latency alignment, reverb tail extent and isolation,
injected failure with no partial publication, dedicated bus detector output,
and rejection of an unseparated bus route.
