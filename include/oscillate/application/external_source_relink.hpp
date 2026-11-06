#pragma once

#include "oscillate/persistence/store.hpp"

#include <cstdint>
#include <stop_token>
#include <utility>

namespace oscillate::application {

class ImportService;
class PreparedExternalSourceRelink;

// Control-side capture, worker-side verification, then serialized finish.
class ExternalSourceRelinkTask {
public:
  ExternalSourceRelinkTask(ExternalSourceRelinkTask&&) noexcept = default;
  ExternalSourceRelinkTask& operator=(ExternalSourceRelinkTask&&) noexcept = default;
  ExternalSourceRelinkTask(const ExternalSourceRelinkTask&) = delete;
  ExternalSourceRelinkTask& operator=(const ExternalSourceRelinkTask&) = delete;

  [[nodiscard]] foundation::Result<PreparedExternalSourceRelink>
  run(const std::stop_token& stop = {}) &&;

private:
  friend class ImportService;
  ExternalSourceRelinkTask(persistence::ExternalSourceVerifyTask verification,
                           project::SourceRecord original, std::uint64_t origin_revision,
                           bool same_path)
      : verification_(std::move(verification)), original_(std::move(original)),
        origin_revision_(origin_revision), same_path_(same_path) {}

  persistence::ExternalSourceVerifyTask verification_;
  project::SourceRecord original_;
  std::uint64_t origin_revision_;
  bool same_path_;
};

class PreparedExternalSourceRelink {
public:
  PreparedExternalSourceRelink(PreparedExternalSourceRelink&&) noexcept = default;
  PreparedExternalSourceRelink& operator=(PreparedExternalSourceRelink&&) noexcept = default;
  PreparedExternalSourceRelink(const PreparedExternalSourceRelink&) = delete;
  PreparedExternalSourceRelink& operator=(const PreparedExternalSourceRelink&) = delete;

  [[nodiscard]] project::SourceId source_id() const noexcept { return original_.id; }
  [[nodiscard]] std::uint64_t origin_revision() const noexcept { return origin_revision_; }

private:
  friend class ExternalSourceRelinkTask;
  friend class ImportService;
  PreparedExternalSourceRelink(persistence::VerifiedExternalSource proof,
                               project::SourceRecord original, std::uint64_t origin_revision,
                               bool same_path)
      : proof_(std::move(proof)), original_(std::move(original)), origin_revision_(origin_revision),
        same_path_(same_path) {}

  persistence::VerifiedExternalSource proof_;
  project::SourceRecord original_;
  std::uint64_t origin_revision_;
  bool same_path_;
};

inline foundation::Result<PreparedExternalSourceRelink>
ExternalSourceRelinkTask::run(const std::stop_token& stop) && {
  auto verified = std::move(verification_).run(stop);
  if (!verified)
    return foundation::Result<PreparedExternalSourceRelink>::failure(verified.error());
  return foundation::Result<PreparedExternalSourceRelink>::success(PreparedExternalSourceRelink(
      std::move(verified).value(), std::move(original_), origin_revision_, same_path_));
}

} // namespace oscillate::application
