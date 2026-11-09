#include "oscillate/persistence/store.hpp"

#include "store_support.hpp"

#include "private.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

// Saving a snapshot as a new generation and moving a pointer to it.
namespace oscillate::persistence {
namespace {
using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;
using detail::check_assets_directory;
using detail::check_directory_identity;
using detail::check_writer_lock;
using detail::checked_asset;
using detail::checked_external_source;
using detail::digest;
using detail::injected;
using detail::open_directory;
using detail::random_name;
using detail::read_bounded;
using detail::same_file_version;
using detail::verify_new_external_source;
using detail::write_sync_file;
using foundation::io_error;

Result<void> flush_directory(int fd) { return foundation::sync(fd, "flush project directory"); }

} // namespace

Result<SaveOutcome> ProjectStore::save(const project::Snapshot& snapshot, SaveKind kind,
                                       std::span<const SourceReceipt> receipts,
                                       const VerifiedExternalSource* external) {
  if (autosave_only_ && kind != SaveKind::autosave)
    return Result<SaveOutcome>::failure(
        {ErrorCode::conflict, "background writer can only autosave"});
  if (explicit_save_only_ && kind != SaveKind::explicit_save)
    return Result<SaveOutcome>::failure(
        {ErrorCode::conflict, "background writer can only publish CURRENT"});
  std::unique_lock save_lock(*save_gate_, std::try_to_lock);
  if (!save_lock.owns_lock())
    return Result<SaveOutcome>::failure({ErrorCode::conflict, "project save already in progress"});
  if (lock_fd_ < 0) {
    return Result<SaveOutcome>::failure({ErrorCode::conflict, "store is read-only"});
  }
  if (external && (!external->impl_ || external->impl_->store_token != store_token_ ||
                   external->impl_->revision != snapshot.revision ||
                   !std::ranges::any_of(snapshot.sources, [&](const auto& source) {
                     return source == external->impl_->record;
                   }))) {
    return Result<SaveOutcome>::failure(
        {ErrorCode::conflict, "external verification does not match this save"});
  }
  auto state_identity = check_directory_identity(root_fd_, "state", state_fd_);
  if (!state_identity)
    return Result<SaveOutcome>::failure(state_identity.error());
  auto lock_identity = check_writer_lock({state_fd_, lock_fd_});
  if (!lock_identity)
    return Result<SaveOutcome>::failure(lock_identity.error());
  auto generations_identity = check_directory_identity(state_fd_, "generations", generations_fd_);
  if (!generations_identity)
    return Result<SaveOutcome>::failure(generations_identity.error());
  auto assets_identity = check_assets_directory({root_fd_, assets_fd_});
  if (!assets_identity)
    return Result<SaveOutcome>::failure(assets_identity.error());
  if (snapshot.sources.size() > detail::max_sources ||
      snapshot.source_tempo_reviews.size() > detail::max_sources ||
      snapshot.source_key_reviews.size() > detail::max_sources ||
      snapshot.tracks.size() > detail::max_tracks ||
      snapshot.tempo.events().size() > detail::max_events ||
      snapshot.meter.events().size() > detail::max_events) {
    return Result<SaveOutcome>::failure({ErrorCode::resource_exhausted, "project count limit"});
  }
  for (const auto& source : snapshot.sources) {
    auto valid_source = project::validate_source_record(source);
    if (!valid_source)
      return Result<SaveOutcome>::failure(valid_source.error());
  }
  std::size_t tempo_markers = 0;
  for (const auto& review : snapshot.source_tempo_reviews) {
    const auto count = review.generated_beat_frames.size() + review.user_beat_frames.size() +
                       review.generated_onset_frames.size() + review.dismissed_onset_frames.size();
    if (review.generated_beat_frames.size() > project::max_tempo_markers_per_source ||
        review.user_beat_frames.size() > project::max_tempo_markers_per_source ||
        review.generated_onset_frames.size() > project::max_tempo_markers_per_source ||
        review.dismissed_onset_frames.size() > project::max_tempo_markers_per_source ||
        count > project::max_tempo_markers_per_project - tempo_markers) {
      return Result<SaveOutcome>::failure(
          {ErrorCode::resource_exhausted, "source tempo marker budget exceeded"});
    }
    tempo_markers += count;
  }
  std::size_t clips = 0;
  std::size_t lanes = 0;
  std::size_t points = 0;
  const auto count_lanes = [&](const std::vector<project::AutomationLane>& owned,
                               std::size_t owner_limit) {
    if (owned.size() > owner_limit ||
        owned.size() > project::max_automation_lanes_per_project - lanes) {
      return false;
    }
    lanes += owned.size();
    for (const auto& lane : owned) {
      if (lane.points.size() > project::max_automation_points_per_lane ||
          lane.points.size() > project::max_automation_points_per_project - points) {
        return false;
      }
      points += lane.points.size();
    }
    return true;
  };
  for (const auto& track : snapshot.tracks) {
    if (!count_lanes(track.automation, project::max_automation_lanes_per_track)) {
      return Result<SaveOutcome>::failure({ErrorCode::resource_exhausted, "automation budget"});
    }
    if (track.clips.size() > detail::max_clips - clips) {
      return Result<SaveOutcome>::failure({ErrorCode::resource_exhausted, "project clip limit"});
    }
    clips += track.clips.size();
    for (const auto& clip : track.clips) {
      if (!count_lanes(clip.automation, project::max_automation_lanes_per_clip)) {
        return Result<SaveOutcome>::failure({ErrorCode::resource_exhausted, "automation budget"});
      }
    }
    for (const auto& clip : track.midi_clips) {
      if (!count_lanes(clip.automation, project::max_automation_lanes_per_clip)) {
        return Result<SaveOutcome>::failure({ErrorCode::resource_exhausted, "automation budget"});
      }
    }
  }
  // Reuse the authoritative project validator before writing any durable bytes.
  auto probe =
      detail::encode_manifest(snapshot, "00000000000000000000000000000001", std::nullopt, kind);
  auto valid = detail::decode_manifest(probe);
  if (!valid)
    return Result<SaveOutcome>::failure(valid.error());
  if (probe.size() > detail::max_manifest_bytes) {
    return Result<SaveOutcome>::failure({ErrorCode::resource_exhausted, "manifest too large"});
  }
  std::optional<std::string> parent;
  auto previous = load_current();
  if (previous) {
    if (previous.value().snapshot.id != snapshot.id) {
      return Result<SaveOutcome>::failure(
          {ErrorCode::conflict, "project ID differs from existing save"});
    }
    if (kind == SaveKind::explicit_save && snapshot.revision < previous.value().snapshot.revision) {
      return Result<SaveOutcome>::failure(
          {ErrorCode::conflict, "explicit-save revision would regress"});
    }
    if (kind == SaveKind::autosave && snapshot.revision < previous.value().snapshot.revision) {
      return Result<SaveOutcome>::failure(
          {ErrorCode::conflict, "autosave predates the current explicit save"});
    }
    parent = previous.value().generation;
  } else if (previous.error().code != ErrorCode::not_found) {
    return Result<SaveOutcome>::failure(previous.error());
  }
  auto existing_autosave = load_autosave();
  {
    if (existing_autosave && existing_autosave.value().snapshot.id != snapshot.id) {
      return Result<SaveOutcome>::failure(
          {ErrorCode::conflict, "project ID differs from existing autosave"});
    }
    if (existing_autosave && kind == SaveKind::autosave &&
        snapshot.revision < existing_autosave.value().snapshot.revision) {
      return Result<SaveOutcome>::failure({ErrorCode::conflict, "autosave revision would regress"});
    }
    if (!existing_autosave && existing_autosave.error().code != ErrorCode::not_found) {
      return Result<SaveOutcome>::failure(existing_autosave.error());
    }
  }
  // A newly referenced source must carry a receipt from this writer. A source
  // already present in a committed pointer or named revision is authorized by
  // that generation. Resolve named revisions only when CURRENT/AUTOSAVE do not
  // prove a source, so ordinary saves do not re-read revision history.
  std::multimap<project::SourceId, const project::SourceRecord*> committed_sources;
  if (previous) {
    for (const auto& source : previous.value().snapshot.sources)
      committed_sources.emplace(source.id, &source);
  }
  if (existing_autosave) {
    for (const auto& source : existing_autosave.value().snapshot.sources)
      committed_sources.emplace(source.id, &source);
  }
  bool checked_named_sources = false;
  std::vector<LoadedGeneration> named_generations;
  std::vector<std::pair<const project::SourceRecord*, struct stat>> verified_external_links;
  std::multimap<project::SourceId, const SourceReceipt*> staged_sources;
  for (const auto& receipt : receipts)
    staged_sources.emplace(receipt.record_.id, &receipt);
  for (const auto& source : snapshot.sources) {
#if !OSCILLATE_HAVE_CODEC
    if (source.encoding != project::SourceEncoding::wav)
      return Result<SaveOutcome>::failure(
          {ErrorCode::unsupported, "encoded source staging is not yet available"});
#endif
    const auto [staged_begin, staged_end] = staged_sources.equal_range(source.id);
    const bool authorized = std::ranges::any_of(staged_begin, staged_end, [&](const auto& item) {
      return item.second->store_token_ == store_token_ && item.second->record_ == source;
    });
    if (source.external_link) {
      if (authorized)
        return Result<SaveOutcome>::failure(
            {ErrorCode::invalid_argument, "managed receipt cannot authorize external link"});
      // The active pointer of this save kind, rather than any historical
      // revision, decides whether this is a new link. A return to an old path
      // must verify its bytes again before CURRENT or AUTOSAVE advances.
      const LoadedGeneration* active = nullptr;
      if (kind == SaveKind::autosave && existing_autosave)
        active = &existing_autosave.value();
      else if (previous)
        active = &previous.value();
      const bool unchanged =
          active && std::ranges::any_of(active->snapshot.sources,
                                        [&](const auto& prior) { return prior == source; });
      if (!unchanged) {
        if (external && source == external->impl_->record) {
          verified_external_links.emplace_back(&source, external->impl_->version);
        } else {
          auto verified = verify_new_external_source(source);
          if (!verified)
            return Result<SaveOutcome>::failure(verified.error());
          verified_external_links.emplace_back(&source, verified.value());
        }
      }
      continue;
    }
    const auto [committed_begin, committed_end] = committed_sources.equal_range(source.id);
    bool previously_committed = std::ranges::any_of(
        committed_begin, committed_end, [&](const auto& item) { return *item.second == source; });
    if (!authorized && !previously_committed && !checked_named_sources) {
      auto listing = list_named_revisions();
      if (!listing)
        return Result<SaveOutcome>::failure(listing.error());
      named_generations.reserve(listing.value().revisions.size());
      for (const auto& item : listing.value().revisions) {
        auto loaded = load_generation(item.generation, std::nullopt);
        if (!loaded)
          return Result<SaveOutcome>::failure(loaded.error());
        if (loaded.value().kind != SaveKind::explicit_save)
          return Result<SaveOutcome>::failure(
              {ErrorCode::corrupt_data, "named revision changed during save"});
        if (loaded.value().snapshot.id == snapshot.id)
          named_generations.push_back(std::move(loaded).value());
      }
      for (const auto& generation : named_generations)
        for (const auto& prior_source : generation.snapshot.sources)
          committed_sources.emplace(prior_source.id, &prior_source);
      checked_named_sources = true;
      const auto [named_begin, named_end] = committed_sources.equal_range(source.id);
      previously_committed = std::ranges::any_of(
          named_begin, named_end, [&](const auto& item) { return *item.second == source; });
    }
    if (!authorized && !previously_committed)
      return Result<SaveOutcome>::failure(
          {ErrorCode::conflict, "source lacks a same-store receipt"});
    auto asset = checked_asset(assets_fd_, source);
    if (!asset)
      return Result<SaveOutcome>::failure(asset.error());
  }
  // ADR-0095: the generation a migration opened stays loadable after this
  // save moves CURRENT past it, as a named revision that pruning keeps.
  if (kind == SaveKind::explicit_save && previous && previous.value().migrated_from) {
    if (auto kept = keep_pre_upgrade_generation(); !kept)
      return Result<SaveOutcome>::failure(
          {kept.error().code, "Cannot keep the pre-upgrade version: " + kept.error().message});
  }
  auto generated = random_name();
  if (!generated)
    return Result<SaveOutcome>::failure(generated.error());
  const std::string name = std::move(generated).value();
  const std::string temporary = ".tmp-" + name;
  const std::string manifest = detail::encode_manifest(snapshot, name, parent, kind);
  if (manifest.size() > detail::max_manifest_bytes) {
    return Result<SaveOutcome>::failure({ErrorCode::resource_exhausted, "manifest too large"});
  }
  auto hash = digest(manifest);
  if (!hash)
    return Result<SaveOutcome>::failure(hash.error());
  if (::mkdirat(generations_fd_, temporary.c_str(), 0700) != 0) {
    return Result<SaveOutcome>::failure(io_error("create temporary generation"));
  }
  auto temporary_fd = open_directory(generations_fd_, temporary.c_str());
  if (!temporary_fd)
    return Result<SaveOutcome>::failure(temporary_fd.error());
  auto written = write_sync_file(temporary_fd.value().get(), "manifest.json", manifest);
  if (!written)
    return Result<SaveOutcome>::failure(written.error());
  written = write_sync_file(temporary_fd.value().get(), "manifest.sha256", hash.value() + "\n");
  if (!written)
    return Result<SaveOutcome>::failure(written.error());
  if (auto stage = injected(fail(SaveStage::manifest_written)); !stage) {
    return Result<SaveOutcome>::failure(stage.error());
  }
  auto flushed = flush_directory(temporary_fd.value().get());
  if (!flushed)
    return Result<SaveOutcome>::failure(flushed.error());
  if (auto stage = injected(fail(SaveStage::generation_flushed)); !stage) {
    return Result<SaveOutcome>::failure(stage.error());
  }
  auto on_disk =
      read_bounded(temporary_fd.value().get(), "manifest.json", detail::max_manifest_bytes);
  if (!on_disk)
    return Result<SaveOutcome>::failure(on_disk.error());
  auto staged_checksum = read_bounded(temporary_fd.value().get(), "manifest.sha256", 65);
  if (!staged_checksum)
    return Result<SaveOutcome>::failure(staged_checksum.error());
  auto staged_hash = digest(on_disk.value());
  if (!staged_hash)
    return Result<SaveOutcome>::failure(staged_hash.error());
  if (staged_checksum.value() != staged_hash.value() + "\n" ||
      staged_hash.value() != hash.value()) {
    return Result<SaveOutcome>::failure(
        {ErrorCode::corrupt_data, "staged generation checksum mismatch"});
  }
  auto parsed = detail::decode_manifest(on_disk.value());
  if (!parsed || on_disk.value() != manifest) {
    return Result<SaveOutcome>::failure(
        {ErrorCode::corrupt_data, "staged generation validation failed"});
  }
  if (auto renamed =
          foundation::rename_in(generations_fd_, temporary, name,
                                foundation::PublishMode::no_replace, "publish generation");
      !renamed) {
    return Result<SaveOutcome>::failure(std::move(renamed.error()));
  }
  if (auto stage = injected(fail(SaveStage::generation_published)); !stage) {
    return Result<SaveOutcome>::failure(stage.error());
  }
  flushed = flush_directory(generations_fd_);
  if (!flushed)
    return Result<SaveOutcome>::failure(flushed.error());
  if (auto stage = injected(fail(SaveStage::generations_flushed)); !stage) {
    return Result<SaveOutcome>::failure(stage.error());
  }
  const char* pointer = kind == SaveKind::explicit_save ? "CURRENT" : "AUTOSAVE";
  const std::string pointer_temporary = std::string(".") + pointer + "-" + name;
  written =
      write_sync_file(state_fd_, pointer_temporary, detail::encode_pointer(name, hash.value()));
  if (!written)
    return Result<SaveOutcome>::failure(written.error());
  if (auto stage = injected(fail(SaveStage::pointer_written)); !stage) {
    return Result<SaveOutcome>::failure(stage.error());
  }
  for (const auto& [source, version] : verified_external_links) {
    auto named = checked_external_source(*source);
    struct stat current{};
    if (!named || ::fstat(named.value().get(), &current) != 0 ||
        !same_file_version(version, current)) {
      return Result<SaveOutcome>::failure(
          {ErrorCode::conflict, "external source changed before project publication"});
    }
  }
  state_identity = check_directory_identity(root_fd_, "state", state_fd_);
  if (!state_identity)
    return Result<SaveOutcome>::failure(state_identity.error());
  lock_identity = check_writer_lock({state_fd_, lock_fd_});
  if (!lock_identity)
    return Result<SaveOutcome>::failure(lock_identity.error());
  generations_identity = check_directory_identity(state_fd_, "generations", generations_fd_);
  if (!generations_identity)
    return Result<SaveOutcome>::failure(generations_identity.error());
  if (::renameat(state_fd_, pointer_temporary.c_str(), state_fd_, pointer) != 0) {
    return Result<SaveOutcome>::failure(io_error("publish project pointer"));
  }
  if (fail(SaveStage::pointer_published)) {
    return Result<SaveOutcome>::success({SaveStatus::uncertain, name});
  }
  if (fail(SaveStage::state_flush_failed)) {
    return Result<SaveOutcome>::success({SaveStatus::uncertain, name});
  }
  flushed = flush_directory(state_fd_);
  if (!flushed) {
    return Result<SaveOutcome>::success({SaveStatus::uncertain, name});
  }
  auto retention = prune_generations_unlocked();
  if (!retention)
    return Result<SaveOutcome>::success({SaveStatus::committed, name, retention.error()});
  return Result<SaveOutcome>::success({SaveStatus::committed, name});
}

} // namespace oscillate::persistence
