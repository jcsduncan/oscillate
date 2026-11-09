#include "oscillate/persistence/store.hpp"

#include "store_support.hpp"

#include "private.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

// Generation enumeration: loading generations and pointers, recovery
// candidates, adopting a generation and pruning the rest.
namespace oscillate::persistence {
namespace {
using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;
using detail::check_directory_identity;
using detail::check_writer_lock;
using detail::checked_asset;
using detail::checked_external_source;
using detail::checksum_file_bytes;
using detail::decoded_footprint;
using detail::digest;
using detail::Fd;
using detail::hex_name;
using detail::injected;
using detail::max_pointer_bytes;
using detail::max_recovery_candidates;
using detail::max_recovery_decoded_bytes;
using detail::max_recovery_entries;
using detail::max_recovery_manifest_bytes;
using detail::open_directory;
using detail::random_name;
using detail::read_bounded;
using detail::retained_recent_generations;
using detail::sha256_hex_digits;
using detail::write_sync_file;
using foundation::io_error;

struct GenerationEntry {
  std::string name;
  timespec modified;
  dev_t device;
  ino_t inode;
};

[[nodiscard]] bool newer_generation(const GenerationEntry& left,
                                    const GenerationEntry& right) noexcept {
  if (left.modified.tv_sec != right.modified.tv_sec)
    return left.modified.tv_sec > right.modified.tv_sec;
  if (left.modified.tv_nsec != right.modified.tv_nsec)
    return left.modified.tv_nsec > right.modified.tv_nsec;
  return left.name > right.name;
}

} // namespace

Result<LoadedGeneration>
ProjectStore::load_generation(const std::string& generation,
                              const std::optional<std::string>& expected_hash,
                              std::size_t* remaining_budget) const {
  if (!hex_name(generation)) {
    return Result<LoadedGeneration>::failure({ErrorCode::corrupt_data, "invalid generation path"});
  }
  auto directory = open_directory(generations_fd_, generation.c_str());
  if (!directory)
    return Result<LoadedGeneration>::failure(directory.error());
  auto bytes = read_bounded(directory.value().get(), "manifest.json", detail::max_manifest_bytes,
                            remaining_budget);
  if (!bytes)
    return Result<LoadedGeneration>::failure(bytes.error());
  auto recorded = read_bounded(directory.value().get(), "manifest.sha256", checksum_file_bytes);
  if (!recorded)
    return Result<LoadedGeneration>::failure(recorded.error());
  auto local_hash = digest(bytes.value());
  if (!local_hash)
    return Result<LoadedGeneration>::failure(local_hash.error());
  if (recorded.value() != local_hash.value() + "\n") {
    return Result<LoadedGeneration>::failure(
        {ErrorCode::corrupt_data, "generation checksum mismatch"});
  }
  if (expected_hash) {
    if (local_hash.value() != *expected_hash) {
      return Result<LoadedGeneration>::failure(
          {ErrorCode::corrupt_data, "manifest checksum mismatch"});
    }
  }
  auto parsed = detail::decode_manifest(bytes.value());
  if (!parsed)
    return parsed;
  if (parsed.value().generation != generation) {
    return Result<LoadedGeneration>::failure(
        {ErrorCode::corrupt_data, "generation identity mismatch"});
  }
#if !OSCILLATE_HAVE_CODEC
  if (std::ranges::any_of(parsed.value().snapshot.sources, [](const auto& source) {
        return source.encoding != project::SourceEncoding::wav;
      })) {
    return Result<LoadedGeneration>::failure(
        {ErrorCode::unsupported, "project contains encoded sources but codec is unavailable"});
  }
#endif
  for (const auto& source : parsed.value().snapshot.sources) {
    auto asset =
        source.external_link ? checked_external_source(source) : checked_asset(assets_fd_, source);
    if (!asset) {
      const auto status = asset.error().code == ErrorCode::not_found
                              ? LoadedGeneration::SourceDiagnostic::Status::missing
                              : LoadedGeneration::SourceDiagnostic::Status::damaged;
      parsed.value().source_diagnostics.push_back({source.id, status});
    }
  }
  std::set<project::SourceId> recorded_sources;
  for (const auto& source : parsed.value().snapshot.sources)
    recorded_sources.insert(source.id);
  std::set<project::SourceId> diagnosed_sources;
  for (const auto& item : parsed.value().source_diagnostics)
    diagnosed_sources.insert(item.id);
  for (const auto& track : parsed.value().snapshot.tracks) {
    for (const auto& clip : track.clips) {
      if (!recorded_sources.contains(clip.source) && diagnosed_sources.insert(clip.source).second) {
        parsed.value().source_diagnostics.push_back(
            {clip.source, LoadedGeneration::SourceDiagnostic::Status::unresolved});
      }
    }
  }
  return parsed;
}

Result<LoadedGeneration> ProjectStore::load_pointer(const char* name) const {
  auto bytes = read_bounded(state_fd_, name, max_pointer_bytes);
  if (!bytes)
    return Result<LoadedGeneration>::failure(bytes.error());
  auto pointer = detail::decode_pointer(bytes.value());
  if (!pointer)
    return Result<LoadedGeneration>::failure(pointer.error());
  auto loaded = load_generation(pointer.value().first, pointer.value().second);
  if (!loaded && loaded.error().code == ErrorCode::not_found) {
    return Result<LoadedGeneration>::failure(
        {ErrorCode::corrupt_data, "pointer target is incomplete"});
  }
  if (loaded) {
    const auto expected =
        std::string_view(name) == "CURRENT" ? SaveKind::explicit_save : SaveKind::autosave;
    if (loaded.value().kind != expected) {
      return Result<LoadedGeneration>::failure(
          {ErrorCode::corrupt_data, "pointer references wrong save kind"});
    }
  }
  return loaded;
}

Result<LoadedGeneration> ProjectStore::load_current() const { return load_pointer("CURRENT"); }
Result<LoadedGeneration> ProjectStore::load_autosave() const { return load_pointer("AUTOSAVE"); }

Result<std::vector<LoadedGeneration>> ProjectStore::recovery_candidates() const {
  Fd enumeration_fd(
      ::openat(state_fd_, "generations", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (!enumeration_fd)
    return Result<std::vector<LoadedGeneration>>::failure(io_error("list generations"));
  const int raw_enumeration_fd = enumeration_fd.release();
  DIR* stream = ::fdopendir(raw_enumeration_fd);
  if (!stream) {
    const auto failure = io_error("list generations");
    ::close(raw_enumeration_fd);
    return Result<std::vector<LoadedGeneration>>::failure(failure);
  }
  std::vector<GenerationEntry> entries;
  std::size_t scanned = 0;
  errno = 0;
  while (const auto* entry = ::readdir(stream)) {
    if (++scanned > max_recovery_entries) {
      ::closedir(stream);
      return Result<std::vector<LoadedGeneration>>::failure(
          {ErrorCode::resource_exhausted, "too many recovery entries"});
    }
    if (!hex_name(entry->d_name))
      continue;
    struct stat status{};
    if (::fstatat(raw_enumeration_fd, entry->d_name, &status, AT_SYMLINK_NOFOLLOW) != 0) {
      const auto failure = io_error("stat recovery generation");
      ::closedir(stream);
      return Result<std::vector<LoadedGeneration>>::failure(failure);
    }
    if (S_ISDIR(status.st_mode))
      entries.push_back({entry->d_name, status.st_mtim, status.st_dev, status.st_ino});
    errno = 0;
  }
  const int read_error = errno;
  ::closedir(stream);
  if (read_error != 0) {
    errno = read_error;
    return Result<std::vector<LoadedGeneration>>::failure(io_error("list generations"));
  }
  std::ranges::sort(entries, newer_generation);
  std::vector<LoadedGeneration> candidates;
  std::size_t remaining_bytes = max_recovery_manifest_bytes;
  std::size_t remaining_decoded = max_recovery_decoded_bytes;
  for (const auto& entry : entries) {
    if (candidates.size() == max_recovery_candidates)
      break;
    auto candidate = load_generation(entry.name, std::nullopt, &remaining_bytes);
    if (!candidate && candidate.error().code == ErrorCode::resource_exhausted) {
      return Result<std::vector<LoadedGeneration>>::failure(candidate.error());
    }
    if (candidate) {
      const auto retained = decoded_footprint(candidate.value());
      if (retained > remaining_decoded) {
        return Result<std::vector<LoadedGeneration>>::failure(
            {ErrorCode::resource_exhausted, "recovery decoded-data budget exhausted"});
      }
      remaining_decoded -= retained;
      candidates.push_back(std::move(candidate).value());
    }
  }
  std::ranges::reverse(candidates);
  return Result<std::vector<LoadedGeneration>>::success(std::move(candidates));
}

Result<SaveOutcome> ProjectStore::adopt_generation(std::string_view generation,
                                                   SaveKind pointer_kind) {
  std::unique_lock save_lock(*save_gate_, std::try_to_lock);
  if (!save_lock.owns_lock())
    return Result<SaveOutcome>::failure({ErrorCode::conflict, "project save already in progress"});
  if (lock_fd_ < 0)
    return Result<SaveOutcome>::failure({ErrorCode::conflict, "store is read-only"});
  if (!hex_name(generation))
    return Result<SaveOutcome>::failure({ErrorCode::invalid_argument, "invalid generation ID"});
  if (auto checked = check_writer_lock({state_fd_, lock_fd_}); !checked)
    return Result<SaveOutcome>::failure(checked.error());
  if (auto checked = check_directory_identity(root_fd_, "state", state_fd_); !checked)
    return Result<SaveOutcome>::failure(checked.error());
  if (auto checked = check_directory_identity(state_fd_, "generations", generations_fd_); !checked)
    return Result<SaveOutcome>::failure(checked.error());

  const std::string name(generation);
  auto selected = load_generation(name, std::nullopt);
  if (!selected)
    return Result<SaveOutcome>::failure(selected.error());
  if (selected.value().kind != pointer_kind)
    return Result<SaveOutcome>::failure({ErrorCode::conflict, "generation has wrong save kind"});
  const char* pointer = pointer_kind == SaveKind::explicit_save ? "CURRENT" : "AUTOSAVE";
  const char* other_pointer = pointer_kind == SaveKind::explicit_save ? "AUTOSAVE" : "CURRENT";
  for (const char* existing_pointer : {pointer, other_pointer}) {
    auto existing = load_pointer(existing_pointer);
    if (existing && existing.value().snapshot.id != selected.value().snapshot.id)
      return Result<SaveOutcome>::failure({ErrorCode::conflict, "project identity differs"});
    if (!existing && existing.error().code != ErrorCode::not_found &&
        existing.error().code != ErrorCode::corrupt_data)
      return Result<SaveOutcome>::failure(existing.error());
  }

  auto directory = open_directory(generations_fd_, name.c_str());
  if (!directory)
    return Result<SaveOutcome>::failure(directory.error());
  auto recorded = read_bounded(directory.value().get(), "manifest.sha256", checksum_file_bytes);
  if (!recorded)
    return Result<SaveOutcome>::failure(recorded.error());
  if (recorded.value().size() != checksum_file_bytes || recorded.value().back() != '\n')
    return Result<SaveOutcome>::failure({ErrorCode::corrupt_data, "invalid generation checksum"});
  const auto hash = recorded.value().substr(0, sha256_hex_digits);
  auto verified = load_generation(name, hash);
  if (!verified)
    return Result<SaveOutcome>::failure(verified.error());
  if (verified.value().kind != pointer_kind ||
      verified.value().snapshot.id != selected.value().snapshot.id)
    return Result<SaveOutcome>::failure(
        {ErrorCode::conflict, "generation changed during recovery"});

  auto random = random_name();
  if (!random)
    return Result<SaveOutcome>::failure(random.error());
  const std::string temporary = std::string(".") + pointer + "-" + random.value();
  auto written = write_sync_file(state_fd_, temporary, detail::encode_pointer(name, hash));
  if (!written)
    return Result<SaveOutcome>::failure({ErrorCode::io_failure, written.error().message});
  if (fail(SaveStage::pointer_written))
    return Result<SaveOutcome>::failure({ErrorCode::io_failure, "injected recovery interruption"});
  if (auto checked = check_writer_lock({state_fd_, lock_fd_}); !checked)
    return Result<SaveOutcome>::failure(checked.error());
  if (auto checked = check_directory_identity(root_fd_, "state", state_fd_); !checked)
    return Result<SaveOutcome>::failure(checked.error());
  if (auto checked = check_directory_identity(state_fd_, "generations", generations_fd_); !checked)
    return Result<SaveOutcome>::failure(checked.error());
  if (::renameat(state_fd_, temporary.c_str(), state_fd_, pointer) != 0)
    return Result<SaveOutcome>::failure(io_error("adopt recovery generation"));
  if (fail(SaveStage::pointer_published) || fail(SaveStage::state_flush_failed))
    return Result<SaveOutcome>::success({SaveStatus::uncertain, name});
  if (::fsync(state_fd_) != 0)
    return Result<SaveOutcome>::success({SaveStatus::uncertain, name});
  return Result<SaveOutcome>::success({SaveStatus::committed, name});
}

Result<std::size_t> ProjectStore::prune_generations() {
  std::unique_lock save_lock(*save_gate_, std::try_to_lock);
  if (!save_lock.owns_lock())
    return Result<std::size_t>::failure({ErrorCode::conflict, "project save already in progress"});
  return prune_generations_unlocked();
}

Result<std::size_t> ProjectStore::prune_generations_unlocked() {
  if (lock_fd_ < 0)
    return Result<std::size_t>::failure({ErrorCode::conflict, "store is read-only"});
  if (auto checked = check_writer_lock({state_fd_, lock_fd_}); !checked)
    return Result<std::size_t>::failure(checked.error());
  if (auto checked = check_directory_identity(root_fd_, "state", state_fd_); !checked)
    return Result<std::size_t>::failure(checked.error());
  if (auto checked = check_directory_identity(state_fd_, "generations", generations_fd_); !checked)
    return Result<std::size_t>::failure(checked.error());

  std::set<std::string> protected_names;
  std::optional<project::ProjectId> active_project;
  for (const char* pointer : {"CURRENT", "AUTOSAVE"}) {
    auto loaded = load_pointer(pointer);
    if (loaded) {
      protected_names.insert(loaded.value().generation);
      if (!active_project)
        active_project = loaded.value().snapshot.id;
      else if (*active_project != loaded.value().snapshot.id)
        return Result<std::size_t>::failure(
            {ErrorCode::conflict, "project pointers disagree during pruning"});
    } else if (loaded.error().code != ErrorCode::not_found)
      return Result<std::size_t>::failure(loaded.error());
  }
  auto named = list_named_revisions();
  if (!named)
    return Result<std::size_t>::failure(named.error());
  if (!named.value().issues.empty())
    return Result<std::size_t>::failure(
        {ErrorCode::conflict, "named revision directory needs repair before pruning"});
  for (const auto& revision : named.value().revisions)
    protected_names.insert(revision.generation);

  Fd enumeration_fd(
      ::openat(state_fd_, "generations", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (!enumeration_fd)
    return Result<std::size_t>::failure(io_error("list generations for retention"));
  const int raw_enumeration_fd = enumeration_fd.release();
  DIR* stream = ::fdopendir(raw_enumeration_fd);
  if (!stream) {
    const auto failure = io_error("list generations for retention");
    ::close(raw_enumeration_fd);
    return Result<std::size_t>::failure(failure);
  }
  std::vector<GenerationEntry> entries;
  std::size_t scanned = 0;
  errno = 0;
  while (const auto* entry = ::readdir(stream)) {
    if (++scanned > max_recovery_entries) {
      ::closedir(stream);
      return Result<std::size_t>::failure(
          {ErrorCode::resource_exhausted, "too many generation entries to prune"});
    }
    if (!hex_name(entry->d_name))
      continue;
    struct stat status{};
    if (::fstatat(raw_enumeration_fd, entry->d_name, &status, AT_SYMLINK_NOFOLLOW) != 0) {
      const auto failure = io_error("stat retained generation");
      ::closedir(stream);
      return Result<std::size_t>::failure(failure);
    }
    if (S_ISDIR(status.st_mode))
      entries.push_back({entry->d_name, status.st_mtim, status.st_dev, status.st_ino});
    errno = 0;
  }
  const int read_error = errno;
  ::closedir(stream);
  if (read_error != 0) {
    errno = read_error;
    return Result<std::size_t>::failure(io_error("list generations for retention"));
  }
  std::ranges::sort(entries, newer_generation);
  std::size_t valid = 0;
  std::size_t removed = 0;
  std::size_t remaining_manifest_bytes = max_recovery_manifest_bytes;
  for (const auto& entry : entries) {
    auto loaded = load_generation(entry.name, std::nullopt, &remaining_manifest_bytes);
    if (!loaded) {
      if (loaded.error().code == ErrorCode::resource_exhausted ||
          loaded.error().code == ErrorCode::unsupported)
        return Result<std::size_t>::failure(loaded.error());
      continue; // Never delete an incomplete or unverifiable generation.
    }
    if (!active_project || loaded.value().snapshot.id != *active_project)
      continue;
    if (++valid <= retained_recent_generations || protected_names.contains(entry.name))
      continue;
    auto directory = open_directory(generations_fd_, entry.name.c_str());
    if (!directory)
      return Result<std::size_t>::failure(directory.error());
    struct stat pinned{};
    if (::fstat(directory.value().get(), &pinned) != 0)
      return Result<std::size_t>::failure(io_error("stat generation selected for pruning"));
    if (pinned.st_dev != entry.device || pinned.st_ino != entry.inode)
      return Result<std::size_t>::failure(
          {ErrorCode::conflict, "generation changed during pruning"});
    Fd listing_fd(::dup(directory.value().get()));
    if (!listing_fd)
      return Result<std::size_t>::failure(io_error("inspect generation for pruning"));
    const int raw_listing_fd = listing_fd.release();
    DIR* contents = ::fdopendir(raw_listing_fd);
    if (!contents) {
      const auto failure = io_error("inspect generation for pruning");
      ::close(raw_listing_fd);
      return Result<std::size_t>::failure(failure);
    }
    bool ordinary_contents = true;
    errno = 0;
    while (const auto* item = ::readdir(contents)) {
      const std::string_view name(item->d_name);
      if (name != "." && name != ".." && name != "manifest.json" && name != "manifest.sha256")
        ordinary_contents = false;
      errno = 0;
    }
    const int contents_error = errno;
    ::closedir(contents);
    if (contents_error != 0) {
      errno = contents_error;
      return Result<std::size_t>::failure(io_error("inspect generation for pruning"));
    }
    if (!ordinary_contents)
      continue;
    if (auto checked = check_directory_identity(state_fd_, "generations", generations_fd_);
        !checked)
      return Result<std::size_t>::failure(checked.error());
    struct stat named_status{};
    if (::fstatat(generations_fd_, entry.name.c_str(), &named_status, AT_SYMLINK_NOFOLLOW) != 0)
      return Result<std::size_t>::failure(io_error("recheck generation for pruning"));
    if (named_status.st_dev != entry.device || named_status.st_ino != entry.inode)
      return Result<std::size_t>::failure(
          {ErrorCode::conflict, "generation name changed during pruning"});
    if (::unlinkat(directory.value().get(), "manifest.json", 0) != 0 ||
        ::unlinkat(directory.value().get(), "manifest.sha256", 0) != 0 ||
        ::unlinkat(generations_fd_, entry.name.c_str(), AT_REMOVEDIR) != 0)
      return Result<std::size_t>::failure(io_error("prune old generation"));
    ++removed;
  }
  if (removed != 0 && ::fsync(generations_fd_) != 0)
    return Result<std::size_t>::failure(io_error("flush generation pruning"));
  return Result<std::size_t>::success(removed);
}

} // namespace oscillate::persistence
