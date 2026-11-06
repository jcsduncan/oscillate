#include "oscillate/export/wav_writer.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"

#include "oscillate/foundation/file_io.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unistd.h>
#include <vector>

namespace {

using oscillate::exporting::PublishOutcome;
using oscillate::exporting::WavFormat;
using oscillate::exporting::WavSpec;
using oscillate::exporting::WavWriter;
using oscillate::foundation::ErrorCode;
namespace fs = std::filesystem;

using Directory = oscillate::test::TempDir;

[[nodiscard]] std::vector<std::uint8_t> read_bytes(const fs::path& path) {
  std::ifstream stream(path, std::ios::binary);
  REQUIRE_TRUE(stream);
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

[[nodiscard]] std::uint16_t le16(std::span<const std::uint8_t> bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(bytes[offset] | (std::uint16_t{bytes[offset + 1]} << 8U));
}

[[nodiscard]] std::uint32_t le32(std::span<const std::uint8_t> bytes, std::size_t offset) {
  return std::uint32_t{bytes[offset]} | (std::uint32_t{bytes[offset + 1]} << 8U) |
         (std::uint32_t{bytes[offset + 2]} << 16U) | (std::uint32_t{bytes[offset + 3]} << 24U);
}

struct ParsedWav {
  std::uint16_t encoding;
  std::uint16_t channels;
  std::uint32_t rate;
  std::uint32_t byte_rate;
  std::uint16_t frame_bytes;
  std::uint16_t bits;
  bool has_fact;
  std::uint32_t fact_frames;
  std::vector<std::uint8_t> data;
};

[[nodiscard]] ParsedWav parse_wav(const fs::path& path) {
  const auto bytes = read_bytes(path);
  REQUIRE_GE(bytes.size(), 44);
  REQUIRE_EQ(std::string(bytes.begin(), bytes.begin() + 4), "RIFF");
  REQUIRE_EQ(std::string(bytes.begin() + 8, bytes.begin() + 12), "WAVE");
  REQUIRE_EQ(std::string(bytes.begin() + 12, bytes.begin() + 16), "fmt ");
  REQUIRE_EQ(le32(bytes, 4) + 8, bytes.size());
  REQUIRE_EQ(le32(bytes, 16), 16);
  ParsedWav parsed{le16(bytes, 20),
                   le16(bytes, 22),
                   le32(bytes, 24),
                   le32(bytes, 28),
                   le16(bytes, 32),
                   le16(bytes, 34),
                   false,
                   0,
                   {}};
  std::size_t offset = 36;
  if (std::string(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                  bytes.begin() + static_cast<std::ptrdiff_t>(offset + 4)) == "fact") {
    REQUIRE_EQ(le32(bytes, offset + 4), 4);
    parsed.has_fact = true;
    parsed.fact_frames = le32(bytes, offset + 8);
    offset += 12;
  }
  REQUIRE_EQ(std::string(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                         bytes.begin() + static_cast<std::ptrdiff_t>(offset + 4)),
             "data");
  REQUIRE_EQ(le32(bytes, offset + 4) + offset + 8, bytes.size());
  parsed.data.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset + 8), bytes.end());
  return parsed;
}

