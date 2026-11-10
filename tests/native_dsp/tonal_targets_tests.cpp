#include "dsp/bs1770.hpp"
#include "oscillate/native_dsp/loudness.hpp"
#include "oscillate/native_dsp/tonal_analysis.hpp"
#include "oscillate/native_dsp/tonal_targets.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/signal.hpp"
#include "oscillate/test/tolerances.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Tonal targets (ADR-0120): the grid tiles without gaps; the K-weighting
// is BS.1770's (against the loudness analyser); normalised curves ignore
// gain; the band meter reads sines and noise true and allocates nothing
// once built; targets rebuild the same from the same references in any
// order and survive their file format; malformed files are refused; the
// factory targets are well formed; a curve's placement against a range.
namespace {

namespace tonal = oscillate::native_dsp::tonal;
namespace dsp = oscillate::native_dsp::dsp;
using oscillate::test::require;
using oscillate::test::sample_rate_48k;
using oscillate::test::sample_rate_48k_hz;

constexpr double edge_tolerance = 1e-9; // Relative, between computed edges.
// A sine's power, read by the meter, within this of its mean square.
constexpr double sine_power_tolerance_db = 0.05;
// White noise per band against its bandwidth, once averaged.
constexpr double noise_flatness_db = 0.5;
constexpr double noise_from_hz = 200.0;
// K-weighting against the loudness analyser's own filter.
constexpr double k_tolerance_db = 0.02;
constexpr std::array k_check_hz{100.0, 997.0, 4'000.0};
constexpr double analyser_seconds = 2.0;
// A sine at a level well inside every range.
constexpr double test_amplitude = 0.5;
constexpr double seconds_of_noise = 20.0;
constexpr double seconds_of_sine = 4.0;
constexpr std::uint64_t noise_seed = 7;
constexpr double gain_db = 12.0;
// Written levels have two decimals.
constexpr double written_resolution_db = 0.005;
// The fewest references a factory target is built from.
constexpr std::size_t factory_min_references = 8;

double mean_square(double amplitude) { return 0.5 * amplitude * amplitude; }

void grid_tiles_without_gaps() {
  for (std::size_t b = 0; b < tonal::bands; ++b) {
    const double centre =
        tonal::pivot_hz *
        std::exp2((tonal::first_band_step + static_cast<int>(b)) / tonal::bands_per_octave);
    require(std::abs(tonal::band_hz(b) / centre - 1.0) < edge_tolerance,
            "band " + std::to_string(b) + " sits on the base-2 third-octave centre");
    if (b > 0) {
      require(std::abs(tonal::band_low_hz(b) / tonal::band_high_hz(b - 1) - 1.0) < edge_tolerance,
              "bands meet");
    }
    require(std::abs(tonal::fine_low_hz(b * tonal::fine_per_band) / tonal::band_low_hz(b) - 1.0) <
                    edge_tolerance &&
                std::abs(tonal::fine_high_hz((b + 1) * tonal::fine_per_band - 1) /
                             tonal::band_high_hz(b) -
                         1.0) < edge_tolerance,
            "four fine bands tile each band");
  }
  for (std::size_t i = 1; i < tonal::fine_bands; ++i) {
    require(std::abs(tonal::fine_low_hz(i) / tonal::fine_high_hz(i - 1) - 1.0) < edge_tolerance,
            "fine bands meet");
  }
}

// The analyser's loudness of a mono sine, less its mean square and the
// BS.1770 offset, is the K-weighting's gain at that frequency.
void k_weighting_is_bs1770() {
  for (const double hz : k_check_hz) {
    oscillate::native_dsp::IntegratedLoudnessAnalyzer analyser(
        sample_rate_48k, oscillate::native_dsp::LoudnessLayout::mono);
    const auto frames = static_cast<std::size_t>(analyser_seconds * sample_rate_48k_hz);
    std::vector<float> samples(frames);
    for (std::size_t i = 0; i < frames; ++i) {
      samples[i] = static_cast<float>(
          test_amplitude *
          std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / sample_rate_48k_hz));
    }
    require(analyser.append(samples) == oscillate::native_dsp::LoudnessStatus::ok, "analysed");
    const auto result = analyser.finish();
    require(result.integrated_lkfs.has_value(), "a loudness");
    const double measured = result.integrated_lkfs.value_or(0.0) - dsp::bs1770::offset_lkfs -
                            10.0 * std::log10(mean_square(test_amplitude));
    require(std::abs(measured - tonal::k_weighting_db(hz)) < k_tolerance_db,
            "K-weighting at " + std::to_string(hz) + " Hz: " + std::to_string(measured) + " vs " +
                std::to_string(tonal::k_weighting_db(hz)));
  }
}

void normalised_curves_ignore_gain() {
  tonal::Curve curve{};
  for (std::size_t b = 0; b < tonal::bands; ++b) {
    curve[b] = -static_cast<double>(b);
  }
  auto louder = curve;
  for (auto& level : louder) {
    level += gain_db;
  }
  const auto a = tonal::normalised(curve);
  const auto b = tonal::normalised(louder);
  for (std::size_t i = 0; i < tonal::bands; ++i) {
    require(std::abs(a[i] - b[i]) < oscillate::test::tolerance::double_accumulation,
            "gain does not move a normalised curve");
  }
  require(std::abs(tonal::loudness_db(a)) < oscillate::test::tolerance::double_accumulation,
          "a normalised curve's loudness is 0 dB");
}

void band_meter_reads_sines_and_noise() {
  // A sine: all its power in the meter, most in its own band.
  for (const std::size_t fine : {std::size_t{20}, std::size_t{60}, std::size_t{100}}) {
    tonal::BandMeter meter(sample_rate_48k_hz);
    const double hz = tonal::fine_hz(fine);
    const auto frames = static_cast<std::size_t>(seconds_of_sine * sample_rate_48k_hz);
    for (std::size_t i = 0; i < frames; ++i) {
      meter.push(test_amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) /
                                           sample_rate_48k_hz),
                 0.0);
    }
    double total = 0.0;
    std::size_t loudest = 0;
    for (std::size_t i = 0; i < tonal::fine_bands; ++i) {
      total += meter.mean_a()[i];
      loudest = meter.mean_a()[i] > meter.mean_a()[loudest] ? i : loudest;
      require(meter.mean_b()[i] <=
                  oscillate::test::tolerance::double_rounding * mean_square(test_amplitude),
              "the silent second signal reads nothing but rounding");
    }
    require(std::abs(dsp::power_to_db(total) - dsp::power_to_db(mean_square(test_amplitude))) <
                sine_power_tolerance_db,
            "a sine's power sums to its mean square");
    require(loudest == fine, "a sine is loudest in its own band");
  }
  // White noise: each band's power is its share of the spectrum.
  tonal::BandMeter meter(sample_rate_48k_hz);
  const auto frames = static_cast<std::size_t>(seconds_of_noise * sample_rate_48k_hz);
  const auto noise = oscillate::test::white_noise(frames, noise_seed, test_amplitude);
  for (const auto sample : noise) {
    meter.push(sample, sample);
  }
  const double density = mean_square(test_amplitude) * 2.0 / 3.0 / (sample_rate_48k_hz / 2.0);
  for (std::size_t i = 0; i < tonal::fine_bands; ++i) {
    if (tonal::fine_low_hz(i) < noise_from_hz) {
      continue;
    }
    const double expected = density * (tonal::fine_high_hz(i) - tonal::fine_low_hz(i));
    require(std::abs(dsp::power_to_db(meter.mean_a()[i]) - dsp::power_to_db(expected)) <
                noise_flatness_db,
            "white noise reads its bandwidth in fine band " + std::to_string(i));
    require(std::abs(meter.mean_a()[i] / meter.mean_b()[i] - 1.0) <
                oscillate::test::tolerance::double_accumulation,
            "both signals read alike");
  }
}

