#include "oscillate/export/dither.hpp"
#include "oscillate/export/wav_writer.hpp"
#include "oscillate/foundation/split_mix.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"

#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <span>
#include <vector>

// Dither (ADR-0117): the quantisation error's spectrum against each shape's
// noise transfer, the error feedback's bound, the flat path's seeded TPDF and
// the writers' use of the shape.
namespace {

using oscillate::exporting::dither_taps;
using oscillate::exporting::DitherQuantizer;
using oscillate::exporting::DitherShape;
using oscillate::exporting::WavFormat;
using oscillate::exporting::WavWriter;

constexpr unsigned pcm16_bits = 16;
constexpr double pcm16_scale = 32'768.0;
constexpr std::uint32_t rate_44k = 44'100;
constexpr std::uint32_t rate_48k = 48'000;
constexpr std::uint32_t rate_88k = 88'200;
constexpr std::uint32_t rate_96k = 96'000;
constexpr std::uint64_t seed = 0x0d17'7e75'eed5'0001ULL;

// The error's expected power: TPDF of +/-1 LSB (1/6) plus rounding (1/12).
constexpr double tpdf_power_lsb2 = 0.25;

// Welch's estimate: Hann-windowed segments, half overlapped.
constexpr std::size_t segment = 2'048;
constexpr std::size_t segments = 256;
constexpr std::size_t samples = segment * (segments + 1) / 2;
// A test signal under the error: a quiet sine, so the input is never
// constant.
constexpr double signal_hz = 997.0;
constexpr double signal_level = 0.01;
// The spectrum is read at these frequencies (Hz), away from the signal.
constexpr std::array probe_hz{200.0, 2'000.0, 3'500.0, 6'000.0, 10'000.0, 15'000.0, 19'000.0};
// The estimate's spread over 256 segments is about 0.3 dB; the curve is
// held to 1 dB.
constexpr double curve_tolerance_db = 1.0;
// The audible band at the high rates, and how far under flat TPDF the
// shaped noise stays in it there.
constexpr double audible_hz = 20'000.0;
constexpr double high_rate_audible_cut_db = 6.5;
constexpr double audible_step_hz = 250.0;
// At 44.1 kHz from 2 to 5 kHz, the F-weighted noise sits this far under
// flat TPDF at least.
constexpr double psychoacoustic_dip_db = 15.0;
constexpr double dip_low_hz = 2'000.0;
constexpr double dip_high_hz = 5'000.0;
// Full scale, for the clipping run.
constexpr float full_scale = 1.0F;
constexpr std::size_t clipping_samples = 4'096;
constexpr double decibels_per_bel = 10.0;

[[nodiscard]] double db(double power) { return decibels_per_bel * std::log10(power); }

// The quantisation error (output less input, in LSB) of channel 0.
std::vector<double> error_of(DitherShape shape, std::uint32_t rate) {
  DitherQuantizer quantizer(pcm16_bits, shape, rate, seed);
  std::vector<double> error(samples);
  for (std::size_t i = 0; i < samples; ++i) {
    const auto x = static_cast<float>(signal_level * std::sin(2.0 * std::numbers::pi * signal_hz *
                                                              static_cast<double>(i) / rate));
    const auto q = quantizer.quantize(x, 0);
    error[i] = static_cast<double>(q) - static_cast<double>(x) * pcm16_scale;
  }
  return error;
}

// The power spectral density (LSB^2 per unit of normalised frequency, so
// white noise of power P reads P) at `hz`.
double psd(const std::vector<double>& x, double hz, std::uint32_t rate) {
  double total = 0.0;
  double window_power = 0.0;
  std::vector<double> window(segment);
  for (std::size_t n = 0; n < segment; ++n) {
    window[n] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(n) / segment);
    window_power += window[n] * window[n];
  }
  const std::complex<double> step = std::polar(1.0, -2.0 * std::numbers::pi * hz / rate);
  for (std::size_t s = 0; s < segments; ++s) {
    std::complex<double> sum{};
    std::complex<double> turn{1.0, 0.0};
    for (std::size_t n = 0; n < segment; ++n) {
      sum += window[n] * x[s * segment / 2 + n] * turn;
      turn *= step;
    }
    total += std::norm(sum) / window_power;
  }
  return total / static_cast<double>(segments);
}

// |1 - sum(taps[k] z^-(k+1))|^2 at `hz`.
double transfer_power(std::span<const double> taps, double hz, std::uint32_t rate) {
  std::complex<double> h{1.0, 0.0};
  for (std::size_t k = 0; k < taps.size(); ++k) {
    h -=
        taps[k] * std::polar(1.0, -2.0 * std::numbers::pi * hz * static_cast<double>(k + 1) / rate);
  }
  return std::norm(h);
}

void follows_the_curve(DitherShape shape, std::uint32_t rate) {
  const auto error = error_of(shape, rate);
  const auto taps = dither_taps(shape, rate);
  for (const double hz : probe_hz) {
    const double measured = db(psd(error, hz, rate));
    const double expected = db(tpdf_power_lsb2 * transfer_power(taps, hz, rate));
    REQUIRE_NEAR(measured, expected, curve_tolerance_db, "the dither noise follows its curve");
  }
}

void flat_dither_is_white_at_a_quarter_lsb() {
  REQUIRE_TRUE(dither_taps(DitherShape::flat, rate_48k).empty(), "flat TPDF has no feedback");
  follows_the_curve(DitherShape::flat, rate_44k);
  follows_the_curve(DitherShape::flat, rate_48k);
}

