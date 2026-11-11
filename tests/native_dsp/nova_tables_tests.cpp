#include "oscillate/native_dsp/synth/nova/tables.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"

#include "dsp/offline_fft.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <numbers>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

// Nova's wavetables and noises (spec 08 §3) against independent checks:
// every level is band-limited to its harmonic limit (by FFT), level_for
// never lets a kept harmonic reach 0.45 fs, guards wrap, tables normalise
// with no DC, a sine survives resampling, the spectral variant lands on
// its source frames and travels rather than combs, every factory table and
// noise is sane, and noise loops have no seam.
namespace {
namespace dsp = oscillate::native_dsp::dsp;
namespace nova = oscillate::native_dsp::synth::nova;
using Complex = std::complex<double>;
constexpr double two_pi = 2.0 * std::numbers::pi;

using oscillate::test::require;

std::shared_ptr<const nova::Wavetable> build(std::span<const float> mono, std::size_t frame_length,
                                             std::string name) {
  auto result = nova::build_wavetable(mono, frame_length, std::move(name));
  REQUIRE_OK(result, "table builds");
  return std::move(result).value();
}

// Naive (aliased-on-purpose) cycles: a raw saw contains energy at every bin.
std::vector<float> raw_saws(std::size_t frames, std::size_t length) {
  std::vector<float> mono(frames * length);
  for (std::size_t f = 0; f < frames; ++f) {
    for (std::size_t n = 0; n < length; ++n) {
      const double x = static_cast<double>(n) / static_cast<double>(length);
      const double sharp = 2.0 * x - 1.0;
      const double mix =
          static_cast<double>(f) / static_cast<double>(std::max<std::size_t>(frames - 1, 1));
      mono[f * length + n] = static_cast<float>((1.0 - mix) * sharp + mix * (x < 0.3 ? 1.0 : -1.0));
    }
  }
  return mono;
}

std::vector<Complex> transform(const float* frame, std::size_t length) {
  std::vector<Complex> bins(frame, frame + length);
  dsp::OfflineFft(length).forward(bins);
  return bins;
}

// Above the level's harmonic limit, no bin comes within 100 dB of the
// strongest.
void check_band_limits(const nova::Wavetable& table, std::string_view what) {
  const std::size_t step = std::max<std::size_t>(1, table.frames() / 7);
  for (std::size_t l = 0; l < nova::table_levels; ++l) {
    const auto& level = table.level(l);
    REQUIRE_GE(level.length, 256, "level length is a power of two in 256..2048");
    REQUIRE_LE(level.length, nova::table_frame_size, "level length is a power of two in 256..2048");
    REQUIRE_TRUE(std::has_single_bit(level.length), "level length is a power of two in 256..2048");
    REQUIRE_EQ(level.harmonics, (nova::table_frame_size / 2) >> l, "level harmonic limit");
    REQUIRE_EQ(level.stride, level.length + 2 * nova::table_guard, "level stride");
    REQUIRE_EQ(level.data.size(), table.frames() * level.stride, "level size");
    for (std::size_t f = 0; f < table.frames(); f += step) {
      const auto bins = transform(table.frame(l, f), level.length);
      double strongest = 0.0;
      double above = 0.0;
      for (std::size_t k = 1; k <= level.length / 2; ++k) {
        const double magnitude = std::abs(bins[k]);
        strongest = std::max(strongest, magnitude);
        if (k > level.harmonics) {
          above = std::max(above, magnitude);
        }
      }
      REQUIRE_GT(strongest, 0.0, "a level frame is not silent");
      if (above > strongest * 1e-5) {
        throw std::runtime_error(std::string(what) + ": level " + std::to_string(l) +
                                 " has energy above its limit");
      }
    }
  }
}

void check_shape(const nova::Wavetable& table, std::string_view what) {
  float peak = 0.0F;
  for (std::size_t l = 0; l < nova::table_levels; ++l) {
    const auto& level = table.level(l);
    for (std::size_t f = 0; f < table.frames(); ++f) {
      const float* frame = table.frame(l, f);
      double sum = 0.0;
      for (std::size_t n = 0; n < level.length; ++n) {
        if (!std::isfinite(frame[n])) {
          throw std::runtime_error(std::string(what) + " is not finite");
        }
        sum += frame[n];
        if (l == 0) {
          peak = std::max(peak, std::fabs(frame[n]));
        }
      }
      if (std::fabs(sum / level.length) > 1e-5) {
        throw std::runtime_error(std::string(what) + " has DC");
      }
      for (std::size_t g = 1; g <= nova::table_guard; ++g) {
        const auto before = frame[-static_cast<std::ptrdiff_t>(g)];
        const auto after = frame[level.length + g - 1];
        if (before != frame[level.length - g] || after != frame[g - 1]) {
          throw std::runtime_error(std::string(what) + " guard does not wrap");
        }
      }
    }
  }
  if (std::fabs(peak - 1.0F) > 1e-5F) {
    throw std::runtime_error(std::string(what) + " level 0 does not peak at 1");
  }
}

double frame_rms(const nova::Wavetable& table, std::size_t level, std::size_t index) {
  const float* frame = table.frame(level, index);
  const auto length = table.level(level).length;
  double energy = 0.0;
  for (std::size_t n = 0; n < length; ++n) {
    energy += static_cast<double>(frame[n]) * frame[n];
  }
  return std::sqrt(energy / length);
}

void sine_resamples_to_a_pure_sine() {
  for (const std::size_t length : {std::size_t{600}, std::size_t{2'048}, std::size_t{97}}) {
    std::vector<float> mono(3 * length);
    for (std::size_t n = 0; n < mono.size(); ++n) {
      mono[n] = static_cast<float>(
          0.6 * std::sin(two_pi * static_cast<double>(n % length) / static_cast<double>(length)));
    }
    const auto table = build(mono, length, "sine");
    REQUIRE_EQ(table->frames(), 3, "three frames");
    for (std::size_t l = 0; l < nova::table_levels; ++l) {
      const auto& level = table->level(l);
      const float* frame = table->frame(l, 1);
      for (std::size_t n = 0; n < level.length; ++n) {
        const double expected = std::sin(two_pi * static_cast<double>(n) / level.length);
        REQUIRE_LT(std::fabs(frame[n] - expected), tolerance::float_processing,
                   "a resampled sine stays a pure sine");
      }
    }
  }
}

void levels_are_band_limited() {
  const auto mono = raw_saws(5, 2'048);
  const auto table = build(mono, 2'048, "raw saws");
  check_band_limits(*table, "raw saws");
  check_band_limits(*table->spectral(), "raw saws (spectral)");
  // A non-power-of-two source exercises the chirp transform.
  const auto odd = raw_saws(3, 1'000);
  check_band_limits(*build(odd, 1'000, "odd saws"), "odd saws");
}

void level_for_stays_under_the_band_edge() {
  const auto mono = raw_saws(1, 2'048);
  const auto table = build(mono, 2'048, "saw");
  for (const double rate : {44'100.0, 48'000.0, 96'000.0, 192'000.0}) {
    for (int note = 0; note <= 127; ++note) {
      const double hz = 440.0 * std::exp2((note - 69) / 12.0);
      const double cycles = hz / rate;
      const std::size_t l = table->level_for(cycles);
      REQUIRE_LT(l, nova::table_levels, "a valid level");
      const double top = table->level(l).harmonics * hz;
      require(top < 0.45 * rate || l + 1 == nova::table_levels,
              "the top kept harmonic stays below 0.45 fs");
      if (l > 0) {
        REQUIRE_GE(table->level(l - 1).harmonics * hz, 0.45 * rate,
                   "level_for picks the richest safe level");
      }
      REQUIRE_EQ(table->level_for(-cycles), l, "negative frequencies read the same level");
    }
  }
  REQUIRE_EQ(table->level_for(0.0), 0, "a stopped read keeps every harmonic");
  REQUIRE_EQ(table->level_for(0.6), nova::table_levels - 1, "above the edge: the fundamental");
}

void tables_normalise_wrap_and_have_no_dc() {
  std::vector<float> mono = raw_saws(4, 2'048);
  for (auto& value : mono) {
    value = 0.3F * value + 0.25F; // Quiet and offset.
  }
  const auto table = build(mono, 2'048, "offset");
  check_shape(*table, "offset");
}

void frames_thin_and_partial_frames_drop() {
  const auto many = raw_saws(301, 256);
  const auto table = build(std::span(many).first(300 * 256 + 100), 256, "many");
  REQUIRE_EQ(table->frames(), nova::max_table_frames, "300 frames thin to 256");
  const auto one = raw_saws(1, 3'000);
  const auto whole = build(one, 0, "whole");
  REQUIRE_EQ(whole->frames(), 1, "frame length 0 is one frame");
  REQUIRE_EQ(whole->spectral(), nullptr, "a one-frame table has no spectral variant");
}

void spectral_variant_lands_on_the_source() {
  const auto mono = raw_saws(4, 2'048);
  const auto table = build(mono, 2'048, "four");
  const auto* spectral = table->spectral();
  REQUIRE_NE(spectral, nullptr, "a multi-frame table has a spectral variant");
  REQUIRE_EQ(spectral->frames(), nova::max_table_frames, "the spectral variant has 256 frames");
  for (std::size_t l = 0; l < nova::table_levels; ++l) {
    const auto length = table->level(l).length;
    REQUIRE_EQ(spectral->level(l).length, length, "spectral levels match");
    const std::array<std::pair<std::size_t, std::size_t>, 2> ends{
        {{0, 0}, {3, nova::max_table_frames - 1}}};
    for (const auto& [source, morphed] : ends) {
      const float* a = table->frame(l, source);
      const float* b = spectral->frame(l, morphed);
      for (std::size_t n = 0; n < length; ++n) {
        REQUIRE_LT(std::fabs(a[n] - b[n]), tolerance::near_silence,
                   "spectral ends match their source frames");
      }
    }
  }
  check_band_limits(*spectral, "spectral");
  // A 256-frame source keeps its frames.
  const auto full = raw_saws(256, 256);
  const auto full_table = build(full, 256, "full");
  REQUIRE_NE(full_table->spectral(), nullptr, "a full table's spectral variant has 256 frames");
  REQUIRE_EQ(full_table->spectral()->frames(), nova::max_table_frames,
             "a full table's spectral variant has 256 frames");
}

// A saw and the same saw a quarter cycle later: the spectral morph moves
// the saw (its level holds), where a crossfade would comb it.
void spectral_morph_travels() {
  constexpr std::size_t length = 2'048;
  std::vector<float> mono(2 * length);
  for (std::size_t n = 0; n < length; ++n) {
    const double x = static_cast<double>(n) / static_cast<double>(length);
    mono[n] = static_cast<float>(2.0 * (x - std::floor(x)) - 1.0);
    const double y = x - 0.25;
    mono[length + n] = static_cast<float>(2.0 * (y - std::floor(y)) - 1.0);
  }
  const auto table = build(mono, length, "shift");
  const double source = frame_rms(*table, 2, 0);
  const double middle = frame_rms(*table->spectral(), 2, 128);
  REQUIRE_LT(std::fabs(middle / source - 1.0), 0.02, "a spectral morph keeps a moving saw's level");
  for (std::size_t j = 0; j < nova::max_table_frames; ++j) {
    const double level = frame_rms(*table->spectral(), 0, j);
    REQUIRE_GT(level, 0.9 * source, "no spectral frame combs out");
  }
}

void spectra_follow_the_sine_phase_convention() {
  std::vector<std::vector<float>> magnitudes{{1.0F}, {0.0F, 0.5F}};
  std::vector<std::vector<float>> phases{{0.0F}, {0.0F, 0.0F}};
  auto result = nova::build_wavetable_from_spectra(magnitudes, phases, "spectra");
  REQUIRE_OK(result, "spectra build");
  const auto& table = *result.value();
  REQUIRE_EQ(table.frames(), 2, "two frames from spectra");
  const float* frame = table.frame(0, 0);
  REQUIRE_LT(std::fabs(frame[0]), tolerance::float_round_trip,
             "phase 0 is a sine starting at zero");
  REQUIRE_LT(std::fabs(frame[512] - 1.0F), tolerance::float_processing,
             "phase 0 is a sine starting at zero");
  // Frame 1 is half the level of frame 0 (one gain for the whole table).
  REQUIRE_LT(std::fabs(frame_rms(table, 0, 1) - 0.5 * frame_rms(table, 0, 0)),
             tolerance::float_processing, "frames keep their relative loudness");
}

void invalid_input_fails() {
  const std::vector<float> empty;
  require(!nova::build_wavetable(empty, 2'048, "empty").has_value(), "no audio fails");
  const std::vector<float> short_audio(1'000, 0.5F);
  require(!nova::build_wavetable(short_audio, 2'048, "short").has_value(),
          "audio shorter than a frame fails");
  const std::vector<float> silent(2'048, 0.0F);
  require(!nova::build_wavetable(silent, 2'048, "silent").has_value(), "a silent table fails");
  auto broken = raw_saws(1, 2'048);
  broken[7] = std::nanf("");
  require(!nova::build_wavetable(broken, 2'048, "nan").has_value(), "NaN audio fails");
  std::vector<std::vector<float>> magnitudes{{1.0F, 0.5F}};
  std::vector<std::vector<float>> phases{{0.0F}};
  require(!nova::build_wavetable_from_spectra(magnitudes, phases, "mismatch").has_value(),
          "mismatched spectra fail");
  std::vector<std::vector<float>> too_long{std::vector<float>(1'025, 0.1F)};
  require(!nova::build_wavetable_from_spectra(too_long, too_long, "long").has_value(),
          "more than 1024 harmonics fail");
}

void builds_quickly() {
  for (const std::size_t frames : {nova::max_table_frames, std::size_t{64}}) {
    const auto mono = raw_saws(frames, 2'048);
    const auto start = std::chrono::steady_clock::now();
    const auto table = build(mono, 2'048, "timed");
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start);
    REQUIRE_NE(table->spectral(), nullptr, "the timed table has its spectral variant");
    if (elapsed.count() >= 1.5) {
      throw std::runtime_error("a " + std::to_string(frames) + "-frame table took " +
                               std::to_string(elapsed.count()) + " s to build");
    }
  }
}

bool snake_case(std::string_view name) {
  return !name.empty() && std::ranges::all_of(name, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
  });
}

void builtin_tables_are_sane() {
  const auto names = nova::builtin_table_names();
  const std::set<std::string_view> unique(names.begin(), names.end());
  REQUIRE_EQ(unique.size(), names.size(), "table names are unique");
  REQUIRE_GE(names.size(), 30, "a rich factory library");
  for (const std::string_view contract :
       {"basic_shapes", "glass_formant", "reese_saw", "vowel_morph", "fm_bell", "grit_fold",
        "spectral_pad", "analog_pulse"}) {
    require(unique.contains(contract), "the contract's table names exist");
  }
  REQUIRE_EQ(names.front(), "basic_shapes", "basic_shapes comes first");
  for (const auto name : names) {
    require(snake_case(name), "table names are snake_case ids");
    const auto table = nova::builtin_table(name);
    if (table == nullptr) {
      throw std::runtime_error("factory table " + std::string(name) + " does not build");
    }
    REQUIRE_EQ(table->name(), name, "a factory table keeps its name");
    REQUIRE_GE(table->frames(), 2, "a factory table morphs");
    REQUIRE_LE(table->frames(), nova::max_table_frames, "a factory table morphs");
    REQUIRE_NE(table->spectral(), nullptr, "a factory table has a spectral variant");
    check_shape(*table, name);
    check_band_limits(*table, name);
    for (std::size_t f = 0; f < table->frames(); ++f) {
      if (frame_rms(*table, 0, f) < 1e-3) {
        throw std::runtime_error("factory table " + std::string(name) + " has a silent frame");
      }
    }
    REQUIRE_EQ(nova::builtin_table(name), table, "a factory table is cached");
  }
  REQUIRE_EQ(nova::builtin_table("no_such_table"), nullptr, "an unknown table is null");
}

// The step from the last frame to the first is no bigger than the steps
// inside the loop: at most twice their 99.9th percentile.
void check_seamless(std::span<const oscillate::engine::StereoFrame> frames, std::string_view what) {
  for (const bool left : {true, false}) {
    const auto at = [&](std::size_t n) { return left ? frames[n].left : frames[n].right; };
    std::vector<float> steps(frames.size() - 1);
    for (std::size_t n = 1; n < frames.size(); ++n) {
      steps[n - 1] = std::fabs(at(n) - at(n - 1));
    }
    const auto rank = static_cast<std::ptrdiff_t>(static_cast<double>(steps.size()) * 0.999);
    std::nth_element(steps.begin(), steps.begin() + rank, steps.end());
    const float typical = steps[static_cast<std::size_t>(rank)];
    const float wrap = std::fabs(at(0) - at(frames.size() - 1));
    if (wrap > 2.0F * typical + 1e-6F) {
      throw std::runtime_error(std::string(what) + " has a seam at its loop point");
    }
  }
}

void builtin_noises_are_sane_and_seamless() {
  const auto names = nova::builtin_noise_names();
  const std::set<std::string_view> unique(names.begin(), names.end());
  REQUIRE_EQ(unique.size(), names.size(), "noise names are unique");
  for (const std::string_view contract :
       {"white", "pink", "brown", "analog_hiss", "vinyl", "breath"}) {
    require(unique.contains(contract), "the contract's noise names exist");
  }
  REQUIRE_EQ(names.front(), "white", "white comes first");
  for (const auto name : names) {
    require(snake_case(name), "noise names are snake_case ids");
    const auto noise = nova::builtin_noise(name);
    if (noise == nullptr) {
      throw std::runtime_error("factory noise " + std::string(name) + " does not build");
    }
    REQUIRE_EQ(noise->name, name, "a noise keeps its name");
    REQUIRE_EQ(noise->sample_rate, 48'000, "factory noises run at 48 kHz");
    REQUIRE_GE(noise->frames.size(), 2 * 48'000, "a noise loop lasts a few seconds");
    double energy = 0.0;
    double left = 0.0;
    double right = 0.0;
    for (const auto& frame : noise->frames) {
      REQUIRE_TRUE(std::isfinite(frame.left), "a noise is finite");
      REQUIRE_TRUE(std::isfinite(frame.right), "a noise is finite");
      REQUIRE_LE(std::fabs(frame.left), 1.0F, "a noise never clips");
      REQUIRE_LE(std::fabs(frame.right), 1.0F, "a noise never clips");
      energy += frame.left * frame.left + frame.right * frame.right;
      left += frame.left;
      right += frame.right;
    }
    const auto count = static_cast<double>(noise->frames.size());
    REQUIRE_GT(std::sqrt(energy / (2.0 * count)), 1e-3, "a noise is not silent");
    REQUIRE_LT(std::fabs(left / count), 0.01, "a noise has no DC");
    REQUIRE_LT(std::fabs(right / count), 0.01, "a noise has no DC");
    check_seamless(noise->frames, name);
    REQUIRE_EQ(nova::builtin_noise(name), noise, "a noise is cached");
  }
  REQUIRE_EQ(nova::builtin_noise("no_such_noise"), nullptr, "an unknown noise is null");
}

void user_noise_loops_seamlessly() {
  // A 440.7 Hz tone over one second ends mid-cycle: a hard seam unfaded.
  oscillate::native_dsp::synth::SynthMediaAsset asset;
  asset.source_rate = oscillate::test::sample_rate_44k;
  asset.frames.resize(44'100);
  for (std::size_t n = 0; n < asset.frames.size(); ++n) {
    const auto value =
        static_cast<float>(0.5 * std::sin(two_pi * 440.7 * static_cast<double>(n) / 44'100.0));
    asset.frames[n] = {value, -value};
  }
  REQUIRE_GT(std::fabs(asset.frames.back().left - asset.frames.front().left), 0.05F,
             "the test tone has a seam");
  auto result = nova::build_noise(asset, "tone");
  REQUIRE_OK(result, "a user noise builds");
  const auto& noise = *result.value();
  REQUIRE_EQ(noise.name, "tone", "a user noise keeps its identity");
  REQUIRE_EQ(noise.sample_rate, 44'100, "a user noise keeps its identity");
  REQUIRE_LT(noise.frames.size(), asset.frames.size(), "the fade shortens the loop a little");
  REQUIRE_GT(noise.frames.size(), asset.frames.size() * 9 / 10,
             "the fade shortens the loop a little");
  check_seamless(noise.frames, "user tone");

  oscillate::native_dsp::synth::SynthMediaAsset empty;
  empty.source_rate = oscillate::test::sample_rate_48k;
  require(!nova::build_noise(empty, "empty").has_value(), "an empty noise fails");
  auto no_rate = asset;
  no_rate.source_rate = 0;
  require(!nova::build_noise(no_rate, "rate").has_value(), "a noise with no rate fails");
}

} // namespace

int main() {
  try {
    sine_resamples_to_a_pure_sine();
    levels_are_band_limited();
    level_for_stays_under_the_band_edge();
    tables_normalise_wrap_and_have_no_dc();
    frames_thin_and_partial_frames_drop();
    spectral_variant_lands_on_the_source();
    spectral_morph_travels();
    spectra_follow_the_sine_phase_convention();
    invalid_input_fails();
    builds_quickly();
    builtin_tables_are_sane();
    builtin_noises_are_sane_and_seamless();
    user_noise_loops_seamlessly();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "nova tables contract passed\n";
  return 0;
}
