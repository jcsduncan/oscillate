#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_dsp/synth/halo.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "synth_test_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {
using namespace synth_test;
constexpr double pi = 3.14159265358979323846;

const SynthEngine& halo_engine() { return halo::engine(); }
const EngineDescriptor& table() { return *halo_engine().descriptor; }
std::size_t at(std::string_view id) { return index(table(), id); }
std::vector<double> patch(std::string_view name) { return preset(table(), name); }

// Amplitude of the component at hz over a whole number of its cycles.
double amplitude(const std::vector<double>& x, std::size_t from, std::size_t length, double hz,
                 double rate) {
  double re = 0.0;
  double im = 0.0;
  for (std::size_t i = from; i < from + length; ++i) {
    const double t = 2.0 * pi * hz * static_cast<double>(i) / rate;
    re += x[i] * std::cos(t);
    im += x[i] * std::sin(t);
  }
  return 2.0 * std::hypot(re, im) / static_cast<double>(length);
}

void descriptor_registry_and_presets() {
  const auto valid = validate_engine(table());
  REQUIRE_OK(valid, valid.has_value() ? "" : valid.error().message);
  REQUIRE_EQ(table().id, "halo", "engine identity is halo v1");
  REQUIRE_EQ(table().version, 1, "engine identity is halo v1");
  REQUIRE_EQ(table().presets.size(), 16, "sixteen factory presets");
  REQUIRE_EQ(find_engine("halo"), &halo_engine(), "Halo registers as the fifth engine");
  REQUIRE_GE(engines().size(), 5, "Halo registers as the fifth engine");
  REQUIRE_EQ(engines()[4], &halo_engine(), "Halo registers as the fifth engine");
  for (const auto& parameter : table().parameters) {
    require(parameter.id.starts_with("halo."), "ids are namespaced");
  }
}

