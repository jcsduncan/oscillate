#pragma once

#include "analogue_state.hpp"
#include "modulation.hpp"
#include "oscillate/foundation/error.hpp"
#include "oscillate/native_dsp/synth/nova/arp.hpp"
#include "oscillate/native_dsp/synth/nova/fx.hpp"
#include "oscillate/native_dsp/synth/nova/parameters.hpp"
#include "oscillate/native_dsp/synth/nova/tables.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"

#include <array>
#include <memory>
#include <string>
#include <vector>

// Nova's compiled document (ADR-0057, contract 08a §3): everything the
// kernel reads that the flat parameters cannot hold, built on control.
namespace oscillate::native_dsp::synth::nova {

struct NovaState final : DocumentState {
  std::vector<CompiledRoute> routes;
  std::array<CurveShape, lfo_count> lfo_shapes{};
  // Remap warp curves per oscillator and warp slot; null when absent (the
  // identity).
  std::array<std::array<const CurveShape*, warp_slots>, oscillator_count> remap{};
  std::vector<std::unique_ptr<CurveShape>> remap_storage;
  // What each oscillator reads: a table (Wavetable, Granular from a table)
  // or a sample (later phases); keys say where it came from, so an
  // unchanged slot reuses the built table.
  std::array<std::shared_ptr<const Wavetable>, oscillator_count> tables;
  std::array<std::shared_ptr<const SynthMediaAsset>, oscillator_count> samples;
  std::array<std::string, oscillator_count> table_keys;
  std::shared_ptr<const NoiseSample> noise;
  std::string noise_key;
  std::shared_ptr<const FxLayout> fx;
  SequencerClips clips;
  // Analogue's painted Voice Lanes (ADR-0073); NaN keeps the seeded value.
  AnalogueEdits analogue;
};

[[nodiscard]] foundation::Result<void>
validate_document(const project::NativeSynthDocument& document);

[[nodiscard]] foundation::Result<std::shared_ptr<const DocumentState>>
prepare_document(const project::NativeSynthDocument& document, const SynthMedia& media,
                 double sample_rate, const DocumentState* previous);

} // namespace oscillate::native_dsp::synth::nova
