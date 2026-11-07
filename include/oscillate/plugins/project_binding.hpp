#pragma once

#include "oscillate/plugins/vst3_bundle_loader.hpp"
#include "oscillate/project/model.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace oscillate::plugins {

enum class Vst3BindingStatus : std::uint8_t {
  ready,
  scan_unavailable,
  identity_mismatch,
  load_failed,
  state_rejected,
  parameter_values_unapplied,
  activation_failed,
  timing_unsupported,
  automation_unavailable,
};

struct Vst3ProjectBindingResult {
  Vst3BindingStatus status{Vst3BindingStatus::scan_unavailable};
  std::unique_ptr<LoadedVst3Effect> effect;
  std::vector<std::uint32_t> unresolved_parameter_ids;
  std::string diagnostic;

  [[nodiscard]] bool ready() const noexcept {
    return status == Vst3BindingStatus::ready && effect != nullptr;
  }
};

// Control side only. A fresh successful scan receipt is mandatory, including when
// the saved instance has missing=true. The saved path is advisory and never used
// for loading. No project mutation or graph insertion occurs here. Callers
// with automation pass vst3_automation_event_budget(); the host never gets
// less than default_vst3_parameter_events so static values still fit.
[[nodiscard]] Vst3ProjectBindingResult
bind_vst3_project_effect(const project::Vst3PluginInstance& saved, const ScanResult& scanned,
                         double sample_rate, std::uint32_t max_block_frames,
                         Vst3ProcessingMode mode = Vst3ProcessingMode::realtime,
                         std::uint32_t max_parameter_events = default_vst3_parameter_events,
                         bool routed_sidechain = false);

// Same verified saved-state contract for an instrument source. The returned
// host has an event input and no audio input. Control/UI code may inspect its
// parameters after binding; it must suspend before saving component state.
[[nodiscard]] Vst3ProjectBindingResult
bind_vst3_project_instrument(const project::Vst3PluginInstance& saved, const ScanResult& scanned,
                             double sample_rate, std::uint32_t max_block_frames,
                             Vst3ProcessingMode mode = Vst3ProcessingMode::realtime,
                             std::uint32_t max_parameter_events = default_vst3_parameter_events);

// The same contracts from a binary hashed on a worker (hash_vst3_binary): the
// digest must match the scan and the saved instance, and loading compares the
// file's stamp instead of hashing it again. Load, plugin creation and state
// restore run on the calling thread (the GUI thread for an editor).
[[nodiscard]] Vst3ProjectBindingResult
bind_hashed_vst3_project_effect(const project::Vst3PluginInstance& saved, const ScanResult& scanned,
                                const Vst3HashedBinary& hashed, double sample_rate,
                                std::uint32_t max_block_frames);

[[nodiscard]] Vst3ProjectBindingResult
bind_hashed_vst3_project_instrument(const project::Vst3PluginInstance& saved,
                                    const ScanResult& scanned, const Vst3HashedBinary& hashed,
                                    double sample_rate, std::uint32_t max_block_frames);

} // namespace oscillate::plugins
