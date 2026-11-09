// The device chain's macro strip and the user's racks (ADR-0059).
#include "project_bridge.hpp"

#include "macro_controller.hpp"
#include "native_synth_device_controller.hpp"
#include "native_synth_session_adapter.hpp"
#include "rack_library_controller.hpp"
#include "routed_playback.hpp"

#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace oscillate::quick {

void ProjectBridge::create_macro_controllers() {
  const auto can_edit = [this] {
    return session_ && !control_busy() && !playback_completion_ && !export_busy() && !import_busy();
  };
  const auto after_edit = [this] {
    refresh();
    submit_playback_edit();
  };
  macros_ = std::make_unique<MacroController>(
      MacroController::Hooks{
          [this] { return session_.get(); }, can_edit,
          [this](QString message) { set_status(std::move(message)); }, after_edit,
          [this] { schedule_background_save(); },
          [this](project::TrackId track, const native_dsp::EngineDescriptor& engine,
                 std::span<const double> values) {
            return live_run() && live_run()->set_live_synth_values(track, engine, values);
          },
          [this](project::TrackId track, project::InsertSlotId slot,
                 const native_dsp::EngineDescriptor& engine, std::span<const double> values) {
            return live_run() && live_run()->set_live_effect_values(track, slot, engine, values);
          },
          [this](project::TrackId track, std::optional<project::InsertSlotId> slot,
                 const std::vector<double>* values) { show_macro_values(track, slot, values); }},
      this);
  racks_ = std::make_unique<RackLibraryController>(
      RackLibraryController::Hooks{[this] { return session_.get(); }, can_edit,
                                   [this](QString message) { set_status(std::move(message)); },
                                   after_edit, [this] { schedule_background_save(); }},
      this);
  connect(this, &ProjectBridge::revisionChanged, macros_.get(), &MacroController::refresh);
  connect(this, &ProjectBridge::projectChanged, macros_.get(), &MacroController::refresh);
}

QObject* ProjectBridge::macros() const noexcept { return macros_.get(); }
QObject* ProjectBridge::racks() const noexcept { return racks_.get(); }

void ProjectBridge::share_macro_map(NativeSynthDeviceController& controller) {
  if (macros_)
    controller.set_macro_map_state(&macros_->map_state());
}

void ProjectBridge::show_macro_values(project::TrackId track,
                                      std::optional<project::InsertSlotId> slot,
                                      const std::vector<double>* values) {
  const auto follow = [&](SessionNativeSynthAdapter* adapter,
                          NativeSynthDeviceController* controller) {
    if (adapter == nullptr || controller == nullptr || adapter->track() != track ||
        adapter->effect_slot() != slot)
      return;
    adapter->show_macro_values(values);
    controller->refresh();
  };
  for (auto& [key, entry] : device_strips_)
    follow(entry.adapter.get(), entry.controller.get());
  follow(native_synth_adapter_.get(), native_synth_controller_.get());
  follow(native_effect_adapter_.get(), native_effect_controller_.get());
}

} // namespace oscillate::quick
