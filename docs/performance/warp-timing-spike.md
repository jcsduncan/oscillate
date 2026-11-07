# Warp timing spike: rejected continuous processor

Status: research record for the Signalsmith Stretch candidate, superseded by [ADR-0026](../decisions/0026-continuous-warp-stream.md). The shipped engine is the in-house `warp_dsp`.

## Why it was tried

The existing segment-by-segment `exact()` render places reviewed warp markers on their requested output frames, but spectral state resets at each segment. On an internal 64-to-128-frame segment, contextual padding of `exact()` used neighbouring source samples yet produced an excessive boundary jump: 0.350821 for a 220 Hz sine and 0.129566 for a 1000 Hz sine. A continuous `Signalsmith Stretch::process()` stream reduced the same jumps to about 0.00864 and 0.03917 respectively, near the natural adjacent-sample step.

## Timing rejection

A continuous processor has ratio-dependent latency. Simply processing successive segments at their target lengths, flushing the reported latency and cropping a fixed prefix did not preserve the warp map:

- At 48 kHz, source anchors `(0,0), (48000,36000), (96000,96000)` and an impulse at source frame 48000 produced its largest output peak at frame **36704**, 704 frames (14.7 ms) after the reviewed marker.
- With impulses at the first and last source frames of that map, the raw first attack appeared around processor output frame 5043 while a fixed 5760-frame crop was used. The first 128 returned output frames contained a peak of only about 0.000141. An independent `exact()` onset patch could mask the missing attack, but did not correct internal marker timing.

These were exploratory signal probes, not production qualification. The amplitude/seam numbers are fixture-specific. The 704-frame timing error is enough to reject this implementation regardless of its seam improvement. No continuous-render code or cache-version change was retained.

## Gate and smallest next approach

Any candidate must first preserve transient timing at reviewed markers and clip endpoints. A focused regression fixture now places a single impulse exactly at a nonidentity marker and checks its rendered peak at the specified output frame. Broader qualification should cover markers/endpoints over short and long segments, ratios from 0.5 to 3, multiple transients, pitch options and block partitions.

The next narrow experiment is a timing-aware continuous stream: account for the processor's input/output latency when *scheduling each ratio change*, rather than merely cropping the final output. Measure latency for each ratio and verify marker impulses before comparing seam quality or CPU. If the library cannot make that contract reliable, evaluate a marker-aware processor with acceptable redistribution terms. Neither path has been validated yet.

## Latency-aware stream follow-up (16 October 2026)

A second, isolated probe used Signalsmith Stretch 1.4.0 at 48 kHz with one channel and the default preset. Its reported input and output latencies were both 2,880 frames. For each output block ending at frame `t`, the probe supplied input through `warp_source_at(t + output_latency) + input_latency`, rounded to the nearest frame. Initial `outputSeek()` consumed the corresponding input prefix. Thus a ratio transition was scheduled ahead of its audible marker instead of relying on a fixed final crop. The probe used 128-frame output blocks, a deterministic processor seed, an impulse on an internal source marker, and a second identity-ratio segment. Scratch source and executables stayed under this worktree's ignored `build/probe/` directory.

The schedule greatly reduced the earlier 704-frame long-marker error, but did not meet sample-accurate marker timing. At source frame 12,000, first-segment ratios 0.5, 0.75, 1, 1.5, 2 and 3 gave peak offsets of 0, +3, 0, -1, 0 and **-50 frames** respectively. At source frame 48,000 the 3× offset was **-70 frames**. An impulse at source frame 256 produced offsets of +117 at 0.5× and **-457 at 3×**; at frame 64, the 3× offset was -125. These short-segment errors are well outside an acceptable reviewed-marker tolerance. The larger errors likely reflect the spectral window and transient smearing as well as time-map scheduling; simply adjusting the integer latency cannot establish a reliable marker contract.

The current independent `exact()` segment render placed an impulse at the reviewed marker exactly in the long-segment 0.5–3× probe. A focused repository test now covers four of those ratios. It does not solve seam continuity or short nonidentity segments, which remain explicit limitations. The continuous stream was rejected again; no production renderer, dependency or cache-format change was made. A future candidate needs transient/anchor preservation in addition to latency-aware scheduling, with tests at short markers and both clip endpoints before seam or CPU comparison.
