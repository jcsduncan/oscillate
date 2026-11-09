#pragma once

#include "oscillate/foundation/file_io.hpp"
#include "oscillate/persistence/store.hpp"

#include <cstddef>
#include <cstdint>
#include <stop_token>
#include <string>
#include <string_view>
#include <sys/stat.h>

// The directory, digest and asset checks the ProjectStore files share.
namespace oscillate::persistence {

struct VerifiedExternalSource::Impl {
  project::SourceRecord record;
  std::uint64_t revision;
  std::string store_token;
  struct stat version;
};

} // namespace oscillate::persistence

namespace oscillate::persistence::detail {

inline constexpr std::size_t max_pointer_bytes = 256;

inline constexpr std::size_t max_recovery_entries = 8192;

inline constexpr std::size_t max_recovery_candidates = 256;

inline constexpr std::size_t max_recovery_manifest_bytes = std::size_t{64} * 1024U * 1024U;

inline constexpr std::size_t max_recovery_decoded_bytes = std::size_t{64} * 1024U * 1024U;

inline constexpr std::size_t max_revision_entries = 1024;

inline constexpr std::size_t max_named_revisions = 256;

inline constexpr std::size_t max_revision_pointer_bytes = 384;

inline constexpr std::uint64_t max_restore_source_bytes = std::uint64_t{64} * 1024U * 1024U * 1024U;

using Fd = foundation::UniqueFd;

struct AssetDirectoryHandles {
  int root;
  int assets;
};

struct WriterLockHandles {
  int state;
  int held;
};

// Conservative retained-state estimate for this fixed schema. A tempo event
// can expand to three timing pieces plus segment/vector storage; reserve 512
// bytes per event rather than using sizeof(TempoEvent) alone.
std::size_t decoded_footprint(const LoadedGeneration& candidate);

bool references_source(const LoadedGeneration& generation, const project::SourceRecord& expected);

foundation::Result<Fd> open_directory(int parent, const char* name);

foundation::Result<std::string> read_bounded(int parent, const char* name, std::size_t max_size,
                                             std::size_t* remaining_budget = nullptr);

foundation::Result<void> write_sync_file(int parent, const std::string& name,
                                         std::string_view bytes);

foundation::Result<std::string> digest(std::string_view bytes);

foundation::Result<void> injected(bool triggered);

std::string asset_name(const project::SourceRecord& source);

foundation::Result<Fd> checked_asset(int assets_fd, const project::SourceRecord& source);

foundation::Result<Fd> checked_external_source(const project::SourceRecord& source);

foundation::Result<void> check_directory_identity(int parent, const char* name, int held);

foundation::Result<void> check_assets_directory(AssetDirectoryHandles handles);

foundation::Result<void> check_writer_lock(WriterLockHandles handles);

foundation::Result<std::string> digest_file(const Fd& fd, std::uint64_t length,
                                            const std::stop_token& stop = {});

bool same_file_version(const struct stat& before, const struct stat& after);

#if OSCILLATE_HAVE_CODEC
foundation::Result<project::SourceRecord>
inspect_encoded_source(project::SourceId id, int fd, std::string hash, std::uint64_t length);
#endif

foundation::Result<struct stat> verify_new_external_source(const project::SourceRecord& source,
                                                           const std::stop_token& stop = {});

} // namespace oscillate::persistence::detail
