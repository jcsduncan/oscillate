#include "fixtures.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/signal.hpp"
#include "oscillate/warp_dsp/stream.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using oscillate::test::counting_process_allocations;
using oscillate::test::process_allocations;

namespace {
namespace warp = oscillate::warp_dsp;
constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;

using oscillate::test::require;

struct Rendered {
  std::vector<float> left;
  std::vector<float> right;
};

warp::WarpTimeMap stretch_map(std::uint64_t source_frames, double speed) {
  const auto output =
      static_cast<std::uint64_t>(std::llround(static_cast<double>(source_frames) / speed));
  auto map = warp::WarpTimeMap::create({{0, output, 0, source_frames << 16, false}});
  REQUIRE_OK(map, "stretch map rejected");
  return std::move(map).value();
}

std::unique_ptr<warp::WarpStream> complex(std::vector<std::uint64_t> onsets = {},
                                          warp::QualityTier tier = warp::QualityTier::q0) {
  auto created = warp::create_complex_stream({rate, 8'192}, tier, {std::move(onsets)});
  REQUIRE_OK(created, created ? "" : created.error().message);
  return std::move(created).value();
}

Rendered render(warp::WarpStream& stream, const warp::WarpTimeMap& map, warp::SourceView& source,
                std::uint64_t first = 0, std::size_t block = 128) {
  REQUIRE_EQ(stream.start(map, source, first), warp::StreamStatus::ok, "start failed");
  Rendered out;
  const auto total = static_cast<std::size_t>(map.output_frames() - first);
  out.left.resize(total);
  out.right.resize(total);
  std::size_t done = 0;
  std::uint64_t random = 13;
  while (done < total) {
    random = random * 6364136223846793005ULL + 1;
    const auto size = std::min(total - done, block != 0 ? block : 1 + (random >> 33) % 999);
    const auto written = stream.render(std::span(out.left).subspan(done, size),
                                       std::span(out.right).subspan(done, size));
    REQUIRE_EQ(written, size, "render returned short");
    done += written;
  }
  return out;
}

warp_fixtures::Stereo tone(double frequency, double seconds, double amplitude = 0.5) {
  auto signal = warp_fixtures::silence(static_cast<std::size_t>(seconds * rate));
  for (std::size_t index = 0; index < signal.left.size(); ++index) {
    const double phase = 2.0 * std::numbers::pi * frequency * static_cast<double>(index) / rate;
    signal.left[index] = static_cast<float>(amplitude * std::sin(phase));
    signal.right[index] = static_cast<float>(amplitude * std::sin(phase + 0.7));
  }
  return signal;
}

// Frequency from zero crossings with linear interpolation, over [begin, end).
double measured_frequency(const std::vector<float>& signal, std::size_t begin, std::size_t end) {
  double first = -1.0;
  double last = -1.0;
  std::size_t crossings = 0;
  for (auto index = begin + 1; index < end; ++index) {
    if (signal[index - 1] < 0.0F && signal[index] >= 0.0F) {
      const double fraction = signal[index - 1] / (signal[index - 1] - signal[index]);
      const double at = static_cast<double>(index - 1) + fraction;
      if (first < 0.0)
        first = at;
      else
        ++crossings;
      last = at;
    }
  }
  return static_cast<double>(crossings) * rate / (last - first);
}

using oscillate::test::rms;

void identity_reconstructs_source() {
  auto mix = warp_fixtures::plucks(rate, 4.0, 0.3, 2);
  warp_fixtures::add_noise_floor(mix, 1e-3, 3);
  warp::MemorySourceView view(mix.left, mix.right);
  const auto map = stretch_map(mix.left.size(), 1.0);
  auto stream = complex();
  const auto out = render(*stream, map, view);
  double error = 0.0;
  double energy = 0.0;
  for (std::size_t index = 0; index < out.left.size(); ++index) {
    error += std::pow(out.left[index] - mix.left[index], 2.0) +
             std::pow(out.right[index] - mix.right[index], 2.0);
    energy += std::pow(mix.left[index], 2.0) + std::pow(mix.right[index], 2.0);
  }
  const double snr = 10.0 * std::log10(energy / std::max(error, 1e-30));
  std::printf("complex identity SNR %.1f dB\n", snr);
  REQUIRE_GE(snr, 60.0, "unity Complex does not reconstruct the source");
}

void stretching_preserves_pitch_and_level() {
  for (const double speed : {0.25, 0.5, 0.8, 1.25, 2.0, 3.0}) {
    const auto source = tone(440.0, 4.0);
    warp::MemorySourceView view(source.left, source.right);
    const auto map = stretch_map(source.left.size(), speed);
    auto stream = complex();
    const auto out = render(*stream, map, view);
    const auto margin = static_cast<std::size_t>(0.1 * rate);
    const double frequency = measured_frequency(out.left, margin, out.left.size() - margin);
    const double cents = 1200.0 * std::log2(frequency / 440.0);
    double low = 1e9;
    double high = 0.0;
    for (auto at = margin; at + 4'800 < out.left.size() - margin; at += 4'800) {
      const double level = rms(out.left, at, at + 4'800);
      low = std::min(low, level);
      high = std::max(high, level);
    }
    const double ripple = 20.0 * std::log10(high / low);
    const double level = 20.0 * std::log10(high / (0.5 / std::numbers::sqrt2));
    std::printf("complex %.2fx: %.3f Hz (%.2f cents), level %.2f dB, ripple %.2f dB\n", speed,
                frequency, cents, level, ripple);
    REQUIRE_LT(std::fabs(cents), 1.0, "Complex stretch changed pitch");
    REQUIRE_LT(ripple, 0.5, "Complex stretch level unstable");
    REQUIRE_LT(std::fabs(level), 0.5, "Complex stretch level unstable");
  }
}

void anchored_clicks_land_without_pre_echo() {
  auto clicks = warp_fixtures::click_train(rate, 4.0, 0.25);
  warp::MemorySourceView view(clicks.left, clicks.right);
  for (const double speed : {0.37, 0.5, 0.8, 1.25, 2.0, 3.0}) {
    const auto map = stretch_map(clicks.left.size(), speed);
    auto stream = complex(clicks.onsets);
    const auto out = render(*stream, map, view);
    double worst_echo = -200.0;
    double worst_post = -200.0;
    std::size_t worst_offset = 0;
    for (const auto onset : clicks.onsets) {
      const auto expected = static_cast<std::size_t>(
          warp::WarpTimeMap::first_output_reaching(map.segments().front(), onset << 16));
      std::size_t peak = expected - 8;
      for (auto at = expected - 8; at <= expected + 8; ++at)
        if (std::fabs(out.left[at]) > std::fabs(out.left[peak]))
          peak = at;
      worst_offset = std::max(worst_offset, peak > expected ? peak - expected : expected - peak);
      // Pre-echo: energy in the 20 ms before the attack, relative to its peak.
      double before = 0.0;
      for (auto at = expected - static_cast<std::size_t>(0.02 * rate); at + 1 < expected; ++at)
        before = std::max(before, static_cast<double>(std::fabs(out.left[at])));
      worst_echo = std::max(worst_echo,
                            20.0 * std::log10(std::max(before, 1e-12) / std::fabs(out.left[peak])));
      // Post-echo: a single click must not repeat 3-60 ms after its attack.
      double after = 0.0;
      for (auto at = expected + static_cast<std::size_t>(0.003 * rate);
           at < expected + static_cast<std::size_t>(0.06 * rate); ++at)
        after = std::max(after, static_cast<double>(std::fabs(out.left[at])));
      worst_post = std::max(worst_post,
                            20.0 * std::log10(std::max(after, 1e-12) / std::fabs(out.left[peak])));
    }
    std::printf(
        "complex clicks %.2fx: worst offset %zu frames, pre-echo %.1f dB, post-echo %.1f dB\n",
        speed, worst_offset, worst_echo, worst_post);
    REQUIRE_LE(worst_offset, 2, "anchored click landed off its mapped frame");
    REQUIRE_LE(worst_echo, -40.0, "anchored click has pre-echo");
    REQUIRE_LE(worst_post, -40.0, "anchored click repeats after its attack");
  }
}

void endpoints_are_exact() {
  // Impulses on the first and last source frames land on the first and last
  // output frames at any ratio.
  for (const double speed : {0.5, 0.75, 1.25, 2.0, 3.0}) {
    auto signal = warp_fixtures::silence(96'000);
    signal.left.front() = signal.right.front() = 0.9F;
    signal.left.back() = signal.right.back() = 0.9F;
    warp::MemorySourceView view(signal.left, signal.right);
    const auto map = stretch_map(signal.left.size(), speed);
    auto stream = complex();
    const auto out = render(*stream, map, view);
    const auto last = out.left.size() - 1;
    std::printf("complex endpoints %.2fx: first %.3f last %.3f\n", speed,
                static_cast<double>(out.left.front()), static_cast<double>(out.left[last]));
    REQUIRE_GT(std::fabs(out.left.front()), 0.9F * 0.7F, "endpoint attack lost or moved");
    REQUIRE_GT(std::fabs(out.left[last]), 0.9F * 0.7F, "endpoint attack lost or moved");
  }
}

double worst_step(const std::vector<float>& signal, std::size_t begin, std::size_t end) {
  double worst = 0.0;
  for (auto index = begin + 1; index < end; ++index)
    worst = std::max(worst, static_cast<double>(std::fabs(signal[index] - signal[index - 1])));
  return worst;
}

void markers_and_splices_are_seamless() {
  const auto source = tone(220.0, 6.0);
  warp::MemorySourceView view(source.left, source.right);
  const double source_step = worst_step(source.left, 1, source.left.size());
  // Ratio change at a marker, then a loop splice back to an earlier position.
  auto map = warp::WarpTimeMap::create({{0, 48'000, 0, 24'000ULL << 16, false},
                                        {48'000, 72'000, 24'000ULL << 16, 72'000ULL << 16, false},
                                        {72'000, 120'000, 30'000ULL << 16, 78'000ULL << 16, true}});
  REQUIRE_OK(map, "seam map rejected");
  auto stream = complex();
  const auto out = render(*stream, map.value(), view);
  const double step = worst_step(out.left, 100, out.left.size() - 100);
  for (std::size_t at = 100; at + 2'000 < out.left.size(); at += 2'000) {
    const double local = worst_step(out.left, at, at + 2'000);
    if (local > 1.5 * source_step)
      std::printf("  step %.4f in [%zu, %zu)\n", local, at, at + 2'000);
  }
  std::printf("complex seams: worst step %.4f vs source %.4f\n", step, source_step);
  REQUIRE_LE(step, 2.0 * source_step, "Complex marker or splice seam clicked");
}

void partition_and_seek_are_deterministic() {
  auto drums = warp_fixtures::drum_hits(rate, 4.0, 0.3, 4);
  warp::MemorySourceView view(drums.left, drums.right);
  const auto map = stretch_map(drums.left.size(), 0.7);
  auto stream = complex(drums.onsets);
  const auto reference = render(*stream, map, view, 0, 8'192);
  for (const std::size_t block : {std::size_t{1}, std::size_t{64}, std::size_t{0}}) {
    const auto pieces = render(*stream, map, view, 0, block);
    REQUIRE_EQ(pieces.left, reference.left, "Complex output depends on render partition");
    REQUIRE_EQ(pieces.right, reference.right, "Complex output depends on render partition");
  }
  // A seek a little after an onset restarts from that onset's epoch exactly.
  for (std::size_t index = 2; index < drums.onsets.size(); index += 5) {
    const auto at =
        warp::WarpTimeMap::first_output_reaching(map.segments().front(), drums.onsets[index] << 16);
    const auto seek = at + 5'000;
    const auto tail = render(*stream, map, view, seek, 0);
    require(std::equal(tail.left.begin(), tail.left.end(),
                       reference.left.begin() + static_cast<std::ptrdiff_t>(seek)),
            "Complex seek near an epoch is not bit-exact");
  }
  // Far from any epoch the seek converges in level.
  const auto pad = warp_fixtures::vibrato_pad(rate, 8.0);
  warp::MemorySourceView pad_view(pad.left, pad.right);
  const auto pad_map = stretch_map(pad.left.size(), 0.6);
  const auto whole = render(*stream, pad_map, pad_view, 0, 0);
  const std::uint64_t seek = std::uint64_t{9} * rate;
  const auto tail = render(*stream, pad_map, pad_view, seek, 0);
  for (std::size_t at = 4'096; at + 4'800 < tail.left.size(); at += 4'800) {
    const double a = rms(tail.left, at, at + 4'800);
    const double b = rms(whole.left, seek + at, seek + at + 4'800);
    REQUIRE_LT(std::fabs(20.0 * std::log10(a / b)), 0.5, "Complex seek level did not converge");
  }
}

std::unique_ptr<warp::WarpStream> complex_pro(std::int32_t cents, float formants,
                                              std::vector<std::uint64_t> onsets = {}) {
  auto created = warp::create_complex_pro_stream({rate, 8'192}, warp::QualityTier::q0,
                                                 {std::move(onsets), cents, formants, 128.0F});
  REQUIRE_OK(created, created ? "" : created.error().message);
  return std::move(created).value();
}

void transpose_is_exact_at_every_ratio() {
  for (const std::int32_t cents : {-2'400, -1'200, -700, 700, 1'200, 2'400}) {
    for (const double speed : {0.5, 1.0, 2.0}) {
      const auto source = tone(440.0, 3.0);
      warp::MemorySourceView view(source.left, source.right);
      const auto map = stretch_map(source.left.size(), speed);
      auto created = warp::create_complex_stream({rate, 8'192}, warp::QualityTier::q0, {{}, cents});
      REQUIRE_OK(created, "transposed Complex creation failed");
      const auto out = render(*created.value(), map, view);
      const auto margin = static_cast<std::size_t>(0.15 * rate);
      const double expected = 440.0 * std::exp2(cents / 1200.0);
      const double frequency = measured_frequency(out.left, margin, out.left.size() - margin);
      const double error = 1200.0 * std::log2(frequency / expected);
      double low = 1e9;
      double high = 0.0;
      for (auto at = margin; at + 4'800 < out.left.size() - margin; at += 4'800) {
        const double level = rms(out.left, at, at + 4'800);
        low = std::min(low, level);
        high = std::max(high, level);
      }
      const double ripple = 20.0 * std::log10(high / low);
      std::printf("complex %+5d cents at %.1fx: %.3f Hz (%.2f cents off), ripple %.2f dB\n", cents,
                  speed, frequency, error, ripple);
      REQUIRE_LT(std::fabs(error), 1.0, "Complex transpose is off pitch");
      REQUIRE_LT(ripple, 0.5, "Complex transpose level unstable");
    }
  }
}

void transposed_clicks_stay_anchored() {
  auto clicks = warp_fixtures::click_train(rate, 3.0, 0.25);
  warp::MemorySourceView view(clicks.left, clicks.right);
  for (const std::int32_t cents : {-1'200, 700}) {
    const auto map = stretch_map(clicks.left.size(), 0.6);
    auto created =
        warp::create_complex_stream({rate, 8'192}, warp::QualityTier::q0, {clicks.onsets, cents});
    REQUIRE_OK(created, "transposed click stream creation failed");
    const auto out = render(*created.value(), map, view);
    std::size_t worst = 0;
    for (const auto onset : clicks.onsets) {
      const auto expected = static_cast<std::size_t>(
          warp::WarpTimeMap::first_output_reaching(map.segments().front(), onset << 16));
      std::size_t peak = expected - 16;
      for (auto at = expected - 16; at <= expected + 16; ++at)
        if (std::fabs(out.left[at]) > std::fabs(out.left[peak]))
          peak = at;
      worst = std::max(worst, peak > expected ? peak - expected : expected - peak);
    }
    std::printf("complex transposed clicks %+d cents: worst offset %zu frames\n", cents, worst);
    REQUIRE_LE(worst, 1, "transposed click left its mapped frame");
  }
}

// Synthetic vowel: 150 Hz harmonics shaped by resonances at 700 and 1200 Hz.
warp_fixtures::Stereo vowel(double seconds) {
  auto signal = warp_fixtures::silence(static_cast<std::size_t>(seconds * rate));
  const auto envelope = [](double frequency) {
    const auto resonance = [frequency](double centre, double width) {
      return 1.0 / (1.0 + std::pow((frequency - centre) / width, 2.0));
    };
    return resonance(700.0, 160.0) + 0.6 * resonance(1'200.0, 200.0) + 0.02;
  };
  for (int harmonic = 1; 150.0 * harmonic < 6'000.0; ++harmonic) {
    const double frequency = 150.0 * harmonic;
    const double amplitude = 0.05 * envelope(frequency);
    for (std::size_t index = 0; index < signal.left.size(); ++index) {
      const auto sample =
          static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * frequency *
                                                      static_cast<double>(index) / rate +
                                                  0.3 * harmonic));
      signal.left[index] += sample;
      signal.right[index] += sample;
    }
  }
  return signal;
}

// Amplitude-weighted centroid of the harmonics of f0 between 200 and 3000 Hz,
// each measured with a long Hann-windowed DFT probe: a robust summary of
// where the spectral envelope sits.
struct Band {
  double low;
  double high;
};

double envelope_centroid(const std::vector<float>& signal, double f0, Band band) {
  const std::size_t begin = signal.size() / 2 - 16'384;
  const std::size_t length = 32'768;
  double weighted = 0.0;
  double total = 0.0;
  for (int harmonic = 1; f0 * harmonic < band.high; ++harmonic) {
    const double frequency = f0 * harmonic;
    if (frequency < band.low)
      continue;
    double re = 0.0;
    double im = 0.0;
    for (std::size_t index = 0; index < length; ++index) {
      const double window =
          0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(index) / length);
      const double angle = 2.0 * std::numbers::pi * frequency * static_cast<double>(index) / rate;
      re += window * signal[begin + index] * std::cos(angle);
      im -= window * signal[begin + index] * std::sin(angle);
    }
    const double magnitude = std::hypot(re, im);
    weighted += magnitude * frequency;
    total += magnitude;
  }
  return weighted / total;
}

void complex_pro_preserves_formants() {
  const auto source = vowel(3.0);
  warp::MemorySourceView view(source.left, source.right);
  const Band band{200.0, 3'000.0};
  const double original = envelope_centroid(source.left, 150.0, band);
  const auto map = stretch_map(source.left.size(), 0.8);
  double worst_kept = 0.0;
  double worst_moved = 0.0;
  for (const std::int32_t cents : {-700, -300, 300, 700}) {
    const double ratio = std::exp2(cents / 1200.0);
    auto kept = complex_pro(cents, 100.0F);
    const auto with = render(*kept, map, view);
    auto moved = complex_pro(cents, 0.0F);
    const auto without = render(*moved, map, view);
    const double kept_shift = envelope_centroid(with.left, 150.0 * ratio, band) / original - 1.0;
    // Without preservation the whole spectrum scales, so compare over the
    // correspondingly scaled band.
    const double moved_shift =
        envelope_centroid(without.left, 150.0 * ratio, {band.low * ratio, band.high * ratio}) /
            (original * ratio) -
        1.0;
    std::printf("complex pro %+d cents: envelope centroid shift %+.2f%% with formants; without, "
                "%+.2f%% from a pure %+.1f%% pitch scaling\n",
                cents, 100.0 * kept_shift, 100.0 * moved_shift, 100.0 * (ratio - 1.0));
    worst_kept = std::max(worst_kept, std::fabs(kept_shift));
    worst_moved = std::max(worst_moved, std::fabs(moved_shift));
  }
  REQUIRE_LT(worst_kept, 0.03, "Complex Pro moved the formant envelope");
  REQUIRE_LT(worst_moved, 0.01, "formants off did not scale the envelope with the pitch");
}

// A full-mix stand-in: many partials with a pink tilt, band-limited at
// 16 kHz like a lossy-coded source, so everything above is near silence.
warp_fixtures::Stereo band_limited_mix(double seconds) {
  auto signal = warp_fixtures::silence(static_cast<std::size_t>(seconds * rate));
  std::uint64_t seed = 0x9e3779b97f4a7c15ULL;
  for (int partial = 0; partial < 240; ++partial) {
    const double frequency = 50.0 * std::pow(16'000.0 / 50.0, partial / 239.0);
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    const double phase = static_cast<double>(seed >> 11U) * 0x1.0p-53 * 2.0 * std::numbers::pi;
    const double amplitude = 0.03 / std::sqrt(frequency / 50.0);
    for (std::size_t index = 0; index < signal.left.size(); ++index) {
      const auto sample = static_cast<float>(
          amplitude *
          std::sin(2.0 * std::numbers::pi * frequency * static_cast<double>(index) / rate + phase));
      signal.left[index] += sample;
      signal.right[index] += sample;
    }
  }
  // Coding noise about 70 dB down fills the band above the cutoff.
  warp_fixtures::add_noise_floor(signal, 3e-4, 7);
  return signal;
}

// A bass-heavy mix: a loud 55 Hz bass harmonic series (as in a mastered
// full mix) over pink partials up to 16 kHz about 15 dB lower.
warp_fixtures::Stereo bass_heavy_mix(double seconds) {
  auto signal = band_limited_mix(seconds);
  for (int harmonic = 1; 55.0 * harmonic < 450.0; ++harmonic) {
    const double frequency = 55.0 * harmonic;
    const double amplitude = 0.12 / harmonic;
    for (std::size_t index = 0; index < signal.left.size(); ++index) {
      const auto sample =
          static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * frequency *
                                                      static_cast<double>(index) / rate +
                                                  0.7 * harmonic));
      signal.left[index] += sample;
      signal.right[index] += sample;
    }
  }
  return signal;
}

struct Level {
  double rms_db; // output RMS relative to the source, steady state
  float peak;
  std::size_t over_unity;
};

Level level(const std::vector<float>& out, const std::vector<float>& source) {
  const auto margin = static_cast<std::size_t>(0.2 * rate);
  float peak = 0.0F;
  std::size_t over = 0;
  for (const auto sample : out) {
    peak = std::max(peak, std::fabs(sample));
    over += std::fabs(sample) >= 1.0F ? 1U : 0U;
  }
  return {20.0 * std::log10(rms(out, margin, out.size() - margin) /
                            rms(source, margin, source.size() - margin)),
          peak, over};
}

// Formant preservation reshapes the spectrum; it must not change the level.
// Re-enveloping a transposed partial-rich frame changes its energy by about
// 10 log10(1/p) dB (more partials under each formant going down), which on a
// mastered mix drove a fifth-down render to twice full scale. Each shaped
// frame is scaled back to its unshaped energy, so the level with formants
// tracks the same transposition without them, in both directions, on a
// band-limited mix and on a bass-heavy one. complex_pro_preserves_formants
// checks that the envelope itself still stays in place.
void complex_pro_formants_keep_the_level() {
  for (const bool bass : {false, true}) {
    const auto source = bass ? bass_heavy_mix(1.5) : band_limited_mix(1.5);
    warp::MemorySourceView view(source.left, source.right);
    const auto map = stretch_map(source.left.size(), 1.0);
    for (const std::int32_t cents : {-1'200, -700, 700, 1'200}) {
      auto kept = complex_pro(cents, 100.0F);
      const auto with = level(render(*kept, map, view).left, source.left);
      auto moved = complex_pro(cents, 0.0F);
      const auto without = level(render(*moved, map, view).left, source.left);
      std::printf("complex pro %+5d cents, %s mix: formants %+.2f dB peak %.3f; without "
                  "%+.2f dB peak %.3f\n",
                  cents, bass ? "bass-heavy" : "band-limited", with.rms_db, with.peak,
                  without.rms_db, without.peak);
      REQUIRE_LT(std::fabs(with.rms_db - without.rms_db), 1.0,
                 "formant preservation changed the level of the transposed mix");
      REQUIRE_EQ(with.over_unity, 0, "formant preservation raised the transposed peak");
      REQUIRE_LT(with.peak, 1.25F * without.peak,
                 "formant preservation raised the transposed peak");
    }
  }
}

void transposed_output_is_deterministic() {
  auto drums = warp_fixtures::drum_hits(rate, 3.0, 0.3, 8);
  warp::MemorySourceView view(drums.left, drums.right);
  const auto map = stretch_map(drums.left.size(), 0.8);
  auto stream = complex_pro(-300, 100.0F, drums.onsets);
  const auto reference = render(*stream, map, view, 0, 8'192);
  for (const std::size_t block : {std::size_t{1}, std::size_t{128}, std::size_t{0}}) {
    const auto pieces = render(*stream, map, view, 0, block);
    REQUIRE_EQ(pieces.left, reference.left, "transposed output depends on partition");
  }
  const auto at =
      warp::WarpTimeMap::first_output_reaching(map.segments().front(), drums.onsets[4] << 16);
  const auto seek = at + 3'000;
  const auto tail = render(*stream, map, view, seek, 0);
  require(std::equal(tail.left.begin(), tail.left.end(),
                     reference.left.begin() + static_cast<std::ptrdiff_t>(seek)),
          "transposed seek near an epoch is not bit-exact");
}

void steady_state_does_not_allocate() {
  auto drums = warp_fixtures::drum_hits(rate, 3.0, 0.25, 6);
  warp::MemorySourceView view(drums.left, drums.right);
  const auto map = stretch_map(drums.left.size(), 0.6);
  auto stream = complex_pro(700, 100.0F, drums.onsets);
  std::vector<float> left(128);
  std::vector<float> right(128);
  counting_process_allocations = true;
  const bool started = stream->start(map, view, 0) == warp::StreamStatus::ok;
  while (started && stream->next_output_frame() < map.output_frames())
    (void)stream->render(left, right);
  counting_process_allocations = false;
  REQUIRE_TRUE(started, "Complex start()/render() allocated");
  REQUIRE_EQ(process_allocations.load(), 0, "Complex start()/render() allocated");
}

void dense_map_lookup_matches_sparse_map_without_allocation() {
  constexpr std::uint64_t frames = 300'000;
  constexpr std::array<std::uint64_t, 2> splices{4'097, 262'145};
  std::vector<warp::TimeMapSegment> dense;
  dense.reserve(frames);
  for (std::uint64_t at = 0; at < frames; ++at)
    dense.push_back({at, at + 1, at << 16, (at + 1) << 16, at == splices[0] || at == splices[1]});
  auto dense_map = warp::WarpTimeMap::create(std::move(dense));
  REQUIRE_OK(dense_map, "dense Complex map rejected");
  auto sparse_map =
      warp::WarpTimeMap::create({{0, splices[0], 0, splices[0] << 16, false},
                                 {splices[0], splices[1], splices[0] << 16, splices[1] << 16, true},
                                 {splices[1], frames, splices[1] << 16, frames << 16, true}});
  REQUIRE_OK(sparse_map, "sparse Complex reference map rejected");
  auto signal = tone(220.0, static_cast<double>(frames) / rate);
  warp::MemorySourceView source(signal.left, signal.right);
  auto dense_stream = complex({280'000});
  auto sparse_stream = complex({280'000});
  std::array<float, 512> dense_left{};
  std::array<float, 512> dense_right{};
  std::array<float, 512> sparse_left{};
  std::array<float, 512> sparse_right{};
  // Cross the bit-index word, group and root boundaries. A long splice-free
  // run and an onset also exercise source-to-output lookup inside the run.
  for (const std::uint64_t seek : {4'000ULL, 250'000ULL, 262'100ULL, 279'900ULL, 290'000ULL}) {
    process_allocations = 0;
    counting_process_allocations = true;
    const bool dense_started =
        dense_stream->start(dense_map.value(), source, seek) == warp::StreamStatus::ok;
    const auto dense_written = dense_started ? dense_stream->render(dense_left, dense_right) : 0;
    counting_process_allocations = false;
    REQUIRE_TRUE(dense_started, "dense Complex start/render failed or allocated");
    REQUIRE_EQ(dense_written, dense_left.size(), "dense Complex start/render failed or allocated");
    REQUIRE_EQ(process_allocations.load(), 0, "dense Complex start/render failed or allocated");
    REQUIRE_EQ(sparse_stream->start(sparse_map.value(), source, seek), warp::StreamStatus::ok,
               "sparse Complex start failed");
    REQUIRE_EQ(sparse_stream->render(sparse_left, sparse_right), sparse_left.size(),
               "sparse Complex render short");
    REQUIRE_EQ(dense_left, sparse_left,
               "dense Complex map changed output, onset or seek semantics");
    REQUIRE_EQ(dense_right, sparse_right,
               "dense Complex map changed output, onset or seek semantics");
  }
}

void rejects_invalid_quality_tier() {
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): an invalid tier on purpose.
  const auto invalid = static_cast<warp::QualityTier>(255);
  require(!warp::create_complex_stream({rate, 128}, invalid, {}).has_value(),
          "Complex accepted invalid quality tier");
  require(!warp::create_complex_pro_stream({rate, 128}, invalid, {}).has_value(),
          "Complex Pro accepted invalid quality tier");
}

void close_onsets_at_half_speed_do_not_invert_source_bounds() {
  // A half-speed map separates these attacks by more than one FFT window in
  // output time, while their source positions are less than one window apart.
  // The two source-side unity zones overlap, so there is no correction range
  // between them. Recorded music can have this spacing even with sparse beats.
  auto signal = tone(220.0, 1.0);
  signal.left[521] += 0.3F;
  signal.right[521] += 0.3F;
  signal.left[3'786] += 0.3F;
  signal.right[3'786] += 0.3F;
  warp::MemorySourceView source(signal.left, signal.right);
  const auto map = stretch_map(signal.left.size(), 0.5);
  for (const bool pro : {false, true}) {
    const auto make = [pro] {
      if (pro) {
        auto created =
            warp::create_complex_pro_stream({rate, 8'192}, warp::QualityTier::q0, {{521, 3'786}});
        REQUIRE_OK(created, "Complex Pro close-onset stream rejected");
        return std::move(created).value();
      }
      return complex({521, 3'786});
    };
    auto first = make();
    auto second = make();
    const auto a = render(*first, map, source, 0, 128);
    const auto b = render(*second, map, source, 0, 97);
    REQUIRE_EQ(a.left, b.left, "close-onset Complex render depends on pull size");
    REQUIRE_EQ(a.right, b.right, "close-onset Complex render depends on pull size");
    REQUIRE_TRUE(std::ranges::all_of(a.left, [](float value) { return std::isfinite(value); }),
                 "close-onset Complex render produced nonfinite samples");
    REQUIRE_TRUE(std::ranges::all_of(a.right, [](float value) { return std::isfinite(value); }),
                 "close-onset Complex render produced nonfinite samples");
  }
}

} // namespace

int main() {
  try {
    identity_reconstructs_source();
    stretching_preserves_pitch_and_level();
    anchored_clicks_land_without_pre_echo();
    endpoints_are_exact();
    markers_and_splices_are_seamless();
    partition_and_seek_are_deterministic();
    transpose_is_exact_at_every_ratio();
    transposed_clicks_stay_anchored();
    complex_pro_preserves_formants();
    complex_pro_formants_keep_the_level();
    transposed_output_is_deterministic();
    steady_state_does_not_allocate();
    dense_map_lookup_matches_sparse_map_without_allocation();
    rejects_invalid_quality_tier();
    close_onsets_at_half_speed_do_not_invert_source_bounds();
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
