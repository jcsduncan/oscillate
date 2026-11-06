#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/synth/nova/fx.hpp"
#include "oscillate/native_dsp/synth/shaping.hpp"
#include "oscillate/project/model.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Nova's effect rack (spec 08 §7, contract 08a §5): every module turns
// silence into silence and noise into bounded audio at any settings; layouts
// share unchanged instances and retire the rest; adoptions with reorders or
// replacements never click; the rack renders identically however the blocks
// are split; the split crossover sums flat; a synced delay lands on the
// beat; the reverb's RT60 follows Decay; the compressor reduces gain above
// its threshold.
namespace {

namespace nova = oscillate::native_dsp::synth::nova;
using oscillate::engine::StereoFrame;
using oscillate::native_dsp::synth::NoiseColour;
using oscillate::native_dsp::synth::NoiseSource;
using oscillate::native_dsp::synth::SynthMedia;
using oscillate::native_dsp::synth::SynthMediaAsset;
using oscillate::project::NativeChain;
using oscillate::project::NativeChainSlot;
using oscillate::project::NativeSynthDocument;

constexpr double rate = oscillate::test::sample_rate_48k_hz;
constexpr std::size_t block = 128;
constexpr std::size_t max_frames = 256;

using oscillate::test::require;

std::string name(nova::FxKind kind) { return std::string(nova::fx_kind_id(kind)); }

std::vector<StereoFrame> silence(std::size_t frames) {
  return std::vector<StereoFrame>(frames, StereoFrame{0.0F, 0.0F});
}

std::vector<StereoFrame> noise(std::size_t frames, double amplitude, std::uint32_t seed = 7) {
  NoiseSource left;
  NoiseSource right;
  left.reset(seed);
  right.reset(seed * 31U + 3U);
  std::vector<StereoFrame> out(frames);
  for (auto& frame : out) {
    frame.left = static_cast<float>(amplitude * left.next(NoiseColour::white));
    frame.right = static_cast<float>(amplitude * right.next(NoiseColour::white));
  }
  return out;
}

std::vector<StereoFrame> sine(double hz, double amplitude, std::size_t frames) {
  std::vector<StereoFrame> out(frames);
  for (std::size_t i = 0; i < frames; ++i) {
    const auto x = static_cast<float>(
        amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / rate));
    out[i] = {x, x};
  }
  return out;
}

double peak(std::span<const StereoFrame> audio) {
  double p = 0.0;
  for (const auto& f : audio) {
    p = std::max(
        {p, std::fabs(static_cast<double>(f.left)), std::fabs(static_cast<double>(f.right))});
  }
  return p;
}

double rms(std::span<const StereoFrame> audio) {
  double sum = 0.0;
  for (const auto& f : audio) {
    sum += 0.5 * (static_cast<double>(f.left) * f.left + static_cast<double>(f.right) * f.right);
  }
  return audio.empty() ? 0.0 : std::sqrt(sum / static_cast<double>(audio.size()));
}

bool finite(std::span<const StereoFrame> audio) {
  return std::all_of(audio.begin(), audio.end(), [](const StereoFrame& f) {
    return std::isfinite(f.left) && std::isfinite(f.right);
  });
}

// A decaying, decorrelated noise IR at 44.1 kHz, so the module resamples it.
std::shared_ptr<const SynthMediaAsset> test_ir() {
  auto asset = std::make_shared<SynthMediaAsset>();
  asset->source_rate = oscillate::test::sample_rate_44k;
  NoiseSource source;
  source.reset(11);
  const std::size_t frames = 22'050;
  asset->frames.resize(frames);
  for (std::size_t i = 0; i < frames; ++i) {
    const double envelope = std::exp(-6.0 * static_cast<double>(i) / static_cast<double>(frames));
    asset->frames[i] = {static_cast<float>(envelope * source.next(NoiseColour::white)),
                        static_cast<float>(envelope * source.next(NoiseColour::white))};
  }
  return asset;
}

std::unique_ptr<nova::FxModule> make(nova::FxKind kind) {
  auto made = nova::make_fx_module(kind, rate, max_frames,
                                   kind == nova::FxKind::convolve ? test_ir() : nullptr);
  REQUIRE_OK(made, name(kind) + " was not made");
  return std::move(made.value());
}

// Processes `audio` in place through a module in `block`-frame calls.
void run(nova::FxModule& module, std::vector<StereoFrame>& audio, std::size_t size = block) {
  for (std::size_t at = 0; at < audio.size(); at += size) {
    module.process(std::span<StereoFrame>(audio).subspan(at, std::min(size, audio.size() - at)));
  }
}

