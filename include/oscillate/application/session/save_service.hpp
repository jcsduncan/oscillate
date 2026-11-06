#pragma once

#include "oscillate/application/session/session_service.hpp"

#include <memory>

namespace oscillate::application {

class SaveService : public SessionService {
public:
  explicit SaveService(SessionState& state) noexcept : SessionService(state) {}

  class AutosaveTask {
  public:
    AutosaveTask(std::shared_ptr<const project::Snapshot> snapshot,
                 persistence::ProjectStore writer)
        : snapshot_(std::move(snapshot)), writer_(std::move(writer)) {}
    AutosaveTask(AutosaveTask&&) noexcept = default;
    AutosaveTask& operator=(AutosaveTask&&) noexcept = default;
    AutosaveTask(const AutosaveTask&) = delete;
    AutosaveTask& operator=(const AutosaveTask&) = delete;
    [[nodiscard]] std::uint64_t revision() const noexcept { return snapshot_->revision; }
    [[nodiscard]] project::ProjectId project_id() const noexcept { return snapshot_->id; }
    [[nodiscard]] foundation::Result<persistence::SaveOutcome> run() && {
      return writer_.save(*snapshot_, persistence::SaveKind::autosave);
    }

  private:
    std::shared_ptr<const project::Snapshot> snapshot_;
    persistence::ProjectStore writer_;
  };

  class BackgroundSaveTask {
  public:
    BackgroundSaveTask(std::shared_ptr<const project::Snapshot> snapshot,
                       persistence::ProjectStore writer)
        : snapshot_(std::move(snapshot)), writer_(std::move(writer)) {}
    BackgroundSaveTask(BackgroundSaveTask&&) noexcept = default;
    BackgroundSaveTask& operator=(BackgroundSaveTask&&) noexcept = default;
    BackgroundSaveTask(const BackgroundSaveTask&) = delete;
    BackgroundSaveTask& operator=(const BackgroundSaveTask&) = delete;
    [[nodiscard]] std::uint64_t revision() const noexcept { return snapshot_->revision; }
    [[nodiscard]] project::ProjectId project_id() const noexcept { return snapshot_->id; }
    [[nodiscard]] foundation::Result<persistence::SaveOutcome> run() && {
      return writer_.save(*snapshot_, persistence::SaveKind::explicit_save);
    }

  private:
    std::shared_ptr<const project::Snapshot> snapshot_;
    persistence::ProjectStore writer_;
  };

  [[nodiscard]] foundation::Result<void> save();
  // Routine parameter edits can publish CURRENT on a worker. Only snapshots
  // whose media and external links are already saved are eligible.
  [[nodiscard]] foundation::Result<BackgroundSaveTask> begin_background_save() const;
  void finish_background_save(std::uint64_t revision, const persistence::SaveOutcome& outcome);
  // Capture a stable revision and a worker-only writer without performing
  // filesystem writes on the UI thread. A pending new managed source still
  // requires its explicit save/receipt publication first.
  [[nodiscard]] foundation::Result<AutosaveTask> begin_autosave() const;
  [[nodiscard]] const std::optional<foundation::Error>& save_maintenance_warning() const noexcept;
  [[nodiscard]] bool has_unsaved_changes() const noexcept;
  [[nodiscard]] std::uint64_t persisted_revision() const noexcept;
  // Names the current explicit save after persisting the model. A name is
  // immutable; an uncertain pointer publication must be inspected by listing.
  [[nodiscard]] foundation::Result<persistence::SaveOutcome>
  create_named_revision(std::string_view name);
  [[nodiscard]] foundation::Result<persistence::NamedRevisionListing> list_named_revisions() const;
  // Requires stopped transport, saved current edits and intact named media.
  // A successful model restore is one undo step. Save failure retains that
  // in-memory edit for retry or undo and does not change the named pointer.
  [[nodiscard]] foundation::Result<void> restore_named_revision(std::string_view name);
};

} // namespace oscillate::application
