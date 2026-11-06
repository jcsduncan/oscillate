#include "oscillate/media/source_rate_reader.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
using namespace oscillate;

using oscillate::test::require;
using oscillate::test::take;
void put16(std::ofstream& file, std::uint16_t value) {
  for (unsigned i = 0; i < 2; ++i) {
    file.put(static_cast<char>((static_cast<std::uint32_t>(value) >> (8U * i)) & 0xffU));
  }
}
void put32(std::ofstream& file, std::uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) {
    file.put(static_cast<char>((value >> (8U * i)) & 0xffU));
  }
}

struct Fixture {
  static inline std::atomic<std::uint64_t> next_id{0};
  std::filesystem::path path = std::filesystem::temp_directory_path() /
                               ("oscillate-source-rate-" + std::to_string(::getpid()) + "-" +
                                std::to_string(next_id.fetch_add(1)) + ".wav");
  Fixture(std::uint32_t rate, std::uint16_t channels,
          const std::vector<media::StereoFrame>& frames) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    const auto bytes = static_cast<std::uint32_t>(frames.size() * channels * sizeof(float));
    file.write("RIFF", 4);
    put32(file, bytes + 36);
    file.write("WAVEfmt ", 8);
    put32(file, 16);
    put16(file, 3);
    put16(file, channels);
    put32(file, rate);
    put32(file, rate * channels * 4U);
    put16(file, static_cast<std::uint16_t>(channels * 4U));
    put16(file, 32);
    file.write("data", 4);
    put32(file, bytes);
    for (const auto& frame : frames) {
      put32(file, std::bit_cast<std::uint32_t>(frame.left));
      if (channels == 2) {
        put32(file, std::bit_cast<std::uint32_t>(frame.right));
      }
    }
    require(static_cast<bool>(file), "source fixture write failed");
  }
  ~Fixture() { std::filesystem::remove(path); }
};

media::SourceRateReader open(const Fixture& fixture, std::uint32_t project_rate) {
  return take(media::SourceRateReader::open(
      media::AudioReader::from_wav(take(media::WavReader::open(fixture.path))), project_rate));
}

