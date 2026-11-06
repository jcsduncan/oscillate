#include "oscillate/native_dsp/synth/anvil.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/signal.hpp"
#include "oscillate/test/tolerances.hpp"
#include "synth_test_support.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

// Anvil's engine contract (ADR-0060): the drawn body follows its curves and
// lands in tune (fixed or following the note), a retrigger never clicks, the
// rumble rings at the tail note and ducks under the hit, the drive stages
// stay bounded and alias little, the ceiling holds, any block split renders
// alike, nothing allocates, the preview is the kernel's own render and every
// preset is sane.
namespace {
using namespace synth_test;
namespace anvil = oscillate::native_dsp::synth::anvil;

constexpr double rate = oscillate::test::sample_rate_48k_hz;

const SynthEngine& anvil_engine() { return anvil::engine(); }
const EngineDescriptor& table() { return *anvil_engine().descriptor; }
std::size_t at(std::string_view id) { return index(table(), id); }

// Only the body, nothing after it but a transparent master.
std::vector<double> body_only() {
  auto v = preset(table(), "Init");
  v[at("anvil.click.level")] = 0.0;
  v[at("anvil.rumble.on")] = 0.0;
  v[at("anvil.sub.on")] = 0.0;
  v[at("anvil.body.drive")] = 0.0;
  v[at("anvil.body.harmonics")] = 0.0;
  v[at("anvil.drive.amount")] = 0.0;
  v[at("anvil.punch")] = 0.0;
  v[at("anvil.glue")] = 0.0;
  v[at("anvil.tone")] = 1.0;
  v[at("anvil.output")] = 0.0;
  v[at("anvil.velocity")] = 0.0;
  return v;
}

project::NativeCurve curve(std::string_view id, std::vector<project::NativeCurvePoint> points) {
  return {std::string(id), std::move(points)};
}
project::NativeCurvePoint pt(double x, double y, double tension = 0.0) {
  return {x, y, tension, project::NativeCurveSegment::curve};
}

// A body whose pitch falls from `from` to `to` Hz by x = 0.3, then holds;
// a flat amplitude so the frequency is easy to read.
project::NativeSynthDocument held_pitch(double from, double to) {
  project::NativeSynthDocument document;
  document.curves.push_back(
      curve(anvil::pitch_curve, {pt(0, anvil::y_of_hz(from), -0.6), pt(0.3, anvil::y_of_hz(to)),
                                 pt(1, anvil::y_of_hz(to))}));
  document.curves.push_back(curve(anvil::amp_curve, {pt(0, 0.8), pt(1, 0.8)}));
  document.curves.push_back(curve(anvil::drive_curve, {pt(0, 0), pt(1, 0)}));
  return document;
}

std::unique_ptr<NativeSynthRuntime> runtime_with(const std::vector<double>& values,
                                                 const project::NativeSynthDocument& document,
                                                 SynthMedia media = {}) {
  auto created =
      NativeSynthRuntime::create(anvil_engine(), values, document, std::move(media), rate);
  REQUIRE_OK(created, "runtime is created");
  return std::move(created.value());
}

std::vector<double> mono(const Rendered& r) {
  std::vector<double> out(r.left.size());
  for (std::size_t i = 0; i < out.size(); ++i)
    out[i] = 0.5 * (r.left[i] + r.right[i]);
  return out;
}

double magnitude(const std::vector<double>& x, std::size_t from, std::size_t length, double hz) {
  double re = 0.0;
  double im = 0.0;
  for (std::size_t i = 0; i < length && from + i < x.size(); ++i) {
    const double window = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979 * static_cast<double>(i) /
                                               static_cast<double>(length - 1));
    const double t = 2.0 * 3.14159265358979 * hz * static_cast<double>(i) / rate;
    re += x[from + i] * window * std::cos(t);
    im += x[from + i] * window * std::sin(t);
  }
  return std::sqrt(re * re + im * im);
}

