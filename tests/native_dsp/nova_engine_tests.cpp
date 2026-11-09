#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_dsp/synth/nova.hpp"
#include "oscillate/native_dsp/synth/nova/parameters.hpp"
#include "oscillate/native_inserts/instrument_binding.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/signal.hpp"
#include "synth_test_support.hpp"

#include <cmath>
#include <cstddef>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

// Nova's engine contract (spec 08 §3–§6, contract 08a): the table, every
// preset, determinism, tuning, unison loudness, voicing and live documents.
namespace {
using namespace synth_test;
using namespace oscillate::native_inserts;
namespace nova = oscillate::native_dsp::synth::nova;

struct Patch {
  std::vector<double> values;
  project::NativeSynthDocument document;
};

Patch preset_patch(std::optional<std::size_t> preset) {
  auto patch = native_patch_for_preset(nova::engine(), preset);
  REQUIRE_OK(patch, "the preset makes a patch");
  auto resolved = resolve_native_instrument(patch.value());
  REQUIRE_OK(resolved, "the preset resolves");
  return {resolved.value().values, resolved.value().document};
}

std::unique_ptr<NativeSynthRuntime> runtime_for(const Patch& patch,
                                                double rate = oscillate::test::sample_rate_48k_hz) {
  auto runtime = NativeSynthRuntime::create(nova::engine(), patch.values, patch.document, {}, rate);
  REQUIRE_OK(runtime, "a Nova runtime is created");
  return std::move(runtime).value();
}

std::vector<Event> chord(std::uint64_t length) {
  std::vector<Event> events;
  std::uint64_t id = 1;
  for (std::uint8_t key :
       {std::uint8_t{48}, std::uint8_t{55}, std::uint8_t{60}, std::uint8_t{64}}) {
    auto n = note(0, length, key, id++);
    events.insert(events.end(), n.begin(), n.end());
  }
  return events;
}

using oscillate::test::rms;

void set(Patch& patch, std::string_view id, double value) {
  patch.values[index(nova::descriptor(), id)] = value;
}

void table_matches_the_contract() {
  const auto& table = nova::descriptor();
  REQUIRE_OK(validate_engine(table), "the table validates");
  REQUIRE_EQ(table.parameters.size(), 1'515, "the table has the contract's 1515 parameters");
  REQUIRE_EQ(find_engine("nova"), &nova::engine(), "Nova is registered");
  const auto& x = nova::index();
  REQUIRE_EQ(table.parameters[x.route_amount[255]].id, "nova.mod.255.amount",
             "resolved indices name the right parameters");
  REQUIRE_EQ(table.parameters[x.fx_slot[23].on].id, "nova.fx.24.on",
             "resolved indices name the right parameters");
  REQUIRE_EQ(table.parameters[x.perf_aftertouch].id, "nova.perf.pressure",
             "resolved indices name the right parameters");
  REQUIRE_EQ(table.parameters[x.lock_mono].id, "nova.lock.mono",
             "resolved indices name the right parameters");
  REQUIRE_EQ(table.parameters[x.analogue.scope].id, "nova.analogue.scope",
             "resolved indices name the right parameters");
  REQUIRE_EQ(table.parameters[x.analogue.flutter_tone].id, "nova.analogue.flutter.tone",
             "resolved indices name the right parameters");
  REQUIRE_EQ(table.parameters[x.noise_am_curve].id, "nova.noise.am.curve",
             "resolved indices name the right parameters");
}

void every_preset_sounds() {
  for (std::size_t p = 0; p < nova::descriptor().presets.size(); ++p) {
    const std::string name(nova::descriptor().presets[p].name);
    auto runtime = runtime_for(preset_patch(p));
    const auto out = render(*runtime, chord(48'000), 96'000);
    double peak_value = 0.0;
    for (std::size_t i = 0; i < out.left.size(); ++i) {
      require(std::isfinite(out.left[i]) && std::isfinite(out.right[i]), name + " is finite");
      peak_value = std::max({peak_value, std::fabs(out.left[i]), std::fabs(out.right[i])});
    }
    REQUIRE_GT(peak_value, 0.01, name + " is audible");
    REQUIRE_LT(peak_value, 4.0, name + " stays in a sane range");
  }
}

void block_splits_and_reruns_are_identical() {
  for (std::size_t p = 0; p < nova::descriptor().presets.size(); ++p) {
    const auto patch = preset_patch(p);
    auto a = runtime_for(patch);
    auto b = runtime_for(patch);
    const auto steady = render(*a, chord(12'000), 24'000, 128);
    const auto irregular = render(*b, chord(12'000), 24'000, 0);
    for (std::size_t i = 0; i < steady.left.size(); ++i) {
      require(steady.left[i] == irregular.left[i] && steady.right[i] == irregular.right[i],
              std::string(nova::descriptor().presets[p].name) +
                  ": any block split renders the same audio");
    }
  }
}

Patch plain_osc(double shape) {
  auto patch = preset_patch(std::nullopt);
  set(patch, "nova.osc.a.type", 0.0);
  set(patch, "nova.osc.a.shape", shape);
  set(patch, "nova.osc.a.drift", 0.0);
  set(patch, "nova.global.drift", 0.0);
  set(patch, "nova.filter.1.on", 0.0);
  set(patch, "nova.osc.a.random_phase", 0.0);
  return patch;
}

void classic_sine_is_in_tune() {
  for (double rate : {44'100.0, 48'000.0, 96'000.0}) {
    auto runtime = runtime_for(plain_osc(0.0), rate);
    const auto frames = static_cast<std::size_t>(rate);
    const auto out = render(*runtime, note(0, frames, 69, 1), frames, 128, rate);
    const double hz = measured_hz(out.left, frames / 4, frames - 100, rate);
    REQUIRE_LT(std::fabs(cents(hz, 440.0)), 0.1, "A4 is 440 Hz within 0.1 cent");
  }
}

void unison_keeps_its_loudness() {
  auto one = plain_osc(2.0);
  auto many = plain_osc(2.0);
  set(many, "nova.osc.a.unison.voices", 16.0);
  set(many, "nova.osc.a.unison.detune", 40.0);
  set(many, "nova.osc.a.unison.blend", 1.0);
  set(many, "nova.osc.a.unison.width", 0.0);
  auto a = runtime_for(one);
  auto b = runtime_for(many);
  const auto x = render(*a, note(0, 96'000, 57, 1), 96'000);
  const auto y = render(*b, note(0, 96'000, 57, 1), 96'000);
  const double difference =
      20.0 * std::log10(rms(y.left, 24'000, 90'000) / rms(x.left, 24'000, 90'000));
  REQUIRE_LT(std::fabs(difference), 1.0, "16 voices are within 1 dB of one");
}

void polyphony_is_bounded() {
  auto patch = plain_osc(2.0);
  set(patch, "nova.global.polyphony", 2.0);
  auto runtime = runtime_for(patch);
  std::vector<Event> notes;
  for (std::uint64_t i = 0; i < 6; ++i) {
    auto n = note(i * 2'000, 40'000, static_cast<std::uint8_t>(60 + i), i + 1);
    notes.insert(notes.end(), n.begin(), n.end());
  }
  const auto out = render(*runtime, notes, 20'000);
  REQUIRE_LE(out.last.active_voices, 3, "two voices play; the rest are stolen with a short fade");
  REQUIRE_GE(out.last.voice_steals, 4, "two voices play; the rest are stolen with a short fade");
}

void documents_change_live() {
  auto patch = plain_osc(2.0);
  set(patch, "nova.filter.1.on", 1.0);
  set(patch, "nova.filter.1.model", 6.0);
  set(patch, "nova.filter.1.cutoff", 200.0);
  auto runtime = runtime_for(patch);
  const auto dark = render(*runtime, note(0, 48'000, 60, 1), 24'000);
  auto routed = patch.document;
  routed.routes.push_back({0, "macro.1", "", "nova.filter.1.cutoff", false, 0.0, 1.0});
  auto values = patch.values;
  values[index(nova::descriptor(), "nova.mod.0.amount")] = 1.0;
  values[index(nova::descriptor(), "nova.macro.1")] = 1.0;
  auto offered = runtime->offer_document(routed, 2);
  REQUIRE_OK(offered, "a route edit is offered live");
  REQUIRE_TRUE(offered.value(), "a route edit is offered live");
  require(runtime->offer_values(values, 2), "with its values");
  auto runtime2 = runtime_for({values, routed});
  const auto bright = render(*runtime2, note(0, 48'000, 60, 1), 24'000);
  REQUIRE_GT(rms(bright.left, 12'000, 24'000), 1.5 * rms(dark.left, 12'000, 24'000),
             "a macro routed to the cutoff opens the filter");
  REQUIRE_LT(runtime->applied_revision(), 2, "nothing is applied before the next block");
}

void telemetry_follows_the_contract() {
  auto runtime = runtime_for(preset_patch(1));
  (void)render(*runtime, chord(24'000), 12'000);
  std::vector<float> frame;
  REQUIRE_TRUE(runtime->read_telemetry(frame), "2038 telemetry values");
  REQUIRE_EQ(frame.size(), 2'038, "2038 telemetry values");
  REQUIRE_EQ(frame[0], 1.0F, "layout version and voices");
  REQUIRE_GE(frame[1], 1.0F, "layout version and voices");
  for (const float value : frame)
    require(std::isfinite(value), "telemetry is finite");
}

} // namespace

int main() {
  try {
    table_matches_the_contract();
    every_preset_sounds();
    block_splits_and_reruns_are_identical();
    classic_sine_is_in_tune();
    unison_keeps_its_loudness();
    polyphony_is_bounded();
    documents_change_live();
    telemetry_follows_the_contract();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "Nova engine contract passed\n";
  return 0;
}
