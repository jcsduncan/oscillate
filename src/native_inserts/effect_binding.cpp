#include "oscillate/native_inserts/effect_binding.hpp"

#include <string>
#include <utility>

namespace oscillate::native_inserts {
namespace {

using foundation::ErrorCode;
using foundation::Result;
namespace effects = native_dsp::effects;
namespace synth = native_dsp::synth;

} // namespace

Result<ResolvedNativeEffect> resolve_native_effect(const project::NativeEffectPatch& patch) {
  using R = Result<ResolvedNativeEffect>;
  const auto* engine = effects::find_engine(patch.engine);
  if (engine == nullptr) {
    return R::failure({ErrorCode::unavailable,
                       "Built-in effect '" + patch.engine + "' is not available in this version"});
  }
  if (patch.engine_version > engine->descriptor->version) {
    return R::failure({ErrorCode::unsupported, "This " + std::string(engine->descriptor->name) +
                                                   " patch was saved by a newer version"});
  }
  if (patch.engine_version < engine->descriptor->version) {
    // Version 1 is the only released table; later versions add upgraders here.
    return R::failure({ErrorCode::unsupported, "No upgrade path for this effect patch version"});
  }
  std::vector<synth::NamedValue> saved;
  saved.reserve(patch.parameters.size());
  for (const auto& parameter : patch.parameters) {
    saved.push_back({parameter.id, parameter.value});
  }
  auto values = synth::resolve_values(*engine->descriptor, saved);
  if (!values) {
    return R::failure(values.error());
  }
  if (engine->validate_document != nullptr) {
    if (auto valid = engine->validate_document(patch.document); !valid) {
      return R::failure(valid.error());
    }
  } else if (!patch.document.empty()) {
    return R::failure({ErrorCode::invalid_argument,
                       std::string(engine->descriptor->name) + " has no structured state"});
  }
  return R::success({engine, std::move(values).value()});
}

Result<project::NativeEffectPatch> make_native_effect_patch(const effects::EffectEngine& engine,
                                                            std::span<const double> values,
                                                            project::NativeSynthDocument document) {
  using R = Result<project::NativeEffectPatch>;
  const auto& table = *engine.descriptor;
  if (values.size() != table.parameters.size()) {
    return R::failure({ErrorCode::invalid_argument, "Effect values do not match the engine"});
  }
  project::NativeEffectPatch patch;
  patch.engine = std::string(table.id);
  patch.engine_version = table.version;
  patch.document = std::move(document);
  for (auto& entry : synth::sparse_values(table, values)) {
    patch.parameters.push_back({std::move(entry.id), entry.value});
  }
  // Round-trip through the resolver so only valid patches leave here.
  if (auto resolved = resolve_native_effect(patch); !resolved) {
    return R::failure(resolved.error());
  }
  return R::success(std::move(patch));
}

Result<project::NativeEffectPatch>
native_effect_patch_for_preset(const effects::EffectEngine& engine,
                               std::optional<std::size_t> preset) {
  using R = Result<project::NativeEffectPatch>;
  const auto& table = *engine.descriptor;
  auto document = engine.preset_document != nullptr ? engine.preset_document(preset)
                                                    : project::NativeSynthDocument{};
  if (!preset) {
    return make_native_effect_patch(engine, synth::default_values(table), std::move(document));
  }
  if (*preset >= table.presets.size()) {
    return R::failure({ErrorCode::not_found, "Effect preset not found"});
  }
  auto values = synth::preset_values(table, table.presets[*preset]);
  if (!values) {
    return R::failure(values.error());
  }
  return make_native_effect_patch(engine, values.value(), std::move(document));
}

Result<std::size_t> native_effect_parameter_index(const effects::EffectEngine& engine,
                                                  std::string_view parameter_id) {
  using R = Result<std::size_t>;
  const auto index = synth::find_parameter(*engine.descriptor, parameter_id);
  if (!index) {
    return R::failure({ErrorCode::not_found, "Unknown " + std::string(engine.descriptor->name) +
                                                 " parameter '" + std::string(parameter_id) + "'"});
  }
  const auto& parameter = engine.descriptor->parameters[*index];
  if (!parameter.automatable) {
    return R::failure(
        {ErrorCode::unsupported, "'" + std::string(parameter.name) + "' cannot be automated"});
  }
  return R::success(*index);
}

Result<project::AutomationDomain>
native_effect_automation_domain(const effects::EffectEngine& engine,
                                std::string_view parameter_id) {
  using R = Result<project::AutomationDomain>;
  const auto index = native_effect_parameter_index(engine, parameter_id);
  if (!index) {
    return R::failure(index.error());
  }
  const auto& parameter = engine.descriptor->parameters[index.value()];
  const bool discrete = synth::is_discrete(parameter);
  const auto steps =
      discrete ? static_cast<std::uint32_t>(parameter.maximum - parameter.minimum) : 0U;
  return R::success({project::AutomationRepresentation::physical, std::string(parameter.id),
                     std::string(parameter.unit), parameter.minimum, parameter.maximum,
                     parameter.default_value, discrete, steps,
                     !discrete && parameter.scale == synth::ParameterScale::logarithmic
                         ? project::AutomationScale::logarithmic
                         : project::AutomationScale::linear});
}

} // namespace oscillate::native_inserts
