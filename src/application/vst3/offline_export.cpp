#include "oscillate/application/vst3/offline_export.hpp"

#include "oscillate/application/routed_playback_control.hpp"
#include "oscillate/application/vst3/instrument_provider.hpp"
#include "oscillate/application/vst3/stage_provider.hpp"
#include "oscillate/application/vst3/track_insert_provider.hpp"
#include "oscillate/engine/chunked_processor.hpp"
#include "oscillate/project/frozen_view.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace oscillate::exporting {
namespace {

using foundation::ErrorCode;

// Every VST3 plugin a render runs: each track's instrument source and each
// enabled effect, on tracks, buses and the master. A bypassed slot needs no
// binary.
[[nodiscard]] std::vector<const project::Vst3PluginInstance*>
rendered_plugins(const project::Snapshot& snapshot) {
  std::vector<const project::Vst3PluginInstance*> plugins;
  const auto add_effects = [&](const std::vector<project::EffectInsert>& effects) {
    for (const auto& slot : effects)
      if (const auto* plugin = std::get_if<project::Vst3PluginInstance>(&slot.processor);
          plugin != nullptr && slot.enabled)
        plugins.push_back(plugin);
  };
  for (const auto& track : snapshot.tracks) {
    if (const auto* source = std::get_if<project::Vst3PluginInstance>(&track.instrument_source))
      plugins.push_back(source);
    add_effects(track.effects);
  }
  add_effects(snapshot.master_effects);
  return plugins;
}

// The VST3 host of one offline render: it verifies each saved plugin against
// a fresh scan, then prepares its stages for offline processing.
class OfflineVst3Host {
public:
  OfflineVst3Host() = default;
  OfflineVst3Host(const OfflineVst3Host&) = delete;
  OfflineVst3Host& operator=(const OfflineVst3Host&) = delete;

  // A frozen track plays its render (ADR-0059), so only the plugins the
  // audible view runs need their binaries.
  [[nodiscard]] foundation::Result<void> verify(const project::Snapshot& saved_snapshot,
                                                plugins::Vst3Scanner& scanner) {
    using R = foundation::Result<void>;
    const auto snapshot = project::has_frozen_track(saved_snapshot)
                              ? project::audible_snapshot(saved_snapshot)
                              : saved_snapshot;
    // One scan per bundle: several slots may load the same plugin.
    std::map<std::string, plugins::ScanResult> scans;
    for (const auto* saved : rendered_plugins(snapshot)) {
      auto found = scans.find(saved->bundle_path);
      if (found == scans.end())
        found = scans.emplace(saved->bundle_path, scanner.scan(saved->bundle_path)).first;
      const auto& scanned = found->second;
      const auto plugin = "VST3 plugin " + saved->name;
      if (!scanned.ok())
        return R::failure(
            {ErrorCode::unavailable, plugin + " fresh scan failed: " + scanned.diagnostic});
      std::error_code canonical_error;
      const auto bundle = std::filesystem::canonical(saved->bundle_path, canonical_error);
      if (canonical_error || bundle != scanned.binary.canonical_bundle ||
          saved->binary_sha256 != scanned.binary.sha256)
        return R::failure(
            {ErrorCode::unavailable, plugin + " binary differs from its saved slot; relink it"});
      if (std::ranges::none_of(scanned.metadata.classes, [&](const auto& klass) {
            return klass.id == saved->class_id && klass.vendor == saved->vendor &&
                   klass.name == saved->name && klass.version == saved->version;
          }))
        return R::failure(
            {ErrorCode::unavailable, plugin + " class differs from its saved slot; relink it"});
      if (plugins::Vst3StageProvider::is_instrument(snapshot, saved->id))
        instruments_.set_scan_receipt(saved->id, scanned);
      else
        effects_.set_scan_receipt(saved->id, scanned);
    }
    return R::success();
  }

