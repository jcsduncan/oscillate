#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/project/model.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace oscillate::persistence {

enum class SaveKind : std::uint8_t { explicit_save, autosave };
enum class SaveStatus : std::uint8_t { committed, uncertain };
enum class RestoreStatus : std::uint8_t { committed, uncertain };
enum class SaveStage : std::uint8_t {
  root_parent_flushed,
  asset_file_flushed,
  asset_published,
  assets_flushed,
  manifest_written,
  generation_flushed,
  generation_published,
  generations_flushed,
  pointer_written,
  pointer_published,
  state_flush_failed,
  revision_pointer_written,
  revision_pointer_published,
  revision_directory_flush_failed
};

struct SaveOutcome {
  SaveStatus status;
  std::string generation;
  // Save remains committed if post-publication retention fails. Callers can
  // report this warning without asking the user to repeat a durable save.
  std::optional<foundation::Error> maintenance_warning = std::nullopt;
};

struct LoadedGeneration {
  project::Snapshot snapshot;
  std::string generation;
  SaveKind kind;
  std::optional<std::string> parent;
  struct SourceDiagnostic {
    project::SourceId id;
    enum class Status : std::uint8_t { unresolved, missing, damaged } status;
  };
  std::vector<SourceDiagnostic> source_diagnostics;
};

struct NamedRevision {
  std::string name;
  std::string generation;
  std::string entry;
};

struct NamedRevisionIssue {
  enum class Kind : std::uint8_t { incomplete, malformed, stale, duplicate };
  std::string entry;
  std::optional<std::string> name;
  Kind kind;
};

struct NamedRevisionListing {
  std::vector<NamedRevision> revisions;
  std::vector<NamedRevisionIssue> issues;
};

// Issued only after a managed asset is durably published. Bound to one store
// instance, including moves; callers cannot construct or forge a receipt.
class SourceReceipt {
public:
  SourceReceipt(const SourceReceipt&) = delete;
  SourceReceipt& operator=(const SourceReceipt&) = delete;
  SourceReceipt(SourceReceipt&&) noexcept = default;
  SourceReceipt& operator=(SourceReceipt&&) noexcept = default;
  [[nodiscard]] const project::SourceRecord& record() const noexcept { return record_; }

private:
  friend class ProjectStore;
  SourceReceipt(project::SourceRecord record, std::string store_token)
      : record_(std::move(record)), store_token_(std::move(store_token)) {}
  project::SourceRecord record_;
  std::string store_token_;
};

// Worker-owned preparation capability. It never owns the project writer lock
// and cannot publish an asset or a project generation. Destroying an unfinished
// task or a prepared result removes its private staging file.
class StagedWav;
class WavStageTask {
public:
  WavStageTask(WavStageTask&&) noexcept;
  WavStageTask& operator=(WavStageTask&&) noexcept;
  WavStageTask(const WavStageTask&) = delete;
  WavStageTask& operator=(const WavStageTask&) = delete;
  ~WavStageTask();

