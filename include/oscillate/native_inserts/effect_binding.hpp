#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/project/model.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// Glue between a saved built-in effect patch and its engine (ADR-0040). The
// session, live playback and offline export all resolve through here, so they
// agree on ids, ranges, defaults and versions. Control thread only.
namespace oscillate::native_inserts {

struct ResolvedNativeEffect {
  const native_dsp::effects::EffectEngine* engine = nullptr;
  std::vector<double> values; // Dense, in the engine's table order.
};

// Unknown engines, newer engine versions, unknown ids, out-of-range values and
// documents the engine does not accept (ADR-0058) fail without guessing; the
// caller keeps the saved patch untouched.
[[nodiscard]] foundation::Result<ResolvedNativeEffect>
resolve_native_effect(const project::NativeEffectPatch& patch);

// Dense values and a document -> the saved form, storing only non-default
// values. The generation starts at 1; the model advances it on edits.
[[nodiscard]] foundation::Result<project::NativeEffectPatch>
make_native_effect_patch(const native_dsp::effects::EffectEngine& engine,
                         std::span<const double> values,
                         project::NativeSynthDocument document = {});

// A factory preset, or the engine defaults when preset is empty, with the
// document it starts with.
[[nodiscard]] foundation::Result<project::NativeEffectPatch>
native_effect_patch_for_preset(const native_dsp::effects::EffectEngine& engine,
                               std::optional<std::size_t> preset);

// The automation domain for one automatable parameter (stepped when
// discrete, ADR-0034).
[[nodiscard]] foundation::Result<project::AutomationDomain>
native_effect_automation_domain(const native_dsp::effects::EffectEngine& engine,
                                std::string_view parameter_id);

// The table index of an automatable parameter, for runtime parameter lanes.
[[nodiscard]] foundation::Result<std::size_t>
native_effect_parameter_index(const native_dsp::effects::EffectEngine& engine,
                              std::string_view parameter_id);

} // namespace oscillate::native_inserts
