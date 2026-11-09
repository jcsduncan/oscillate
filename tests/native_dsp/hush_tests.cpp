#include "effect_test_support.hpp"

#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/effects/hush.hpp"
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
#include <numbers>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

// Hush (ADR-0116) against measured oracles: the closed gain is exactly the
// range (and silence at its minimum), the open gain exactly unity, no
// chatter on a key hovering at the threshold, attack, hold and release
// timed, the expander's static curve, lookahead opening ahead of the
// transient with the latency reported, the key filter, the external key,
// listen, stereo link, flip, block splits, export against live, hostile
// input, restrained presets, an allocation-free callback and the panel's
// telemetry.
namespace {
using namespace effect_test;
namespace hush = oscillate::native_dsp::effects::hush;

const effects::EffectEngine& engine() { return hush::engine(); }
const EngineDescriptor& table() { return *engine().descriptor; }
std::size_t at(std::string_view id) { return index(table(), id); }

constexpr double rate = oscillate::test::sample_rate_48k_hz;
constexpr double one_second = rate;
constexpr double milliseconds = 1'000.0;
// One millisecond of frames: timing tolerance.
constexpr double millisecond_frames = rate / milliseconds;
// The input whose output reads the gain directly: a constant.
constexpr double steady = 0.5;
// A key well above every threshold (-6 dBFS) and a tone the default key
// filter passes.
constexpr double loud = 0.5;
constexpr double tone_hz = 1'000.0;
// The expander's key: high enough that the detector barely ripples.
constexpr double ripple_free_hz = 8'000.0;
constexpr double threshold_db = -40.0;
constexpr double hysteresis_db = 6.0;
constexpr std::size_t expander_mode = 1;
constexpr std::size_t input_key = 0;
constexpr std::size_t parameter_count = 15;
constexpr std::size_t preset_count = 8;
// Render lengths, and where a key starts and stops within them.
constexpr double short_seconds = 0.2;
constexpr double render_seconds = 0.5;
constexpr double long_seconds = 1.0;
constexpr double wobble_seconds = 2.0;
constexpr double key_on_seconds = 0.1;
constexpr double key_off_seconds = 0.4;
constexpr double short_key_off_seconds = 0.3;
// The fastest gate the table allows, for the chatter oracle.
constexpr double fastest_attack_ms = 0.1;
constexpr double fastest_release_ms = 1.0;
// Key filter corners: a kick's band, above the lows, and a listen split.
constexpr double kick_key_lp_hz = 200.0;
constexpr double bleed_key_hp_hz = 2'000.0;
constexpr double listen_key_hp_hz = 1'000.0;
constexpr std::size_t lookahead_5ms = 4;
constexpr std::size_t lookahead_off = 0;

std::vector<double> defaults() { return preset(table(), "Default"); }

double gain(double decibels) { return oscillate::native_dsp::dsp::db_to_gain(decibels); }

std::size_t frames(double seconds) { return static_cast<std::size_t>(seconds * one_second); }

Stereo constant(std::size_t count, double level = steady) {
  return {std::vector<double>(count, level), std::vector<double>(count, level)};
}

Stereo silence(std::size_t count) { return constant(count, 0.0); }

// A sine at `hz` on [from, to), silence elsewhere.
Stereo burst(std::size_t count, std::size_t from, std::size_t to, double amplitude,
             double hz = tone_hz) {
  Stereo out = silence(count);
  for (std::size_t i = from; i < std::min(to, count); ++i) {
    const double x =
        amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i - from) / rate);
    out.left[i] = x;
    out.right[i] = x;
  }
  return out;
}

Stereo added(Stereo a, const Stereo& b) {
  for (std::size_t i = 0; i < a.size(); ++i) {
    a.left[i] += b.left[i];
    a.right[i] += b.right[i];
  }
  return a;
}

Stereo render(const std::vector<double>& values, const Stereo& input, const Stereo* key = nullptr,
              std::size_t block = oscillate::test::block_128) {
  auto runtime = make_effect(engine(), values, rate);
  return process(*runtime, input, {.block = block, .sidechain = key});
}

