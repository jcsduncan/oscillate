#include "effect_test_support.hpp"

#include "oscillate/native_dsp/effects/chop.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

// Chop (ADR-0075) against independent oracles: Init is the input; a closed
// step is silence at full depth; every edge crosses half way where the grid
// puts it plus half of Smooth, after a seek and a loop too; ratchets make
// their hits; a repeat replays the step's first slice exactly, and an
// octave down at 12 semitones; chance follows (seed, pattern, step) and an
// export from anywhere matches playback; no edge jumps more than its fade
// allows; swing delays the even steps, rotate shifts the pattern; any
// block split renders the same; the callback never allocates.
namespace {
using namespace effect_test;
namespace chop = oscillate::native_dsp::effects::chop;

const effects::EffectEngine& engine() { return chop::engine(); }
const EngineDescriptor& table() { return *engine().descriptor; }
std::size_t at(std::string_view id) { return index(table(), id); }

constexpr double rate = oscillate::test::sample_rate_48k_hz;
constexpr double bpm = 120.0;
// A 1/16 step at 120 BPM.
constexpr std::size_t step_frames = 6'000;
constexpr std::size_t bar_frames = 16 * step_frames;
// Smooth's default in frames.
constexpr double smooth_frames = 2.9e-3 * rate;

std::string step_id(std::size_t step, std::string_view field) {
  return "chop.step." + std::to_string(step + 1) + "." + std::string(field);
}

std::vector<double> init() { return preset(table(), "Init"); }

void set_all(std::vector<double>& values, std::string_view field, double value) {
  for (std::size_t s = 0; s < chop::max_steps; ++s)
    values[at(step_id(s, field))] = value;
}

Stereo dc(std::size_t frames, double level = 1.0) {
  return {std::vector<double>(frames, level), std::vector<double>(frames, level)};
}

// The input as the runtime receives it: rounded to float.
double as_float(double x) { return static_cast<double>(static_cast<float>(x)); }

// The fractional frame where `x` first rises through `level` at or after
// `from`, or -1.
double rising(const std::vector<double>& x, std::size_t from, std::size_t to, double level) {
  for (std::size_t i = std::max<std::size_t>(from, 1); i < std::min(to, x.size()); ++i) {
    if (x[i - 1] < level && x[i] >= level)
      return static_cast<double>(i - 1) + (level - x[i - 1]) / (x[i] - x[i - 1]);
  }
  return -1.0;
}

std::size_t count_rises(const std::vector<double>& x, std::size_t from, std::size_t to,
                        double level) {
  std::size_t count = 0;
  for (std::size_t i = std::max<std::size_t>(from, 1); i < std::min(to, x.size()); ++i)
    count += x[i - 1] < level && x[i] >= level ? 1U : 0U;
  return count;
}

double max_jump(const std::vector<double>& x) {
  double worst = 0.0;
  for (std::size_t i = 1; i < x.size(); ++i)
    worst = std::max(worst, std::fabs(x[i] - x[i - 1]));
  return worst;
}

void descriptor_registry_and_presets() {
  REQUIRE_EQ(effects::find_engine("chop"), &engine(), "Chop is registered");
  REQUIRE_OK(validate_engine(table()), "the table validates");
  REQUIRE_EQ(table().parameters.size(), 12 + chop::max_steps * 5, "12 globals and 5 per step");
  REQUIRE_EQ(engine().latency_frames(init(), rate), 0, "Chop adds no latency");
  REQUIRE_TRUE(!engine().sidechain, "an insert on the input");
  REQUIRE_TRUE(!engine().generator, "an insert on the input");
  const std::vector<std::string_view> names{
      "Init",     "Off-beat chop", "Rolling 16ths", "Minimal stutter",
      "Dub stab", "Triplet roll",  "Glitch fill"};
  REQUIRE_EQ(table().presets.size(), names.size(), "the factory presets");
  for (std::size_t i = 0; i < names.size(); ++i)
    REQUIRE_EQ(table().presets[i].name, names[i], "preset " + std::string(names[i]));
  REQUIRE_EQ(init(), default_values(table()), "Init is the defaults");
  const auto fill = preset(table(), "Glitch fill");
  REQUIRE_EQ(fill[at("chop.mode")], 2.0, "Glitch fill stutters the last beat of each bar");
  REQUIRE_EQ(fill[at(step_id(15, "repeat"))], 3.0,
             "Glitch fill stutters the last beat of each bar");
  REQUIRE_EQ(fill[at(step_id(31, "repeat"))], 3.0,
             "Glitch fill stutters the last beat of each bar");
}

void init_is_transparent() {
  const auto input = noise(0.5, 3 * bar_frames / 4);
  for (const double mode : {0.0, 1.0, 2.0}) {
    auto values = init();
    values[at("chop.mode")] = mode;
    auto runtime = make_effect(engine(), values, rate);
    const auto out = process(*runtime, input, {.block = 0});
    for (std::size_t i = 0; i < input.size(); ++i) {
      REQUIRE_EQ(out.left[i], as_float(input.left[i]), "Init is the input, bit for bit");
      REQUIRE_EQ(out.right[i], as_float(input.right[i]), "Init is the input, bit for bit");
    }
  }
}

void a_closed_step_is_silent() {
  auto values = init();
  values[at(step_id(1, "level"))] = 0.0;
  auto runtime = make_effect(engine(), values, rate);
  const auto out = process(*runtime, dc(3 * step_frames));
  const auto faded = step_frames + static_cast<std::size_t>(std::ceil(smooth_frames)) + 1;
  for (std::size_t i = faded; i < 2 * step_frames; ++i)
    require(out.left[i] == 0.0 && out.right[i] == 0.0, "a closed step is silence at depth 1");
  for (std::size_t i = 0; i < step_frames; ++i)
    REQUIRE_EQ(out.left[i], 1.0, "the open step before it is untouched");
  // Half depth stops half way.
  values[at("chop.depth")] = 0.5;
  auto half = make_effect(engine(), values, rate);
  const auto halved = process(*half, dc(3 * step_frames));
  REQUIRE_EQ(halved.left[step_frames + step_frames / 2], 0.5, "depth sets how far it closes");
  // A step whose chance is 0 is closed too.
  values = init();
  values[at(step_id(1, "chance"))] = 0.0;
  auto never = make_effect(engine(), values, rate);
  REQUIRE_EQ(process(*never, dc(3 * step_frames)).left[step_frames + step_frames / 2], 0.0,
             "a step that never plays is closed");
}

// Odd steps closed: every even step opens on its grid line.
void edges_land_on_the_grid() {
  auto values = init();
  for (std::size_t s = 1; s < chop::max_steps; s += 2)
    values[at(step_id(s, "level"))] = 0.0;
  // 124 BPM: steps of 5806.45 frames, so edges fall between frames.
  constexpr double tempo = 124.0;
  const double step = 0.25 * 60.0 / tempo * rate;
  const auto check = [&](const Stereo& out, std::uint64_t start, std::size_t first_step,
                         std::size_t last_step, std::string_view what) {
    for (std::size_t k = first_step; k <= last_step; k += 2) {
      const double edge = static_cast<double>(k) * step - static_cast<double>(start);
      const auto from = static_cast<std::size_t>(std::max(0.0, edge - 10.0));
      const double crossed = rising(out.left, from, from + step_frames, 0.5);
      const double error = crossed - (edge + 0.5 * smooth_frames);
      require(crossed >= 0.0 && std::fabs(error) < 0.1,
              std::string(what) + ": step " + std::to_string(k) + " opens " +
                  std::to_string(error) + " frames off the grid");
    }
  };
  const std::size_t frames = 12 * step_frames;
  auto runtime = make_effect(engine(), values, rate);
  check(process(*runtime, dc(frames), {.bpm = tempo}), 0, 2, 10, "from the start");

  // A seek into the middle of step 7 (closed), on a new generation.
  const std::uint64_t seek = 7 * static_cast<std::uint64_t>(step) + 1'234;
  check(process(*runtime, dc(frames), {.generation = 2, .bpm = tempo}, seek), seek, 8, 18,
        "after a seek");

  // A loop back to the start of step 2: the transport jumps without a new
  // generation (a whole control tick, so the first tick sees the jump).
  const std::uint64_t loop = 2 * 5'824;
  check(process(*runtime, dc(frames), {.generation = 2, .bpm = tempo}, loop), loop, 4, 12,
        "across a loop");
}

void ratchets_make_their_hits() {
  for (std::size_t hits = 1; hits <= 4; ++hits) {
    auto values = init();
    set_all(values, "level", 0.0);
    values[at(step_id(2, "level"))] = 1.0;
    values[at(step_id(2, "length"))] = 0.5;
    values[at(step_id(2, "ratchet"))] = static_cast<double>(hits);
    auto runtime = make_effect(engine(), values, rate);
    const auto out = process(*runtime, dc(4 * step_frames));
    REQUIRE_EQ(count_rises(out.left, step_frames, 4 * step_frames, 0.5), hits,
               std::to_string(hits) + " ratchets make " + std::to_string(hits) + " onsets");
    const double sub = static_cast<double>(step_frames) / static_cast<double>(hits);
    for (std::size_t h = 0; h < hits; ++h) {
      const double edge = 2.0 * static_cast<double>(step_frames) + static_cast<double>(h) * sub;
      const double crossed =
          rising(out.left, static_cast<std::size_t>(edge) - 10, 4 * step_frames, 0.5);
      REQUIRE_LT(std::fabs(crossed - (edge + 0.5 * smooth_frames)), 0.1, "each hit on its share");
    }
  }
}

void repeats_replay_the_slice() {
  // Repeat mode, step 3 twice: its second half is its first half again.
  auto values = init();
  values[at("chop.mode")] = 1.0;
  values[at(step_id(2, "repeat"))] = 1.0;
  set_all(values, "level", 0.0); // Repeat mode does not gate.
  auto runtime = make_effect(engine(), values, rate);
  const auto input = noise(0.4, 4 * step_frames, true);
  const auto out = process(*runtime, input);
  const std::size_t start = 2 * step_frames;
  const std::size_t slice = step_frames / 2;
  const auto cross = static_cast<std::size_t>(std::ceil(smooth_frames));
  for (std::size_t t = 0; t < slice; ++t)
    REQUIRE_EQ(out.left[start + t], as_float(input.left[start + t]), "the first slice is live");
  double error = 0.0;
  for (std::size_t t = cross; t < slice; ++t)
    error =
        std::max(error, std::fabs(out.left[start + slice + t] - as_float(input.left[start + t])));
  REQUIRE_LT(error, 1e-6, "the repeat replays the step's first slice: " + std::to_string(error));
  for (std::size_t i = start + step_frames + cross; i < 4 * step_frames; ++i)
    REQUIRE_EQ(out.left[i], as_float(input.left[i]), "the next step plays through ungated");

  // Twelve semitones down: the replay of a 440 Hz tone is 220 Hz.
  values[at("chop.repeat_pitch")] = 12.0;
  auto lower = make_effect(engine(), values, rate);
  const auto tone = sine(440.0, 0.5, 4 * step_frames);
  const auto dropped = process(*lower, tone);
  const double hz = measured_hz(dropped.left, start + slice + cross, start + step_frames, rate);
  REQUIRE_LT(std::fabs(cents(hz, 220.0)), 10.0,
             "the repeat drops an octave: " + std::to_string(hz));

  // Eight repeats with decay 7/8: the last is an eighth of the level.
  values[at("chop.repeat_pitch")] = 0.0;
  values[at(step_id(2, "repeat"))] = 3.0;
  values[at("chop.repeat_decay")] = 0.875;
  auto fading = make_effect(engine(), values, rate);
  const auto faded = process(*fading, tone);
  const std::size_t eighth = step_frames / 8;
  const double last = rms(faded.left, start + 7 * eighth + cross, start + step_frames);
  const double first = rms(tone.left, start + cross, start + eighth);
  REQUIRE_LT(std::fabs(last / first - 0.125), 0.01, "decay lowers each repeat in turn");
}

void chance_follows_seed_bar_and_step() {
  REQUIRE_EQ(chop::chance_roll(7, 3, 5), chop::chance_roll(7, 3, 5), "a roll is a pure function");
  std::size_t below = 0;
  constexpr std::size_t rolls = 20'000;
  for (std::size_t i = 0; i < rolls; ++i)
    below += chop::chance_roll(7, static_cast<std::int64_t>(i / 32), i % 32) < 0.5 ? 1U : 0U;
  REQUIRE_LT(std::fabs(static_cast<double>(below) / static_cast<double>(rolls) - 0.5), 0.02,
             "rolls are uniform");

  auto values = init();
  set_all(values, "chance", 0.5);
  constexpr std::size_t bars = 4;
  const auto render = [&](std::uint32_t seed) {
    values[at("chop.seed")] = static_cast<double>(seed);
    auto runtime = make_effect(engine(), values, rate);
    return process(*runtime, dc(bars * bar_frames), {.block = 0});
  };
  const auto out = render(7);
  for (std::size_t bar = 0; bar < bars; ++bar) {
    for (std::size_t s = 0; s < 16; ++s) {
      const bool plays = chop::chance_roll(7, static_cast<std::int64_t>(bar), s) < 0.5;
      const double middle = out.left[bar * bar_frames + s * step_frames + step_frames / 2];
      REQUIRE_EQ(middle, (plays ? 1.0 : 0.0), "each step plays as its (seed, bar, step) roll says");
    }
  }
  REQUIRE_EQ(render(7).left, out.left, "two renders are the same");
  REQUIRE_NE(render(8).left, out.left, "another seed rolls another variation");

  // An export from bar 2 sounds as playback did there, a step in (the
  // first step fades in from where the gain was).
  values[at("chop.seed")] = 7.0;
  auto exported = make_effect(engine(), values, rate);
  const std::uint64_t from = 2 * bar_frames;
  const auto part = process(*exported, dc(2 * bar_frames), {.generation = 2}, from);
  // Its song position is anchored at bar 2 rather than at the start, so
  // the two agree to rounding.
  for (std::size_t i = step_frames; i < part.size(); ++i)
    REQUIRE_LT(std::fabs(part.left[i] - out.left[from + i]), tolerance::float_round_trip,
               "an export from bar 2 matches playback");
}

void edges_never_click() {
  constexpr double amplitude = 0.5;
  const auto bass = sine(55.0, amplitude, 2 * bar_frames);
  const double input_jump = max_jump(bass.left);
  const auto bound = [&](double smooth) {
    return 2.0 * input_jump + 1.5 * amplitude * std::numbers::pi / smooth;
  };
  for (const auto& entry : table().presets) {
    auto runtime = make_effect(engine(), preset(table(), entry.name), rate);
    const auto out = process(*runtime, bass);
    const double jump = std::max(max_jump(out.left), max_jump(out.right));
    REQUIRE_LE(jump, bound(smooth_frames),
               std::string(entry.name) + " jumps " + std::to_string(jump / amplitude) +
                   " of the signal in a frame");
  }
  // 1/128 ratchets, half open, at the shortest Smooth.
  auto values = init();
  values[at("chop.division")] = 2.0;
  values[at("chop.smooth")] = 0.5;
  set_all(values, "ratchet", 4.0);
  set_all(values, "length", 0.5);
  auto fast = make_effect(engine(), values, rate);
  const auto out = process(*fast, bass);
  REQUIRE_LE(max_jump(out.left), bound(0.5e-3 * rate), "even 1/128 ratchets fade every edge");
  REQUIRE_GT(rms(out.left, 0, bar_frames), 0.1, "and they sound");
}

void swing_delays_the_even_steps() {
  auto values = init();
  for (std::size_t s = 1; s < chop::max_steps; s += 2)
    values[at(step_id(s, "level"))] = 0.5;
  auto straight = make_effect(engine(), values, rate);
  const auto even = process(*straight, dc(4 * step_frames));
  REQUIRE_EQ(even.left[step_frames + step_frames / 3], 0.5, "straight: step 2 starts on its line");
  values[at("chop.swing")] = 1.0;
  auto swung = make_effect(engine(), values, rate);
  const auto late = process(*swung, dc(4 * step_frames));
  REQUIRE_EQ(late.left[step_frames + step_frames / 3], 1.0, "75 % swing holds step 1 longer");
  const std::size_t swung_edge = step_frames * 3 / 2;
  REQUIRE_EQ(late.left[swung_edge - 100], 1.0, "step 2 starts half a step late");
  REQUIRE_EQ(late.left[swung_edge + 300], 0.5, "step 2 starts half a step late");
  REQUIRE_EQ(late.left[2 * step_frames + step_frames / 2], 1.0, "and step 3 is back on the grid");
}

void rotate_shifts_the_pattern() {
  auto values = init();
  values[at(step_id(0, "level"))] = 0.0;
  values[at("chop.rotate")] = 3.0;
  auto runtime = make_effect(engine(), values, rate);
  const auto out = process(*runtime, dc(bar_frames));
  REQUIRE_EQ(out.left[step_frames / 2], 1.0, "slot 1 plays another step");
  REQUIRE_EQ(out.left[3 * step_frames + step_frames / 2], 0.0, "step 1 sounds three steps later");
  std::vector<float> telemetry;
  REQUIRE_TRUE(runtime->read_telemetry(telemetry), "the panel hears which step plays where");
  REQUIRE_EQ(telemetry[chop::telemetry::slot], 15.0F, "the panel hears which step plays where");
  REQUIRE_EQ(telemetry[chop::telemetry::step], 12.0F, "the panel hears which step plays where");
}

void any_block_split_renders_the_same() {
  auto values = preset(table(), "Glitch fill");
  values[at("chop.swing")] = 0.4;
  set_all(values, "chance", 0.7);
  const auto input = noise(0.3, 2 * bar_frames);
  for (const std::uint64_t start : {std::uint64_t{0}, std::uint64_t{7 * step_frames + 999}}) {
    auto a = make_effect(engine(), values, rate);
    auto b = make_effect(engine(), values, rate);
    auto c = make_effect(engine(), values, rate);
    const auto x = process(*a, input, {.block = 128}, start);
    const auto y = process(*b, input, {.block = 0}, start);
    const auto z = process(*c, input, {.block = 1'024}, start);
    REQUIRE_EQ(x.left, y.left, "any block split renders the same");
    REQUIRE_EQ(x.right, y.right, "any block split renders the same");
    REQUIRE_EQ(x.left, z.left, "any block split renders the same");
    REQUIRE_EQ(x.right, z.right, "any block split renders the same");
  }
}

void the_callback_never_allocates() {
  const auto values = preset(table(), "Glitch fill");
  auto runtime = make_effect(engine(), values, rate);
  (void)process(*runtime, noise(0.3, 4'096));
  auto changed = values;
  changed[at("chop.steps")] = 1.0;
  changed[at("chop.division")] = 0.0;
  changed[at("chop.swing")] = 0.5;
  require(runtime->offer_values(changed, 1), "a new pattern is offered");
  std::vector<engine::StereoFrame> buffer(128, {0.2F, -0.2F});
  const double beats_per_frame = bpm / 60.0 / rate;
  counted_allocations = 0;
  counting_allocations = true;
  for (std::uint64_t block = 0; block < 2'000; ++block) {
    const std::uint64_t frame = 4'096 + block * buffer.size();
    const double beat = static_cast<double>(frame) * beats_per_frame;
    const SynthTransport transport{beat, beats_per_frame, beat / 4.0, 4.0};
    (void)runtime->process(buffer, {}, frame, block < 1'000 ? 1U : 2U, transport, {});
  }
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "the callback allocates nothing");
}

void telemetry_feeds_the_panel() {
  auto values = init();
  set_all(values, "chance", 0.5);
  values[at(step_id(0, "level"))] = 0.0;
  auto runtime = make_effect(engine(), values, rate);
  (void)process(*runtime, sine(55.0, 0.5, 2 * bar_frames + bar_frames / 4));
  std::vector<float> frame;
  REQUIRE_TRUE(runtime->read_telemetry(frame), "a telemetry frame is published");
  REQUIRE_EQ(frame.size(), chop::telemetry::size, "a telemetry frame is published");
  REQUIRE_GE(frame[chop::telemetry::phase], 0.24F, "the playhead is a quarter into the bar");
  REQUIRE_LT(frame[chop::telemetry::phase], 0.26F, "the playhead is a quarter into the bar");
  REQUIRE_EQ(frame[chop::telemetry::pattern_beats], 4.0F,
             "16 sixteenths: a four-beat pattern, the third");
  REQUIRE_EQ(frame[chop::telemetry::pattern], 2.0F,
             "16 sixteenths: a four-beat pattern, the third");
  for (std::size_t s = 0; s < chop::max_steps; ++s) {
    const bool plays = s < 16 && chop::chance_roll(7, 2, s) < 0.5;
    REQUIRE_EQ((frame[chop::telemetry::played + s] == 1.0F), plays, "the played steps are shown");
  }
  const auto input = std::span(frame).subspan(chop::telemetry::input, chop::view_points);
  const auto gain = std::span(frame).subspan(chop::telemetry::gain, chop::view_points);
  REQUIRE_GT(*std::ranges::max_element(input), 0.45F, "the last bar's input is captured");
  REQUIRE_LT(*std::ranges::min_element(gain), 0.01F, "the gate's gain is captured");
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "native chop contract passed",
      {
          {"descriptor_registry_and_presets", [] { descriptor_registry_and_presets(); }},
          {"init_is_transparent", [] { init_is_transparent(); }},
          {"a_closed_step_is_silent", [] { a_closed_step_is_silent(); }},
          {"edges_land_on_the_grid", [] { edges_land_on_the_grid(); }},
          {"ratchets_make_their_hits", [] { ratchets_make_their_hits(); }},
          {"repeats_replay_the_slice", [] { repeats_replay_the_slice(); }},
          {"chance_follows_seed_bar_and_step", [] { chance_follows_seed_bar_and_step(); }},
          {"edges_never_click", [] { edges_never_click(); }},
          {"swing_delays_the_even_steps", [] { swing_delays_the_even_steps(); }},
          {"rotate_shifts_the_pattern", [] { rotate_shifts_the_pattern(); }},
          {"any_block_split_renders_the_same", [] { any_block_split_renders_the_same(); }},
          {"the_callback_never_allocates", [] { the_callback_never_allocates(); }},
          {"telemetry_feeds_the_panel", [] { telemetry_feeds_the_panel(); }},
      });
}
