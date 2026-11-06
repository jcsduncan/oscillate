#include "oscillate/codec/encoded_reader.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>

namespace {
namespace fs = std::filesystem;
using namespace oscillate;
using namespace oscillate::persistence;

using oscillate::test::require;

using oscillate::test::take;

template <typename T> T id(std::uint64_t low) { return take(T::from_bits({0, low})); }

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-encoded-store") {}
};

class InputFd {
public:
  explicit InputFd(const fs::path& path) : fd_(::open(path.c_str(), O_RDONLY | O_CLOEXEC)) {
    REQUIRE_GE(fd_, 0, "fixture input could not be opened");
  }
  ~InputFd() { ::close(fd_); }
  InputFd(const InputFd&) = delete;
  InputFd& operator=(const InputFd&) = delete;
  [[nodiscard]] int get() const { return fd_; }

private:
  int fd_;
};

[[nodiscard]] fs::path fixture(std::string_view name) {
  return fs::path(OSCILLATE_CODEC_FIXTURE_DIR) / name;
}

[[nodiscard]] project::Snapshot snapshot() {
  auto tempo = take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1));
  auto meter = take(timeline::MeterMap::create({{{0}, 4, 4}}, 1));
  return {id<project::ProjectId>(1), 1, 48'000, std::move(tempo), std::move(meter), {}};
}

void stage_save_reopen(std::string_view name, project::SourceEncoding expected,
                       std::string_view expected_hash) {
  Directory temp;
  const auto input = temp.path() / "arbitrary.input";
  fs::copy_file(fixture(name), input);
  InputFd fd(input);
  auto writer = take(ProjectStore::open_writer(temp.path() / "project", true));
  auto stage = take(writer.begin_encoded_stage(id<project::SourceId>(2), fd.get()));
  auto staged = take(std::move(stage).run());
  REQUIRE_EQ(staged.record().encoding, expected,
             "staging did not preserve encoded identity and metadata");
  REQUIRE_EQ(staged.record().sha256, expected_hash,
             "staging did not preserve encoded identity and metadata");
  REQUIRE_EQ(staged.record().byte_length, fs::file_size(input),
             "staging did not preserve encoded identity and metadata");
  REQUIRE_EQ(staged.record().sample_rate, 48'000,
             "staging did not preserve encoded identity and metadata");
  REQUIRE_EQ(staged.record().channels, 1, "staging did not preserve encoded identity and metadata");
  REQUIRE_GE(staged.record().frames, 960, "staging did not preserve encoded identity and metadata");
  auto receipt = take(writer.publish_encoded_stage(std::move(staged)));
  auto state = snapshot();
  state.sources.push_back(receipt.record());
  const auto without_receipt = writer.save(state, SaveKind::explicit_save);
  REQUIRE_TRUE(!without_receipt, "unreceipted encoded source was saved");
  REQUIRE_EQ(without_receipt.error().code, foundation::ErrorCode::conflict,
             "unreceipted encoded source was saved");
  std::array<SourceReceipt, 1> receipts{std::move(receipt)};
  REQUIRE_OK(writer.save(state, SaveKind::explicit_save, receipts));
  auto duplicate =
      take(std::move(take(writer.begin_encoded_stage(id<project::SourceId>(3), fd.get()))).run());
  REQUIRE_EQ(duplicate.record().sha256, state.sources[0].sha256,
             "identical encoded source did not retain its identity");
  REQUIRE_EQ(duplicate.record().encoding, expected,
             "identical encoded source did not retain its identity");
  REQUIRE_OK(writer.publish_encoded_stage(std::move(duplicate)));
  const auto asset =
      temp.path() / "project" / "assets" /
      (std::string(expected_hash) + "." + std::string(name.substr(name.find('.') + 1)));
  REQUIRE_TRUE(fs::is_regular_file(asset),
               "managed encoded bytes were not published under their detected tag");
  REQUIRE_EQ(fs::file_size(asset), state.sources[0].byte_length,
             "managed encoded bytes were not published under their detected tag");
  REQUIRE_EQ(std::distance(fs::directory_iterator(asset.parent_path()), fs::directory_iterator{}),
             1, "deduplicated encoded source created another managed asset");
  fs::remove(input);
  auto reader = take(ProjectStore::open_reader(temp.path() / "project"));
  const auto loaded = take(reader.load_current());
  REQUIRE_EQ(loaded.snapshot.sources, state.sources,
             "encoded source did not reopen without the original path");
  REQUIRE_TRUE(loaded.source_diagnostics.empty(),
               "encoded source did not reopen without the original path");
  const auto pinned = take(reader.open_managed_source(state.sources[0]));
  auto decoded = take(codec::EncodedReader::open_fd(pinned));
  ::close(pinned);
  std::array<codec::StereoFrame, 16> audio{};
  REQUIRE_EQ(decoded.read_at(0, audio).value(), audio.size(),
             "verified managed source did not decode");
  auto incorrect = state.sources[0];
  ++incorrect.frames;
  require(!reader.open_managed_source(incorrect), "mismatched encoded metadata was accepted");
  {
    std::fstream changed(asset, std::ios::binary | std::ios::in | std::ios::out);
    changed.seekp(100);
    changed.put('\x7f');
  }
  require(!reader.open_managed_source(state.sources[0]),
          "same-size encoded asset tampering passed digest verification");
  fs::remove(asset);
  fs::create_symlink(fixture(name), asset);
  require(!reader.open_managed_source(state.sources[0]),
          "symlink replacement of encoded asset passed no-follow verification");
}