std::vector<double> with(std::vector<double> values, std::string_view id, double value) {
  values[at(id)] = value;
  return values;
}

// The gain each frame applied to a constant input.
std::vector<double> gains(const Stereo& out) {
  std::vector<double> result(out.size());
  for (std::size_t i = 0; i < out.size(); ++i) {
    result[i] = out.left[i] / steady;
  }
  return result;
}

// The first frame at or after `from` where pred holds; the end if none.
template <typename Pred>
std::size_t first(const std::vector<double>& x, std::size_t from, Pred pred) {
  for (std::size_t i = from; i < x.size(); ++i) {
    if (pred(x[i])) {
      return i;
    }
  }
  return x.size();
}

void descriptor_registry_and_presets() {
  REQUIRE_EQ(effects::find_engine("hush"), &engine(), "Hush is registered");
  require(engine().sidechain && !engine().generator, "Hush takes a key");
  REQUIRE_EQ(table().parameters.size(), parameter_count, "the table's size");
  require(!table().parameters[at("hush.lookahead")].automatable, "lookahead sets the latency");
  REQUIRE_EQ(table().presets.size(), preset_count, "Default and seven presets");
  for (const auto* name : {"Default", "Tight Kick", "Snare Bleed", "Tom Bleed", "Gated Verb Key",
                           "Hum Gate", "Soft Expander", "Vocal Breath"}) {
    const auto out = render(preset(table(), name), noise(steady, frames(short_seconds)));
    for (std::size_t i = 0; i < out.size(); ++i) {
      require(std::isfinite(out.left[i]) && std::isfinite(out.right[i]),
              "preset renders finite: " + std::string(name));
    }
  }
}

// With the key silent the gain settles on the range exactly; at the range's
// minimum the output is silence, not merely quiet.
void closed_is_exactly_the_range() {
  const auto count = frames(render_seconds);
  const auto quiet = silence(count);
  constexpr std::array ranges_db{-6.0, -30.0, -79.0};
  for (const double range_db : ranges_db) {
    const auto g = gains(render(with(defaults(), "hush.range", range_db), constant(count), &quiet));
    for (std::size_t i = count / 2; i < count; ++i) {
      REQUIRE_LT(std::fabs(g[i] - gain(range_db)), tolerance::float_round_trip,
                 "the range: " + std::to_string(range_db));
    }
  }
  const auto out = render(defaults(), constant(count), &quiet);
  for (std::size_t i = 0; i < count; ++i) {
    REQUIRE_EQ(out.left[i], 0.0, "the minimum range mutes");
    REQUIRE_EQ(out.right[i], 0.0, "the minimum range mutes");
  }
}

// Open, the input passes bit for bit.
void open_is_unity() {
  const auto count = frames(render_seconds);
  const auto key = burst(count, 0, count, loud);
  const auto input = noise(steady, count);
  const auto out = render(defaults(), input, &key);
  constexpr double opened_seconds = 0.05;
  const auto opened = frames(opened_seconds);
  for (std::size_t i = opened; i < count; ++i) {
    require(out.left[i] == static_cast<double>(static_cast<float>(input.left[i])) &&
                out.right[i] == static_cast<double>(static_cast<float>(input.right[i])),
            "open passes the input untouched");
  }
}

// How often the gate switched over a key whose level wobbles slowly about
// the threshold by less than the hysteresis.
std::size_t switches(const std::vector<double>& values) {
  constexpr double wobble_db = 1.5;
  constexpr double wobble_hz = 3.0;
  const auto count = frames(wobble_seconds);
  Stereo key = silence(count);
  for (std::size_t i = 0; i < count; ++i) {
    const double t = static_cast<double>(i) / rate;
    const double level_db =
        threshold_db + wobble_db * std::cos(2.0 * std::numbers::pi * wobble_hz * t);
    key.left[i] = key.right[i] = gain(level_db) * std::sin(2.0 * std::numbers::pi * tone_hz * t);
  }
  const auto g = gains(render(values, constant(count), &key));
  constexpr double middle = 0.5;
  std::size_t flips = 0;
  bool open = false;
  for (const double x : g) {
    if ((x > middle) != open) {
      open = !open;
      ++flips;
    }
  }
  return flips;
}

