#include "oscillate/native_dsp/synth/ember.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"
#include "synth_test_support.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {
using namespace synth_test;

const SynthEngine& ember_engine() { return ember::engine(); }
const EngineDescriptor& table() { return *ember_engine().descriptor; }
std::size_t at(std::string_view id) { return index(table(), id); }
std::vector<double> patch(std::string_view name) { return preset(table(), name); }

// A clean triangle on VCO B: DCO silent, filter wide open, no effects, no
// age, a calibrated circuit that leaks nothing.
std::vector<double> clean_patch(double age = 0.0) {
  auto values = patch("Init");
  values[at("ember.dco.saw")] = 0;
  values[at("ember.dco.sub")] = 0;
  values[at("ember.vco.wave")] = 2;
  values[at("ember.vco.level")] = 1;
  values[at("ember.vco.fine")] = 0;
  values[at("ember.lfo.pitch")] = 0;
  values[at("ember.age")] = age;
  values[at("ember.circuit.tuning")] = 0;
  values[at("ember.circuit.bleed")] = 0;
  values[at("ember.circuit.hum")] = 0;
  return values;
}

std::vector<float> telemetry_after(NativeSynthRuntime& runtime) {
  std::vector<float> frame;
  REQUIRE_TRUE(runtime.read_telemetry(frame), "a telemetry frame is published");
  REQUIRE_EQ(frame.size(), ember::telemetry::size, "a telemetry frame is published");
  return frame;
}

// ---- The circuit (ADR-0066) ------------------------------------------------

