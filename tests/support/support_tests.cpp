#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/signal.hpp"
#include "oscillate/test/temp_dir.hpp"
#include "oscillate/test/tolerances.hpp"
#include "oscillate/test/wait.hpp"
#include "oscillate/test/wav.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace test = oscillate::test;
namespace foundation = oscillate::foundation;

// Runs `check` and returns the failure it reported; fails when it passed.
template <typename Check> std::string failure_of(Check&& check) {
  try {
    check();
  } catch (const test::TestFailure& failure) {
    return failure.what();
  }
  throw test::TestFailure("the check passed but should have failed");
}

void contains(const std::string& text, std::string_view part) {
  test::require(text.find(part) != std::string::npos,
                "\"" + text + "\" does not contain \"" + std::string(part) + "\"");
}

void comparisons_pass_and_report_both_values() {
  constexpr int three = 3;
  constexpr int four = 4;
  const std::vector<int> values{1, 2, 3};
  REQUIRE_EQ(values.size(), three);
  REQUIRE_NE(three, four);
  REQUIRE_LT(three, four);
  REQUIRE_LE(three, three);
  REQUIRE_GT(four, three);
  REQUIRE_GE(four, four);
  REQUIRE_TRUE(three < four);
  REQUIRE_FALSE(three > four);
  const auto report = failure_of([&] { REQUIRE_EQ(values.size(), four, "three values"); });
  contains(report, "support_tests.cpp:");
  contains(report, "REQUIRE_EQ(values.size(), four)");
  contains(report, "3 vs 4");
  contains(report, "three values");
  contains(failure_of([&] { REQUIRE_GT(three, four); }), "REQUIRE_GT(three, four) failed: 3 vs 4");
  contains(failure_of([&] { REQUIRE_TRUE(three > four); }), "REQUIRE_TRUE(three > four)");
  contains(failure_of([&] { REQUIRE_EQ(values, (std::vector<int>{1, 2})); }),
           "{1, 2, 3} vs {1, 2}");
}

void integers_compare_by_value_whatever_their_sign() {
  constexpr std::size_t large = std::numeric_limits<std::size_t>::max();
  constexpr int negative = -1;
  // The built-in comparison would convert -1 to SIZE_MAX and call them equal.
  REQUIRE_NE(large, negative);
  REQUIRE_LT(negative, large);
  const std::atomic<unsigned> counter{2};
  constexpr int two = 2;
  REQUIRE_EQ(counter, two);
  constexpr float half = 0.5F;
  constexpr double half_double = 0.5;
  REQUIRE_EQ(half, half_double);
}

void near_uses_the_operands_types_and_fails_on_nan() {
  constexpr double one = 1.0;
  constexpr double close = 1.0 + 0.5 * test::tolerance::float_round_trip;
  REQUIRE_NEAR(close, one, test::tolerance::float_round_trip);
  // The bound itself passes; anything beyond it fails.
  REQUIRE_NEAR(one + test::tolerance::coarse, one, test::tolerance::coarse);
  contains(failure_of([&] {
             REQUIRE_NEAR(one + test::tolerance::coarse, one, test::tolerance::near_silence);
           }),
           "> tolerance");
  const double nan = std::numeric_limits<double>::quiet_NaN();
  contains(failure_of([&] { REQUIRE_NEAR(nan, one, test::tolerance::coarse); }), "REQUIRE_NEAR");
}

void results_report_their_error() {
  const auto failed =
      foundation::Result<int>::failure({foundation::ErrorCode::not_found, "no such clip"});
  contains(failure_of([&] { REQUIRE_OK(failed, "the clip loads"); }), "no such clip");
  contains(failure_of([&] { (void)test::take(failed); }), "no such clip");
  constexpr int seven = 7;
  REQUIRE_EQ(test::take(foundation::Result<int>::success(seven)), seven);
  test::take(foundation::Result<void>::success());
  const std::optional<int> empty;
  contains(failure_of([&] { REQUIRE_OK(empty); }), "no value");
  contains(failure_of([] { test::require(false, "the reason"); }), "the reason");
}

void temp_dirs_are_real_and_removed() {
  std::filesystem::path kept;
  {
    const test::TempDir directory("oscillate-support");
    kept = directory.path();
    REQUIRE_TRUE(std::filesystem::is_directory(kept));
    contains(kept.filename().string(), "oscillate-support-");
    test::write_bytes(directory / "file.bin", std::vector<std::uint8_t>{1, 2, 3});
    REQUIRE_TRUE(std::filesystem::exists(directory / "file.bin"));
  }
  REQUIRE_FALSE(std::filesystem::exists(kept));
}