void no_chatter_at_the_threshold() {
  auto v = with(defaults(), "hush.threshold", threshold_db);
  v[at("hush.hysteresis")] = hysteresis_db;
  v[at("hush.hold")] = 0.0;
  v[at("hush.attack")] = fastest_attack_ms;
  v[at("hush.release")] = fastest_release_ms;
  REQUIRE_LE(switches(v), 1, "hysteresis: it opens once and stays open");
  // Without hysteresis the same key opens and closes with every wobble, so
  // the oracle can fail.
  v[at("hush.hysteresis")] = 0.0;
  constexpr std::size_t chattering = 4;
  REQUIRE_GT(switches(v), chattering, "without hysteresis it chatters");
}

void attack_hold_and_release_are_timed() {
  constexpr double attack_ms = 10.0;
  constexpr double hold_ms = 50.0;
  constexpr double release_ms = 100.0;
  constexpr double range_db = -60.0;
  auto v = with(defaults(), "hush.range", range_db);
  v[at("hush.threshold")] = threshold_db;
  v[at("hush.hysteresis")] = hysteresis_db;
  v[at("hush.attack")] = attack_ms;
  v[at("hush.hold")] = hold_ms;
  v[at("hush.release")] = release_ms;
  const auto count = frames(long_seconds);
  const auto on = frames(key_on_seconds);
  const auto off = frames(key_off_seconds);
  const auto key = burst(count, on, off, loud);
  const auto g = gains(render(v, constant(count), &key));
  const auto floor = gain(range_db);
  const auto tolerance_frames = millisecond_frames;

  const auto opened = first(g, on, [](double x) { return x >= 1.0; });
  REQUIRE_LT(std::fabs(static_cast<double>(opened - on) - attack_ms * millisecond_frames),
             tolerance_frames, "attack: from the range to unity");
  // The detector falls from the key's peak to the close threshold, then
  // the hold runs, then the release.
  const double decay_frames =
      hush::detector_fall_seconds * rate * std::log(loud / gain(threshold_db - hysteresis_db));
  const auto closing = first(g, off, [](double x) { return x < 1.0; });
  const double expected_closing =
      static_cast<double>(off) + decay_frames + hold_ms * millisecond_frames;
  REQUIRE_LT(std::fabs(static_cast<double>(closing) - expected_closing), tolerance_frames,
             "hold: open until the key has stayed under the close threshold this long");
  const auto closed = first(
      g, closing, [&](double x) { return std::fabs(x - floor) < tolerance::float_round_trip; });
  REQUIRE_LT(std::fabs(static_cast<double>(closed - closing) - release_ms * millisecond_frames),
             tolerance_frames, "release: from unity to the range");
}

// Below the threshold the expander lowers the level ratio - 1 dB for every
// dB the key falls, down to the range.
void the_expander_follows_its_ratio() {
  constexpr double ratio = 3.0;
  constexpr double expander_threshold_db = -30.0;
  constexpr double range_db = -60.0;
  // The detector's ripple on an 8 kHz key, doubled by the ratio.
  constexpr double ripple_db = 0.3;
  auto v = with(defaults(), "hush.mode", static_cast<double>(expander_mode));
  v[at("hush.ratio")] = ratio;
  v[at("hush.threshold")] = expander_threshold_db;
  v[at("hush.range")] = range_db;
  const auto count = frames(render_seconds);
  constexpr std::array keys_db{-35.0, -40.0, -50.0, -70.0};
  for (const double key_db : keys_db) {
    const auto key = burst(count, 0, count, gain(key_db), ripple_free_hz);
    const auto g = gains(render(v, constant(count), &key));
    const double expected = std::max(range_db, (key_db - expander_threshold_db) * (ratio - 1.0));
    for (std::size_t i = count / 2; i < count; ++i) {
      REQUIRE_LT(std::fabs(db(g[i]) - expected), ripple_db,
                 "the static curve at " + std::to_string(key_db) + " dB");
    }
  }
}

