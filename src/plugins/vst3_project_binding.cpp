#include "oscillate/plugins/project_binding.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace oscillate::plugins {
namespace {

[[nodiscard]] bool valid_sha256(const std::string& value) noexcept {
  return value.size() == 64 && std::ranges::all_of(value, [](char digit) {
           return (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f');
         });
}

[[nodiscard]] bool valid_references(const project::Vst3PluginInstance& saved) {
  if (saved.parameters.size() > project::max_vst3_parameter_references)
    return false;
  std::unordered_set<std::uint32_t> ids;
  for (const auto& reference : saved.parameters) {
    if (!std::isfinite(reference.normalized_value) || reference.normalized_value < 0.0 ||
        reference.normalized_value > 1.0 || !ids.insert(reference.id).second)
      return false;
  }
  return true;
}

[[nodiscard]] const Vst3Class* matching_class(const project::Vst3PluginInstance& saved,
                                              const Vst3Metadata& metadata) noexcept {
  const auto found = std::ranges::find_if(metadata.classes, [&](const Vst3Class& candidate) {
    return candidate.id == saved.class_id && candidate.vendor == saved.vendor &&
           candidate.name == saved.name && candidate.version == saved.version;
  });
  return found == metadata.classes.end() ? nullptr : &*found;
}

[[nodiscard]] Vst3ProjectBindingResult failure(Vst3BindingStatus status, std::string diagnostic) {
  return {.status = status,
          .effect = {},
          .unresolved_parameter_ids = {},
          .diagnostic = std::move(diagnostic)};
}

Vst3ProjectBindingResult bind_saved(const project::Vst3PluginInstance& saved,
                                    const ScanResult& scanned, double sample_rate,
                                    std::uint32_t max_block_frames, Vst3ProcessingMode mode,
                                    Vst3HostKind kind, std::uint32_t max_parameter_events,
                                    bool routed_sidechain) {
  if (!scanned.ok())
    return failure(Vst3BindingStatus::scan_unavailable,
                   "VST3 scan is unavailable: " + scanned.diagnostic);
  if (!valid_sha256(saved.binary_sha256) || saved.binary_sha256 != scanned.binary.sha256 ||
      saved.component_state.size() > project::max_vst3_state_bytes ||
      (saved.controller_state && saved.controller_state->size() > project::max_vst3_state_bytes) ||
      !valid_references(saved))
    return failure(Vst3BindingStatus::identity_mismatch,
                   "Saved VST3 identity or state metadata is invalid or differs from scan");
  const auto* klass = matching_class(saved, scanned.metadata);
  if (!klass)
    return failure(Vst3BindingStatus::identity_mismatch,
                   "Saved VST3 class identity differs from scan");

  std::string error;
  const auto capacity = std::max(max_parameter_events, default_vst3_parameter_events);
  auto effect =
      kind == Vst3HostKind::instrument
          ? load_verified_vst3_instrument({*klass, scanned.binary}, sample_rate, max_block_frames,
                                          error, mode, capacity)
          : load_verified_vst3_effect({*klass, scanned.binary}, sample_rate, max_block_frames,
                                      error, mode, capacity, routed_sidechain);
  if (!effect)
    return failure(Vst3BindingStatus::load_failed, std::move(error));

  std::vector<std::uint32_t> unresolved;
  for (const auto& reference : saved.parameters) {
    const auto& available = effect->host().parameters();
    if (std::ranges::find(available, reference.id, &Vst3ParameterInfo::id) == available.end())
      unresolved.push_back(reference.id);
  }
  if ((!saved.component_state.empty() || saved.controller_state) &&
      !effect->host().restore_state_pair({saved.component_state, saved.controller_state}, error))
    return failure(Vst3BindingStatus::state_rejected, std::move(error));
  const bool static_values = saved.component_state.empty() && !saved.parameters.empty();
  if (static_values) {
    if (!unresolved.empty())
      return {.status = Vst3BindingStatus::parameter_values_unapplied,
              .effect = {},
              .unresolved_parameter_ids = std::move(unresolved),
              .diagnostic = "Saved VST3 parameter values include unavailable parameters"};
    std::vector<Vst3ParameterEvent> values;
    values.reserve(saved.parameters.size());
    for (const auto& reference : saved.parameters)
      values.push_back({reference.id, 0, reference.normalized_value});
    std::ranges::sort(values, {}, &Vst3ParameterEvent::id);
    if (!effect->host().stage_static_parameter_values(values, error))
      return failure(Vst3BindingStatus::parameter_values_unapplied, std::move(error));
  }
  if (!effect->host().activate(error))
    return failure(Vst3BindingStatus::activation_failed, std::move(error));
  if (static_values && !effect->host().flush_static_parameter_values(error))
    return failure(Vst3BindingStatus::parameter_values_unapplied, std::move(error));
  const bool has_unresolved = !unresolved.empty();
  std::string diagnostic;
  if (has_unresolved)
    diagnostic = "Saved VST3 parameter references are unavailable";
  if (effect->host().controller_state_sync() == Vst3ControllerStateSync::not_implemented &&
      !saved.controller_state) {
    if (!diagnostic.empty())
      diagnostic += "; ";
    diagnostic += "VST3 controller state synchronization is not implemented; native editor state "
                  "requires review";
  }
  const auto buses = effect->host().audio_bus_limits();
  if (buses.silent_auxiliary_inputs != 0) {
    if (!diagnostic.empty())
      diagnostic += "; ";
    diagnostic += "Unconnected VST3 auxiliary audio inputs receive silence; sidechain input is "
                  "not routed";
  }
  if (buses.discarded_secondary_outputs != 0) {
    if (!diagnostic.empty())
      diagnostic += "; ";
    diagnostic += "VST3 secondary audio outputs are discarded";
  }
  return {.status = Vst3BindingStatus::ready,
          .effect = std::move(effect),
          .unresolved_parameter_ids = std::move(unresolved),
          .diagnostic = std::move(diagnostic)};
}

} // namespace

Vst3ProjectBindingResult bind_vst3_project_effect(const project::Vst3PluginInstance& saved,
                                                  const ScanResult& scanned, double sample_rate,
                                                  std::uint32_t max_block_frames,
                                                  Vst3ProcessingMode mode,
                                                  std::uint32_t max_parameter_events,
                                                  bool routed_sidechain) {
  return bind_saved(saved, scanned, sample_rate, max_block_frames, mode, Vst3HostKind::effect,
                    max_parameter_events, routed_sidechain);
}

Vst3ProjectBindingResult bind_vst3_project_instrument(const project::Vst3PluginInstance& saved,
                                                      const ScanResult& scanned, double sample_rate,
                                                      std::uint32_t max_block_frames,
                                                      Vst3ProcessingMode mode,
                                                      std::uint32_t max_parameter_events) {
  return bind_saved(saved, scanned, sample_rate, max_block_frames, mode, Vst3HostKind::instrument,
                    max_parameter_events, false);
}

} // namespace oscillate::plugins
