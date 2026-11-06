#include "oscillate/application/session/media_repair_service.hpp"

#include "edit_support.hpp"
#include "midi_clips.hpp"
#include "oscillate/application/key_cache.hpp"
#include "oscillate/application/tempo_cache.hpp"
#include "oscillate/foundation/file_io.hpp"
#include "session_state.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <fcntl.h>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace oscillate::application {
namespace {

using ChannelKey = project::TrackControlTarget::Key;
using detail::find_midi_clip;
using detail::midi_anchor_tick;

} // namespace

struct ManagedRepairState {
  ManagedRepairState(project::SourceId id, std::string saved_generation,
                     std::uint64_t saved_revision,
                     std::unique_ptr<project::StoppedTransportLease> stopped_lease)
      : source(id), generation(std::move(saved_generation)), revision(saved_revision),
        lease(std::move(stopped_lease)) {}
  std::atomic<bool> done{false};
  project::SourceId source;
  std::string generation;
  std::uint64_t revision = 0;
  std::unique_ptr<project::StoppedTransportLease> lease;
  std::optional<foundation::Result<persistence::RestoreStatus>> result;
};

foundation::Result<std::vector<ManagedMediaIssue>>
MediaRepairService::managed_media_issues() const {
  using Result = foundation::Result<std::vector<ManagedMediaIssue>>;
  if (state_.repair_state)
    return Result::failure(repair_in_progress());
  auto loaded = state_.store.load_current();
  if (!loaded)
    return Result::failure(loaded.error());
  std::vector<ManagedMediaIssue> issues;
  issues.reserve(loaded.value().source_diagnostics.size());
  for (const auto& diagnostic : loaded.value().source_diagnostics) {
    const auto source =
        std::ranges::find_if(loaded.value().snapshot.sources, [&diagnostic](const auto& candidate) {
          return candidate.id == diagnostic.id;
        });
    issues.push_back({diagnostic.id,
                      source == loaded.value().snapshot.sources.end()
                          ? std::nullopt
                          : std::optional<project::SourceRecord>(*source),
                      diagnostic.status});
  }
  return Result::success(std::move(issues));
}

foundation::Result<void> MediaRepairService::begin_managed_source_repair(project::SourceId source,
                                                                         int candidate_fd) {
  using Result = foundation::Result<void>;
  if (state_.repair_state)
    return Result::failure({foundation::ErrorCode::conflict, "Managed-media repair is active"});
  auto stopped = state_.gate->acquire_stopped();
  if (!stopped)
    return Result::failure(stopped.error());
  if (!state_.pending_receipts.empty())
    return Result::failure(
        {foundation::ErrorCode::conflict, "Save pending media before repairing a source"});
  auto current = state_.store.load_current();
  if (!current)
    return Result::failure(current.error());
  if (current.value().snapshot.revision != snapshot().revision)
    return Result::failure(
        {foundation::ErrorCode::conflict, "Save current edits before repairing a source"});
  const auto expected = std::ranges::find_if(
      current.value().snapshot.sources, [source](const auto& item) { return item.id == source; });
  if (expected == current.value().snapshot.sources.end())
    return Result::failure(
        {foundation::ErrorCode::not_found, "Saved CURRENT does not contain this source"});
  if (expected->external_link)
    return Result::failure({foundation::ErrorCode::unsupported,
                            "External source requires relinking, not managed-media repair"});
  const auto missing = std::ranges::find_if(
      current.value().source_diagnostics, [source](const auto& item) { return item.id == source; });
  if (missing == current.value().source_diagnostics.end())
    return Result::failure(
        {foundation::ErrorCode::conflict, "Managed source is already available"});
  if (missing->status != persistence::LoadedGeneration::SourceDiagnostic::Status::missing)
    return Result::failure(
        {foundation::ErrorCode::unsupported,
         "Only missing managed assets can be repaired; damaged assets are not replaced"});
  const int duplicate = candidate_fd >= 0 ? ::fcntl(candidate_fd, F_DUPFD_CLOEXEC, 3) : -1;
  if (duplicate < 0)
    return Result::failure(
        {foundation::ErrorCode::invalid_argument, "Cannot pin replacement file descriptor"});
  foundation::UniqueFd pinned(duplicate);
  auto state = std::make_shared<ManagedRepairState>(
      source, current.value().generation, snapshot().revision, std::move(stopped.value()));
  try {
    repair_worker_ =
        std::jthread([this, state, record = project::SourceRecord(*expected),
                      pinned = std::move(pinned)](const std::stop_token& stop) mutable {
          try {
            state->result.emplace(
                state_.store.restore_managed_source(state->generation, record, pinned.get(), stop));
          } catch (const std::exception& error) {
            state->result.emplace(foundation::Result<persistence::RestoreStatus>::failure(
                {foundation::ErrorCode::internal,
                 std::string("Managed-media repair failed: ") + error.what()}));
          } catch (...) {
            state->result.emplace(foundation::Result<persistence::RestoreStatus>::failure(
                {foundation::ErrorCode::internal, "Managed-media repair failed unexpectedly"}));
          }
          state->done.store(true, std::memory_order_release);
        });
  } catch (const std::exception& error) {
    return Result::failure({foundation::ErrorCode::resource_exhausted,
                            std::string("Cannot start managed-media repair: ") + error.what()});
  }
  state_.repair_state = std::move(state);
  return Result::success();
}

bool MediaRepairService::managed_source_repair_busy() const noexcept {
  return state_.repair_state != nullptr;
}

bool MediaRepairService::managed_source_repair_ready() const noexcept {
  return state_.repair_state && state_.repair_state->done.load(std::memory_order_acquire);
}

void MediaRepairService::cancel_managed_source_repair() noexcept {
  if (repair_worker_.joinable())
    repair_worker_.request_stop();
}

std::optional<foundation::Result<persistence::RestoreStatus>>
MediaRepairService::take_managed_source_repair_result() {
  using Result = foundation::Result<persistence::RestoreStatus>;
  if (!state_.repair_state || !state_.repair_state->done.load(std::memory_order_acquire))
    return std::nullopt;
  if (repair_worker_.joinable())
    repair_worker_.join();
  auto state = std::move(state_.repair_state);
  if (!state->result)
    return Result::failure(
        {foundation::ErrorCode::internal, "Managed-media repair returned no result"});
  auto result = std::move(*state->result);
  if (!result)
    return result;
  auto current = state_.store.load_current();
  if (!current)
    return Result::failure(current.error());
  if (current.value().generation != state->generation || snapshot().revision != state->revision)
    return Result::failure(
        {foundation::ErrorCode::conflict,
         "Repair completed for a prior saved generation; refresh current media status"});
  const bool still_missing =
      std::ranges::any_of(current.value().source_diagnostics,
                          [&state](const auto& item) { return item.id == state->source; });
  if (still_missing)
    return Result::failure(
        {foundation::ErrorCode::unavailable,
         "Repaired source is not available in saved CURRENT; inspect project media"});
  return result;
}

} // namespace oscillate::application