void pitch_is_exact() {
  for (double rate : {44'100.0, 48'000.0, 96'000.0}) {
    for (std::uint8_t key : {std::uint8_t{45}, std::uint8_t{69}, std::uint8_t{93}}) {
      auto runtime = make_runtime(halo_engine(), patch("Init"), rate);
      const auto frames = static_cast<std::size_t>(rate);
      const auto out = render(*runtime, note(0, frames, key, 1), frames, 128, rate);
      const double expected = 440.0 * std::exp2((key - 69.0) / 12.0);
      REQUIRE_LT(std::fabs(cents(measured_hz(out.left, frames / 4, frames, rate), expected)), 0.5,
                 "pitch within 0.5 cent");
    }
  }
}

// Through the whole voice (envelopes, oversampling, decimation): op 2 at
// ratio 4 modulating op 1 with index beta puts J_n(beta) at (1 + 4n) f, and
// the negative orders at (4|n| - 1) f, none of which overlap.
void voice_sidebands_follow_bessel_functions() {
  for (double beta : {0.7, 1.4}) {
    auto values = patch("Init");
    values[at("halo.op2.ratio")] = 4;
    values[at("halo.op2.velocity")] = 0;
    values[at("halo.op2.l2")] = 99; // Sustain at full level.
    values[at("halo.op2.l3")] = 99;
    values[at("halo.op2.level")] = beta / (2.0 * pi * 2.0);
    auto runtime = make_runtime(halo_engine(), values);
    const auto out = render(*runtime, note(0, 72'000, 57, 1), 72'000);
    const double f = 220.0;
    const double carrier = amplitude(out.left, 24'000, 48'000, f, 48'000.0);
    const auto check = [&](double hz, int order) {
      const double measured = amplitude(out.left, 24'000, 48'000, hz, 48'000.0) / carrier;
      const double expected = std::fabs(std::cyl_bessel_j(static_cast<double>(order), beta) /
                                        std::cyl_bessel_j(0.0, beta));
      REQUIRE_LT(std::fabs(20.0 * std::log10(measured / expected)), 0.2,
                 "voice sideband matches J_n(beta)");
    };
    check(5.0 * f, 1);
    check(3.0 * f, 1);
    check(9.0 * f, 2);
  }
}

// The brightest stock patch at C6, at 44.1 kHz, held at its attack-peak
// indices (every envelope sustains at 1) so its spectrum is steady:
// everything that is not a harmonic of the note is alias, and must stay
// below -70 dBFS.
void e_piano_alias_floor() {
  constexpr double rate = oscillate::test::sample_rate_44k_hz;
  auto values = patch("Bright EP");
  for (char op = '1'; op <= '6'; ++op) {
    values[at(std::string("halo.op") + op + ".l2")] = 99;
    values[at(std::string("halo.op") + op + ".l3")] = 99;
  }
  values[at("halo.fx.chorus")] = 0;
  values[at("halo.fx.delay")] = 0;
  auto runtime = make_runtime(halo_engine(), values, rate);
  const auto out = render(*runtime, note(0, 20'000, 84, 1, 127), 20'000, 128, rate);
  const std::size_t n = 8'192;
  const std::size_t from = 4'410;
  std::vector<std::complex<double>> window(n);
  for (std::size_t i = 0; i < n; ++i) {
    const double phase = 2.0 * pi * static_cast<double>(i) / static_cast<double>(n);
    const double blackman_harris = 0.35875 - 0.48829 * std::cos(phase) +
                                   0.14128 * std::cos(2 * phase) - 0.01168 * std::cos(3 * phase);
    window[i] = out.left[from + i] * blackman_harris;
  }
  fft(window);
  const double f0 = 440.0 * std::exp2((84 - 69) / 12.0);
  const double bin_hz = rate / static_cast<double>(n);
  double worst = -300.0;
  double worst_hz = 0.0;
  for (std::size_t bin = 1; bin < n / 2; ++bin) {
    const double hz = static_cast<double>(bin) * bin_hz;
    const double harmonic = std::round(hz / f0);
    // Harmonics carry +/-3 cent detuned copies beside the window main lobe.
    if (harmonic >= 1.0 && std::fabs(hz - harmonic * f0) < 25.0 + 0.002 * harmonic * f0) {
      continue;
    }
    const double db =
        20.0 * std::log10(std::abs(window[bin]) / (0.35875 * static_cast<double>(n)) + 1e-30);
    if (db > worst) {
      worst = db;
      worst_hz = hz;
    }
  }
  std::cout << "  E-Piano C6 alias floor " << worst << " dBFS at " << worst_hz << " Hz\n";
  REQUIRE_LT(worst, -70.0, "inharmonic alias floor below -70 dBFS");
}

void any_block_split_renders_identically() {
  for (std::string_view name : {"E-Piano", "Strings", "Metallic Lead", "Glass Pad"}) {
    auto a = make_runtime(halo_engine(), patch(name));
    auto b = make_runtime(halo_engine(), patch(name));
    auto events = note(0, 20'000, 48, 1);
    auto more = note(7'000, 20'000, 60, 2);
    events.insert(events.end(), more.begin(), more.end());
    const auto x = render(*a, events, 40'000, 64);
    const auto y = render(*b, events, 40'000, 0);
    require(x.left == y.left && x.right == y.right,
            std::string(name) + ": block splits render identically");
  }
}

// Algorithms 4 and 6 close true loops (op 4 and op 5 into op 6). They share
// their routings with 3 and 5, which feed op 6 back on itself, so with no
// feedback each pair renders the same, and with feedback the loop differs
// and stays bounded at full depth.
void feedback_loops_close_through_the_chain() {
  for (const auto [self, loop] : {std::pair{2, 3}, std::pair{4, 5}}) {
    for (const double amount : {0.0, 1.0}) {
      auto values = patch("Init");
      for (std::size_t n = 0; n < 6; ++n) {
        values[at("halo.op" + std::to_string(n + 1) + ".level")] = 0.7;
        values[at("halo.op" + std::to_string(n + 1) + ".ratio")] = static_cast<double>(n % 3 + 1);
      }
      values[at("halo.feedback")] = amount;
      values[at("halo.algorithm")] = self;
      auto a = make_runtime(halo_engine(), values);
      values[at("halo.algorithm")] = loop;
      auto b = make_runtime(halo_engine(), values);
      const auto x = render(*a, note(0, 24'000, 48, 1), 24'000);
      const auto y = render(*b, note(0, 24'000, 48, 1), 24'000);
      const bool same = x.left == y.left && x.right == y.right;
      if (amount == 0.0) {
        require(same, "without feedback, a loop algorithm matches its self-feedback twin");
      } else {
        require(!same, "with feedback, the loop differs from self-feedback");
        REQUIRE_LT(peak(y.left, 0), 1.0, "a full-depth loop stays bounded");
        REQUIRE_LT(peak(y.right, 0), 1.0, "a full-depth loop stays bounded");
        for (std::size_t i = 0; i < y.left.size(); ++i) {
          REQUIRE_TRUE(std::isfinite(y.left[i]), "the loop stays finite");
          REQUIRE_TRUE(std::isfinite(y.right[i]), "the loop stays finite");
        }
      }
    }
  }
}

// Algorithm, unison and filter-type changes reach the sounding note
// (ADR-0038): no deferral, and after the crossfade the note sounds through
// the new routing. The switch contract proves the change is click-free.
void structural_changes_apply_live_and_polyphony_steals() {
  const auto values = patch("Strings");
  for (const auto& [id, value] :
       {std::pair{"halo.algorithm", 31.0}, std::pair{"halo.voice.unison", 3.0},
        std::pair{"halo.filter.type", 5.0}}) {
    auto changed = values;
    changed[at(id)] = value;
    auto kept = make_runtime(halo_engine(), values);
    auto switched = make_runtime(halo_engine(), values);
    (void)render(*kept, note(0, 96'000, 60, 1), 24'000);
    (void)render(*switched, note(0, 96'000, 60, 1), 24'000);
    require(switched->offer_values(changed, 1), "change offered");
    std::vector<engine::StereoFrame> a(128);
    std::vector<engine::StereoFrame> b(128);
    bool deferred = false;
    bool differs = false;
    for (std::uint64_t block = 0; block < 40; ++block) {
      a.assign(128, {0.0F, 0.0F});
      b.assign(128, {0.0F, 0.0F});
      (void)kept->process(a, {}, 24'000 + block * 128, 1, {}, {});
      const auto report = switched->process(b, {}, 24'000 + block * 128, 1, {}, {});
      deferred = deferred || report.kernel.deferred_changes;
      if (block > 10) {
        for (std::size_t i = 0; i < a.size(); ++i) {
          differs = differs || a[i].left != b[i].left;
        }
      }
    }
    require(!deferred, std::string(id) + ": never deferred");
    require(differs, std::string(id) + ": the sounding note changes");
  }

  auto poly = make_runtime(halo_engine(), patch("Organ"));
  std::vector<Event> events;
  for (std::uint64_t i = 0; i < 17; ++i) {
    auto n = note(i * 240, 40'000, static_cast<std::uint8_t>(40 + i), i + 1);
    events.insert(events.end(), n.begin(), n.end());
  }
  const auto out = render(*poly, events, 6'000);
  REQUIRE_EQ(out.last.active_voices, halo::voice_limit,
             "sixteen voices; the seventeenth note steals");
  REQUIRE_EQ(out.last.voice_steals, 1, "sixteen voices; the seventeenth note steals");
}

void mono_and_legato_use_one_voice() {
  for (double mode : {1.0, 2.0}) {
    auto values = patch("Init"); // A pure sine, so the zero-crossing oracle is exact.
    values[at("halo.voice.mode")] = mode;
    auto runtime = make_runtime(halo_engine(), values);
    auto events = note(0, 24'000, 60, 1);
    auto more = note(6'000, 6'000, 64, 2);
    events.insert(events.end(), more.begin(), more.end());
    const auto out = render(*runtime, events, 18'000);
    REQUIRE_EQ(out.last.active_voices, 1, "mono modes play one voice");
    // After the upper note ends, the held lower note sounds again.
    REQUIRE_LT(std::fabs(cents(measured_hz(out.left, 14'000, 18'000, 48'000.0), 261.6256)), 15.0,
               "releasing the top note returns to the held note");
  }
}

void tail_bound_covers_release_and_delay() {
  for (std::string_view name : {"E-Piano", "Bells", "Glass Pad"}) {
    const auto values = patch(name);
    auto runtime = make_runtime(halo_engine(), values);
    const auto tail = halo_engine().tail_frames(values, 48'000.0);
    const auto out = render(*runtime, note(0, 4'800, 60, 1), 4'800 + tail + 9'600);
    require(peak(out.left, 4'800 + tail) < 1e-3 && peak(out.right, 4'800 + tail) < 1e-3,
            std::string(name) + ": output settles inside the tail bound");
  }
}

void presets_render_sane_audio() {
  for (const auto& entry : table().presets) {
    auto runtime = make_runtime(halo_engine(), patch(entry.name));
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
  auto values = patch("Strings");
  values[at("halo.voice.unison")] = 4;
  values[at("halo.filter.type")] = 5;
  values[at("halo.fx.delay")] = 0.5;
  auto runtime = make_runtime(halo_engine(), values);
  std::vector<engine::StereoFrame> buffer(128);
  std::vector<engine::BlockMidiEvent> on(1);
  on[0].clip_id_bits = {1, 1};
  on[0].note_id_bits = {0, 1};
  on[0].kind = engine::MidiEventKind::note_on;
  on[0].key = 60;
  on[0].velocity = 100;
  std::vector<double> lane(128, 0.5);
  const std::vector<ParameterLane> lanes{{static_cast<std::uint32_t>(at("halo.brightness")), lane}};
  counting_allocations = true;
  counted_allocations = 0;
  for (std::uint64_t block = 0; block < 64; ++block) {
    (void)runtime->process(buffer, block == 0 ? std::span(on) : std::span(on).subspan(0, 0),
                           block * 128, 1, {}, lanes);
  }
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "the Halo callback never allocates");
}

void presets_match_golden_fingerprints() {
  struct Golden {
    std::string_view name;
    Fingerprint expected;
  };
  static constexpr std::array<Golden, 16> golden{{
      {"E-Piano", {-23.324, -61.350, 355.1}},
      {"Bright EP", {-23.491, -52.766, 539.0}},
      {"Bells", {-21.192, -28.682, 1226.6}},
      {"Tubular", {-17.567, -23.679, 1133.9}},
      {"Marimba", {-42.193, -174.214, 266.7}},
      {"FM Bass", {-15.731, -149.700, 185.5}},
      {"Slap Bass", {-26.827, -157.185, 135.8}},
      {"Brass", {-20.607, -67.850, 666.3}},
      {"Strings", {-30.415, -40.135, 1363.0}},
      {"Organ", {-24.868, -134.916, 723.8}},
      {"Clav", {-24.067, -238.120, 601.8}},
      {"Harp", {-23.269, -37.400, 304.4}},
      {"Glass Pad", {-27.683, -31.235, 778.2}},
      {"Metallic Lead", {-26.005, -44.630, 1941.0}},
      {"Sitar-ish", {-26.694, -50.385, 783.8}},
      {"Init", {-12.409, -142.127, 229.9}},
  }};
  const bool rebaseline = std::getenv("HALO_REBASELINE") != nullptr;
  for (const auto& entry : golden) {
    const auto actual = fingerprint(halo_engine(), entry.name);
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
    pitch_is_exact();
    voice_sidebands_follow_bessel_functions();
    e_piano_alias_floor();
    any_block_split_renders_identically();
    structural_changes_apply_live_and_polyphony_steals();
    feedback_loops_close_through_the_chain();
    mono_and_legato_use_one_voice();
    tail_bound_covers_release_and_delay();
    presets_render_sane_audio();
    full_load_does_not_allocate();
    presets_match_golden_fingerprints();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "halo engine contract passed\n";
  return 0;
}
