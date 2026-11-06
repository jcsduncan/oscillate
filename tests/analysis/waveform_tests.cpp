#include "oscillate/analysis/waveform.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

namespace {
using namespace oscillate;
namespace fs = std::filesystem;

using oscillate::test::require;

using oscillate::test::take;

void put16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value));
  bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}
void put32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
  put16(bytes, static_cast<std::uint16_t>(value));
  put16(bytes, static_cast<std::uint16_t>(value >> 16U));
}

class WavFixture {
public:
  WavFixture(std::uint16_t channels, const std::vector<std::array<std::int16_t, 2>>& samples) {
    std::string pattern = (fs::temp_directory_path() / "oscillate-waveform-XXXXXX").string();
    const auto fd = ::mkstemp(pattern.data());
    REQUIRE_GE(fd, 0, "fixture creation failed");
    path_ = pattern;
    ::close(fd);
    std::vector<std::uint8_t> bytes{'R', 'I', 'F', 'F'};
    put32(bytes, 36U + static_cast<std::uint32_t>(samples.size() * channels * 2));
    bytes.insert(bytes.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put32(bytes, 16);
    put16(bytes, 1);
    put16(bytes, channels);
    put32(bytes, 48'000);
    put32(bytes, 48'000U * channels * 2U);
    put16(bytes, static_cast<std::uint16_t>(channels * 2U));
    put16(bytes, 16);
    bytes.insert(bytes.end(), {'d', 'a', 't', 'a'});
    put32(bytes, static_cast<std::uint32_t>(samples.size() * channels * 2));
    for (const auto& frame : samples) {
      put16(bytes, static_cast<std::uint16_t>(frame[0]));
      if (channels == 2) {
        put16(bytes, static_cast<std::uint16_t>(frame[1]));
      }
    }
    std::ofstream out(path_, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(out), "fixture write failed");
  }
  ~WavFixture() { fs::remove(path_); }
  [[nodiscard]] const fs::path& path() const { return path_; }

private:
  fs::path path_;
};

media::AudioReader reader_for(const WavFixture& fixture) {
  return media::AudioReader::from_wav(take(media::WavReader::open(fixture.path())));
}

void known_signal_and_partial_bucket() {
  const WavFixture file(2, {{{-32768, 0},
                             {-16384, 16384},
                             {0, 32767},
                             {16384, 0},
                             {32767, -32768},
                             {0, -16384},
                             {0, 0},
                             {0, 0}}});
  auto source = reader_for(file);
  auto result = take(analysis::analyze_waveform(source, {"source-a", 9}, {3, 20, 3}));
  REQUIRE_EQ(result.identity.source_key, "source-a", "identity or source metadata lost");
  REQUIRE_EQ(result.identity.generation, 9, "identity or source metadata lost");
  REQUIRE_EQ(result.sample_rate, 48'000, "identity or source metadata lost");
  REQUIRE_EQ(result.channels, 2, "identity or source metadata lost");
  REQUIRE_EQ(result.frames, 8, "identity or source metadata lost");
  REQUIRE_EQ(result.levels.size(), 2, "waveform levels or partial bucket count wrong");
  REQUIRE_EQ(result.levels[0].frames_per_bucket, 3,
             "waveform levels or partial bucket count wrong");
  REQUIRE_EQ(result.levels[0].buckets.size(), 3, "waveform levels or partial bucket count wrong");
  REQUIRE_EQ(result.levels[1].frames_per_bucket, 8,
             "waveform levels or partial bucket count wrong");
  REQUIRE_EQ(result.levels[1].buckets.size(), 1, "waveform levels or partial bucket count wrong");
  const auto& first = result.levels[0].buckets[0];
  const auto& second = result.levels[0].buckets[1];
  const auto& tail = result.levels[0].buckets[2];
  REQUIRE_EQ(first.begin_frame, 0, "first left bucket disagrees with hand-calculated signal");
  REQUIRE_EQ(first.end_frame, 3, "first left bucket disagrees with hand-calculated signal");
  REQUIRE_EQ(first.channels[0].minimum, -1.F,
             "first left bucket disagrees with hand-calculated signal");
  REQUIRE_EQ(first.channels[0].maximum, 0.F,
             "first left bucket disagrees with hand-calculated signal");
  REQUIRE_LT(std::abs(first.channels[0].rms - std::sqrt(1.25 / 3.0)), tolerance::float_round_trip,
             "first left bucket disagrees with hand-calculated signal");
  REQUIRE_EQ(first.channels[1].minimum, 0.F,
             "first right bucket disagrees with independent channel oracle");
  REQUIRE_GT(first.channels[1].maximum, 0.99F,
             "first right bucket disagrees with independent channel oracle");
  REQUIRE_LT(std::abs(first.channels[1].rms - std::sqrt(1.25 / 3.0)), 3e-5,
             "first right bucket disagrees with independent channel oracle");
  REQUIRE_EQ(second.begin_frame, 3, "stereo channels or middle interval wrong");
  REQUIRE_EQ(second.end_frame, 6, "stereo channels or middle interval wrong");
  REQUIRE_GT(second.channels[0].maximum, 0.99F, "stereo channels or middle interval wrong");
  REQUIRE_EQ(second.channels[1].minimum, -1.F, "stereo channels or middle interval wrong");
  REQUIRE_EQ(tail.begin_frame, 6, "partial silent tail wrong");
  REQUIRE_EQ(tail.end_frame, 8, "partial silent tail wrong");
  REQUIRE_EQ(tail.channels[0].rms, 0.0, "partial silent tail wrong");
  REQUIRE_EQ(tail.channels[1].rms, 0.0, "partial silent tail wrong");
  const auto& whole = result.levels[1].buckets[0];
  REQUIRE_EQ(whole.begin_frame, 0, "overview RMS must weight partial bucket by frame count");
  REQUIRE_EQ(whole.end_frame, 8, "overview RMS must weight partial bucket by frame count");
  REQUIRE_LT(std::abs(whole.channels[0].rms - std::sqrt(2.5 / 8.0)), tolerance::float_processing,
             "overview RMS must weight partial bucket by frame count");
  REQUIRE_EQ(whole.channels[0].minimum, -1.F,
             "overview RMS must weight partial bucket by frame count");
  REQUIRE_GT(whole.channels[0].maximum, 0.99F,
             "overview RMS must weight partial bucket by frame count");
}

void mono_bounds_and_budget() {
  std::vector<std::array<std::int16_t, 2>> signal(16);
  signal[0][0] = -32768;
  signal[15][0] = 16384;
  const WavFixture file(1, signal);
  auto source = reader_for(file);
  auto result = take(analysis::analyze_waveform(source, {"mono", 1}, {1, 5, 3}));
  REQUIRE_EQ(result.channels, 1, "budget did not coarsen or cap buckets");
  REQUIRE_EQ(result.levels.size(), 2, "budget did not coarsen or cap buckets");
  REQUIRE_EQ(result.levels[0].frames_per_bucket, 4, "budget did not coarsen or cap buckets");
  REQUIRE_EQ(result.levels[0].buckets.size(), 4, "budget did not coarsen or cap buckets");
  REQUIRE_EQ(result.levels[1].buckets.size(), 1, "budget did not coarsen or cap buckets");
  REQUIRE_EQ(result.levels[1].buckets[0].channels[0].minimum, -1.F,
             "mono bounds or unused channel wrong");
  REQUIRE_EQ(result.levels[1].buckets[0].channels[0].maximum, 0.5F,
             "mono bounds or unused channel wrong");
  REQUIRE_EQ(result.levels[1].buckets[0].channels[1].rms, 0.0,
             "mono bounds or unused channel wrong");
  auto too_small = analysis::analyze_waveform(source, {"mono", 2}, {1, 1, 3});
  REQUIRE_TRUE(too_small, "one-bucket budget should still provide an overview");
  REQUIRE_EQ(too_small.value().levels.size(), 1,
             "one-bucket budget should still provide an overview");
  REQUIRE_EQ(too_small.value().levels.front().buckets.size(), 1,
             "one-bucket budget should still provide an overview");
}

void chunk_boundary_and_cancellation() {
  std::vector<std::array<std::int16_t, 2>> signal(4'101);
  signal[4'095][0] = -32768;
  signal[4'096][0] = 16384;
  const WavFixture file(1, signal);
  auto source = reader_for(file);
  auto complete = take(analysis::analyze_waveform(source, {"boundary", 1}, {256, 200, 3}));
  REQUIRE_EQ(complete.levels.front().buckets[15].channels[0].minimum, -1.F,
             "read boundary changed source frame position");
  REQUIRE_EQ(complete.levels.front().buckets[16].channels[0].maximum, 0.5F,
             "read boundary changed source frame position");
  REQUIRE_EQ(complete.levels.front().buckets[16].end_frame, 4'101,
             "read boundary changed source frame position");
  int calls = 0;
  auto cancelled = analysis::analyze_waveform(source, {"boundary", 2}, {256, 200, 3},
                                              [&calls] { return ++calls >= 2; });
  REQUIRE_TRUE(!cancelled, "cancellation after first chunk must discard partial result");
  REQUIRE_EQ(cancelled.error().code, foundation::ErrorCode::cancelled,
             "cancellation after first chunk must discard partial result");
  REQUIRE_EQ(calls, 2, "cancellation after first chunk must discard partial result");
  calls = 0;
  cancelled = analysis::analyze_waveform(source, {"boundary", 3}, {256, 200, 3},
                                         [&calls] { return ++calls >= 3; });
  REQUIRE_TRUE(!cancelled, "cancellation at publication boundary must discard completed scan");
  REQUIRE_EQ(cancelled.error().code, foundation::ErrorCode::cancelled,
             "cancellation at publication boundary must discard completed scan");
  REQUIRE_EQ(calls, 3, "cancellation at publication boundary must discard completed scan");
  auto invalid = analysis::analyze_waveform(source, {"", 0});
  REQUIRE_TRUE(!invalid, "identity validation omitted");
  REQUIRE_EQ(invalid.error().code, foundation::ErrorCode::invalid_argument,
             "identity validation omitted");
  invalid = analysis::analyze_waveform(source, {"key", 1}, {0, 1, 1});
  REQUIRE_TRUE(!invalid, "zero bucket width accepted");
  REQUIRE_EQ(invalid.error().code, foundation::ErrorCode::invalid_argument,
             "zero bucket width accepted");
  invalid = analysis::analyze_waveform(source, {"key", 1}, {1, 1'000'001, 17});
  REQUIRE_TRUE(!invalid, "unbounded result options accepted");
  REQUIRE_EQ(invalid.error().code, foundation::ErrorCode::invalid_argument,
             "unbounded result options accepted");
  invalid = analysis::analyze_waveform(source, {std::string(129, 'x'), 1});
  REQUIRE_TRUE(!invalid, "unbounded source identity accepted");
  REQUIRE_EQ(invalid.error().code, foundation::ErrorCode::invalid_argument,
             "unbounded source identity accepted");
}

void empty_and_changed_source() {
  const WavFixture empty_file(1, {});
  auto empty_reader = reader_for(empty_file);
  auto empty = take(analysis::analyze_waveform(empty_reader, {"empty", 1}));
  REQUIRE_EQ(empty.frames, 0, "empty source must have no buckets");
  REQUIRE_TRUE(empty.levels.empty(), "empty source must have no buckets");

  const WavFixture changed_file(1, {{{16384, 0}, {16384, 0}}});
  auto changed_reader = reader_for(changed_file);
  fs::resize_file(changed_file.path(), 44);
  auto failed = analysis::analyze_waveform(changed_reader, {"changed", 2}, {1, 10, 2});
  REQUIRE_TRUE(!failed, "changed source must fail rather than publish incomplete waveform");
  REQUIRE_EQ(failed.error().code, foundation::ErrorCode::io_failure,
             "changed source must fail rather than publish incomplete waveform");
}

#if OSCILLATE_HAVE_CODEC
void encoded_sources() {
  struct Case {
    const char* name;
    project::SourceEncoding encoding;
    const char* sha256;
  };
  constexpr std::array cases{
      Case{"sine.aiff", project::SourceEncoding::aiff,
           "b20dfead2878ed5e6c2130fc12f97a2d531ae78458835ad8cb317416a0f35938"},
      Case{"sine.flac", project::SourceEncoding::flac,
           "2adab4cc0289e2a5a3df2791388ac9465845bf8cdd796cec9bf40d862630df61"},
      Case{"sine.mp3", project::SourceEncoding::mp3,
           "bf62c32b101636811ad8d05f9e21d87765a0b738d038d5c43f0869e354de1389"}};
  for (const auto& input : cases) {
    const auto path = fs::path(OSCILLATE_CODEC_FIXTURE_DIR) / input.name;
    const auto fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE_GE(fd, 0, "encoded fixture descriptor failed");
    const auto source_id = take(project::SourceId::from_bits({0, 1}));
    const project::SourceRecord record{
        source_id, input.encoding, input.sha256, fs::file_size(path), 48'000, 1, 960};
    auto opened = media::AudioReader::open_fd(fd, record);
    ::close(fd);
    REQUIRE_OK(opened, "verified encoded fixture failed to open");
    auto reader = std::move(opened).value();
    auto summary = take(analysis::analyze_waveform(reader, {input.sha256, 3}, {100, 30, 3}));
    REQUIRE_EQ(summary.channels, 1, "encoded waveform extent or grouping wrong");
    REQUIRE_EQ(summary.frames, 960, "encoded waveform extent or grouping wrong");
    REQUIRE_EQ(summary.levels.front().buckets.size(), 10,
               "encoded waveform extent or grouping wrong");
    REQUIRE_EQ(summary.levels.front().buckets.back().end_frame, 960,
               "encoded waveform extent or grouping wrong");
    const auto peak = summary.levels.back().buckets.front().channels.front();
    REQUIRE_LT(peak.minimum, -0.08F, "encoded sine amplitude outside independently known range");
    REQUIRE_GT(peak.maximum, 0.08F, "encoded sine amplitude outside independently known range");
    REQUIRE_LT(peak.maximum, 0.2F, "encoded sine amplitude outside independently known range");
    REQUIRE_GT(peak.rms, 0.05, "encoded sine amplitude outside independently known range");
    REQUIRE_LT(peak.rms, 0.11, "encoded sine amplitude outside independently known range");
  }
}
#endif
} // namespace

int main() {
  try {
    known_signal_and_partial_bucket();
    mono_bounds_and_budget();
    chunk_boundary_and_cancellation();
    empty_and_changed_source();
#if OSCILLATE_HAVE_CODEC
    encoded_sources();
#endif
    std::cout << "waveform analysis fixtures passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