// With lookahead the gain is fully open by the time the delayed transient
// reaches the output, and the output is the input exactly that late.
void lookahead_opens_before_the_transient() {
  constexpr double attack_ms = 2.0;
  constexpr std::uint32_t five_ms = 240;
  auto v = with(defaults(), "hush.key", static_cast<double>(input_key));
  v[at("hush.attack")] = attack_ms;
  v[at("hush.lookahead")] = static_cast<double>(lookahead_5ms);
  REQUIRE_EQ(engine().latency_frames(v, rate), five_ms, "the engine reports the lookahead");
  REQUIRE_EQ(hush::latency_frames(lookahead_5ms, rate), five_ms, "5 ms at 48 kHz");
  REQUIRE_EQ(hush::latency_frames(lookahead_off, rate), 0, "none when off");
  const auto count = frames(short_key_off_seconds);
  const auto on = frames(key_on_seconds);
  const auto input = burst(count, on, count, loud);
  const auto out = render(v, input);
  for (std::size_t i = on; i + five_ms < count; ++i) {
    REQUIRE_EQ(out.left[i + five_ms], static_cast<double>(static_cast<float>(input.left[i])),
               "the transient arrives through an open gate, exactly delayed");
  }
  // Without lookahead the same attack swallows the transient's start.
  const auto late = render(with(v, "hush.lookahead", static_cast<double>(lookahead_off)), input);
  const auto start = on + static_cast<std::size_t>(millisecond_frames / 2.0);
  REQUIRE_LT(std::fabs(late.left[start]), std::fabs(input.left[start]),
             "no lookahead: the attack is still opening");
}

// The key filter keeps bleed out: a hat far above a 200 Hz low-pass never
// opens it, a kick inside it does; a high-pass keeps out the lows.
void the_key_filter_keeps_bleed_out() {
  constexpr double hat_hz = 5'000.0;
  constexpr double kick_hz = 60.0;
  constexpr double bleed_level = 0.3;
  constexpr double kick_level = 0.1;
  const auto count = frames(render_seconds);
  const auto low_pass = with(defaults(), "hush.key_lp", kick_key_lp_hz);
  const auto hat = burst(count, 0, count, bleed_level, hat_hz);
  const auto shut = render(low_pass, constant(count), &hat);
  REQUIRE_EQ(peak(shut.left), 0.0, "bleed above the low-pass stays shut");
  const auto kick = burst(count, 0, count, kick_level, kick_hz);
  const auto g = gains(render(low_pass, constant(count), &kick));
  REQUIRE_EQ(g.back(), 1.0, "the kick opens it");
  const auto high_pass = with(defaults(), "hush.key_hp", bleed_key_hp_hz);
  const auto lows = burst(count, 0, count, bleed_level, kick_hz);
  REQUIRE_EQ(peak(render(high_pass, constant(count), &lows).left), 0.0,
             "lows under the high-pass stay shut");
}

// Keyed externally, the input's own level does nothing; set to Input, it
// opens on the same input.
void an_external_key_drives_it() {
  const auto count = frames(render_seconds);
  const auto input = noise(steady, count);
  const auto quiet = silence(count);
  REQUIRE_EQ(peak(render(defaults(), input, &quiet).left), 0.0, "a silent key keeps it shut");
  const auto own =
      render(with(defaults(), "hush.key", static_cast<double>(input_key)), input, &quiet);
  REQUIRE_GT(peak(own.left, count / 2), steady / 2.0, "keyed by the input it opens");
  const auto on = frames(key_on_seconds);
  const auto off = frames(short_key_off_seconds);
  const auto key = burst(count, on, off, loud);
  const auto g = gains(render(defaults(), constant(count), &key));
  REQUIRE_EQ(g[on - 1], 0.0, "shut before the key");
  REQUIRE_EQ(g[(on + off) / 2], 1.0, "open while the key plays");
  REQUIRE_EQ(g.back(), 0.0, "shut after it");
}

