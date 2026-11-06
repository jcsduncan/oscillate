#include "oscillate/media/prefetch.hpp"
#include "private.hpp"

#include "oscillate/foundation/file_io.hpp"
#include "oscillate/foundation/hex.hpp"
#include "oscillate/foundation/sha256.hpp"

#if OSCILLATE_HAVE_CODEC
#include "oscillate/codec/encoded_reader.hpp"
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace oscillate::persistence {
namespace {
using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;

constexpr std::size_t max_pointer_bytes = 256;
constexpr std::size_t max_recovery_entries = 8192;
constexpr std::size_t max_recovery_candidates = 256;
constexpr std::size_t retained_recent_generations = 10;
constexpr std::size_t max_recovery_manifest_bytes = std::size_t{64} * 1024U * 1024U;
constexpr std::size_t max_recovery_decoded_bytes = std::size_t{64} * 1024U * 1024U;
constexpr std::size_t max_revision_entries = 1024;
constexpr std::size_t max_named_revisions = 256;
constexpr std::size_t max_revision_pointer_bytes = 384;
constexpr std::uint64_t max_restore_source_bytes = std::uint64_t{64} * 1024U * 1024U * 1024U;

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

// Conservative retained-state estimate for this fixed schema. A tempo event
// can expand to three timing pieces plus segment/vector storage; reserve 512
// bytes per event rather than using sizeof(TempoEvent) alone.
std::size_t decoded_footprint(const LoadedGeneration& candidate) {
  std::size_t bytes = 1024;
  bytes += candidate.snapshot.tempo.events().size() * 512U;
  bytes += candidate.snapshot.meter.events().size() * 64U;
  bytes += candidate.snapshot.tracks.size() * 128U;
  bytes += candidate.snapshot.sources.size() * 256U;
  for (const auto& source : candidate.snapshot.sources)
    if (source.external_link)
      bytes += source.external_link->last_known_path.size();
  bytes += candidate.snapshot.source_tempo_reviews.size() * 256U;
  bytes += candidate.snapshot.source_key_reviews.size() * 256U;
  for (const auto& review : candidate.snapshot.source_tempo_reviews) {
    bytes += (review.generated_beat_frames.size() + review.user_beat_frames.size() +
              review.generated_onset_frames.size() + review.dismissed_onset_frames.size()) *
             32U;
  }
  bytes += candidate.source_diagnostics.size() * 64U;
  const auto account_lanes = [&](const std::vector<project::AutomationLane>& lanes) {
    bytes += lanes.size() * 512U;
    for (const auto& lane : lanes) {
      bytes += lane.points.size() * 128U;
    }
  };
  for (const auto& track : candidate.snapshot.tracks) {
    bytes += track.clips.size() * 256U;
    account_lanes(track.automation);
    for (const auto& clip : track.clips) {
      account_lanes(clip.automation);
      bytes += clip.processing.reviewed_warp_markers.size() * 64U;
    }
    if (!std::holds_alternative<std::monostate>(track.instrument_source)) {
      bytes += 128U;
    }
    if (const auto* plugin = std::get_if<project::Vst3PluginInstance>(&track.instrument_source)) {
      bytes += plugin->component_state.size() + plugin->parameters.size() * 64U;
    }
    bytes += track.effects.size() * 256U;
    for (const auto& slot : track.effects) {
      if (const auto* plugin = std::get_if<project::Vst3PluginInstance>(&slot.processor)) {
        bytes += plugin->component_state.size() + plugin->parameters.size() * 64U;
      } else if (const auto* patch = std::get_if<project::NativeEffectPatch>(&slot.processor)) {
        bytes += patch->parameters.size() * 64U;
      }
    }
    bytes += track.sends.size() * 128U;
    bytes += track.midi_clips.size() * 256U;
    for (const auto& clip : track.midi_clips) {
      // Include vector spare capacity and allocator overhead, not just MidiNote's size.
      bytes += clip.notes.size() * 128U;
      account_lanes(clip.automation);
    }
  }
  bytes += candidate.snapshot.master_effects.size() * 256U;
  for (const auto& slot : candidate.snapshot.master_effects) {
    if (const auto* plugin = std::get_if<project::Vst3PluginInstance>(&slot.processor)) {
      bytes += plugin->component_state.size() + plugin->parameters.size() * 64U;
    } else if (const auto* patch = std::get_if<project::NativeEffectPatch>(&slot.processor)) {
      bytes += patch->parameters.size() * 64U;
    }
  }
  return bytes;
}

bool references_source(const LoadedGeneration& generation, const project::SourceRecord& expected) {
  return std::ranges::any_of(generation.snapshot.sources,
                             [&](const auto& source) { return source == expected; });
}

using Fd = foundation::UniqueFd;
using foundation::io_error;

Result<void> ensure_directory(int parent, const char* name, bool create) {
  if (create && ::mkdirat(parent, name, 0700) != 0 && errno != EEXIST) {
    return Result<void>::failure(io_error("create project directory"));
  }
  return Result<void>::success();
}

Result<Fd> open_directory(int parent, const char* name) {
  Fd fd(::openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (!fd) {
    return Result<Fd>::failure(io_error("open project directory"));
  }
  return Result<Fd>::success(std::move(fd));
}

Result<std::string> read_bounded(int parent, const char* name, std::size_t max_size,
                                 std::size_t* remaining_budget = nullptr) {
  Fd fd(::openat(parent, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  if (!fd) {
    return Result<std::string>::failure(
        errno == ENOENT ? foundation::Error{ErrorCode::not_found, "project file missing"}
                        : io_error("open project file"));
  }
  struct stat status{};
  if (::fstat(fd.get(), &status) != 0)
    return Result<std::string>::failure(io_error("stat project file"));
  if (!S_ISREG(status.st_mode) || status.st_size < 0 ||
      static_cast<std::uint64_t>(status.st_size) > max_size) {
    return Result<std::string>::failure(
        {ErrorCode::corrupt_data, "invalid project file size/type"});
  }
  if (remaining_budget != nullptr) {
    const auto size = static_cast<std::size_t>(status.st_size);
    if (size > *remaining_budget) {
      return Result<std::string>::failure(
          {ErrorCode::resource_exhausted, "recovery byte budget exhausted"});
    }
    *remaining_budget -= size;
  }
  std::string bytes(static_cast<std::size_t>(status.st_size), '\0');
  std::size_t at = 0;
  while (at < bytes.size()) {
    const auto count = ::read(fd.get(), bytes.data() + at, bytes.size() - at);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return Result<std::string>::failure(io_error("read project file"));
    at += static_cast<std::size_t>(count);
  }
  char extra;
  const auto more = ::read(fd.get(), &extra, 1);
  if (more != 0) {
    return Result<std::string>::failure(
        {ErrorCode::corrupt_data, "project file changed during read"});
  }
  return Result<std::string>::success(std::move(bytes));
}

constexpr std::size_t random_name_bytes = 16;

bool hex_name(std::string_view value) {
  return foundation::is_lowercase_hex(value, random_name_bytes * foundation::hex_digits_per_byte);
}

Result<std::string> random_name() {
  std::array<std::uint8_t, random_name_bytes> bytes{};
  std::size_t at = 0;
  while (at < bytes.size()) {
    const auto count = ::getrandom(bytes.data() + at, bytes.size() - at, 0);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return Result<std::string>::failure(io_error("create generation ID"));
    at += static_cast<std::size_t>(count);
  }
  return Result<std::string>::success(foundation::to_hex(std::span(bytes)));
}

Result<void> write_sync_file(int parent, const std::string& name, std::string_view bytes) {
  return foundation::write_new_synced(
      parent, name, std::as_bytes(std::span(bytes.data(), bytes.size())), "project file");
}

Result<void> flush_directory(int fd) { return foundation::sync(fd, "flush project directory"); }

Result<std::string> digest(std::string_view bytes) {
  auto hash = foundation::sha256(bytes);
  if (!hash)
    return Result<std::string>::failure(std::move(hash.error()));
  return Result<std::string>::success(foundation::to_hex(std::span(hash.value())));
}

Result<void> injected(bool triggered) {
  if (triggered)
    return Result<void>::failure({ErrorCode::io_failure, "injected storage interruption"});
  return Result<void>::success();
}

std::string_view source_extension(project::SourceEncoding encoding) {
  switch (encoding) {
  case project::SourceEncoding::wav:
    return ".wav";
  case project::SourceEncoding::aiff:
    return ".aiff";
  case project::SourceEncoding::flac:
    return ".flac";
  case project::SourceEncoding::mp3:
    return ".mp3";
  }
  return {};
}

std::string asset_name(const project::SourceRecord& source) {
  return source.sha256 + std::string(source_extension(source.encoding));
}

bool valid_sha256(std::string_view value) {
  return value.size() == 64 && std::ranges::all_of(value, [](char ch) {
           return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         });
}

Result<Fd> checked_asset(int assets_fd, const project::SourceRecord& source) {
  if (source.external_link || !valid_sha256(source.sha256) ||
      source_extension(source.encoding).empty())
    return Result<Fd>::failure({ErrorCode::invalid_argument, "invalid managed source identity"});
#if !OSCILLATE_HAVE_CODEC
  if (source.encoding != project::SourceEncoding::wav)
    return Result<Fd>::failure({ErrorCode::unsupported, "encoded source codec is unavailable"});
#endif
  const auto name = asset_name(source);
  Fd fd(::openat(assets_fd, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  if (!fd) {
    return Result<Fd>::failure(
        errno == ENOENT ? foundation::Error{ErrorCode::not_found, "managed source missing"}
                        : io_error("open managed source"));
  }
  struct stat status{};
  if (::fstat(fd.get(), &status) != 0)
    return Result<Fd>::failure(io_error("stat managed source"));
  if (!S_ISREG(status.st_mode) || status.st_size < 0 ||
      static_cast<std::uint64_t>(status.st_size) != source.byte_length) {
    return Result<Fd>::failure({ErrorCode::corrupt_data, "managed source size/type mismatch"});
  }
  return Result<Fd>::success(std::move(fd));
}

Result<Fd> checked_external_source(const project::SourceRecord& source) {
  if (!source.external_link)
    return Result<Fd>::failure({ErrorCode::invalid_argument, "source is not externally linked"});
  const auto& path = source.external_link->last_known_path;
  Fd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  if (!fd) {
    return Result<Fd>::failure(
        errno == ENOENT || errno == ENOTDIR
            ? foundation::Error{ErrorCode::not_found, "external source missing"}
            : foundation::Error{ErrorCode::corrupt_data, "external source unavailable"});
  }
  struct stat status{};
  if (::fstat(fd.get(), &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0 ||
      static_cast<std::uint64_t>(status.st_size) != source.byte_length)
    return Result<Fd>::failure({ErrorCode::corrupt_data, "external source size/type mismatch"});
  return Result<Fd>::success(std::move(fd));
}

struct AssetDirectoryHandles {
  int root;
  int assets;
};
Result<void> check_directory_identity(int parent, const char* name, int held) {
  auto current = open_directory(parent, name);
  if (!current)
    return Result<void>::failure(current.error());
  struct stat held_status{};
  struct stat current_status{};
  if (::fstat(held, &held_status) != 0 || ::fstat(current.value().get(), &current_status) != 0)
    return Result<void>::failure(io_error("stat project directory"));
  if (held_status.st_dev != current_status.st_dev || held_status.st_ino != current_status.st_ino)
    return Result<void>::failure({ErrorCode::conflict, "project directory changed after open"});
  return Result<void>::success();
}
Result<void> check_assets_directory(AssetDirectoryHandles handles) {
  return check_directory_identity(handles.root, "assets", handles.assets);
}

struct WriterLockHandles {
  int state;
  int held;
};
Result<void> check_writer_lock(WriterLockHandles handles) {
  struct stat held{};
  struct stat named{};
  if (::fstat(handles.held, &held) != 0)
    return Result<void>::failure(io_error("stat writer lock"));
  if (::fstatat(handles.state, "WRITER.lock", &named, AT_SYMLINK_NOFOLLOW) != 0) {
    if (errno == ENOENT)
      return Result<void>::failure({ErrorCode::conflict, "writer lock changed after open"});
    return Result<void>::failure(io_error("stat writer lock"));
  }
  if (!S_ISREG(named.st_mode) || held.st_dev != named.st_dev || held.st_ino != named.st_ino)
    return Result<void>::failure({ErrorCode::conflict, "writer lock changed after open"});
  return Result<void>::success();
}

Result<std::string> digest_file(const Fd& fd, std::uint64_t length,
                                const std::stop_token& stop = {}) {
  auto hasher = foundation::Sha256::create();
  if (!hasher)
    return Result<std::string>::failure(std::move(hasher.error()));
  std::array<unsigned char, 65536> buffer{};
  std::uint64_t offset = 0;
  while (offset < length) {
    if (stop.stop_requested())
      return Result<std::string>::failure({ErrorCode::cancelled, "source import cancelled"});
    const auto wanted =
        static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), length - offset));
    const auto count = ::pread(fd.get(), buffer.data(), wanted, static_cast<off_t>(offset));
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return Result<std::string>::failure(io_error("read managed source"));
    if (auto updated = hasher.value().update(
            std::as_bytes(std::span(buffer).first(static_cast<std::size_t>(count))));
        !updated)
      return Result<std::string>::failure(std::move(updated.error()));
    offset += static_cast<std::uint64_t>(count);
  }
  auto hash = hasher.value().finish();
  if (!hash)
    return Result<std::string>::failure(std::move(hash.error()));
  return Result<std::string>::success(foundation::to_hex(std::span(hash.value())));
}

bool same_file_version(const struct stat& before, const struct stat& after) {
  return before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
         before.st_size == after.st_size && before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
         before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
         before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
         before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
}

// Managed assets are private and never rewritten in place, so a file this
// process has already verified against its record (the same file version:
// device, inode, size, modification and change times) is not hashed and
// decoded again each time playback or analysis opens it. Starting playback
// of long encoded tracks otherwise decoded each of them in full first.
struct VerifiedAsset {
  struct stat file;
  project::SourceRecord source;
};
constexpr std::size_t max_verified_assets = 4'096;

std::mutex& verified_assets_mutex() {
  static std::mutex mutex;
  return mutex;
}

std::vector<VerifiedAsset>& verified_assets() {
  static std::vector<VerifiedAsset> assets;
  return assets;
}

bool verified_before(const struct stat& file, const project::SourceRecord& source) {
  const std::lock_guard lock(verified_assets_mutex());
  return std::ranges::any_of(verified_assets(), [&](const VerifiedAsset& asset) {
    return same_file_version(asset.file, file) && asset.source == source;
  });
}

void remember_verified(const struct stat& file, const project::SourceRecord& source) {
  const std::lock_guard lock(verified_assets_mutex());
  auto& assets = verified_assets();
  std::erase_if(assets, [&](const VerifiedAsset& asset) {
    return asset.file.st_dev == file.st_dev && asset.file.st_ino == file.st_ino;
  });
  if (assets.size() >= max_verified_assets)
    assets.erase(assets.begin());
  assets.push_back({file, source});
}

#if OSCILLATE_HAVE_CODEC
Result<project::SourceRecord> inspect_encoded_source(project::SourceId id, int fd, std::string hash,
                                                     std::uint64_t length) {
  auto decoded = codec::EncodedReader::open_fd(fd);
  if (!decoded)
    return Result<project::SourceRecord>::failure(decoded.error());
  const auto& info = decoded.value().info();
  project::SourceEncoding encoding;
  switch (info.format) {
  case codec::EncodedFormat::aiff:
    encoding = project::SourceEncoding::aiff;
    break;
  case codec::EncodedFormat::flac:
    encoding = project::SourceEncoding::flac;
    break;
  case codec::EncodedFormat::mp3:
    encoding = project::SourceEncoding::mp3;
    break;
  case codec::EncodedFormat::wav:
    return Result<project::SourceRecord>::failure(
        {ErrorCode::unsupported, "use the WAV staging path for WAV media"});
  default:
    return Result<project::SourceRecord>::failure(
        {ErrorCode::unsupported, "unrecognized encoded source format"});
  }
  if (info.encoded_bytes != length)
    return Result<project::SourceRecord>::failure(
        {ErrorCode::conflict, "encoded source changed during inspection"});
  std::array<codec::StereoFrame, 1> first{};
  auto probe = decoded.value().read_at(0, first);
  if (!probe || probe.value() != 1)
    return Result<project::SourceRecord>::failure(
        {ErrorCode::corrupt_data, "encoded source has no decodable audio frames"});
  return Result<project::SourceRecord>::success(
      {id, encoding, std::move(hash), length, info.sample_rate, info.channels, info.frames});
}
#endif

Result<void> clean_abandoned_wav_stages(int assets_fd) {
  Fd scan_fd(::openat(assets_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (!scan_fd)
    return Result<void>::failure(io_error("scan staged sources"));
  const int directory_fd = scan_fd.release();
  struct Directory {
    DIR* value;
    ~Directory() {
      if (value)
        ::closedir(value);
    }
  } directory{::fdopendir(directory_fd)};
  if (!directory.value) {
    ::close(directory_fd);
    return Result<void>::failure(io_error("scan staged sources"));
  }
  // Opening the sole writer is the cleanup boundary for stages abandoned by a
  // terminated process. Never touch a published hash-named asset or a symlink.
  std::size_t scanned = 0;
  bool removed = false;
  for (;;) {
    errno = 0;
    auto* entry = ::readdir(directory.value);
    if (!entry) {
      if (errno != 0)
        return Result<void>::failure(io_error("scan staged sources"));
      break;
    }
    if (++scanned > max_recovery_entries)
      return Result<void>::failure(
          {ErrorCode::resource_exhausted, "too many entries for source stage cleanup"});
    const std::string_view name(entry->d_name);
    if (!name.starts_with(".stage-") || !hex_name(name.substr(7)))
      continue;
    struct stat status{};
    if (::fstatat(assets_fd, entry->d_name, &status, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISREG(status.st_mode))
      continue;
    if (::unlinkat(assets_fd, entry->d_name, 0) != 0 && errno != ENOENT)
      return Result<void>::failure(io_error("remove abandoned staged source"));
    removed = true;
  }
  if (removed && ::fsync(assets_fd) != 0)
    return Result<void>::failure(io_error("flush source stage cleanup"));
  return Result<void>::success();
}

struct DirectoryCloser {
  void operator()(DIR* stream) const noexcept { (void)::closedir(stream); }
};
using DirectoryStream = std::unique_ptr<DIR, DirectoryCloser>;

Result<void> visit_directory_entries(int directory_fd, const char* operation,
                                     const std::function<Result<void>(const char*)>& visit) {
  Fd scan_fd(::openat(directory_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (!scan_fd)
    return Result<void>::failure(io_error(operation));
  const int raw_fd = scan_fd.release();
  DIR* raw = ::fdopendir(raw_fd);
  if (!raw) {
    const auto failure = io_error(operation);
    ::close(raw_fd);
    return Result<void>::failure(failure);
  }
  DirectoryStream stream(raw);
  std::size_t scanned = 0;
  for (;;) {
    errno = 0;
    const auto* entry = ::readdir(stream.get());
    if (!entry) {
      if (errno != 0)
        return Result<void>::failure(io_error(operation));
      return Result<void>::success();
    }
    if (++scanned > max_recovery_entries)
      return Result<void>::failure(
          {ErrorCode::resource_exhausted, "too many entries for stage cleanup"});
    auto handled = visit(entry->d_name);
    if (!handled)
      return handled;
  }
}

Result<void> clean_abandoned_pointer_stages(int state_fd) {
  bool removed = false;
  auto scanned =
      visit_directory_entries(state_fd, "scan temporary pointers", [&](const char* entry) {
        const std::string_view name(entry);
        const auto suffix = name.starts_with(".CURRENT-")    ? name.substr(9)
                            : name.starts_with(".AUTOSAVE-") ? name.substr(10)
                                                             : std::string_view{};
        if (!hex_name(suffix))
          return Result<void>::success();
        struct stat status{};
        if (::fstatat(state_fd, entry, &status, AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISREG(status.st_mode))
          return Result<void>::success();
        if (::unlinkat(state_fd, entry, 0) != 0 && errno != ENOENT)
          return Result<void>::failure(io_error("remove abandoned pointer"));
        removed = true;
        return Result<void>::success();
      });
  if (!scanned)
    return scanned;
  if (removed && ::fsync(state_fd) != 0)
    return Result<void>::failure(io_error("flush pointer stage cleanup"));
  return Result<void>::success();
}

Result<void> clean_abandoned_generation_stages(int generations_fd) {
  bool removed = false;
  auto scanned =
      visit_directory_entries(generations_fd, "scan temporary generations", [&](const char* entry) {
        const std::string_view name(entry);
        if (!name.starts_with(".tmp-") || !hex_name(name.substr(5)))
          return Result<void>::success();
        auto stage = open_directory(generations_fd, entry);
        if (!stage) // Symlinks and non-directories are not ours to remove.
          return Result<void>::success();
        std::vector<std::string> files;
        bool known = true;
        auto contents = visit_directory_entries(
            stage.value().get(), "scan staged generation", [&](const char* item) {
              const std::string_view file(item);
              if (file == "." || file == "..")
                return Result<void>::success();
              struct stat status{};
              if ((file != "manifest.json" && file != "manifest.sha256") ||
                  ::fstatat(stage.value().get(), item, &status, AT_SYMLINK_NOFOLLOW) != 0 ||
                  !S_ISREG(status.st_mode)) {
                known = false;
                return Result<void>::success();
              }
              files.emplace_back(file);
              return Result<void>::success();
            });
        if (!contents)
          return contents;
        if (!known) {
          return Result<void>::success();
        }
        for (const auto& file : files)
          if (::unlinkat(stage.value().get(), file.c_str(), 0) != 0 && errno != ENOENT)
            return Result<void>::failure(io_error("remove staged generation file"));
        struct stat held{};
        struct stat named{};
        if (::fstat(stage.value().get(), &held) != 0 ||
            ::fstatat(generations_fd, entry, &named, AT_SYMLINK_NOFOLLOW) != 0 ||
            held.st_dev != named.st_dev || held.st_ino != named.st_ino)
          return Result<void>::failure({ErrorCode::conflict, "temporary generation changed"});
        if (::unlinkat(generations_fd, entry, AT_REMOVEDIR) != 0)
          return Result<void>::failure(io_error("remove abandoned generation"));
        removed = true;
        return Result<void>::success();
      });
  if (!scanned)
    return scanned;
  if (removed && ::fsync(generations_fd) != 0)
    return Result<void>::failure(io_error("flush generation stage cleanup"));
  return Result<void>::success();
}
} // namespace

struct WavStageTask::Impl {
  Impl(project::SourceId source_id, bool wav_path) : id(source_id), wav_only(wav_path) {}
  Fd root;
  Fd assets;
  Fd input;
  project::SourceId id;
  bool wav_only;
  std::optional<std::uint64_t> required_length;
  std::string store_token;
};

struct StagedWav::Impl {
  Fd root;
  Fd assets;
  Fd file;
  std::string temporary_name;
  std::unique_ptr<project::SourceRecord> record;
  bool wav_only = true;
  std::string store_token;
  struct stat file_status{};
  std::optional<struct stat> existing_status;

  ~Impl() {
    if (!assets || !file || temporary_name.empty())
      return;
    struct stat held{};
    struct stat named{};
    if (::fstat(file.get(), &held) == 0 &&
        ::fstatat(assets.get(), temporary_name.c_str(), &named, AT_SYMLINK_NOFOLLOW) == 0 &&
        held.st_dev == named.st_dev && held.st_ino == named.st_ino)
      ::unlinkat(assets.get(), temporary_name.c_str(), 0);
  }
};

WavStageTask::WavStageTask(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
WavStageTask::WavStageTask(WavStageTask&&) noexcept = default;
WavStageTask& WavStageTask::operator=(WavStageTask&&) noexcept = default;
WavStageTask::~WavStageTask() = default;

StagedWav::StagedWav(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
StagedWav::StagedWav(StagedWav&&) noexcept = default;
StagedWav& StagedWav::operator=(StagedWav&&) noexcept = default;
StagedWav::~StagedWav() = default;
const project::SourceRecord& StagedWav::record() const noexcept { return *impl_->record; }

Result<StagedWav> WavStageTask::run(const std::stop_token& stop) && {
  auto state = std::move(impl_);
  if (!state)
    return Result<StagedWav>::failure(
        {ErrorCode::invalid_argument, "source stage task was consumed"});
  if (stop.stop_requested())
    return Result<StagedWav>::failure({ErrorCode::cancelled, "source import cancelled"});
  auto directory = check_assets_directory({state->root.get(), state->assets.get()});
  if (!directory)
    return Result<StagedWav>::failure(directory.error());
  struct stat before{};
  if (::fstat(state->input.get(), &before) != 0 || !S_ISREG(before.st_mode) ||
      before.st_size < (state->wav_only ? 44 : 1)) {
    return Result<StagedWav>::failure({ErrorCode::invalid_argument, "invalid source input"});
  }
  if (state->required_length &&
      static_cast<std::uint64_t>(before.st_size) != *state->required_length)
    return Result<StagedWav>::failure(
        {ErrorCode::conflict, "restore candidate length changed before staging"});
  auto random = random_name();
  if (!random)
    return Result<StagedWav>::failure(random.error());
  auto staged = std::make_unique<StagedWav::Impl>();
  staged->root = std::move(state->root);
  staged->assets = std::move(state->assets);
  staged->temporary_name = ".stage-" + random.value();
  staged->store_token = std::move(state->store_token);
  staged->wav_only = state->wav_only;
  staged->file = Fd(::openat(staged->assets.get(), staged->temporary_name.c_str(),
                             O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
  if (!staged->file)
    return Result<StagedWav>::failure(io_error("create staged source"));
  std::array<char, 65536> buffer{};
  std::uint64_t at = 0;
  const auto length = static_cast<std::uint64_t>(before.st_size);
  while (at < length) {
    if (stop.stop_requested())
      return Result<StagedWav>::failure({ErrorCode::cancelled, "source import cancelled"});
    const auto wanted =
        static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), length - at));
    const auto count = ::pread(state->input.get(), buffer.data(), wanted, static_cast<off_t>(at));
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return Result<StagedWav>::failure(io_error("read source input"));
    auto written = foundation::write_all(
        staged->file.get(), std::string_view(buffer.data(), static_cast<std::size_t>(count)),
        "write project file");
    if (!written)
      return Result<StagedWav>::failure(written.error());
    at += static_cast<std::uint64_t>(count);
  }
  if (stop.stop_requested())
    return Result<StagedWav>::failure({ErrorCode::cancelled, "source import cancelled"});
  struct stat after{};
  if (::fstat(state->input.get(), &after) != 0 || !same_file_version(before, after)) {
    return Result<StagedWav>::failure({ErrorCode::conflict, "source input changed while staging"});
  }
  if (::fsync(staged->file.get()) != 0 || ::fstat(staged->file.get(), &staged->file_status) != 0)
    return Result<StagedWav>::failure(io_error("flush staged source"));
  auto hash = digest_file(staged->file, length, stop);
  if (!hash)
    return Result<StagedWav>::failure(hash.error());
  if (state->wav_only) {
    auto wav = media::WavReader::open_fd(staged->file.get());
    if (!wav)
      return Result<StagedWav>::failure(wav.error());
    staged->record = std::make_unique<project::SourceRecord>(project::SourceRecord{
        state->id, project::SourceEncoding::wav, std::move(hash).value(), length,
        wav.value().sample_rate(), wav.value().channels(), wav.value().frame_count()});
  } else {
#if OSCILLATE_HAVE_CODEC
    auto inspected =
        inspect_encoded_source(state->id, staged->file.get(), std::move(hash).value(), length);
    if (!inspected)
      return Result<StagedWav>::failure(inspected.error());
    staged->record = std::make_unique<project::SourceRecord>(std::move(inspected).value());
#else
    return Result<StagedWav>::failure({ErrorCode::unsupported, "encoded codec is unavailable"});
#endif
  }
  struct stat verified_file{};
  if (::fstat(staged->file.get(), &verified_file) != 0 ||
      !same_file_version(staged->file_status, verified_file)) {
    return Result<StagedWav>::failure(
        {ErrorCode::conflict, "staged source changed during verification"});
  }
  // A deduplicated on-disk asset is verified here, away from the UI thread.
  // Publication later accepts it only if its inode and metadata are unchanged.
  auto existing = checked_asset(staged->assets.get(), *staged->record);
  if (existing) {
    struct stat before_existing{};
    if (::fstat(existing.value().get(), &before_existing) != 0)
      return Result<StagedWav>::failure(io_error("stat managed source"));
    auto existing_hash = digest_file(existing.value(), length, stop);
    if (!existing_hash)
      return Result<StagedWav>::failure(existing_hash.error());
    if (existing_hash.value() != staged->record->sha256)
      return Result<StagedWav>::failure(
          {ErrorCode::corrupt_data, "deduplicated source hash mismatch"});
    struct stat status{};
    if (::fstat(existing.value().get(), &status) != 0 ||
        !same_file_version(before_existing, status)) {
      return Result<StagedWav>::failure(
          {ErrorCode::conflict, "managed source changed during verification"});
    }
    staged->existing_status = status;
  } else if (existing.error().code != ErrorCode::not_found) {
    return Result<StagedWav>::failure(existing.error());
  }
  if (stop.stop_requested())
    return Result<StagedWav>::failure({ErrorCode::cancelled, "source import cancelled"});
  return Result<StagedWav>::success(StagedWav(std::move(staged)));
}

ProjectStore::ProjectStore(ProjectStore&& other) noexcept
    : root_fd_(std::exchange(other.root_fd_, -1)), assets_fd_(std::exchange(other.assets_fd_, -1)),
      state_fd_(std::exchange(other.state_fd_, -1)),
      generations_fd_(std::exchange(other.generations_fd_, -1)),
      lock_fd_(std::exchange(other.lock_fd_, -1)), fault_(std::move(other.fault_)),
      store_token_(std::move(other.store_token_)), save_gate_(std::move(other.save_gate_)),
      autosave_only_(other.autosave_only_), explicit_save_only_(other.explicit_save_only_) {}

ProjectStore& ProjectStore::operator=(ProjectStore&& other) noexcept {
  if (this != &other) {
    if (lock_fd_ >= 0)
      ::close(lock_fd_);
    if (generations_fd_ >= 0)
      ::close(generations_fd_);
    if (state_fd_ >= 0)
      ::close(state_fd_);
    if (assets_fd_ >= 0)
      ::close(assets_fd_);
    if (root_fd_ >= 0)
      ::close(root_fd_);
    root_fd_ = std::exchange(other.root_fd_, -1);
    assets_fd_ = std::exchange(other.assets_fd_, -1);
    state_fd_ = std::exchange(other.state_fd_, -1);
    generations_fd_ = std::exchange(other.generations_fd_, -1);
    lock_fd_ = std::exchange(other.lock_fd_, -1);
    fault_ = std::move(other.fault_);
    store_token_ = std::move(other.store_token_);
    save_gate_ = std::move(other.save_gate_);
    autosave_only_ = other.autosave_only_;
    explicit_save_only_ = other.explicit_save_only_;
  }
  return *this;
}

ProjectStore::~ProjectStore() {
  if (lock_fd_ >= 0)
    ::close(lock_fd_);
  if (generations_fd_ >= 0)
    ::close(generations_fd_);
  if (state_fd_ >= 0)
    ::close(state_fd_);
  if (assets_fd_ >= 0)
    ::close(assets_fd_);
  if (root_fd_ >= 0)
    ::close(root_fd_);
}

Result<ProjectStore> ProjectStore::fork_autosave_writer() const {
  if (lock_fd_ < 0 || !save_gate_ || autosave_only_ || explicit_save_only_)
    return Result<ProjectStore>::failure({ErrorCode::conflict, "project writer is unavailable"});
  Fd root(::fcntl(root_fd_, F_DUPFD_CLOEXEC, 0));
  Fd assets(::fcntl(assets_fd_, F_DUPFD_CLOEXEC, 0));
  Fd state(::fcntl(state_fd_, F_DUPFD_CLOEXEC, 0));
  Fd generations(::fcntl(generations_fd_, F_DUPFD_CLOEXEC, 0));
  Fd lock(::fcntl(lock_fd_, F_DUPFD_CLOEXEC, 0));
  if (!root || !assets || !state || !generations || !lock)
    return Result<ProjectStore>::failure(io_error("duplicate autosave writer"));
  ProjectStore copy(
      {root.release(), assets.release(), state.release(), generations.release(), lock.release()},
      fault_);
  copy.store_token_ = store_token_;
  copy.save_gate_ = save_gate_;
  copy.autosave_only_ = true;
  return Result<ProjectStore>::success(std::move(copy));
}

Result<ProjectStore> ProjectStore::fork_explicit_save_writer() const {
  auto writer = fork_autosave_writer();
  if (!writer)
    return writer;
  writer.value().autosave_only_ = false;
  writer.value().explicit_save_only_ = true;
  return writer;
}

Result<SourceReceipt> ProjectStore::stage_wav(project::SourceId id, int input_fd) {
  auto task = begin_wav_stage(id, input_fd);
  if (!task)
    return Result<SourceReceipt>::failure(task.error());
  auto staged = std::move(task).value().run();
  if (!staged)
    return Result<SourceReceipt>::failure(staged.error());
  return publish_wav_stage(std::move(staged).value());
}

Result<WavStageTask> ProjectStore::begin_wav_stage(project::SourceId id, int input_fd) const {
  if (lock_fd_ < 0)
    return Result<WavStageTask>::failure({ErrorCode::conflict, "store is read-only"});
  if (id.bits().is_nil())
    return Result<WavStageTask>::failure({ErrorCode::invalid_argument, "source ID is nil"});
  auto assets_identity = check_assets_directory({root_fd_, assets_fd_});
  if (!assets_identity)
    return Result<WavStageTask>::failure(assets_identity.error());
  if (input_fd < 0)
    return Result<WavStageTask>::failure({ErrorCode::invalid_argument, "input is not a WAV file"});
  Fd root(::fcntl(root_fd_, F_DUPFD_CLOEXEC, 0));
  Fd assets(::fcntl(assets_fd_, F_DUPFD_CLOEXEC, 0));
  Fd input(::fcntl(input_fd, F_DUPFD_CLOEXEC, 0));
  if (!root || !assets || !input)
    return Result<WavStageTask>::failure(io_error("duplicate WAV import descriptors"));
  auto task = std::make_unique<WavStageTask::Impl>(id, true);
  task->root = std::move(root);
  task->assets = std::move(assets);
  task->input = std::move(input);
  task->store_token = store_token_;
  return Result<WavStageTask>::success(WavStageTask(std::move(task)));
}

Result<EncodedStageTask> ProjectStore::begin_encoded_stage(project::SourceId id,
                                                           int input_fd) const {
#if OSCILLATE_HAVE_CODEC
  auto prepared = begin_wav_stage(id, input_fd);
  if (prepared)
    prepared.value().impl_->wav_only = false;
  return prepared;
#else
  (void)id;
  (void)input_fd;
  return Result<EncodedStageTask>::failure(
      {ErrorCode::unsupported, "encoded source codec is unavailable"});
#endif
}

Result<SourceReceipt> ProjectStore::publish_wav_stage(StagedWav&& staged) {
  if (!staged.impl_ || !staged.impl_->wav_only)
    return Result<SourceReceipt>::failure(
        {ErrorCode::invalid_argument, "WAV publication requires a WAV stage"});
  return publish_encoded_stage(std::move(staged));
}

Result<SourceReceipt> ProjectStore::publish_encoded_stage(StagedEncoded&& staged) {
  std::unique_lock save_lock(*save_gate_, std::try_to_lock);
  if (!save_lock.owns_lock())
    return Result<SourceReceipt>::failure(
        {ErrorCode::conflict, "project save already in progress"});
  return publish_encoded_stage_unlocked(std::move(staged));
}

Result<SourceReceipt> ProjectStore::publish_encoded_stage_unlocked(StagedEncoded&& staged) {
  auto state = std::move(staged.impl_);
  if (!state || !state->record)
    return Result<SourceReceipt>::failure(
        {ErrorCode::invalid_argument, "source stage was consumed"});
  if (lock_fd_ < 0 || state->store_token != store_token_)
    return Result<SourceReceipt>::failure(
        {ErrorCode::conflict, "source stage belongs to another writer"});
  auto lock_identity = check_writer_lock({state_fd_, lock_fd_});
  if (!lock_identity)
    return Result<SourceReceipt>::failure(lock_identity.error());
  auto state_identity = check_directory_identity(root_fd_, "state", state_fd_);
  if (!state_identity)
    return Result<SourceReceipt>::failure(state_identity.error());
  auto directory = check_assets_directory({root_fd_, assets_fd_});
  if (!directory)
    return Result<SourceReceipt>::failure(directory.error());
  directory = check_assets_directory({state->root.get(), state->assets.get()});
  if (!directory)
    return Result<SourceReceipt>::failure(directory.error());
  struct stat held_assets{};
  struct stat staged_assets{};
  if (::fstat(assets_fd_, &held_assets) != 0 || ::fstat(state->assets.get(), &staged_assets) != 0)
    return Result<SourceReceipt>::failure(io_error("stat assets directory"));
  if (held_assets.st_dev != staged_assets.st_dev || held_assets.st_ino != staged_assets.st_ino)
    return Result<SourceReceipt>::failure({ErrorCode::conflict, "source stage directory differs"});
  struct stat held_file{};
  struct stat named_file{};
  if (::fstat(state->file.get(), &held_file) != 0 ||
      ::fstatat(assets_fd_, state->temporary_name.c_str(), &named_file, AT_SYMLINK_NOFOLLOW) != 0)
    return Result<SourceReceipt>::failure(io_error("stat staged source"));
  const auto& initial = state->file_status;
  if (!S_ISREG(named_file.st_mode) || held_file.st_dev != named_file.st_dev ||
      held_file.st_ino != named_file.st_ino || !same_file_version(initial, held_file)) {
    return Result<SourceReceipt>::failure({ErrorCode::conflict, "staged source changed"});
  }
  if (fail(SaveStage::asset_file_flushed))
    return Result<SourceReceipt>::failure({ErrorCode::io_failure, "injected asset interruption"});
  // A staged repair can have validated a generation through our pinned fd
  // while another actor replaced its name in state/. Stop before linking.
  auto generations_identity = check_directory_identity(state_fd_, "generations", generations_fd_);
  if (!generations_identity)
    return Result<SourceReceipt>::failure(generations_identity.error());
  const auto name = asset_name(*state->record);
  if (::linkat(assets_fd_, state->temporary_name.c_str(), assets_fd_, name.c_str(), 0) != 0) {
    if (errno != EEXIST)
      return Result<SourceReceipt>::failure(io_error("publish managed source"));
    if (!state->existing_status)
      return Result<SourceReceipt>::failure(
          {ErrorCode::conflict, "managed source appeared after staging"});
    auto existing = checked_asset(assets_fd_, *state->record);
    if (!existing)
      return Result<SourceReceipt>::failure(existing.error());
    struct stat current{};
    if (::fstat(existing.value().get(), &current) != 0)
      return Result<SourceReceipt>::failure(io_error("stat managed source"));
    const auto& verified = *state->existing_status;
    if (!same_file_version(verified, current)) {
      return Result<SourceReceipt>::failure(
          {ErrorCode::conflict, "managed source changed after verification"});
    }
  }
  if (fail(SaveStage::asset_published))
    return Result<SourceReceipt>::failure({ErrorCode::io_failure, "injected asset interruption"});
  if (::fsync(assets_fd_) != 0)
    return Result<SourceReceipt>::failure(io_error("flush managed assets"));
  if (fail(SaveStage::assets_flushed))
    return Result<SourceReceipt>::failure({ErrorCode::io_failure, "injected asset interruption"});
  return Result<SourceReceipt>::success(SourceReceipt(std::move(*state->record), store_token_));
}

Result<int> ProjectStore::open_managed_source(const project::SourceRecord& source) const {
  if (source.external_link)
    return Result<int>::failure({ErrorCode::invalid_argument, "source is externally linked"});
#if !OSCILLATE_HAVE_CODEC
  if (source.encoding != project::SourceEncoding::wav)
    return Result<int>::failure(
        {ErrorCode::unsupported, "encoded source verification is not yet available"});
#endif
  auto assets_identity = check_assets_directory({root_fd_, assets_fd_});
  if (!assets_identity)
    return Result<int>::failure(assets_identity.error());
  auto file = checked_asset(assets_fd_, source);
  if (!file)
    return Result<int>::failure(file.error());
  struct stat initial{};
  if (::fstat(file.value().get(), &initial) != 0)
    return Result<int>::failure(io_error("stat managed source"));
  if (verified_before(initial, source))
    return Result<int>::success(file.value().release());
  auto hash = digest_file(file.value(), source.byte_length);
  if (!hash)
    return Result<int>::failure(hash.error());
  if (hash.value() != source.sha256)
    return Result<int>::failure({ErrorCode::corrupt_data, "managed source hash mismatch"});
  if (source.encoding == project::SourceEncoding::wav) {
    auto wav = media::WavReader::open_fd(file.value().get());
    if (!wav)
      return Result<int>::failure(wav.error());
    if (source.sample_rate != wav.value().sample_rate() ||
        source.channels != wav.value().channels() || source.frames != wav.value().frame_count()) {
      return Result<int>::failure({ErrorCode::corrupt_data, "managed source metadata mismatch"});
    }
  } else {
#if OSCILLATE_HAVE_CODEC
    auto inspected =
        inspect_encoded_source(source.id, file.value().get(), source.sha256, source.byte_length);
    if (!inspected)
      return Result<int>::failure(inspected.error());
    if (inspected.value() != source)
      return Result<int>::failure({ErrorCode::corrupt_data, "managed source metadata mismatch"});
#else
    return Result<int>::failure({ErrorCode::unsupported, "encoded codec is unavailable"});
#endif
  }
  struct stat verified{};
  if (::fstat(file.value().get(), &verified) != 0 || !same_file_version(initial, verified))
    return Result<int>::failure(
        {ErrorCode::conflict, "managed source changed during verification"});
  remember_verified(verified, source);
  return Result<int>::success(file.value().release());
}

namespace {

Result<std::pair<Fd, struct stat>> open_verified_external_file(const project::SourceRecord& source,
                                                               const std::stop_token& stop = {}) {
#if !OSCILLATE_HAVE_CODEC
  if (source.encoding != project::SourceEncoding::wav)
    return Result<std::pair<Fd, struct stat>>::failure(
        {ErrorCode::unsupported, "encoded source codec is unavailable"});
#endif
  auto file = checked_external_source(source);
  if (!file)
    return Result<std::pair<Fd, struct stat>>::failure(file.error());
  struct stat initial{};
  if (::fstat(file.value().get(), &initial) != 0)
    return Result<std::pair<Fd, struct stat>>::failure(io_error("stat external source"));
  auto hash = digest_file(file.value(), source.byte_length, stop);
  if (!hash)
    return Result<std::pair<Fd, struct stat>>::failure(hash.error());
  if (hash.value() != source.sha256)
    return Result<std::pair<Fd, struct stat>>::failure(
        {ErrorCode::corrupt_data, "external source hash mismatch"});
  if (source.encoding == project::SourceEncoding::wav) {
    auto wav = media::WavReader::open_fd(file.value().get());
    if (!wav)
      return Result<std::pair<Fd, struct stat>>::failure(wav.error());
    if (source.sample_rate != wav.value().sample_rate() ||
        source.channels != wav.value().channels() || source.frames != wav.value().frame_count())
      return Result<std::pair<Fd, struct stat>>::failure(
          {ErrorCode::corrupt_data, "external source metadata mismatch"});
  } else {
#if OSCILLATE_HAVE_CODEC
    auto inspected =
        inspect_encoded_source(source.id, file.value().get(), source.sha256, source.byte_length);
    if (!inspected)
      return Result<std::pair<Fd, struct stat>>::failure(inspected.error());
    auto expected = source;
    expected.external_link.reset();
    if (inspected.value() != expected)
      return Result<std::pair<Fd, struct stat>>::failure(
          {ErrorCode::corrupt_data, "external source metadata mismatch"});
#endif
  }
  struct stat verified{};
  if (::fstat(file.value().get(), &verified) != 0 || !same_file_version(initial, verified))
    return Result<std::pair<Fd, struct stat>>::failure(
        {ErrorCode::conflict, "external source changed during verification"});
  auto named = checked_external_source(source);
  struct stat named_version{};
  if (!named || ::fstat(named.value().get(), &named_version) != 0 ||
      !same_file_version(verified, named_version))
    return Result<std::pair<Fd, struct stat>>::failure(
        {ErrorCode::conflict, "external source path changed during verification"});
  return Result<std::pair<Fd, struct stat>>::success({std::move(file).value(), verified});
}

Result<struct stat> verify_new_external_source(const project::SourceRecord& source,
                                               const std::stop_token& stop = {}) {
  auto opened = open_verified_external_file(source, stop);
  return opened ? Result<struct stat>::success(opened.value().second)
                : Result<struct stat>::failure(opened.error());
}

} // namespace

struct VerifiedExternalSource::Impl {
  project::SourceRecord record;
  std::uint64_t revision;
  std::string store_token;
  struct stat version;
};

VerifiedExternalSource::VerifiedExternalSource(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
VerifiedExternalSource::VerifiedExternalSource(VerifiedExternalSource&&) noexcept = default;
VerifiedExternalSource&
VerifiedExternalSource::operator=(VerifiedExternalSource&&) noexcept = default;
VerifiedExternalSource::~VerifiedExternalSource() = default;

const project::SourceRecord& VerifiedExternalSource::record() const noexcept {
  return impl_->record;
}
std::uint64_t VerifiedExternalSource::revision() const noexcept { return impl_->revision; }

Result<VerifiedExternalSource> ExternalSourceVerifyTask::run(const std::stop_token& stop) && {
  auto verified = verify_new_external_source(record_, stop);
  if (!verified)
    return Result<VerifiedExternalSource>::failure(verified.error());
  if (stop.stop_requested())
    return Result<VerifiedExternalSource>::failure(
        {ErrorCode::cancelled, "external source verification cancelled"});
  return Result<VerifiedExternalSource>::success(VerifiedExternalSource(
      std::make_unique<VerifiedExternalSource::Impl>(VerifiedExternalSource::Impl{
          std::move(record_), revision_, std::move(store_token_), verified.value()})));
}

Result<ExternalSourceVerifyTask>
ProjectStore::begin_external_source_verification(const project::SourceRecord& source,
                                                 std::uint64_t candidate_revision) const {
  if (lock_fd_ < 0)
    return Result<ExternalSourceVerifyTask>::failure({ErrorCode::conflict, "store is read-only"});
  if (!source.external_link || candidate_revision == 0)
    return Result<ExternalSourceVerifyTask>::failure(
        {ErrorCode::invalid_argument, "external verification requires a linked source"});
  return Result<ExternalSourceVerifyTask>::success(
      ExternalSourceVerifyTask(source, candidate_revision, store_token_));
}

Result<int> ProjectStore::open_verified_source(const project::SourceRecord& source,
                                               const std::stop_token& stop) const {
  if (!source.external_link)
    return open_managed_source(source);
  if (stop.stop_requested())
    return Result<int>::failure({ErrorCode::cancelled, "external source open cancelled"});
  auto current = load_current();
  if (!current)
    return Result<int>::failure(current.error());
  if (std::ranges::none_of(current.value().snapshot.sources,
                           [&](const auto& saved) { return saved == source; }))
    return Result<int>::failure(
        {ErrorCode::conflict, "external source link differs from saved project"});
  auto verified = open_verified_external_file(source, stop);
  if (!verified)
    return Result<int>::failure(verified.error());
  return Result<int>::success(verified.value().first.release());
}

Result<int> ProjectStore::open_verified_generation_source(std::string_view generation,
                                                          const project::SourceRecord& source,
                                                          const std::stop_token& stop) const {
  if (stop.stop_requested())
    return Result<int>::failure({ErrorCode::cancelled, "source open cancelled"});
  auto selected = load_generation(std::string(generation), std::nullopt);
  if (!selected)
    return Result<int>::failure(selected.error());
  if (!references_source(selected.value(), source))
    return Result<int>::failure(
        {ErrorCode::conflict, "saved generation does not reference the source"});
  if (!source.external_link)
    return open_managed_source(source);
  auto verified = open_verified_external_file(source, stop);
  if (!verified)
    return Result<int>::failure(verified.error());
  return Result<int>::success(verified.value().first.release());
}

Result<RestoreStatus> ProjectStore::restore_managed_source(std::string_view generation,
                                                           const project::SourceRecord& expected,
                                                           int candidate_fd,
                                                           const std::stop_token& stop) {
  std::unique_lock save_lock(*save_gate_, std::try_to_lock);
  if (!save_lock.owns_lock())
    return Result<RestoreStatus>::failure(
        {ErrorCode::conflict, "project save already in progress"});
  if (lock_fd_ < 0)
    return Result<RestoreStatus>::failure({ErrorCode::conflict, "store is read-only"});
  if (expected.byte_length > max_restore_source_bytes)
    return Result<RestoreStatus>::failure(
        {ErrorCode::resource_exhausted, "managed source restore exceeds byte limit"});
  auto selected = load_generation(std::string(generation), std::nullopt);
  if (!selected)
    return Result<RestoreStatus>::failure(selected.error());
  if (!references_source(selected.value(), expected))
    return Result<RestoreStatus>::failure(
        {ErrorCode::conflict, "selected generation does not reference the source"});
  struct stat candidate{};
  if (candidate_fd < 0 || ::fstat(candidate_fd, &candidate) != 0 || !S_ISREG(candidate.st_mode) ||
      candidate.st_size < 0)
    return Result<RestoreStatus>::failure(
        {ErrorCode::invalid_argument, "restore candidate must be a regular file"});
  if (static_cast<std::uint64_t>(candidate.st_size) != expected.byte_length)
    return Result<RestoreStatus>::failure(
        {ErrorCode::conflict, "restore candidate length differs from saved source"});
  auto missing = checked_asset(assets_fd_, expected);
  if (missing)
    return Result<RestoreStatus>::failure({ErrorCode::conflict, "managed source already exists"});
  if (missing.error().code != ErrorCode::not_found)
    return Result<RestoreStatus>::failure(missing.error());
  Result<WavStageTask> task = expected.encoding == project::SourceEncoding::wav
                                  ? begin_wav_stage(expected.id, candidate_fd)
                                  : begin_encoded_stage(expected.id, candidate_fd);
  if (!task)
    return Result<RestoreStatus>::failure(task.error());
  task.value().impl_->required_length = expected.byte_length;
  auto staged = std::move(task).value().run(stop);
  if (!staged)
    return Result<RestoreStatus>::failure(staged.error());
  if (staged.value().record() != expected)
    return Result<RestoreStatus>::failure(
        {ErrorCode::conflict, "restore candidate differs from saved source"});
  if (stop.stop_requested())
    return Result<RestoreStatus>::failure({ErrorCode::cancelled, "source restore cancelled"});
  selected = load_generation(std::string(generation), std::nullopt);
  if (!selected || !references_source(selected.value(), expected))
    return Result<RestoreStatus>::failure(
        {ErrorCode::conflict, "selected generation changed during source restore"});
  auto generations_identity = check_directory_identity(state_fd_, "generations", generations_fd_);
  if (!generations_identity)
    return Result<RestoreStatus>::failure(generations_identity.error());
  auto published = publish_encoded_stage_unlocked(std::move(staged).value());
  if (!published) {
    if (published.error().code == ErrorCode::io_failure) {
      auto visible = open_managed_source(expected);
      if (visible) {
        ::close(visible.value());
        return Result<RestoreStatus>::success(RestoreStatus::uncertain);
      }
    }
    return Result<RestoreStatus>::failure(published.error());
  }
  auto verified = open_managed_source(expected);
  if (!verified)
    return Result<RestoreStatus>::failure(verified.error());
  ::close(verified.value());
  return Result<RestoreStatus>::success(RestoreStatus::committed);
}

Result<int> ProjectStore::open_cache_directory() const {
  auto directory = open_directory(root_fd_, "cache");
  if (!directory)
    return Result<int>::failure(directory.error());
  return Result<int>::success(directory.value().release());
}

Result<ProjectStore> ProjectStore::open_writer(const std::filesystem::path& root, bool create,
                                               FaultHook fault) {
  Fd root_fd;
  if (create) {
    const auto parent_path =
        root.parent_path().empty() ? std::filesystem::path{"."} : root.parent_path();
    const auto child = root.filename().string();
    if (child.empty() || child == "." || child == "..") {
      return Result<ProjectStore>::failure(
          {ErrorCode::invalid_argument, "project root must name a directory"});
    }
    Fd parent(::open(parent_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!parent)
      return Result<ProjectStore>::failure(io_error("open project parent"));
    if (::mkdirat(parent.get(), child.c_str(), 0700) != 0 && errno != EEXIST) {
      return Result<ProjectStore>::failure(io_error("create project root"));
    }
    // The new root's directory entry belongs to the parent, not the root.
    // Repeat this flush when create=true on an existing root so an earlier
    // interrupted attempt cannot be mistaken for a durable project path.
    if (::fsync(parent.get()) != 0) {
      return Result<ProjectStore>::failure(io_error("flush project parent"));
    }
    if (fault && fault(SaveStage::root_parent_flushed)) {
      return Result<ProjectStore>::failure(
          {ErrorCode::io_failure, "injected project-root interruption"});
    }
    root_fd =
        Fd(::openat(parent.get(), child.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  } else {
    root_fd = Fd(::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  }
  if (!root_fd)
    return Result<ProjectStore>::failure(io_error("open project root"));
  for (const auto* name : {"assets", "state"}) {
    auto made = ensure_directory(root_fd.get(), name, create);
    if (!made)
      return Result<ProjectStore>::failure(made.error());
    auto checked = open_directory(root_fd.get(), name);
    if (!checked)
      return Result<ProjectStore>::failure(checked.error());
  }
  // cache/ is disposable: reopening a valid project restores it if deleted.
  auto cache_made = ensure_directory(root_fd.get(), "cache", true);
  if (!cache_made)
    return Result<ProjectStore>::failure(cache_made.error());
  auto cache_checked = open_directory(root_fd.get(), "cache");
  if (!cache_checked)
    return Result<ProjectStore>::failure(cache_checked.error());
  auto state_fd = open_directory(root_fd.get(), "state");
  auto assets_fd = open_directory(root_fd.get(), "assets");
  if (!assets_fd)
    return Result<ProjectStore>::failure(assets_fd.error());
  if (!state_fd)
    return Result<ProjectStore>::failure(state_fd.error());
  auto made = ensure_directory(state_fd.value().get(), "generations", create);
  if (!made)
    return Result<ProjectStore>::failure(made.error());
  auto generations_fd = open_directory(state_fd.value().get(), "generations");
  if (!generations_fd)
    return Result<ProjectStore>::failure(generations_fd.error());
  Fd lock_fd(::openat(state_fd.value().get(), "WRITER.lock",
                      O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600));
  if (!lock_fd)
    return Result<ProjectStore>::failure(io_error("open writer lock"));
  struct stat lock_status{};
  if (::fstat(lock_fd.get(), &lock_status) != 0 || !S_ISREG(lock_status.st_mode)) {
    return Result<ProjectStore>::failure({ErrorCode::corrupt_data, "invalid writer lock"});
  }
  if (::flock(lock_fd.get(), LOCK_EX | LOCK_NB) != 0) {
    return Result<ProjectStore>::failure({ErrorCode::conflict, "another project writer is active"});
  }
  auto lock_identity = check_writer_lock({state_fd.value().get(), lock_fd.get()});
  if (!lock_identity)
    return Result<ProjectStore>::failure(lock_identity.error());
  auto cleaned = clean_abandoned_wav_stages(assets_fd.value().get());
  if (!cleaned)
    return Result<ProjectStore>::failure(cleaned.error());
  cleaned = clean_abandoned_pointer_stages(state_fd.value().get());
  if (!cleaned)
    return Result<ProjectStore>::failure(cleaned.error());
  cleaned = clean_abandoned_generation_stages(generations_fd.value().get());
  if (!cleaned)
    return Result<ProjectStore>::failure(cleaned.error());
  if (create && (::fsync(state_fd.value().get()) != 0 || ::fsync(root_fd.get()) != 0)) {
    return Result<ProjectStore>::failure(io_error("flush project directory layout"));
  }
  auto token = random_name();
  if (!token)
    return Result<ProjectStore>::failure(token.error());
  ProjectStore store({root_fd.release(), assets_fd.value().release(), state_fd.value().release(),
                      generations_fd.value().release(), lock_fd.release()},
                     std::move(fault));
  store.store_token_ = std::move(token).value();
  return Result<ProjectStore>::success(std::move(store));
}

Result<ProjectStore> ProjectStore::open_reader(const std::filesystem::path& root) {
  Fd root_fd(::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (!root_fd)
    return Result<ProjectStore>::failure(io_error("open project root"));
  auto state_fd = open_directory(root_fd.get(), "state");
  auto assets_fd = open_directory(root_fd.get(), "assets");
  if (!assets_fd)
    return Result<ProjectStore>::failure(assets_fd.error());
  if (!state_fd)
    return Result<ProjectStore>::failure(state_fd.error());
  auto generations_fd = open_directory(state_fd.value().get(), "generations");
  if (!generations_fd)
    return Result<ProjectStore>::failure(generations_fd.error());
  return Result<ProjectStore>::success(
      ProjectStore({root_fd.release(), assets_fd.value().release(), state_fd.value().release(),
                    generations_fd.value().release(), -1},
                   {}));
}

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
  auto recorded = read_bounded(directory.value().get(), "manifest.sha256", 65);
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
  auto recorded = read_bounded(directory.value().get(), "manifest.sha256", 65);
  if (!recorded)
    return Result<SaveOutcome>::failure(recorded.error());
  if (recorded.value().size() != 65 || recorded.value().back() != '\n')
    return Result<SaveOutcome>::failure({ErrorCode::corrupt_data, "invalid generation checksum"});
  const auto hash = recorded.value().substr(0, 64);
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
  auto pointer_bytes = read_bounded(state_fd_, "CURRENT", max_pointer_bytes);
  if (!pointer_bytes)
    return Result<SaveOutcome>::failure(pointer_bytes.error());
  auto current = detail::decode_pointer(pointer_bytes.value());
  if (!current)
    return Result<SaveOutcome>::failure(current.error());
  auto generation = load_generation(current.value().first, current.value().second);
  if (!generation)
    return Result<SaveOutcome>::failure(generation.error());
  if (generation.value().kind != SaveKind::explicit_save)
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
                      detail::encode_named_pointer(
                          {std::string(name), current.value().first, current.value().second}));
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
    return Result<SaveOutcome>::success({SaveStatus::uncertain, current.value().first});
  if (::fsync(revisions.value().get()) != 0)
    return Result<SaveOutcome>::success({SaveStatus::uncertain, current.value().first});
  return Result<SaveOutcome>::success({SaveStatus::committed, current.value().first});
}

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