void reject_bad_sources_and_stages() {
  Directory temp;
  auto writer = take(ProjectStore::open_writer(temp.path() / "project", true));
  InputFd flac(fixture("sine.flac"));
  std::stop_source cancel;
  cancel.request_stop();
  auto cancelled = take(writer.begin_encoded_stage(id<project::SourceId>(3), flac.get()));
  const auto cancelled_result = std::move(cancelled).run(cancel.get_token());
  REQUIRE_TRUE(!cancelled_result, "cancelled encoded import was published");
  REQUIRE_EQ(cancelled_result.error().code, foundation::ErrorCode::cancelled,
             "cancelled encoded import was published");
  const auto assets = temp.path() / "project" / "assets";
  require(fs::is_empty(assets), "cancelled import left a staged asset");

  for (const auto name : {"unsupported.ogg", "stereo.wav"}) {
    InputFd unsupported(fixture(name));
    auto task = take(writer.begin_encoded_stage(id<project::SourceId>(4), unsupported.get()));
    const auto rejected = std::move(task).run();
    REQUIRE_TRUE(!rejected, "unsupported or WAV input entered the encoded staging path");
    REQUIRE_EQ(rejected.error().code, foundation::ErrorCode::unsupported,
               "unsupported or WAV input entered the encoded staging path");
    require(fs::is_empty(assets), "rejected import left a staged asset");
  }
  const auto malformed = temp.path() / "malformed.flac";
  {
    std::ofstream out(malformed, std::ios::binary);
    out << "fLaCnot-a-valid-stream";
  }
  InputFd bad(malformed);
  auto task = take(writer.begin_encoded_stage(id<project::SourceId>(5), bad.get()));
  require(!std::move(task).run(), "malformed encoded media was staged");
  require(fs::is_empty(assets), "malformed import left a staged asset");

  {
    auto wrong_api = take(
        std::move(take(writer.begin_encoded_stage(id<project::SourceId>(6), flac.get()))).run());
    require(!writer.publish_wav_stage(std::move(wrong_api)),
            "encoded stage entered the WAV-only publication API");
  }
  require(fs::is_empty(assets), "rejected WAV publication left a staged asset");
  auto prepared =
      take(std::move(take(writer.begin_encoded_stage(id<project::SourceId>(6), flac.get()))).run());
  for (const auto& entry : fs::directory_iterator(assets)) {
    if (entry.path().filename().string().starts_with(".stage-")) {
      std::fstream changed(entry.path(), std::ios::binary | std::ios::in | std::ios::out);
      changed.seekp(100);
      changed.put('\x7f');
    }
  }
  const auto changed = writer.publish_encoded_stage(std::move(prepared));
  REQUIRE_TRUE(!changed, "changed staged bytes received a publication receipt");
  REQUIRE_EQ(changed.error().code, foundation::ErrorCode::conflict,
             "changed staged bytes received a publication receipt");

  const auto input = temp.path() / "pinned.flac";
  fs::copy_file(fixture("sine.flac"), input);
  InputFd pinned(input);
  auto held = take(writer.begin_encoded_stage(id<project::SourceId>(7), pinned.get()));
  fs::rename(input, temp.path() / "original.flac");
  fs::copy_file(fixture("unsupported.ogg"), input);
  const auto staged = take(std::move(held).run());
  REQUIRE_EQ(staged.record().encoding, project::SourceEncoding::flac,
             "path replacement changed the pinned encoded input");
}

