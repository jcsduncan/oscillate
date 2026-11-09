#include "store_support.hpp"

#include "oscillate/foundation/hex.hpp"
#include "oscillate/foundation/sha256.hpp"
#include "private.hpp"

#if OSCILLATE_HAVE_CODEC
#include "oscillate/codec/encoded_reader.hpp"
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace oscillate::persistence::detail {
namespace {
using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;
using foundation::io_error;

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

bool valid_sha256(std::string_view value) {
  return value.size() == sha256_hex_digits && std::ranges::all_of(value, [](char ch) {
           return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         });
}

} // namespace

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

Result<Fd> open_directory(int parent, const char* name) {
  Fd fd(::openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (!fd) {
    return Result<Fd>::failure(io_error("open project directory"));
  }
  return Result<Fd>::success(std::move(fd));
}

Result<std::string> read_bounded(int parent, const char* name, std::size_t max_size,
                                 std::size_t* remaining_budget) {
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

Result<void> write_sync_file(int parent, const std::string& name, std::string_view bytes) {
  return foundation::write_new_synced(
      parent, name, std::as_bytes(std::span(bytes.data(), bytes.size())), "project file");
}

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

std::string asset_name(const project::SourceRecord& source) {
  return source.sha256 + std::string(source_extension(source.encoding));
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

Result<std::string> digest_file(const Fd& fd, std::uint64_t length, const std::stop_token& stop) {
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

} // namespace oscillate::persistence::detail