using oscillate::test::rms;

// The project model's curve rules (ADR-0057): x from 0 to 1, rising; y and
// tension in range.
bool model_valid(const project::NativeSynthDocument& document) {
  for (const auto& c : document.curves) {
    if (c.points.size() < 2 || c.points.front().x != 0.0 || c.points.back().x != 1.0)
      return false;
    for (std::size_t i = 0; i < c.points.size(); ++i) {
      const auto& point = c.points[i];
      if (point.y < 0.0 || point.y > 1.0 || point.tension < -1.0 || point.tension > 1.0 ||
          (i > 0 && point.x <= c.points[i - 1].x))
        return false;
    }
  }
  return true;
}

void descriptor_registry_and_presets() {
  REQUIRE_EQ(find_engine("anvil"), &anvil_engine(), "Anvil is registered");
  REQUIRE_NE(std::ranges::find(engines(), &anvil_engine()), engines().end(), "Anvil is registered");
  REQUIRE_OK(validate_engine(table()), "the table validates");
  REQUIRE_GE(table().presets.size(), 24,
             "at least 24 presets, Warehouse Rumble first (the defaults)");
  REQUIRE_EQ(table().presets.front().name, "Warehouse Rumble",
             "at least 24 presets, Warehouse Rumble first (the defaults)");
  std::size_t techno = 0;
  std::size_t house = 0;
  for (std::size_t i = 0; i < table().presets.size(); ++i) {
    const auto& entry = table().presets[i];
    REQUIRE_OK(preset_values(table(), entry), std::string(entry.name) + " resolves");
    const auto document = anvil_engine().preset_document(i);
    require(anvil_engine().validate_document(document).has_value() && model_valid(document),
            std::string(entry.name) + "'s body is a valid document");
    techno += entry.category == "Techno" ? 1U : 0U;
    house += entry.category == "House" ? 1U : 0U;
  }
  REQUIRE_GE(techno, 8, "weighted to techno and house");
  REQUIRE_GE(house, 6, "weighted to techno and house");
  require(anvil_engine().models_pedals, "a kick ignores the pedals");
  for (const auto& parameter : table().parameters)
    require(parameter.automatable && parameter.timing == ParameterTiming::live,
            std::string(parameter.id) + " is live and automatable");
}