void band_meter_allocates_nothing() {
  tonal::BandMeter meter(sample_rate_48k_hz);
  const auto frames = 4 * meter.window_frames();
  oscillate::test::counted_allocations = 0;
  oscillate::test::counting_allocations = true;
  for (std::size_t i = 0; i < frames; ++i) {
    meter.push(std::sin(static_cast<double>(i)), 0.0);
  }
  meter.reset();
  oscillate::test::counting_allocations = false;
  REQUIRE_EQ(oscillate::test::counted_allocations, 0, "push and reset never allocate");
  REQUIRE_EQ(meter.windows(), 0, "reset clears the mean");
}

std::vector<tonal::Curve> some_curves() {
  std::vector<tonal::Curve> curves;
  constexpr std::size_t count = 9;
  constexpr double slope_db = 1.5;
  constexpr double spread_db = 0.37;
  for (std::size_t c = 0; c < count; ++c) {
    tonal::Curve curve{};
    for (std::size_t b = 0; b < tonal::bands; ++b) {
      curve[b] = -slope_db * static_cast<double>(b) / tonal::bands_per_octave +
                 spread_db * static_cast<double>((c * 7 + b * 3) % count);
    }
    curves.push_back(tonal::normalised(curve));
  }
  return curves;
}

void targets_rebuild_deterministically() {
  auto curves = some_curves();
  const auto first = tonal::build_target("t", "Test", curves);
  REQUIRE_OK(first, "a target");
  std::ranges::reverse(curves);
  std::ranges::rotate(curves, curves.begin() + 2);
  const auto again = tonal::build_target("t", "Test", curves);
  REQUIRE_OK(again, "a target");
  const auto text = tonal::serialise_target(first.value());
  require(text == tonal::serialise_target(again.value()),
          "the same references in any order give the same file");
  // The median and quartiles by hand for one band.
  std::vector<double> column;
  column.reserve(curves.size());
  for (const auto& c : curves) {
    column.push_back(c[0]);
  }
  std::ranges::sort(column);
  require(first.value().median[0] == column[column.size() / 2], "the median of nine");
  require(first.value().low[0] == column[2] && first.value().high[0] == column[6],
          "the quartiles of nine are the third and seventh");
  const auto parsed = tonal::parse_target(text, "user/test");
  REQUIRE_OK(parsed, "the file reads back");
  const auto& t = parsed.value();
  require(t.id == "user/test" && t.name == "Test" && t.references == curves.size(),
          "name, id and references survive");
  for (std::size_t b = 0; b < tonal::bands; ++b) {
    require(std::abs(t.median[b] - first.value().median[b]) <= written_resolution_db &&
                std::abs(t.low[b] - first.value().low[b]) <= written_resolution_db &&
                std::abs(t.high[b] - first.value().high[b]) <= written_resolution_db,
            "levels survive to a hundredth of a dB");
  }
  REQUIRE_FALSE(tonal::build_target("t", "Empty", {}).has_value(), "no references, no target");
}