  [[nodiscard]] foundation::Result<StagedWav> run(const std::stop_token& stop = {}) &&;

private:
  friend class ProjectStore;
  struct Impl;
  explicit WavStageTask(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

class StagedWav {
public:
  StagedWav(StagedWav&&) noexcept;
  StagedWav& operator=(StagedWav&&) noexcept;
  StagedWav(const StagedWav&) = delete;
  StagedWav& operator=(const StagedWav&) = delete;
  ~StagedWav();

  [[nodiscard]] const project::SourceRecord& record() const noexcept;

private:
  friend class WavStageTask;
  friend class ProjectStore;
  struct Impl;
  explicit StagedWav(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

// The encoded worker path shares the same staged-file and receipt lifecycle.
// These aliases preserve the original WAV API while the worker validates an
// explicitly detected AIFF, FLAC or MP3 source in the optional codec build.
using EncodedStageTask = WavStageTask;
using StagedEncoded = StagedWav;

// A worker verifies an exact external source without holding the project
// writer. The result is bound to this store instance and a file version;
// publication still checks the named path immediately before CURRENT changes.
class VerifiedExternalSource;
class ExternalSourceVerifyTask {
public:
  ExternalSourceVerifyTask(ExternalSourceVerifyTask&&) noexcept = default;
  ExternalSourceVerifyTask& operator=(ExternalSourceVerifyTask&&) noexcept = default;
  ExternalSourceVerifyTask(const ExternalSourceVerifyTask&) = delete;
  ExternalSourceVerifyTask& operator=(const ExternalSourceVerifyTask&) = delete;
  [[nodiscard]] foundation::Result<VerifiedExternalSource> run(const std::stop_token& stop = {}) &&;

private:
  friend class ProjectStore;
  ExternalSourceVerifyTask(project::SourceRecord record, std::uint64_t revision,
                           std::string store_token)
      : record_(std::move(record)), revision_(revision), store_token_(std::move(store_token)) {}
  project::SourceRecord record_;
  std::uint64_t revision_;
  std::string store_token_;
};

class VerifiedExternalSource {
public:
  VerifiedExternalSource(VerifiedExternalSource&&) noexcept;
  VerifiedExternalSource& operator=(VerifiedExternalSource&&) noexcept;
  VerifiedExternalSource(const VerifiedExternalSource&) = delete;
  VerifiedExternalSource& operator=(const VerifiedExternalSource&) = delete;
  ~VerifiedExternalSource();

  [[nodiscard]] const project::SourceRecord& record() const noexcept;
  [[nodiscard]] std::uint64_t revision() const noexcept;

private:
  friend class ExternalSourceVerifyTask;
  friend class ProjectStore;
  struct Impl;
  explicit VerifiedExternalSource(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

// The hook simulates an interruption after a completed stage. The
// state_flush_failed checkpoint specifically simulates a failed final fsync.
// It is intended for deterministic fault-injection tests, never the callback.
using FaultHook = std::function<bool(SaveStage)>;

class ProjectStore {
public:
  ProjectStore(const ProjectStore&) = delete;
  ProjectStore& operator=(const ProjectStore&) = delete;
  ProjectStore(ProjectStore&& other) noexcept;
  ProjectStore& operator=(ProjectStore&& other) noexcept;
  ~ProjectStore();

  [[nodiscard]] static foundation::Result<ProjectStore>
  open_writer(const std::filesystem::path& root, bool create, FaultHook fault = {});
  [[nodiscard]] static foundation::Result<ProjectStore>
  open_reader(const std::filesystem::path& root);

  // Duplicate the held writer capability for one background autosave. Both
  // handles share a non-blocking save gate; an explicit save in progress is
  // reported as busy rather than blocking the UI thread.
  [[nodiscard]] foundation::Result<ProjectStore> fork_autosave_writer() const;
  // A worker capability for publishing an explicit CURRENT generation. It
  // shares the same non-blocking save gate as the owning session.
  [[nodiscard]] foundation::Result<ProjectStore> fork_explicit_save_writer() const;

  [[nodiscard]] foundation::Result<SaveOutcome>
  save(const project::Snapshot& snapshot, SaveKind kind,
       std::span<const SourceReceipt> receipts = {},
       const VerifiedExternalSource* external = nullptr);
  [[nodiscard]] foundation::Result<ExternalSourceVerifyTask>
  begin_external_source_verification(const project::SourceRecord& source,
                                     std::uint64_t candidate_revision) const;
  // The caller owns input_fd. It must refer to a pinned readable regular WAV.
  [[nodiscard]] foundation::Result<SourceReceipt> stage_wav(project::SourceId id, int input_fd);
  // Duplicate a pinned input descriptor and directory capabilities on the
  // control thread. run() may then copy/verify the WAV on one worker without
  // accessing this store or the project model. Only this writer may publish it.
  [[nodiscard]] foundation::Result<WavStageTask> begin_wav_stage(project::SourceId id,
                                                                 int input_fd) const;
  [[nodiscard]] foundation::Result<SourceReceipt> publish_wav_stage(StagedWav&& staged);
  // Optional codec build only. The worker copies original encoded bytes to a
  // private project asset and validates the detected format and metadata.
  // The default build returns unsupported before staging any bytes.
  [[nodiscard]] foundation::Result<EncodedStageTask> begin_encoded_stage(project::SourceId id,
                                                                         int input_fd) const;
  [[nodiscard]] foundation::Result<SourceReceipt> publish_encoded_stage(StagedEncoded&& staged);
  // Returns a caller-owned pinned fd after full integrity and source-format
  // metadata verification. Control/worker thread only; caller must close it.
  [[nodiscard]] foundation::Result<int>
  open_managed_source(const project::SourceRecord& source) const;
  // Worker-only. External links require the exact saved CURRENT record and
  // full encoded/decoded verification before a pinned fd is returned.
  [[nodiscard]] foundation::Result<int>
  open_verified_source(const project::SourceRecord& source, const std::stop_token& stop = {}) const;
  // Worker-only. Verify that a saved generation references this exact source
  // before opening its managed asset or external link for a history copy.
  [[nodiscard]] foundation::Result<int>
  open_verified_generation_source(std::string_view generation, const project::SourceRecord& source,
                                  const std::stop_token& stop = {}) const;
  // Repair a missing managed asset referenced by the selected validated
  // generation. The caller owns a pinned regular input fd. Run off the UI and
  // audio threads; copying is bounded by the persisted length and cancellable.
  // Existing assets are never replaced. After-link durability faults return
  // uncertain only if the published bytes still verify in this process.
  [[nodiscard]] foundation::Result<RestoreStatus>
  restore_managed_source(std::string_view generation, const project::SourceRecord& expected,
                         int candidate_fd, const std::stop_token& stop = {});
  // Caller owns the returned pinned directory fd. It is confined to the
  // project's disposable cache/ directory and can be passed to CacheStore.
  [[nodiscard]] foundation::Result<int> open_cache_directory() const;
  [[nodiscard]] foundation::Result<LoadedGeneration> load_current() const;
  [[nodiscard]] foundation::Result<LoadedGeneration> load_autosave() const;
  [[nodiscard]] foundation::Result<std::vector<LoadedGeneration>> recovery_candidates() const;
  // Explicit recovery choice. Revalidate the complete generation and replace
  // only the matching pointer kind through a durable temporary pointer.
  [[nodiscard]] foundation::Result<SaveOutcome> adopt_generation(std::string_view generation,
                                                                 SaveKind pointer_kind);
  // Writer-side retention keeps the ten newest complete generations and all
  // CURRENT, AUTOSAVE and named-revision targets. Control/worker thread only.
  [[nodiscard]] foundation::Result<std::size_t> prune_generations();
  // Names are bounded ASCII display labels; they never become path segments.
  // A revision points to the validated generation currently named by CURRENT.
  [[nodiscard]] foundation::Result<SaveOutcome> name_current_revision(std::string_view name);
  [[nodiscard]] foundation::Result<NamedRevisionListing> list_named_revisions() const;
  [[nodiscard]] foundation::Result<LoadedGeneration>
  load_named_revision(std::string_view name) const;

private:
  struct Descriptors {
    int root;
    int assets;
    int state;
    int generations;
    int lock;
  };
  ProjectStore(Descriptors descriptors, FaultHook fault)
      : root_fd_(descriptors.root), assets_fd_(descriptors.assets), state_fd_(descriptors.state),
        generations_fd_(descriptors.generations), lock_fd_(descriptors.lock),
        fault_(std::move(fault)) {}
  [[nodiscard]] foundation::Result<LoadedGeneration> load_pointer(const char* name) const;
  [[nodiscard]] foundation::Result<LoadedGeneration>
  load_generation(const std::string& generation, const std::optional<std::string>& expected_hash,
                  std::size_t* remaining_budget = nullptr) const;
  [[nodiscard]] foundation::Result<std::size_t> prune_generations_unlocked();
  [[nodiscard]] foundation::Result<SourceReceipt>
  publish_encoded_stage_unlocked(StagedEncoded&& staged);
  [[nodiscard]] bool fail(SaveStage stage) const { return fault_ && fault_(stage); }

  int root_fd_ = -1;
  int assets_fd_ = -1;
  int state_fd_ = -1;
  int generations_fd_ = -1;
  int lock_fd_ = -1;
  FaultHook fault_;
  std::string store_token_;
  std::shared_ptr<std::mutex> save_gate_ = std::make_shared<std::mutex>();
  bool autosave_only_ = false;
  bool explicit_save_only_ = false;
};

} // namespace oscillate::persistence
