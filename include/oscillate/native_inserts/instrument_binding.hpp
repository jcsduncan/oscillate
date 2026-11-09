#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"
#include "oscillate/project/model.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// Glue between the engine-agnostic saved patch and a concrete engine. The
// session, live playback and offline export all resolve through here, so they
// agree on ids, ranges, defaults and versions. Control thread only.
namespace oscillate::native_inserts {

struct ResolvedNativeInstrument {
  const native_dsp::synth::SynthEngine* engine = nullptr;
  std::vector<double> values;            // Dense, in the engine's table order.
  project::NativeSynthDocument document; // Validated by the engine.
};

// Unknown engines, newer engine versions, unknown ids, out-of-range values and
// documents the engine rejects fail without guessing; the caller keeps the
// saved patch untouched.
[[nodiscard]] foundation::Result<ResolvedNativeInstrument>
resolve_native_instrument(const project::NativeInstrumentPatch& patch);

// Dense values and a document -> the saved form, storing only non-default
// values. Both must be valid for the engine.
[[nodiscard]] foundation::Result<project::NativeInstrumentPatch>
make_native_patch(const native_dsp::synth::SynthEngine& engine, std::span<const double> values,
                  project::NativeSynthDocument document = {});

// A factory preset, or the engine defaults when preset is empty.
[[nodiscard]] foundation::Result<project::NativeInstrumentPatch>
native_patch_for_preset(const native_dsp::synth::SynthEngine& engine,
                        std::optional<std::size_t> preset);

// The automation domain for one continuous, automatable live parameter.
[[nodiscard]] foundation::Result<project::AutomationDomain>
native_automation_domain(const native_dsp::synth::SynthEngine& engine,
                         std::string_view parameter_id);

// The engine table index of an automatable parameter, for runtime parameter
// lanes. Fails for unknown ids and for parameters the engine cannot automate.
[[nodiscard]] foundation::Result<std::size_t>
native_parameter_index(const native_dsp::synth::SynthEngine& engine, std::string_view parameter_id);

} // namespace oscillate::native_inserts