void format_and_golden_bytes() {
  const Directory directory("oscillate-export");
  const std::array samples{1.25F, -1.25F, 0.5F, -0.5F};
  for (const auto& [format, bits, frame_bytes, encoding] :
       std::array<std::tuple<WavFormat, int, int, int>, 3>{{{WavFormat::pcm16, 16, 4, 1},
                                                            {WavFormat::pcm24, 24, 6, 1},
                                                            {WavFormat::float32, 32, 8, 3}}}) {
    const auto path = directory.path() / (std::to_string(bits) + ".wav");
    auto opened = WavWriter::begin(path, {96'000, format, 2, 123});
    REQUIRE_TRUE(opened);
    auto writer = std::move(opened).value();
    REQUIRE_TRUE(writer.write_interleaved_stereo(samples));
    const auto published = writer.finish();
    REQUIRE_TRUE(published && published.value() == PublishOutcome::durable);
    const auto wav = parse_wav(path);
    REQUIRE_TRUE(wav.encoding == encoding && wav.channels == 2 && wav.rate == 96'000);
    REQUIRE_TRUE(wav.bits == bits && wav.frame_bytes == frame_bytes);
    REQUIRE_EQ(wav.byte_rate, 96'000U * static_cast<unsigned>(frame_bytes));
    REQUIRE_EQ(wav.data.size(), std::size_t{2} * static_cast<std::size_t>(frame_bytes));
    REQUIRE_EQ(wav.has_fact, (format == WavFormat::float32));
    if (wav.has_fact) {
      REQUIRE_EQ(wav.fact_frames, 2);
    }
    if (format == WavFormat::pcm16) {
      // Clipping at +/- full scale is unaffected by the TPDF draw.
      REQUIRE_TRUE(std::equal(wav.data.begin(), wav.data.begin() + 4,
                              std::array<std::uint8_t, 4>{0xff, 0x7f, 0x00, 0x80}.begin()));
    } else if (format == WavFormat::pcm24) {
      REQUIRE_TRUE(std::equal(wav.data.begin(), wav.data.begin() + 6,
                              std::array<std::uint8_t, 6>{0xff, 0xff, 0x7f, 0, 0, 0x80}.begin()));
    } else {
      REQUIRE_TRUE(std::equal(wav.data.begin(), wav.data.end(),
                              std::array<std::uint8_t, 16>{0, 0, 0xa0, 0x3f, 0, 0, 0xa0, 0xbf, 0, 0,
                                                           0, 0x3f, 0, 0, 0, 0xbf}
                                  .begin()));
    }
  }
}

void dither_is_reproducible_across_chunks() {
  const Directory directory("oscillate-export");
  std::vector<float> samples(1024, 0.0F);
  struct RenderCase {
    fs::path path;
    std::uint64_t seed;
    std::size_t chunk;
  };
  const auto render = [&](const RenderCase& scenario) {
    auto opened = WavWriter::begin(scenario.path, {44'100, WavFormat::pcm16, 512, scenario.seed});
    REQUIRE_TRUE(opened);
    auto writer = std::move(opened).value();
    for (std::size_t offset = 0; offset < samples.size(); offset += scenario.chunk) {
      REQUIRE_TRUE(
          writer.write_interleaved_stereo(std::span(samples).subspan(offset, scenario.chunk)));
    }
    REQUIRE_TRUE(writer.finish());
    return parse_wav(scenario.path).data;
  };
  const auto first = render({directory.path() / "first.wav", 42, samples.size()});
  const auto chunks = render({directory.path() / "chunks.wav", 42, 16});
  const auto changed = render({directory.path() / "changed.wav", 43, 16});
  REQUIRE_TRUE(first == chunks && first != changed);
  // Independent Python oracle for zero input, seed 42: two SplitMix64 uniforms
  // subtracted per sample, rounded to PCM16 LSBs.
  const std::array<std::uint8_t, 32> expected_prefix{1, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0,    0, 0,
                                                     0, 0, 0, 0, 0,    0,    0,    0xff, 0xff, 1, 0,
                                                     0, 0, 0, 0, 0,    0,    0,    0,    0,    0};
  REQUIRE_TRUE(std::ranges::equal(std::span(first).first(expected_prefix.size()), expected_prefix));
  REQUIRE_TRUE(std::ranges::any_of(first, [](std::uint8_t byte) { return byte != 0; }));
}

void invalid_input_and_failure_keep_existing_file() {
  const Directory directory("oscillate-export");
  const auto target = directory.path() / "master.wav";
  const std::array original{'o', 'l', 'd'};
  {
    std::ofstream stream(target, std::ios::binary);
    stream.write(original.data(), original.size());
  }
  const auto old_bytes = read_bytes(target);
  REQUIRE_TRUE(!WavWriter::begin(fs::path{}, {48'000, WavFormat::pcm16, 1, 1}));
  REQUIRE_TRUE(!WavWriter::begin(directory.path() / "absent" / "master.wav",
                                 {48'000, WavFormat::pcm16, 1, 1}));
  REQUIRE_TRUE(!WavWriter::begin(target, {48'000, WavFormat::pcm24, 800'000'000, 1}));
  REQUIRE_TRUE(!WavWriter::begin(target, {22'050, WavFormat::pcm16, 1, 1}));
  constexpr auto max_pcm16_frames = (std::numeric_limits<std::uint32_t>::max() - 36ULL) / 4;
  {
    auto maximum = WavWriter::begin(target, {48'000, WavFormat::pcm16, max_pcm16_frames, 1});
    REQUIRE_TRUE(maximum);
  }
  REQUIRE_TRUE(!WavWriter::begin(target, {48'000, WavFormat::pcm16, max_pcm16_frames + 1, 1}));
  {
    auto opened = WavWriter::begin(target, {48'000, WavFormat::pcm16, 2, 1});
    REQUIRE_TRUE(opened);
    auto writer = std::move(opened).value();
    const std::array one_frame{0.0F, 0.0F};
    REQUIRE_TRUE(writer.write_interleaved_stereo(one_frame));
    auto finished = writer.finish();
    REQUIRE_TRUE(!finished && finished.error().code == ErrorCode::invalid_argument);
  }
  REQUIRE_EQ(read_bytes(target), old_bytes);
  {
    auto opened = WavWriter::begin(target, {48'000, WavFormat::pcm16, 1, 1});
    auto writer = std::move(opened).value();
    const std::array invalid{std::numeric_limits<float>::quiet_NaN(), 0.0F};
    auto result = writer.write_interleaved_stereo(invalid);
    REQUIRE_TRUE(!result && result.error().code == ErrorCode::invalid_argument);
    REQUIRE_TRUE(!writer.finish());
  }
  REQUIRE_EQ(read_bytes(target), old_bytes);
  {
    auto opened = WavWriter::begin(target, {48'000, WavFormat::pcm16, 1, 1});
    auto writer = std::move(opened).value();
    const std::array too_many{0.0F, 0.0F, 0.0F, 0.0F};
    REQUIRE_TRUE(!writer.write_interleaved_stereo(too_many));
  }
  {
    auto opened = WavWriter::begin(target, {48'000, WavFormat::pcm16, 1, 1});
    auto writer = std::move(opened).value();
    const std::array odd{0.0F};
    REQUIRE_TRUE(!writer.write_interleaved_stereo(odd));
  }
  REQUIRE_EQ(read_bytes(target), old_bytes);
  REQUIRE_EQ(std::distance(fs::directory_iterator(directory.path()), fs::directory_iterator{}), 1);
}

void rename_failure_retains_target() {
  const Directory directory("oscillate-export");
  const auto target = directory.path() / "master.wav";
  fs::create_directory(target);
  auto opened = WavWriter::begin(target, {48'000, WavFormat::float32, 0, 0});
  REQUIRE_TRUE(opened);
  auto writer = std::move(opened).value();
  auto result = writer.finish();
  REQUIRE_TRUE(!result && result.error().code == ErrorCode::io_failure);
  REQUIRE_TRUE(fs::is_directory(target));
}

enum class FakeMode : std::uint8_t { short_then_interrupt, zero, no_space };
FakeMode fake_mode;
unsigned fake_calls;
std::vector<std::uint8_t> fake_output;

ssize_t fake_write(int, const void* data, std::size_t size) {
  ++fake_calls;
  if (fake_mode == FakeMode::zero) {
    return 0;
  }
  if (fake_mode == FakeMode::no_space) {
    errno = ENOSPC;
    return -1;
  }
  if (fake_calls == 2) {
    errno = EINTR;
    return -1;
  }
  const auto count = std::min<std::size_t>(size, 2);
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  fake_output.insert(fake_output.end(), bytes, bytes + count);
  return static_cast<ssize_t>(count);
}

void short_write_and_interruption() {
  const std::array bytes{std::uint8_t{1}, std::uint8_t{2}, std::uint8_t{3}, std::uint8_t{4},
                         std::uint8_t{5}};
  fake_mode = FakeMode::short_then_interrupt;
  fake_calls = 0;
  fake_output.clear();
  REQUIRE_TRUE(oscillate::foundation::write_all(0, std::as_bytes(std::span(bytes)), "WAV write",
                                                fake_write));
  REQUIRE_TRUE(fake_calls == 4 &&
               std::equal(fake_output.begin(), fake_output.end(), bytes.begin()));
  fake_mode = FakeMode::zero;
  REQUIRE_TRUE(!oscillate::foundation::write_all(0, std::as_bytes(std::span(bytes)), "WAV write",
                                                 fake_write));
  fake_mode = FakeMode::no_space;
  const auto no_space =
      oscillate::foundation::write_all(0, std::as_bytes(std::span(bytes)), "WAV write", fake_write);
  REQUIRE_TRUE(!no_space && no_space.error().code == ErrorCode::io_failure);
}

} // namespace

int main() {
  try {
    format_and_golden_bytes();
    dither_is_reproducible_across_chunks();
    invalid_input_and_failure_keep_existing_file();
    rename_failure_retains_target();
    short_write_and_interruption();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
