#include "oscillate/native_inserts/instrument_binding.hpp"

#include "oscillate/native_dsp/synth/engines.hpp"

#include <string>
#include <utility>

namespace oscillate::native_inserts {

using namespace native_dsp;
using namespace native_dsp::synth;

namespace {

using foundation::ErrorCode;
using foundation::Result;

} // namespace

Result<ResolvedNativeInstrument>
resolve_native_instrument(const project::NativeInstrumentPatch& patch) {
  using R = Result<ResolvedNativeInstrument>;
  const SynthEngine* engine = find_engine(patch.engine);
  if (engine == nullptr) {
    return R::failure({ErrorCode::unavailable,
                       "Built-in synth '" + patch.engine + "' is not available in this version"});
  }
  if (patch.engine_version > engine->descriptor->version) {
    return R::failure({ErrorCode::unsupported, "This " + std::string(engine->descriptor->name) +
                                                   " patch was saved by a newer version"});
  }
  if (patch.engine_version < engine->descriptor->version) {
    // Version 1 is the only released table; later versions add upgraders here.
    return R::failure({ErrorCode::unsupported, "No upgrade path for this synth patch version"});
  }
  std::vector<NamedValue> saved;
  saved.reserve(patch.parameters.size());
  for (const auto& parameter : patch.parameters) {
    saved.push_back({parameter.id, parameter.value});
  }
  auto values = resolve_values(*engine->descriptor, saved);
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
  return R::success({engine, std::move(values).value(), patch.document});
}

Result<project::NativeInstrumentPatch> make_native_patch(const SynthEngine& engine,
                                                         std::span<const double> values,
                                                         project::NativeSynthDocument document) {
  using R = Result<project::NativeInstrumentPatch>;
  const auto& table = *engine.descriptor;
  if (values.size() != table.parameters.size()) {
    return R::failure({ErrorCode::invalid_argument, "Synth values do not match the engine"});
  }
  project::NativeInstrumentPatch patch;
  patch.engine = std::string(table.id);
  patch.engine_version = table.version;
  for (auto& entry : sparse_values(table, values)) {
    patch.parameters.push_back({std::move(entry.id), entry.value});
  }
  patch.document = std::move(document);
  // Round-trip through the resolver so only valid patches leave here.
  if (auto resolved = resolve_native_instrument(patch); !resolved) {
    return R::failure(resolved.error());
  }
  return R::success(std::move(patch));
}

Result<project::NativeInstrumentPatch> native_patch_for_preset(const SynthEngine& engine,
                                                               std::optional<std::size_t> preset) {
  using R = Result<project::NativeInstrumentPatch>;
  const auto& table = *engine.descriptor;
  if (preset && *preset >= table.presets.size()) {
    return R::failure({ErrorCode::not_found, "Synth preset not found"});
  }
  auto document =
      engine.preset_document ? engine.preset_document(preset) : project::NativeSynthDocument{};
  if (!preset) {
    return make_native_patch(engine, default_values(table), std::move(document));
  }
  auto values = preset_values(table, table.presets[*preset]);
  if (!values) {
    return R::failure(values.error());
  }
  return make_native_patch(engine, values.value(), std::move(document));
}

Result<std::size_t> native_parameter_index(const SynthEngine& engine,
                                           std::string_view parameter_id) {
  using R = Result<std::size_t>;
  const auto index = find_parameter(*engine.descriptor, parameter_id);
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

Result<project::AutomationDomain> native_automation_domain(const SynthEngine& engine,
                                                           std::string_view parameter_id) {
  using R = Result<project::AutomationDomain>;
  const auto index = native_parameter_index(engine, parameter_id);
  if (!index) {
    return R::failure(index.error());
  }
  const auto& parameter = engine.descriptor->parameters[index.value()];
  // Discrete parameters get a stepped domain over their integer range
  // (ADR-0034): points hold, and every value is a whole step.
  const bool discrete = is_discrete(parameter);
  const auto steps =
      discrete ? static_cast<std::uint32_t>(parameter.maximum - parameter.minimum) : 0U;
  return R::success({project::AutomationRepresentation::physical, std::string(parameter.id),
                     std::string(parameter.unit), parameter.minimum, parameter.maximum,
                     parameter.default_value, discrete, steps,
                     !discrete && parameter.scale == ParameterScale::logarithmic
                         ? project::AutomationScale::logarithmic
                         : project::AutomationScale::linear});
}

} // namespace oscillate::native_inserts