// Listen plays the key through its filter, not the input.
void listen_plays_the_filtered_key() {
  constexpr double kept_hz = 5'000.0;
  constexpr double removed_hz = 100.0;
  constexpr double kept_db = 0.1;
  constexpr double removed_db = -60.0;
  const auto count = frames(render_seconds);
  auto v = with(defaults(), "hush.listen", 1.0);
  v[at("hush.key_hp")] = listen_key_hp_hz;
  const auto high = burst(count, 0, count, loud, kept_hz);
  const auto heard = render(v, constant(count), &high);
  const auto settled = count / 2;
  const std::vector<double> tail_in(high.left.begin() + static_cast<std::ptrdiff_t>(settled),
                                    high.left.end());
  const std::vector<double> tail_out(heard.left.begin() + static_cast<std::ptrdiff_t>(settled),
                                     heard.left.end());
  REQUIRE_LT(std::fabs(db(rms(tail_out) / rms(tail_in))), kept_db, "the key's highs are heard");
  const auto low = burst(count, 0, count, loud, removed_hz);
  const auto filtered = render(v, constant(count), &low);
  const std::vector<double> low_out(filtered.left.begin() + static_cast<std::ptrdiff_t>(settled),
                                    filtered.left.end());
  REQUIRE_LT(db(rms(low_out) / loud), removed_db,
             "its lows are filtered, and the input is not heard");
}

// Linked, both channels take the gain of the louder key; unlinked, each its
// own.
void stereo_link_shares_the_gain() {
  const auto count = frames(render_seconds);
  Stereo key = burst(count, 0, count, loud);
  std::ranges::fill(key.right, 0.0);
  const auto linked = render(defaults(), constant(count), &key);
  for (std::size_t i = 0; i < count; ++i) {
    REQUIRE_EQ(linked.left[i], linked.right[i], "linked: one gain for both");
  }
  REQUIRE_EQ(linked.right.back(), steady, "the quiet side opens with the loud");
  const auto separate = render(with(defaults(), "hush.link", 0.0), constant(count), &key);
  REQUIRE_EQ(separate.left.back(), steady, "unlinked: the keyed side opens");
  REQUIRE_EQ(peak(separate.right), 0.0, "unlinked: the silent side stays shut");
}

// Flipped, the key ducks the input by the range and lets go after it.
void flip_ducks() {
  constexpr double range_db = -20.0;
  auto v = with(defaults(), "hush.flip", 1.0);
  v[at("hush.range")] = range_db;
  const auto count = frames(long_seconds);
  const auto on = frames(key_on_seconds);
  const auto off = frames(key_off_seconds);
  const auto key = burst(count, on, off, loud);
  const auto g = gains(render(v, constant(count), &key));
  REQUIRE_EQ(g[on - 1], 1.0, "unity with no key");
  REQUIRE_LT(std::fabs(g[(on + off) / 2] - gain(range_db)), tolerance::float_round_trip,
             "ducked by the range under the key");
  REQUIRE_EQ(g.back(), 1.0, "back to unity after it");
}

// Drum hits every 0.25 s over noise, keyed by themselves.
Stereo drums(std::size_t count) {
  constexpr double hit_hz = 180.0;
  constexpr double decay = 20.0;
  constexpr double bleed = 0.003;
  constexpr double hit_seconds = 0.25;
  const auto period = frames(hit_seconds);
  Stereo out = noise(bleed, count);
  for (std::size_t i = 0; i < count; ++i) {
    const double t = static_cast<double>(i % period) / rate;
    const double x = loud * std::exp(-decay * t) * std::sin(2.0 * std::numbers::pi * hit_hz * t);
    out.left[i] += x;
    out.right[i] += x;
  }
  return out;
}

std::vector<std::vector<double>> varied() {
  std::vector<std::vector<double>> result;
  for (const auto* name : {"Tight Kick", "Snare Bleed", "Soft Expander", "Gated Verb Key"}) {
    result.push_back(with(preset(table(), name), "hush.key", static_cast<double>(input_key)));
  }
  auto ducking = with(defaults(), "hush.flip", 1.0);
  constexpr double half_linked = 0.5;
  ducking[at("hush.link")] = half_linked;
  ducking[at("hush.listen")] = 1.0;
  result.push_back(ducking);
  return result;
}