void circuit_laws_hold() {
  namespace c = ember_circuit;
  // DCO A lands on clock / N: within half a division, and exact on one.
  for (const double hz : {65.4, 440.0, 4'186.0}) {
    const double n = std::round(c::dco_clock_hz / hz);
    const double bound = 1'200.0 * std::log2(1.0 + 0.5 / n) + 1e-9;
    REQUIRE_LE(std::fabs(cents(c::dco_hz(hz), hz)), bound, "DCO within half a clock division");
  }
  REQUIRE_EQ(c::dco_hz(c::dco_clock_hz / 1'000.0), c::dco_clock_hz / 1'000.0,
             "an exact division is exact");
  // VCO B: tracking is linear in octaves from middle C; droop and reset pull
  // the top flat; a calibrated card is ideal.
  c::Card tracking{};
  tracking.tracking = 2.0;
  REQUIRE_LT(std::fabs(cents(c::vco_hz(1'046.5, 84.0, tracking), 1'046.5) - 4.0),
             tolerance::float_round_trip, "2 cents per octave is 4 cents two octaves up");
  c::Card reset{};
  reset.reset = 1.5e-6;
  REQUIRE_LT(std::fabs(c::vco_hz(4'000.0, 107.0, reset) - 4'000.0 / (1.0 + 4'000.0 * 1.5e-6)),
             tolerance::double_accumulation, "the core's reset adds to every period");
  REQUIRE_EQ(c::vco_hz(440.0, 69.0, c::Card{}), 440.0, "a calibrated card plays in tune");
  // The bowed ramp keeps its ends and stays monotonic.
  for (const double k : {0.0, 0.2, 0.5}) {
    REQUIRE_LT(std::fabs(c::bow(-1.0, k) + 1.0), tolerance::double_rounding,
               "the bowed ramp keeps its ends");
    REQUIRE_LT(std::fabs(c::bow(1.0, k) - 1.0), tolerance::double_rounding,
               "the bowed ramp keeps its ends");
    double last = -2.0;
    for (int i = -10; i <= 10; ++i) {
      const double y = c::bow(i / 10.0, k);
      REQUIRE_GT(y, last, "the bowed ramp rises");
      last = y;
    }
  }
  // Units are different instruments; the same unit is the same instrument.
  REQUIRE_NE(c::card(1, 3, 1.0, 1.0, 1.0).tracking, c::card(2, 3, 1.0, 1.0, 1.0).tracking,
             "units differ");
  REQUIRE_EQ(c::card(5, 3, 1.0, 1.0, 1.0).tracking, c::card(5, 3, 1.0, 1.0, 1.0).tracking,
             "a unit is repeatable");
}

void the_dco_holds_while_the_vco_wanders() {
  auto values = clean_patch(1.0);
  values[at("ember.circuit.tuning")] = 1.0;
  auto runtime = make_runtime(ember_engine(), values);
  auto events = note(0, 240'000, 69, 1);
  std::vector<float> early;
  std::vector<float> late;
  (void)render(*runtime, events, 48'000);
  early = telemetry_after(*runtime);
  events.clear();
  (void)render(*runtime, events, 144'000);
  late = telemetry_after(*runtime);
  const auto vco = ember::telemetry::card_vco_cents;
  const auto dco = ember::telemetry::card_dco_cents;
  REQUIRE_EQ(early[dco], late[dco],
             "the crystal DCO sits within a twentieth of a cent at A4 and never moves");
  REQUIRE_LT(std::fabs(early[dco]), 0.05,
             "the crystal DCO sits within a twentieth of a cent at A4 and never moves");
  REQUIRE_GT(std::fabs(late[vco] - early[vco]), 0.1,
             "VCO B wanders (" + std::to_string(early[vco]) + " -> " + std::to_string(late[vco]) +
                 " cents)");
  REQUIRE_LT(std::fabs(late[vco]), 25.0, "but stays within a gentle detune");
}

void the_vco_goes_flat_at_the_top() {
  const auto error_at = [](std::uint8_t key) {
    auto values = clean_patch(0.0);
    values[at("ember.circuit.tuning")] = 1.0;
    values[at("ember.vco.semi")] = 0;
    auto runtime = make_runtime(ember_engine(), values);
    const auto out = render(*runtime, note(0, 48'000, key, 1), 48'000);
    const double expected = 440.0 * std::exp2((key - 69.0) / 12.0);
    return cents(measured_hz(out.left, 12'000, 48'000, 48'000.0), expected);
  };
  const double middle = error_at(60);
  const double top = error_at(103);
  REQUIRE_LT(top, middle - 5.0,
             "an uncalibrated VCO is several cents flat three octaves up (" +
                 std::to_string(middle) + " -> " + std::to_string(top) + ")");
}

void the_supply_sags_under_a_big_chord() {
  auto values = clean_patch(1.0);
  values[at("ember.amp.sustain")] = 1.0;
  const auto vco_cents = [&](std::uint8_t voices) {
    auto runtime = make_runtime(ember_engine(), values);
    std::vector<Event> events;
    for (std::uint8_t v = 0; v < voices; ++v) {
      auto more =
          note(0, 96'000, static_cast<std::uint8_t>(48 + 3 * v), static_cast<std::uint32_t>(v + 1));
      events.insert(events.end(), more.begin(), more.end());
    }
    (void)render(*runtime, events, 48'000);
    return static_cast<double>(telemetry_after(*runtime)[ember::telemetry::card_vco_cents]);
  };
  const double alone = vco_cents(1);
  const double loaded = vco_cents(8);
  REQUIRE_LT(loaded, alone - 1.0,
             "eight voices flatten the first card's VCO (" + std::to_string(alone) + " -> " +
                 std::to_string(loaded) + ")");
}

void bleed_leaks_and_then_lets_go() {
  auto quiet = clean_patch(0.0);
  quiet[at("ember.vco.level")] = 0;
  const auto rms_of = [](const std::vector<double>& x, std::size_t from, std::size_t to) {
    double sum = 0.0;
    for (std::size_t i = from; i < to; ++i) {
      sum += x[i] * x[i];
    }
    return std::sqrt(sum / static_cast<double>(to - from));
  };
  {
    auto runtime = make_runtime(ember_engine(), quiet);
    const auto out = render(*runtime, note(0, 24'000, 57, 1), 24'000);
    REQUIRE_LT(rms_of(out.left, 4'800, 24'000), 1e-6,
               "with no bleed, silent oscillators are silent");
  }
  auto loud = clean_patch(0.0);
  auto runtime = make_runtime(ember_engine(), loud);
  const auto full = render(*runtime, note(0, 24'000, 57, 1), 24'000);
  quiet[at("ember.circuit.bleed")] = 1.0;
  auto leaky = make_runtime(ember_engine(), quiet);
  const auto leaked = render(*leaky, note(0, 24'000, 57, 1), 72'000);
  const double ratio =
      20.0 * std::log10(rms_of(leaked.left, 4'800, 24'000) / rms_of(full.left, 4'800, 24'000));
  require(ratio > -66.0 && ratio < -45.0,
          "full bleed leaks the oscillators about 60 dB down (" + std::to_string(ratio) + " dB)");
  REQUIRE_EQ(leaked.last.active_voices, 0, "and the voice lets go once its leak has faded");
}

void hum_sits_at_the_mains() {
  const auto at_hz = [](const std::vector<double>& x, double hz) {
    double re = 0.0;
    double im = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
      const double a = 2.0 * 3.141592653589793 * hz * static_cast<double>(i) / 48'000.0;
      re += x[i] * std::cos(a);
      im += x[i] * std::sin(a);
    }
    return std::hypot(re, im);
  };
  auto values = clean_patch(0.0);
  values[at("ember.vco.level")] = 0;
  values[at("ember.circuit.hum")] = 1.0;
  for (const int mains : {0, 1}) {
    values[at("ember.circuit.mains")] = mains;
    auto runtime = make_runtime(ember_engine(), values);
    const auto out = render(*runtime, note(0, 48'000, 57, 1), 48'000);
    const std::vector<double> held(out.left.begin() + 9'600, out.left.end());
    const double fifty = at_hz(held, 50.0);
    const double sixty = at_hz(held, 60.0);
    require(mains == 0 ? fifty > 10.0 * sixty : sixty > 10.0 * fifty,
            "the hum sits at the chosen mains frequency");
  }
  values[at("ember.circuit.hum")] = 0.0;
  auto runtime = make_runtime(ember_engine(), values);
  const auto out = render(*runtime, note(0, 48'000, 57, 1), 48'000);
  REQUIRE_LT(at_hz(std::vector<double>(out.left.begin() + 9'600, out.left.end()), 50.0), 1e-6,
             "no hum at zero");
}

void descriptor_registry_and_presets() {
  const auto valid = validate_engine(table());
  REQUIRE_OK(valid, valid.has_value() ? "" : valid.error().message);
  REQUIRE_EQ(table().id, "ember", "engine identity is ember v1");
  REQUIRE_EQ(table().version, 1, "engine identity is ember v1");
  REQUIRE_EQ(table().presets.size(), 32, "thirty-two factory presets");
  REQUIRE_EQ(find_engine("ember"), &ember_engine(), "Ember registers beside Prism");
  REQUIRE_NE(find_engine("prism"), nullptr, "Ember registers beside Prism");
  for (const auto& parameter : table().parameters) {
    require(parameter.id.starts_with("ember."), "ids are namespaced");
  }
}

void pitch_is_exact_without_age() {
  for (double rate : {44'100.0, 48'000.0, 96'000.0}) {
    for (std::uint8_t key : {std::uint8_t{45}, std::uint8_t{69}, std::uint8_t{93}}) {
      auto runtime = make_runtime(ember_engine(), clean_patch(), rate);
      const auto frames = static_cast<std::size_t>(rate);
      const auto out = render(*runtime, note(0, frames, key, 1), frames, 128, rate);
      const double expected = 440.0 * std::exp2((key - 69.0) / 12.0);
      REQUIRE_LT(std::fabs(cents(measured_hz(out.left, frames / 4, frames, rate), expected)), 0.5,
                 "pitch within 0.5 cent at age zero");
    }
  }
}

double second_voice_pitch(double age) {
  // The first note takes voice card 0; the second lands on card 1.
  auto runtime = make_runtime(ember_engine(), clean_patch(age));
  auto events = note(0, 2'000, 40, 1);
  auto played = note(2'400, 96'000, 69, 2);
  events.insert(events.end(), played.begin(), played.end());
  const auto out = render(*runtime, events, 96'000);
  return measured_hz(out.left, 24'000, 96'000, 48'000.0);
}

void age_is_deterministic_and_makes_voices_differ() {
  for (double age : {0.0, 0.6, 1.0}) {
    auto values = patch("Age Max");
    values[at("ember.age")] = age;
    auto a = make_runtime(ember_engine(), values);
    auto b = make_runtime(ember_engine(), values);
    auto chord = note(0, 30'000, 48, 1);
    auto more = note(500, 30'000, 55, 2);
    chord.insert(chord.end(), more.begin(), more.end());
    const auto x = render(*a, chord, 48'000, 64);
    const auto y = render(*b, chord, 48'000, 0);
    REQUIRE_EQ(x.left, y.left, "any Age renders identically across runs and block splits");
    REQUIRE_EQ(x.right, y.right, "any Age renders identically across runs and block splits");
  }
  const double clean = second_voice_pitch(0.0);
  const double aged = second_voice_pitch(1.0);
  REQUIRE_LT(std::fabs(cents(clean, 440.0)), 0.5, "an unaged voice card is in tune");
  const double offset = std::fabs(cents(aged, 440.0));
  REQUIRE_GT(offset, 0.3, "an aged voice card is audibly but gently detuned");
  REQUIRE_LT(offset, 25.0, "an aged voice card is audibly but gently detuned");
}

void chord_and_unison_modes() {
  auto values = patch("Chord Stab"); // Minor 7th shape.
  auto runtime = make_runtime(ember_engine(), values);
  auto out = render(*runtime, note(0, 24'000, 60, 1), 12'000);
  REQUIRE_EQ(out.last.active_voices, 4, "a minor-7th chord sounds four voices from one key");

  values = patch("Unison Lead");
  runtime = make_runtime(ember_engine(), values);
  auto events = note(0, 24'000, 57, 1);
  auto legato = note(12'000, 24'000, 64, 2);
  events.insert(events.end(), legato.begin(), legato.end());
  out = render(*runtime, events, 30'000);
  REQUIRE_EQ(out.last.active_voices, ember::voice_limit, "unison stacks every voice card");
  double width = 0.0;
  for (std::size_t i = 6'000; i < 12'000; ++i) {
    width += std::fabs(out.left[i] - out.right[i]);
  }
  REQUIRE_GT(width, 1.0, "unison spreads the voice cards across the stereo field");
}

void mode_changes_keep_note_offs_with_their_voices() {
  auto values = clean_patch();
  values[at("ember.voice.mode")] = 1; // Unison.
  values[at("ember.amp.mode")] = 0;   // A short shared release makes held notes observable.
  values[at("ember.env.release")] = 0.002;
  values[at("ember.fx.chorus")] = 0;
  values[at("ember.fx.tape")] = 0;

  const auto event = [](engine::MidiEventKind kind, std::uint64_t id, std::uint8_t key) {
    engine::BlockMidiEvent midi;
    midi.clip_id_bits = {1, 1};
    midi.note_id_bits = {0, id};
    midi.kind = kind;
    midi.key = key;
    midi.velocity = kind == engine::MidiEventKind::note_on ? 100 : 0;
    return midi;
  };
  const auto unison_on = event(engine::MidiEventKind::note_on, 1, 60);
  const auto unison_off = event(engine::MidiEventKind::note_off, 1, 60);
  const auto poly_on = event(engine::MidiEventKind::note_on, 2, 67);
  const auto poly_off = event(engine::MidiEventKind::note_off, 2, 67);

  for (const bool release_poly_first : {true, false}) {
    auto runtime = make_runtime(ember_engine(), values);
    std::uint64_t frame = 0;
    const auto block = [&](std::span<const engine::BlockMidiEvent> midi) {
      std::array<engine::StereoFrame, 128> audio{};
      const auto report = runtime->process(audio, midi, frame, 1, {}, {});
      frame += audio.size();
      REQUIRE_EQ(report.status, SynthProcessStatus::rendered, "mode-switch block renders");
      return report.kernel.active_voices;
    };
    (void)block(std::span(&unison_on, 1));
    values[at("ember.voice.mode")] = 0; // New notes use Poly; old unison voices remain.
    require(runtime->offer_values(values, 1), "live mode change is accepted");
    (void)block(std::span(&poly_on, 1));

    const auto first_off = release_poly_first ? poly_off : unison_off;
    const auto second_off = release_poly_first ? unison_off : poly_off;
    (void)block(std::span(&first_off, 1));
    std::uint32_t active = 0;
    for (int i = 0; i < 100; ++i) {
      active = block({});
    }
    REQUIRE_EQ(active, (release_poly_first ? ember::voice_limit - 1 : 1),
               "releasing one voicing leaves the other sounding");
    (void)block(std::span(&second_off, 1));
    for (int i = 0; i < 100; ++i) {
      active = block({});
    }
    REQUIRE_EQ(active, 0, "both voicings release after their own note-offs");
    values[at("ember.voice.mode")] = 1;
  }
}

void polyphony_round_robin_and_steals() {
  auto runtime = make_runtime(ember_engine(), clean_patch());
  std::vector<Event> events;
  for (std::uint64_t i = 0; i < 10; ++i) {
    auto n = note(i * 480, 40'000, static_cast<std::uint8_t>(48 + i), i + 1);
    events.insert(events.end(), n.begin(), n.end());
  }
  const auto out = render(*runtime, events, 12'000);
  REQUIRE_EQ(out.last.active_voices, ember::voice_limit, "eight-voice polyphony");
  REQUIRE_EQ(out.last.voice_steals, 2, "the ninth and tenth notes steal");
}

// A filter-type change reaches the sounding note (ADR-0038): no deferral,
// and after the crossfade the note plays through the other topology.
void filter_type_changes_apply_live() {
  const auto values = patch("Ember Default");
  auto changed = values;
  changed[at("ember.filter.type")] = 1; // SEM 12.
  auto kept = make_runtime(ember_engine(), values);
  auto switched = make_runtime(ember_engine(), values);
  (void)render(*kept, note(0, 96'000, 57, 1), 12'000);
  (void)render(*switched, note(0, 96'000, 57, 1), 12'000);
  require(switched->offer_values(changed, 1), "filter change offered");
  std::vector<engine::StereoFrame> a(128);
  std::vector<engine::StereoFrame> b(128);
  bool deferred = false;
  bool differs = false;
  for (std::uint64_t block = 0; block < 40; ++block) {
    a.assign(128, {0.0F, 0.0F});
    b.assign(128, {0.0F, 0.0F});
    (void)kept->process(a, {}, 12'000 + block * 128, 1, {}, {});
    const auto report = switched->process(b, {}, 12'000 + block * 128, 1, {}, {});
    deferred = deferred || report.kernel.deferred_changes;
    if (block > 10) {
      for (std::size_t i = 0; i < a.size(); ++i) {
        differs = differs || a[i].left != b[i].left;
      }
    }
  }
  require(!deferred, "a filter change is never deferred");
  require(differs, "the sounding note now plays through the new filter");
}

// Ping-pong feeds the mixed input to the left line, so the first repeat of
// a centred note is on the left only; straight stereo repeats it centred.
void ping_pong_switches_the_repeat_image() {
  for (const double pong : {1.0, 0.0}) {
    auto values = clean_patch();
    values[at("ember.fx.chorus")] = 0;
    values[at("ember.fx.ensemble")] = 0;
    values[at("ember.fx.tape")] = 0;
    values[at("ember.fx.wobble")] = 0;
    values[at("ember.fx.delay")] = 1;
    values[at("ember.fx.delay_time")] = 0.25;
    values[at("ember.fx.delay_feedback")] = 0.5;
    values[at("ember.fx.delay_sync")] = 0;
    values[at("ember.fx.delay_pingpong")] = pong;
    values[at("ember.amp.release")] = 0.01;
    auto runtime = make_runtime(ember_engine(), values);
    const auto out = render(*runtime, note(0, 2'400, 57, 1), 24'000);
    // The first repeat arrives 0.25 s after the note: 12,000 frames.
    double left = 0.0;
    double right = 0.0;
    for (std::size_t i = 12'400; i < 14'000; ++i) {
      left += out.left[i] * out.left[i];
      right += out.right[i] * out.right[i];
    }
    REQUIRE_GT(left, 0.0, "the first repeat is heard");
    if (pong == 1.0) {
      REQUIRE_LT(right, 1e-3 * left, "ping-pong's first repeat is on the left");
    } else {
      REQUIRE_LT(std::fabs(left - right), 0.05 * left, "a straight repeat stays centred");
    }
  }
}

void tail_bound_covers_delay_repeats() {
  for (std::string_view name : {"Dream Delay", "Sweep Pad", "Bass Pluck"}) {
    const auto values = patch(name);
    auto runtime = make_runtime(ember_engine(), values);
    const auto tail = ember_engine().tail_frames(values, 48'000.0);
    const auto out = render(*runtime, note(0, 4'800, 60, 1), 4'800 + tail + 9'600);
    require(peak(out.left, 4'800 + tail) < 1e-3 && peak(out.right, 4'800 + tail) < 1e-3,
            std::string(name) + ": output settles inside the tail bound");
  }
}

void presets_render_sane_audio() {
  for (const auto& entry : table().presets) {
    auto runtime = make_runtime(ember_engine(), patch(entry.name));
    std::vector<Event> chord;
    std::uint64_t id = 1;
    for (std::uint8_t key :
         {std::uint8_t{48}, std::uint8_t{55}, std::uint8_t{60}, std::uint8_t{64}}) {
      auto n = note(0, 36'000, key, id++);
      chord.insert(chord.end(), n.begin(), n.end());
    }
    const auto out = render(*runtime, chord, 48'000);
    double power = 0.0;
    double sum = 0.0;
    for (std::size_t i = 0; i < out.left.size(); ++i) {
      REQUIRE_TRUE(std::isfinite(out.left[i]), "preset is finite");
      REQUIRE_TRUE(std::isfinite(out.right[i]), "preset is finite");
      power += out.left[i] * out.left[i] + out.right[i] * out.right[i];
      sum += out.left[i] + out.right[i];
    }
    const double rms_db = 10.0 * std::log10(power / (2.0 * 48'000.0));
    std::cout << "  " << entry.name << ": rms " << rms_db << " dBFS, peak "
              << 20.0 * std::log10(std::max(peak(out.left), peak(out.right))) << " dBFS\n";
    REQUIRE_LE(std::max(peak(out.left), peak(out.right)), 1.0, "preset never exceeds 0 dBFS");
    REQUIRE_GT(rms_db, -40.0, "preset is audible and not slammed");
    REQUIRE_LT(rms_db, -3.0, "preset is audible and not slammed");
    REQUIRE_LT(std::fabs(sum / (2.0 * 48'000.0)), 0.01, "preset has no DC offset");
  }
}

void full_load_does_not_allocate() {
  auto values = patch("Age Max");
  values[at("ember.fx.ensemble")] = 1;
  values[at("ember.fx.delay")] = 0.5;
  values[at("ember.pm.osc")] = 0.5;
  values[at("ember.pm.cutoff")] = 1;
  auto runtime = make_runtime(ember_engine(), values);
  std::vector<engine::StereoFrame> buffer(128);
  std::vector<engine::BlockMidiEvent> on(1);
  on[0].clip_id_bits = {1, 1};
  on[0].note_id_bits = {0, 1};
  on[0].kind = engine::MidiEventKind::note_on;
  on[0].key = 60;
  on[0].velocity = 100;
  std::vector<double> lane(128, 900.0);
  const std::vector<ParameterLane> lanes{
      {static_cast<std::uint32_t>(at("ember.filter.cutoff")), lane}};
  counting_allocations = true;
  counted_allocations = 0;
  for (std::uint64_t block = 0; block < 64; ++block) {
    (void)runtime->process(buffer, block == 0 ? std::span(on) : std::span(on).subspan(0, 0),
                           block * 128, 1, {}, lanes);
  }
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "the Ember callback never allocates");
}

void presets_match_golden_fingerprints() {
  struct Golden {
    std::string_view name;
    Fingerprint expected;
  };
  static constexpr std::array<Golden, 32> golden{{
      {"Init", {-11.546, -110.481, 2820.3}},
      {"Ember Default", {-17.058, -44.941, 1944.1}},
      {"Juno Strings", {-16.556, -30.150, 1859.6}},
      {"Solina Ensemble", {-16.752, -28.744, 2606.1}},
      {"Brass Section", {-15.026, -74.175, 2354.5}},
      {"Poly Keys", {-21.395, -51.357, 1581.9}},
      {"Sync Lead", {-12.421, -29.138, 2214.5}},
      {"Unison Lead", {-11.278, -23.983, 3544.0}},
      {"Bass Pluck", {-19.035, -96.549, 228.4}},
      {"Fat Unison Bass", {-11.818, -103.278, 350.1}},
      {"Sweep Pad", {-11.247, -14.242, 1052.0}},
      {"Dark Pad", {-18.139, -21.705, 646.6}},
      {"Chord Stab", {-24.833, -41.915, 1094.2}},
      {"Dream Delay", {-23.287, -33.690, 1820.1}},
      {"Age Max", {-17.796, -44.924, 1974.0}},
      {"Tape Keys", {-23.660, -48.973, 1545.3}},
      {"Velvet Strings", {-16.726, -24.543, 2871.6}},
      {"Prophet Poly", {-14.596, -38.710, 2128.7}},
      {"Sunrise Swell", {-16.644, -19.726, 2334.5}},
      {"Glass Bells", {-20.951, -34.813, 3440.3}},
      {"Sync Scream", {-16.443, -37.902, 3807.5}},
      {"Liquid Lead", {-17.443, -36.162, 2993.5}},
      {"Round Sub", {-10.547, -105.385, 165.2}},
      {"Rubber Bass", {-12.350, -100.152, 195.1}},
      {"Acid Squelch", {-11.535, -32.060, 420.0}},
      {"Crystal Arp", {-33.741, -54.343, 2183.3}},
      {"Analog Organ", {-16.501, -76.675, 1276.5}},
      {"Night Keys", {-16.793, -49.876, 2127.2}},
      {"Cassette Memory", {-17.382, -20.924, 1261.7}},
      {"Fifths Drone", {-15.209, -16.640, 873.6}},
      {"Brass '82", {-15.951, -75.594, 2796.9}},
      {"Silk Pad", {-18.289, -25.515, 1519.8}},
  }};
  const bool rebaseline = std::getenv("EMBER_REBASELINE") != nullptr;
  for (const auto& entry : golden) {
    const auto actual = fingerprint(ember_engine(), entry.name);
    if (rebaseline) {
      std::printf("      {\"%s\", {%.3f, %.3f, %.1f}},\n", std::string(entry.name).c_str(),
                  actual.hold_db, actual.tail_db, actual.centroid_hz);
      continue;
    }
    const bool silent_tail = entry.expected.tail_db < -80.0;
    REQUIRE_LT(std::fabs(actual.hold_db - entry.expected.hold_db), 0.2,
               std::string(entry.name) + ": level changed");
    require(silent_tail ? actual.tail_db < -80.0
                        : std::fabs(actual.tail_db - entry.expected.tail_db) < 0.5,
            std::string(entry.name) + ": release tail changed");
    REQUIRE_LT(std::fabs(actual.centroid_hz / entry.expected.centroid_hz - 1.0), 0.02,
               std::string(entry.name) + ": tone changed");
  }
}

} // namespace

int main() {
  try {
    descriptor_registry_and_presets();
    circuit_laws_hold();
    pitch_is_exact_without_age();
    the_dco_holds_while_the_vco_wanders();
    the_vco_goes_flat_at_the_top();
    the_supply_sags_under_a_big_chord();
    bleed_leaks_and_then_lets_go();
    hum_sits_at_the_mains();
    age_is_deterministic_and_makes_voices_differ();
    chord_and_unison_modes();
    mode_changes_keep_note_offs_with_their_voices();
    polyphony_round_robin_and_steals();
    filter_type_changes_apply_live();
    ping_pong_switches_the_repeat_image();
    tail_bound_covers_delay_repeats();
    presets_render_sane_audio();
    full_load_does_not_allocate();
    presets_match_golden_fingerprints();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "ember engine contract passed\n";
  return 0;
}
