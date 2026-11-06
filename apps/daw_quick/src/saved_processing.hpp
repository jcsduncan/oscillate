#pragma once

#include "oscillate/project/model.hpp"

#include <cstddef>
#include <variant>
#include <vector>

namespace oscillate::quick::detail {

[[nodiscard]] inline const project::Vst3PluginInstance*
vst3_instrument_source(const project::Track& track) {
  return std::get_if<project::Vst3PluginInstance>(&track.instrument_source);
}

[[nodiscard]] inline const project::Vst3PluginInstance*
vst3_effect(const project::EffectInsert& slot) {
  return std::get_if<project::Vst3PluginInstance>(&slot.processor);
}

[[nodiscard]] inline const project::Vst3PluginInstance* find_vst3(const project::Track& track,
                                                                  project::PluginInstanceId id) {
  if (const auto* source = vst3_instrument_source(track); source && source->id == id)
    return source;
  for (const auto& slot : track.effects) {
    if (const auto* effect = vst3_effect(slot); effect && effect->id == id)
      return effect;
  }
  return nullptr;
}

[[nodiscard]] inline std::vector<const project::Vst3PluginInstance*>
saved_vst3_plugins(const project::Track& track) {
  std::vector<const project::Vst3PluginInstance*> plugins;
  if (const auto* source = vst3_instrument_source(track))
    plugins.push_back(source);
  for (const auto& slot : track.effects) {
    if (const auto* effect = vst3_effect(slot))
      plugins.push_back(effect);
  }
  return plugins;
}

[[nodiscard]] inline bool has_saved_vst3(const project::Track& track) {
  if (vst3_instrument_source(track))
    return true;
  for (const auto& slot : track.effects) {
    if (vst3_effect(slot))
      return true;
  }
  return false;
}

} // namespace oscillate::quick::detail
