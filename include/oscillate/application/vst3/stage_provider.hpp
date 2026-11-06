#pragma once

#include "oscillate/application/routed_playback_control.hpp"
#include "oscillate/application/vst3/instrument_provider.hpp"
#include "oscillate/application/vst3/track_insert_provider.hpp"

#include <algorithm>
#include <variant>

namespace oscillate::plugins {

// Hosts every VST3 stage of a chain (ADR-0050): a track's instrument through
// the instrument provider, any effect through the effect provider.
class Vst3StageProvider final : public application::TrackInsertProvider {
public:
  Vst3StageProvider(Vst3InstrumentTrackProvider& instrument, Vst3TrackInsertProvider& effect)
      : instrument_(instrument), effect_(effect) {}

  [[nodiscard]] foundation::Result<application::PreparedTrackInsert>
  prepare(const project::Snapshot& snapshot, const project::Vst3PluginInstance& saved,
          std::uint32_t max_frames) override {
    if (is_instrument(snapshot, saved.id))
      return instrument_.prepare(snapshot, saved, max_frames);
    return effect_.prepare(snapshot, saved, max_frames);
  }

  // True when `id` is a track's VST3 instrument in `snapshot`.
  [[nodiscard]] static bool is_instrument(const project::Snapshot& snapshot,
                                          project::PluginInstanceId id) noexcept {
    return std::ranges::any_of(snapshot.tracks, [id](const project::Track& track) {
      const auto* source = std::get_if<project::Vst3PluginInstance>(&track.instrument_source);
      return source && source->id == id;
    });
  }

private:
  Vst3InstrumentTrackProvider& instrument_;
  Vst3TrackInsertProvider& effect_;
};

} // namespace oscillate::plugins
