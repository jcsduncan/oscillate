#include "oscillate/engine/audio_worker_pool.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_dsp/synth/nova.hpp"
#include "oscillate/native_dsp/synth/nova/parameters.hpp"
#include "oscillate/native_inserts/instrument_binding.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"
#include "synth_test_support.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

// Nova's pads render as they did before the November 2026 performance pass
// (docs/performance/nova-synth.md). Each pad holds a five-note chord, as
// the Chord track plays them, through its whole patch (voices, filters and
// FX rack); the render's fingerprint, window by window, is compared with
// the one recorded from the build before the pass.
//
// The pass is bit-identical except in two places, both far below the
// tolerances here: Formant, Vowel and Phaser filters blend their
// coefficients within a gentle ramp (Whispered Ahh differs by at most
// -144 dBFS), and Expanse's reverb rotates its wander sines between exact
// anchors (at most -150 dBFS). Any audible change moves a fingerprint by
// orders of magnitude more.
//
//   oscillate_nova_render_tests --print   prints the fingerprints
namespace {
using namespace synth_test;
using namespace oscillate::native_inserts;
namespace nova = oscillate::native_dsp::synth::nova;

constexpr double rate = oscillate::test::sample_rate_48k_hz;
constexpr std::size_t window = 24'000; // Half a second.
constexpr std::size_t windows = 8;
constexpr std::uint64_t held = 120'000; // 2.5 s, then 1.5 s of release.
constexpr std::array<std::uint8_t, 5> chord_keys{48, 55, 60, 64, 67};

// One window's level on each side and a projection of the signal on
// pseudo-random signs, which moves with any change to any sample.
struct Window {
  double left_rms;
  double right_rms;
  double projection;
};
using Fingerprint = std::array<Window, windows>;

struct Expected {
  std::string_view preset;
  Fingerprint fingerprint;
};

// Recorded from the build before the pass (--print).
const std::array<Expected, 9> expected{{
#include "nova_render_fingerprints.inc"
}};

std::size_t preset_index(std::string_view name) {
  const auto& presets = nova::descriptor().presets;
  for (std::size_t p = 0; p < presets.size(); ++p) {
    if (presets[p].name == name)
      return p;
  }
  require(false, "Nova has the preset " + std::string(name));
  return 0;
}

// The five-note chord through a preset's whole patch; with `workers`, its
// voices render on those helpers (ADR-0085).
Rendered render_chord(std::string_view name, oscillate::engine::AudioWorkerPool* workers = nullptr,
                      std::size_t frames = window * windows) {
  auto patch = native_patch_for_preset(nova::engine(), preset_index(name));
  REQUIRE_OK(patch, "the preset makes a patch");
  auto resolved = resolve_native_instrument(patch.value());
  REQUIRE_OK(resolved, "the preset resolves");
  auto runtime = NativeSynthRuntime::create(nova::engine(), resolved.value().values,
                                            resolved.value().document, {}, rate);
  REQUIRE_OK(runtime, "a Nova runtime is created");
  runtime.value()->use_workers(workers);
  std::vector<Event> events;
  std::uint64_t id = 1;
  for (const auto key : chord_keys) {
    const auto n = note(0, held, key, id++);
    events.insert(events.end(), n.begin(), n.end());
  }
  return render(*runtime.value(), events, frames);
}

Fingerprint render_fingerprint(std::string_view name) {
  const auto out = render_chord(name);
  Fingerprint fingerprint{};
  std::uint32_t state = 0x2545F491U;
  for (std::size_t w = 0; w < windows; ++w) {
    double left = 0.0, right = 0.0, projection = 0.0;
    for (std::size_t i = w * window; i < (w + 1) * window; ++i) {
      left += out.left[i] * out.left[i];
      right += out.right[i] * out.right[i];
      state = state * 1'664'525U + 1'013'904'223U;
      const double sign = (state >> 31U) != 0U ? 1.0 : -1.0;
      projection += sign * (out.left[i] + out.right[i]);
    }
    fingerprint[w] = {std::sqrt(left / window), std::sqrt(right / window),
                      projection / std::sqrt(static_cast<double>(window))};
  }
  return fingerprint;
}

// Every value to 1e-6 of the window's level (the projection is about the
// level in size). The pass moved them by at most 1.2e-7 of it (Whispered
// Ahh's release); a change 100 dB below the signal moves them by about 1e-5.
constexpr double tolerance = 1e-6;

void matches(const Expected& entry) {
  const auto got = render_fingerprint(entry.preset);
  for (std::size_t w = 0; w < windows; ++w) {
    const auto& want = entry.fingerprint[w];
    const double level = std::max(want.left_rms, want.right_rms);
    const bool same = std::fabs(got[w].left_rms - want.left_rms) <= tolerance * level + 1e-12 &&
                      std::fabs(got[w].right_rms - want.right_rms) <= tolerance * level + 1e-12 &&
                      std::fabs(got[w].projection - want.projection) <= tolerance * level + 1e-12;
    if (!same) {
      std::cerr << entry.preset << " window " << w << ": got " << got[w].left_rms << ' '
                << got[w].right_rms << ' ' << got[w].projection << ", want " << want.left_rms << ' '
                << want.right_rms << ' ' << want.projection << '\n';
    }
    require(same, std::string(entry.preset) + " renders as before the performance pass");
  }
}

// Voices handed to audio helpers sum into the output in voice order, so a
// chord renders to the bit as it does on one thread, whichever helper took
// which voice: unison, noise, both filters, the rack.
constexpr std::size_t helper_threads = 3;
constexpr std::size_t helper_render_frames = 6 * window; // 3 s: held, then released.

void voices_on_helpers_render_identically() {
  for (const std::string_view name : {"Analog Pad", "Whispered Ahh"}) {
    const auto alone = render_chord(name, nullptr, helper_render_frames);
    oscillate::engine::AudioWorkerPool pool(helper_threads);
    pool.start_owned_threads(helper_threads);
    const auto helped = render_chord(name, &pool, helper_render_frames);
    pool.stop_owned_threads();
    for (std::size_t i = 0; i < helper_render_frames; ++i) {
      require(std::bit_cast<std::uint64_t>(alone.left[i]) ==
                      std::bit_cast<std::uint64_t>(helped.left[i]) &&
                  std::bit_cast<std::uint64_t>(alone.right[i]) ==
                      std::bit_cast<std::uint64_t>(helped.right[i]),
              std::string(name) + " renders the same with its voices on helpers");
    }
  }
}

int print() {
  for (const auto& entry : expected) {
    const auto got = render_fingerprint(entry.preset);
    std::printf("    {\"%.*s\",\n     {{", static_cast<int>(entry.preset.size()),
                entry.preset.data());
    for (std::size_t w = 0; w < windows; ++w)
      std::printf("%s{%.17g, %.17g, %.17g}", w == 0 ? "" : ",\n       ", got[w].left_rms,
                  got[w].right_rms, got[w].projection);
    std::printf("}}},\n");
  }
  return 0;
}

const Expected& entry(std::string_view preset) {
  for (const auto& e : expected) {
    if (e.preset == preset)
      return e;
  }
  require(false, "a fingerprint for " + std::string(preset));
  return expected.front();
}

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape): an uncaught exception is the test's failure report.
int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--print")
    return print();
  return oscillate::test::run_cases(
      argc, argv, "Nova pad render contract passed",
      {
          {"analog_pad", [] { matches(entry("Analog Pad")); }},
          {"glass_pad", [] { matches(entry("Glass Pad")); }},
          {"dark_strings", [] { matches(entry("Dark Strings")); }},
          {"spectral_drift", [] { matches(entry("Spectral Drift")); }},
          {"choir_pad", [] { matches(entry("Choir Pad")); }},
          {"warm_pad", [] { matches(entry("Warm Pad")); }},
          {"sweep_pad", [] { matches(entry("Sweep Pad")); }},
          {"warehouse_poly_pad", [] { matches(entry("Warehouse Poly Pad")); }},
          {"whispered_ahh", [] { matches(entry("Whispered Ahh")); }},
          {"voices_on_helpers", voices_on_helpers_render_identically},
      });
}
