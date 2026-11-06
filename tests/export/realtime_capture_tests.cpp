#include "oscillate/export/realtime_capture.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
namespace fs = std::filesystem;
using oscillate::engine::BlockContext;
using oscillate::engine::StereoFrame;
using oscillate::exporting::RealtimeWavCapture;
using oscillate::exporting::WavFormat;

using Directory = oscillate::test::TempDir;

[[nodiscard]] std::vector<char> bytes(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

[[nodiscard]] RealtimeWavCapture::DeviceEvidence clean_evidence() {
  return {.xruns_before = 7, .xruns_after = 7};
}

void writes_exact_interval_and_preserves_existing_target_on_failure() {
  const Directory directory("oscillate-realtime-export");
  const auto target = directory.path() / "master.wav";
  const std::array first{StereoFrame{0.25F, -0.25F}, StereoFrame{0.5F, -0.5F}};
  const std::array second{StereoFrame{0.75F, -0.75F}, StereoFrame{1.0F, -1.0F},
                          StereoFrame{0.0F, 0.0F}};
  auto opened = RealtimeWavCapture::begin(target, {48'000, WavFormat::float32, 4}, 100, 3);
  REQUIRE_TRUE(opened);
  auto capture = std::move(opened.value());
  capture->capture(first, BlockContext{100, 1}, true);
  REQUIRE_EQ(capture->drain().value(), 2);
  capture->capture(second, BlockContext{102, 1}, true);
  REQUIRE_EQ(capture->frames_captured(), 4);
  REQUIRE_TRUE(capture->finish(clean_evidence()));
  const auto golden = bytes(target);
  REQUIRE_EQ(golden.size(), 56 + 4 * 8);
  const auto sample_bits = [&](std::size_t byte_offset) {
    const auto* data = reinterpret_cast<const unsigned char*>(golden.data());
    return std::uint32_t{data[byte_offset]} | (std::uint32_t{data[byte_offset + 1]} << 8U) |
           (std::uint32_t{data[byte_offset + 2]} << 16U) |
           (std::uint32_t{data[byte_offset + 3]} << 24U);
  };
  REQUIRE_EQ(sample_bits(56), std::bit_cast<std::uint32_t>(0.25F));
  REQUIRE_EQ(sample_bits(60), std::bit_cast<std::uint32_t>(-0.25F));
  REQUIRE_EQ(sample_bits(80), std::bit_cast<std::uint32_t>(1.0F));
  REQUIRE_EQ(sample_bits(84), std::bit_cast<std::uint32_t>(-1.0F));
  REQUIRE_TRUE(!capture->finish(clean_evidence()));

  auto failed = RealtimeWavCapture::begin(target, {48'000, WavFormat::float32, 4}, 100, 4);
  REQUIRE_TRUE(failed);
  auto replacement = std::move(failed.value());
  replacement->capture(first, BlockContext{100, 1}, true);
  replacement->capture(first, BlockContext{103, 1}, true);
  REQUIRE_TRUE(!replacement->finish(clean_evidence()));
  replacement.reset();
  REQUIRE_EQ(bytes(target), golden);
}

void refuses_overflow_xrun_render_failure_and_unknown_evidence() {
  const Directory directory("oscillate-realtime-export");
  const std::array block{StereoFrame{0.1F, 0.2F}, StereoFrame{0.3F, 0.4F}};
  const auto run = [&](const std::string& name, auto trigger,
                       RealtimeWavCapture::DeviceEvidence evidence, std::size_t ring = 2) {
    const auto target = directory.path() / name;
    auto opened = RealtimeWavCapture::begin(target, {48'000, WavFormat::float32, 4}, 0, ring);
    REQUIRE_TRUE(opened);
    auto capture = std::move(opened.value());
    capture->capture(block, BlockContext{0, 1}, true);
    trigger(*capture);
    REQUIRE_TRUE(!capture->finish(evidence));
    capture.reset();
    REQUIRE_TRUE(!fs::exists(target));
  };
  run(
      "overflow.wav",
      [&](RealtimeWavCapture& capture) { capture.capture(block, BlockContext{2, 1}, true); },
      clean_evidence());
  run(
      "render.wav",
      [&](RealtimeWavCapture& capture) { capture.capture(block, BlockContext{2, 1}, false); },
      clean_evidence());
  run("unknown.wav",
      [&](RealtimeWavCapture& capture) {
        REQUIRE_TRUE(capture.drain());
        capture.capture(block, BlockContext{2, 1}, true);
      },
      {});
  auto reported = clean_evidence();
  reported.xruns_after = 8;
  run(
      "device-xrun.wav",
      [&](RealtimeWavCapture& capture) {
        REQUIRE_TRUE(capture.drain());
        capture.capture(block, BlockContext{2, 1}, true);
      },
      reported);
}

void short_or_seeked_capture_does_not_publish() {
  const Directory directory("oscillate-realtime-export");
  const std::array block{StereoFrame{0.1F, 0.2F}, StereoFrame{0.3F, 0.4F}};
  for (const auto seek : {false, true}) {
    const auto target = directory.path() / (seek ? "seek.wav" : "short.wav");
    auto opened = RealtimeWavCapture::begin(target, {48'000, WavFormat::pcm24, 4}, 10, 4);
    REQUIRE_TRUE(opened);
    auto capture = std::move(opened.value());
    capture->capture(block, BlockContext{10, 1}, true);
    if (seek)
      capture->capture(block, BlockContext{12, 2}, true);
    REQUIRE_TRUE(!capture->finish(clean_evidence()));
    capture.reset();
    REQUIRE_TRUE(!fs::exists(target));
  }
}
} // namespace

int main() {
  writes_exact_interval_and_preserves_existing_target_on_failure();
  refuses_overflow_xrun_render_failure_and_unknown_evidence();
  short_or_seeked_capture_does_not_publish();
}