constexpr std::array<nova::FxKind, nova::fx_kind_count> all_kinds{
    nova::FxKind::distortion, nova::FxKind::filter,     nova::FxKind::chorus,
    nova::FxKind::flanger,    nova::FxKind::phaser,     nova::FxKind::delay,
    nova::FxKind::reverb,     nova::FxKind::compressor, nova::FxKind::multiband,
    nova::FxKind::eq,         nova::FxKind::dimension,  nova::FxKind::hyper,
    nova::FxKind::utility,    nova::FxKind::convolve,   nova::FxKind::shifter,
    nova::FxKind::flyby};

void ids_and_defaults_follow_the_contract() {
  for (const auto kind : all_kinds) {
    REQUIRE_EQ(nova::fx_kind_from_id(nova::fx_kind_id(kind)), kind, name(kind) + " id round trip");
    const auto defaults = nova::fx_defaults(kind);
    for (const double p : defaults.p) {
      require(p >= 0.0 && p <= 1.0, name(kind) + " default out of range");
    }
  }
  require(!nova::fx_kind_from_id("nova.fx.unknown"), "unknown ids are refused");
  REQUIRE_EQ(nova::fx_defaults(nova::FxKind::delay).mix, 0.25, "delay mix default");
  REQUIRE_EQ(nova::fx_defaults(nova::FxKind::phaser).p[7], 0.5, "phaser stage default");
}

