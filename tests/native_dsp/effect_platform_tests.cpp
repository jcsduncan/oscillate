#include "effect_test_support.hpp"

#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <new>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

// The effect platform contract (ADR-0040), run over every registered engine
// and every factory preset: a consistent registry, block-split invariance,
// lanes and offers, rejection without side effects, seek resets, silence,
// the tail bound and no allocation on the callback.
namespace {
using namespace effect_test;

std::string label(const effects::EffectEngine& engine, std::string_view preset_name = {}) {
  return std::string(engine.descriptor->id) + (preset_name.empty() ? "" : " / ") +
         std::string(preset_name);
}

void each_preset(const std::function<void(const effects::EffectEngine&, std::string_view,
                                          const std::vector<double>&)>& check) {
  for (const auto* engine : effects::engines()) {
    for (const auto& entry : engine->descriptor->presets) {
      check(*engine, entry.name, preset(*engine->descriptor, entry.name));
    }
  }
}

// Programme-like input: a chord over noise, with a gap so tails show.
Stereo programme(std::size_t frames) {
  auto out = noise(0.2, frames);
  for (const double hz : {110.0, 277.18, 659.25}) {
    const auto tone = sine(hz, 0.15, frames);
    for (std::size_t i = 0; i < frames; ++i) {
      out.left[i] += tone.left[i];
      out.right[i] += tone.right[i] * 0.8;
    }
  }
  for (std::size_t i = frames / 2; i < frames / 2 + frames / 8; ++i) {
    out.left[i] = 0.0;
    out.right[i] = 0.0;
  }
  return out;
}

void registry_is_consistent() {
  require(!effects::engines().empty(), "effects are registered");
  std::set<std::string_view> ids;
  for (const auto* engine : effects::engines()) {
    require(engine->descriptor != nullptr && engine->create_kernel != nullptr &&
                engine->tail_frames != nullptr && engine->latency_frames != nullptr,
            "engine is complete: " + label(*engine));
    REQUIRE_OK(validate_engine(*engine->descriptor), "descriptor validates: " + label(*engine));
    require(ids.insert(engine->descriptor->id).second, "ids are unique: " + label(*engine));
    REQUIRE_EQ(effects::find_engine(engine->descriptor->id), engine, "find_engine finds it");
    require(!engine->descriptor->presets.empty(), "engine has presets: " + label(*engine));
  }
  REQUIRE_EQ(effects::find_engine("no-such-effect"), nullptr, "unknown ids are not found");
}

void block_splits_render_identically() {
  const auto input = programme(24'000);
  each_preset([&](const effects::EffectEngine& engine, std::string_view name,
                  const std::vector<double>& values) {
    const Stereo* side = engine.sidechain ? &input : nullptr;
    auto a = make_effect(engine, values);
    auto b = make_effect(engine, values);
    auto c = make_effect(engine, values);
    const auto x = process(*a, input, {.block = 128, .sidechain = side});
    const auto y = process(*b, input, {.block = 0, .sidechain = side});
    const auto z = process(*c, input, {.block = 1'024, .sidechain = side});
    require(x.left == y.left && x.right == y.right && x.left == z.left && x.right == z.right,
            "block splits are bit-identical: " + label(engine, name));
  });
}

void silence_stays_silent() {
  const Stereo quiet{std::vector<double>(24'000), std::vector<double>(24'000)};
  each_preset([&](const effects::EffectEngine& engine, std::string_view name,
                  const std::vector<double>& values) {
    if (engine.generator) {
      return; // A generator sounds on silence; its own contract covers Off.
    }
    auto runtime = make_effect(engine, values);
    const auto out = process(*runtime, quiet, {.sidechain = engine.sidechain ? &quiet : nullptr});
    require(peak(out.left) < 1e-9 && peak(out.right) < 1e-9,
            "silence in, silence out: " + label(engine, name));
  });
}

void tails_end_within_their_bound() {
  each_preset([&](const effects::EffectEngine& engine, std::string_view name,
                  const std::vector<double>& values) {
    // A tail follows the latency, which hosts account separately; it may be
    // zero for an effect whose output stops with its (delayed) input, as the
    // classic compressor's and limiter's do.
    const auto tail = engine.tail_frames(values, 48'000.0);
    const auto latency = engine.latency_frames(values, 48'000.0);
    REQUIRE_LT(tail, 48'000ULL * 60, "tail is bounded: " + label(engine, name));
    if (engine.generator) {
      return; // Its tail is the fade after Off, which its own contract covers.
    }
    const std::size_t burst = 12'000;
    const std::size_t end = burst + latency + tail;
    auto input = noise(0.5, end + 4'800);
    std::fill(input.left.begin() + burst, input.left.end(), 0.0);
    std::fill(input.right.begin() + burst, input.right.end(), 0.0);
    auto runtime = make_effect(engine, values);
    const auto out = process(*runtime, input);
    // Loudest anywhere before the tail ends: an effect may delay its input
    // past the burst (Flyby's distant source is still travelling).
    const double loud = std::max(peak(out.left, 0, end), peak(out.right, 0, end));
    const double after = std::max(peak(out.left, end), peak(out.right, end));
    REQUIRE_LE(after, loud * 1e-3,
               "the tail falls 60 dB within tail_frames: " + label(engine, name) + " " +
                   std::to_string(db(after / loud)));
  });
}

void a_seek_clears_history() {
  each_preset([&](const effects::EffectEngine& engine, std::string_view name,
                  const std::vector<double>& values) {
    if (engine.generator) {
      return; // A seek restarts a generator rather than silencing it.
    }
    auto runtime = make_effect(engine, values);
    (void)process(*runtime, noise(0.5, 12'000));
    const Stereo quiet{std::vector<double>(4'800), std::vector<double>(4'800)};
    const auto out = process(*runtime, quiet, {.generation = 2}, 96'000);
    require(peak(out.left, 48) < 1e-6 && peak(out.right, 48) < 1e-6,
            "a transport-generation change clears the old signal: " + label(engine, name));
  });
}

std::size_t first_continuous_automatable(const EngineDescriptor& table) {
  for (std::size_t i = 0; i < table.parameters.size(); ++i) {
    if (table.parameters[i].automatable && !is_discrete(table.parameters[i])) {
      return i;
    }
  }
  throw std::runtime_error("no automatable parameter");
}

void constant_lanes_equal_the_same_value() {
  const auto input = programme(12'000);
  for (const auto* engine : effects::engines()) {
    const auto& table = *engine->descriptor;
    auto values = default_values(table);
    const std::size_t index = first_continuous_automatable(table);
    const double value = from_normalised(table.parameters[index], 0.37);
    auto set = values;
    set[index] = value;
    auto expected_runtime = make_effect(*engine, set);
    const auto expected = process(*expected_runtime, input);

    auto runtime = make_effect(*engine, values);
    std::vector<double> lane_values(input.size(), value);
    std::vector<engine::StereoFrame> buffer;
    Stereo out{std::vector<double>(input.size()), std::vector<double>(input.size())};
    for (std::size_t position = 0; position < input.size(); position += 100) {
      const std::size_t size = std::min<std::size_t>(100, input.size() - position);
      buffer.resize(size);
      for (std::size_t i = 0; i < size; ++i) {
        buffer[i] = {static_cast<float>(input.left[position + i]),
                     static_cast<float>(input.right[position + i])};
      }
      const ParameterLane lane{static_cast<std::uint32_t>(index),
                               std::span<const double>(lane_values).subspan(position, size)};
      // The transport process() renders the expected output with.
      const double beats_per_frame = 120.0 / 60.0 / 48'000.0;
      const double beat = static_cast<double>(position) * beats_per_frame;
      const SynthTransport transport{beat, beats_per_frame, beat / 4.0, 4.0};
      REQUIRE_EQ(runtime->process(buffer, {}, position, 1, transport, std::span(&lane, 1)),
                 effects::EffectProcessStatus::rendered, "a lane block renders");
      for (std::size_t i = 0; i < size; ++i) {
        out.left[position + i] = buffer[i].left;
        out.right[position + i] = buffer[i].right;
      }
    }
    require(out.left == expected.left && out.right == expected.right,
            "a constant lane renders like the same panel value: " + label(*engine));
  }
}

void bad_blocks_are_rejected_untouched() {
  for (const auto* engine : effects::engines()) {
    const auto& table = *engine->descriptor;
    auto runtime = make_effect(*engine, default_values(table));
    std::vector<engine::StereoFrame> audio(64, {0.25F, -0.25F});
    const auto original = audio;
    const SynthTransport transport{0.0, 0.0, 0.0, 4.0};
    std::vector<engine::StereoFrame> empty;
    REQUIRE_EQ(runtime->process(empty, {}, 0, 1, transport, {}),
               effects::EffectProcessStatus::invalid_block, "an empty block is rejected");
    std::vector<engine::StereoFrame> huge(engine::max_midi_block_frames + 1);
    REQUIRE_EQ(runtime->process(huge, {}, 0, 1, transport, {}),
               effects::EffectProcessStatus::invalid_block, "an oversized block is rejected");
    std::vector<engine::StereoFrame> short_side(32);
    REQUIRE_EQ(runtime->process(audio, short_side, 0, 1, transport, {}),
               effects::EffectProcessStatus::invalid_sidechain,
               "a mismatched sidechain is rejected");
    const std::size_t index = first_continuous_automatable(table);
    std::vector<double> outside(64, table.parameters[index].maximum + 1.0);
    const ParameterLane bad{static_cast<std::uint32_t>(index), outside};
    REQUIRE_EQ(runtime->process(audio, {}, 0, 1, transport, std::span(&bad, 1)),
               effects::EffectProcessStatus::invalid_lanes, "an out-of-range lane is rejected");
    std::vector<double> short_lane(10, table.parameters[index].default_value);
    const ParameterLane cut{static_cast<std::uint32_t>(index), short_lane};
    REQUIRE_EQ(runtime->process(audio, {}, 0, 1, transport, std::span(&cut, 1)),
               effects::EffectProcessStatus::invalid_lanes,
               "a lane of the wrong length is rejected");
    require(std::equal(audio.begin(), audio.end(), original.begin(),
                       [](const engine::StereoFrame& x, const engine::StereoFrame& y) {
                         return x.left == y.left && x.right == y.right;
                       }),
            "rejected blocks leave the audio alone: " + label(*engine));
  }
}

void offers_follow_revisions() {
  for (const auto* engine : effects::engines()) {
    const auto& table = *engine->descriptor;
    auto values = default_values(table);
    auto runtime = make_effect(*engine, values);
    REQUIRE_EQ(runtime->applied_revision(), 0, "nothing applied before an offer");
    require(!runtime->offer_values(values, 0), "revision zero is refused");
    require(runtime->offer_values(values, 3), "a first offer is accepted");
    require(!runtime->offer_values(values, 3), "a repeated revision is refused");
    auto bad = values;
    bad[0] = table.parameters[0].maximum + 1.0;
    require(!runtime->offer_values(bad, 4), "an invalid value set is refused");
    require(!runtime->offer_values(std::span(values).first(values.size() - 1), 5),
            "a short value set is refused");
    (void)process(*runtime, noise(0.1, 256));
    REQUIRE_EQ(runtime->applied_revision(), 3, "the newest offer is applied at a block");
    std::vector<float> telemetry;
    (void)runtime->read_telemetry(telemetry);
    REQUIRE_EQ(runtime->latency_frames(), engine->latency_frames(values, 48'000.0),
               "latency is fixed at creation");
  }
}

// A panel drag's live values (ADR-0054) are heard exactly as a saved offer
// of the same values, but leave the applied revision naming the last save.
void live_offers_are_heard_without_a_revision() {
  const auto input = programme(4'096);
  for (const auto* engine : effects::engines()) {
    const auto& table = *engine->descriptor;
    const auto values = default_values(table);
    auto moved = values;
    const std::size_t index = first_continuous_automatable(table);
    moved[index] = table.parameters[index].maximum;
    const Stereo* side = engine->sidechain ? &input : nullptr;
    auto saved = make_effect(*engine, values);
    auto live = make_effect(*engine, values);
    require(saved->offer_values(moved, 2) && live->offer_live_values(moved),
            "a live offer is accepted: " + label(*engine));
    const auto heard_saved = process(*saved, input, {.block = 256, .sidechain = side});
    const auto heard_live = process(*live, input, {.block = 256, .sidechain = side});
    require(heard_saved.left == heard_live.left && heard_saved.right == heard_live.right,
            "live values sound as saved ones do: " + label(*engine));
    require(saved->applied_revision() == 2 && live->applied_revision() == 0,
            "a live offer applies no revision: " + label(*engine));
    auto bad = moved;
    bad[index] = table.parameters[index].maximum + 1.0;
    require(!live->offer_live_values(bad), "an invalid live value set is refused");
    // The saved edit at the release still counts from the last saved revision.
    require(live->offer_values(moved, 1), "a live offer moved the revision floor");
    (void)process(*live, noise(0.1, 256));
    REQUIRE_EQ(live->applied_revision(), 1, "the saved edit after a drag is applied");
  }
}

void creation_rejects_bad_input() {
  for (const auto* engine : effects::engines()) {
    auto values = default_values(*engine->descriptor);
    require(!effects::NativeEffectRuntime::create(*engine, values, 1'000.0).has_value(),
            "a bad sample rate is refused");
    values.pop_back();
    require(!effects::NativeEffectRuntime::create(*engine, values, 48'000.0).has_value(),
            "a short value set is refused");
  }
  effects::EffectEngine incomplete;
  require(!effects::NativeEffectRuntime::create(incomplete, {}, 48'000.0).has_value(),
          "an incomplete engine is refused");
}

void the_callback_never_allocates() {
  const auto input = programme(6'000);
  each_preset([&](const effects::EffectEngine& engine, std::string_view name,
                  const std::vector<double>& values) {
    auto runtime = make_effect(engine, values);
    std::vector<engine::StereoFrame> buffer(256);
    std::vector<engine::StereoFrame> side(256, {0.3F, 0.3F});
    const SynthTransport transport{0.0, 120.0 / 60.0 / 48'000.0, 0.0, 4.0};
    auto later = values;
    const auto& first = engine.descriptor->parameters[0];
    later[0] = values[0] == first.minimum ? first.maximum : first.minimum;
    require(runtime->offer_values(later, 1), "a switch is offered");
    counted_allocations = 0;
    counting_allocations = true;
    for (std::size_t block = 0; block < 40; ++block) {
      for (std::size_t i = 0; i < buffer.size(); ++i) {
        buffer[i] = {static_cast<float>(input.left[block * 128 + i % 128]),
                     static_cast<float>(input.right[block * 128 + i % 128])};
      }
      (void)runtime->process(buffer, engine.sidechain ? std::span(side) : std::span(side).first(0),
                             block * buffer.size(), 1, transport, {});
    }
    counting_allocations = false;
    REQUIRE_EQ(counted_allocations, 0, "process never allocates: " + label(engine, name));
  });
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "native effect platform contract passed",
      {
          {"registry_is_consistent", [] { registry_is_consistent(); }},
          {"block_splits_render_identically", [] { block_splits_render_identically(); }},
          {"silence_stays_silent", [] { silence_stays_silent(); }},
          {"tails_end_within_their_bound", [] { tails_end_within_their_bound(); }},
          {"a_seek_clears_history", [] { a_seek_clears_history(); }},
          {"constant_lanes_equal_the_same_value", [] { constant_lanes_equal_the_same_value(); }},
          {"bad_blocks_are_rejected_untouched", [] { bad_blocks_are_rejected_untouched(); }},
          {"offers_follow_revisions", [] { offers_follow_revisions(); }},
          {"live_offers_are_heard_without_a_revision",
           [] { live_offers_are_heard_without_a_revision(); }},
          {"creation_rejects_bad_input", [] { creation_rejects_bad_input(); }},
          {"the_callback_never_allocates", [] { the_callback_never_allocates(); }},
      });
}
