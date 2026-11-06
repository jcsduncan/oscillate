// The Constellation (ADR-0077): sound measurements on synthetic signals with
// known answers, real Nova renders measured twice alike, the morph and
// mutate rules, and the measurement cache.
//   oscillate_application_sound_map_tests [case...]
#include "oscillate/application/nova_morph.hpp"
#include "oscillate/application/sound_map.hpp"
#include "oscillate/native_dsp/synth/nova.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/native_dsp/synth/project_binding.hpp"
#include "oscillate/persistence/effect_preset.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"
#include "oscillate/test/tolerances.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

namespace {
namespace map = oscillate::application::sound_map;
namespace morph = oscillate::application::nova_morph;
namespace engine = oscillate::engine;
namespace foundation = oscillate::foundation;
namespace persistence = oscillate::persistence;
namespace project = oscillate::project;
namespace synth = oscillate::native_dsp::synth;
namespace fs = std::filesystem;

constexpr double two_pi = 2.0 * std::numbers::pi;
constexpr double amplitude = 0.5;
constexpr auto rate = map::render_rate;
const auto total_frames = static_cast<std::size_t>(map::render_seconds * rate);
const auto held_frames = static_cast<std::size_t>(map::held_seconds * rate);

using oscillate::test::require;

using oscillate::test::take;

const synth::SynthEngine& nova() { return synth::nova::engine(); }
const synth::EngineDescriptor& table() { return *nova().descriptor; }

std::size_t index_of(std::string_view id) {
  const auto index = synth::find_parameter(table(), id);
  REQUIRE_OK(index, "Nova has " + std::string(id));
  return *index;
}

std::size_t preset(std::string_view name) {
  const auto& presets = table().presets;
  for (std::size_t i = 0; i < presets.size(); ++i)
    if (presets[i].name == name)
      return i;
  throw std::runtime_error("no preset " + std::string(name));
}

map::Sound factory(std::string_view name) { return take(map::factory_sound(nova(), preset(name))); }

// A render of `seconds` from a per-frame function of (frame, seconds).
using Signal = std::function<engine::StereoFrame(std::size_t, double)>;
std::vector<engine::StereoFrame> make(const Signal& signal, std::size_t frames = total_frames) {
  std::vector<engine::StereoFrame> out(frames);
  for (std::size_t i = 0; i < frames; ++i)
    out[i] = signal(i, static_cast<double>(i) / rate);
  return out;
}

engine::StereoFrame mono(double value) {
  return {static_cast<float>(value), static_cast<float>(value)};
}

// A deterministic white noise source, -1..1.
class Noise {
public:
  explicit Noise(std::uint64_t seed) : state_(seed) {}
  double next() {
    state_ = state_ * 6'364'136'223'846'793'005ULL + 1'442'695'040'888'963'407ULL;
    return static_cast<double>(state_ >> 11U) * 0x1.0p-52 - 1.0;
  }

private:
  std::uint64_t state_;
};

map::Features measured(const std::vector<engine::StereoFrame>& frames) {
  return map::measure(frames, rate, held_frames);
}

// What the centroid of a pure sine at `hz` must read.
double sine_brightness(double hz) {
  return (std::log2(hz) - std::log2(map::centroid_low_hz)) /
         (std::log2(map::centroid_high_hz) - std::log2(map::centroid_low_hz));
}

void features_from_synthetic_signals() {
  constexpr double low_hz = 200.0;
  constexpr double high_hz = 4'000.0;
  // A Hann window's main lobe spreads a sine over a few bins.
  constexpr double centroid_tolerance = 0.02;
  const auto sine = [](double hz) {
    return make(
        [hz](std::size_t, double t) { return mono(amplitude * std::sin(two_pi * hz * t)); });
  };
  const auto low = measured(sine(low_hz));
  const auto high = measured(sine(high_hz));
  REQUIRE_TRUE(low.audible, "sines are audible");
  REQUIRE_TRUE(high.audible, "sines are audible");
  REQUIRE_LT(std::abs(low.brightness - sine_brightness(low_hz)), centroid_tolerance,
             "a 200 Hz sine's centroid is 200 Hz");
  REQUIRE_LT(std::abs(high.brightness - sine_brightness(high_hz)), centroid_tolerance,
             "a 4 kHz sine's centroid is 4 kHz");

  // White noise is brighter than the same noise through a one-pole lowpass.
  constexpr double lowpass_coefficient = 0.98;
  Noise white_source(7);
  const auto white =
      make([&](std::size_t, double) { return mono(amplitude * white_source.next()); });
  Noise dark_source(7);
  double state = 0.0;
  const auto dark = make([&](std::size_t, double) {
    state = lowpass_coefficient * state + (1.0 - lowpass_coefficient) * dark_source.next();
    return mono(state);
  });
  REQUIRE_GT(measured(white).brightness, measured(dark).brightness + 0.1,
             "white noise is brighter than filtered noise");

  // A sine held to the end with no attack: all of the tail share, none of
  // the attack's.
  REQUIRE_LT(std::abs(low.length - (1.0 - map::attack_share)), 0.01,
             "a held sine with no attack is the tail share long");
  // A 50 ms decay is short; a held note with a slow release is long; a slow
  // attack is longer still.
  constexpr double pluck_seconds = 0.05;
  constexpr double release_seconds = 1.0;
  constexpr double attack_seconds = 0.8;
  const auto pluck = measured(make([](std::size_t, double t) {
    return mono(amplitude * std::exp(-t / pluck_seconds) * std::sin(two_pi * low_hz * t));
  }));
  const auto released = measured(make([](std::size_t, double t) {
    const double level = t < map::held_seconds
                             ? 1.0
                             : std::max(0.0, 1.0 - (t - map::held_seconds) / release_seconds);
    return mono(amplitude * level * std::sin(two_pi * low_hz * t));
  }));
  const auto swell = measured(make([](std::size_t, double t) {
    const double level = std::min(1.0, t / attack_seconds);
    return mono(amplitude * level * std::sin(two_pi * low_hz * t));
  }));
  REQUIRE_LT(pluck.length, released.length, "a pluck is shorter than a released note");
  REQUIRE_LT(released.length, swell.length, "a slow attack is longer");

  // Width: mono 0, opposite channels 1, independent noise about half.
  Noise left(1);
  Noise right(2);
  const auto wide = measured(make([&](std::size_t, double) {
    return engine::StereoFrame{static_cast<float>(amplitude * left.next()),
                               static_cast<float>(amplitude * right.next())};
  }));
  const auto opposite = measured(make([](std::size_t, double t) {
    const double v = amplitude * std::sin(two_pi * low_hz * t);
    return engine::StereoFrame{static_cast<float>(v), static_cast<float>(-v)};
  }));
  REQUIRE_LT(low.width, 1e-9, "a mono sine has no width");
  REQUIRE_LT(std::abs(wide.width - 0.5), 0.05, "independent noise is half side");
  REQUIRE_GT(opposite.width, 1.0 - 1e-9, "opposite channels are all side");

  // Movement: a steady sine barely moves; a sweep over the held part does.
  constexpr double sweep_to_hz = 3'000.0;
  const auto sweep = measured(make([](std::size_t, double t) {
    // Phase of a linear chirp from low_hz to sweep_to_hz over the hold.
    const double rate_hz = (sweep_to_hz - low_hz) / map::held_seconds;
    return mono(amplitude * std::sin(two_pi * (low_hz * t + 0.5 * rate_hz * t * t)));
  }));
  REQUIRE_LT(low.movement, 0.01, "a steady sine is still");
  REQUIRE_GT(sweep.movement, low.movement + 0.05, "a sweep moves");

  // Silence measures as nothing; the same frames measure the same.
  const auto silent = measured(make([](std::size_t, double) { return mono(0.0); }));
  REQUIRE_TRUE(!silent.audible, "silence is not a star");
  REQUIRE_EQ(silent, map::Features{}, "silence is not a star");
  REQUIRE_EQ(measured(sine(low_hz)), low, "measuring is deterministic");
}

void renders_are_deterministic_and_ordered() {
  const auto analyse = [](std::string_view name) {
    const auto category = table().presets[preset(name)].category;
    return take(map::analyse(nova(), factory(name), map::register_for(category)));
  };
  const auto sub = analyse("Deep Sub");
  const auto saw = analyse("Supersaw Lead");
  const auto pluck = analyse("Pluck");
  const auto pad = analyse("Warm Pad");
  REQUIRE_TRUE(sub.audible, "presets sound");
  REQUIRE_TRUE(saw.audible, "presets sound");
  REQUIRE_TRUE(pluck.audible, "presets sound");
  REQUIRE_TRUE(pad.audible, "presets sound");
  REQUIRE_EQ(analyse("Deep Sub"), sub, "the same preset measures the same, bit for bit");
  REQUIRE_EQ(analyse("Warm Pad"), pad, "the same preset measures the same, bit for bit");
  REQUIRE_LT(sub.brightness, saw.brightness, "a sub is darker than a supersaw");
  REQUIRE_LT(pluck.length, pad.length, "a pluck is shorter than a pad");

  // A user preset's text is the same sound.
  const auto sound = factory("Warm Pad");
  auto patch = take(synth::make_native_patch(nova(), sound.values, sound.document));
  project::NativeEffectPatch saved;
  saved.engine = patch.engine;
  saved.engine_version = patch.engine_version;
  saved.parameters = patch.parameters;
  saved.document = patch.document;
  const auto text = persistence::encode_native_effect_preset(saved);
  REQUIRE_EQ(take(map::sound_from_preset_text(nova(), text)), sound,
             "a user preset reads back as its sound");

  // Imported audio cannot be rendered without its project.
  auto imported = sound;
  imported.document.media.push_back(
      {"osc.a",
       project::ManagedAudioReference{project::SourceId::from_bits({0, 1}).value(), "abc", 0, rate,
                                      2, 1},
       0});
  require(!map::render(nova(), imported, map::Register::other), "imported media is refused");
}

double expected_blend(std::string_view id,
                      const std::vector<std::pair<const map::Sound*, double>>& parts) {
  const auto index = index_of(id);
  const auto& parameter = table().parameters[index];
  double position = 0.0;
  double weight = 0.0;
  for (const auto& [sound, w] : parts) {
    position += w * synth::to_normalised(parameter, sound->values[index]);
    weight += w;
  }
  return synth::sanitise(parameter, synth::from_normalised(parameter, position / weight));
}

void morph_rules() {
  const auto init = factory("Init");
  const auto lead = factory("Supersaw Lead");
  const auto sub = factory("Deep Sub");
  const std::vector<morph::Part> parts{{&lead, 0.75}, {&sub, 0.25}};
  const auto blended = take(morph::morph(table(), parts, init, {}));

  // Continuous: in knob space, honouring skew.
  for (const std::string_view id :
       {"nova.filter.1.cutoff", "nova.env.1.release", "nova.osc.a.unison.detune"}) {
    REQUIRE_EQ(blended.values[index_of(id)], expected_blend(id, {{&lead, 0.75}, {&sub, 0.25}}),
               std::string(id) + " blends in knob space");
  }
  // Discrete and the document: the heaviest's.
  const auto voices = index_of("nova.osc.a.unison.voices");
  REQUIRE_EQ(blended.values[voices], lead.values[voices], "a stepped value is the heaviest's");
  REQUIRE_EQ(blended.document, lead.document, "the document is the heaviest's");
  const std::vector<morph::Part> swapped{{&lead, 0.25}, {&sub, 0.75}};
  const auto other = take(morph::morph(table(), swapped, init, {}));
  REQUIRE_EQ(other.values[voices], sub.values[voices], "the heaviest decides");
  REQUIRE_EQ(other.document, sub.document, "the heaviest decides");
  // The performance controls are the player's, not a preset's.
  auto current = init;
  const auto wheel = index_of("nova.perf.wheel");
  current.values[wheel] = 0.5;
  REQUIRE_EQ(take(morph::morph(table(), parts, current, {})).values[wheel], 0.5,
             "the mod wheel stays where it is");

  // Locks keep the current sound's part.
  auto mine = factory("Analog Pad");
  const auto locked = take(morph::morph(table(), parts, mine, {true, true, true}));
  for (std::size_t i = 0; i < table().parameters.size(); ++i) {
    const std::string_view id = table().parameters[i].id;
    if (id.starts_with(morph::fx_prefix) || id.starts_with(morph::envelope_prefix) ||
        (id.starts_with(morph::route_prefix) && id.ends_with(morph::route_amount_suffix)))
      REQUIRE_EQ(locked.values[i], mine.values[i], std::string(id) + " is kept");
  }
  REQUIRE_EQ(locked.document.chains, mine.document.chains, "the effect chains are kept");
  REQUIRE_EQ(locked.document.routes, mine.document.routes, "the routes are kept");

  // Route amounts blend over routes of the same source to the same
  // destination, whatever their slot; absent counts as zero.
  const auto amount = [](std::uint32_t slot) {
    return index_of("nova.mod." + std::to_string(slot) + ".amount");
  };
  auto first = init;
  first.document.routes = {{0, "lfo.1", "", "nova.filter.1.cutoff"}};
  first.values[amount(0)] = 0.8;
  auto second = init;
  second.document.routes = {{5, "lfo.1", "", "nova.filter.1.cutoff"}};
  second.values[amount(5)] = 0.2;
  const auto third = init;
  const std::vector<morph::Part> routed{{&first, 0.5}, {&second, 0.3}, {&third, 0.2}};
  const auto routes = take(morph::morph(table(), routed, init, {}));
  REQUIRE_EQ(routes.document.routes, first.document.routes, "routes are the heaviest's");
  REQUIRE_LT(std::abs(routes.values[amount(0)] - (0.5 * 0.8 + 0.3 * 0.2)),
             tolerance::double_rounding, "a route's amount blends; absent counts as zero");

  // An effect slot's settings blend only over the same module.
  const auto mix = index_of("nova.fx.1.mix");
  auto chorus_a = init;
  chorus_a.document.chains = {{"fx.main", {{1, "chorus"}}}};
  chorus_a.values[mix] = 0.2;
  auto chorus_b = chorus_a;
  chorus_b.values[mix] = 0.6;
  auto reverb = init;
  reverb.document.chains = {{"fx.main", {{1, "reverb"}}}};
  reverb.values[mix] = 1.0;
  const std::vector<morph::Part> effects{{&chorus_a, 0.5}, {&chorus_b, 0.3}, {&reverb, 0.2}};
  const auto fx = take(morph::morph(table(), effects, init, {}));
  REQUIRE_LT(std::abs(fx.values[mix] - (0.5 * 0.2 + 0.3 * 0.6) / 0.8), tolerance::double_rounding,
             "a slot blends only with the same module");

  require(!morph::morph(table(), {}, init, {}), "no parts is no morph");
  const std::vector<morph::Part> weightless{{&lead, 0.0}, {&sub, 0.0}};
  require(!morph::morph(table(), weightless, init, {}), "no weight is no morph");
}

void mutate_rules() {
  const auto base = factory("Analog Pad");
  constexpr std::uint64_t seed = 42;
  const auto variations = morph::mutate(table(), base, seed);
  REQUIRE_EQ(variations.size(), morph::variation_count, "six variations");
  REQUIRE_EQ(morph::mutate(table(), base, seed), variations, "the same seed scatters the same");
  REQUIRE_NE(morph::mutate(table(), base, seed + 1), variations, "another seed scatters anew");
  for (std::size_t v = 0; v < variations.size(); ++v) {
    const auto& variation = variations[v];
    REQUIRE_EQ(variation.document, base.document, "the document stays");
    REQUIRE_NE(variation, base, "a variation differs");
    for (std::size_t w = v + 1; w < variations.size(); ++w)
      REQUIRE_NE(variation, variations[w], "variations differ from each other");
    for (std::size_t i = 0; i < table().parameters.size(); ++i) {
      const auto& parameter = table().parameters[i];
      if (!morph::mutable_parameter(parameter, {})) {
        REQUIRE_EQ(variation.values[i], base.values[i],
                   std::string(parameter.id) + " is not mutated");
        continue;
      }
      const double moved = std::abs(synth::to_normalised(parameter, variation.values[i]) -
                                    synth::to_normalised(parameter, base.values[i]));
      REQUIRE_LE(moved, morph::mutation_spread + 1e-9, "a mutation is modest");
    }
  }
  for (const std::string_view id :
       {"nova.master", "nova.global.polyphony", "nova.osc.a.fine", "nova.perf.wheel"})
    require(!morph::mutable_parameter(table().parameters[index_of(id)], {}),
            std::string(id) + " is never mutated");
  const morph::Locks envelopes{false, true, false};
  for (const auto& variation : morph::mutate(table(), base, seed, envelopes))
    REQUIRE_EQ(variation.values[index_of("nova.env.1.attack")],
               base.values[index_of("nova.env.1.attack")], "a locked envelope stays");
}

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-sound-map") {}
};