void every_module_turns_silence_to_silence_and_noise_to_finite_audio() {
  for (const auto kind : all_kinds) {
    auto module = make(kind);
    const nova::FxContext context{2.0, rate};
    module->set(nova::fx_defaults(kind).p, context, 0);
    auto quiet = silence(24'000);
    run(*module, quiet);
    REQUIRE_LT(peak(quiet), 1e-6, name(kind) + ": silence in is silence out");
    auto loud = noise(24'000, 0.25);
    run(*module, loud);
    require(finite(loud), name(kind) + ": noise gives finite output");
    REQUIRE_LT(peak(loud), 8.0, name(kind) + ": noise stays bounded");
    const std::size_t tail = static_cast<std::size_t>(module->tail_frames());
    REQUIRE_LT(tail, static_cast<std::size_t>(40.0 * rate), name(kind) + ": tail is finite");
    auto after = silence(tail + 9'600);
    run(*module, after);
    const auto last = std::span<const StereoFrame>(after).last(4'800);
    REQUIRE_LT(peak(last), 1e-3,
               name(kind) + ": silent after its tail (" + std::to_string(peak(last)) + ")");
  }
}

void parameter_extremes_stay_bounded() {
  // All low, all high, and the two alternations.
  std::array<nova::FxParams, 4> extremes{};
  for (std::size_t k = 0; k < nova::fx_slot_params; ++k) {
    extremes[1][k] = 1.0;
    extremes[2][k] = k % 2 == 0 ? 1.0 : 0.0;
    extremes[3][k] = k % 2 == 0 ? 0.0 : 1.0;
  }
  for (const auto kind : all_kinds) {
    for (const auto& p : extremes) {
      for (const double factor : {1.0, 4.0}) {
        auto module = make(kind);
        module->set(p, {2.0, rate * factor}, 0);
        auto audio = noise(12'000 * static_cast<std::size_t>(factor), 0.5, 3);
        run(*module, audio, block * static_cast<std::size_t>(factor));
        require(finite(audio), name(kind) + ": extremes stay finite");
        REQUIRE_LT(peak(audio), 64.0,
                   name(kind) + ": extremes stay bounded (" + std::to_string(peak(audio)) +
                       " with p0 " + std::to_string(p[0]) + ", p7 " + std::to_string(p[7]) +
                       " at " + std::to_string(factor) + "x)");
        require(std::isfinite(module->telemetry()), name(kind) + ": telemetry is finite");
      }
    }
  }
}

NativeSynthDocument document(std::vector<NativeChainSlot> main,
                             std::vector<NativeChainSlot> bus1 = {},
                             std::vector<NativeChainSlot> bus2 = {}) {
  NativeSynthDocument doc;
  doc.chains.push_back(NativeChain{"fx.main", std::move(main)});
  if (!bus1.empty()) {
    doc.chains.push_back(NativeChain{"fx.bus.1", std::move(bus1)});
  }
  if (!bus2.empty()) {
    doc.chains.push_back(NativeChain{"fx.bus.2", std::move(bus2)});
  }
  return doc;
}

std::shared_ptr<const nova::FxLayout> layout(const NativeSynthDocument& doc,
                                             const nova::FxLayout* previous = nullptr) {
  auto built = nova::build_fx_layout(doc, rate, max_frames, SynthMedia{}, previous);
  REQUIRE_OK(built, "layout builds");
  return built.value();
}

void layouts_share_unchanged_instances_and_retire_the_rest() {
  const auto first = layout(document({{1, "reverb"}, {2, "delay"}, {3, "eq"}}));
  REQUIRE_EQ(first->chains[0].size(), 3, "first layout");
  REQUIRE_TRUE(first->retiring.empty(), "first layout");
  // uid 2 changes kind, uid 3 leaves, uid 4 arrives on a bus, uid 1 stays.
  const auto second =
      layout(document({{1, "reverb"}, {2, "chorus"}}, {{4, "compressor"}}), first.get());
  REQUIRE_EQ(second->chains[0][0].module, first->chains[0][0].module, "reverb is shared");
  REQUIRE_NE(second->chains[0][1].module, first->chains[0][1].module, "uid 2 is fresh");
  REQUIRE_EQ(second->chains[0][1].kind, nova::FxKind::chorus, "uid 2 is a chorus");
  REQUIRE_EQ(second->chains[1].size(), 1, "bus 1 holds the compressor");
  REQUIRE_EQ(second->retiring.size(), 2, "the delay and the eq retire");
  for (const auto& slot : second->retiring) {
    require(slot.module == first->chains[0][1].module || slot.module == first->chains[0][2].module,
            "retiring holds the old instances");
  }
  // A module moved to another chain keeps its instance.
  const auto third = layout(document({{2, "chorus"}}, {{1, "reverb"}}), second.get());
  REQUIRE_EQ(third->chains[1][0].module, first->chains[0][0].module, "a moved module is shared");

  const auto refused = [](const NativeSynthDocument& doc) {
    return !nova::build_fx_layout(doc, rate, max_frames, SynthMedia{}, nullptr).has_value();
  };
  require(refused(document({{0, "eq"}})), "uid 0 is refused");
  require(refused(document({{25, "eq"}})), "uid 25 is refused");
  require(refused(document({{1, "eq"}}, {{1, "reverb"}})), "a repeated uid is refused");
  require(refused(document({{1, "wobble"}})), "an unknown module is refused");
  NativeSynthDocument bad;
  bad.chains.push_back(NativeChain{"fx.bus.3", {}});
  require(refused(bad), "an unknown chain is refused");
  NativeSynthDocument crowded;
  crowded.chains.push_back(NativeChain{"fx.main", {}});
  for (std::uint32_t uid = 1; uid <= 9; ++uid) {
    crowded.chains[0].modules.push_back({uid, "utility"});
  }
  require(refused(crowded), "nine modules in a chain are refused");
}

nova::FxRackSettings settings_for(std::uint32_t main_factor = 1) {
  nova::FxRackSettings settings;
  settings.oversample = {main_factor, 1, 1};
  for (std::size_t u = 0; u < nova::fx_slot_uids; ++u) {
    settings.slots[u].p.fill(0.5);
    settings.slots[u].mix = 1.0;
    settings.slots[u].on = true;
  }
  return settings;
}

// The largest sample-to-sample step of the left channel.
double largest_step(std::span<const StereoFrame> audio) {
  double step = 0.0;
  for (std::size_t i = 1; i < audio.size(); ++i) {
    step = std::max(step, std::fabs(static_cast<double>(audio[i].left) - audio[i - 1].left));
  }
  return step;
}

void adoptions_with_reorders_or_replacements_do_not_click() {
  for (const bool reorder : {true, false}) {
    nova::FxRack rack;
    rack.prepare(rate, max_frames);
    auto settings = settings_for(2);
    settings.slots[0].p[0] = 0.625; // Utility: +6 dB.
    // EQ: its defaults with a +3.6 dB low shelf (a Utility in this slot
    // reads the shelf's type value as its gain).
    settings.slots[1].p = nova::fx_defaults(nova::FxKind::eq).p;
    settings.slots[1].p[2] = (3.6 + nova::eq_max_db) / (2.0 * nova::eq_max_db);
    const auto first = layout(document({{1, "utility"}, {2, "eq"}}));
    rack.adopt(*first);
    rack.set(settings, {2.0, rate}, 0);
    auto audio = sine(220.0, 0.4, 48'000);
    const std::size_t change = 24'064; // A block boundary.
    std::shared_ptr<const nova::FxLayout> second;
    for (std::size_t at = 0; at < audio.size(); at += block) {
      if (at == change) {
        second = reorder ? layout(document({{2, "eq"}, {1, "utility"}}), first.get())
                         : layout(document({{1, "utility"}, {2, "utility"}}), first.get());
        rack.adopt(*second);
      }
      rack.set(settings, {2.0, rate}, static_cast<std::uint32_t>(block));
      rack.process(std::span<StereoFrame>(audio).subspan(at, block));
    }
    // Steady state before and after the edit (a replacement may change the
    // level).
    const double steady =
        std::max(largest_step(std::span<const StereoFrame>(audio).subspan(12'000, 9'600)),
                 largest_step(std::span<const StereoFrame>(audio).subspan(38'400, 9'600)));
    const double edit =
        largest_step(std::span<const StereoFrame>(audio).subspan(change - 1, 4'800));
    REQUIRE_GT(steady, 0.0, "the sine sounds");
    REQUIRE_LE(edit, 1.5 * steady,
               std::string(reorder ? "reorder" : "replacement") +
                   " steps no more than 1.5x steady state (" + std::to_string(edit / steady) +
                   "x)");
  }
}

std::vector<StereoFrame> render_rack(const std::vector<std::size_t>& splits) {
  nova::FxRack rack;
  rack.prepare(rate, max_frames);
  auto settings = settings_for(2);
  settings.split = true;
  settings.split_hz = 200.0;
  settings.bus_send = {0.4, 0.3};
  settings.bus_send_post = {true, false};
  settings.oversample = {2, 1, 2};
  settings.slots[0].p = nova::fx_defaults(nova::FxKind::chorus).p;
  settings.slots[1].p = nova::fx_defaults(nova::FxKind::distortion).p;
  settings.slots[2].p = nova::fx_defaults(nova::FxKind::delay).p;
  settings.slots[3].p = nova::fx_defaults(nova::FxKind::reverb).p;
  settings.slots[3].mix = 0.5;
  const auto built =
      layout(document({{1, "chorus"}, {2, "distortion"}}, {{3, "delay"}}, {{4, "reverb"}}));
  rack.adopt(*built);
  rack.set(settings, {2.0, rate}, 0);
  auto audio = noise(36'000, 0.3, 5);
  std::size_t at = 0;
  std::size_t k = 0;
  while (at < audio.size()) {
    const std::size_t n = std::min(splits[k++ % splits.size()], audio.size() - at);
    rack.process(std::span<StereoFrame>(audio).subspan(at, n));
    at += n;
  }
  return audio;
}

void the_rack_renders_identically_however_blocks_split() {
  const auto whole = render_rack({256});
  const auto split = render_rack({37, 128, 1, 200, 64, 255});
  require(finite(whole), "rack output is finite");
  double difference = 0.0;
  for (std::size_t i = 0; i < whole.size(); ++i) {
    difference =
        std::max({difference, std::fabs(static_cast<double>(whole[i].left) - split[i].left),
                  std::fabs(static_cast<double>(whole[i].right) - split[i].right)});
  }
  REQUIRE_LT(difference, 1e-6,
             "block splits render identically (" + std::to_string(difference) + ")");
}

void the_split_crossover_sums_flat_with_main_empty() {
  for (const std::uint32_t factor : {1U, 2U, 4U}) {
    for (const double hz : {40.0, 150.0, 1'000.0, 8'000.0}) {
      nova::FxRack rack;
      rack.prepare(rate, max_frames);
      auto settings = settings_for(factor);
      settings.split = true;
      settings.split_hz = 150.0;
      const auto empty = layout(document({}));
      rack.adopt(*empty);
      rack.set(settings, {2.0, rate}, 0);
      auto audio = sine(hz, 0.5, 48'000);
      for (std::size_t at = 0; at < audio.size(); at += block) {
        rack.process(std::span<StereoFrame>(audio).subspan(at, block));
      }
      const double level =
          rms(std::span<const StereoFrame>(audio).last(24'000)) / (0.5 / std::sqrt(2.0));
      REQUIRE_LT(std::fabs(20.0 * std::log10(level)), 0.1,
                 "split sums flat at " + std::to_string(hz) + " Hz, " + std::to_string(factor) +
                     "x (" + std::to_string(20.0 * std::log10(level)) + " dB)");
    }
  }
}

void a_synced_delay_lands_on_the_beat() {
  for (const double bpm : {120.0, 150.0, 97.0}) {
    auto delay = make(nova::FxKind::delay);
    auto p = nova::fx_defaults(nova::FxKind::delay).p;
    p[0] = 10.0 / 14.0; // 1/4
    p[5] = 1.0;         // Sync.
    p[7] = 0.0;         // Stereo.
    delay->set(p, {bpm / 60.0, rate}, 0);
    auto audio = silence(48'000);
    audio[0] = {1.0F, 1.0F};
    run(*delay, audio);
    const auto expected = static_cast<std::size_t>(std::lround(60.0 / bpm * rate));
    std::size_t loudest = 1;
    for (std::size_t i = 1; i < expected + expected / 2; ++i) {
      if (std::fabs(audio[i].left) > std::fabs(audio[loudest].left)) {
        loudest = i;
      }
    }
    const auto miss = loudest > expected ? loudest - expected : expected - loudest;
    REQUIRE_LE(miss, 1, "the first echo lands on the beat at " + std::to_string(bpm) + " BPM");
    REQUIRE_LT(std::fabs(delay->telemetry() - 60'000.0 / bpm), 1.0, "telemetry reports the time");
  }
}

// T20 from Schroeder's backward integral, extrapolated to 60 dB.
double rt60(nova::FxModule& reverb) {
  auto audio = silence(static_cast<std::size_t>(6.0 * rate));
  audio[0] = {1.0F, 1.0F};
  run(reverb, audio);
  std::vector<double> energy(audio.size() + 1, 0.0);
  for (std::size_t i = audio.size(); i-- > 0;) {
    const double l = audio[i].left;
    const double r = audio[i].right;
    energy[i] = energy[i + 1] + l * l + r * r;
  }
  const double total = energy[0];
  std::size_t minus5 = 0;
  std::size_t minus25 = 0;
  for (std::size_t i = 0; i < audio.size(); ++i) {
    const double db = 10.0 * std::log10(energy[i] / total + 1e-30);
    if (minus5 == 0 && db <= -5.0) {
      minus5 = i;
    }
    if (minus25 == 0 && db <= -25.0) {
      minus25 = i;
      break;
    }
  }
  REQUIRE_GT(minus25, minus5, "the reverb decays 25 dB");
  return 3.0 * static_cast<double>(minus25 - minus5) / rate;
}

void reverb_rt60_grows_with_decay() {
  for (const double type : {0.0, 1.0 / 3.0, 2.0 / 3.0, 1.0}) {
    auto shorter = make(nova::FxKind::reverb);
    auto longer = make(nova::FxKind::reverb);
    auto p = nova::fx_defaults(nova::FxKind::reverb).p;
    p[7] = type;
    p[1] = 0.3; // 0.55 s
    shorter->set(p, {2.0, rate}, 0);
    p[1] = 0.6; // 3 s
    longer->set(p, {2.0, rate}, 0);
    const double a = rt60(*shorter);
    const double b = rt60(*longer);
    REQUIRE_GT(b, 2.0 * a,
               "RT60 grows with decay (" + std::to_string(a) + " s -> " + std::to_string(b) +
                   " s)");
  }
}

void the_compressor_reduces_gain_above_threshold() {
  auto p = nova::fx_defaults(nova::FxKind::compressor).p;
  p[0] = 0.5; // -30 dB
  p[1] = 0.6; // ratio 6
  p[2] = 0.2; // fast attack
  p[4] = 0.0; // no makeup
  const auto measure = [&](double amplitude) {
    auto compressor = make(nova::FxKind::compressor);
    compressor->set(p, {2.0, rate}, 0);
    auto audio = sine(1'000.0, amplitude, 48'000);
    run(*compressor, audio);
    const double out = rms(std::span<const StereoFrame>(audio).last(9'600));
    return std::make_pair(20.0 * std::log10(out / (amplitude / std::sqrt(2.0))),
                          static_cast<double>(compressor->telemetry()));
  };
  const auto [loud_db, loud_reduction] = measure(0.5);
  REQUIRE_LT(loud_db, -12.0, "a loud tone is reduced (" + std::to_string(loud_db) + " dB)");
  REQUIRE_GT(loud_reduction, 12.0, "telemetry reports the reduction");
  const auto [quiet_db, quiet_reduction] = measure(0.003);
  REQUIRE_LT(std::fabs(quiet_db), 0.5, "a tone below threshold passes");
  REQUIRE_LT(quiet_reduction, 0.5, "no reduction below threshold");
}

void rack_telemetry_and_meters_are_finite() {
  nova::FxRack rack;
  rack.prepare(rate, max_frames);
  auto settings = settings_for(1);
  settings.bus_send = {0.5, 0.0};
  settings.slots[0].p = nova::fx_defaults(nova::FxKind::compressor).p;
  settings.slots[1].p = nova::fx_defaults(nova::FxKind::chorus).p;
  settings.slots[2].p = nova::fx_defaults(nova::FxKind::multiband).p;
  const auto built = layout(document({{1, "compressor"}, {3, "multiband"}}, {{2, "chorus"}}));
  rack.adopt(*built);
  rack.set(settings, {2.0, rate}, 0);
  auto audio = noise(9'600, 0.5);
  for (std::size_t at = 0; at < audio.size(); at += block) {
    rack.process(std::span<StereoFrame>(audio).subspan(at, block));
  }
  nova::FxRack::Telemetry t;
  rack.telemetry(t);
  const auto all_finite = [](std::span<const float> values) {
    return std::all_of(values.begin(), values.end(), [](float v) { return std::isfinite(v); });
  };
  REQUIRE_TRUE(all_finite(t.live), "every telemetry value is finite");
  REQUIRE_TRUE(all_finite(t.input), "every telemetry value is finite");
  REQUIRE_TRUE(all_finite(t.output), "every telemetry value is finite");
  REQUIRE_TRUE(all_finite(t.detail), "every telemetry value is finite");
  REQUIRE_TRUE(all_finite(t.spectrum), "every telemetry value is finite");
  REQUIRE_GE(t.live[1], 0.0F, "chorus reports its phase");
  REQUIRE_LT(t.live[1], 1.0F, "chorus reports its phase");
  REQUIRE_GT(t.meters[0], -40.0F, "Main and Bus 1 meter their output");
  REQUIRE_GT(t.meters[2], -40.0F, "Main and Bus 1 meter their output");
  REQUIRE_LE(t.meters[4], -119.0F, "an idle Bus 2 meters silence");
  const auto detail =
      std::span<const float>(t.detail).subspan(2 * nova::fx_detail_values, nova::fx_detail_values);
  require(std::any_of(detail.begin(), detail.end(), [](float v) { return std::fabs(v) > 0.5F; }),
          "Multiband reports its band gain changes");
  REQUIRE_LE(t.input[3], -119.0F, "an empty uid meters nothing");
  REQUIRE_LE(t.output[3], -119.0F, "an empty uid meters nothing");
  REQUIRE_GT(rack.tail_frames(), 0, "the rack reports a tail");
}

// The level change in dB a module makes to a sine at `hz` (amplitude 0.25),
// measured on the last half second of two.
double gain_db(nova::FxKind kind, const nova::FxParams& p, double hz) {
  auto module = make(kind);
  module->set(p, {2.0, rate}, 0);
  auto audio = sine(hz, 0.25, 96'000);
  run(*module, audio);
  const double out = rms(std::span<const StereoFrame>(audio).last(24'000));
  return 20.0 * std::log10(std::max(out, 1e-12) / (0.25 / std::sqrt(2.0)));
}

// An EQ with only band 1 on: `type`, at `hz`, `db` and `q`.
nova::FxParams eq_band(nova::EqType type, double hz, double db, double q) {
  auto p = nova::fx_defaults(nova::FxKind::eq).p;
  for (std::size_t b = 0; b < nova::eq_bands; ++b) {
    p[b * nova::eq_band_params] = nova::eq_type_value(nova::EqType::off);
  }
  p[0] = nova::eq_type_value(type);
  p[1] = std::log(hz / nova::eq_min_hz) / std::log(nova::eq_max_hz / nova::eq_min_hz);
  p[2] = (db + nova::eq_max_db) / (2.0 * nova::eq_max_db);
  p[3] = std::log(q / nova::eq_min_q) / std::log(nova::eq_max_q / nova::eq_min_q);
  return p;
}

void the_eq_is_transparent_when_flat() {
  auto module = make(nova::FxKind::eq);
  module->set(nova::fx_defaults(nova::FxKind::eq).p, {2.0, rate}, 0);
  const auto in = noise(9'600, 0.3);
  auto audio = in;
  run(*module, audio);
  double worst = 0.0;
  for (std::size_t i = 0; i < in.size(); ++i) {
    worst = std::max(worst, std::fabs(static_cast<double>(audio[i].left) - in[i].left));
  }
  REQUIRE_LT(worst, 1e-6, "the default EQ (every band at 0 dB) leaves the input untouched");
}

void eq_bands_follow_their_types() {
  const auto near = [](double measured, double expected, double tolerance, const char* what) {
    REQUIRE_LT(std::fabs(measured - expected), tolerance,
               std::string(what) + ": " + std::to_string(measured) + " dB, expected " +
                   std::to_string(expected));
  };
  const auto eq = nova::FxKind::eq;
  near(gain_db(eq, eq_band(nova::EqType::bell, 1'000.0, 12.0, 1.0), 1'000.0), 12.0, 0.3,
       "a bell lifts its centre");
  near(gain_db(eq, eq_band(nova::EqType::bell, 1'000.0, 12.0, 1.0), 60.0), 0.0, 0.3,
       "a bell leaves far frequencies");
  near(gain_db(eq, eq_band(nova::EqType::high_shelf, 2'000.0, 6.0, 0.707), 16'000.0), 6.0, 0.5,
       "a high shelf lifts above it");
  near(gain_db(eq, eq_band(nova::EqType::low_shelf, 200.0, -9.0, 0.707), 40.0), -9.0, 0.5,
       "a low shelf cuts below it");
  near(gain_db(eq, eq_band(nova::EqType::low_cut_12, 400.0, 0.0, 0.707), 100.0), -24.0, 1.5,
       "a 12 dB low cut falls 12 dB an octave");
  near(gain_db(eq, eq_band(nova::EqType::low_cut_24, 400.0, 0.0, 0.707), 100.0), -48.0, 2.0,
       "a 24 dB low cut falls 24 dB an octave");
  near(gain_db(eq, eq_band(nova::EqType::low_cut_24, 400.0, 0.0, 0.707), 400.0), -3.0, 0.3,
       "a Butterworth 24 dB cut is 3 dB down at its corner");
  near(gain_db(eq, eq_band(nova::EqType::high_cut_24, 1'000.0, 0.0, 0.707), 4'000.0), -48.0, 2.0,
       "a 24 dB high cut falls 24 dB an octave");
  REQUIRE_LT(gain_db(eq, eq_band(nova::EqType::notch, 1'000.0, 0.0, 4.0), 1'000.0), -30.0,
             "a notch removes its centre");
  near(gain_db(eq, eq_band(nova::EqType::band_pass, 1'000.0, 0.0, 2.0), 1'000.0), 0.0, 0.3,
       "a band pass passes its centre at unity");
  REQUIRE_LT(gain_db(eq, eq_band(nova::EqType::band_pass, 1'000.0, 0.0, 2.0), 125.0), -20.0,
             "a band pass rejects three octaves down");
}

void an_eq_type_change_does_not_click() {
  auto module = make(nova::FxKind::eq);
  const nova::FxContext context{2.0, rate};
  module->set(eq_band(nova::EqType::bell, 1'000.0, 12.0, 1.0), context, 0);
  auto audio = sine(1'000.0, 0.25, 24'000);
  run(*module, audio);
  const double before = largest_step(std::span<const StereoFrame>(audio).last(4'800));
  module->set(eq_band(nova::EqType::high_cut_24, 300.0, 0.0, 0.707), context, block);
  auto after = sine(1'000.0, 0.25, 24'000);
  run(*module, after);
  REQUIRE_LE(largest_step(std::span<const StereoFrame>(after).first(4'800)), before * 1.05,
             "switching a band's type crossfades");
}

// Hann-windowed DFT magnitude of the left (or right) channel at `hz`.
double magnitude(std::span<const StereoFrame> audio, double hz, bool right = false) {
  double re = 0.0;
  double im = 0.0;
  const double n = static_cast<double>(audio.size());
  for (std::size_t i = 0; i < audio.size(); ++i) {
    const double w = oscillate::native_dsp::dsp::hann(static_cast<double>(i), n);
    const double x = w * (right ? audio[i].right : audio[i].left);
    const double a = 2.0 * std::numbers::pi * hz * static_cast<double>(i) / rate;
    re += x * std::cos(a);
    im -= x * std::sin(a);
  }
  return std::hypot(re, im);
}

void the_shifter_moves_every_partial_by_the_same_hertz() {
  const double shift = 100.0;
  const auto p_for = [](double hz) { return 0.5 * (std::cbrt(hz / nova::shifter_max_hz) + 1.0); };
  const auto render = [&](double hz, double spread, bool ring) {
    auto module = make(nova::FxKind::shifter);
    auto p = nova::fx_defaults(nova::FxKind::shifter).p;
    p[0] = p_for(hz);
    p[2] = spread;
    p[7] = ring ? 1.0 : 0.0;
    module->set(p, {2.0, rate}, 0);
    auto audio = sine(1'000.0, 0.25, 48'000);
    run(*module, audio);
    return std::vector<StereoFrame>(audio.end() - 24'000, audio.end());
  };
  const auto db = [](double a, double b) { return 20.0 * std::log10(a / std::max(b, 1e-12)); };
  const auto up = render(shift, 0.0, false);
  REQUIRE_GT(db(magnitude(up, 1'100.0), magnitude(up, 900.0)), 40.0,
             "a positive shift moves 1 kHz up to 1.1 kHz, not down");
  REQUIRE_GT(db(magnitude(up, 1'100.0), magnitude(up, 1'000.0)), 30.0, "nothing stays at 1 kHz");
  const auto down = render(-shift, 0.0, false);
  REQUIRE_GT(db(magnitude(down, 900.0), magnitude(down, 1'100.0)), 30.0,
             "a negative shift moves it down");
  const auto spread = render(shift, 1.0, false);
  REQUIRE_GT(db(magnitude(spread, 900.0, true), magnitude(spread, 1'100.0, true)), 30.0,
             "full spread turns the right channel the other way");
  const auto ring = render(shift, 0.0, true);
  REQUIRE_LT(std::fabs(db(magnitude(ring, 1'100.0), magnitude(ring, 900.0))), 1.0,
             "ring keeps both sidebands");
  // The panel's harmonic test: 200 Hz and 400 Hz both move by 100 Hz.
  auto module = make(nova::FxKind::shifter);
  auto p = nova::fx_defaults(nova::FxKind::shifter).p;
  p[0] = p_for(shift);
  module->set(p, {2.0, rate}, 0);
  auto chord = sine(200.0, 0.2, 48'000);
  const auto upper = sine(400.0, 0.2, 48'000);
  for (std::size_t i = 0; i < chord.size(); ++i) {
    chord[i].left += upper[i].left;
    chord[i].right += upper[i].right;
  }
  run(*module, chord);
  const std::span<const StereoFrame> tail(chord.end() - 24'000, chord.end());
  REQUIRE_GT(db(magnitude(tail, 300.0), magnitude(tail, 400.0)), 25.0,
             "partials at 200 and 400 Hz land on 300 and 500 Hz (inharmonic)");
  REQUIRE_GT(db(magnitude(tail, 500.0), magnitude(tail, 600.0)), 25.0,
             "partials at 200 and 400 Hz land on 300 and 500 Hz (inharmonic)");
}

void the_rack_reports_each_slot_level_and_spectrum() {
  nova::FxRack rack;
  rack.prepare(rate, max_frames);
  auto settings = settings_for(1);
  settings.slots[4].p = nova::fx_defaults(nova::FxKind::utility).p;
  settings.slots[4].p[0] = 0.25; // -12 dB
  const auto built = layout(document({{5, "utility"}}));
  rack.adopt(*built);
  rack.set(settings, {2.0, rate}, 0);
  auto audio = sine(1'000.0, 0.5, 192 * block);
  for (std::size_t at = 0; at < audio.size(); at += block) {
    rack.process(std::span<StereoFrame>(audio).subspan(at, block));
  }
  nova::FxRack::Telemetry t;
  rack.telemetry(t);
  const double in_db = 20.0 * std::log10(0.5);
  REQUIRE_LT(std::fabs(t.input[4] - in_db), 0.5, "the slot meters its input");
  REQUIRE_LT(std::fabs(t.output[4] - (in_db - 12.0)), 0.5, "and its output after the gain");
  // 1 kHz lies in band floor(48 log(1000 / 20) / log(1000)) = 27.
  const auto spectrum = std::span<const float>(t.spectrum)
                            .subspan(4 * nova::fx_spectrum_bands, nova::fx_spectrum_bands);
  REQUIRE_LT(std::fabs(spectrum[27] - (in_db - 12.0)), 2.0,
             "the spectrum shows the tone at its level (" + std::to_string(spectrum[27]) + " dB)");
  REQUIRE_LT(spectrum[10], -60.0F, "and little far from it");
  REQUIRE_LT(spectrum[44], -60.0F, "and little far from it");
  const auto peak_band = std::max_element(spectrum.begin(), spectrum.end()) - spectrum.begin();
  REQUIRE_EQ(peak_band, 27, "the spectrum peaks in the tone's band");
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "nova fx contract passed",
      {
          {"ids_and_defaults_follow_the_contract", [] { ids_and_defaults_follow_the_contract(); }},
          {"every_module_turns_silence_to_silence_and_noise_to_finite_audio",
           [] { every_module_turns_silence_to_silence_and_noise_to_finite_audio(); }},
          {"parameter_extremes_stay_bounded", [] { parameter_extremes_stay_bounded(); }},
          {"layouts_share_unchanged_instances_and_retire_the_rest",
           [] { layouts_share_unchanged_instances_and_retire_the_rest(); }},
          {"adoptions_with_reorders_or_replacements_do_not_click",
           [] { adoptions_with_reorders_or_replacements_do_not_click(); }},
          {"the_rack_renders_identically_however_blocks_split",
           [] { the_rack_renders_identically_however_blocks_split(); }},
          {"the_split_crossover_sums_flat_with_main_empty",
           [] { the_split_crossover_sums_flat_with_main_empty(); }},
          {"a_synced_delay_lands_on_the_beat", [] { a_synced_delay_lands_on_the_beat(); }},
          {"reverb_rt60_grows_with_decay", [] { reverb_rt60_grows_with_decay(); }},
          {"the_compressor_reduces_gain_above_threshold",
           [] { the_compressor_reduces_gain_above_threshold(); }},
          {"rack_telemetry_and_meters_are_finite", [] { rack_telemetry_and_meters_are_finite(); }},
          {"the_eq_is_transparent_when_flat", [] { the_eq_is_transparent_when_flat(); }},
          {"eq_bands_follow_their_types", [] { eq_bands_follow_their_types(); }},
          {"an_eq_type_change_does_not_click", [] { an_eq_type_change_does_not_click(); }},
          {"the_shifter_moves_every_partial_by_the_same_hertz",
           [] { the_shifter_moves_every_partial_by_the_same_hertz(); }},
          {"the_rack_reports_each_slot_level_and_spectrum",
           [] { the_rack_reports_each_slot_level_and_spectrum(); }},
      });
}
