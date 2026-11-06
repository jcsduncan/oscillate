#pragma once

#include "oscillate/application/session/session_service.hpp"
#include "oscillate/export/offline_master.hpp"

#include <memory>
#include <variant>

namespace oscillate::application {

// Prepared on the project control sequencer; move to one worker for master or
// stem rendering.
// The snapshot, managed-asset reader, transport gate and stopped lease survive
// independently of ProjectSession and its UI window.
class MasterExportTask {
public:
  MasterExportTask(MasterExportTask&&) noexcept = default;
  MasterExportTask& operator=(MasterExportTask&&) noexcept = default;
  MasterExportTask(const MasterExportTask&) = delete;
  MasterExportTask& operator=(const MasterExportTask&) = delete;

  [[nodiscard]] std::uint64_t revision() const noexcept { return snapshot_->revision; }
  [[nodiscard]] foundation::Result<exporting::PublishOutcome> run() &&;

private:
  friend class ExportService;
  MasterExportTask(std::shared_ptr<const project::Snapshot> snapshot,
                   persistence::ProjectStore reader,
                   std::shared_ptr<project::TransportGate> gate_owner,
                   exporting::StoppedExportAuthority authority,
                   std::variant<exporting::MasterRenderOptions, exporting::EncodedMasterOptions,
                                exporting::StemRenderOptions>
                       options)
      : snapshot_(std::move(snapshot)), reader_(std::move(reader)),
        gate_owner_(std::move(gate_owner)), authority_(std::move(authority)),
        options_(std::move(options)) {}

  std::shared_ptr<const project::Snapshot> snapshot_;
  persistence::ProjectStore reader_;
  std::shared_ptr<project::TransportGate> gate_owner_;
  exporting::StoppedExportAuthority authority_;
  std::variant<exporting::MasterRenderOptions, exporting::EncodedMasterOptions,
               exporting::StemRenderOptions>
      options_;
};

// A stopped, immutable project snapshot and pinned managed-asset reader for an
// optional exporter. The reader, gate and lease outlive ProjectSession until
// the worker's render or failure ends.
class StoppedSnapshotExportTask {
public:
  StoppedSnapshotExportTask(StoppedSnapshotExportTask&&) noexcept = default;
  StoppedSnapshotExportTask& operator=(StoppedSnapshotExportTask&&) noexcept = default;
  StoppedSnapshotExportTask(const StoppedSnapshotExportTask&) = delete;
  StoppedSnapshotExportTask& operator=(const StoppedSnapshotExportTask&) = delete;

  [[nodiscard]] const project::Snapshot& snapshot() const noexcept { return *snapshot_; }
  [[nodiscard]] const persistence::ProjectStore& reader() const noexcept { return reader_; }
  [[nodiscard]] exporting::StoppedExportAuthority take_authority() && noexcept {
    return std::move(authority_);
  }

private:
  friend class ExportService;
  StoppedSnapshotExportTask(std::shared_ptr<const project::Snapshot> snapshot,
                            persistence::ProjectStore reader,
                            std::shared_ptr<project::TransportGate> gate_owner,
                            exporting::StoppedExportAuthority authority)
      : snapshot_(std::move(snapshot)), reader_(std::move(reader)),
        gate_owner_(std::move(gate_owner)), authority_(std::move(authority)) {}

  std::shared_ptr<const project::Snapshot> snapshot_;
  persistence::ProjectStore reader_;
  std::shared_ptr<project::TransportGate> gate_owner_;
  exporting::StoppedExportAuthority authority_;
};

// A pinned, immutable snapshot and a managed-asset reader for offline
// analysis (Mix Check, ADR-0074). Like a track render it never touches the
// live graph or the transport; the reader outlives the session.
struct PinnedSnapshot {
  std::shared_ptr<const project::Snapshot> snapshot;
  persistence::ProjectStore reader;
  // The project's name: its folder's.
  std::string name = {};
};

class ExportService : public SessionService {
public:
  explicit ExportService(SessionState& state) noexcept : SessionService(state) {}

  [[nodiscard]] foundation::Result<MasterExportTask>
  prepare_wav_master_export(exporting::MasterRenderOptions options);
  [[nodiscard]] foundation::Result<MasterExportTask>
  prepare_encoded_master_export(exporting::EncodedMasterOptions options);
  [[nodiscard]] foundation::Result<MasterExportTask>
  prepare_wav_stem_export(exporting::StemRenderOptions options);
  [[nodiscard]] foundation::Result<StoppedSnapshotExportTask> prepare_stopped_snapshot_export();
  // The current snapshot and a reader, for a worker's offline renders
  // (ADR-0074); refused while managed media is pending or being repaired.
  [[nodiscard]] foundation::Result<PinnedSnapshot> pin_snapshot() const;

private:
  [[nodiscard]] foundation::Result<MasterExportTask> prepare_master_export(
      std::variant<exporting::MasterRenderOptions, exporting::EncodedMasterOptions,
                   exporting::StemRenderOptions> options);
};

} // namespace oscillate::application
