#include "oscillate/persistence/store.hpp"

#include "store_support.hpp"

#include "oscillate/media/prefetch.hpp"
#include "private.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

// Source staging: WAV and encoded sources copied into managed assets and
// published, and a managed source restored from a generation.
namespace oscillate::persistence {
namespace {
using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;
using detail::asset_name;
using detail::check_assets_directory;
using detail::check_directory_identity;
using detail::check_writer_lock;
using detail::checked_asset;
using detail::digest_file;
using detail::Fd;
using detail::injected;
using detail::max_restore_source_bytes;
using detail::random_name;
using detail::references_source;
using detail::same_file_version;
using foundation::io_error;
#if OSCILLATE_HAVE_CODEC
using detail::inspect_encoded_source;
#endif
using detail::random_name;

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

} // namespace oscillate::persistence