void documents_validate_and_compile() {
  const auto& engine = anvil_engine();
  REQUIRE_OK(engine.validate_document({}), "an empty document plays the defaults");
  auto twice = held_pitch(200, 50);
  twice.curves.push_back(twice.curves.front());
  require(!engine.validate_document(twice).has_value(), "a curve appears once");
  auto stranger = held_pitch(200, 50);
  stranger.curves.push_back(curve("filter", {pt(0, 0), pt(1, 1)}));
  require(!engine.validate_document(stranger).has_value(), "unknown curves are refused");
  project::NativeSynthDocument routed;
  routed.routes.push_back({});
  require(!engine.validate_document(routed).has_value(), "Anvil has no routes");
  project::NativeSynthDocument builtin;
  builtin.media.push_back({std::string(anvil::click_slot), project::NativeBuiltinMedia{"x"}, 0});
  require(!engine.validate_document(builtin).has_value(), "the click slot is a user sample");

  // A user transient: a decaying 3 kHz burst at 44.1 kHz.
  auto asset = std::make_shared<SynthMediaAsset>();
  asset->source_rate = oscillate::test::sample_rate_44k;
  for (int i = 0; i < 2'000; ++i) {
    const auto s = static_cast<float>(std::sin(2.0 * 3.14159265358979 * 3'000.0 * i / 44'100.0) *
                                      std::exp(-i / 300.0));
    asset->frames.push_back({s, s});
  }
  const project::ManagedAudioReference reference{
      project::SourceId::from_bits({1, 1}).value(), std::string(64, 'a'), 8'000, 44'100, 2, 2'000};
  auto sampled = held_pitch(200, 50);
  sampled.media.push_back({std::string(anvil::click_slot), reference, 0});
  REQUIRE_OK(engine.validate_document(sampled), "a managed click sample validates");
  require(!engine.prepare_document(sampled, {}, rate, nullptr).has_value(),
          "an unloaded sample is reported, not ignored");
  const SynthMedia media{{std::string(anvil::click_slot), reference.sha256, asset}};
  REQUIRE_OK(engine.prepare_document(sampled, media, rate, nullptr), "a loaded sample compiles");

  auto values = body_only();
  values[at("anvil.click.type")] = 4.0; // Sample.
  values[at("anvil.click.level")] = 1.0;
  values[at("anvil.click.length")] = 20.0;
  values[at("anvil.body.level")] = 0.0;
  const auto with = anvil::render_preview(values, sampled, &media, 48'000, 120.0);
  const auto without = anvil::render_preview(values, held_pitch(200, 50), nullptr, 48'000, 120.0);
  double heard = 0.0;
  double silent = 0.0;
  for (std::size_t i = 0; i < 960; ++i) {
    heard = std::max(heard, std::fabs(static_cast<double>(with.master[i])));
    silent = std::max(silent, std::fabs(static_cast<double>(without.master[i])));
  }
  REQUIRE_GT(heard, 0.1, "the sample is the click");
  REQUIRE_LT(silent, 1e-6, "the sample is the click");
}

// The body's frequency follows the drawn sweep: zero-crossing frequency over
// short windows against the curve at each window's centre.
void pitch_follows_the_curve() {
  project::NativeSynthDocument document;
  document.curves.push_back(
      curve(anvil::pitch_curve, {pt(0, anvil::y_of_hz(400), -0.5), pt(1, anvil::y_of_hz(60))}));
  document.curves.push_back(curve(anvil::amp_curve, {pt(0, 0.8), pt(1, 0.8)}));
  auto values = body_only();
  values[at("anvil.length")] = 1'000.0;
  values[at("anvil.body.phase")] = 0.0;
  auto runtime = runtime_with(values, document);
  const auto x = mono(render(*runtime, note(0, 100, 36, 1), 48'000));
  const auto expected = [](double t) {
    // The engine's bend: tension -0.5 is 1 - (1 - t)^3.
    const double y0 = anvil::y_of_hz(400);
    const double y1 = anvil::y_of_hz(60);
    return anvil::hz_of_y(y0 + (y1 - y0) * (1.0 - std::pow(1.0 - t, 3.0)));
  };
  for (const double t : {0.15, 0.3, 0.5, 0.7, 0.9}) {
    const auto centre = static_cast<std::size_t>(t * 48'000.0);
    const double hz = expected(t);
    const auto half = static_cast<std::size_t>(1.5 * rate / hz);
    const double measured = measured_hz(x, centre - half, centre + half, rate);
    REQUIRE_LT(std::fabs(cents(measured, hz)), 15.0,
               "the body's pitch follows the drawn curve at x=" + std::to_string(t));
  }
}

// The tail lands on the drawn note, tuned; Follow note puts it on the key.
void the_tail_is_in_tune() {
  const auto document = held_pitch(300, 55);
  for (const double tune : {0.0, -3.0, 5.0}) {
    auto values = body_only();
    values[at("anvil.tune")] = tune;
    auto runtime = runtime_with(values, document);
    const auto x = mono(render(*runtime, note(0, 100, 60, 1), 20'000));
    const double expected = 55.0 * std::pow(2.0, tune / 12.0);
    const double measured = measured_hz(x, 12'000, 19'000, rate);
    REQUIRE_LT(std::fabs(cents(measured, expected)), 2.0, "the tail is the drawn note, tuned");
  }
  auto values = body_only();
  values[at("anvil.key")] = 1.0; // Follow note.
  for (const std::uint8_t key : {std::uint8_t{33}, std::uint8_t{38}, std::uint8_t{45}}) {
    auto runtime = runtime_with(values, document);
    const auto x = mono(render(*runtime, note(0, 100, key, 1), 20'000));
    const double expected = 440.0 * std::pow(2.0, (key - 69.0) / 12.0);
    const double measured = measured_hz(x, 12'000, 19'000, rate);
    REQUIRE_LT(std::fabs(cents(measured, expected)), 2.0, "Follow note lands the tail on the key");
  }
  const auto preview = anvil::render_preview(values, document, nullptr, 48'000, 120.0, 38);
  REQUIRE_LT(std::fabs(cents(preview.tail_hz, 440.0 * std::pow(2.0, (38 - 69) / 12.0))), 0.01,
             "the preview reports the tail as played");
}

// A linear amplitude curve is the body's envelope.
void amp_curve_is_the_envelope() {
  project::NativeSynthDocument document = held_pitch(100, 100);
  document.curves[1] = curve(anvil::amp_curve, {pt(0, 1), pt(1, 0)});
  auto values = body_only();
  values[at("anvil.length")] = 1'000.0;
  values[at("anvil.body.level")] = 0.8;
  values[at("anvil.body.phase")] = 0.0;
  auto runtime = runtime_with(values, document);
  const auto x = mono(render(*runtime, note(0, 100, 36, 1), 48'000));
  for (const double t : {0.25, 0.5, 0.75}) {
    const auto centre = static_cast<std::size_t>(t * 48'000.0);
    const double measured = peak(x, centre - 240, centre + 240); // One period of 100 Hz.
    const double expected = 0.8 * (1.0 - t);
    REQUIRE_LT(std::fabs(measured / expected - 1.0), 0.02, "the envelope is the drawn amplitude");
  }
  REQUIRE_LT(peak(x, 47'990, 48'000), 0.01, "the hit ends at its length");
  REQUIRE_LT(peak(x, 50'000), 2e-3, "the hit ends at its length");
}

// A retrigger mid-body fades the outgoing hit: no step larger than the
// waveform's own steepest slope.
void retrigger_never_clicks() {
  auto values = body_only();
  values[at("anvil.body.phase")] = 0.0;
  values[at("anvil.length")] = 800.0;
  const std::size_t again = 9'611; // Mid-body, off the control grid.
  auto events = note(0, 100, 36, 1);
  const auto second = note(again, 100, 36, 2);
  events.insert(events.end(), second.begin(), second.end());
  auto both = runtime_with(values, held_pitch(200, 60));
  auto alone = runtime_with(values, held_pitch(200, 60));
  const auto mixed = mono(render(*both, events, 20'000));
  const auto incoming = mono(render(*alone, second, 20'000));
  // The master is linear at these settings, so what remains is the outgoing
  // hit and its fade; the incoming hit's own attack is a kick, not a click.
  std::vector<double> x(mixed.size());
  for (std::size_t i = 0; i < x.size(); ++i)
    x[i] = mixed[i] - incoming[i];
  // A step shows as a spike in the second difference; a smooth fade does not.
  const auto curvature = [&](std::size_t from, std::size_t to) {
    double most = 0.0;
    for (std::size_t i = from; i < to; ++i)
      most = std::max(most, std::fabs(x[i + 1] - 2.0 * x[i] + x[i - 1]));
    return most;
  };
  const double steady = curvature(again - 3'000, again - 50);
  REQUIRE_LT(curvature(again - 2, again + 400), std::max(20.0 * steady, 1e-3),
             "a retrigger declicks the outgoing hit");
}

// Sync: the hit lasts a note value at the song's tempo, within the longest
// hit; Time keeps Length.
void synced_length_follows_the_tempo() {
  auto values = body_only();
  values[at("anvil.length.mode")] = 1.0;
  values[at("anvil.length.division")] = 2.0; // 1/16
  const auto document = held_pitch(100, 100);
  REQUIRE_EQ(anvil::render_preview(values, document, nullptr, 48'000, 120.0).kick_frames, 6'000,
             "a sixteenth at 120 BPM is 125 ms");
  REQUIRE_EQ(anvil::render_preview(values, document, nullptr, 48'000, 150.0).kick_frames, 4'800,
             "and follows the tempo");
  values[at("anvil.length.division")] = 13.0; // 1 Bar
  REQUIRE_EQ(anvil::render_preview(values, document, nullptr, 48'000, 60.0).kick_frames, 96'000,
             "a bar at 60 BPM is held to the longest hit");
  values[at("anvil.length.mode")] = 0.0;
  values[at("anvil.length")] = 300.0;
  REQUIRE_EQ(anvil::render_preview(values, document, nullptr, 48'000, 150.0).kick_frames, 14'400,
             "Time keeps Length");
}

// Note: a note in a clip sets its hit's length, rendering as Time does at
// that length; a key played live (length unknown) rings until it is let go,
// within Length. What remains after a hit is the DC blocker settling.
void note_length_sets_the_hit() {
  auto values = body_only();
  values[at("anvil.length.mode")] = 2.0;
  values[at("anvil.length")] = 1'000.0;
  values[at("anvil.body.phase")] = 0.0;
  auto known = note(0, 4'800, 36, 1);
  known.front().frames = 4'800;
  auto clip = runtime_with(values, held_pitch(200, 60));
  const auto x = mono(render(*clip, known, 20'000));
  auto timed_values = values;
  timed_values[at("anvil.length.mode")] = 0.0;
  timed_values[at("anvil.length")] = 100.0;
  auto timed = runtime_with(timed_values, held_pitch(200, 60));
  const auto t = mono(render(*timed, known, 20'000));
  double most = 0.0;
  for (std::size_t i = 0; i < x.size(); ++i)
    most = std::max(most, std::fabs(x[i] - t[i]));
  REQUIRE_LT(most, 1e-6, "a note's length plays as Length set to it");
  REQUIRE_GT(peak(x, 3'800, 4'300), 0.3, "and ends with the note");
  REQUIRE_LT(peak(x, 5'000), 0.05, "and ends with the note");

  auto live = runtime_with(values, held_pitch(100, 100));
  const auto y = mono(render(*live, note(0, 9'600, 36, 1), 30'000));
  REQUIRE_GT(peak(y, 8'800, 9'500), 0.3, "a live key rings while held");
  REQUIRE_LT(peak(y, 10'000), 0.05, "and stops when let go");
  auto held = runtime_with(values, held_pitch(100, 100));
  const auto z = mono(render(*held, note(0, 96'000, 36, 1), 60'000));
  REQUIRE_GT(peak(z, 46'000, 47'000), 0.3, "but never past Length");
  REQUIRE_LT(peak(z, 49'000), 0.05, "but never past Length");

  // Time ignores the note's length.
  values[at("anvil.length.mode")] = 0.0;
  auto ignoring = runtime_with(values, held_pitch(100, 100));
  const auto w = mono(render(*ignoring, known, 20'000));
  REQUIRE_GT(peak(w, 8'000, 9'000), 0.3, "Time plays on past the note");
}

// The rumble rings at the tail note, and ducks under the kick.
void rumble_is_tuned_and_ducks() {
  auto values = preset(table(), "Warehouse Rumble");
  values[at("anvil.rumble.drive")] = 0.0;
  values[at("anvil.rumble.space")] = 0.0;
  // A kick shorter than the beat, so the rumble rings on alone after it.
  values[at("anvil.length")] = 300.0;
  const auto document = held_pitch(260, 52);
  const auto preview = anvil::render_preview(values, document, nullptr, 48'000, 124.0);
  const std::vector<double> rumble(preview.rumble.begin(), preview.rumble.end());
  const std::size_t from = preview.kick_frames;
  const std::size_t length = std::min<std::size_t>(9'600, rumble.size() - from);
  double best_hz = 0.0;
  double best = 0.0;
  for (double hz = 30.0; hz <= 80.0; hz += 0.25) {
    const double m = magnitude(rumble, from, length, hz);
    if (m > best) {
      best = m;
      best_hz = hz;
    }
  }
  REQUIRE_LT(std::fabs(cents(best_hz, preview.tail_hz)), 35.0, "the rumble rings at the tail note");

  auto ducked = values;
  ducked[at("anvil.rumble.duck")] = 1.0;
  auto open = values;
  open[at("anvil.rumble.duck")] = 0.0;
  // At 60 BPM a beat leaves half a second after the kick.
  const auto a = anvil::render_preview(ducked, document, nullptr, 48'000, 60.0);
  const auto b = anvil::render_preview(open, document, nullptr, 48'000, 60.0);
  const std::vector<double> ra(a.rumble.begin(), a.rumble.end());
  const std::vector<double> rb(b.rumble.begin(), b.rumble.end());
  const auto during = static_cast<std::size_t>(0.6 * static_cast<double>(a.kick_frames));
  REQUIRE_LT(rms(ra, 2'400, during), 0.2 * rms(rb, 2'400, during),
             "the rumble ducks under the kick");
  const std::size_t after = a.kick_frames + 4'800;
  REQUIRE_GT(rms(ra, after, after + 4'800), 0.5 * rms(rb, after, after + 4'800),
             "and comes back after it");
}

// Every drive type stays bounded, and oversampling keeps aliases low: a
// steady 1.5 kHz body driven hard has little energy between its harmonics.
void drive_is_bounded_and_clean() {
  const auto document = held_pitch(1'500, 1'500);
  for (double type = 0.0; type <= 3.0; type += 1.0) {
    auto values = body_only();
    values[at("anvil.drive.type")] = type;
    values[at("anvil.drive.amount")] = 1.0;
    values[at("anvil.output")] = -12.0;
    values[at("anvil.length")] = 1'000.0;
    auto runtime = runtime_with(values, document);
    const auto x = mono(render(*runtime, note(0, 100, 36, 1), 40'000));
    REQUIRE_LT(peak(x), 0.5, "the drive keeps full scale where it was");
    const double fundamental = magnitude(x, 16'000, 16'384, 1'500.0);
    double alias = 0.0;
    for (double hz = 2'250.0; hz < 20'000.0; hz += 1'500.0)
      alias = std::max(alias, magnitude(x, 16'000, 16'384, hz));
    REQUIRE_LT(20.0 * std::log10(alias / fundamental), -30.0,
               "oversampled drive keeps aliases 30 dB down");
  }
}

// The hottest settings never pass the ceiling.
void the_ceiling_holds() {
  auto values = preset(table(), "Industrial Hammer");
  values[at("anvil.output")] = 6.0;
  values[at("anvil.drive.amount")] = 1.0;
  values[at("anvil.punch")] = 1.0;
  values[at("anvil.rumble.amount")] = 1.0;
  values[at("anvil.click.level")] = 1.0;
  auto runtime = runtime_with(values, anvil_engine().preset_document(2));
  std::vector<Event> events;
  for (std::uint64_t i = 0; i < 8; ++i) {
    const auto hit = note(i * 11'613, 100, 36, i + 1, 127);
    events.insert(events.end(), hit.begin(), hit.end());
  }
  const auto r = render(*runtime, events, 96'000);
  REQUIRE_LE(peak(r.left), 0.996, "the ceiling holds");
  REQUIRE_LE(peak(r.right), 0.996, "the ceiling holds");
  std::size_t pinned = 0;
  for (std::size_t i = 0; i < r.left.size(); ++i)
    pinned += std::fabs(r.left[i]) >= 0.994 ? 1U : 0U;
  REQUIRE_LT(pinned, r.left.size() / 100, "the soft ceiling, not the hard guard, does the work");
}

// Any block split, and a second runtime, render the same samples: live and
// export run this one kernel.
void any_block_split_renders_identically() {
  for (const auto* name : {"Warehouse Rumble", "Hardstyle Tok", "Dub Techno", "909 Tribute"}) {
    const auto values = preset(table(), name);
    std::size_t index = 0;
    while (table().presets[index].name != name)
      ++index;
    const auto document = anvil_engine().preset_document(index);
    std::vector<Event> events;
    for (std::uint64_t i = 0; i < 4; ++i) {
      const auto hit = note(i * 23'225 + 7, 100, static_cast<std::uint8_t>(33 + 3 * i), i + 1,
                            static_cast<std::uint8_t>(70 + 15 * i));
      events.insert(events.end(), hit.begin(), hit.end());
    }
    auto a = runtime_with(values, document);
    auto b = runtime_with(values, document);
    auto c = runtime_with(values, document);
    const auto x = render(*a, events, 120'000, 128);
    const auto y = render(*b, events, 120'000, 0);
    const auto z = render(*c, events, 120'000, 1'024);
    require(x.left == y.left && x.right == y.right && x.left == z.left && x.right == z.right,
            std::string(name) + ": block splits and runs render identically");
  }
}

void full_load_does_not_allocate() {
  auto values = preset(table(), "Hypnotic");
  values[at("anvil.click.type")] = 3.0;
  values[at("anvil.sub.on")] = 1.0;
  auto runtime = runtime_with(values, anvil_engine().preset_document(3));
  std::vector<engine::StereoFrame> buffer(128);
  std::vector<engine::BlockMidiEvent> events(1);
  events[0].clip_id_bits = {1, 1};
  events[0].kind = engine::MidiEventKind::note_on;
  events[0].key = 36;
  events[0].velocity = 120;
  counted_allocations = 0;
  counting_allocations = true;
  for (std::uint64_t b = 0; b < 400; ++b) {
    events[0].note_id_bits = {0, b + 1};
    const auto hit = b % 90 == 0 ? std::span(events) : std::span(events).subspan(0, 0);
    (void)runtime->process(buffer, hit, b * 128, b < 300 ? 1 : 2, {}, {});
  }
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "the callback allocates nothing");
}

void tail_bound_covers_the_rumble() {
  for (const auto* name : {"Warehouse Rumble", "Dub Techno", "808 Long"}) {
    const auto values = preset(table(), name);
    std::size_t index = 0;
    while (table().presets[index].name != name)
      ++index;
    auto runtime = runtime_with(values, anvil_engine().preset_document(index));
    const auto tail = anvil_engine().tail_frames(values, rate);
    const auto r = render(*runtime, note(0, 100, 36, 1, 127), tail + 4'800);
    require(peak(r.left, tail) < 1e-4 && peak(r.right, tail) < 1e-4,
            std::string(name) + ": silent after the tail bound");
  }
}

// The panel's preview is the kernel's render, frame for frame.
void preview_is_the_kernel() {
  const auto values = preset(table(), "Peak Time");
  const auto document = anvil_engine().preset_document(4);
  const auto preview = anvil::render_preview(values, document, nullptr, 48'000, 124.0, 36, 127);
  REQUIRE_EQ(preview.master.size(), static_cast<std::size_t>(std::ceil(60.0 / 124.0 * rate)),
             "the preview spans a beat and names the kick's length");
  REQUIRE_EQ(preview.rumble.size(), preview.master.size(),
             "the preview spans a beat and names the kick's length");
  REQUIRE_EQ(preview.kick_frames,
             static_cast<std::size_t>(std::ceil(values[at("anvil.length")] * 1e-3 * rate)),
             "the preview spans a beat and names the kick's length");
  auto runtime = runtime_with(values, document);
  const auto r = render(*runtime, note(0, 100, 36, 1, 127), preview.master.size());
  for (std::size_t i = 0; i < preview.master.size(); ++i) {
    const float expected = 0.5F * (static_cast<float>(r.left[i]) + static_cast<float>(r.right[i]));
    REQUIRE_LT(std::fabs(preview.master[i] - expected), tolerance::float_round_trip_f,
               "the preview is the kernel's render");
  }
  REQUIRE_GT(preview.peak_db, -12.0, "the preview reports its peak");
  REQUIRE_LT(preview.peak_db, 0.0, "the preview reports its peak");
}

void telemetry_reports_the_hit() {
  auto runtime = runtime_with(preset(table(), "Warehouse Rumble"), {});
  std::vector<float> frame;
  (void)render(*runtime, note(0, 100, 41, 1, 99), 4'800);
  REQUIRE_TRUE(runtime->read_telemetry(frame), "telemetry is published");
  REQUIRE_EQ(frame.size(), anvil::telemetry::size, "telemetry is published");
  REQUIRE_GT(frame[anvil::telemetry::position], 0.2F, "position, note, velocity and peak");
  REQUIRE_LT(frame[anvil::telemetry::position], 0.3F, "position, note, velocity and peak");
  REQUIRE_EQ(frame[anvil::telemetry::last_note], 41.0F, "position, note, velocity and peak");
  REQUIRE_EQ(frame[anvil::telemetry::last_velocity], 99.0F, "position, note, velocity and peak");
  REQUIRE_GT(frame[anvil::telemetry::output_peak_db], -30.0F, "position, note, velocity and peak");
}

void presets_render_sane_audio() {
  for (std::size_t i = 0; i < table().presets.size(); ++i) {
    const auto& entry = table().presets[i];
    const auto values = preset(table(), entry.name);
    const auto preview =
        anvil::render_preview(values, anvil_engine().preset_document(i), nullptr, 48'000, 124.0);
    bool finite = true;
    for (const float s : preview.master)
      finite = finite && std::isfinite(s);
    require(finite, std::string(entry.name) + " renders finite audio");
    require(preview.peak_db > -20.0 && preview.peak_db < 0.0,
            std::string(entry.name) + " is loud and under full scale");
    require(preview.tail_hz > 25.0 && preview.tail_hz < 120.0,
            std::string(entry.name) + " has a kick's tail note");
  }
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "anvil engine contract passed",
      {
          {"descriptor_registry_and_presets", [] { descriptor_registry_and_presets(); }},
          {"documents_validate_and_compile", [] { documents_validate_and_compile(); }},
          {"pitch_follows_the_curve", [] { pitch_follows_the_curve(); }},
          {"the_tail_is_in_tune", [] { the_tail_is_in_tune(); }},
          {"amp_curve_is_the_envelope", [] { amp_curve_is_the_envelope(); }},
          {"retrigger_never_clicks", [] { retrigger_never_clicks(); }},
          {"rumble_is_tuned_and_ducks", [] { rumble_is_tuned_and_ducks(); }},
          {"drive_is_bounded_and_clean", [] { drive_is_bounded_and_clean(); }},
          {"the_ceiling_holds", [] { the_ceiling_holds(); }},
          {"any_block_split_renders_identically", [] { any_block_split_renders_identically(); }},
          {"full_load_does_not_allocate", [] { full_load_does_not_allocate(); }},
          {"tail_bound_covers_the_rumble", [] { tail_bound_covers_the_rumble(); }},
          {"preview_is_the_kernel", [] { preview_is_the_kernel(); }},
          {"telemetry_reports_the_hit", [] { telemetry_reports_the_hit(); }},
          {"presets_render_sane_audio", [] { presets_render_sane_audio(); }},
          {"synced_length_follows_the_tempo", [] { synced_length_follows_the_tempo(); }},
          {"note_length_sets_the_hit", [] { note_length_sets_the_hit(); }},
      });
}
