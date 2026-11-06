#include "effect_test_support.hpp"

#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/effects/dust.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <numbers>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

// Dust (ADR-0069) against independent oracles: with every layer off it is
// the input, delayed by its latency; each layer alone sits near the layer
// reference and has its own spectrum (hum on the mains and its harmonics,
// hiss rising, rumble below 50 Hz, room tone low); pops arrive at the
// crackle rate and scratches recur once a revolution; the radio's gate
// follows the tempo; a seed is deterministic, any block split and a reset
// render identically; the duck lowers the bed, never the input; Age folds
// lows to mono and wow and flutter bend a sine within their depth.
namespace {
using namespace effect_test;
namespace dust = oscillate::native_dsp::effects::dust;
using dust::Layer;

const effects::EffectEngine& engine() { return dust::engine(); }
const EngineDescriptor& table() { return *engine().descriptor; }
std::size_t at(std::string_view id) { return index(table(), id); }

constexpr double rate = oscillate::test::sample_rate_48k_hz;
constexpr std::size_t second = 48'000;
constexpr std::array<std::string_view, dust::layer_count> layer_names{
    "vinyl", "tape", "hum", "radio", "rain", "wind", "room", "machine"};
// The presets the contract names, Dusty 33 (the defaults) first.
constexpr std::array<std::string_view, 16> preset_names{"Dusty 33",
                                                        "Worn 78",
                                                        "Fresh 45",
                                                        "Cassette Deck",
                                                        "Late Night Radio",
                                                        "Static Pulse",
                                                        "Rain on the Window",
                                                        "Storm Front",
                                                        "Wind Through the Yard",
                                                        "Dub Chamber",
                                                        "Basement Hum",
                                                        "Server Room",
                                                        "Warehouse",
                                                        "Lo-fi Bed",
                                                        "Field Day",
                                                        "Init"};

std::string layer_id(Layer layer, std::string_view control) {
  return "dust." + std::string(layer_names[static_cast<std::size_t>(layer)]) + "." +
         std::string(control);
}

std::vector<double> init() { return preset(table(), "Init"); }

std::size_t latency() { return engine().latency_frames(init(), rate); }

Stereo silence(std::size_t frames) {
  return {std::vector<double>(frames), std::vector<double>(frames)};
}

// One layer alone at Level 0 dB and its defaults; the bed at 0 dB, no duck.
std::vector<double> solo(Layer layer) {
  auto v = init();
  v[at(layer_id(layer, "on"))] = 1.0;
  v[at("dust.mix.bed")] = 0.0;
  return v;
}

Stereo render(const std::vector<double>& v, const Stereo& input, std::size_t block = 128,
              const Stereo* sidechain = nullptr, double bpm = 120.0) {
  auto runtime = make_effect(engine(), v, rate);
  return process(*runtime, input,
                 {.block = block, .sidechain = sidechain, .rate = rate, .bpm = bpm});
}

double level_db(const Stereo& x, std::size_t from, std::size_t to) {
  double power = 0.0;
  for (std::size_t i = from; i < to; ++i) {
    power += x.left[i] * x.left[i] + x.right[i] * x.right[i];
  }
  return 10.0 * std::log10(power / (2.0 * static_cast<double>(to - from)) + 1e-30);
}

// Hann-windowed power per bin, `size` (a power of two) frames from `from`.
std::vector<double> power_spectrum(const std::vector<double>& x, std::size_t from,
                                   std::size_t size) {
  std::vector<std::complex<double>> window(size);
  for (std::size_t i = 0; i < size; ++i) {
    const double hann =
        oscillate::native_dsp::dsp::hann(static_cast<double>(i), static_cast<double>(size));
    window[i] = x[from + i] * hann;
  }
  fft(window);
  std::vector<double> power(size / 2);
  for (std::size_t bin = 0; bin < power.size(); ++bin) {
    power[bin] = std::norm(window[bin]);
  }
  return power;
}

double bin_hz(std::size_t size) { return rate / static_cast<double>(size); }

double band_energy(const std::vector<double>& power, double low_hz, double high_hz,
                   std::size_t size) {
  double sum = 0.0;
  for (std::size_t bin = 1; bin < power.size(); ++bin) {
    const double hz = static_cast<double>(bin) * bin_hz(size);
    if (hz >= low_hz && hz < high_hz) {
      sum += power[bin];
    }
  }
  return sum;
}

// The power-weighted mean frequency.
double power_centroid(const std::vector<double>& power, std::size_t size) {
  double weighted = 0.0;
  double total = 0.0;
  for (std::size_t bin = 1; bin < power.size(); ++bin) {
    weighted += power[bin] * static_cast<double>(bin) * bin_hz(size);
    total += power[bin];
  }
  return weighted / (total + 1e-30);
}

double max_difference(const Stereo& a, const Stereo& b, std::size_t from = 0) {
  double worst = 0.0;
  for (std::size_t i = from; i < a.size(); ++i) {
    worst = std::max({worst, std::fabs(a.left[i] - b.left[i]), std::fabs(a.right[i] - b.right[i])});
  }
  return worst;
}

// A kick-like key: a 60 Hz burst at `level` every `period` frames.
Stereo kicks(std::size_t frames, std::size_t period, std::size_t length, double level) {
  auto out = silence(frames);
  for (std::size_t start = period / 2; start < frames; start += period) {
    for (std::size_t i = 0; i < length && start + i < frames; ++i) {
      const double x =
          level * std::sin(2.0 * std::numbers::pi * 60.0 * static_cast<double>(i) / rate);
      out.left[start + i] = x;
      out.right[start + i] = x;
    }
  }
  return out;
}

// ---- The contract -------------------------------------------------------------------------

void descriptor_registry_and_presets() {
  REQUIRE_EQ(effects::find_engine("dust"), &engine(), "Dust is registered");
  REQUIRE_EQ(table().id, "dust", "its id and name");
  REQUIRE_EQ(table().name, "Dust", "its id and name");
  REQUIRE_OK(validate_engine(table()), "its table validates");
  REQUIRE_TRUE(engine().generator, "a generator, keyed by a sidechain or notes");
  REQUIRE_TRUE(engine().sidechain, "a generator, keyed by a sidechain or notes");
  REQUIRE_TRUE(engine().key_notes, "a generator, keyed by a sidechain or notes");
  REQUIRE_EQ(table().presets.size(), preset_names.size(), "every named preset");
  for (std::size_t i = 0; i < preset_names.size(); ++i) {
    REQUIRE_EQ(table().presets[i].name, preset_names[i],
               "preset in order: " + std::string(preset_names[i]));
    (void)preset(table(), preset_names[i]);
  }
  REQUIRE_EQ(preset(table(), "Dusty 33"), default_values(table()), "Dusty 33 is the defaults");
  const auto v = default_values(table());
  REQUIRE_EQ(v[at("dust.vinyl.on")], 1.0, "the defaults play vinyl and tape");
  REQUIRE_EQ(v[at("dust.tape.on")], 1.0, "the defaults play vinyl and tape");
  REQUIRE_EQ(v[at("dust.hum.on")], 0.0, "the defaults play vinyl and tape");
  REQUIRE_EQ(v[at("dust.rain.on")], 0.0, "the defaults play vinyl and tape");
  REQUIRE_EQ(engine().latency_frames(v, rate),
             static_cast<std::uint32_t>(std::lround(dust::latency_seconds * rate)),
             "the latency is the age's base delay");
}

void every_layer_off_is_the_input() {
  const auto quiet = render(init(), silence(second));
  REQUIRE_EQ(peak(quiet.left), 0.0, "silence in, every layer off: silence out");
  REQUIRE_EQ(peak(quiet.right), 0.0, "silence in, every layer off: silence out");
  const auto input = noise(0.4, second);
  const auto out = render(init(), input);
  const std::size_t delay = latency();
  for (std::size_t i = delay; i < input.size(); ++i) {
    REQUIRE_EQ(out.left[i], static_cast<double>(static_cast<float>(input.left[i - delay])),
               "Init is the input, delayed by the latency, bit for bit");
    REQUIRE_EQ(out.right[i], static_cast<double>(static_cast<float>(input.right[i - delay])),
               "Init is the input, delayed by the latency, bit for bit");
  }
}

void each_layer_sits_at_its_level() {
  for (std::size_t l = 0; l < dust::layer_count; ++l) {
    const auto layer = static_cast<Layer>(l);
    const auto out = render(solo(layer), silence(10 * second));
    const double db = level_db(out, second, out.size());
    // Event layers (pops and drops) vary more around their mean.
    const bool events = layer == Layer::vinyl || layer == Layer::rain;
    const double tolerance = events ? 4.0 : 3.0;
    std::printf("  %-8s %.2f dBFS\n", std::string(layer_names[l]).c_str(), db);
    REQUIRE_LT(std::fabs(db - dust::layer_reference_dbfs), tolerance,
               std::string(layer_names[l]) + " sits near the reference: " + std::to_string(db));
  }
}

void each_layer_has_its_character() {
  constexpr std::size_t size = 32'768;
  // Hum: nearly all of it on the mains and its harmonics; the strongest at
  // the mains itself.
  for (std::size_t mains = 0; mains < dust::mains_hz.size(); ++mains) {
    auto v = solo(Layer::hum);
    v[at("dust.hum.mains")] = static_cast<double>(mains);
    const auto out = render(v, silence(2 * second));
    const auto power = power_spectrum(out.left, second / 2, size);
    const double hz = dust::mains_hz[mains];
    double harmonic = 0.0;
    for (std::size_t n = 1; static_cast<double>(n) * hz < rate / 2.0; ++n) {
      harmonic += band_energy(power, static_cast<double>(n) * hz - 3.0,
                              static_cast<double>(n) * hz + 3.0, size);
    }
    const double total = band_energy(power, 0.0, rate / 2.0, size);
    REQUIRE_GT(harmonic, 0.95 * total, "hum lies on the mains' harmonics");
    const auto strongest =
        static_cast<std::size_t>(std::max_element(power.begin() + 1, power.end()) - power.begin());
    REQUIRE_LT(std::fabs(static_cast<double>(strongest) * bin_hz(size) - hz), 2.0 * bin_hz(size),
               "the strongest line is the mains: " + std::to_string(hz));
  }
  // Tape hiss rises: most of its energy above 2 kHz.
  {
    const auto out = render(solo(Layer::tape), silence(2 * second));
    const auto power = power_spectrum(out.left, second / 2, size);
    REQUIRE_GT(band_energy(power, 2'000.0, rate / 2.0, size),
               4.0 * band_energy(power, 0.0, 2'000.0, size), "hiss energy rises above 2 kHz");
  }
  // Rumble: Rumble up puts energy below 50 Hz, where the rest has little.
  {
    auto v = solo(Layer::vinyl);
    v[at("dust.vinyl.crackle")] = 0.0;
    v[at("dust.vinyl.wear")] = 0.0;
    v[at("dust.vinyl.rumble")] = 0.0;
    const auto still = render(v, silence(3 * second));
    v[at("dust.vinyl.rumble")] = 1.0;
    const auto rumbling = render(v, silence(3 * second));
    const auto a = power_spectrum(still.left, second, size);
    const auto b = power_spectrum(rumbling.left, second, size);
    REQUIRE_GT(band_energy(b, 5.0, 50.0, size), 100.0 * band_energy(a, 5.0, 50.0, size),
               "rumble lives below 50 Hz");
    REQUIRE_GT(band_energy(b, 5.0, 50.0, size), band_energy(b, 50.0, 200.0, size),
               "and stays there");
  }
  // Room tone is low; wind sits in its bands.
  {
    const auto room = render(solo(Layer::room), silence(2 * second));
    const auto power = power_spectrum(room.left, second / 2, size);
    REQUIRE_GT(band_energy(power, 0.0, 1'000.0, size),
               0.9 * band_energy(power, 0.0, rate / 2.0, size), "room tone is a low bed");
    const auto wind = render(solo(Layer::wind), silence(2 * second));
    const auto blowing = power_spectrum(wind.left, second / 2, size);
    REQUIRE_GT(band_energy(blowing, 60.0, 6'000.0, size),
               0.85 * band_energy(blowing, 0.0, rate / 2.0, size), "wind blows in its bands");
    const double c = power_centroid(blowing, size);
    require(c > 200.0 && c < 3'000.0, "wind's centre: " + std::to_string(c));
  }
  // Rain on glass rings brighter than on a roof.
  {
    auto v = solo(Layer::rain);
    v[at("dust.rain.surface")] = 0.0;
    const auto glass = render(v, silence(2 * second));
    v[at("dust.rain.surface")] = 1.0;
    const auto roof = render(v, silence(2 * second));
    REQUIRE_GT(power_centroid(power_spectrum(glass.left, second / 2, size), size),
               power_centroid(power_spectrum(roof.left, second / 2, size), size),
               "glass is brighter than a roof");
  }
  // A fan has a blade-pass line below 200 Hz standing over its air.
  {
    const auto out = render(solo(Layer::machine), silence(2 * second));
    const auto power = power_spectrum(out.left, second / 2, size);
    const auto first = static_cast<std::size_t>(20.0 / bin_hz(size));
    const auto last = static_cast<std::size_t>(200.0 / bin_hz(size));
    const auto line = std::max_element(power.begin() + static_cast<std::ptrdiff_t>(first),
                                       power.begin() + static_cast<std::ptrdiff_t>(last));
    std::vector<double> around(power.begin() + static_cast<std::ptrdiff_t>(first),
                               power.begin() + static_cast<std::ptrdiff_t>(last));
    std::nth_element(around.begin(),
                     around.begin() + static_cast<std::ptrdiff_t>(around.size() / 2), around.end());
    REQUIRE_GT(*line, 1'000.0 * around[around.size() / 2], "the fan's blade-pass line stands out");
  }
}

void pops_follow_the_crackle_rate() {
  constexpr double crackle = 0.8;
  constexpr std::size_t seconds = 20;
  auto v = solo(Layer::vinyl);
  v[at("dust.vinyl.crackle")] = crackle;
  v[at("dust.vinyl.wear")] = 0.0; // No scratches: only pops count.
  auto runtime = make_effect(engine(), v, rate);
  (void)process(*runtime, silence(seconds * second), {.rate = rate});
  std::vector<float> frame;
  REQUIRE_TRUE(runtime->read_telemetry(frame), "telemetry is published");
  REQUIRE_EQ(frame.size(), dust::telemetry::size, "telemetry is published");
  const double expected = dust::pop_rate_max * (std::pow(dust::pop_rate_span, crackle) - 1.0) /
                          (dust::pop_rate_span - 1.0) * static_cast<double>(seconds);
  const double counted = static_cast<double>(frame[dust::telemetry::pops]);
  // Poisson: a standard deviation is sqrt(expected), about 6% here.
  REQUIRE_LT(std::fabs(counted - expected), 0.2 * expected,
             "pops arrive at the crackle rate: " + std::to_string(counted) + " of " +
                 std::to_string(expected));
  auto none = solo(Layer::vinyl);
  none[at("dust.vinyl.crackle")] = 0.0;
  none[at("dust.vinyl.wear")] = 0.0;
  auto quiet = make_effect(engine(), none, rate);
  (void)process(*quiet, silence(5 * second), {.rate = rate});
  REQUIRE_TRUE(quiet->read_telemetry(frame), "no crackle, no pops");
  REQUIRE_EQ(frame[dust::telemetry::pops], 0.0F, "no crackle, no pops");
}

// Pearson correlation of x[n] with x[n + lag] over [from, size - lag).
double correlation(const std::vector<double>& x, std::size_t from, std::size_t lag) {
  const std::size_t n = x.size() - lag - from;
  double ma = 0.0;
  double mb = 0.0;
  for (std::size_t i = from; i + lag < x.size(); ++i) {
    ma += x[i];
    mb += x[i + lag];
  }
  ma /= static_cast<double>(n);
  mb /= static_cast<double>(n);
  double ab = 0.0;
  double aa = 0.0;
  double bb = 0.0;
  for (std::size_t i = from; i + lag < x.size(); ++i) {
    const double a = x[i] - ma;
    const double b = x[i + lag] - mb;
    ab += a * b;
    aa += a * a;
    bb += b * b;
  }
  return ab / std::sqrt(aa * bb + 1e-30);
}

void scratches_come_round_with_the_record() {
  constexpr std::size_t speed = 2; // 78
  auto v = solo(Layer::vinyl);
  v[at("dust.vinyl.speed")] = static_cast<double>(speed);
  v[at("dust.vinyl.crackle")] = 0.0;
  v[at("dust.vinyl.dust")] = 0.0;
  v[at("dust.vinyl.wear")] = 0.9;
  v[at("dust.vinyl.rumble")] = 0.0;
  v[at("dust.vinyl.width")] = 0.0;
  const double period = 60.0 * rate / dust::rpm[speed];
  const auto revolution = static_cast<std::size_t>(std::lround(period));
  const auto out = render(v, silence(7 * revolution));
  // The envelope: |x| through a 1 ms one-pole.
  std::vector<double> envelope(out.size());
  const double k = 1.0 - std::exp(-1.0 / (1e-3 * rate));
  double e = 0.0;
  for (std::size_t i = 0; i < out.size(); ++i) {
    e += k * (std::fabs(out.left[i]) - e);
    envelope[i] = e;
  }
  const double full = correlation(envelope, revolution / 4, revolution);
  const double half = correlation(envelope, revolution / 4, revolution / 2);
  // The surface swells once a revolution too, so half a turn on the
  // envelope is anti-correlated; what matters is that a whole turn matches
  // far better than half of one.
  constexpr double recurrence = 0.3;
  require(full > recurrence && full > half + recurrence,
          "scratches recur once a revolution: " + std::to_string(full) + " against " +
              std::to_string(half));
}

void radio_gate_follows_the_tempo() {
  constexpr std::size_t gate = 3; // 1/16
  constexpr double bpm = 120.0;
  auto v = solo(Layer::radio);
  v[at("dust.radio.gate")] = static_cast<double>(gate);
  const auto out = render(v, silence(4 * second), 128, nullptr, bpm);
  const double step = dust::gate_beats[gate] * 60.0 / bpm * rate;
  const std::size_t delay = latency();
  std::size_t opened = 0;
  std::size_t steps = 0;
  for (std::size_t k = 1; static_cast<double>(k + 1) * step + static_cast<double>(delay) <
                          static_cast<double>(out.size());
       ++k) {
    const double start = static_cast<double>(k) * step + static_cast<double>(delay);
    const auto frame = [&](double share) { return static_cast<std::size_t>(start + share * step); };
    const double open = level_db(out, frame(0.05), frame(0.9 * dust::gate_hold));
    const double closed = level_db(out, frame(dust::gate_hold + 0.15), frame(0.95));
    ++steps;
    if (open - closed > 12.0) {
      ++opened;
    }
  }
  REQUIRE_GT(opened * 2, steps,
             "most steps open on the beat grid and close within it: " + std::to_string(opened) +
                 " of " + std::to_string(steps));
}

void seeds_are_deterministic_and_distinct() {
  const auto v = default_values(table());
  const auto a = render(v, silence(second));
  const auto b = render(v, silence(second));
  REQUIRE_EQ(max_difference(a, b), 0.0, "the same seed renders the same");
  auto w = v;
  w[at("dust.mix.seed")] = 2.0;
  const auto c = render(w, silence(second));
  REQUIRE_GT(max_difference(a, c), 1e-3, "another seed, another record");
}

void any_block_split_renders_identically() {
  const auto input = noise(0.2, second);
  const auto side = kicks(second, second / 4, 960, 0.8);
  const std::vector<effects::KeyNote> notes{{6'000, 36, 127}, {18'000, 36, 100}, {30'000, 36, 90}};
  for (const auto& entry : table().presets) {
    for (const double source : {0.0, 1.0}) {
      auto v = preset(table(), entry.name);
      v[at("dust.duck.source")] = source;
      std::vector<Stereo> outs;
      for (const std::size_t block : {std::size_t{128}, std::size_t{0}, std::size_t{1'024}}) {
        auto runtime = make_effect(engine(), v, rate);
        (void)runtime->offer_key_notes(notes);
        outs.push_back(
            process(*runtime, input, {.block = block, .sidechain = &side, .rate = rate}));
      }
      require(max_difference(outs[0], outs[1]) == 0.0 && max_difference(outs[0], outs[2]) == 0.0,
              "block splits render identically: " + std::string(entry.name));
    }
  }
}

void reset_is_deterministic() {
  constexpr std::uint64_t seek = 96'000;
  const std::size_t settle = static_cast<std::size_t>(std::lround(0.02 * rate));
  for (const std::string_view name :
       {"Dusty 33", "Late Night Radio", "Dub Chamber", "Server Room"}) {
    const auto v = preset(table(), name);
    auto played = make_effect(engine(), v, rate);
    (void)process(*played, noise(0.2, second), {.rate = rate});
    const auto after = process(*played, silence(second), {.generation = 2, .rate = rate}, seek);
    auto fresh = make_effect(engine(), v, rate);
    const auto expected = process(*fresh, silence(second), {.rate = rate}, seek);
    REQUIRE_EQ(max_difference(after, expected, settle), 0.0,
               "a seek renders as a fresh start there: " + std::string(name));
  }
}

void duck_lowers_the_bed_not_the_input() {
  constexpr std::size_t period = second / 2;
  constexpr std::size_t length = 960;
  const std::size_t frames = 3 * second;
  const auto side = kicks(frames, period, length, 0.8);
  auto v = solo(Layer::room);
  v[at("dust.duck.amount")] = 1.0;
  v[at("dust.duck.release")] = 100.0;
  // Sidechain.
  {
    const auto out = render(v, silence(frames), 128, &side);
    for (std::size_t start = period / 2 + period; start + period < frames; start += period) {
      const double ducked = level_db(out, start + 96, start + length);
      const double open = level_db(out, start + period - 4'800, start + period - 96);
      REQUIRE_LT(ducked, open - 20.0, "the bed ducks under the sidechain");
    }
  }
  // Key notes, heard on the compensated clock.
  {
    auto keyed = v;
    keyed[at("dust.duck.source")] = 1.0;
    auto runtime = make_effect(engine(), keyed, rate);
    std::vector<effects::KeyNote> notes;
    for (std::size_t start = period / 2; start < frames; start += period) {
      notes.push_back({static_cast<std::int64_t>(start), 36, 127});
    }
    require(runtime->offer_key_notes(notes), "notes are offered");
    const auto out = process(*runtime, silence(frames), {.rate = rate});
    const std::size_t delay = latency();
    // Down on the hit, then releasing.
    const std::size_t hit = static_cast<std::size_t>(std::lround(2e-3 * rate));
    for (std::size_t start = period / 2 + period; start + period < frames; start += period) {
      const double ducked = level_db(out, start + delay, start + delay + hit);
      const double open =
          level_db(out, start + delay + period - 4'800, start + delay + period - 480);
      REQUIRE_LT(ducked, open - 20.0, "the bed ducks on each key note");
    }
  }
  // The input is never ducked.
  {
    auto input_only = init();
    input_only[at("dust.duck.amount")] = 1.0;
    const auto input = sine(1'000.0, 0.25, frames);
    const auto out = render(input_only, input, 128, &side);
    const std::size_t delay = latency();
    for (std::size_t i = delay; i < frames; ++i) {
      REQUIRE_EQ(out.left[i], static_cast<double>(static_cast<float>(input.left[i - delay])),
                 "the input passes the duck untouched");
    }
  }
}

// Amplitude of `hz` in x over [from, to), by projection.
double amplitude_at(const std::vector<double>& x, double hz, std::size_t from, std::size_t to) {
  double s = 0.0;
  double c = 0.0;
  for (std::size_t i = from; i < to; ++i) {
    const double w = 2.0 * std::numbers::pi * hz * static_cast<double>(i) / rate;
    s += x[i] * std::sin(w);
    c += x[i] * std::cos(w);
  }
  return 2.0 * std::hypot(s, c) / static_cast<double>(to - from);
}

void age_folds_lows_to_mono() {
  constexpr double low_hz = 60.0;
  constexpr double high_hz = 3'000.0;
  constexpr double mono_hz = 200.0;
  const std::size_t frames = second;
  // All side: the left and right in opposite phase.
  Stereo input = silence(frames);
  for (std::size_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i) / rate;
    const double x = 0.3 * std::sin(2.0 * std::numbers::pi * low_hz * t) +
                     0.3 * std::sin(2.0 * std::numbers::pi * high_hz * t);
    input.left[i] = x;
    input.right[i] = -x;
  }
  auto v = init();
  v[at("dust.age.mono")] = mono_hz;
  const auto out = render(v, input);
  std::vector<double> side(frames);
  for (std::size_t i = 0; i < frames; ++i) {
    side[i] = 0.5 * (out.left[i] - out.right[i]);
  }
  const std::size_t from = frames / 4;
  REQUIRE_LT(amplitude_at(side, low_hz, from, frames), 0.3 * 0.1, "lows fold to mono");
  REQUIRE_LT(std::fabs(amplitude_at(side, high_hz, from, frames) - 0.3), 0.3 * 0.12,
             "highs keep their width");
}

// The largest deviation of the pitch from `hz`, over windows of `window`.
double largest_deviation(const std::vector<double>& x, double hz, std::size_t from,
                         std::size_t window) {
  double worst = 0.0;
  for (std::size_t start = from; start + window < x.size(); start += window / 2) {
    worst = std::max(worst, std::fabs(measured_hz(x, start, start + window, rate) / hz - 1.0));
  }
  return worst;
}

void wow_and_flutter_bend_a_sine() {
  constexpr double hz = 1'000.0;
  const auto input = sine(hz, 0.5, 8 * second);
  auto v = init();
  v[at("dust.age.wow")] = 1.0;
  const auto wow = render(v, input);
  const double w = largest_deviation(wow.left, hz, second / 2, second / 50);
  require(w > 0.5 * dust::wow_max_deviation && w < 1.1 * dust::wow_max_deviation,
          "wow bends the pitch within its depth: " + std::to_string(w));
  v[at("dust.age.wow")] = 0.0;
  v[at("dust.age.flutter")] = 1.0;
  const auto flutter = render(v, input);
  const double f = largest_deviation(flutter.left, hz, second / 2, second / 100);
  require(f > 0.3 * dust::flutter_max_deviation && f < 1.1 * dust::flutter_max_deviation,
          "flutter bends the pitch within its depth: " + std::to_string(f));
}

void every_preset_is_finite_and_bounded() {
  const auto input = noise(0.3, 3 * second);
  const auto side = kicks(3 * second, second / 2, 960, 0.9);
  for (const auto& entry : table().presets) {
    const auto out = render(preset(table(), entry.name), input, 128, &side);
    bool finite = true;
    for (std::size_t i = 0; i < out.size(); ++i) {
      finite = finite && std::isfinite(out.left[i]) && std::isfinite(out.right[i]);
    }
    require(finite, "finite: " + std::string(entry.name));
    REQUIRE_LT(std::max(peak(out.left), peak(out.right)), 2.0,
               "bounded: " + std::string(entry.name));
  }
}

void the_callback_never_allocates() {
  const auto values = preset(table(), "Storm Front");
  auto runtime = make_effect(engine(), values, rate);
  (void)runtime->offer_key_notes({{1'000, 36, 100}, {20'000, 36, 100}});
  std::vector<engine::StereoFrame> buffer(128, {0.1F, -0.1F});
  std::vector<engine::StereoFrame> side(128, {0.5F, 0.5F});
  // Every layer on, every switch turned, mid-run.
  auto all = values;
  for (std::size_t l = 0; l < dust::layer_count; ++l) {
    all[at(layer_id(static_cast<Layer>(l), "on"))] = 1.0;
  }
  all[at("dust.radio.gate")] = 2.0;
  all[at("dust.duck.source")] = 1.0;
  all[at("dust.mix.seed")] = 7.0;
  (void)process(*runtime, silence(1'024), {.rate = rate});
  require(runtime->offer_values(all, 1), "every layer is switched on");
  const SynthTransport transport{0.0, 120.0 / 60.0 / rate, 0.0, 4.0};
  counted_allocations = 0;
  counting_allocations = true;
  for (std::uint64_t block = 0; block < 400; ++block) {
    (void)runtime->process(buffer, side, 1'024 + block * buffer.size(), block < 300 ? 1U : 2U,
                           transport, {});
  }
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "the callback allocates nothing");
}

void telemetry_feeds_the_panel() {
  auto runtime = make_effect(engine(), default_values(table()), rate);
  (void)process(*runtime, silence(4 * second), {.rate = rate});
  std::vector<float> frame;
  REQUIRE_TRUE(runtime->read_telemetry(frame), "a telemetry frame is published");
  REQUIRE_EQ(frame.size(), dust::telemetry::size, "a telemetry frame is published");
  const auto layer = [&](Layer l) {
    return frame[dust::telemetry::first_layer + static_cast<std::size_t>(l)];
  };
  REQUIRE_GT(layer(Layer::vinyl), -60.0F, "playing layers meter");
  REQUIRE_GT(layer(Layer::tape), -60.0F, "playing layers meter");
  REQUIRE_EQ(layer(Layer::hum), static_cast<float>(dust::meter_floor_db), "silent layers rest");
  REQUIRE_GE(frame[dust::telemetry::rotation], 0.0F, "the record turns");
  REQUIRE_LT(frame[dust::telemetry::rotation], 1.0F, "the record turns");
  REQUIRE_GE(frame[dust::telemetry::duck_db], 0.0F, "the duck reads a reduction");
  REQUIRE_GT(frame[dust::telemetry::output_peak], -60.0F, "the output peaks");
  REQUIRE_GT(frame[dust::telemetry::output_peak + 1], -60.0F, "the output peaks");
}

// Each preset's sound: its level and brightness over four seconds of quiet
// programme with a kick on the sidechain. Not pinned yet: run with
// DUST_REBASELINE=1 after the first build and paste the printed rows here;
// rebaseline the same way after an intended change of sound.
struct Fingerprint {
  double level_db;
  double bright_hz;
};
const std::vector<std::pair<std::string_view, Fingerprint>> golden{
    {"Dusty 33", {-33.722, 7275.1}},
    {"Worn 78", {-35.779, 5145.0}},
    {"Fresh 45", {-32.824, 9637.4}},
    {"Cassette Deck", {-34.481, 6810.0}},
    {"Late Night Radio", {-36.493, 5031.6}},
    {"Static Pulse", {-30.753, 11940.8}},
    {"Rain on the Window", {-30.720, 11933.3}},
    {"Storm Front", {-30.597, 11688.7}},
    {"Wind Through the Yard", {-30.679, 11856.8}},
    {"Dub Chamber", {-34.267, 6801.6}},
    {"Basement Hum", {-30.743, 11943.0}},
    {"Server Room", {-30.726, 11912.0}},
    {"Warehouse", {-32.174, 10469.0}},
    {"Lo-fi Bed", {-35.436, 5813.2}},
    {"Field Day", {-30.754, 11951.1}},
    {"Init", {-30.790, 11988.9}},
};

void preset_fingerprints() {
  const bool print = std::getenv("DUST_REBASELINE") != nullptr;
  const auto input = noise(0.05, 4 * second);
  const auto side = kicks(4 * second, second / 2, 960, 0.8);
  for (const auto& entry : table().presets) {
    const auto out = render(preset(table(), entry.name), input, 128, &side);
    const double level = level_db(out, second, out.size());
    const double bright = centroid_hz(out.left, second);
    if (print) {
      std::printf("    {\"%s\", {%.3f, %.1f}},\n", std::string(entry.name).c_str(), level, bright);
      continue;
    }
    const auto expected =
        std::ranges::find(golden, entry.name, &std::pair<std::string_view, Fingerprint>::first);
    REQUIRE_NE(expected, golden.end(),
               std::string(entry.name) +
                   " has a golden fingerprint (DUST_REBASELINE=1 prints them)");
    require(std::fabs(level - expected->second.level_db) < 0.01 &&
                std::fabs(bright - expected->second.bright_hz) < 0.1,
            std::string(entry.name) + " sounds as it did");
  }
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "dust contract passed",
      {
          {"descriptor_registry_and_presets", [] { descriptor_registry_and_presets(); }},
          {"every_layer_off_is_the_input", [] { every_layer_off_is_the_input(); }},
          {"each_layer_sits_at_its_level", [] { each_layer_sits_at_its_level(); }},
          {"each_layer_has_its_character", [] { each_layer_has_its_character(); }},
          {"pops_follow_the_crackle_rate", [] { pops_follow_the_crackle_rate(); }},
          {"scratches_come_round_with_the_record", [] { scratches_come_round_with_the_record(); }},
          {"radio_gate_follows_the_tempo", [] { radio_gate_follows_the_tempo(); }},
          {"seeds_are_deterministic_and_distinct", [] { seeds_are_deterministic_and_distinct(); }},
          {"any_block_split_renders_identically", [] { any_block_split_renders_identically(); }},
          {"reset_is_deterministic", [] { reset_is_deterministic(); }},
          {"duck_lowers_the_bed_not_the_input", [] { duck_lowers_the_bed_not_the_input(); }},
          {"age_folds_lows_to_mono", [] { age_folds_lows_to_mono(); }},
          {"wow_and_flutter_bend_a_sine", [] { wow_and_flutter_bend_a_sine(); }},
          {"every_preset_is_finite_and_bounded", [] { every_preset_is_finite_and_bounded(); }},
          {"the_callback_never_allocates", [] { the_callback_never_allocates(); }},
          {"telemetry_feeds_the_panel", [] { telemetry_feeds_the_panel(); }},
          {"preset_fingerprints", [] { preset_fingerprints(); }},
      });
}
