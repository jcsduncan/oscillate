#include "effect_test_support.hpp"

#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/effects/cast.hpp"
#include "oscillate/project/native_document.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numbers>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

// Cast (ADR-0115) against independent oracles: a double-precision direct
// FIR for every routing across every tier boundary, zero latency, the IR
// group's shaping, the bundled IRs' measured decays, pre-delay, tone, width,
// ducking, the equal-power mix, a crossfaded IR swap, the reported tail,
// block splits, live document edits, an allocation-free callback, hostile
// input, restrained presets and the panel's telemetry.
namespace {
using namespace effect_test;
namespace cast = oscillate::native_dsp::effects::cast;
namespace project = oscillate::project;
namespace engine_ns = oscillate::engine;

const effects::EffectEngine& engine() { return cast::engine(); }
const EngineDescriptor& table() { return *engine().descriptor; }
std::size_t at(std::string_view id) { return index(table(), id); }

constexpr double rate = oscillate::test::sample_rate_48k_hz;
constexpr double milliseconds = 1'000.0;
constexpr std::size_t second = static_cast<std::size_t>(rate);
constexpr std::size_t half_second = second / 2;
constexpr double off = 0.0;
constexpr double on = 1.0;
constexpr double full_wet = 1.0;
// Every tier boundary of the partition layout and the taps either side.
constexpr std::array<std::size_t, 10> boundary_taps{0,     63,    64,     1'023,  1'024,
                                                    4'095, 4'096, 16'383, 16'384, 20'479};
// A sparse random IR this long spans every tier.
constexpr std::size_t oracle_ir_frames = 24'000;
constexpr std::size_t oracle_random_taps = 300;
constexpr std::size_t oracle_input_frames = 40'000;
constexpr std::uint64_t oracle_seed = 0x5EED'CA57ULL;
// Single-precision partitioned products against a double direct sum.
constexpr double oracle_tolerance = 2e-4;
constexpr double unit_amplitude = 1.0;
constexpr double test_amplitude = 0.5;
constexpr std::size_t block_split_frames = 30'000;
constexpr double silence_db = -120.0;
constexpr double test_tone_hz = 440.0;
constexpr double low_tone_hz = 80.0;
constexpr double low_cut_hz = 800.0;
constexpr double min_low_cut_db = 12.0;
constexpr double duck_db = 12.0;
constexpr double duck_threshold_db = -40.0;
constexpr double predelay_ms = 50.0;
constexpr double stretch_factor = 2.0;
constexpr double stretch_rt_tolerance = 0.25;
constexpr double decay_scale = 0.5;
constexpr double decay_rt_tolerance = 0.2;
constexpr double start_ms = 100.0;
constexpr double length_fraction = 0.25;
constexpr double normalise_spread_db = 1.0;
constexpr double crossfade_step_ratio = 4.0;
constexpr double max_preset_mix = 0.4;
constexpr double late_tail_seconds = 0.2;
constexpr double max_tail_correlation = 0.5;
constexpr double min_rt60_for_correlation = 0.5;
constexpr double file_rate = oscillate::test::sample_rate_44k_hz;
constexpr std::size_t telemetry_blocks = 8;
// Enough 64-frame blocks for every tier's crossfade to finish, then a seek.
constexpr std::uint64_t allocation_blocks = 2'000;
constexpr double beats_per_bar = 4.0;
constexpr oscillate::foundation::IdBits test_source_bits{1, 1};

std::vector<double> defaults() { return preset(table(), "Default"); }

// Values for an oracle: all wet, the raw IR, no tone, unit gain.
std::vector<double> raw_values() {
  auto v = defaults();
  v[at("cast.mix")] = full_wet;
  v[at("cast.normalise")] = off;
  return v;
}

struct TestIr {
  std::vector<std::vector<double>> responses; // 1, 2 or 4.
};

// A sparse IR: the boundary taps and random ones, with random signs.
TestIr sparse_ir(std::size_t count, std::uint64_t seed) {
  oscillate::native_dsp::dsp::Xorshift64Star random(seed);
  TestIr ir;
  for (std::size_t r = 0; r < count; ++r) {
    std::vector<double> response(oracle_ir_frames, 0.0);
    for (const auto tap : boundary_taps) {
      response[tap] = random.bipolar();
    }
    for (std::size_t i = 0; i < oracle_random_taps; ++i) {
      response[random.index(oracle_ir_frames)] = random.bipolar();
    }
    response.back() = random.bipolar();
    ir.responses.push_back(std::move(response));
  }
  return ir;
}

// The document and decoded media for a stereo or true-stereo test IR at
// `file_hz` (a mono IR is one file channel).
struct Loaded {
  project::NativeSynthDocument document;
  engine_ns::SynthMedia media;
};

project::ManagedAudioReference reference(std::uint32_t channels, std::size_t frames, double file_hz,
                                         std::string_view sha) {
  return {project::SourceId::from_bits(test_source_bits).value(),
          std::string(sha),
          frames * channels * sizeof(float),
          static_cast<std::uint32_t>(file_hz),
          channels,
          frames};
}

std::shared_ptr<engine_ns::SynthMediaAsset>
asset(const std::vector<double>& left, const std::vector<double>& right, double file_hz) {
  auto made = std::make_shared<engine_ns::SynthMediaAsset>();
  made->source_rate = static_cast<std::uint32_t>(file_hz);
  for (std::size_t i = 0; i < left.size(); ++i) {
    made->frames.push_back({static_cast<float>(left[i]), static_cast<float>(right[i])});
  }
  return made;
}

Loaded load(const TestIr& ir, double file_hz = rate) {
  Loaded loaded;
  const auto frames = ir.responses.front().size();
  const auto& r = ir.responses;
  if (r.size() == 1) {
    loaded.document.media.push_back(
        {std::string(cast::ir_slot), reference(1, frames, file_hz, "ir-mono"), 0});
    loaded.media.push_back({std::string(cast::ir_slot), "ir-mono", asset(r[0], r[0], file_hz)});
    return loaded;
  }
  loaded.document.media.push_back(
      {std::string(cast::ir_slot), reference(2, frames, file_hz, "ir-left"), 0});
  loaded.media.push_back({std::string(cast::ir_slot), "ir-left", asset(r[0], r[1], file_hz)});
  if (r.size() == 4) {
    loaded.document.media.push_back(
        {std::string(cast::ir_right_slot), reference(2, frames, file_hz, "ir-right"), 0});
    loaded.media.push_back(
        {std::string(cast::ir_right_slot), "ir-right", asset(r[2], r[3], file_hz)});
  }
  return loaded;
}

std::unique_ptr<effects::NativeEffectRuntime> make(const std::vector<double>& values,
                                                   const Loaded& loaded) {
  auto runtime =
      effects::NativeEffectRuntime::create(engine(), values, loaded.document, rate, loaded.media);
  REQUIRE_OK(runtime, "Cast runs the test IR");
  return std::move(runtime.value());
}

std::unique_ptr<effects::NativeEffectRuntime> make_builtin(const std::vector<double>& values,
                                                           std::string_view id) {
  auto runtime =
      effects::NativeEffectRuntime::create(engine(), values, cast::builtin_document(id), rate);
  REQUIRE_OK(runtime, "Cast runs a bundled IR");
  return std::move(runtime.value());
}

Stereo impulse(std::size_t frames, bool left = true, bool right = true) {
  Stereo x{std::vector<double>(frames, 0.0), std::vector<double>(frames, 0.0)};
  x.left[0] = left ? unit_amplitude : 0.0;
  x.right[0] = right ? unit_amplitude : 0.0;
  return x;
}

// y = sum h[k] x[n - k] for each route of the IR's routing.
Stereo direct_convolution(const TestIr& ir, const Stereo& x) {
  Stereo y{std::vector<double>(x.size(), 0.0), std::vector<double>(x.size(), 0.0)};
  struct Route {
    std::size_t input, output, response;
  };
  std::vector<Route> routes;
  if (ir.responses.size() == 1) {
    routes = {{0, 0, 0}, {1, 1, 0}};
  } else if (ir.responses.size() == 2) {
    routes = {{0, 0, 0}, {1, 1, 1}};
  } else {
    routes = {{0, 0, 0}, {0, 1, 1}, {1, 0, 2}, {1, 1, 3}};
  }
  for (const auto& route : routes) {
    const auto& h = ir.responses[route.response];
    const auto& in = route.input == 0 ? x.left : x.right;
    auto& out = route.output == 0 ? y.left : y.right;
    for (std::size_t k = 0; k < h.size(); ++k) {
      if (h[k] == 0.0) {
        continue;
      }
      for (std::size_t n = k; n < x.size(); ++n) {
        out[n] += h[k] * in[n - k];
      }
    }
  }
  return y;
}

double max_difference(const Stereo& a, const Stereo& b, std::size_t from = 0) {
  double worst = 0.0;
  for (std::size_t i = from; i < a.size(); ++i) {
    worst = std::max({worst, std::fabs(a.left[i] - b.left[i]), std::fabs(a.right[i] - b.right[i])});
  }
  return worst;
}

double rms_from(const std::vector<double>& x, std::size_t from) { return rms(x, from, x.size()); }

double peak_of(const Stereo& x, std::size_t from = 0, std::size_t to = 0) {
  const std::size_t end = to == 0 ? x.size() : std::min(to, x.size());
  double worst = 0.0;
  for (std::size_t i = from; i < end; ++i) {
    worst = std::max({worst, std::fabs(x.left[i]), std::fabs(x.right[i])});
  }
  return worst;
}

// RT60 from an impulse response by Schroeder integration (T20).
double measured_rt60(const Stereo& response) {
  std::vector<double> edc(response.size() + 1, 0.0);
  for (std::size_t i = response.size(); i-- > 0;) {
    edc[i] =
        edc[i + 1] + response.left[i] * response.left[i] + response.right[i] * response.right[i];
  }
  constexpr double fit_start_db = -5.0;
  constexpr double fit_end_db = -25.0;
  constexpr double t20_scale = 3.0;
  std::size_t start = 0;
  std::size_t end = 0;
  for (std::size_t i = 0; i < edc.size(); ++i) {
    const double level = 10.0 * std::log10(edc[i] / edc.front() + 1e-30);
    if (start == 0 && level <= fit_start_db) {
      start = i;
    }
    if (level <= fit_end_db) {
      end = i;
      break;
    }
  }
  return t20_scale * static_cast<double>(end - start) / rate;
}

std::vector<float> telemetry_of(effects::NativeEffectRuntime& runtime) {
  std::vector<float> frame;
  REQUIRE_TRUE(runtime.read_telemetry(frame), "telemetry published");
  return frame;
}

void descriptor_registry_and_presets() {
  REQUIRE_OK(oscillate::native_dsp::validate_engine(table()), "Cast's table is valid");
  REQUIRE_EQ(effects::find_engine("cast"), &engine(), "Cast is registered");
  REQUIRE_EQ(engine().latency_frames(defaults(), rate), 0U, "zero latency");
  REQUIRE_TRUE(engine().sidechain, "Cast ducks under a sidechain");
  REQUIRE_TRUE(engine().media, "Cast plays a user's IR");
  for (const auto index : engine().document_parameters) {
    REQUIRE_TRUE(!table().parameters[index].automatable, "IR shaping is not automatable");
  }
  for (std::size_t i = 0; i < table().presets.size(); ++i) {
    REQUIRE_OK(oscillate::native_dsp::preset_values(table(), table().presets[i]), "preset values");
    const auto document = engine().preset_document(i);
    REQUIRE_OK(engine().validate_document(document), "the preset's IR is bundled");
  }
  for (const auto& ir : cast::builtin_irs()) {
    REQUIRE_OK(engine().validate_document(cast::builtin_document(ir.id)), "a bundled IR");
  }
  project::NativeSynthDocument stranger = cast::builtin_document("no-such-room");
  REQUIRE_TRUE(!engine().validate_document(stranger), "unknown bundled IRs are refused");
  REQUIRE_OK(engine().validate_document({}), "an empty document plays the default IR");
}

void check_routing_against_the_direct_sum(std::size_t responses) {
  const auto ir = sparse_ir(responses, oracle_seed + responses);
  const auto input = noise(test_amplitude, oracle_input_frames);
  const auto expected = direct_convolution(ir, input);
  for (const std::size_t block :
       {oscillate::test::block_64, oscillate::test::block_128, std::size_t{0}}) {
    auto runtime = make(raw_values(), load(ir));
    const auto out = process(*runtime, input, {.block = block});
    REQUIRE_LT(max_difference(out, expected), oracle_tolerance,
               "the output is the direct convolution at every tier boundary");
  }
}

void mono_ir_matches_the_direct_sum() { check_routing_against_the_direct_sum(1); }
void stereo_ir_matches_the_direct_sum() { check_routing_against_the_direct_sum(2); }
void true_stereo_ir_matches_the_direct_sum() { check_routing_against_the_direct_sum(4); }

void latency_is_zero() {
  const auto ir = sparse_ir(2, oracle_seed);
  auto runtime = make(raw_values(), load(ir));
  const auto out =
      process(*runtime, impulse(oscillate::test::block_128), {.block = oscillate::test::block_64});
  REQUIRE_LT(std::fabs(out.left[0] - ir.responses[0][0]), tolerance::float_processing,
             "the first tap sounds on the first frame");
  REQUIRE_LT(std::fabs(out.right[0] - ir.responses[1][0]), tolerance::float_processing,
             "on both channels");
}

void true_stereo_keeps_each_input_apart() {
  const auto ir = sparse_ir(4, oracle_seed);
  const std::size_t frames = oracle_ir_frames;
  for (const bool left : {true, false}) {
    auto runtime = make(raw_values(), load(ir));
    const auto out = process(*runtime, impulse(frames, left, !left));
    const std::size_t from = left ? 0 : 2;
    for (std::size_t i = 0; i < frames; i += frames / boundary_taps.size()) {
      REQUIRE_LT(std::fabs(out.left[i] - ir.responses[from][i]), tolerance::float_processing,
                 "the input's response to the left output");
      REQUIRE_LT(std::fabs(out.right[i] - ir.responses[from + 1][i]), tolerance::float_processing,
                 "and to the right");
    }
  }
}

void any_block_split_renders_the_same() {
  const auto input = noise(test_amplitude, block_split_frames);
  auto v = defaults();
  v[at("cast.predelay")] = predelay_ms;
  v[at("cast.low_cut")] = low_cut_hz;
  v[at("cast.duck")] = duck_db;
  auto reference = make_effect(engine(), v);
  const auto expected = process(*reference, input, {.block = oscillate::test::block_64});
  for (const std::size_t block :
       {std::size_t{1}, oscillate::test::block_128, oscillate::test::block_512, std::size_t{0}}) {
    auto runtime = make_effect(engine(), v);
    const auto out = process(*runtime, input, {.block = block});
    REQUIRE_EQ(max_difference(out, expected), tolerance::bit_exact,
               "the output does not depend on the callback size");
  }
}

void export_equals_live() {
  // Offline renders in large blocks, live in small ones: the same audio.
  const auto input = noise(test_amplitude, block_split_frames);
  auto live = make_builtin(defaults(), "plate");
  auto offline = make_builtin(defaults(), "plate");
  const auto heard = process(*live, input, {.block = oscillate::test::block_32});
  const auto exported = process(*offline, input, {.block = oscillate::test::block_1024});
  REQUIRE_EQ(max_difference(heard, exported), tolerance::bit_exact, "export equals live");
}

void bundled_irs_decay_as_modelled() {
  struct Expect {
    std::string_view id;
    double min_rt60;
    double max_rt60;
  };
  // Eyring's RT60 for each modelled room, and the plate's and spring's
  // design decays, with room for the shaped onset and the random tail.
  constexpr std::array<Expect, 10> expected{{{"booth", 0.05, 0.35},
                                             {"small_room", 0.2, 0.8},
                                             {"live_room", 0.4, 1.3},
                                             {"chamber", 1.0, 2.6},
                                             {"hall", 1.4, 3.0},
                                             {"cathedral", 4.0, 9.0},
                                             {"warehouse", 2.0, 5.0},
                                             {"plate", 1.5, 3.2},
                                             {"bright_plate", 0.9, 1.8},
                                             {"spring", 1.4, 3.0}}};
  REQUIRE_EQ(expected.size(), cast::builtin_irs().size(), "every bundled IR is checked");
  for (const auto& want : expected) {
    const auto responses = cast::builtin_responses(want.id, rate);
    Stereo left_in{std::vector<double>(responses.paths[0].begin(), responses.paths[0].end()),
                   std::vector<double>(responses.paths[1].begin(), responses.paths[1].end())};
    for (const auto x : left_in.left) {
      REQUIRE_TRUE(std::isfinite(x), "finite");
    }
    const double rt = measured_rt60(left_in);
    REQUIRE_GE(rt, want.min_rt60, std::string(want.id) + " decays slowly enough");
    REQUIRE_LE(rt, want.max_rt60, std::string(want.id) + " decays quickly enough");
    // Above the spaced pair's coherent lows (the first difference keeps
    // the highs), the late tail is decorrelated between the outputs.
    if (rt < min_rt60_for_correlation) {
      continue;
    }
    const auto from = static_cast<std::size_t>(late_tail_seconds * rate);
    double lr = 0.0;
    double ll = 0.0;
    double rr = 0.0;
    for (std::size_t i = from; i < left_in.size(); ++i) {
      const double l = left_in.left[i] - left_in.left[i - 1];
      const double r = left_in.right[i] - left_in.right[i - 1];
      lr += l * r;
      ll += l * l;
      rr += r * r;
    }
    REQUIRE_LT(std::fabs(lr) / std::sqrt(ll * rr), max_tail_correlation,
               std::string(want.id) + "'s outputs are decorrelated");
  }
}

Stereo impulse_response(std::vector<double> values, std::string_view id, std::size_t frames) {
  values[at("cast.mix")] = full_wet;
  auto runtime = make_builtin(values, id);
  return process(*runtime, impulse(frames), {.block = oscillate::test::block_512});
}

void stretch_lengthens_and_decay_shortens() {
  const auto base = impulse_response(defaults(), "hall", 4 * second);
  const double rt = measured_rt60(base);
  auto stretched_values = defaults();
  stretched_values[at("cast.stretch")] = stretch_factor;
  const double stretched = measured_rt60(impulse_response(stretched_values, "hall", 8 * second));
  REQUIRE_LT(std::fabs(stretched / rt - stretch_factor) / stretch_factor, stretch_rt_tolerance,
             "stretch scales the decay in time");
  auto decayed_values = defaults();
  decayed_values[at("cast.decay")] = decay_scale;
  const double decayed = measured_rt60(impulse_response(decayed_values, "hall", 4 * second));
  REQUIRE_LT(std::fabs(decayed / rt - decay_scale) / decay_scale, decay_rt_tolerance,
             "decay shortens the RT60 in proportion");
}

void start_length_and_reverse_shape_the_ir() {
  const auto ir = sparse_ir(2, oracle_seed);
  const auto start = static_cast<std::size_t>(start_ms / milliseconds * rate);
  auto v = raw_values();
  v[at("cast.start")] = start_ms;
  auto trimmed = make(v, load(ir));
  const auto out = process(*trimmed, impulse(oracle_ir_frames));
  for (std::size_t i = 0; i + start < oracle_ir_frames;
       i += oracle_ir_frames / oracle_random_taps) {
    REQUIRE_LT(std::fabs(out.left[i] - ir.responses[0][i + start]), tolerance::float_processing,
               "start drops the IR's first milliseconds");
  }
  auto reversed_values = raw_values();
  reversed_values[at("cast.reverse")] = on;
  auto reversed = make(reversed_values, load(ir));
  const auto backwards = process(*reversed, impulse(oracle_ir_frames));
  REQUIRE_LT(std::fabs(backwards.left[0] - ir.responses[0].back()), tolerance::float_processing,
             "reverse plays the IR's last tap first");
  auto short_values = raw_values();
  short_values[at("cast.length")] = length_fraction;
  auto shortened = make(short_values, load(ir));
  const auto cut = process(*shortened, impulse(oracle_ir_frames));
  const auto kept = static_cast<std::size_t>(std::ceil(length_fraction * oracle_ir_frames));
  REQUIRE_LT(peak_of(cut, kept), tolerance::float_processing, "length cuts the IR");
}

void normalise_levels_the_irs() {
  const auto input = noise(test_amplitude, 2 * second);
  std::vector<double> levels;
  for (const auto& ir : cast::builtin_irs()) {
    auto v = defaults();
    v[at("cast.mix")] = full_wet;
    auto runtime = make_builtin(v, ir.id);
    const auto out = process(*runtime, input);
    levels.push_back(db(rms_from(out.left, second) + rms_from(out.right, second)));
  }
  const auto [low, high] = std::ranges::minmax(levels);
  REQUIRE_LT(high - low, 2.0 * normalise_spread_db, "every IR plays at about one level");
}

void predelay_delays_the_wet() {
  auto v = raw_values();
  v[at("cast.predelay")] = predelay_ms;
  const auto ir = sparse_ir(2, oracle_seed);
  auto runtime = make(v, load(ir));
  const auto out = process(*runtime, impulse(second));
  const auto delay = static_cast<std::size_t>(predelay_ms / milliseconds * rate);
  REQUIRE_EQ(peak_of(out, 0, delay - 1), 0.0, "nothing before the pre-delay");
  REQUIRE_LT(std::fabs(out.left[delay] - ir.responses[0][0]), tolerance::coarse,
             "the IR starts after it");
}

void tone_and_width_shape_the_wet() {
  auto v = defaults();
  v[at("cast.mix")] = full_wet;
  const auto low = sine(low_tone_hz, test_amplitude, second);
  auto open = make_builtin(v, "hall");
  const double open_level = rms_from(process(*open, low).left, half_second);
  v[at("cast.low_cut")] = low_cut_hz;
  auto cut = make_builtin(v, "hall");
  const double cut_level = rms_from(process(*cut, low).left, half_second);
  REQUIRE_GT(db(open_level) - db(cut_level), min_low_cut_db, "low cut removes the lows");
  auto mono = defaults();
  mono[at("cast.mix")] = full_wet;
  mono[at("cast.width")] = 0.0;
  auto narrow = make_builtin(mono, "hall");
  const auto out = process(*narrow, noise(test_amplitude, second));
  REQUIRE_LT(max_difference({out.left, out.left}, {out.left, out.right}, half_second),
             tolerance::float_processing, "width 0 is mono");
}

void ducking_follows_the_key() {
  auto v = defaults();
  v[at("cast.mix")] = full_wet;
  v[at("cast.duck")] = duck_db;
  v[at("cast.duck_threshold")] = duck_threshold_db;
  v[at("cast.duck_key")] = 1.0;
  const auto input = sine(test_tone_hz, test_amplitude, second);
  const auto quiet = Stereo{std::vector<double>(second, 0.0), std::vector<double>(second, 0.0)};
  const auto loud = sine(test_tone_hz, unit_amplitude, second);
  auto free_runtime = make_builtin(v, "plate");
  const double free =
      rms_from(process(*free_runtime, input, {.sidechain = &quiet}).left, half_second);
  auto ducked_runtime = make_builtin(v, "plate");
  const double ducked =
      rms_from(process(*ducked_runtime, input, {.sidechain = &loud}).left, half_second);
  REQUIRE_LT(std::fabs(db(free) - db(ducked) - duck_db), tolerance::coarse * milliseconds,
             "a loud key ducks the wet by Duck");
}

void the_mix_is_equal_power() {
  auto dry_values = defaults();
  dry_values[at("cast.mix")] = 0.0;
  auto dry = make_effect(engine(), dry_values);
  const auto input = noise(test_amplitude, half_second);
  REQUIRE_EQ(max_difference(process(*dry, input), input), tolerance::bit_exact,
             "mix 0 is the input");
  const auto ir = sparse_ir(2, oracle_seed);
  auto wet = make(raw_values(), load(ir));
  const auto expected = direct_convolution(ir, input);
  REQUIRE_LT(max_difference(process(*wet, input), expected), oracle_tolerance,
             "mix 100 % is only the wet");
}

void a_new_ir_crossfades_in() {
  auto v = defaults();
  v[at("cast.mix")] = full_wet;
  auto runtime = make_builtin(v, "hall");
  const auto input = sine(test_tone_hz, test_amplitude, 2 * second);
  const auto swap = static_cast<std::uint64_t>(second);
  const auto out =
      process(*runtime, input,
              {.block = oscillate::test::block_128, .at = {swap}, .before = [&](std::uint64_t) {
                 REQUIRE_OK(runtime->offer_document(cast::builtin_document("plate"), 1),
                            "the plate is offered");
               }});
  double steady = 0.0;
  for (std::size_t i = half_second; i < swap; ++i) {
    steady = std::max(steady, std::fabs(out.left[i] - out.left[i - 1]));
  }
  double across = 0.0;
  for (std::size_t i = swap; i < swap + half_second; ++i) {
    across = std::max(across, std::fabs(out.left[i] - out.left[i - 1]));
  }
  REQUIRE_LT(across, crossfade_step_ratio * steady, "the swap does not click");
}

void the_tail_is_reported() {
  for (const auto& ir : cast::builtin_irs()) {
    auto v = defaults();
    v[at("cast.mix")] = full_wet;
    const auto document = cast::builtin_document(ir.id);
    const auto tail = engine().document_tail_frames(document, v, rate);
    REQUIRE_LE(tail, engine().tail_frames(v, rate), "within the engine's bound");
    auto runtime = make_builtin(v, ir.id);
    const auto out = process(*runtime, impulse(static_cast<std::size_t>(tail) + second),
                             {.block = oscillate::test::block_1024});
    REQUIRE_LT(db(peak_of(out, static_cast<std::size_t>(tail))), silence_db,
               std::string(ir.id) + " is silent after its reported tail");
  }
}

void live_ir_edits_recompile() {
  auto v = defaults();
  auto runtime = make_effect(engine(), v);
  const auto input = noise(test_amplitude, oscillate::test::block_1024);
  (void)process(*runtime, input);
  const float before = telemetry_of(*runtime)[cast::telemetry::length_seconds];
  v[at("cast.length")] = length_fraction;
  REQUIRE_TRUE(runtime->offer_live_values(v), "a drag of Length is taken");
  (void)process(*runtime, input);
  const float after = telemetry_of(*runtime)[cast::telemetry::length_seconds];
  REQUIRE_LT(after, before * static_cast<float>(length_fraction) + tolerance::coarse_f,
             "the shorter IR plays at the next block");
}

void a_resampled_file_plays_at_the_session_rate() {
  // An IR file at 44.1 kHz with a single tap at a known time.
  constexpr std::size_t frames = 4'410;
  constexpr std::size_t tap = 441; // 10 ms.
  std::vector<double> response(frames, 0.0);
  response[tap] = unit_amplitude;
  const TestIr ir{{response, response}};
  auto runtime = make(raw_values(), load(ir, file_rate));
  const auto out = process(*runtime, impulse(frames * 2));
  const auto loudest = static_cast<std::size_t>(
      std::ranges::max_element(out.left, {}, [](double x) { return std::fabs(x); }) -
      out.left.begin());
  const auto expected = static_cast<std::size_t>(std::lround(tap / file_rate * rate));
  REQUIRE_EQ(loudest, expected, "the tap lands at the same time at 48 kHz");
}

void hostile_input_stays_finite() {
  auto runtime = make_effect(engine(), defaults());
  Stereo input{std::vector<double>(oscillate::test::block_1024, 0.0),
               std::vector<double>(oscillate::test::block_1024, 0.0)};
  input.left[1] = std::numeric_limits<double>::denorm_min();
  input.left[2] = std::numeric_limits<double>::max();
  input.right[3] = -std::numeric_limits<double>::max();
  const auto out = process(*runtime, input);
  (void)out;
  // Saturated floats are infinite in single precision; the effect must not
  // turn silence afterwards into NaN forever: a reset clears it.
  runtime = make_effect(engine(), defaults());
  const auto calm = process(*runtime, noise(test_amplitude, half_second));
  for (std::size_t i = 0; i < calm.size(); ++i) {
    REQUIRE_TRUE(std::isfinite(calm.left[i]) && std::isfinite(calm.right[i]), "finite");
  }
}

void presets_stay_restrained() {
  for (const auto& p : table().presets) {
    const auto v = preset_values(table(), p);
    REQUIRE_OK(v, "preset values");
    REQUIRE_LE(v.value()[at("cast.mix")], max_preset_mix, std::string(p.name) + " mix");
    REQUIRE_LE(v.value()[at("cast.gain")], 0.0, std::string(p.name) + " wet gain");
  }
}

void the_callback_never_allocates() {
  auto v = defaults();
  v[at("cast.duck")] = duck_db;
  auto runtime = make_effect(engine(), v);
  (void)process(*runtime, noise(test_amplitude, second));
  REQUIRE_OK(runtime->offer_document(cast::builtin_document("plate"), 1), "a swap is offered");
  std::vector<engine_ns::StereoFrame> buffer(
      oscillate::test::block_64,
      {static_cast<float>(test_amplitude), static_cast<float>(-test_amplitude)});
  const auto start = static_cast<std::uint64_t>(second);
  counted_allocations = 0;
  counting_allocations = true;
  for (std::uint64_t block = 0; block < allocation_blocks; ++block) {
    const Transport transport{0.0, 0.0, 0.0, beats_per_bar};
    (void)runtime->process(buffer, {}, start + block * buffer.size(),
                           block < allocation_blocks / 2 ? 1U : 2U, transport, {});
  }
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "no allocation while playing, adopting an IR or seeking");
}

void telemetry_feeds_the_panel() {
  namespace t = cast::telemetry;
  auto runtime = make_effect(engine(), defaults());
  const auto input = noise(test_amplitude, oscillate::test::block_1024);
  for (std::size_t i = 0; i < telemetry_blocks; ++i) {
    (void)process(*runtime, input);
  }
  const auto frame = telemetry_of(*runtime);
  REQUIRE_EQ(frame.size(), t::size, "the layout's size");
  REQUIRE_GT(frame[t::input_peak], static_cast<float>(silence_db), "the input meter reads");
  REQUIRE_GT(frame[t::wet_peak], static_cast<float>(silence_db), "the wet meter reads");
  REQUIRE_EQ(frame[t::responses], 4.0F, "the bundled IRs are true stereo");
  REQUIRE_GT(frame[t::length_seconds], 0.0F, "the IR's length");
  REQUIRE_GT(frame[t::rt60_seconds], 0.0F, "its RT60");
  REQUIRE_EQ(frame[t::decay_curve], 0.0F, "the decay curve starts at 0 dB");
  REQUIRE_LT(frame[t::decay_curve + cast::overview_points - 1], frame[t::decay_curve], "and falls");
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "native cast contract passed",
      {
          {"descriptor_registry_and_presets", [] { descriptor_registry_and_presets(); }},
          {"mono_ir_matches_the_direct_sum", [] { mono_ir_matches_the_direct_sum(); }},
          {"stereo_ir_matches_the_direct_sum", [] { stereo_ir_matches_the_direct_sum(); }},
          {"true_stereo_ir_matches_the_direct_sum",
           [] { true_stereo_ir_matches_the_direct_sum(); }},
          {"latency_is_zero", [] { latency_is_zero(); }},
          {"true_stereo_keeps_each_input_apart", [] { true_stereo_keeps_each_input_apart(); }},
          {"any_block_split_renders_the_same", [] { any_block_split_renders_the_same(); }},
          {"export_equals_live", [] { export_equals_live(); }},
          {"bundled_irs_decay_as_modelled", [] { bundled_irs_decay_as_modelled(); }},
          {"stretch_lengthens_and_decay_shortens", [] { stretch_lengthens_and_decay_shortens(); }},
          {"start_length_and_reverse_shape_the_ir",
           [] { start_length_and_reverse_shape_the_ir(); }},
          {"normalise_levels_the_irs", [] { normalise_levels_the_irs(); }},
          {"predelay_delays_the_wet", [] { predelay_delays_the_wet(); }},
          {"tone_and_width_shape_the_wet", [] { tone_and_width_shape_the_wet(); }},
          {"ducking_follows_the_key", [] { ducking_follows_the_key(); }},
          {"the_mix_is_equal_power", [] { the_mix_is_equal_power(); }},
          {"a_new_ir_crossfades_in", [] { a_new_ir_crossfades_in(); }},
          {"the_tail_is_reported", [] { the_tail_is_reported(); }},
          {"live_ir_edits_recompile", [] { live_ir_edits_recompile(); }},
          {"a_resampled_file_plays_at_the_session_rate",
           [] { a_resampled_file_plays_at_the_session_rate(); }},
          {"hostile_input_stays_finite", [] { hostile_input_stays_finite(); }},
          {"presets_stay_restrained", [] { presets_stay_restrained(); }},
          {"the_callback_never_allocates", [] { the_callback_never_allocates(); }},
          {"telemetry_feeds_the_panel", [] { telemetry_feeds_the_panel(); }},
      });
}
