#include "oscillate/native_dsp/synth/engines.hpp"

#include "oscillate/native_dsp/synth/anvil.hpp"
#include "oscillate/native_dsp/synth/concert.hpp"
#include "oscillate/native_dsp/synth/crate.hpp"
#include "oscillate/native_dsp/synth/drift_engine.hpp"
#include "oscillate/native_dsp/synth/ember.hpp"
#include "oscillate/native_dsp/synth/fret.hpp"
#include "oscillate/native_dsp/synth/halo.hpp"
#include "oscillate/native_dsp/synth/knock.hpp"
#include "oscillate/native_dsp/synth/nova.hpp"
#include "oscillate/native_dsp/synth/prism.hpp"
#include "oscillate/native_dsp/synth/pulse.hpp"
#include "oscillate/native_dsp/synth/torque.hpp"
#include "oscillate/native_dsp/synth/wren.hpp"

#include <array>

namespace oscillate::native_dsp::synth {
namespace {
// NOLINTNEXTLINE(bugprone-throwing-static-initialization): engines are built from constant tables.
const std::array<const SynthEngine*, 13> registry{
    &prism::engine(),  &ember::engine(), &nova::engine(),    &pulse::engine(), &halo::engine(),
    &torque::engine(), &drift::engine(), &concert::engine(), &anvil::engine(), &crate::engine(),
    &knock::engine(),  &fret::engine(),  &wren::engine()};
} // namespace

std::span<const SynthEngine* const> engines() noexcept { return registry; }

const SynthEngine* find_engine(std::string_view id) noexcept {
  for (const SynthEngine* engine : registry) {
    if (engine->descriptor->id == id) {
      return engine;
    }
  }
  return nullptr;
}

const SynthEngine& default_engine() noexcept { return prism::engine(); }

} // namespace oscillate::native_dsp::synth