std::string replaced(std::string text, std::string_view from, std::string_view to) {
  const auto at = text.find(from);
  require(at != std::string::npos, "fixture holds " + std::string(from));
  text.replace(at, from.size(), to);
  return text;
}

void target_files_are_validated() {
  const auto target = tonal::build_target("t", "Test", some_curves());
  REQUIRE_OK(target, "a target");
  const auto good = tonal::serialise_target(target.value());
  REQUIRE_OK(tonal::parse_target(good, "x"), "the file itself is valid");
  const auto first_level = [&](std::string_view key) {
    const auto at = good.find(std::string(key) + "\": [") + key.size() + 4;
    return good.substr(at, good.find(',', at) - at);
  };
  const std::string long_name(tonal::max_name_bytes + 1, 'n');
  const std::vector<std::pair<std::string, std::string>> bad{
      {"not JSON", "{\"format\":"},
      {"not an object", "[1, 2]"},
      {"another format", replaced(good, "oscillate-tonal-target", "something-else")},
      {"a later version", replaced(good, "\"version\": 1", "\"version\": 2")},
      {"an empty name", replaced(good, "\"name\": \"Test\"", "\"name\": \"\"")},
      {"a long name", replaced(good, "\"name\": \"Test\"", "\"name\": \"" + long_name + "\"")},
      {"no references", replaced(good, "\"references\": 9", "\"references\": 0")},
      {"off the grid", replaced(good, "24.80", "25.00")},
      {"a band short",
       replaced(good, "\"median_db\": [" + first_level("median_db") + ", ", "\"median_db\": [")},
      {"a string level",
       replaced(good, "\"low_db\": [" + first_level("low_db"), "\"low_db\": [\"NaN\"")},
      {"a level out of range",
       replaced(good, "\"high_db\": [" + first_level("high_db"), "\"high_db\": [41.00")},
      {"a range that misses its median",
       replaced(good, "\"low_db\": [" + first_level("low_db"), "\"low_db\": [39.00")},
      {"too large", good + std::string(tonal::max_file_bytes, ' ')},
  };
  for (const auto& [why, text] : bad) {
    REQUIRE_FALSE(tonal::parse_target(text, "x").has_value(), "refused: " + why);
  }
}