void test_sine_chunk_seek_and_duration() {
  constexpr std::uint32_t source_rate = oscillate::test::sample_rate_48k;
  constexpr std::uint32_t project_rate = oscillate::test::sample_rate_44k;
  std::vector<media::StereoFrame> source(source_rate);
  constexpr double pi = 3.14159265358979323846;
  for (std::size_t index = 0; index < source.size(); ++index) {
    const auto value = static_cast<float>(
        0.5 * std::sin(2.0 * pi * 440.0 * static_cast<double>(index) / source_rate));
    source[index] = {value, value * -0.25F};
  }
  Fixture fixture(source_rate, 2, source);
  auto reader = open(fixture, project_rate);
  REQUIRE_EQ(reader.frame_count(), project_rate, "rate or rounded duration wrong");
  REQUIRE_EQ(reader.source_rate(), source_rate, "rate or rounded duration wrong");
  REQUIRE_EQ(reader.project_rate(), project_rate, "rate or rounded duration wrong");
  std::array<media::StereoFrame, 1'024> whole{};
  std::array<media::StereoFrame, 1'024> pieces{};
  for (std::uint64_t start : {0ULL, 257ULL, 1'024ULL, 21'000ULL, 43'076ULL}) {
    const auto count = static_cast<std::size_t>(
        std::min<std::uint64_t>(whole.size(), reader.frame_count() - start));
    REQUIRE_EQ(take(reader.read_at(start, std::span(whole).first(count))), count,
               "whole source-rate read short");
    std::size_t done = 0;
    while (done < count) {
      const auto step = std::min<std::size_t>(113, count - done);
      REQUIRE_EQ(take(reader.read_at(start + done, std::span(pieces).subspan(done, step))), step,
                 "chunk source-rate read short");
      done += step;
    }
    for (std::size_t index = 0; index < count; ++index) {
      REQUIRE_LT(std::abs(whole[index].left - pieces[index].left), 2e-5F,
                 "chunk or seek changed source-rate samples");
      REQUIRE_LT(std::abs(whole[index].right - pieces[index].right), 2e-5F,
                 "chunk or seek changed source-rate samples");
      if (start + index > 500 && start + index + 500 < reader.frame_count()) {
        const auto expected =
            0.5 * std::sin(2.0 * pi * 440.0 * static_cast<double>(start + index) / project_rate);
        REQUIRE_LT(std::abs(whole[index].left - expected), 0.001,
                   "resampled sine differs from independent timing oracle");
      }
    }
  }
  REQUIRE_EQ(take(reader.read_at(reader.frame_count(), whole)), 0, "EOF read was not empty");
  REQUIRE_EQ(take(reader.read_at(0, std::span(whole).first(0))), 0,
             "empty worker read was not empty");
}

void test_impulse_mono_and_cancel() {
  std::vector<media::StereoFrame> source(48'001);
  source[24'000] = {1.0F, 0.0F};
  Fixture fixture(48'000, 1, source);
  auto reader = open(fixture, 44'100);
  REQUIRE_EQ(reader.frame_count(), 44'101, "fractional duration did not round to nearest frame");
  std::array<media::StereoFrame, 1'024> frames{};
  REQUIRE_EQ(take(reader.read_at(21'600, frames)), frames.size(), "impulse seek short");
  std::size_t peak = 0;
  for (std::size_t index = 1; index < frames.size(); ++index) {
    if (std::abs(frames[index].left) > std::abs(frames[peak].left)) {
      peak = index;
    }
    REQUIRE_LT(std::abs(frames[index].left - frames[index].right), 1e-7F,
               "mono source was not duplicated to stereo");
  }
  REQUIRE_NEAR(static_cast<std::int64_t>(21'600 + peak), 22'050, 1,
               "impulse position or passband gain wrong");
  REQUIRE_GT(frames[peak].left, 0.8F, "impulse position or passband gain wrong");
  std::stop_source cancellation;
  cancellation.request_stop();
  const auto rejected = reader.read_at(0, frames, cancellation.get_token());
  REQUIRE_TRUE(!rejected, "cancelled worker read was accepted");
  REQUIRE_EQ(rejected.error().code, foundation::ErrorCode::cancelled,
             "cancelled worker read was accepted");
  std::array<media::StereoFrame, 1'025> oversized{};
  require(!reader.read_at(0, oversized), "oversize worker read was accepted");
}

void test_rate_matrix_and_reset_edges() {
  constexpr std::array source_rates{8'000U, 44'100U, 48'000U, 88'200U, 96'000U, 384'000U};
  constexpr std::array project_rates{44'100U, 48'000U, 88'200U, 96'000U};
  for (const auto source_rate : source_rates) {
    std::vector<media::StereoFrame> source(4'097);
    source[0] = {0.5F, -0.25F};
    source[2'048] = {1.0F, 0.0F};
    source.back() = {0.25F, 0.125F};
    Fixture fixture(source_rate, 2, source);
    for (const auto project_rate : project_rates) {
      auto reader = open(fixture, project_rate);
      const auto expected =
          (static_cast<std::uint64_t>(source.size()) * project_rate + source_rate / 2U) /
          source_rate;
      REQUIRE_EQ(reader.frame_count(), expected, "rate matrix duration rounding differs");
      if (expected == 0) {
        continue;
      }
      std::array<media::StereoFrame, 1'024> whole{};
      std::array<media::StereoFrame, 1'024> split{};
      for (const auto start : std::array<std::uint64_t, 4>{
               0, expected / 3, expected / 2, expected > 1'024 ? expected - 1'024 : 0}) {
        const auto count =
            static_cast<std::size_t>(std::min<std::uint64_t>(whole.size(), expected - start));
        REQUIRE_EQ(take(reader.read_at(start, std::span(whole).first(count))), count,
                   "rate matrix whole read short");
        std::size_t copied = 0;
        while (copied < count) {
          const auto piece = std::min<std::size_t>(73, count - copied);
          REQUIRE_EQ(take(reader.read_at(start + copied, std::span(split).subspan(copied, piece))),
                     piece, "rate matrix chunk read short");
          copied += piece;
        }
        for (std::size_t index = 0; index < count; ++index) {
          REQUIRE_LT(std::abs(whole[index].left - split[index].left), 3e-5F,
                     "rate matrix seek/chunk changed output");
          REQUIRE_LT(std::abs(whole[index].right - split[index].right), 3e-5F,
                     "rate matrix seek/chunk changed output");
        }
      }
      REQUIRE_EQ(take(reader.read_at(expected, whole)), 0,
                 "rate matrix reader passed final rounded frame");
    }
  }
}

void test_short_source_endpoint_context() {
  for (const std::size_t source_frames : {1U, 3U, 17U}) {
    std::vector<media::StereoFrame> source(source_frames);
    source.front() = {1.0F, 0.0F};
    Fixture fixture(48'000, 2, source);
    auto reader = open(fixture, 96'000);
    REQUIRE_EQ(reader.frame_count(), source_frames * 2,
               "short source duration changed under integer doubling");
    std::array<media::StereoFrame, 64> whole{};
    std::array<media::StereoFrame, 64> separate{};
    const auto count = static_cast<std::size_t>(reader.frame_count());
    REQUIRE_EQ(take(reader.read_at(0, std::span(whole).first(count))), count,
               "short source end drain was not available");
    for (std::size_t frame = 0; frame < count; ++frame) {
      REQUIRE_EQ(take(reader.read_at(frame, std::span(separate).subspan(frame, 1))), 1,
                 "short source seek missed an endpoint frame");
      REQUIRE_LT(std::abs(whole[frame].left - separate[frame].left), 3e-5F,
                 "short source padding changed chunking or leaked channels");
      REQUIRE_LT(std::abs(whole[frame].right), 1e-7F,
                 "short source padding changed chunking or leaked channels");
    }
    REQUIRE_GT(whole.front().left, 0.8F, "short source first impulse or rounded endpoint was lost");
    REQUIRE_EQ(take(reader.read_at(count, whole)), 0,
               "short source first impulse or rounded endpoint was lost");
  }
}

void test_validation_and_source_failure() {
  std::vector<media::StereoFrame> source(1'024);
  source[0] = {1.0F, -1.0F};
  Fixture fixture(48'000, 2, source);
  auto invalid = media::SourceRateReader::open(
      media::AudioReader::from_wav(take(media::WavReader::open(fixture.path))), 42'000);
  REQUIRE_TRUE(!invalid, "unsupported project rate was accepted");
  REQUIRE_EQ(invalid.error().code, foundation::ErrorCode::invalid_argument,
             "unsupported project rate was accepted");
  Fixture coprime(48'001, 2, source);
  auto unsupported_grid = media::SourceRateReader::open(
      media::AudioReader::from_wav(take(media::WavReader::open(coprime.path))), 44'100);
  REQUIRE_TRUE(!unsupported_grid, "unbounded rational seek grid was accepted");
  REQUIRE_EQ(unsupported_grid.error().code, foundation::ErrorCode::unsupported,
             "unbounded rational seek grid was accepted");
  auto reader = open(fixture, 44'100);
  auto reassigned = open(fixture, 44'100);
  reassigned = std::move(reader);
  std::filesystem::resize_file(fixture.path, 44);
  std::array<media::StereoFrame, 128> output{};
  auto failed = reassigned.read_at(0, output);
  REQUIRE_TRUE(!failed, "truncated source was converted as valid audio");
  REQUIRE_EQ(failed.error().code, foundation::ErrorCode::io_failure,
             "truncated source was converted as valid audio");
}
} // namespace

int run_tests() {
  test_sine_chunk_seek_and_duration();
  test_impulse_mono_and_cancel();
  test_rate_matrix_and_reset_edges();
  test_short_source_endpoint_context();
  test_validation_and_source_failure();
  return 0;
}

int main() {
  try {
    return run_tests();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
