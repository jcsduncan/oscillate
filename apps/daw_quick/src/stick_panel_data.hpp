#pragma once

#include "oscillate/native_dsp/synth/stick.hpp"

#include <QVariantMap>

#include <span>

// What Stick's panel (ADR-0114) reads beside its parameters: the chosen
// kit's library, the kit's mics and pieces, and its telemetry decoded. The
// device controller hands these to QML for the Stick engine.
namespace oscillate::quick::stick_panel {

// The kit's library: {state ("missing", "invalid", "ready" or "loading"),
// message, directory, name, attribution, install_command (installing this
// kit), required (what it plays, with its credit), resident_mb, pack_mb,
// samples (hits), mics, licence_url, kits: [{name, description, installed}]
// in Kit order}.
[[nodiscard]] QVariantMap library(native_dsp::synth::stick::Kit kit);

// The kit's mics and pieces: {kit, toms, groups: [{group, name, channels}]
// in MicGroup order, pieces: [{piece, id, name, hits, articulations
// ("side-stick, brushes"), keys ("C♯2, D2"), key (its plain stroke's,
// "D2"), brushes, present}] in Piece order}. A kit
// not yet loaded shows every piece its facts list, without hit counts.
[[nodiscard]] QVariantMap layout(native_dsp::synth::stick::Kit kit);

// Stick's telemetry frame into `out`: {library, kit, hits, voices,
// output_peak_db, pedal, last_piece, underruns, piece_levels [0..1 per
// Piece], mic_db [dBFS per MicGroup]}.
void decode(std::span<const float> frame, QVariantMap& out);

} // namespace oscillate::quick::stick_panel