void factory_targets_are_well_formed() {
  std::set<std::string> ids;
  require(!tonal::factory_targets().empty(), "there are factory targets");
  for (const auto& target : tonal::factory_targets()) {
    require(ids.insert(target.id).second, "ids are unique: " + target.id);
    require(tonal::find_factory_target(target.id) == &target, "found by id");
    require(target.references >= factory_min_references,
            target.id + " is measured from enough references");
    for (std::size_t b = 0; b < tonal::bands; ++b) {
      require(target.low[b] <= target.median[b] && target.median[b] <= target.high[b],
              target.id + ": each range holds its median");
    }
    const auto text = tonal::serialise_target(target);
    REQUIRE_OK(tonal::parse_target(text, target.id), target.id + " survives its file format");
  }
  require(tonal::find_factory_target("no such target") == nullptr, "unknown ids find nothing");
}

void placement_against_a_range() {
  tonal::Target target;
  target.low.fill(-1.0);
  target.median.fill(0.0);
  target.high.fill(1.0);
  tonal::Curve curve{};
  curve.fill(0.0);
  curve[0] = -1.0; // On the edge: inside.
  curve[1] = 1.0;
  curve[2] = -1.0 - std::numeric_limits<double>::epsilon() * 4;
  curve[3] = 1.5;
  const auto placed = tonal::assess(curve, target);
  require(placed[0] == tonal::Placement::inside && placed[1] == tonal::Placement::inside,
          "the range's ends are inside");
  require(placed[2] == tonal::Placement::below, "under the range is below");
  require(placed[3] == tonal::Placement::above, "over the range is above");
  require(placed[4] == tonal::Placement::inside, "the median is inside");
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "tonal targets contract passed",
      {{"grid_tiles_without_gaps", grid_tiles_without_gaps},
       {"k_weighting_is_bs1770", k_weighting_is_bs1770},
       {"normalised_curves_ignore_gain", normalised_curves_ignore_gain},
       {"band_meter_reads_sines_and_noise", band_meter_reads_sines_and_noise},
       {"band_meter_allocates_nothing", band_meter_allocates_nothing},
       {"targets_rebuild_deterministically", targets_rebuild_deterministically},
       {"target_files_are_validated", target_files_are_validated},
       {"factory_targets_are_well_formed", factory_targets_are_well_formed},
       {"placement_against_a_range", placement_against_a_range}});
}
