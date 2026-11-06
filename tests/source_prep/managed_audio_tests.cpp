#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"

#include <oscillate/persistence/store.hpp>
#include <oscillate/project/model.hpp>
#include <oscillate/source_prep/managed_audio.hpp>
#include <oscillate/timeline/timing.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {
namespace source_prep = oscillate::source_prep;
namespace foundation = oscillate::foundation;
namespace persistence = oscillate::persistence;
namespace project = oscillate::project;
namespace timeline = oscillate::timeline;
namespace fs = std::filesystem;

using oscillate::test::require;

using oscillate::test::take;
template <typename T> T id(std::uint64_t low) { return take(T::from_bits({0, low})); }

std::size_t open_descriptor_count() {
  return static_cast<std::size_t>(std::ranges::distance(fs::directory_iterator("/proc/self/fd")));
}

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-managed-audio") {}
};

void write_u16(std::ofstream& output, std::uint16_t value) {
  const std::array<char, 2> bytes{static_cast<char>(value), static_cast<char>(value >> 8U)};
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}
void write_u32(std::ofstream& output, std::uint32_t value) {
  const std::array<char, 4> bytes{static_cast<char>(value), static_cast<char>(value >> 8U),
                                  static_cast<char>(value >> 16U), static_cast<char>(value >> 24U)};
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void write_wav(const fs::path& path, bool nonfinite = false) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  require(output.is_open(), "cannot create WAV fixture");
  constexpr std::uint32_t frames = 4;
  constexpr std::uint32_t frame_bytes = 4;
  output.write("RIFF", 4);
  write_u32(output, 36 + frames * frame_bytes);
  output.write("WAVEfmt ", 8);
  write_u32(output, 16);
  write_u16(output, nonfinite ? 3 : 1);
  write_u16(output, nonfinite ? 1 : 2);
  write_u32(output, 48000);
  write_u32(output, 48000 * frame_bytes);
  write_u16(output, frame_bytes);
  write_u16(output, nonfinite ? 32 : 16);
  output.write("data", 4);
  write_u32(output, frames * frame_bytes);
  for (std::uint32_t index = 0; index < frames; ++index) {
    if (nonfinite) {
      write_u32(output, index == 2 ? 0x7fc00000U : std::bit_cast<std::uint32_t>(0.25F));
    } else {
      write_u16(output, static_cast<std::uint16_t>((index + 1) * 1000));
      write_u16(output, static_cast<std::uint16_t>(-static_cast<std::int32_t>((index + 1) * 1000)));
    }
  }
  output.flush();
  require(output.good(), "cannot complete WAV fixture");
}

struct ManagedFixture {
  Directory temp;
  fs::path root;
  project::SourceRecord source;

  explicit ManagedFixture(bool nonfinite = false)
      : root(temp.path() / "project"),
        source{id<project::SourceId>(4), project::SourceEncoding::wav, "", 0, 0, 0, 0} {
    const auto external = temp.path() / "external.wav";
    write_wav(external, nonfinite);
    auto writer = take(persistence::ProjectStore::open_writer(root, true));
    const int input = ::open(external.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    REQUIRE_GE(input, 0, "cannot open WAV fixture");
    auto staged = writer.stage_wav(id<project::SourceId>(4), input);
    ::close(input);
    auto receipt = take(std::move(staged));
    source = receipt.record();
    auto tempo = take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1));
    auto meter = take(timeline::MeterMap::create({{{0}, 4, 4}}, 1));
    project::Snapshot snapshot{id<project::ProjectId>(1), 1,  48000,   std::move(tempo),
                               std::move(meter),          {}, {source}};
    std::array<persistence::SourceReceipt, 1> receipts{std::move(receipt)};
    REQUIRE_OK(writer.save(snapshot, persistence::SaveKind::explicit_save, receipts),
               "cannot save managed source fixture");
    fs::remove(external);
  }
  [[nodiscard]] fs::path asset() const { return root / "assets" / (source.sha256 + ".wav"); }
};

void pinned_source_and_limits() {
  ManagedFixture fixture;
  auto reader = take(persistence::ProjectStore::open_reader(fixture.root));
  const auto descriptors_before = open_descriptor_count();
  auto loaded = take(reader.load_current());
  REQUIRE_EQ(loaded.snapshot.sources, std::vector<project::SourceRecord>{fixture.source},
             "fixture source record did not survive save/reopen");
  auto source = source_prep::load_managed_audio(reader, loaded.snapshot.sources.front());
  REQUIRE_TRUE(source, "managed source did not decode at its original rate");
  REQUIRE_EQ(source.value().source_rate, 48000,
             "managed source did not decode at its original rate");
  REQUIRE_EQ(source.value().frames.size(), 4, "managed source did not decode at its original rate");
  for (std::size_t index = 0; index < 4; ++index) {
    const float expected = static_cast<float>((index + 1) * 1000) / 32768.0F;
    REQUIRE_LT(std::fabs(source.value().frames[index].left - expected), 0.00001F,
               "decoded frame differs from pinned source");
    REQUIRE_LT(std::fabs(source.value().frames[index].right + expected), 0.00001F,
               "decoded frame differs from pinned source");
  }
  const auto descriptors_after = open_descriptor_count();
  REQUIRE_EQ(descriptors_after, descriptors_before,
             "successful decode retained its pinned source descriptor");
  require(!source_prep::load_managed_audio(reader, fixture.source, 0),
          "zero frame budget accepted");
  require(!source_prep::load_managed_audio(reader, fixture.source,
                                           source_prep::max_managed_audio_frames + 1),
          "oversized caller budget accepted");
  auto bounded = source_prep::load_managed_audio(reader, fixture.source, 3);
  REQUIRE_TRUE(!bounded, "source larger than caller budget was allocated");
  REQUIRE_EQ(bounded.error().code, foundation::ErrorCode::resource_exhausted,
             "source larger than caller budget was allocated");
  auto empty_record = fixture.source;
  empty_record.frames = 0;
  auto empty = source_prep::load_managed_audio(reader, empty_record);
  REQUIRE_TRUE(!empty, "empty source record was accepted");
  REQUIRE_EQ(empty.error().code, foundation::ErrorCode::corrupt_data,
             "empty source record was accepted");
  auto wrong_frames = fixture.source;
  wrong_frames.frames = 5;
  auto mismatch = source_prep::load_managed_audio(reader, wrong_frames);
  REQUIRE_TRUE(!mismatch, "project source metadata mismatch was accepted");
  REQUIRE_EQ(mismatch.error().code, foundation::ErrorCode::corrupt_data,
             "project source metadata mismatch was accepted");
}