  // Each stage runs in host blocks of at most vst3_host_block_frames, chunked
  // when the render's blocks are larger, as live playback runs it.
  [[nodiscard]] render_graph::Vst3StagePreparer preparer() {
    return [this](const project::Snapshot& snapshot, const track_chain::SavedStage& stage,
                  std::size_t max_frames) -> foundation::Result<track_chain::Stage> {
      using R = foundation::Result<track_chain::Stage>;
      const auto* plugin = std::get_if<project::Vst3PluginInstance>(&stage.processor);
      if (plugin == nullptr)
        return R::failure({ErrorCode::corrupt_data, "Offline VST3 stage has no plugin"});
      const auto host_frames =
          std::min<std::size_t>(max_frames, application::vst3_host_block_frames);
      auto prepared = stages_.prepare(snapshot, *plugin, static_cast<std::uint32_t>(host_frames));
      if (!prepared)
        return R::failure({prepared.error().code,
                           "VST3 plugin " + plugin->name + ": " + prepared.error().message});
      auto& insert = prepared.value();
      if (!insert.processor || !insert.process_failed)
        return R::failure({ErrorCode::corrupt_data,
                           "VST3 plugin " + plugin->name + " returned an incomplete processor"});
      if (insert.instrument_source != !stage.slot)
        return R::failure({ErrorCode::unsupported,
                           "VST3 plugin " + plugin->name + " is not the saved stage's kind"});
      auto processor = std::move(insert.processor);
      if (max_frames > host_frames) {
        auto chunked = engine::ChunkedProcessorRuntime::prepare(std::move(processor), host_frames);
        if (!chunked)
          return R::failure(chunked.error());
        processor = std::move(chunked).value();
      }
      // An offline render keeps the plugin's declared tail: no live edit can
      // change it while the render runs.
      std::optional<std::uint64_t> tail;
      if (insert.finite_tail_frames)
        tail = *insert.finite_tail_frames;
      return R::success({stage, std::move(processor), insert.latency_frames, tail, tail,
                         insert.routed_sidechain, std::move(insert.process_failed)});
    };
  }

private:
  plugins::Vst3InstrumentTrackProvider instruments_{plugins::Vst3ProcessingMode::offline};
  plugins::Vst3TrackInsertProvider effects_{plugins::Vst3ProcessingMode::offline};
  plugins::Vst3StageProvider stages_{instruments_, effects_};
};

[[nodiscard]] foundation::Result<PublishOutcome> stale(const std::string& what) {
  return foundation::Result<PublishOutcome>::failure(
      {ErrorCode::conflict, what + " export rejected: stopped authority is stale"});
}

} // namespace

foundation::Result<PublishOutcome> render_vst3_master(const project::Snapshot& snapshot,
                                                      const persistence::ProjectStore& store,
                                                      StoppedExportAuthority authority,
                                                      plugins::Vst3Scanner& scanner,
                                                      const MasterOutputOptions& options) {
  if (!authority.matches(snapshot))
    return stale("Master");
  OfflineVst3Host host;
  if (auto verified = host.verify(snapshot, scanner); !verified)
    return foundation::Result<PublishOutcome>::failure(verified.error());
  if (const auto* wav = std::get_if<MasterRenderOptions>(&options))
    return render_wav_master(snapshot, store, std::move(authority), *wav, host.preparer());
  return render_encoded_master(snapshot, store, std::move(authority),
                               std::get<EncodedMasterOptions>(options), host.preparer());
}

foundation::Result<PublishOutcome>
render_vst3_stem(const project::Snapshot& snapshot, const persistence::ProjectStore& store,
                 StoppedExportAuthority authority, plugins::Vst3Scanner& scanner,
                 project::TrackId track, const MasterRenderOptions& options) {
  if (!authority.matches(snapshot))
    return stale("Stem");
  OfflineVst3Host host;
  if (auto verified = host.verify(snapshot, scanner); !verified)
    return foundation::Result<PublishOutcome>::failure(verified.error());
  return render_wav_stem(snapshot, store, std::move(authority), track, options, host.preparer());
}

} // namespace oscillate::exporting
