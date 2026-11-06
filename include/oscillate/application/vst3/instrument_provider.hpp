#pragma once

#include "oscillate/application/routed_playback_control.hpp"
#include "oscillate/plugins/vst3_effect_host.hpp"
#include "oscillate/plugins/vst3_graph_node.hpp"
#include "oscillate/plugins/vst3_scan.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace oscillate::plugins {

class Vst3InstrumentRuntime;

// Independently reviewed evidence for one saved instrument source and one
// exact binary. A tail declaration alone cannot establish note-off release.
// This receipt is control-side and session-local, never saved in the project.
struct Vst3InstrumentTailQualification {
  std::string binary_sha256;
  std::uint64_t binary_byte_size = 0;
  std::string class_id;
  std::string vendor;
  std::string name;
  std::string version;
  std::uint32_t sample_rate = 0;
  std::uint32_t declared_tail_frames = 0;
  std::vector<std::uint8_t> component_state;
  std::vector<project::Vst3ParameterReference> parameters;
  // Both probes must send actual MIDI note-offs, then verify bounded release
  // through and beyond the declared tail after a fresh state restore.
  bool note_off_release_tested_64_frames = false;
  bool note_off_release_tested_128_frames = false;
  std::optional<std::vector<std::uint8_t>> controller_state = std::nullopt;
};

// Control-side bindings for verified saved VST3 instrument tracks, each the
// first stage of its track's chain (ADR-0050). Provide a fresh out-of-process
// scan receipt per saved plugin; each prepare binds the given snapshot's
// MIDI, tempo and automation. A live chain edit keeps the running instrument;
// changing its MIDI, tempo or state needs a fresh preparation.
class Vst3InstrumentTrackProvider final : public application::TrackInsertProvider {
public:
  explicit Vst3InstrumentTrackProvider(Vst3ProcessingMode mode = Vst3ProcessingMode::realtime,
                                       bool fixed_latency_verified = false,
                                       std::vector<std::uint32_t> qualified_manual_ids = {});
  void set_scan_receipt(project::PluginInstanceId id, ScanResult scanned);
  // Control-side manual admission for one saved source. The caller must have
  // independently qualified the exact binary/control/rate/block combination.
  // An explicit empty record keeps another source's admission from leaking.
  void set_manual_qualification(project::PluginInstanceId id, bool fixed_latency_verified,
                                std::vector<std::uint32_t> qualified_manual_ids = {});
  // Exact actual host.process segment-size evidence for production controls.
  void set_size_scoped_manual_qualification(project::PluginInstanceId id,
                                            bool fixed_latency_verified,
                                            std::vector<Vst3QualifiedManualBlock> qualified_blocks);
  // A caller supplies this only after independent binary/state qualification.
  // Saved automation or admitted manual edits invalidate fixed-tail admission.
  void set_tail_qualification(project::PluginInstanceId id,
                              Vst3InstrumentTailQualification qualification);
  [[nodiscard]] foundation::Result<application::PreparedTrackInsert>
  prepare(const project::Snapshot& snapshot, const project::Vst3PluginInstance& saved,
          std::uint32_t max_frames) override;

  // Call only after stopping and joining the callback. This sends a stopped
  // block to every prepared host so held notes receive note-offs before the
  // hosts are retired. The keyed overload addresses one exact instance.
  [[nodiscard]] bool stop_after_join() noexcept;
  [[nodiscard]] bool stop_after_join(project::TrackId track,
                                     project::PluginInstanceId plugin) noexcept;

  // One-shot control-side operation on a freshly prepared runtime, before any
  // callback. Requires exact finite note-off release evidence and no saved
  // automation or admitted manual edits. Returns the exclusive source end;
  // the caller renders the declared release tail after this sample.
  [[nodiscard]] foundation::Result<std::uint64_t> prepare_export_end(std::uint64_t end_sample);
  [[nodiscard]] foundation::Result<std::uint64_t>
  prepare_export_end(project::TrackId track, project::PluginInstanceId plugin,
                     std::uint64_t end_sample);
  [[nodiscard]] std::optional<std::uint64_t> prepared_export_end() const noexcept;
  [[nodiscard]] std::optional<std::uint64_t>
  prepared_export_end(project::TrackId track, project::PluginInstanceId plugin) const noexcept;

  // The no-argument receipt is retained for one-instance callers; it returns
  // no slot if multiple source instances are prepared. A caller needs
  // independent fixed-latency evidence before enabling manual admission.
  [[nodiscard]] std::shared_ptr<Vst3ManualEventSlot> manual_events() const noexcept;
  [[nodiscard]] std::shared_ptr<Vst3ManualEventSlot>
  manual_events(project::TrackId track, project::PluginInstanceId plugin) const noexcept;

private:
  Vst3ProcessingMode mode_;
  bool fixed_latency_verified_;
  std::vector<std::uint32_t> qualified_manual_ids_;
  struct ManualQualification {
    bool fixed_latency_verified = false;
    std::vector<std::uint32_t> qualified_manual_ids;
    std::optional<std::vector<Vst3QualifiedManualBlock>> qualified_blocks;
  };
  std::map<project::PluginInstanceId, ManualQualification> manual_qualifications_;
  std::map<project::PluginInstanceId, ScanResult> receipts_;
  std::map<project::PluginInstanceId, Vst3InstrumentTailQualification> tail_qualifications_;
  struct PreparedSource {
    project::TrackId track;
    std::weak_ptr<Vst3InstrumentRuntime> runtime;
    std::shared_ptr<Vst3ManualEventSlot> manual_events;
    std::optional<std::uint32_t> export_tail_frames;
  };
  std::map<project::PluginInstanceId, PreparedSource> prepared_;
};

} // namespace oscillate::plugins
