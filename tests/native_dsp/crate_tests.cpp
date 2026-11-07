#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/synth/crate.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"
#include "oscillate/warp_dsp/bandlimited.hpp"
#include "synth_test_support.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

// Crate's engine contract (ADR-0063, spec 09 §6): the public band-limited
// reader is the identity at unity; a chop reads back bit-exactly, reversed,
// in each mode; choke groups and the AHDSR land where they should; a sine
// pitched up two octaves in Clean leaves no image above -80 dB; the eras'
// bits and rates are measurable; Follow Tempo stretches without repitching;
// swing, pedals and telemetry work; any block split renders alike; nothing
// allocates; no sample is silence.
namespace {
using namespace synth_test;
namespace crate = oscillate::native_dsp::synth::crate;

constexpr double rate = oscillate::test::sample_rate_48k_hz;
constexpr double pi = 3.14159265358979323846;

const SynthEngine& crate_engine() { return crate::engine(); }
const EngineDescriptor& table() { return *crate_engine().descriptor; }
std::size_t at(std::string_view id) { return index(table(), id); }
std::size_t chop_at(std::size_t slot, std::string_view control) {
  return at(crate::chop_parameter_id(slot, control));
}

using Frames = std::vector<engine::StereoFrame>;

Frames noise(std::size_t frames, std::uint32_t seed = 7) {
  Frames out(frames);
  for (auto& frame : out) {
    seed = seed * 1'664'525U + 1'013'904'223U;
    frame.left = static_cast<float>(static_cast<double>(seed >> 8U) / 16'777'216.0 - 0.5);
    seed = seed * 1'664'525U + 1'013'904'223U;
    frame.right = static_cast<float>(static_cast<double>(seed >> 8U) / 16'777'216.0 - 0.5);
  }
  return out;
}

Frames sine(std::size_t frames, double hz, double amplitude = 0.5) {
  Frames out(frames);
  for (std::size_t i = 0; i < frames; ++i) {
    const auto value =
        static_cast<float>(amplitude * std::sin(2.0 * pi * hz * static_cast<double>(i) / rate));
    out[i] = {value, value};
  }
  return out;
}

// Constant levels, one per section of `length` frames.
Frames steps(const std::vector<float>& levels, std::size_t length) {
  Frames out;
  for (const float level : levels)
    out.insert(out.end(), length, engine::StereoFrame{level, level});
  return out;
}

struct Cut {
  std::uint32_t slot;
  std::uint64_t start;
};

project::NativeSynthDocument document_for(const Frames& frames, const std::vector<Cut>& cuts,
                                          double bpm = 0.0) {
  project::NativeSynthDocument document;
  const project::ManagedAudioReference reference{project::SourceId::from_bits({1, 1}).value(),
                                                 std::string(64, 'a'),
                                                 8'000,
                                                 48'000,
                                                 2,
                                                 frames.size()};
  document.media.push_back({std::string(crate::sample_slot), reference, 0});
  for (const auto& cut : cuts)
    document.regions.push_back({cut.slot, std::string(crate::sample_slot), cut.start, "other", ""});
  if (bpm > 0.0)
    document.labels.push_back({std::string(crate::bpm_label), std::to_string(bpm)});
  return document;
}

SynthMedia media_for(const Frames& frames) {
  auto asset = std::make_shared<SynthMediaAsset>();
  asset->source_rate = oscillate::test::sample_rate_48k;
  asset->frames = frames;
  return {{std::string(crate::sample_slot), std::string(64, 'a'), std::move(asset)}};
}

std::unique_ptr<NativeSynthRuntime> runtime_with(const std::vector<double>& values,
                                                 const Frames& frames, const std::vector<Cut>& cuts,
                                                 double bpm = 0.0) {
  auto created = NativeSynthRuntime::create(crate_engine(), values, document_for(frames, cuts, bpm),
                                            media_for(frames), rate);
  REQUIRE_OK(created, "runtime is created");
  return std::move(created.value());
}

// Defaults with an instant attack, so reads can be compared sample for sample.
std::vector<double> plain() {
  auto values = default_values(table());
  for (std::size_t slot = 0; slot < crate::max_chops; ++slot)
    values[chop_at(slot, "attack")] = 0.0;
  return values;
}

double magnitude_db(const std::vector<double>& x, std::size_t from, std::size_t length,
                    double reference_amplitude) {
  std::vector<std::complex<double>> window(length);
  for (std::size_t i = 0; i < length; ++i) {
    const double hann =
        oscillate::native_dsp::dsp::hann(static_cast<double>(i), static_cast<double>(length));
    window[i] = x[from + i] * hann;
  }
  fft(window);
  double largest = 0.0;
  for (std::size_t bin = 1; bin < length / 2; ++bin)
    largest = std::max(largest, std::abs(window[bin]));
  // A Hann-windowed sine of amplitude A peaks at A * N / 4.
  return 20.0 *
         std::log10(largest / (reference_amplitude * static_cast<double>(length) / 4.0) + 1e-30);
}

double bin_db(const std::vector<double>& x, std::size_t from, std::size_t length, double hz,
              double reference_amplitude) {
  double re = 0.0;
  double im = 0.0;
  for (std::size_t i = 0; i < length; ++i) {
    const double hann =
        oscillate::native_dsp::dsp::hann(static_cast<double>(i), static_cast<double>(length));
    const double t = 2.0 * pi * hz * static_cast<double>(i) / rate;
    re += x[from + i] * hann * std::cos(t);
    im += x[from + i] * hann * std::sin(t);
  }
  return 20.0 *
         std::log10(std::hypot(re, im) / (reference_amplitude * static_cast<double>(length) / 4.0) +
                    1e-30);
}

// ---------------------------------------------------------------------------

void descriptor_registry_and_presets() {
  REQUIRE_OK(validate_engine(table()), "the table validates");
  REQUIRE_EQ(find_engine("crate"), &crate_engine(), "Crate is registered");
  REQUIRE_EQ(table().parameters.size(), 14 + crate::max_chops * crate::chop_controls.size(),
             "14 globals and 17 controls for each of 32 slots");
  REQUIRE_EQ(crate::chop_parameter_id(7, "cutoff"), "crate.chop.07.cutoff", "slot ids");
  REQUIRE_EQ(table().parameters[14 + 7 * 17 + 11].id, "crate.chop.07.cutoff",
             "slot blocks follow the globals in control order");
  for (const auto& parameter : table().parameters) {
    if (parameter.automatable)
      require(!is_discrete(parameter) && parameter.timing == ParameterTiming::live,
              std::string(parameter.id) + ": only continuous live controls automate");
  }
  require(crate_engine().models_pedals, "Crate latches its own pedals");
  for (std::size_t i = 0; i < table().presets.size(); ++i) {
    const auto document = crate_engine().preset_document(i);
    REQUIRE_OK(crate_engine().validate_document(document),
               std::string(table().presets[i].name) + "'s document validates");
  }
  require(crate_engine().preset_document(0).empty(), "Init has no sample");
  require(!crate_engine().preset_document(std::nullopt).empty(), "a new Crate starts with a kick");
  // The built-in starters are the bundled assets' length and level.
  const std::pair<std::string_view, std::pair<std::size_t, double>> starters[] = {
      {"kick", {21'600, 0.65}},
      {"snare", {13'440, 0.58}},
      {"hat", {6'720, 0.42}},
      {"pluck", {33'600, 0.62}}};
  for (const auto& [name, expected] : starters) {
    const auto sample = crate::builtin_sample(name);
    double largest = 0.0;
    for (const float s : sample)
      largest = std::max(largest, std::fabs(static_cast<double>(s)));
    require(sample.size() == expected.first &&
                std::fabs(largest - expected.second) < tolerance::coarse,
            std::string(name) + " matches the bundled starter");
  }
  require(crate::builtin_sample("break").empty(), "unknown built-ins are empty");
}

void documents_validate_and_compile() {
  const auto valid = crate_engine().validate_document;
  const auto frames = steps({0.25F, 0.5F}, 24'000);
  REQUIRE_OK(valid(document_for(frames, {{5, 0}, {2, 24'000}})), "a cut sample");
  REQUIRE_OK(valid(document_for(frames, {}, 120.0)), "an uncut sample with a tempo");
  REQUIRE_OK(valid({}), "an empty document");
  const auto fails = [&](const project::NativeSynthDocument& document, std::string_view why) {
    require(!valid(document).has_value(), why);
  };
  fails(document_for(frames, {{32, 0}}), "slot 32 is out of range");
  fails(document_for(frames, {{1, 0}, {1, 10}}), "slots are unique");
  fails(document_for(frames, {{1, 48'000}}), "a start beyond the sample");
  {
    std::vector<Cut> cuts;
    for (std::uint32_t i = 0; i <= 32; ++i)
      cuts.push_back({i % 32, static_cast<std::uint64_t>(i) * 100});
    fails(document_for(frames, cuts), "at most 32 chops");
  }
  {
    auto document = document_for(frames, {{0, 0}});
    document.regions.front().tag = "cowbell";
    fails(document, "tags are kick, snare, hat or other");
    document.regions.front().tag = "hat";
    document.regions.front().media = "click";
    fails(document, "regions cut the sample slot");
  }
  {
    auto document = document_for(frames, {{0, 0}});
    document.media.front().id = "click";
    fails(document, "media live in the sample slot");
  }
  {
    auto document = document_for(frames, {{0, 0}});
    document.labels.push_back({"crate.bpm", "fast"});
    fails(document, "the tempo label is a number");
    document.labels.front() = {"crate.key", "C"};
    fails(document, "only the tempo and name labels");
    document.labels.front() = {std::string(crate::sample_name_label), ""};
    fails(document, "the sample's name is not empty");
    document.labels.front() = {std::string(crate::sample_name_label), "break.wav"};
    document.labels.push_back({std::string(crate::sample_name_label), "again.wav"});
    fails(document, "one name label");
  }
  {
    auto document = document_for(frames, {{0, 0}});
    document.curves.push_back({"amp", {{0, 0}, {1, 1}}});
    fails(document, "no curves");
  }
  {
    project::NativeSynthDocument document;
    document.media.push_back(
        {std::string(crate::sample_slot), project::NativeBuiltinMedia{"break"}, 0});
    fails(document, "unknown built-in sample");
  }
  // A missing decode is reported, not played.
  auto missing =
      NativeSynthRuntime::create(crate_engine(), plain(), document_for(frames, {{0, 0}}), {}, rate);
  require(!missing.has_value(), "an undecoded sample fails to prepare");

  // Pads follow time order, whatever the slots; each slot's controls follow it.
  auto values = plain();
  values[chop_at(2, "gain")] = -6.0;
  auto runtime = runtime_with(values, frames, {{5, 0}, {2, 24'000}});
  auto events = note(0, 100, 36, 1, 127);
  const auto second = note(1'000, 100, 37, 2, 127);
  events.insert(events.end(), second.begin(), second.end());
  const auto r = render(*runtime, events, 2'000);
  REQUIRE_EQ(r.left[500], 0.25, "pad 1 plays the earliest chop (slot 5)");
  const double expected = 0.25 + 0.5 * std::pow(10.0, -6.0 / 20.0);
  REQUIRE_LT(std::fabs(r.left[1'500] - expected), tolerance::float_round_trip,
             "pad 2 plays slot 2 with slot 2's gain");
  // Keys: every key plays crate.keys_chop's chop, repitched from the root.
  values = plain();
  values[at("crate.layout")] = 1.0;
  values[at("crate.keys_chop")] = 2.0;
  runtime = runtime_with(values, frames, {{5, 0}, {2, 24'000}});
  const auto keys = render(*runtime, note(0, 4'000, 60, 1, 127), 2'000);
  REQUIRE_EQ(keys.left[1'000], 0.5, "the root key plays slot 2 at its own pitch");
}

void public_reader_is_the_warp_read() {
  const auto kernel = warp_dsp::BandlimitedKernel::for_tier(warp_dsp::QualityTier::q0);
  const auto frames = noise(4'096);
  std::vector<float> left(frames.size());
  std::vector<float> right(frames.size());
  for (std::size_t i = 0; i < frames.size(); ++i) {
    left[i] = frames[i].left;
    right[i] = frames[i].right;
  }
  for (const double speed : {0.5, 1.0, 1.7, 4.0}) {
    const auto plan = warp_dsp::make_bandlimited_read_plan(speed);
    REQUIRE_LT(warp_dsp::bandlimited_reach(*kernel, speed), 1'000, "reach is bounded");
    for (std::uint32_t fraction : {0U, 1U, 12'345U, 32'768U, 65'535U}) {
      const std::size_t frame = 2'048;
      const auto planar =
          warp_dsp::bandlimited_read(&left[frame], &right[frame], fraction, *kernel, plan);
      const auto interleaved = warp_dsp::bandlimited_read(&frames[frame], fraction, *kernel, plan);
      REQUIRE_EQ(planar.left, interleaved.left, "planar and interleaved reads are bit-identical");
      REQUIRE_EQ(planar.right, interleaved.right, "planar and interleaved reads are bit-identical");
      if (fraction == 0 && speed <= 1.0)
        require(planar.left == left[frame] && planar.right == right[frame],
                "a whole-frame unity read is the source sample");
    }
  }
}

void identity_read_at_unity() {
  const auto frames = noise(48'000);
  auto runtime = runtime_with(plain(), frames, {{0, 0}});
  const auto r = render(*runtime, note(0, 30'000, 36, 1, 127), 40'000);
  for (std::size_t i = 0; i < 40'000; ++i) {
    require(static_cast<float>(r.left[i]) == frames[i].left &&
                static_cast<float>(r.right[i]) == frames[i].right,
            "frame " + std::to_string(i) + " reads back bit-exactly");
  }
}

void reverse_reads_backward() {
  const auto frames = noise(24'000);
  auto values = plain();
  values[chop_at(0, "reverse")] = 1.0;
  auto runtime = runtime_with(values, frames, {{0, 0}});
  const auto r = render(*runtime, note(0, 100, 36, 1, 127), 30'000);
  for (std::size_t i = 0; i < 23'800; ++i) {
    REQUIRE_EQ(static_cast<float>(r.left[i]), frames[frames.size() - 1 - i].left,
               "reverse reads from the chop's end");
  }
  REQUIRE_EQ(peak(r.left, 24'000), 0.0, "and stops at its start");
}

void modes_shape_the_note() {
  const auto frames = noise(24'000);
  auto values = plain();
  // One-shot: the note-off does not end it; the chop's end does.
  auto runtime = runtime_with(values, frames, {{0, 0}});
  auto r = render(*runtime, note(0, 1'000, 36, 1, 127), 30'000);
  REQUIRE_GT(peak(r.left, 20'000, 21'000), 0.1, "one-shot plays past its note-off");
  REQUIRE_EQ(peak(r.left, 24'000), 0.0, "and ends with its chop");
  // Gate: released at the note-off.
  values[chop_at(0, "mode")] = 1.0;
  runtime = runtime_with(values, frames, {{0, 0}});
  r = render(*runtime, note(0, 6'000, 36, 1, 127), 30'000);
  REQUIRE_GT(peak(r.left, 5'000, 6'000), 0.1, "gate sounds while held");
  REQUIRE_EQ(peak(r.left, 6'000 + 1'441), 0.0, "and is silent a release after the note-off");
  // Loop: whole periods loop seamlessly until the note-off.
  const auto tone = sine(4'800, 100.0);
  values[chop_at(0, "mode")] = 2.0;
  runtime = runtime_with(values, tone, {{0, 0}});
  r = render(*runtime, note(0, 20'000, 36, 1, 127), 24'000);
  double jump = 0.0;
  std::size_t at = 0;
  for (std::size_t i = 1; i < 20'000; ++i) {
    if (std::fabs(r.left[i] - r.left[i - 1]) > jump) {
      jump = std::fabs(r.left[i] - r.left[i - 1]);
      at = i;
    }
  }
  REQUIRE_GT(peak(r.left, 15'000, 20'000), 0.49, "the loop keeps playing at its level");
  REQUIRE_LT(peak(r.left, 0, 20'000), 0.5001, "the loop keeps playing at its level");
  // A click is a step; this loop spans the whole sample, so its join blends
  // the tail with its own opening (no audio either side), steepening the
  // slope a little over 2 ms: up to twice the tone's own step, never a jump.
  REQUIRE_LT(jump, 2.0 * 2.0 * std::numbers::pi * 100.0 * 0.5 / rate,
             "and its joins never click: " + std::to_string(jump) + " at frame " +
                 std::to_string(at));
  REQUIRE_EQ(peak(r.left, 20'000 + 1'441), 0.0, "the loop ends a release after the note-off");
}

void choke_groups_cut_each_other() {
  const auto frames = steps({0.25F, 0.5F}, 24'000);
  auto values = plain();
  for (const std::size_t slot : {0U, 1U})
    values[chop_at(slot, "choke")] = 1.0;
  auto events = note(0, 100, 36, 1, 127);
  const auto second = note(4'800, 100, 37, 2, 127);
  events.insert(events.end(), second.begin(), second.end());
  auto runtime = runtime_with(values, frames, {{0, 0}, {1, 24'000}});
  const auto choked = render(*runtime, events, 12'000);
  REQUIRE_EQ(choked.left[4'000], 0.25, "the first chop sounds alone");
  REQUIRE_GT(choked.left[4'800 + 120], 0.5, "the choked chop fades under the new one...");
  REQUIRE_LT(choked.left[4'800 + 120], 0.75, "the choked chop fades under the new one...");
  REQUIRE_EQ(choked.left[4'800 + 245], 0.5, "...within 5 ms");
  values = plain();
  runtime = runtime_with(values, frames, {{0, 0}, {1, 24'000}});
  const auto open = render(*runtime, events, 12'000);
  REQUIRE_EQ(open.left[6'000], 0.75, "without a group both sound");
}

void ahdsr_lands_on_time() {
  const auto frames = steps({0.5F}, 96'000);
  auto values = plain();
  values[chop_at(0, "mode")] = 1.0;
  values[chop_at(0, "attack")] = 10.0;
  values[chop_at(0, "hold")] = 10.0;
  values[chop_at(0, "decay")] = 20.0;
  values[chop_at(0, "sustain")] = 0.5;
  values[chop_at(0, "release")] = 20.0;
  auto runtime = runtime_with(values, frames, {{0, 0}});
  const auto r = render(*runtime, note(0, 4'800, 36, 1, 127), 7'000);
  REQUIRE_LT(std::fabs(r.left[239] - 0.25), tolerance::float_round_trip, "attack rises linearly");
  REQUIRE_LT(std::fabs(r.left[479] - 0.5), tolerance::float_round_trip, "and peaks after 10 ms");
  REQUIRE_LT(std::fabs(r.left[700] - 0.5), tolerance::float_round_trip,
             "hold stays at the peak for 10 ms");
  REQUIRE_LT(std::fabs(r.left[959] - 0.5), tolerance::float_round_trip,
             "hold stays at the peak for 10 ms");
  REQUIRE_LT(r.left[1'200], 0.5, "decay falls...");
  REQUIRE_GT(r.left[1'200], 0.25, "decay falls...");
  REQUIRE_LT(std::fabs(r.left[1'919] - 0.25), tolerance::float_round_trip,
             "...onto the sustain after 20 ms");
  REQUIRE_LT(std::fabs(r.left[4'000] - 0.25), tolerance::float_round_trip, "sustain holds");
  REQUIRE_GT(r.left[5'758], 0.0, "release lands on zero after 20 ms");
  REQUIRE_EQ(r.left[5'759], 0.0, "release lands on zero after 20 ms");
}

void clean_pitch_up_does_not_alias() {
  auto values = plain();
  values[chop_at(0, "pitch")] = 24.0;
  // 8 kHz two octaves up is 32 kHz: above Nyquist, so it must vanish, and
  // its image at 16 kHz must stay below -80 dB.
  auto runtime = runtime_with(values, sine(96'000, 8'000.0), {{0, 0}});
  auto r = render(*runtime, note(0, 100, 36, 1, 127), 20'000);
  const double image = magnitude_db(r.left, 2'000, 16'384, 0.5);
  REQUIRE_LT(image, -80.0, "no image above -80 dB: " + std::to_string(image));
  // A 2 kHz sine plays at 8 kHz, at its level.
  runtime = runtime_with(values, sine(96'000, 2'000.0), {{0, 0}});
  r = render(*runtime, note(0, 100, 36, 1, 127), 20'000);
  REQUIRE_LT(std::fabs(bin_db(r.left, 2'000, 16'384, 8'000.0, 0.5)), 0.1,
             "the passband is untouched");
}

void eras_set_bits_and_rate() {
  // Noise, not a tone: a quantised tone repeats values near its peaks, which
  // would hide clock ticks from the count.
  const auto tone = noise(96'000);
  const auto changes = [](const std::vector<double>& x, std::size_t begin, std::size_t end) {
    std::size_t count = 0;
    for (std::size_t i = begin + 1; i < end; ++i)
      count += x[i] != x[i - 1] ? 1U : 0U;
    return count;
  };
  auto values = plain();
  values[at("crate.era")] = 1.0; // SP-12, drive off so the knee stays clean.
  values[at("crate.bits")] = 12.0;
  values[at("crate.rate")] = 26'040.0;
  values[at("crate.drive")] = 0.0;
  auto runtime = runtime_with(values, tone, {{0, 0}});
  auto r = render(*runtime, note(0, 100, 36, 1, 127), 60'000);
  bool on_grid = true;
  for (std::size_t i = 0; i < 60'000; ++i) {
    const double scaled = r.left[i] * 2'048.0;
    on_grid = on_grid && scaled == std::round(scaled);
  }
  require(on_grid, "SP-12 plays 12-bit steps");
  const auto held = changes(r.left, 1'000, 49'000);
  require(held > 25'900 && held <= 26'100,
          "SP-12 holds at 26.04 kHz: " + std::to_string(held) + " changes a second");
  // Clean with the rate knob down decimates too.
  values = plain();
  values[at("crate.rate")] = 8'000.0;
  values[at("crate.bits")] = 8.0;
  runtime = runtime_with(values, tone, {{0, 0}});
  r = render(*runtime, note(0, 100, 36, 1, 127), 60'000);
  const auto slow = changes(r.left, 1'000, 49'000);
  REQUIRE_GT(slow, 7'900, "the rate knob sets the clock");
  REQUIRE_LE(slow, 8'100, "the rate knob sets the clock");
  for (std::size_t i = 0; i < 60'000; ++i)
    REQUIRE_EQ(r.left[i] * 128.0, std::round(r.left[i] * 128.0), "8 bits are 8-bit steps");
  // Every era renders finite and bounded audio.
  // The sweep grid is the test input.
  // NOLINTNEXTLINE(bugprone-float-loop-counter,clang-analyzer-security.FloatLoopCounter)
  for (double era = 0.0; era <= 4.0; era += 1.0) {
    values = plain();
    values[at("crate.era")] = era;
    values[at("crate.drive")] = 1.0;
    runtime = runtime_with(values, noise(48'000), {{0, 0}});
    r = render(*runtime, note(0, 100, 36, 1, 127), 30'000);
    require(peak(r.left) > 0.05 && peak(r.left) < 1.5, "era " + std::to_string(era) + " is sane");
  }
}

void follow_tempo_stretches_without_repitching() {
  // Half a second at 120 BPM, played at 60 BPM, lasts a second at 1 kHz.
  const auto tone = sine(24'000, 1'000.0);
  auto values = plain();
  values[at("crate.time")] = 1.0;
  auto runtime = runtime_with(values, tone, {{0, 0}}, 120.0);
  const auto r = render(*runtime, note(0, 100, 36, 1, 127), 60'000, 128, rate, 60.0);
  REQUIRE_GT(peak(r.left, 40'000, 46'000), 0.3, "it plays twice as long");
  REQUIRE_LT(peak(r.left, 48'000 + 2'400), 1e-6, "and then ends");
  const double hz = measured_hz(r.left, 4'800, 40'000, rate);
  REQUIRE_LT(std::fabs(cents(hz, 1'000.0)), 10.0, "at its own pitch: " + std::to_string(hz));
  // The first frames are the source itself: the attack stays sharp.
  for (std::size_t i = 0; i < 1'000; ++i)
    REQUIRE_LT(std::fabs(r.left[i] - tone[i].left), tolerance::float_round_trip,
               "the grain starts on the chop");
  // Repitch ignores the tempo.
  values[at("crate.time")] = 0.0;
  runtime = runtime_with(values, tone, {{0, 0}}, 120.0);
  const auto plainly = render(*runtime, note(0, 100, 36, 1, 127), 30'000, 128, rate, 60.0);
  REQUIRE_EQ(peak(plainly.left, 24'000), 0.0, "repitch plays the chop's own length");
}

// A Follow Tempo loop over a short chop inside a longer sample stays inside
// its chop: its grains wrap at the loop's end rather than play on into the
// next chop's (much louder) audio.
void follow_tempo_loop_stays_in_its_chop() {
  constexpr std::size_t chop_frames = 4'800;
  constexpr float chop_level = 0.25F;
  constexpr float beyond_level = 0.9F;
  constexpr std::size_t held = 40'000;
  constexpr double source_bpm = 120.0;
  constexpr double played_bpm = 60.0;
  constexpr double loop_mode = 2.0;
  constexpr double follow_time = 1.0;
  // Overlapping grain windows may sum a little above one; never to the next chop's level.
  constexpr double ceiling = 0.5;
  constexpr double audible = 0.1;
  const auto frames = steps({chop_level, beyond_level, beyond_level}, chop_frames);
  auto values = plain();
  values[at("crate.time")] = follow_time;
  values[chop_at(0, "mode")] = loop_mode;
  auto runtime = runtime_with(values, frames, {{0, 0}, {1, chop_frames}}, source_bpm);
  const auto r = render(*runtime, note(0, held, 36, 1, 127), held, 128, rate, played_bpm);
  REQUIRE_LT(peak(r.left, 0, held), ceiling, "the loop never plays past its chop");
  REQUIRE_GT(peak(r.left, held - chop_frames, held), audible, "and keeps looping while held");
}

void swing_delays_odd_sixteenths() {
  const auto frames = steps({0.5F}, 48'000);
  auto values = plain();
  values[at("crate.swing")] = 75.0;
  // At 120 BPM a sixteenth is 6000 frames; 75 % swing is half of one late.
  auto runtime = runtime_with(values, frames, {{0, 0}});
  auto r = render(*runtime, note(6'000, 100, 36, 1, 127), 12'000);
  REQUIRE_EQ(peak(r.left, 0, 9'000), 0.0, "an odd sixteenth is late");
  REQUIRE_EQ(r.left[9'000], 0.5, "an odd sixteenth is late");
  runtime = runtime_with(values, frames, {{0, 0}});
  r = render(*runtime, note(12'000, 100, 36, 1, 127), 13'000);
  REQUIRE_EQ(peak(r.left, 0, 12'000), 0.0, "an even one is not");
  REQUIRE_EQ(r.left[12'000], 0.5, "an even one is not");
  // The note-off follows its note-on.
  values[chop_at(0, "mode")] = 1.0;
  values[chop_at(0, "release")] = 1.0;
  runtime = runtime_with(values, frames, {{0, 0}});
  r = render(*runtime, note(6'000, 1'000, 36, 1, 127), 12'000);
  REQUIRE_EQ(r.left[9'990], 0.5, "the gate is as long as played");
  REQUIRE_EQ(peak(r.left, 10'100), 0.0, "the gate is as long as played");
}

void sustain_pedal_holds_a_gate() {
  const auto frames = noise(48'000);
  auto values = plain();
  values[chop_at(0, "mode")] = 1.0;
  auto runtime = runtime_with(values, frames, {{0, 0}});
  const auto off = render(*runtime, note(0, 2'400, 36, 1, 127), 12'000);
  runtime = runtime_with(values, frames, {{0, 0}});
  const auto on =
      render(*runtime, note(0, 2'400, 36, 1, 127), 12'000, 128, rate, 120.0,
             {{0, project::MidiPedal::sustain, 1.0}, {9'000, project::MidiPedal::sustain, 0.0}});
  REQUIRE_EQ(peak(off.left, 6'000), 0.0, "released without the pedal");
  REQUIRE_GT(peak(on.left, 6'000, 9'000), 0.1, "held until the pedal lifts");
  REQUIRE_EQ(peak(on.left, 9'000 + 1'441), 0.0, "held until the pedal lifts");
}

void any_block_split_renders_identically() {
  const auto frames = noise(96'000, 11);
  constexpr std::uint32_t cut_count = 8;
  std::vector<Cut> cuts;
  cuts.reserve(cut_count);
  for (std::uint32_t i = 0; i < cut_count; ++i)
    cuts.push_back({7 - i, static_cast<std::uint64_t>(i) * 12'000});
  const auto check = [&](const std::vector<double>& values, std::string_view what) {
    std::vector<Event> events;
    for (std::uint64_t i = 0; i < 24; ++i) {
      const auto n =
          note(i * 1'500 + (i % 3) * 7, 900 + (i % 5) * 300, static_cast<std::uint8_t>(36 + i % 8),
               i + 1, static_cast<std::uint8_t>(60 + i * 2));
      events.insert(events.end(), n.begin(), n.end());
    }
    std::vector<Rendered> renders;
    for (const std::size_t block : {std::size_t{128}, std::size_t{64}, std::size_t{0}}) {
      auto runtime = runtime_with(values, frames, cuts, 96.0);
      renders.push_back(render(*runtime, events, 48'000, block));
    }
    for (std::size_t i = 1; i < renders.size(); ++i)
      require(renders[i].left == renders[0].left && renders[i].right == renders[0].right,
              std::string(what) + " renders identically under any block split");
    REQUIRE_GT(peak(renders[0].left), 0.01, std::string(what) + " sounds");
  };
  // Each stage alone first, so a failure names the stage.
  const std::vector<std::pair<std::string_view, std::pair<std::string_view, double>>> stages{
      {"swing", {"crate.swing", 62.0}}, {"tape", {"crate.era", 4.0}},
      {"vinyl", {"crate.vinyl", 0.6}},  {"wow", {"crate.wow", 0.7}},
      {"drive", {"crate.drive", 0.4}},  {"sp12", {"crate.era", 1.0}},
      {"mpc60", {"crate.era", 2.0}},    {"s950", {"crate.era", 3.0}},
      {"follow", {"crate.time", 1.0}},
  };
  for (const auto& [name, setting] : stages) {
    auto alone = default_values(table());
    alone[at(setting.first)] = setting.second;
    check(alone, name);
  }
  auto values = default_values(table());
  values[at("crate.swing")] = 62.0;
  values[at("crate.era")] = 4.0;
  values[at("crate.vinyl")] = 0.6;
  values[at("crate.wow")] = 0.7;
  values[at("crate.drive")] = 0.4;
  for (std::size_t slot = 0; slot < 8; ++slot) {
    values[chop_at(slot, "mode")] = static_cast<double>(slot % 3);
    values[chop_at(slot, "reverse")] = static_cast<double>(slot % 2);
    values[chop_at(slot, "filter")] = static_cast<double>(slot % 5);
    values[chop_at(slot, "pitch")] = static_cast<double>(slot) * 3.0 - 9.0;
    values[chop_at(slot, "choke")] = slot < 3 ? 1.0 : 0.0;
  }
  check(values, "repitch through tape");
  values[at("crate.time")] = 1.0;
  values[at("crate.era")] = 3.0;
  check(values, "follow tempo through the S950");
}

void full_load_does_not_allocate() {
  auto values = default_values(table());
  values[at("crate.era")] = 2.0;
  values[at("crate.vinyl")] = 0.5;
  values[at("crate.wow")] = 0.5;
  values[at("crate.swing")] = 60.0;
  values[at("crate.time")] = 1.0;
  values[at("crate.voices")] = 32.0;
  constexpr std::uint32_t cut_count = 32;
  std::vector<Cut> cuts;
  cuts.reserve(cut_count);
  for (std::uint32_t i = 0; i < cut_count; ++i)
    cuts.push_back({i, static_cast<std::uint64_t>(i) * 3'000});
  for (std::size_t slot = 0; slot < 32; ++slot) {
    values[chop_at(slot, "filter")] = static_cast<double>(slot % 5);
    values[chop_at(slot, "mode")] = static_cast<double>(slot % 3);
    values[chop_at(slot, "pitch")] = 12.0;
  }
  auto runtime = runtime_with(values, noise(96'000), cuts, 90.0);
  std::vector<engine::StereoFrame> buffer(128);
  std::vector<engine::BlockMidiEvent> events(1);
  events[0].clip_id_bits = {1, 1};
  events[0].velocity = 120;
  const double beats_per_frame = 124.0 / 60.0 / rate;
  counted_allocations = 0;
  counting_allocations = true;
  for (std::uint64_t b = 0; b < 600; ++b) {
    events[0].note_id_bits = {0, b / 2 + 1};
    events[0].kind = b % 2 == 0 ? engine::MidiEventKind::note_on : engine::MidiEventKind::note_off;
    events[0].key = static_cast<std::uint8_t>(36 + (b / 2) % 32);
    const double beat = static_cast<double>(b * 128) * beats_per_frame;
    const SynthTransport transport{beat, beats_per_frame, beat / 4.0, 4.0};
    (void)runtime->process(buffer, events, b * 128, b < 400 ? 1 : 2, transport, {});
  }
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "the callback allocates nothing");
}

void silent_without_a_sample() {
  auto values = default_values(table());
  values[at("crate.vinyl")] = 1.0;
  auto runtime = make_runtime(crate_engine(), values);
  const auto r = render(*runtime, note(0, 4'800, 36, 1, 127), 9'600);
  REQUIRE_EQ(peak(r.left), 0.0, "no sample is silence");
  REQUIRE_EQ(peak(r.right), 0.0, "no sample is silence");
  auto init = NativeSynthRuntime::create(crate_engine(), preset(table(), "Init"),
                                         crate_engine().preset_document(0), {}, rate);
  REQUIRE_OK(init, "Init prepares");
  const auto quiet = render(*init.value(), note(0, 4'800, 36, 1, 127), 9'600);
  REQUIRE_EQ(peak(quiet.left), 0.0, "Init is silent until a sample is loaded");
}

void telemetry_reports_the_playheads() {
  const auto frames = noise(48'000);
  auto runtime = runtime_with(plain(), frames, {{4, 0}, {9, 24'000}});
  std::vector<float> telemetry;
  (void)render(*runtime, note(0, 100, 37, 1, 99), 6'000);
  REQUIRE_TRUE(runtime->read_telemetry(telemetry), "telemetry is published");
  REQUIRE_EQ(telemetry.size(), crate::telemetry::size, "telemetry is published");
  const float playhead = telemetry[crate::telemetry::playheads + 9];
  REQUIRE_GT(playhead, 0.2F, "slot 9's playhead is a quarter in");
  REQUIRE_LT(playhead, 0.3F, "slot 9's playhead is a quarter in");
  REQUIRE_EQ(telemetry[crate::telemetry::playheads + 4], -1.0F, "slot 4 is idle");
  REQUIRE_EQ(telemetry[crate::telemetry::last_slot], 9.0F, "slot, velocity and peak");
  REQUIRE_EQ(telemetry[crate::telemetry::last_velocity], 99.0F, "slot, velocity and peak");
  REQUIRE_GT(telemetry[crate::telemetry::output_peak_db], -20.0F, "slot, velocity and peak");
}

void presets_render_sane_audio() {
  for (std::size_t i = 0; i < table().presets.size(); ++i) {
    const auto& entry = table().presets[i];
    auto created = NativeSynthRuntime::create(crate_engine(), preset(table(), entry.name),
                                              crate_engine().preset_document(i), {}, rate);
    REQUIRE_OK(created, std::string(entry.name) + " prepares");
    const auto key = static_cast<std::uint8_t>(entry.name == "Pluck" ? 57 : 36);
    const auto r = render(*created.value(), note(0, 24'000, key, 1, 110), 48'000);
    bool finite = true;
    for (const double s : r.left)
      finite = finite && std::isfinite(s);
    require(finite, std::string(entry.name) + " renders finite audio");
    const double level = peak(r.left);
    if (entry.name == "Init") {
      REQUIRE_EQ(level, 0.0, "Init is silent");
      continue;
    }
    require(level > 0.1 && level < 1.0, std::string(entry.name) + " is audible and in range");
    const auto tail = crate_engine().tail_frames(preset(table(), entry.name), rate);
    REQUIRE_GT(tail, 0, std::string(entry.name) + " declares a tail");
  }
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "crate contract passed",
      {
          {"descriptor_registry_and_presets", [] { descriptor_registry_and_presets(); }},
          {"documents_validate_and_compile", [] { documents_validate_and_compile(); }},
          {"public_reader_is_the_warp_read", [] { public_reader_is_the_warp_read(); }},
          {"identity_read_at_unity", [] { identity_read_at_unity(); }},
          {"reverse_reads_backward", [] { reverse_reads_backward(); }},
          {"modes_shape_the_note", [] { modes_shape_the_note(); }},
          {"choke_groups_cut_each_other", [] { choke_groups_cut_each_other(); }},
          {"ahdsr_lands_on_time", [] { ahdsr_lands_on_time(); }},
          {"clean_pitch_up_does_not_alias", [] { clean_pitch_up_does_not_alias(); }},
          {"eras_set_bits_and_rate", [] { eras_set_bits_and_rate(); }},
          {"follow_tempo_stretches_without_repitching",
           [] { follow_tempo_stretches_without_repitching(); }},
          {"follow_tempo_loop_stays_in_its_chop", [] { follow_tempo_loop_stays_in_its_chop(); }},
          {"swing_delays_odd_sixteenths", [] { swing_delays_odd_sixteenths(); }},
          {"sustain_pedal_holds_a_gate", [] { sustain_pedal_holds_a_gate(); }},
          {"any_block_split_renders_identically", [] { any_block_split_renders_identically(); }},
          {"full_load_does_not_allocate", [] { full_load_does_not_allocate(); }},
          {"silent_without_a_sample", [] { silent_without_a_sample(); }},
          {"telemetry_reports_the_playheads", [] { telemetry_reports_the_playheads(); }},
          {"presets_render_sane_audio", [] { presets_render_sane_audio(); }},
      });
}
