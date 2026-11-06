#pragma once

#include "oscillate/native_dsp/synth/ember_circuit.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"
#include "oscillate/native_dsp/synth/voice_pool.hpp"

#include <cstddef>
#include <cstdint>

namespace oscillate::native_dsp::synth::ember {

inline constexpr std::uint32_t voice_limit = ember_circuit::cards;

// Ember: a warm vintage poly. A Juno-style DCO with mixable waves, a
// Prophet-style VCO B with sync and poly-mod, a 2x-oversampled OTA 4-pole or
// SEM-style morphing filter per mono voice, a circuit model (ADR-0066: the
// crystal DCO against the wandering VCO, per-card tolerances by unit,
// leakage, supply sag and mains hum), and an ensemble -> chorus -> BBD
// delay -> tape chain. Defaults are the "Ember Default" warm poly.
[[nodiscard]] const SynthEngine& engine();

// Telemetry (ADR-0066): the version, then per voice card whether it sounds,
// how far VCO B and DCO A sit from their ideal pitch now (cents) and its
// cutoff offset (semitones), then the hum gate (0..1) and the supply load
// (0..1, the sounding cards' summed level over the card count).
namespace telemetry {
inline constexpr std::size_t version = 0;
inline constexpr std::size_t card_active = 1;
inline constexpr std::size_t card_vco_cents = card_active + ember_circuit::cards;
inline constexpr std::size_t card_dco_cents = card_vco_cents + ember_circuit::cards;
inline constexpr std::size_t card_cutoff = card_dco_cents + ember_circuit::cards;
inline constexpr std::size_t hum_gate = card_cutoff + ember_circuit::cards;
inline constexpr std::size_t load = hum_gate + 1;
inline constexpr std::size_t size = load + 1;
} // namespace telemetry

} // namespace oscillate::native_dsp::synth::ember
