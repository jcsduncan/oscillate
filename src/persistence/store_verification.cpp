#include "oscillate/persistence/store.hpp"

#include "store_support.hpp"

#include "oscillate/media/prefetch.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <utility>
#include <vector>

// Source integrity verification: opening managed and external sources
// only after their identity and digest check out.
namespace oscillate::persistence {
namespace {
using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;
using detail::check_assets_directory;
using detail::checked_asset;
using detail::checked_external_source;
using detail::digest_file;
using detail::Fd;
using detail::references_source;
using detail::same_file_version;
using foundation::io_error;
#if OSCILLATE_HAVE_CODEC
using detail::inspect_encoded_source;
#endif
using detail::verify_new_external_source;

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

} // namespace

namespace detail {

Result<struct stat> verify_new_external_source(const project::SourceRecord& source,
                                               const std::stop_token& stop) {
  auto opened = open_verified_external_file(source, stop);
  return opened ? Result<struct stat>::success(opened.value().second)
                : Result<struct stat>::failure(opened.error());
}

} // namespace detail

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

} // namespace oscillate::persistence