void cache_hits_and_invalidates() {
  const Directory directory;
  const auto file = directory.path() / "nested" / "nova.json";
  REQUIRE_EQ(map::FeatureCache::load(file).size(), 0, "no file is an empty cache");

  const map::Features a{0.1, 0.2, 0.3, 0.4, true};
  const map::Features b{0.5, 0.6, 0.7, 0.8, false};
  auto cache = map::FeatureCache::load(file);
  cache.insert(1, a);
  cache.insert(0xffff'ffff'ffff'ffffULL, b);
  REQUIRE_OK(cache.save(), "the cache saves");
  auto loaded = map::FeatureCache::load(file);
  REQUIRE_EQ(loaded.find(1), a, "measurements survive the file");
  REQUIRE_EQ(loaded.find(0xffff'ffff'ffff'ffffULL), b, "measurements survive the file");
  const std::vector<std::uint64_t> keep{1};
  loaded.retain(keep);
  REQUIRE_OK(loaded.save(), "the cache saves again");
  require(!map::FeatureCache::load(file).find(0xffff'ffff'ffff'ffffULL), "retain forgets");

  {
    std::ofstream out(file, std::ios::trunc);
    out << "{not json";
  }
  REQUIRE_EQ(map::FeatureCache::load(file).size(), 0, "a corrupt cache is empty");
  {
    std::ofstream out(file, std::ios::trunc);
    out << R"({"version": )" << map::analysis_version + 1
        << R"(, "sounds": {"0000000000000001": [0.1, 0.2, 0.3, 0.4, 1]}})";
  }
  REQUIRE_EQ(map::FeatureCache::load(file).size(), 0, "another version's cache is empty");

  // Keys follow the sound: editing one sound misses only it.
  std::vector<map::Sound> library{factory("Init"), factory("Pluck"), factory("Warm Pad")};
  std::vector<std::uint64_t> keys;
  for (const auto& sound : library)
    keys.push_back(take(map::sound_key(nova(), sound, map::Register::other)));
  REQUIRE_NE(keys[0], keys[1], "different sounds, different keys");
  REQUIRE_NE(keys[1], keys[2], "different sounds, different keys");
  REQUIRE_EQ(take(map::sound_key(nova(), library[1], map::Register::other)), keys[1],
             "the same sound, the same key");
  REQUIRE_NE(take(map::sound_key(nova(), library[1], map::Register::bass)), keys[1],
             "another phrase, another key");
  map::FeatureCache known;
  for (const auto key : keys)
    known.insert(key, a);
  library[1].values[index_of("nova.filter.1.cutoff")] *= 0.5;
  std::size_t misses = 0;
  for (const auto& sound : library)
    misses += known.find(take(map::sound_key(nova(), sound, map::Register::other))) ? 0U : 1U;
  REQUIRE_EQ(misses, 1, "an edited sound is the only one listened to again");

  // Robust placement: one outlier does not crowd the rest.
  std::vector<map::Features> sky_library;
  for (int i = 0; i < 20; ++i)
    sky_library.push_back({0.4 + 0.01 * i, 0.3 + 0.01 * i, 0.0, 0.0, true});
  sky_library.push_back({1.0, 1.0, 0.0, 0.0, true});
  const map::Sky sky(sky_library);
  const auto lowest = sky.place(sky_library.front());
  const auto middle = sky.place(sky_library[10]);
  REQUIRE_EQ(lowest.x, 0.0, "the library spreads over the sky");
  REQUIRE_GT(middle.x, 0.4, "the library spreads over the sky");
  REQUIRE_LT(middle.x, 0.7, "the library spreads over the sky");
  REQUIRE_EQ(sky.place(sky_library.back()).x, 1.0, "an outlier is clamped to the edge");
  REQUIRE_EQ(middle.movement, 0.5, "an axis without spread places at the middle");
}

} // namespace

int main(int argc, char** argv) {
  const std::vector<std::pair<std::string_view, void (*)()>> cases{
      {"features", features_from_synthetic_signals},
      {"render", renders_are_deterministic_and_ordered},
      {"morph", morph_rules},
      {"mutate", mutate_rules},
      {"cache", cache_hits_and_invalidates},
  };
  const std::vector<std::string_view> wanted(argv + 1, argv + argc);
  try {
    for (const auto& [name, run] : cases)
      if (wanted.empty() || std::ranges::find(wanted, name) != wanted.end())
        run();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "sound map contract passed\n";
  return 0;
}