void receipt_and_interrupted_save_are_store_bound() {
  Directory temp;
  InputFd flac(fixture("sine.flac"));
  auto first = take(ProjectStore::open_writer(temp.path() / "first", true));
  auto second = take(ProjectStore::open_writer(temp.path() / "second", true));
  auto foreign =
      take(std::move(take(first.begin_encoded_stage(id<project::SourceId>(2), flac.get()))).run());
  const auto rejected = second.publish_encoded_stage(std::move(foreign));
  REQUIRE_TRUE(!rejected, "foreign writer published an encoded stage");
  REQUIRE_EQ(rejected.error().code, foundation::ErrorCode::conflict,
             "foreign writer published an encoded stage");
  require(fs::is_empty(temp.path() / "second" / "assets"),
          "foreign publication wrote an asset to another project");

  auto faulted =
      take(ProjectStore::open_writer(temp.path() / "interrupted", true, [](SaveStage stage) {
        return stage == SaveStage::manifest_written;
      }));
  auto local = take(
      std::move(take(faulted.begin_encoded_stage(id<project::SourceId>(2), flac.get()))).run());
  auto receipt = take(faulted.publish_encoded_stage(std::move(local)));
  auto state = snapshot();
  state.sources.push_back(receipt.record());
  std::array<SourceReceipt, 1> receipts{std::move(receipt)};
  const auto interrupted = faulted.save(state, SaveKind::explicit_save, receipts);
  REQUIRE_TRUE(!interrupted, "interrupted encoded save advanced the project pointer");
  REQUIRE_EQ(interrupted.error().code, foundation::ErrorCode::io_failure,
             "interrupted encoded save advanced the project pointer");
  const auto current = faulted.load_current();
  REQUIRE_TRUE(!current, "interrupted encoded save became CURRENT");
  REQUIRE_EQ(current.error().code, foundation::ErrorCode::not_found,
             "interrupted encoded save became CURRENT");
}

void missing_encoded_source_restores_from_exact_candidate() {
  Directory temp;
  const auto root = temp.path() / "project";
  InputFd flac(fixture("sine.flac"));
  auto writer = take(ProjectStore::open_writer(root, true));
  auto staged =
      take(std::move(take(writer.begin_encoded_stage(id<project::SourceId>(2), flac.get()))).run());
  auto receipt = take(writer.publish_encoded_stage(std::move(staged)));
  auto state = snapshot();
  state.sources.push_back(receipt.record());
  std::array<SourceReceipt, 1> receipts{std::move(receipt)};
  const auto saved = take(writer.save(state, SaveKind::explicit_save, receipts));
  const auto asset = root / "assets" / (state.sources[0].sha256 + ".flac");
  fs::remove(asset);
  REQUIRE_EQ(take(writer.load_current()).source_diagnostics.size(), 1,
             "missing FLAC was not diagnosed");
  REQUIRE_EQ(take(writer.restore_managed_source(saved.generation, state.sources[0], flac.get())),
             RestoreStatus::committed, "matching encoded bytes did not restore saved identity");
  auto reader = take(ProjectStore::open_reader(root));
  require(take(reader.load_current()).source_diagnostics.empty(),
          "restored encoded asset still reported missing");
  const int verified = take(reader.open_managed_source(state.sources[0]));
  ::close(verified);
}

} // namespace

int main() {
  try {
    stage_save_reopen("sine.aiff", project::SourceEncoding::aiff,
                      "b20dfead2878ed5e6c2130fc12f97a2d531ae78458835ad8cb317416a0f35938");
    stage_save_reopen("sine.flac", project::SourceEncoding::flac,
                      "2adab4cc0289e2a5a3df2791388ac9465845bf8cdd796cec9bf40d862630df61");
    stage_save_reopen("sine.mp3", project::SourceEncoding::mp3,
                      "bf62c32b101636811ad8d05f9e21d87765a0b738d038d5c43f0869e354de1389");
    reject_bad_sources_and_stages();
    receipt_and_interrupted_save_are_store_bound();
    missing_encoded_source_restores_from_exact_candidate();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
