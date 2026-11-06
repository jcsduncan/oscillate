#pragma once

#include "oscillate/application/routed_playback_control.hpp"
#include "oscillate/plugins/vst3_graph_node.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace oscillate::plugins {

// Evidence supplied by a separate, repeatable compatibility qualification.
// This is deliberately session-local: it is never inferred from a plugin's
// getTailSamples() declaration, project file, or scan receipt alone.
struct Vst3FixedTailQualification {
  std::string binary_sha256;
  std::uint64_t binary_byte_size = 0;
  std::string class_id;
  std::string vendor;
  std::string version;
  std::uint32_t sample_rate = 0;
  std::uint32_t declared_tail_frames = 0;
  std::vector<std::uint8_t> component_state;
  std::vector<project::Vst3ParameterReference> parameters;
  // A qualification run must exercise both callback sizes. The provider
  // admits only these exact prepared block sizes; no interpolation is assumed.
  bool tested_64_frames = false;
  bool tested_128_frames = false;
  std::optional<std::vector<std::uint8_t>> controller_state = std::nullopt;
};

// Control-side adapter for a per-project routed-live session. The caller
// supplies an out-of-process scan receipt for each saved plugin ID. Every
// prepared effect instance receives a unique high-half graph ID, so a changed
// plugin's new instance runs beside the old one while it rings out (ADR-0050).
// No scanning, binary loading or factory use occurs on the audio callback.
class Vst3TrackInsertProvider final : public application::TrackInsertProvider {
public:
  // The bool is reserved for synthetic single-effect full-binary fixtures.
  // Production callers supply independent evidence per saved instance below.
  explicit Vst3TrackInsertProvider(Vst3ProcessingMode mode = Vst3ProcessingMode::realtime,
                                   bool fixed_latency_verified = false);

  void set_scan_receipt(project::PluginInstanceId id, ScanResult scanned);
  // Control side only, before prepare. An explicit empty record denies manual
  // admission for this saved instance. A record for any instance disables the
  // constructor-wide synthetic fallback for every unrecorded instance.
  void set_manual_qualification(project::PluginInstanceId id, bool fixed_latency_verified,
                                std::vector<std::uint32_t> parameter_ids = {});
  // Production evidence is scoped to each actual host.process segment size.
  // This replaces any prior per-instance record; no size is inferred from the
  // requested device quantum or the host's maximum block allocation.
  // A fixed-latency record with both 64/128 sizes and no parameter IDs may
  // coexist with independent exact-state fixed-tail evidence. It admits no
  // live manual controls, so that tail receipt cannot be invalidated by one.
  void set_size_scoped_manual_qualification(project::PluginInstanceId id,
                                            bool fixed_latency_verified,
                                            std::vector<Vst3QualifiedManualBlock> qualified_blocks);
  // Control side only. The caller is responsible for independently proving
  // silence after the declared drain and repeatability across state restore.
  // Supplying a record does not override a changed binary, saved state,
  // automation or the runtime's own tail declaration.
  void set_fixed_tail_qualification(project::PluginInstanceId id,
                                    Vst3FixedTailQualification qualification);
  [[nodiscard]] foundation::Result<application::PreparedTrackInsert>
  prepare(const project::Snapshot& snapshot, const project::Vst3PluginInstance& saved,
          std::uint32_t max_frames) override;
  // Control-side observation only: the newest instance's graph ID. Failed
  // preparations never reserve an ID.
  [[nodiscard]] std::optional<std::uint64_t>
  assigned_instance_id(project::PluginInstanceId id) const noexcept;

private:
  Vst3ProcessingMode mode_;
  bool fixed_latency_verified_;
  std::map<project::PluginInstanceId, ScanResult> receipts_;
  struct ManualQualification {
    bool fixed_latency_verified = false;
    std::vector<std::uint32_t> parameter_ids;
    std::optional<std::vector<Vst3QualifiedManualBlock>> qualified_blocks;
  };
  std::map<project::PluginInstanceId, ManualQualification> manual_qualifications_;
  std::map<project::PluginInstanceId, Vst3FixedTailQualification> fixed_tail_qualifications_;
  std::map<project::PluginInstanceId, std::uint64_t> instance_ids_;
  std::uint64_t next_instance_id_;
};

} // namespace oscillate::plugins