void wav_fixtures_round_trip() {
  const test::TempDir directory("oscillate-support-wav");
  const std::array<std::int16_t, 4> pcm{0, 1'000, -1'000, std::numeric_limits<std::int16_t>::min()};
  const auto pcm_path = directory / "pcm.wav";
  test::write_wav_pcm16(pcm_path, test::sample_rate_44k, test::stereo_channels, pcm);
  const auto bytes = test::read_bytes(pcm_path);
  REQUIRE_EQ(bytes.size(), test::wav_header_bytes + pcm.size() * sizeof(std::int16_t));
  // An independent oracle: the canonical header for 44.1 kHz stereo PCM16
  // with 8 data bytes, written out byte by byte (little-endian fields).
  constexpr std::array<std::uint8_t, test::wav_header_bytes> expected_header{
      'R',  'I',  'F',  'F', 44,  0,   0,   0,                // RIFF size: 36 + 8 data bytes
      'W',  'A',  'V',  'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, // fmt chunk size
      1,    0,                                                // PCM
      2,    0,                                                // two channels
      0x44, 0xAC, 0,    0,                                    // 44100 Hz
      0x10, 0xB1, 0x02, 0,                                    // byte rate 176400
      4,    0,                                                // block align
      16,   0,                                                // bits per sample
      'd',  'a',  't',  'a', 8,   0,   0,   0};               // data size
  REQUIRE_TRUE(std::equal(expected_header.begin(), expected_header.end(), bytes.begin()));
  REQUIRE_EQ(test::read_le32(bytes, test::wav_rate_offset), test::sample_rate_44k);
  const auto parsed = test::read_wav(pcm_path);
  REQUIRE_EQ(parsed.layout.channels, test::stereo_channels);
  REQUIRE_EQ(parsed.layout.bits_per_sample, test::pcm16_bits);
  REQUIRE_EQ(parsed.layout.encoding, test::WavEncoding::pcm);
  REQUIRE_EQ(parsed.data.size(), pcm.size() * sizeof(std::int16_t));
  const std::vector<float> samples{0.25F, -0.5F, 1.0F};
  const auto float_path = directory / "float.wav";
  test::write_wav_float(float_path, test::sample_rate_48k, test::mono_channels, samples);
  REQUIRE_EQ(test::read_float_wav(float_path), samples);
  REQUIRE_EQ(test::read_wav(float_path).layout.encoding, test::WavEncoding::ieee_float);
}

void signals_and_levels() {
  constexpr double frequency = 1'000.0;
  constexpr std::size_t frames = test::sample_rate_48k;
  const auto tone = test::sine(frequency, test::sample_rate_48k_hz, frames);
  // A whole number of cycles of a unit sine has an rms of 1/sqrt(2).
  REQUIRE_NEAR(test::rms(tone), 1.0 / std::sqrt(2.0), test::tolerance::double_accumulation);
  REQUIRE_NEAR(test::peak(tone), 1.0, test::tolerance::float_round_trip);
  constexpr std::uint64_t seed = 7;
  REQUIRE_EQ(test::white_noise(test::block_64, seed), test::white_noise(test::block_64, seed));
  REQUIRE_LE(test::peak(test::white_noise(test::block_1024, seed)), 1.0);
  constexpr double minus_twenty = -20.0;
  REQUIRE_NEAR(test::gain_to_db(0.1), minus_twenty, test::tolerance::double_rounding);
  REQUIRE_NEAR(test::db_to_gain(minus_twenty), 0.1, test::tolerance::double_rounding);
  REQUIRE_EQ(test::gain_to_db(0.0, test::silence_floor), test::gain_to_db(test::silence_floor));
  REQUIRE_TRUE(std::isinf(test::gain_to_db(0.0)));
}

void waits_return_once_ready() {
  int polls = 0;
  constexpr int ready_after = 3;
  test::wait_until([&] { return ++polls == ready_after; }, "the counter reaches three");
  REQUIRE_EQ(polls, ready_after);
  constexpr std::chrono::milliseconds short_deadline{1};
  contains(failure_of([&] { test::wait_until([] { return false; }, "never", short_deadline); }),
           "timed out waiting: never");
}

} // namespace

int main(int argc, char** argv) {
  return test::run_cases(
      argc, argv, "test support contract passed",
      {
          {"comparisons_pass_and_report_both_values", comparisons_pass_and_report_both_values},
          {"integers_compare_by_value_whatever_their_sign",
           integers_compare_by_value_whatever_their_sign},
          {"near_uses_the_operands_types_and_fails_on_nan",
           near_uses_the_operands_types_and_fails_on_nan},
          {"results_report_their_error", results_report_their_error},
          {"temp_dirs_are_real_and_removed", temp_dirs_are_real_and_removed},
          {"wav_fixtures_round_trip", wav_fixtures_round_trip},
          {"signals_and_levels", signals_and_levels},
          {"waits_return_once_ready", waits_return_once_ready},
      });
}