void shaped_noise_follows_its_curve() {
  for (const auto rate : {rate_44k, rate_48k}) {
    follows_the_curve(DitherShape::shaped, rate);
    follows_the_curve(DitherShape::psychoacoustic, rate);
  }
  // The psychoacoustic shape puts its noise where the ear hears least.
  const auto taps = dither_taps(DitherShape::psychoacoustic, rate_44k);
  constexpr std::size_t dip_steps = 6; // every 500 Hz from 2 to 5 kHz
  for (std::size_t step = 0; step <= dip_steps; ++step) {
    const double hz =
        dip_low_hz + (dip_high_hz - dip_low_hz) * static_cast<double>(step) / dip_steps;
    REQUIRE_LE(db(transfer_power(taps, hz, rate_44k)), -psychoacoustic_dip_db,
               "the F-weighted noise dips from 2 to 5 kHz");
  }
}

void high_rates_push_the_noise_above_the_band() {
  for (const auto rate : {rate_88k, rate_96k}) {
    for (const auto shape : {DitherShape::shaped, DitherShape::psychoacoustic}) {
      follows_the_curve(shape, rate);
      const auto taps = dither_taps(shape, rate);
      const auto steps = static_cast<std::size_t>(audible_hz / audible_step_hz);
      for (std::size_t step = 1; step <= steps; ++step) {
        const double hz = audible_step_hz * static_cast<double>(step);
        REQUIRE_LE(db(transfer_power(taps, hz, rate)), -high_rate_audible_cut_db,
                   "at the high rates the noise leaves the audible band");
      }
    }
  }
}

void feedback_stays_bounded_when_clipping() {
  DitherQuantizer quantizer(pcm16_bits, DitherShape::psychoacoustic, rate_44k, seed);
  for (std::size_t i = 0; i < clipping_samples; ++i) {
    (void)quantizer.quantize(i % 2 == 0 ? full_scale : -full_scale, 0);
  }
  // After clipping, silence quantises to the usual small noise.
  double largest = 0.0;
  for (std::size_t i = 0; i < clipping_samples; ++i) {
    largest = std::max(largest, std::fabs(static_cast<double>(quantizer.quantize(0.0F, 0))));
  }
  // The shaped error is the fed-back history (at most 1.5 LSB a tap) plus
  // the new error: bounded by 1.5 times the taps' magnitudes, plus one.
  double bound = 1.0;
  for (const double tap : dither_taps(DitherShape::psychoacoustic, rate_44k)) {
    bound += 1.5 * std::fabs(tap);
  }
  REQUIRE_LE(largest, bound, "the error feedback recovers from clipping");
}

void flat_is_the_seeded_tpdf() {
  DitherQuantizer quantizer(pcm16_bits, DitherShape::flat, rate_48k, seed);
  oscillate::foundation::SplitMix64 random(seed);
  for (std::size_t i = 0; i < clipping_samples; ++i) {
    const auto x = static_cast<float>(signal_level * std::sin(static_cast<double>(i)));
    const double noise = random.unit() - random.unit();
    const auto expected = static_cast<std::int32_t>(std::round(x * pcm16_scale + noise));
    REQUIRE_EQ(quantizer.quantize(x, i % 2), expected, "flat dither is plain seeded TPDF");
  }
}

void the_writer_applies_the_shape() {
  const oscillate::test::TempDir directory("oscillate-dither");
  const auto path = directory.path() / "shaped.wav";
  constexpr std::size_t frames = 512;
  std::vector<float> interleaved(2 * frames);
  for (std::size_t i = 0; i < interleaved.size(); ++i) {
    interleaved[i] = static_cast<float>(signal_level * std::sin(static_cast<double>(i)));
  }
  auto opened = WavWriter::begin(
      path, {rate_44k, WavFormat::pcm16, frames, seed, DitherShape::psychoacoustic});
  REQUIRE_TRUE(static_cast<bool>(opened), "the writer opens");
  auto writer = std::move(opened).value();
  REQUIRE_TRUE(static_cast<bool>(writer.write_interleaved_stereo(interleaved)), "it writes");
  REQUIRE_TRUE(static_cast<bool>(writer.finish()), "it publishes");
  std::ifstream in(path, std::ios::binary);
  const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), {});
  constexpr std::size_t header_bytes = 44;
  constexpr std::size_t sample_bytes = 2;
  constexpr unsigned byte_bits = 8;
  REQUIRE_EQ(bytes.size(), header_bytes + interleaved.size() * sample_bytes, "a PCM16 file");
  DitherQuantizer oracle(pcm16_bits, DitherShape::psychoacoustic, rate_44k, seed);
  for (std::size_t i = 0; i < interleaved.size(); ++i) {
    const auto low = static_cast<std::uint8_t>(bytes[header_bytes + sample_bytes * i]);
    const auto high = static_cast<std::uint8_t>(bytes[header_bytes + sample_bytes * i + 1]);
    const auto value =
        static_cast<std::int16_t>(static_cast<std::uint16_t>(low | (high << byte_bits)));
    REQUIRE_EQ(static_cast<std::int32_t>(value), oracle.quantize(interleaved[i], i % 2),
               "the writer quantises with the shape it was given");
  }
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "export dither contract passed",
      {
          {"flat_dither_is_white_at_a_quarter_lsb", flat_dither_is_white_at_a_quarter_lsb},
          {"shaped_noise_follows_its_curve", shaped_noise_follows_its_curve},
          {"high_rates_push_the_noise_above_the_band", high_rates_push_the_noise_above_the_band},
          {"feedback_stays_bounded_when_clipping", feedback_stays_bounded_when_clipping},
          {"flat_is_the_seeded_tpdf", flat_is_the_seeded_tpdf},
          {"the_writer_applies_the_shape", the_writer_applies_the_shape},
      });
}