void missing_and_damaged_source() {
  ManagedFixture fixture;
  auto reader = take(persistence::ProjectStore::open_reader(fixture.root));
  const auto descriptors_before = open_descriptor_count();
  {
    std::fstream damaged(fixture.asset(), std::ios::in | std::ios::out | std::ios::binary);
    damaged.seekp(46);
    damaged.put('\x7f');
  }
  auto corrupt = source_prep::load_managed_audio(reader, fixture.source);
  REQUIRE_TRUE(!corrupt, "damaged managed asset bypassed integrity check");
  REQUIRE_EQ(corrupt.error().code, foundation::ErrorCode::corrupt_data,
             "damaged managed asset bypassed integrity check");
  fs::remove(fixture.asset());
  auto missing = source_prep::load_managed_audio(reader, fixture.source);
  REQUIRE_TRUE(!missing, "missing source lost its actionable store diagnostic");
  REQUIRE_EQ(missing.error().code, foundation::ErrorCode::not_found,
             "missing source lost its actionable store diagnostic");
  REQUIRE_EQ(missing.error().message, "managed source missing",
             "missing source lost its actionable store diagnostic");
  auto loaded = take(reader.load_current());
  REQUIRE_EQ(loaded.snapshot.sources, std::vector<project::SourceRecord>{fixture.source},
             "source failure changed project intent");
  REQUIRE_EQ(open_descriptor_count(), descriptors_before,
             "failed decode retained a source descriptor");
}

void nonfinite_source() {
  ManagedFixture fixture(true);
  auto reader = take(persistence::ProjectStore::open_reader(fixture.root));
  auto decoded = source_prep::load_managed_audio(reader, fixture.source);
  REQUIRE_TRUE(!decoded, "non-finite managed WAV sample reached decoded audio");
  REQUIRE_EQ(decoded.error().code, foundation::ErrorCode::corrupt_data,
             "non-finite managed WAV sample reached decoded audio");
}

#if OSCILLATE_HAVE_CODEC
void encoded_managed_source() {
  for (const auto name : {"sine.aiff", "sine.flac", "sine.mp3"}) {
    Directory temp;
    const auto root = temp.path() / "project";
    const auto external = temp.path() / name;
    fs::copy_file(fs::path(OSCILLATE_CODEC_FIXTURE_DIR) / name, external);
    {
      auto writer = take(persistence::ProjectStore::open_writer(root, true));
      const int input = ::open(external.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
      REQUIRE_GE(input, 0, "cannot open encoded fixture");
      auto begun = writer.begin_encoded_stage(id<project::SourceId>(4), input);
      ::close(input);
      auto stage = take(std::move(begun));
      auto staged = take(std::move(stage).run());
      auto receipt = take(writer.publish_encoded_stage(std::move(staged)));
      auto tempo = take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1));
      auto meter = take(timeline::MeterMap::create({{{0}, 4, 4}}, 1));
      project::Snapshot snapshot{
          id<project::ProjectId>(1), 1, 48'000, std::move(tempo), std::move(meter), {},
          {receipt.record()}};
      std::array<persistence::SourceReceipt, 1> receipts{std::move(receipt)};
      REQUIRE_OK(writer.save(snapshot, persistence::SaveKind::explicit_save, receipts));
    }
    fs::remove(external);
    auto reader = take(persistence::ProjectStore::open_reader(root));
    const auto loaded = take(reader.load_current());
    const auto& record = loaded.snapshot.sources.front();
    auto prepared = take(source_prep::load_managed_audio(reader, record));
    REQUIRE_EQ(prepared.source_rate, 48'000, "encoded source lost its saved metadata");
    REQUIRE_EQ(prepared.frames.size(), record.frames, "encoded source lost its saved metadata");
    REQUIRE_GE(prepared.frames.size(), 960, "encoded source lost its saved metadata");
    REQUIRE_LT(std::fabs(prepared.frames[12].left - 0.125F), 0.02F,
               "encoded managed audio differs from sine oracle");
    REQUIRE_EQ(prepared.frames[12].left, prepared.frames[12].right,
               "encoded managed audio differs from sine oracle");
  }
}
#endif
} // namespace

int main() {
  try {
    pinned_source_and_limits();
    missing_and_damaged_source();
    nonfinite_source();
#if OSCILLATE_HAVE_CODEC
    encoded_managed_source();
#endif
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
