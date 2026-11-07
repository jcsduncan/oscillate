# Offline export with VST3 stages

`render_vst3_master` and `render_vst3_stem`
(`include/oscillate/application/vst3/offline_export.hpp`) export a stopped,
revision-owned project snapshot whose chains hold VST3 effects. They are the
shared offline render ([ADR-0090](../decisions/0090-render-graph-builder.md)):
`render_wav_master`, `render_encoded_master` and `render_wav_stem` with a VST3
stage preparer, so a VST3 effect is a chain stage like a built-in one
([ADR-0050](../decisions/0050-one-chain-per-track.md)). The Qt Quick Export
Master action takes this path when a track or the master holds a saved VST3
effect. Export runs on a worker and a failure never replaces an existing
output.

## Verification

Before any output opens, each enabled VST3 effect the render runs (on a track,
a bus or the master) and each VST3 instrument source is scanned afresh from its
saved bundle path. The scan must succeed, resolve to the saved canonical bundle
and binary hash, and list the saved class (identifier, vendor, name and
version); otherwise the export fails with the plugin's name and the reason. A
bypassed slot needs no binary. A frozen track plays its render, so its plugins
are not scanned.

## Rendering

The render is the native master's: `render_graph` prepares every chain, the
routed mixer and the master chain, and export runs 1,024-frame blocks. The
plugin host (`src/application/vst3/offline_export.cpp`) prepares each VST3
stage through the providers live playback uses, in VST3 offline processing
mode, for 128-frame host blocks (`vst3_host_block_frames`), and wraps it in
`engine::ChunkedProcessorRuntime`, which hands it each 1,024-frame block as
128-frame chunks with their project positions. A plugin therefore sees the
same blocks, positions, tempo, automation and sidechain detector input that
live playback at 128 frames gives it.

- **Chains.** Any chain renders: built-in and VST3 effects in saved order, on
  any number of audio, instrument, group and return paths and on the master.
  Clips may be warped, stretched or Clay-edited, as in any export.
- **Latency.** A track chain's plugin latency joins the route's delay
  compensation (up to 8,192 frames); a master plugin's latency drains after the
  mix. Both are trimmed at the writer, so the file starts at project frame 0.
- **Tails.** Each plugin's declared finite tail rings out whole; an unbounded
  tail fails before output opens.
- **Faults.** A plugin that fails a block fails the render, and the temporary
  output is discarded.
- **Stems.** `render_vst3_stem` writes one source track's stem as
  `render_wav_stems` does: the track alone through its routes and the master
  chain, keyed by the project's sidechains, padded to the master's length.

Output formats, rates, dither, loudness reports and tail trimming are the
native master's; see [VST3 output formats](vst3-output-formats.md).

## Evidence

`application_vst3_export_parity` holds the export to live routed playback: a
VST3 effect's master and both stems, its finite tail, a 96-frame latent plugin
on a track and on the master at 64- and 128-frame blocks, and a VST3
instrument's master and stem null exactly against the live stream with its
latency trimmed. `application_vst3_effect_export` covers serial, sidechained
and mixed native/VST3 chains, bus chains, PDC alignment, tempo changes,
static parameters, output formats and the verification and fault paths.
`render_graph_contract` checks that a preparer's stages join the chains, route
latency, master chain and fault check. The Quick fixture dispatches a 96 kHz
PCM24 effect master.