void any_block_split_renders_the_same() {
  const auto input = drums(frames(long_seconds));
  for (const auto& v : varied()) {
    const auto a = render(v, input, nullptr, oscillate::test::block_128);
    const auto b = render(v, input, nullptr, 0);
    require(a.left == b.left && a.right == b.right, "the split changes nothing");
  }
}

// Export renders in long slices on its own transport generation, stopped;
// live in callback blocks while playing. The audio is the same.
void export_equals_live() {
  constexpr std::size_t export_block = 4'096;
  constexpr double live_bpm = 124.0;
  const auto input = drums(frames(long_seconds));
  for (const auto& v : varied()) {
    auto live = make_effect(engine(), v, rate);
    const auto played =
        process(*live, input, {.block = oscillate::test::block_128, .bpm = live_bpm});
    auto offline = make_effect(engine(), v, rate);
    const auto exported =
        process(*offline, input, {.block = export_block, .generation = 2, .bpm = 0.0});
    require(played.left == exported.left && played.right == exported.right, "export equals live");
  }
}

void hostile_input_stays_finite() {
  constexpr std::size_t nan_at = 100;
  constexpr std::size_t infinity_at = 200;
  constexpr std::size_t huge_at = 300;
  constexpr double huge = 1e30;
  constexpr double tiny = 1e-30;
  constexpr std::size_t tiny_from = 4'000;
  Stereo input = noise(steady, frames(short_seconds));
  input.left[nan_at] = std::numeric_limits<double>::quiet_NaN();
  input.right[infinity_at] = std::numeric_limits<double>::infinity();
  input.left[huge_at] = huge;
  for (std::size_t i = tiny_from; i < input.size(); ++i) {
    input.left[i] = input.right[i] = tiny;
  }
  for (const auto& v : varied()) {
    for (const bool keyed : {false, true}) {
      const auto out = render(v, input, keyed ? &input : nullptr);
      for (std::size_t i = 0; i < out.size(); ++i) {
        require(std::isfinite(out.left[i]) && std::isfinite(out.right[i]), "finite output");
      }
    }
  }
}

// The source each preset is for: a hit at the start of every second (its
// tone, decay and level), and what lies between (noise at `floor`), with
// the key routed separately for Gated Verb Key.
struct Source {
  const char* preset;
  double hz;
  double decay;
  double floor;
  bool keyed;
};

