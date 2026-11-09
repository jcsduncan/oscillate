#include "oscillate/persistence/store.hpp"

#include "store_support.hpp"

#include "private.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

// Named revisions: listing, loading and naming them, and the revision kept
// before a schema upgrade.
namespace oscillate::persistence {
namespace {
using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;
using detail::check_directory_identity;
using detail::check_writer_lock;
using detail::decoded_footprint;
using detail::Fd;
using detail::hex_name;
using detail::injected;
using detail::max_named_revisions;
using detail::max_pointer_bytes;
using detail::max_recovery_decoded_bytes;
using detail::max_recovery_manifest_bytes;
using detail::max_revision_entries;
using detail::max_revision_pointer_bytes;
using detail::open_directory;
using detail::random_name;
using detail::read_bounded;
using detail::write_sync_file;
using foundation::io_error;

} // namespace

Result<NamedRevisionListing> ProjectStore::list_named_revisions() const {
  auto state_identity = check_directory_identity(root_fd_, "state", state_fd_);
  if (!state_identity)
    return Result<NamedRevisionListing>::failure(state_identity.error());
  Fd revision_fd(::openat(state_fd_, "revisions", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (!revision_fd) {
    if (errno == ENOENT)
      return Result<NamedRevisionListing>::success({});
    return Result<NamedRevisionListing>::failure(io_error("open named revisions"));
  }
  const int raw_fd = revision_fd.release();
  struct Directory {
    DIR* value;
    ~Directory() {
      if (value)
        ::closedir(value);
    }
  } stream{::fdopendir(raw_fd)};
  if (!stream.value) {
    ::close(raw_fd);
    return Result<NamedRevisionListing>::failure(io_error("list named revisions"));
  }
  NamedRevisionListing listing;
  std::map<std::string, std::vector<NamedRevision>> candidates;
  std::size_t scanned = 0;
  std::size_t valid = 0;
  std::size_t remaining_manifest_bytes = max_recovery_manifest_bytes;
  std::size_t remaining_decoded_bytes = max_recovery_decoded_bytes;
  errno = 0;
  while (const auto* entry = ::readdir(stream.value)) {
    const std::string_view id(entry->d_name);
    if (id == "." || id == "..")
      continue;
    if (++scanned > max_revision_entries)
      return Result<NamedRevisionListing>::failure(
          {ErrorCode::resource_exhausted, "too many named revision entries"});
    if (id.starts_with(".tmp-") && hex_name(id.substr(5))) {
      listing.issues.push_back(
          {std::string(id), std::nullopt, NamedRevisionIssue::Kind::incomplete});
      errno = 0;
      continue;
    }
    if (!hex_name(id)) {
      listing.issues.push_back(
          {std::string(id), std::nullopt, NamedRevisionIssue::Kind::malformed});
      errno = 0;
      continue;
    }
    auto bytes = read_bounded(::dirfd(stream.value), entry->d_name, max_revision_pointer_bytes);
    if (!bytes) {
      listing.issues.push_back(
          {std::string(id), std::nullopt, NamedRevisionIssue::Kind::malformed});
      errno = 0;
      continue;
    }
    auto pointer = detail::decode_named_pointer(bytes.value());
    if (!pointer) {
      listing.issues.push_back(
          {std::string(id), std::nullopt, NamedRevisionIssue::Kind::malformed});
      errno = 0;
      continue;
    }
    auto loaded = load_generation(pointer.value().generation, pointer.value().hash,
                                  &remaining_manifest_bytes);
    if (!loaded && (loaded.error().code == ErrorCode::resource_exhausted ||
                    loaded.error().code == ErrorCode::unsupported))
      return Result<NamedRevisionListing>::failure(loaded.error());
    if (!loaded || loaded.value().kind != SaveKind::explicit_save) {
      listing.issues.push_back(
          {std::string(id), pointer.value().name, NamedRevisionIssue::Kind::stale});
      errno = 0;
      continue;
    }
    const auto retained = decoded_footprint(loaded.value());
    if (retained > remaining_decoded_bytes || ++valid > max_named_revisions)
      return Result<NamedRevisionListing>::failure(
          {ErrorCode::resource_exhausted, "named revision validation budget exhausted"});
    remaining_decoded_bytes -= retained;
    candidates[pointer.value().name].push_back(
        {pointer.value().name, pointer.value().generation, std::string(id)});
    errno = 0;
  }
  const int read_error = errno;
  if (read_error != 0) {
    errno = read_error;
    return Result<NamedRevisionListing>::failure(io_error("list named revisions"));
  }
  for (auto& [name, entries] : candidates) {
    if (entries.size() == 1) {
      listing.revisions.push_back(std::move(entries.front()));
    } else {
      for (const auto& item : entries)
        listing.issues.push_back({item.entry, name, NamedRevisionIssue::Kind::duplicate});
    }
  }
  std::ranges::sort(listing.issues, {}, &NamedRevisionIssue::entry);
  state_identity = check_directory_identity(root_fd_, "state", state_fd_);
  if (!state_identity)
    return Result<NamedRevisionListing>::failure(state_identity.error());
  auto revisions_identity = check_directory_identity(state_fd_, "revisions", ::dirfd(stream.value));
  if (!revisions_identity)
    return Result<NamedRevisionListing>::failure(revisions_identity.error());
  return Result<NamedRevisionListing>::success(std::move(listing));
}

Result<LoadedGeneration> ProjectStore::load_named_revision(std::string_view name) const {
  if (!detail::valid_revision_name(name))
    return Result<LoadedGeneration>::failure(
        {ErrorCode::invalid_argument, "invalid revision name"});
  auto listing = list_named_revisions();
  if (!listing)
    return Result<LoadedGeneration>::failure(listing.error());
  const auto item = std::ranges::find(listing.value().revisions, name, &NamedRevision::name);
  if (item == listing.value().revisions.end()) {
    const bool damaged = std::ranges::any_of(listing.value().issues, [&](const auto& issue) {
      return issue.name && *issue.name == name;
    });
    return Result<LoadedGeneration>::failure(
        damaged ? foundation::Error{ErrorCode::corrupt_data, "named revision is damaged"}
                : foundation::Error{ErrorCode::not_found, "named revision missing"});
  }
  auto revisions = open_directory(state_fd_, "revisions");
  if (!revisions)
    return Result<LoadedGeneration>::failure(revisions.error());
  auto bytes =
      read_bounded(revisions.value().get(), item->entry.c_str(), max_revision_pointer_bytes);
  if (!bytes)
    return Result<LoadedGeneration>::failure(bytes.error());
  auto pointer = detail::decode_named_pointer(bytes.value());
  if (!pointer || pointer.value().name != name || pointer.value().generation != item->generation)
    return Result<LoadedGeneration>::failure({ErrorCode::corrupt_data, "named revision changed"});
  auto loaded = load_generation(pointer.value().generation, pointer.value().hash);
  if (!loaded)
    return loaded;
  if (loaded.value().kind != SaveKind::explicit_save)
    return Result<LoadedGeneration>::failure(
        {ErrorCode::corrupt_data, "named revision is not an explicit save"});
  return loaded;
}

Result<SaveOutcome> ProjectStore::name_current_revision(std::string_view name) {
  std::unique_lock save_lock(*save_gate_, std::try_to_lock);
  if (!save_lock.owns_lock())
    return Result<SaveOutcome>::failure({ErrorCode::conflict, "project save already in progress"});
  if (!detail::valid_revision_name(name))
    return Result<SaveOutcome>::failure({ErrorCode::invalid_argument, "invalid revision name"});
  if (lock_fd_ < 0)
    return Result<SaveOutcome>::failure({ErrorCode::conflict, "store is read-only"});
  auto state_identity = check_directory_identity(root_fd_, "state", state_fd_);
  if (!state_identity)
    return Result<SaveOutcome>::failure(state_identity.error());
  auto pointer_bytes = read_bounded(state_fd_, "CURRENT", max_pointer_bytes);
  if (!pointer_bytes)
    return Result<SaveOutcome>::failure(pointer_bytes.error());
  auto current = detail::decode_pointer(pointer_bytes.value());
  if (!current)
    return Result<SaveOutcome>::failure(current.error());
  return name_generation_unlocked(name, current.value().first, current.value().second);
}

std::string ProjectStore::pre_upgrade_revision_name(std::string_view schema,
                                                    std::string_view generation) {
  constexpr std::size_t generation_digits = 8;
  return "Before upgrade from " + std::string(schema) + ' ' +
         std::string(generation.substr(0, generation_digits));
}

Result<void> ProjectStore::keep_pre_upgrade_generation() {
  auto pointer_bytes = read_bounded(state_fd_, "CURRENT", max_pointer_bytes);
  if (!pointer_bytes)
    return Result<void>::failure(pointer_bytes.error());
  auto current = detail::decode_pointer(pointer_bytes.value());
  if (!current)
    return Result<void>::failure(current.error());
  const auto& [generation, hash] = current.value();
  auto loaded = load_generation(generation, hash);
  if (!loaded)
    return Result<void>::failure(loaded.error());
  const auto schema = loaded.value().migrated_from.value_or(std::string{});
  if (schema.empty())
    return Result<void>::success();
  auto listing = list_named_revisions();
  if (!listing)
    return Result<void>::failure(listing.error());
  if (std::ranges::any_of(listing.value().revisions,
                          [&](const auto& item) { return item.generation == generation; }))
    return Result<void>::success();
  auto named =
      name_generation_unlocked(pre_upgrade_revision_name(schema, generation), generation, hash);
  if (!named)
    return Result<void>::failure(named.error());
  // CURRENT must not move past the generation until its revision is durable:
  // the next save finds the revision if it landed, or names it again.
  if (named.value().status != SaveStatus::committed)
    return Result<void>::failure(
        {ErrorCode::io_failure, "the pre-upgrade version's revision may not be durable"});
  return Result<void>::success();
}

Result<SaveOutcome> ProjectStore::name_generation_unlocked(std::string_view name,
                                                           const std::string& generation,
                                                           const std::string& hash) {
  if (lock_fd_ < 0)
    return Result<SaveOutcome>::failure({ErrorCode::conflict, "store is read-only"});
  auto state_identity = check_directory_identity(root_fd_, "state", state_fd_);
  if (!state_identity)
    return Result<SaveOutcome>::failure(state_identity.error());
  auto lock_identity = check_writer_lock({state_fd_, lock_fd_});
  if (!lock_identity)
    return Result<SaveOutcome>::failure(lock_identity.error());
  auto listing = list_named_revisions();
  if (!listing)
    return Result<SaveOutcome>::failure(listing.error());
  if (listing.value().revisions.size() + listing.value().issues.size() >= max_revision_entries)
    return Result<SaveOutcome>::failure(
        {ErrorCode::resource_exhausted, "named revision entry limit reached"});
  if (std::ranges::any_of(listing.value().revisions,
                          [&](const auto& entry) { return entry.name == name; }) ||
      std::ranges::any_of(listing.value().issues,
                          [&](const auto& issue) { return issue.name && *issue.name == name; }))
    return Result<SaveOutcome>::failure({ErrorCode::conflict, "named revision already exists"});
  auto loaded = load_generation(generation, hash);
  if (!loaded)
    return Result<SaveOutcome>::failure(loaded.error());
  if (loaded.value().kind != SaveKind::explicit_save)
    return Result<SaveOutcome>::failure(
        {ErrorCode::corrupt_data, "CURRENT is not an explicit save"});
  if (::mkdirat(state_fd_, "revisions", 0700) != 0 && errno != EEXIST)
    return Result<SaveOutcome>::failure(io_error("create named revisions"));
  auto revisions = open_directory(state_fd_, "revisions");
  if (!revisions)
    return Result<SaveOutcome>::failure(revisions.error());
  if (::fsync(state_fd_) != 0)
    return Result<SaveOutcome>::failure(io_error("flush revisions directory entry"));
  auto id = random_name();
  if (!id)
    return Result<SaveOutcome>::failure(id.error());
  const auto temporary = ".tmp-" + id.value();
  auto written =
      write_sync_file(revisions.value().get(), temporary,
                      detail::encode_named_pointer({std::string(name), generation, hash}));
  if (!written)
    return Result<SaveOutcome>::failure(written.error());
  if (fail(SaveStage::revision_pointer_written))
    return Result<SaveOutcome>::failure({ErrorCode::io_failure, "injected revision interruption"});
  state_identity = check_directory_identity(root_fd_, "state", state_fd_);
  if (!state_identity)
    return Result<SaveOutcome>::failure(state_identity.error());
  lock_identity = check_writer_lock({state_fd_, lock_fd_});
  if (!lock_identity)
    return Result<SaveOutcome>::failure(lock_identity.error());
  auto revisions_identity =
      check_directory_identity(state_fd_, "revisions", revisions.value().get());
  if (!revisions_identity)
    return Result<SaveOutcome>::failure(revisions_identity.error());
  if (auto renamed =
          foundation::rename_in(revisions.value().get(), temporary, id.value(),
                                foundation::PublishMode::no_replace, "publish named revision");
      !renamed)
    return Result<SaveOutcome>::failure(std::move(renamed.error()));
  if (fail(SaveStage::revision_pointer_published) ||
      fail(SaveStage::revision_directory_flush_failed))
    return Result<SaveOutcome>::success({SaveStatus::uncertain, generation});
  if (::fsync(revisions.value().get()) != 0)
    return Result<SaveOutcome>::success({SaveStatus::uncertain, generation});
  return Result<SaveOutcome>::success({SaveStatus::committed, generation});
}

} // namespace oscillate::persistence