void presets_stay_restrained() {
  constexpr double hit_level = loud;     // -6 dBFS
  constexpr double through_db = 1.0;     // Hits pass within this.
  constexpr double least_cut_db = 3.0;   // Each preset does something...
  constexpr double range_slack_db = 0.5; // ...and never more than its range.
  constexpr double hit_window_seconds = 0.05;
  constexpr double gap_from_seconds = 0.85;
  constexpr std::size_t periods = 3;
  constexpr double room_floor = 0.001; // -60 dBFS
  constexpr double hum_floor = 0.0003; // -70 dBFS: under Hum Gate's close threshold
  constexpr double verb_level = 0.1;   // -20 dBFS
  // Hit tones in hertz and decays per second.
  constexpr double kick_hz = 60.0;
  constexpr double snare_hz = 200.0;
  constexpr double tom_hz = 120.0;
  constexpr double crack_hz = 1'000.0;
  constexpr double note_hz = 440.0;
  constexpr double voice_hz = 300.0;
  constexpr double fast_decay = 20.0;
  constexpr double slow_decay = 15.0;
  const std::array<Source, 7> sources{{
      {"Tight Kick", kick_hz, fast_decay, room_floor, false},
      {"Snare Bleed", snare_hz, fast_decay, room_floor, false},
      {"Tom Bleed", tom_hz, slow_decay, room_floor, false},
      {"Gated Verb Key", crack_hz, fast_decay, verb_level, true},
      {"Hum Gate", note_hz, slow_decay, hum_floor, false},
      {"Soft Expander", voice_hz, slow_decay, room_floor, false},
      {"Vocal Breath", voice_hz, slow_decay, room_floor, false},
  }};
  const auto period = frames(long_seconds);
  const auto count = period * periods;
  for (const auto& source : sources) {
    const auto values = preset(table(), source.preset);
    Stereo hits = silence(count);
    for (std::size_t i = 0; i < count; ++i) {
      const double t = static_cast<double>(i % period) / rate;
      hits.left[i] = hits.right[i] = hit_level * std::exp(-source.decay * t) *
                                     std::sin(2.0 * std::numbers::pi * source.hz * t);
    }
    const auto bed = noise(source.floor, count, false, static_cast<std::uint32_t>(source.hz));
    const Stereo input = source.keyed ? bed : added(hits, bed);
    const auto latency = engine().latency_frames(values, rate);
    auto runtime = make_effect(engine(), values, rate);
    const auto out = process(*runtime, input, {.sidechain = source.keyed ? &hits : nullptr});
    const double range_db = values[at("hush.range")];
    const bool mutes = range_db <= table().parameters[at("hush.range")].minimum;
    for (std::size_t p = 1; p < periods; ++p) {
      const auto start = p * period;
      const auto hit_end = start + frames(hit_window_seconds);
      const double in_peak = peak(input.left, start, hit_end);
      const double out_peak = peak(out.left, start + latency, hit_end + latency);
      REQUIRE_LT(std::fabs(db(out_peak / in_peak)), through_db,
                 std::string(source.preset) + " lets the hit through");
      const auto gap = start + frames(gap_from_seconds);
      const auto end = start + period - latency;
      double in_power = 0.0;
      double out_power = 0.0;
      for (std::size_t i = gap; i < end; ++i) {
        in_power += input.left[i] * input.left[i];
        out_power += out.left[i + latency] * out.left[i + latency];
      }
      if (mutes) {
        REQUIRE_EQ(out_power, 0.0, std::string(source.preset) + " mutes between hits");
        continue;
      }
      const double cut_db = -oscillate::native_dsp::dsp::power_to_db(out_power / in_power);
      REQUIRE_GT(cut_db, least_cut_db, std::string(source.preset) + " lowers the gap");
      REQUIRE_LT(cut_db, -range_db + range_slack_db,
                 std::string(source.preset) +
                     " by no more than its range: " + std::to_string(cut_db));
    }
  }
}

// A seek (a new transport generation) clears every history: after it Hush
// plays as a fresh one.
void a_seek_starts_clean() {
  const auto before = drums(frames(render_seconds));
  constexpr std::uint32_t other_seed = 99;
  const auto after = noise(steady, frames(render_seconds), true, other_seed);
  const auto v = with(preset(table(), "Snare Bleed"), "hush.key", static_cast<double>(input_key));
  auto seeked = make_effect(engine(), v, rate);
  (void)process(*seeked, before);
  const auto resumed = process(*seeked, after, {.generation = 2});
  const auto fresh = render(v, after);
  require(resumed.left == fresh.left && resumed.right == fresh.right,
          "after a seek Hush plays as a fresh one");
}

void the_callback_never_allocates() {
  constexpr std::uint64_t blocks = 2'000;
  constexpr std::uint64_t start = 4'096;
  constexpr float level = 0.9F;
  const auto values = preset(table(), "Tom Bleed");
  auto runtime = make_effect(engine(), values, rate);
  (void)process(*runtime, drums(start));
  auto changed = values;
  changed[at("hush.key_hp")] = listen_key_hp_hz;
  changed[at("hush.key_lp")] = bleed_key_hp_hz;
  changed[at("hush.flip")] = 1.0;
  changed[at("hush.listen")] = 1.0;
  changed[at("hush.mode")] = static_cast<double>(expander_mode);
  require(runtime->offer_values(changed, 1), "new values are offered");
  std::vector<engine::StereoFrame> buffer(oscillate::test::block_128, {level, -level});
  counted_allocations = 0;
  counting_allocations = true;
  for (std::uint64_t block = 0; block < blocks; ++block) {
    constexpr double beats_per_bar = 4.0;
    const Transport transport{0.0, 0.0, 0.0, beats_per_bar};
    (void)runtime->process(buffer, buffer, start + block * buffer.size(),
                           block < blocks / 2 ? 1U : 2U, transport, {});
  }
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "the callback allocates nothing");
}

void telemetry_feeds_the_panel() {
  namespace t = hush::telemetry;
  constexpr float reading_tolerance = 0.5F;
  const auto count = frames(hush::history_seconds);
  const auto on = frames(long_seconds);
  const auto off = frames(wobble_seconds);
  const auto key = burst(count, on, off, loud);
  auto runtime = make_effect(engine(), defaults(), rate);
  (void)process(*runtime, constant(count), {.sidechain = &key});
  std::vector<float> frame;
  REQUIRE_TRUE(runtime->read_telemetry(frame), "a telemetry frame");
  REQUIRE_EQ(frame.size(), t::size, "the layout's size");
  REQUIRE_LT(std::fabs(frame[t::input_peak] - static_cast<float>(db(steady))), reading_tolerance,
             "the input peak");
  REQUIRE_EQ(frame[t::open_threshold], static_cast<float>(threshold_db), "the open threshold");
  REQUIRE_EQ(frame[t::close_threshold], static_cast<float>(threshold_db - hysteresis_db),
             "the close threshold");
  REQUIRE_EQ(frame[t::open], 0.0F, "closed at the end");
  REQUIRE_EQ(frame[t::reduction], static_cast<float>(hush::reduction_floor_db), "muted");
  REQUIRE_EQ(frame[t::latency_ms], 0.0F, "no lookahead");
  // A third of the view open, the rest shut, each slice reading the input.
  std::size_t open_slices = 0;
  for (std::size_t s = 0; s < hush::history_slices; ++s) {
    REQUIRE_LT(std::fabs(frame[t::history_peak + s] - static_cast<float>(steady)),
               tolerance::float_round_trip_f, "each slice's input peak");
    open_slices += frame[t::history_gain + s] == 1.0F ? std::size_t{1} : std::size_t{0};
  }
  constexpr std::size_t thirds = 3;
  constexpr std::size_t slack = 4;
  REQUIRE_GE(open_slices, hush::history_slices / thirds - slack, "the open second");
  REQUIRE_LE(open_slices, hush::history_slices / thirds + slack, "and no more");
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "native hush contract passed",
      {
          {"descriptor_registry_and_presets", [] { descriptor_registry_and_presets(); }},
          {"closed_is_exactly_the_range", [] { closed_is_exactly_the_range(); }},
          {"open_is_unity", [] { open_is_unity(); }},
          {"no_chatter_at_the_threshold", [] { no_chatter_at_the_threshold(); }},
          {"attack_hold_and_release_are_timed", [] { attack_hold_and_release_are_timed(); }},
          {"the_expander_follows_its_ratio", [] { the_expander_follows_its_ratio(); }},
          {"lookahead_opens_before_the_transient", [] { lookahead_opens_before_the_transient(); }},
          {"the_key_filter_keeps_bleed_out", [] { the_key_filter_keeps_bleed_out(); }},
          {"an_external_key_drives_it", [] { an_external_key_drives_it(); }},
          {"listen_plays_the_filtered_key", [] { listen_plays_the_filtered_key(); }},
          {"stereo_link_shares_the_gain", [] { stereo_link_shares_the_gain(); }},
          {"flip_ducks", [] { flip_ducks(); }},
          {"any_block_split_renders_the_same", [] { any_block_split_renders_the_same(); }},
          {"export_equals_live", [] { export_equals_live(); }},
          {"hostile_input_stays_finite", [] { hostile_input_stays_finite(); }},
          {"presets_stay_restrained", [] { presets_stay_restrained(); }},
          {"a_seek_starts_clean", [] { a_seek_starts_clean(); }},
          {"the_callback_never_allocates", [] { the_callback_never_allocates(); }},
          {"telemetry_feeds_the_panel", [] { telemetry_feeds_the_panel(); }},
      });
}
