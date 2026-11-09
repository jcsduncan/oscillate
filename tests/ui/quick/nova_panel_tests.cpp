#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"

#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/nova.hpp"
#include "oscillate/project/model.hpp"
#include "oscillate/timeline/timing.hpp"

#include "bundled_fonts.hpp"
#include "oscillate/test/check.hpp"
#include <QColor>
#include <QCoreApplication>
#include <QDir>

#include <QGuiApplication>
#include <QJSValue>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSet>
#include <QSignalSpy>
#include <QTest>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <memory>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// The Nova panel (spec 08 §2, contract 08a). The panel runs against the
// registered engine's table; a second table written here straight from the
// contract (§2) must match it parameter for parameter, so the engine, the
// panel and the contract cannot drift apart. Every check below holds the
// panel to the engine: each bound control names a real parameter and each
// parameter has a control. Telemetry frames follow the contract (§6).
namespace {
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
using oscillate::quick::NativeSynthGesture;
namespace synth = oscillate::native_dsp::synth;
namespace project = oscillate::project;
using namespace std::string_view_literals;

using oscillate::test::require;
// How long a UI wait may take before it counts as a hang (not a timing claim).
constexpr int ui_wait_ms = 5'000;

// ---- The contract's parameter table (08a §2) -------------------------------------------

constexpr std::array off_on{"Off"sv, "On"sv};
constexpr std::array osc_types{"Classic"sv,     "Wavetable"sv, "Sample"sv,
                               "Multisample"sv, "Granular"sv,  "Spectral"sv};
constexpr std::array shapes{"Sine"sv, "Triangle"sv, "Saw"sv, "Square"sv, "Pulse"sv};
constexpr std::array interpolations{"Crossfade"sv, "Spectral"sv};
constexpr std::array warp_modes{
    "Off"sv, "Bend +"sv, "Bend −"sv, "Sync"sv, "Window Sync"sv, "Mirror"sv, "Asym"sv, "Quantise"sv,
    "PWM"sv, "Flip"sv,   "Remap"sv,  "Fold"sv, "FM"sv,          "PM"sv,     "AM"sv,   "RM"sv};
constexpr std::array warp_sources{"Osc A"sv, "Osc B"sv, "Osc C"sv, "Sub"sv, "Noise"sv};
constexpr std::array unison_curves{"Linear"sv, "Exponential"sv, "Super"sv, "Inverse"sv, "Random"sv};
constexpr std::array stacks{"Off"sv,   "Octave"sv,         "2 Octaves"sv,
                            "Fifth"sv, "Fifth + Octave"sv, "Chord"sv};
constexpr std::array routes{"Filter 1"sv, "Filter 2"sv, "Direct"sv};
constexpr std::array loops{"Off"sv, "Forward"sv, "Ping-pong"sv, "Reverse"sv};
constexpr std::array windows{"Hann"sv, "Gauss"sv, "Tukey"sv, "Triangle"sv};
constexpr std::array ffts{"1024"sv, "2048"sv, "4096"sv, "8192"sv};
constexpr std::array sub_shapes{"Sine"sv, "Triangle"sv, "Saw"sv, "Square"sv};
constexpr std::array sub_octaves{"−2"sv, "−1"sv, "0"sv};
constexpr std::array noise_modes{"Loop"sv, "One-shot"sv};
constexpr std::array filter_models{
    "Ladder 24"sv, "Ladder 12"sv, "Diode"sv,     "MG Low"sv, "OTA"sv,    "LP 12"sv,
    "LP 24"sv,     "BP 12"sv,     "BP 24"sv,     "HP 12"sv,  "HP 24"sv,  "Notch"sv,
    "Peak"sv,      "Formant"sv,   "Vowel"sv,     "Comb +"sv, "Comb −"sv, "Phaser"sv,
    "Flanger"sv,   "LP→BP→HP"sv,  "Dual Peak"sv, "Ring"sv};
constexpr std::array filter_routings{"Serial"sv, "Parallel"sv, "Split"sv};
constexpr std::array lfo_syncs{"Hz"sv,   "8 bars"sv, "4 bars"sv, "2 bars"sv, "1 bar"sv,  "1/2 D"sv,
                               "1/2"sv,  "1/2 T"sv,  "1/4 D"sv,  "1/4"sv,    "1/4 T"sv,  "1/8 D"sv,
                               "1/8"sv,  "1/8 T"sv,  "1/16 D"sv, "1/16"sv,   "1/16 T"sv, "1/32 D"sv,
                               "1/32"sv, "1/32 T"sv, "1/64"sv};
constexpr std::array lfo_modes{"Trig"sv, "Env"sv, "Free"sv};
constexpr std::array scopes{"Voice"sv, "Global"sv};
constexpr std::array oversamples{"Off"sv, "2×"sv, "4×"sv};
constexpr std::array send_points{"Pre"sv, "Post"sv};
constexpr std::array arp_modes{"Up"sv,       "Down"sv,    "Up/Down"sv, "Down/Up"sv,
                               "Converge"sv, "Diverge"sv, "Played"sv,  "Thumb"sv,
                               "Pinky"sv,    "Random"sv,  "Shuffle"sv, "Random walk"sv,
                               "Chord"sv,    "Strum"sv,   "Shape"sv,   "Euclidean"sv};
constexpr std::array arp_note_modes{"Held"sv, "Scale"sv};
constexpr std::array arp_scales{"Chord"sv,
                                "Major"sv,
                                "Minor"sv,
                                "Dorian"sv,
                                "Phrygian"sv,
                                "Lydian"sv,
                                "Mixolydian"sv,
                                "Locrian"sv,
                                "Harmonic minor"sv,
                                "Melodic minor"sv,
                                "Major pentatonic"sv,
                                "Minor pentatonic"sv,
                                "Blues"sv,
                                "Chromatic"sv};
constexpr std::array arp_roots{"C"sv,  "C#"sv, "D"sv,  "D#"sv, "E"sv,  "F"sv,
                               "F#"sv, "G"sv,  "G#"sv, "A"sv,  "A#"sv, "B"sv};
constexpr std::array arp_variations{"Off"sv, "2 bars"sv, "4 bars"sv, "8 bars"sv, "16 bars"sv};
constexpr std::array arp_groove_grids{"1/4"sv, "1/8"sv, "1/8 T"sv, "1/16"sv, "1/16 T"sv, "1/32"sv};
constexpr std::array arp_conditions{"Always"sv, "1:2"sv,      "2:2"sv,   "1:3"sv,      "2:3"sv,
                                    "3:3"sv,    "1:4"sv,      "2:4"sv,   "3:4"sv,      "4:4"sv,
                                    "Fill"sv,   "Not fill"sv, "First"sv, "Not first"sv};
constexpr std::array arp_chords{"Off"sv, "+Oct"sv, "5th"sv, "Triad"sv, "7th"sv, "Sus"sv};
constexpr std::array arp_rates{"1/1"sv,   "1/2"sv,    "1/2 T"sv, "1/4 D"sv,  "1/4"sv,
                               "1/4 T"sv, "1/8 D"sv,  "1/8"sv,   "1/8 T"sv,  "1/16 D"sv,
                               "1/16"sv,  "1/16 T"sv, "1/32"sv,  "1/32 T"sv, "1/64"sv};
constexpr std::array arp_velocities{"Played"sv, "Pattern"sv};
constexpr std::array clips{"Clip 1"sv, "Clip 2"sv, "Clip 3"sv, "Clip 4"sv};
constexpr std::array triggers{"Transport"sv, "Note"sv};
constexpr std::array voicings{"Poly"sv, "Mono"sv, "Legato"sv};
constexpr std::array porta_modes{"Legato only"sv, "Always"sv};
constexpr std::array porta_rates{"Constant time"sv, "Constant rate"sv};
constexpr std::array qualities{"Eco"sv, "High"sv, "Ultra"sv};
constexpr std::array tunings{"12-TET"sv, "Scale file"sv};
constexpr std::array analogue_scopes{"Notes + unison"sv, "Notes only"sv, "Unison only"sv};
constexpr std::array noise_am_sources{"Off"sv, "Osc A"sv, "Osc B"sv, "Osc C"sv};
constexpr std::array porta_divisions{
    "1/1"sv,    "1/2"sv,  "1/4 D"sv,  "1/4"sv,    "1/4 T"sv, "1/8 D"sv,  "1/8"sv,  "1/8 T"sv,
    "1/16 D"sv, "1/16"sv, "1/16 T"sv, "1/32 D"sv, "1/32"sv,  "1/32 T"sv, "1/64"sv, "1/128"sv};

// Values per effect slot (08a §2.10): p0..p15.
constexpr int fx_slot_params = 16;

class ContractTable {
public:
  ContractTable() { build(); }
  [[nodiscard]] const oscillate::native_dsp::EngineDescriptor& descriptor() const {
    return descriptor_;
  }

private:
  std::string_view keep(std::string text) {
    text_.push_back(std::move(text));
    return text_.back();
  }
  // A descriptor's fields, in its own order, as the tables list them.
  // NOLINTBEGIN(bugprone-easily-swappable-parameters)
  void add(std::string id, std::string name, std::string group, std::string_view unit,
           oscillate::native_dsp::ParameterScale scale, double minimum, double maximum,
           double fallback, std::span<const std::string_view> choices, bool automatable,
           double skew = 1.0) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    oscillate::native_dsp::ParameterDescriptor p;
    p.id = keep(std::move(id));
    p.name = keep(std::move(name));
    p.group = keep(std::move(group));
    p.unit = unit;
    p.scale = scale;
    p.minimum = minimum;
    p.maximum = maximum;
    p.default_value = fallback;
    p.choices = choices;
    p.glide_seconds = automatable ? 0.02 : 0.0;
    p.automatable = automatable;
    p.skew = skew;
    parameters_.push_back(p);
  }
  void lin(const std::string& id, const std::string& name, const std::string& group,
           std::string_view unit, double lo, double hi, double fallback, double skew = 1.0,
           bool automatable = true) {
    add(id, name, group, unit, oscillate::native_dsp::ParameterScale::linear, lo, hi, fallback, {},
        automatable, skew);
  }
  void log(const std::string& id, const std::string& name, const std::string& group,
           std::string_view unit, double lo, double hi, double fallback) {
    add(id, name, group, unit, oscillate::native_dsp::ParameterScale::logarithmic, lo, hi, fallback,
        {}, true);
  }
  void step(const std::string& id, const std::string& name, const std::string& group,
            std::string_view unit, double lo, double hi, double fallback) {
    add(id, name, group, unit, oscillate::native_dsp::ParameterScale::stepped, lo, hi, fallback, {},
        false);
  }
  void choice(const std::string& id, const std::string& name, const std::string& group,
              std::span<const std::string_view> choices, double fallback) {
    add(id, name, group, "", oscillate::native_dsp::ParameterScale::choice, 0,
        static_cast<double>(choices.size() - 1), fallback, choices, false);
  }

  void build() {
    lin("nova.master", "Master", "Output", "dB", -60, 6, 0);
    const std::array<std::string, 3> oscs{"a", "b", "c"};
    for (std::size_t o = 0; o < oscs.size(); ++o) {
      const std::string base = "nova.osc." + oscs[o];
      const std::string group = "Osc " + std::string(1, static_cast<char>('A' + o));
      choice(base + ".on", "On", group, off_on, o == 0 ? 1 : 0);
      choice(base + ".type", "Type", group, osc_types, o == 0 ? 1 : 0);
      lin(base + ".level", "Level", group, "%", 0, 1, 0.75);
      lin(base + ".pan", "Pan", group, "%", -1, 1, 0);
      lin(base + ".position", "Position", group, "%", 0, 1, 0);
      choice(base + ".shape", "Shape", group, shapes, 2);
      lin(base + ".pw", "Pulse Width", group, "%", 0.01, 0.99, 0.5);
      lin(base + ".sync", "Sync", group, "×", 1, 8, 1);
      lin(base + ".drift", "Drift", group, "%", 0, 1, 0.15);
      choice(base + ".interpolation", "Interpolation", group, interpolations, 0);
      for (int w = 1; w <= 2; ++w) {
        const std::string warp = base + ".warp." + std::to_string(w);
        choice(warp + ".mode", "Warp " + std::to_string(w), group, warp_modes, 0);
        lin(warp + ".amount", "Warp " + std::to_string(w) + " Amount", group, "%", 0, 1, 0);
        choice(warp + ".source", "Warp " + std::to_string(w) + " Source", group, warp_sources,
               static_cast<double>((o + 1) % 3));
      }
      step(base + ".unison.voices", "Unison", group, "", 1, 16, 1);
      lin(base + ".unison.detune", "Detune", group, "ct", 0, 1200, 18, 3);
      choice(base + ".unison.curve", "Detune Curve", group, unison_curves, 2);
      lin(base + ".unison.blend", "Blend", group, "%", 0, 1, 0.6);
      lin(base + ".unison.width", "Width", group, "%", 0, 1, 0.8);
      choice(base + ".unison.stack", "Stack", group, stacks, 0);
      lin(base + ".unison.position_spread", "Position Spread", group, "%", 0, 1, 0);
      lin(base + ".unison.warp_spread", "Warp Spread", group, "%", 0, 1, 0);
      step(base + ".octave", "Octave", group, "", -4, 4, 0);
      step(base + ".semitone", "Semitone", group, "st", -24, 24, 0);
      lin(base + ".fine", "Fine", group, "ct", -100, 100, 0);
      lin(base + ".pitch", "Pitch", group, "st", -48, 48, 0);
      lin(base + ".phase", "Phase", group, "°", 0, 360, 0);
      lin(base + ".random_phase", "Random Phase", group, "%", 0, 1, o == 1 ? 0.3 : 1.0);
      choice(base + ".route", "Route", group, routes, 0);
      step(base + ".sample.root", "Root", group, "", 0, 127, 60);
      choice(base + ".sample.keytrack", "Keytrack", group, off_on, 1);
      lin(base + ".sample.start", "Start", group, "%", 0, 1, 0);
      choice(base + ".sample.loop", "Loop", group, loops, 1);
      lin(base + ".sample.loop_start", "Loop Start", group, "%", 0, 1, 0);
      lin(base + ".sample.loop_end", "Loop End", group, "%", 0, 1, 1);
      lin(base + ".sample.crossfade", "Crossfade", group, "%", 0, 1, 0.05);
      log(base + ".grain.size", "Grain Size", group, "ms", 10, 500, 80);
      log(base + ".grain.density", "Density", group, "Hz", 1, 200, 24);
      lin(base + ".grain.scan_rate", "Scan Rate", group, "×", -2, 2, 0);
      lin(base + ".grain.spray", "Spray", group, "%", 0, 1, 0.2);
      lin(base + ".grain.pitch_spray", "Pitch Spray", group, "st", 0, 24, 0);
      choice(base + ".grain.window", "Window", group, windows, 0);
      lin(base + ".grain.reverse", "Reverse", group, "%", 0, 1, 0);
      choice(base + ".spectral.fft", "FFT", group, ffts, 2);
      lin(base + ".spectral.shift", "Shift", group, "st", -24, 24, 0);
      lin(base + ".spectral.stretch", "Stretch", group, "×", 0.5, 2, 1);
      lin(base + ".spectral.smear", "Smear", group, "%", 0, 1, 0);
      lin(base + ".spectral.tilt", "Tilt", group, "dB", -12, 12, 0);
      choice(base + ".spectral.freeze", "Freeze", group, off_on, 0);
    }
    choice("nova.sub.on", "On", "Sub", off_on, 0);
    choice("nova.sub.shape", "Shape", "Sub", sub_shapes, 0);
    choice("nova.sub.octave", "Octave", "Sub", sub_octaves, 1);
    lin("nova.sub.level", "Level", "Sub", "%", 0, 1, 0.5);
    lin("nova.sub.pan", "Pan", "Sub", "%", -1, 1, 0);
    choice("nova.sub.route", "Route", "Sub", routes, 2);
    choice("nova.noise.on", "On", "Noise", off_on, 0);
    lin("nova.noise.level", "Level", "Noise", "%", 0, 1, 0.15);
    lin("nova.noise.pitch", "Pitch", "Noise", "st", -48, 48, 0);
    choice("nova.noise.keytrack", "Keytrack", "Noise", off_on, 0);
    lin("nova.noise.tone", "Tone", "Noise", "%", -1, 1, 0);
    choice("nova.noise.mode", "Mode", "Noise", noise_modes, 0);
    lin("nova.noise.pan", "Pan", "Noise", "%", -1, 1, 0);
    choice("nova.noise.route", "Route", "Noise", routes, 0);
    for (int f = 1; f <= 2; ++f) {
      const std::string base = "nova.filter." + std::to_string(f);
      const std::string group = "Filter " + std::to_string(f);
      choice(base + ".on", "On", group, off_on, f == 1 ? 1 : 0);
      choice(base + ".model", "Model", group, filter_models, f == 1 ? 0 : 13);
      log(base + ".cutoff", "Cutoff", group, "Hz", 8, 22'000, f == 1 ? 2'000 : 800);
      lin(base + ".resonance", "Resonance", group, "%", 0, 1, 0.2);
      lin(base + ".drive", "Drive", group, "dB", 0, 24, 0);
      lin(base + ".fat", "Fat", group, "%", 0, 1, 0);
      lin(base + ".keytrack", "Keytrack", group, "%", -1, 2, 0);
      lin(base + ".pan", "Pan", group, "%", -1, 1, 0);
      lin(base + ".mix", "Mix", group, "%", 0, 1, 1);
    }
    choice("nova.filter.routing", "Routing", "Filters", filter_routings, 0);
    for (int e = 1; e <= 4; ++e) {
      const std::string base = "nova.env." + std::to_string(e);
      const std::string group = "Env " + std::to_string(e);
      const bool amp = e == 1;
      lin(base + ".attack", "Attack", group, "s", 0, 20, amp ? 0.002 : 0.01, 4);
      lin(base + ".hold", "Hold", group, "s", 0, 10, 0, 4);
      lin(base + ".decay", "Decay", group, "s", 0, 20, amp ? 0.3 : 0.5, 4);
      lin(base + ".sustain", "Sustain", group, "%", 0, 1, amp ? 1.0 : 0.4);
      lin(base + ".release", "Release", group, "s", 0, 20, amp ? 0.15 : 0.4, 4);
      lin(base + ".attack_curve", "Attack Curve", group, "%", -1, 1, 0);
      lin(base + ".decay_curve", "Decay Curve", group, "%", -1, 1, 0.4);
      lin(base + ".release_curve", "Release Curve", group, "%", -1, 1, 0.4);
      choice(base + ".loop", "Loop", group, off_on, 0);
      choice(base + ".legato", "Legato", group, off_on, 0);
      lin(base + ".velocity", "Velocity", group, "%", 0, 1, amp ? 0.7 : 0.0);
    }
    for (int l = 1; l <= 8; ++l) {
      const std::string base = "nova.lfo." + std::to_string(l);
      const std::string group = "LFO " + std::to_string(l);
      log(base + ".rate", "Rate", group, "Hz", 0.01, 50, 2);
      choice(base + ".sync", "Sync", group, lfo_syncs, 9);
      choice(base + ".mode", "Mode", group, lfo_modes, 0);
      lin(base + ".rise", "Rise", group, "s", 0, 10, 0, 3);
      lin(base + ".delay", "Delay", group, "s", 0, 10, 0, 3);
      lin(base + ".smooth", "Smooth", group, "%", 0, 1, 0);
      lin(base + ".phase", "Phase", group, "°", 0, 360, 0);
      choice(base + ".scope", "Scope", group, scopes, 0);
      step(base + ".grid", "Grid", group, "", 1, 32, 8);
    }
    for (int m = 1; m <= 8; ++m) {
      lin("nova.macro." + std::to_string(m), "Macro " + std::to_string(m), "Macros", "%", 0, 1, 0);
    }
    for (int c = 1; c <= 2; ++c) {
      const std::string group = "Chaos " + std::to_string(c);
      log("nova.chaos." + std::to_string(c) + ".rate", "Rate", group, "Hz", 0.01, 50, 1);
      lin("nova.chaos." + std::to_string(c) + ".smooth", "Smooth", group, "%", 0, 1, 0.5);
    }
    for (int s = 0; s < 256; ++s) {
      lin("nova.mod." + std::to_string(s) + ".amount", "Route " + std::to_string(s) + " Amount",
          "Modulation", "%", -1, 1, 0);
    }
    choice("nova.fx.split", "Split", "FX", off_on, 0);
    log("nova.fx.split_frequency", "Split Frequency", "FX", "Hz", 40, 2'000, 150);
    choice("nova.fx.main.oversample", "Main Oversampling", "FX", oversamples, 1);
    for (int b = 1; b <= 2; ++b) {
      const std::string base = "nova.fx.bus." + std::to_string(b);
      const std::string group = "Bus " + std::to_string(b);
      choice(base + ".oversample", "Oversampling", group, oversamples, 0);
      lin(base + ".send", "Send", group, "%", 0, 1, 0);
      choice(base + ".send_point", "Send Point", group, send_points, 1);
      lin(base + ".return", "Return", group, "dB", -60, 6, 0);
    }
    for (int u = 1; u <= 24; ++u) {
      const std::string base = "nova.fx." + std::to_string(u);
      const std::string group = "FX " + std::to_string(u);
      for (int k = 0; k < fx_slot_params; ++k) {
        lin(base + ".p" + std::to_string(k), "Knob " + std::to_string(k + 1), group, "%", 0, 1, 0);
      }
      lin(base + ".mix", "Mix", group, "%", 0, 1, 1);
      choice(base + ".on", "On", group, off_on, 1);
    }
    choice("nova.arp.on", "On", "Arpeggiator", off_on, 0);
    choice("nova.arp.mode", "Mode", "Arpeggiator", arp_modes, 0);
    choice("nova.arp.rate", "Rate", "Arpeggiator", arp_rates, 10);
    lin("nova.arp.gate", "Gate", "Arpeggiator", "%", 0, 1, 0.6);
    lin("nova.arp.swing", "Swing", "Arpeggiator", "%", 0, 0.75, 0);
    step("nova.arp.octaves", "Octaves", "Arpeggiator", "", 1, 4, 1);
    choice("nova.arp.velocity", "Velocity", "Arpeggiator", arp_velocities, 0);
    choice("nova.arp.retrigger", "Retrigger", "Arpeggiator", off_on, 1);
    choice("nova.arp.latch", "Latch", "Arpeggiator", off_on, 0);
    // The pattern engine (ADR-0078).
    choice("nova.arp.note_mode", "Note Mode", "Arpeggiator", arp_note_modes, 0);
    choice("nova.arp.scale", "Scale", "Arpeggiator", arp_scales, 0);
    choice("nova.arp.root", "Root", "Arpeggiator", arp_roots, 0);
    choice("nova.arp.passing", "Passing Notes", "Arpeggiator", off_on, 0);
    lin("nova.arp.strum", "Strum", "Arpeggiator", "%", 0, 1, 0.35);
    step("nova.arp.euclid.hits", "Hits", "Arpeggiator", "", 0, 32, 5);
    step("nova.arp.euclid.steps", "Steps", "Arpeggiator", "", 1, 32, 16);
    step("nova.arp.euclid.rotate", "Rotate", "Arpeggiator", "", 0, 31, 0);
    lin("nova.arp.fill", "Fill", "Arpeggiator", "%", 0, 1, 0);
    choice("nova.arp.variation", "Variation", "Arpeggiator", arp_variations, 0);
    step("nova.arp.seed", "Seed", "Arpeggiator", "", 0, 9999, 0);
    lin("nova.arp.groove_amount", "Groove", "Arpeggiator", "%", 0, 1, 1);
    step("nova.arp.groove.steps", "Groove Steps", "Arp Groove", "", 0, 32, 0);
    choice("nova.arp.groove.grid", "Groove Grid", "Arp Groove", arp_groove_grids, 3);
    for (int n = 1; n <= 32; ++n) {
      const std::string base = "nova.arp.groove." + std::to_string(n);
      lin(base + ".timing", "Timing " + std::to_string(n), "Arp Groove", "", -0.5, 0.5, 0, 1.0,
          false);
      lin(base + ".velocity", "Velocity " + std::to_string(n), "Arp Groove", "", 0, 2, 1, 1.0,
          false);
    }
    // The lanes: (id, name, kind) with ranges and defaults.
    struct Lane {
      std::string id;
      std::string name;
      char kind; // l level, i integer, t toggle, c condition, h chord
      double lo, hi, fallback;
    };
    const std::array<Lane, 11> lanes{{{"note", "Note", 'i', -12, 12, 0},
                                      {"octave", "Octave", 'i', -2, 2, 0},
                                      {"velocity", "Velocity", 'l', 0, 1, 1},
                                      {"gate", "Gate", 'l', 0, 1, 1},
                                      {"tie", "Tie", 't', 0, 1, 0},
                                      {"ratchet", "Ratchet", 'i', 1, 4, 1},
                                      {"chance", "Chance", 'l', 0, 1, 1},
                                      {"condition", "Condition", 'c', 0, 13, 0},
                                      {"accent", "Accent", 't', 0, 1, 0},
                                      {"mod", "Mod", 'l', 0, 1, 0},
                                      {"chord", "Chord", 'h', 0, 5, 0}}};
    for (const auto& lane : lanes) {
      const std::string base = "nova.arp.lane." + lane.id + ".";
      const std::string group = "Arp " + lane.name;
      step(base + "length", "Length", group, "", 1, 32, 16);
      for (int n = 1; n <= 32; ++n) {
        const std::string id = base + std::to_string(n);
        const std::string name = lane.name + " " + std::to_string(n);
        switch (lane.kind) {
        case 'l':
          lin(id, name, group, "%", lane.lo, lane.hi, lane.fallback, 1.0, false);
          break;
        case 'i':
          step(id, name, group, "", lane.lo, lane.hi, lane.fallback);
          break;
        case 't':
          choice(id, name, group, off_on, lane.fallback);
          break;
        case 'c':
          choice(id, name, group, arp_conditions, lane.fallback);
          break;
        default:
          choice(id, name, group, arp_chords, lane.fallback);
          break;
        }
      }
    }
    choice("nova.seq.on", "On", "Sequencer", off_on, 0);
    choice("nova.seq.clip", "Clip", "Sequencer", clips, 0);
    choice("nova.seq.trigger", "Trigger", "Sequencer", triggers, 0);
    step("nova.seq.root", "Root", "Sequencer", "", 0, 127, 24);
    choice("nova.seq.follow", "Follow", "Sequencer", off_on, 1);
    choice("nova.global.voicing", "Voicing", "Global", voicings, 0);
    step("nova.global.polyphony", "Polyphony", "Global", "", 1, 32, 16);
    lin("nova.global.portamento", "Portamento", "Global", "s", 0, 10, 0, 4);
    lin("nova.global.portamento_curve", "Portamento Curve", "Global", "%", -1, 1, 0);
    choice("nova.global.portamento_mode", "Portamento Mode", "Global", porta_modes, 0);
    choice("nova.global.portamento_rate", "Portamento Rate", "Global", porta_rates, 0);
    lin("nova.global.drift", "Drift", "Global", "%", 0, 1, 0.1);
    step("nova.global.bend_range", "Bend Range", "Global", "st", 0, 48, 2);
    lin("nova.global.velocity_curve", "Velocity Curve", "Global", "%", -1, 1, 0, 1.0, false);
    choice("nova.global.mpe", "MPE", "Global", off_on, 0);
    step("nova.global.mpe_range", "MPE Range", "Global", "st", 0, 96, 48);
    choice("nova.global.quality", "Quality", "Global", qualities, 1);
    choice("nova.global.tuning", "Tuning", "Global", tunings, 0);
    lin("nova.global.a4", "A4", "Global", "Hz", 400, 480, 440, 1.0, false);
    lin("nova.perf.bend", "Bend", "Performance", "", -1, 1, 0);
    lin("nova.perf.wheel", "Wheel", "Performance", "", 0, 1, 0);
    lin("nova.perf.pressure", "Pressure", "Performance", "", 0, 1, 0);
    choice("nova.lock.on", "Lock", "Lock", off_on, 0);
    lin("nova.lock.duck", "Duck", "Lock", "%", 0, 1, 0.65);
    log("nova.lock.crossover", "Crossover", "Lock", "Hz", 60, 250, 120);
    choice("nova.lock.mono", "Mono Lows", "Lock", off_on, 1);
    // Analogue (the VOICES page): per-voice tolerances scaled by Age, drift.
    choice("nova.analogue.on", "Analogue", "Analogue", off_on, 0);
    lin("nova.analogue.age", "Age", "Analogue", "%", 0, 1, 0.35);
    step("nova.analogue.seed", "Seed", "Analogue", "", 1, 9'999, 4'127);
    lin("nova.analogue.pitch", "Pitch", "Analogue", "%", 0, 1, 0.6);
    lin("nova.analogue.cutoff", "Cutoff", "Analogue", "%", 0, 1, 0.5);
    lin("nova.analogue.resonance", "Resonance", "Analogue", "%", 0, 1, 0.3);
    lin("nova.analogue.env", "Env Times", "Analogue", "%", 0, 1, 0.5);
    lin("nova.analogue.level", "Level", "Analogue", "%", 0, 1, 0.4);
    lin("nova.analogue.pan", "Pan", "Analogue", "%", 0, 1, 0.4);
    lin("nova.analogue.shape", "Osc Shape", "Analogue", "%", 0, 1, 0.3);
    lin("nova.analogue.drift.amount", "Drift", "Analogue", "%", 0, 1, 0.4);
    lin("nova.analogue.drift.rate", "Drift Rate", "Analogue", "%", 0, 1, 0.3);
    lin("nova.analogue.drift.cutoff", "Drift on Cutoff", "Analogue", "%", 0, 1, 0.25);
    choice("nova.analogue.freerun", "Free-running", "Analogue", off_on, 1);
    choice("nova.analogue.scope", "Applies To", "Analogue", analogue_scopes, 0);
    // Voice cards, re-roll per note and flutter; then the noise layer's AM.
    step("nova.analogue.cards", "Voice Cards", "Analogue", "", 0, 16, 0);
    lin("nova.analogue.reroll", "Re-roll", "Analogue", "%", 0, 1, 0);
    lin("nova.analogue.flutter.amount", "Flutter", "Analogue", "%", 0, 1, 0);
    log("nova.analogue.flutter.tone", "Flutter Tone", "Analogue", "Hz", 2, 60, 12);
    choice("nova.noise.am.source", "AM Source", "Noise", noise_am_sources, 0);
    lin("nova.noise.am.amount", "AM Amount", "Noise", "%", 0, 1, 1);
    lin("nova.noise.am.curve", "AM Curve", "Noise", "%", -1, 1, 0);
    choice("nova.global.portamento_sync", "Portamento Sync", "Global", off_on, 0);
    choice("nova.global.portamento_division", "Portamento Division", "Global", porta_divisions, 12);
    lin("nova.analogue.glide", "Glide", "Analogue", "%", 0, 1, 0.3);
    descriptor_ = {"nova", "Nova", 1, parameters_, {}};
  }

  std::deque<std::string> text_;
  std::vector<oscillate::native_dsp::ParameterDescriptor> parameters_;
  oscillate::native_dsp::EngineDescriptor descriptor_{};
};

const oscillate::native_dsp::EngineDescriptor& contract_table() {
  static const ContractTable contract;
  return contract.descriptor();
}

const oscillate::native_dsp::EngineDescriptor& table() { return *synth::nova::engine().descriptor; }

// The engine's table is the contract's, in order: ids, scales, ranges,
// defaults, choices, timing and automation.
void engine_matches_the_contract() {
  const auto& engine = table().parameters;
  const auto& contract = contract_table().parameters;
  require(engine.size() == contract.size(), "the engine and the contract have the same size");
  for (std::size_t i = 0; i < engine.size(); ++i) {
    const auto& a = engine[i];
    const auto& b = contract[i];
    const std::string where = " (" + std::string(a.id) + " vs " + std::string(b.id) + ")";
    require(a.id == b.id, "parameter order matches" + where);
    require(a.scale == b.scale && a.minimum == b.minimum && a.maximum == b.maximum,
            "scale and range match" + where);
    require(std::fabs(a.default_value - b.default_value) < 1e-9, "default matches" + where);
    require(a.automatable == b.automatable, "automation matches" + where);
    require(std::ranges::equal(a.choices, b.choices), "choices match" + where);
  }
}

// ---- Telemetry (08a §6) ------------------------------------------------------------------

namespace t {
constexpr std::size_t size = 2038;
constexpr std::size_t voices = 1;
constexpr std::size_t polyphony = 2;
constexpr std::size_t cpu = 3;
constexpr std::size_t note = 4;
constexpr std::size_t playing = 5;
constexpr std::size_t position = 8;
constexpr std::size_t unison = 11;
constexpr std::size_t peak = 14;
constexpr std::size_t cutoff = 19;
constexpr std::size_t resonance = 21;
constexpr std::size_t env_level = 23;
constexpr std::size_t env_stage = 27;
constexpr std::size_t env_progress = 31;
constexpr std::size_t lfo_value = 35;
constexpr std::size_t lfo_phase = 43;
constexpr std::size_t macro = 51;
constexpr std::size_t perf = 59;
constexpr std::size_t meters = 69;
constexpr std::size_t clip = 77;
constexpr std::size_t clip_playhead = 78;
constexpr std::size_t arp_step = 79;
constexpr std::size_t routes = 80;
constexpr std::size_t frames = 336;
constexpr std::size_t frame_points = 128;
constexpr std::size_t fx = 720;
constexpr std::size_t slot_in = 744;
constexpr std::size_t slot_out = 768;
constexpr std::size_t slot_detail = 792;
constexpr std::size_t slot_spectrum = 864;
constexpr std::size_t detail_values = 3;
constexpr std::size_t spectrum_bands = 48;
constexpr std::size_t slot_count = 24;
} // namespace t

std::vector<float> live_frame() {
  std::vector<float> frame(t::size, 0.0F);
  frame[0] = 1.0F;
  frame[t::voices] = 3.0F;
  frame[t::polyphony] = 16.0F;
  frame[t::cpu] = 0.07F;
  frame[t::note] = 48.0F;
  frame[t::playing] = 1.0F;
  const std::array<float, 3> positions{0.42F, 0.0F, 0.55F};
  for (std::size_t o = 0; o < 3; ++o) {
    frame[t::position + o] = positions[o];
    frame[t::unison + o] = o == 0 ? 7.0F : 1.0F;
    frame[t::peak + o] = 0.6F;
    // A detuned saw-ish cycle for each oscillator's front trace.
    for (std::size_t i = 0; i < t::frame_points; ++i) {
      const double x = static_cast<double>(i) / t::frame_points;
      double v = 0.0;
      for (int h = 1; h < 16; ++h) {
        v += std::sin(2.0 * std::numbers::pi * h * x + 0.3 * h * positions[o]) / h;
      }
      frame[t::frames + o * t::frame_points + i] = static_cast<float>(0.6 * v);
    }
  }
  frame[t::cutoff] = 1'450.0F;
  frame[t::cutoff + 1] = 900.0F;
  frame[t::resonance] = 0.35F;
  frame[t::resonance + 1] = 0.6F;
  for (std::size_t e = 0; e < 4; ++e) {
    frame[t::env_level + e] = 0.7F - 0.1F * static_cast<float>(e);
    frame[t::env_stage + e] = 3.0F;
    frame[t::env_progress + e] = 0.4F;
  }
  for (std::size_t l = 0; l < 8; ++l) {
    frame[t::lfo_value + l] = 0.5F + 0.4F * static_cast<float>(std::sin(static_cast<double>(l)));
    frame[t::lfo_phase + l] = 0.3F + 0.05F * static_cast<float>(l);
  }
  for (std::size_t m = 0; m < 8; ++m) {
    frame[t::macro + m] = 0.1F * static_cast<float>(m + 1);
  }
  const std::array<float, 10> perf{0.8F, 0.38F, 0.2F, 0.1F, 0.4F, 0.3F, 0.55F, 0.7F, 0.25F, 0.5F};
  for (std::size_t p = 0; p < perf.size(); ++p) {
    frame[t::perf + p] = perf[p];
  }
  const std::array<float, 8> meters{-9.0F, -10.0F, -18.0F, -19.0F, -30.0F, -28.0F, -7.0F, -8.0F};
  for (std::size_t m = 0; m < meters.size(); ++m) {
    frame[t::meters + m] = meters[m];
  }
  frame[t::clip] = 0.0F;
  frame[t::clip_playhead] = 1.5F;
  frame[t::arp_step] = 5.0F;
  for (std::size_t s = 0; s < 8; ++s) {
    frame[t::routes + s] = 0.12F * static_cast<float>(std::sin(1.0 + static_cast<double>(s)));
  }
  frame[t::fx + 3] = 4.5F;
  frame[t::fx + 2] = 0.3F;
  frame[t::fx + 7] = 6.0F;
  frame[t::fx + 8] = 40.0F;
  // Every slot: levels, Multiband's band changes, a falling, lumpy spectrum.
  for (std::size_t u = 0; u < t::slot_count; ++u) {
    frame[t::slot_in + u] = -14.0F - static_cast<float>(u % 3);
    frame[t::slot_out + u] = -12.0F - static_cast<float>(u % 4);
    for (std::size_t b = 0; b < t::spectrum_bands; ++b) {
      const double x = static_cast<double>(b) / t::spectrum_bands;
      frame[t::slot_spectrum + u * t::spectrum_bands + b] =
          static_cast<float>(-30.0 - 36.0 * x + 6.0 * std::sin(9.0 * x + static_cast<double>(u)));
    }
  }
  const std::array<float, t::detail_values> bands{-4.5F, 2.0F, -6.0F};
  for (std::size_t b = 0; b < bands.size(); ++b) {
    frame[t::slot_detail + 3 * t::detail_values + b] = bands[b];
  }
  return frame;
}

// ---- A patch like the mockup's "Glass Reese" --------------------------------------------

project::NativeModRoute route(std::uint32_t slot, std::string source, std::string destination) {
  project::NativeModRoute r;
  r.slot = slot;
  r.source = std::move(source);
  r.destination = std::move(destination);
  return r;
}

project::NativeSynthDocument demo_document() {
  project::NativeSynthDocument document;
  document.routes = {route(0, "lfo.1", "nova.osc.a.position"),
                     route(1, "lfo.1", "nova.filter.1.cutoff"),
                     route(2, "env.2", "nova.filter.1.cutoff"),
                     route(3, "macro.1", "nova.osc.a.warp.2.amount"),
                     route(4, "lfo.2", "nova.osc.a.unison.detune"),
                     route(5, "velocity", "nova.filter.1.resonance"),
                     route(6, "macro.3", "nova.fx.6.mix")};
  document.routes[1].bipolar = true;
  document.routes[2].tension = 0.5;
  document.routes[6].scale = 0.8;
  using project::NativeCurvePoint;
  using project::NativeCurveSegment;
  document.curves = {{"lfo.1",
                      {NativeCurvePoint{0, 0.1, 0, NativeCurveSegment::curve},
                       NativeCurvePoint{0.18, 1, -0.5, NativeCurveSegment::curve},
                       NativeCurvePoint{0.5, 0.35, 0.4, NativeCurveSegment::curve},
                       NativeCurvePoint{0.75, 0.8, 0, NativeCurveSegment::curve},
                       NativeCurvePoint{1, 0.1, 0, NativeCurveSegment::curve}}},
                     {"lfo.2",
                      {NativeCurvePoint{0, 0.5, 0, NativeCurveSegment::curve},
                       NativeCurvePoint{0.5, 1, 0.6, NativeCurveSegment::curve},
                       NativeCurvePoint{1, 0.5, 0.6, NativeCurveSegment::curve}}}};
  document.chains = {{"fx.main",
                      {{1, "distortion"},
                       {2, "filter"},
                       {3, "flanger"},
                       {4, "multiband"},
                       {5, "delay"},
                       {6, "reverb"},
                       {7, "eq"},
                       {8, "compressor"}}},
                     {"fx.bus.1", {{9, "shifter"}, {10, "reverb"}}}};
  document.media = {{"osc.a", project::NativeBuiltinMedia{"reese_saw"}, 2048},
                    {"osc.c", project::NativeBuiltinMedia{"spectral_pad"}, 2048},
                    {"noise", project::NativeBuiltinMedia{"analog_hiss"}, 0}};
  const std::array<std::string, 8> names{"Growl",    "Width", "Space", "Bite",
                                         "Movement", "Air",   "Drive", "Tone"};
  for (std::size_t m = 0; m < names.size(); ++m) {
    document.labels.push_back({"macro." + std::to_string(m + 1), names[m]});
  }
  const auto q = oscillate::timeline::ticks_per_quarter;
  project::NativeSequence clip{"clip.1", {4 * q}, {}};
  const std::array<std::pair<int, int>, 8> notes{
      {{0, 48}, {2, 55}, {3, 51}, {6, 58}, {8, 48}, {10, 60}, {12, 55}, {14, 51}}};
  for (const auto& [sixteenth, key] : notes) {
    clip.notes.push_back({{sixteenth * q / 4}, {q / 4}, static_cast<std::uint8_t>(key), 100});
  }
  document.sequences = {clip};
  return document;
}

std::vector<double> demo_values() {
  auto values = oscillate::native_dsp::default_values(table());
  const auto set = [&](std::string_view id, double value) {
    const auto index = oscillate::native_dsp::find_parameter(table(), id);
    require(index.has_value(), "demo value names a contract id");
    values[*index] = value;
  };
  set("nova.osc.a.position", 0.32);
  set("nova.osc.a.warp.1.mode", 1);
  set("nova.osc.a.warp.1.amount", 0.35);
  set("nova.osc.a.warp.2.mode", 12);
  set("nova.osc.a.warp.2.amount", 0.22);
  set("nova.osc.a.unison.voices", 7);
  set("nova.osc.a.unison.detune", 36);
  set("nova.osc.a.unison.position_spread", 0.25);
  set("nova.osc.b.on", 1);
  set("nova.osc.b.level", 0.6);
  set("nova.osc.b.octave", -1);
  set("nova.osc.b.fine", 6);
  set("nova.osc.c.type", 4);
  set("nova.osc.c.position", 0.55);
  set("nova.sub.on", 1);
  set("nova.noise.on", 1);
  set("nova.filter.1.cutoff", 1'200);
  set("nova.filter.1.resonance", 0.32);
  set("nova.filter.1.drive", 5);
  set("nova.filter.2.on", 1);
  set("nova.filter.2.resonance", 0.6);
  set("nova.filter.2.fat", 0.3);
  const std::array<double, 8> macros{0.2, 0.6, 0.3, 0.1, 0.45, 0, 0.15, 0.5};
  for (std::size_t m = 0; m < macros.size(); ++m) {
    set("nova.macro." + std::to_string(m + 1), macros[m]);
  }
  const std::array<double, 7> amounts{0.35, 0.22, 0.3, 0.5, 0.15, -0.12, 0.6};
  for (std::size_t s = 0; s < amounts.size(); ++s) {
    set("nova.mod." + std::to_string(s) + ".amount", amounts[s]);
  }
  // The rack's settings (08a §5 defaults, a little moved).
  const std::array<std::array<double, 4>, 8> knobs{{{0.4, 0.55, 0.6, 1},
                                                    {0.62, 0.4, 0.1, 1},
                                                    {0.35, 0.5, 0.7, 0.5},
                                                    {0.5, 0.5, 0.5, 1},
                                                    {8.0 / 14, 0.4, 0.5, 0.25},
                                                    {0.6, 0.5, 0.4, 0.25},
                                                    {0.6, 0.45, 0.62, 0.4},
                                                    {0.7, 0.4, 0.5, 1}}};
  constexpr std::size_t eq_slot = 6;
  for (std::size_t u = 0; u < knobs.size(); ++u) {
    if (u == eq_slot) {
      continue;
    }
    const std::string base = "nova.fx." + std::to_string(u + 1);
    set(base + ".p0", knobs[u][0]);
    set(base + ".p1", knobs[u][1]);
    set(base + ".p2", knobs[u][2]);
    set(base + ".mix", knobs[u][3]);
  }
  // The EQ (uid 7): a low cut, a cut at 350 Hz, a lift at 3 kHz, a high shelf.
  const auto freq = [](double hz) { return std::log(hz / 20.0) / std::log(1'000.0); };
  const auto gain = [](double db) { return (db + 24.0) / 48.0; };
  const auto q = [](double value) { return std::log(value / 0.25) / std::log(64.0); };
  const std::array<std::array<double, 4>, 4> eq{{{5.0 / 9, freq(60), gain(0), q(0.707)},
                                                 {1.0 / 9, freq(350), gain(-4), q(1.2)},
                                                 {1.0 / 9, freq(3'000), gain(3), q(0.9)},
                                                 {3.0 / 9, freq(9'000), gain(2.5), q(0.707)}}};
  for (std::size_t b = 0; b < eq.size(); ++b) {
    for (std::size_t k = 0; k < 4; ++k) {
      set("nova.fx.7.p" + std::to_string(4 * b + k), eq[b][k]);
    }
  }
  set("nova.fx.7.mix", 1);
  // Bus 1: a shifter at +40 Hz and a reverb, fed by the send.
  set("nova.fx.9.p0", 0.6);
  set("nova.fx.9.mix", 0.5);
  set("nova.fx.10.p0", 0.7);
  set("nova.fx.10.p1", 0.55);
  set("nova.fx.10.p2", 0.4);
  set("nova.fx.10.mix", 0.35);
  set("nova.fx.9.on", 1);
  set("nova.fx.10.on", 1);
  set("nova.fx.bus.1.send", 0.35);
  set("nova.fx.4.p3", 0.6);
  set("nova.fx.4.p4", 0.45);
  set("nova.fx.4.p5", 0.55);
  set("nova.fx.5.p5", 1);
  set("nova.fx.5.p7", 0.5);
  // An arp pattern: a polymetric velocity lane, rests in the gate lane, a
  // tie, a ratchet and an accent.
  set("nova.arp.lane.velocity.length", 12);
  for (int n = 1; n <= 12; ++n) {
    set("nova.arp.lane.velocity." + std::to_string(n), 0.3 + 0.7 * ((n * 37) % 70) / 70.0);
  }
  set("nova.arp.lane.gate.length", 7);
  set("nova.arp.lane.gate.5", 0);
  set("nova.arp.lane.tie.7", 1);
  set("nova.arp.lane.ratchet.16", 3);
  set("nova.arp.lane.accent.1", 1);
  set("nova.arp.lane.note.length", 5);
  set("nova.arp.lane.note.2", 2);
  set("nova.arp.lane.note.4", -1);
  return values;
}

// ---- Harness ---------------------------------------------------------------------------------

struct Panel {
  QQmlEngine engine;
  std::unique_ptr<QQuickWindow> window = std::make_unique<QQuickWindow>();
  QQuickItem* root = nullptr;
};

std::unique_ptr<Panel> load_panel(NativeSynthDeviceController& controller) {
  auto panel = std::make_unique<Panel>();
  QQmlComponent component(&panel->engine);
  component.setData(R"(
      import QtQuick
      import "qrc:/nova"
      NovaPanel {}
    )",
                    QUrl(QStringLiteral("qrc:/NovaPanelTest.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("Nova panel failed to load: " + component.errorString().toStdString());
  }
  QObject* object = component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}});
  require(object != nullptr,
          "Nova panel was not created: " + component.errorString().toStdString());
  panel->root = qobject_cast<QQuickItem*>(object);
  require(panel->root != nullptr, "panel is a Quick item");
  panel->root->setWidth(panel->root->implicitWidth());
  panel->root->setHeight(panel->root->implicitHeight());
  panel->window->resize(static_cast<int>(panel->root->width()),
                        static_cast<int>(panel->root->height()));
  panel->root->setParentItem(panel->window->contentItem());
  panel->window->show();
  require(QTest::qWaitForWindowExposed(panel->window.get()), "panel window is exposed");
  QCoreApplication::processEvents();
  return panel;
}

QQuickItem* find_item(QQuickItem* parent, const QString& name) {
  if (parent->objectName() == name) {
    return parent;
  }
  for (QQuickItem* child : parent->childItems()) {
    if (QQuickItem* found = find_item(child, name)) {
      return found;
    }
  }
  return nullptr;
}

QQuickItem* item(Panel& panel, const QString& name) {
  auto* found = find_item(panel.root, name);
  require(found != nullptr, "missing item " + name.toStdString());
  return found;
}

QPoint centre(QQuickItem* target) {
  return target->mapToScene(QPointF(target->width() / 2, target->height() / 2)).toPoint();
}

// The centre of a NovaKnob's dial.
QPoint dial_centre(QQuickItem* knob) {
  const double dial = knob->property("dial").toDouble();
  return knob->mapToScene(QPointF(knob->width() / 2, dial / 2)).toPoint();
}

// Runs pending work, including deleting items a Loader let go of.
void settle() {
  QCoreApplication::processEvents();
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  QCoreApplication::processEvents();
}

// The length of a QML `var` array property.
int js_length(const QVariant& value) {
  if (value.metaType() == QMetaType::fromType<QJSValue>()) {
    return value.value<QJSValue>().property(QStringLiteral("length")).toInt();
  }
  return static_cast<int>(value.toList().size());
}

// Entry `index` of a QML `var` array property of objects.
QVariantMap js_entry(const QVariant& value, int index) {
  if (value.metaType() == QMetaType::fromType<QJSValue>()) {
    return value.value<QJSValue>().property(static_cast<quint32>(index)).toVariant().toMap();
  }
  return value.toList().at(index).toMap();
}

void click(Panel& panel, QPoint point, Qt::MouseButton button = Qt::LeftButton) {
  QTest::mouseClick(panel.window.get(), button, Qt::NoModifier, point);
  settle();
}

void drag(Panel& panel, QPoint from, QPoint to, int steps = 8) {
  QTest::mousePress(panel.window.get(), Qt::LeftButton, Qt::NoModifier, from);
  for (int step = 1; step <= steps; ++step) {
    QTest::mouseMove(panel.window.get(), from + (to - from) * step / steps);
  }
  QTest::mouseRelease(panel.window.get(), Qt::LeftButton, Qt::NoModifier, to);
  settle();
}

// Opens a page as the user does: FX and VOICES from the signal strip, the
// others from the header's page buttons (MAIN is novaTab:0).
void show_page(Panel& panel, int page) {
  const auto control = page == 1   ? QStringLiteral("novaSpine:fx")
                       : page == 5 ? QStringLiteral("novaSpine:voices")
                                   : QStringLiteral("novaTab:%1").arg(page);
  click(panel, centre(item(panel, control)));
  require(panel.root->property("page").toInt() == page, "the control shows its page");
  require(item(panel, QStringLiteral("novaPage:%1").arg(page))->isVisible(), "the page is visible");
}

// Lays out the MODULATION card's source tabs: their Flow places the tabs
// when it polishes, so positions read before then are stale.
void lay_out_tabs(Panel& panel) {
  QMetaObject::invokeMethod(item(panel, QStringLiteral("novaModTabs")), "forceLayout");
  settle();
}

// The rectangle `target` covers in the window.
QRectF scene_rect(QQuickItem* target) {
  return target->mapRectToScene(QRectF(0, 0, target->width(), target->height()));
}

// Waits for a card's opacity, which fades, to settle near `opacity`.
bool fades_to(QQuickItem* card, double opacity) {
  return QTest::qWaitFor([&] { return std::fabs(card->opacity() - opacity) < 0.01; }, 1'000);
}

// Ids named by visible-or-not bound controls under `root`: "novaKnob:<id>",
// "novaSegs:<id>[:tile]" and so on.
void collect_bound(QQuickItem* root, QSet<QString>& ids, std::vector<QString>& names) {
  std::vector<QQuickItem*> stack{root};
  while (!stack.empty()) {
    auto* at = stack.back();
    stack.pop_back();
    const QString name = at->objectName();
    const auto colon = name.indexOf(QLatin1Char(':'));
    if (name.startsWith(QStringLiteral("nova")) && colon > 0) {
      QString id = name.mid(colon + 1);
      const auto tile = id.indexOf(QLatin1Char(':'));
      if (tile > 0) {
        id = id.left(tile);
      }
      if (id.startsWith(QStringLiteral("nova."))) {
        ids.insert(id);
        names.push_back(name);
      }
    }
    for (auto* child : at->childItems()) {
      stack.push_back(child);
    }
  }
}

// ---- Contracts -------------------------------------------------------------------------------

// The stub is a valid engine table within the platform's limit, and the
// panel loads at its designed size with every page reachable.
void panel_loads_and_visits_every_page() {
  require(oscillate::native_dsp::validate_engine(table()).has_value(), "the engine table is valid");
  require(table().parameters.size() == 1515, "the engine table has 1515 parameters");
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(panel->root->implicitWidth() == panel->root->property("designedWidth").toDouble() &&
              panel->root->implicitHeight() == panel->root->property("designedHeight").toDouble(),
          "the panel has its designed size");
  for (int page : {1, 2, 3, 4, 5, 0}) {
    show_page(*panel, page);
  }
}

// The overview (ADR-0076 revision 2): MAIN shows every module's card at
// once, laid out as the mockup's grid and fitting the designed size without
// scrolling: the three oscillators and SUB · NOISE across the top, the
// filters and ROUTING under them, MODULATION and MACROS at the foot. Each
// card holds its display and its controls; the master level is on ROUTING.
void overview_shows_every_module() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(panel->root->property("page").toInt() == 0, "the panel opens on MAIN");
  auto* overview = item(*panel, QStringLiteral("novaOverview"));
  const QRectF area = scene_rect(item(*panel, QStringLiteral("novaPage:0")));
  for (const char* key :
       {"a", "b", "c", "subnoise", "filter1", "filter2", "routing", "mod", "macros"}) {
    auto* card = item(*panel, QStringLiteral("novaModuleCard:%1").arg(QLatin1String(key)));
    const QRectF rect = scene_rect(card);
    require(card->isVisible() && area.contains(rect.adjusted(1, 1, -1, -1)),
            std::string("MAIN shows the whole card ") + key);
  }
  for (const char* name :
       {"novaOsc:a", "novaOsc:b", "novaOsc:c", "novaOscDisplay:a", "novaOscDisplay:c",
        "novaSubNoise", "novaSubNoiseDisplay", "novaFilter:1", "novaFilter:2",
        "novaFilterResponse:1", "novaFilterResponse:2", "novaRouting", "novaMacros",
        "novaEnvelopeView:env.1", "novaSourceControls:env.1", "novaKnob:nova.master"}) {
    require(item(*panel, QString::fromLatin1(name))->isVisible(),
            std::string("MAIN shows ") + name);
  }
  require(overview->property("columns").toInt() == 4, "wide: four columns");
  auto* scroller = item(*panel, QStringLiteral("novaOverviewScroller"));
  require(scroller->property("contentHeight").toDouble() <= scroller->height() + 0.5 &&
              overview->parentItem()->scale() == 1.0,
          "the overview fits the designed size, unscaled and unscrolled");
  const auto top = [&](const char* key) {
    return scene_rect(item(*panel, QStringLiteral("novaModuleCard:%1").arg(QLatin1String(key))))
        .top();
  };
  require(top("a") == top("b") && top("b") == top("c") && top("c") == top("subnoise"),
          "the oscillators and SUB · NOISE share the top row");
  require(top("filter1") > top("a") && top("filter1") == top("routing") &&
              top("mod") > top("filter1") && top("mod") == top("macros"),
          "the filters and routing, then modulation and macros");
}

// A card whose module is off fades to offOpacity and comes back when it is
// switched on; its strip light turns grey. OSC B starts off.
void an_off_module_fades() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto* a = item(*panel, QStringLiteral("novaModuleCard:a"));
  auto* b = item(*panel, QStringLiteral("novaModuleCard:b"));
  const double faded = b->property("offOpacity").toDouble();
  require(faded < 0.5, "an off card fades to under half");
  require(fades_to(b, faded) && fades_to(a, 1.0), "OSC B is off and faded, OSC A on and solid");
  const auto led_on = [&](const char* key) {
    return item(*panel, QStringLiteral("novaSpineLed:%1").arg(QLatin1String(key)))
               ->property("color")
               .value<QColor>() !=
           panel->root->property("theme").value<QObject*>()->property("faint").value<QColor>();
  };
  require(led_on("a") && !led_on("b"), "the strip's lights say so");
  click(*panel, centre(item(*panel, QStringLiteral("novaLed:nova.osc.b.on"))));
  require(controller.choice(QStringLiteral("nova.osc.b.on")) == 1,
          "the card's light switches it on");
  require(fades_to(b, 1.0) && led_on("b"), "an on card is solid and its strip light lit");
  controller.set_choice(QStringLiteral("nova.filter.1.on"), 0);
  require(fades_to(item(*panel, QStringLiteral("novaModuleCard:filter1")), faded),
          "switching a filter off fades its card");
}

// ⤢ or a double-click on the title opens a card large over the overview,
// under a scrim, with a bigger display; Esc, ⤢ again or a click on the
// scrim restores it. `focusModule` names the large card.
void expanding_a_card_in_place() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto* card = item(*panel, QStringLiteral("novaModuleCard:a"));
  auto* display = item(*panel, QStringLiteral("novaOscDisplay:a"));
  auto* scrim = item(*panel, QStringLiteral("novaScrim"));
  const double width = card->width();
  const double tall = display->height();
  require(!scrim->isVisible(), "no scrim while every card is in the grid");

  click(*panel, centre(item(*panel, QStringLiteral("novaExpand:a"))));
  require(panel->root->property("focusModule").toString() == QStringLiteral("a"),
          "⤢ opens OSC A large");
  require(card->property("expanded").toBool() && card->width() > 2 * width &&
              display->height() > tall && scrim->isVisible(),
          "the card covers the overview, its display bigger, over a scrim");
  QTest::keyClick(panel->window.get(), Qt::Key_Escape);
  settle();
  require(panel->root->property("focusModule").toString().isEmpty() &&
              std::fabs(card->width() - width) < 0.5 && !scrim->isVisible(),
          "Esc puts it back in its place");

  const QPoint title = centre(item(*panel, QStringLiteral("novaCardTitle:filter1")));
  QTest::mouseDClick(panel->window.get(), Qt::LeftButton, Qt::NoModifier, title);
  settle();
  require(panel->root->property("focusModule").toString() == QStringLiteral("filter1"),
          "a double-click on a title opens the card");
  click(*panel, centre(item(*panel, QStringLiteral("novaExpand:filter1"))));
  require(panel->root->property("focusModule").toString().isEmpty(), "⤢ again restores it");

  click(*panel, centre(item(*panel, QStringLiteral("novaExpand:mod"))));
  require(panel->root->property("focusModule").toString() == QStringLiteral("mod"),
          "the modulation card opens large too");
  const QRectF large = scene_rect(item(*panel, QStringLiteral("novaModuleCard:mod")));
  click(*panel, (large.topLeft() - QPointF(4, 4)).toPoint());
  require(panel->root->property("focusModule").toString().isEmpty(),
          "a click on the scrim restores it");
}

// The strip: a sound module's item shows MAIN, scrolled to its card, and
// flashes the card; FX, VOICES and OUT open their pages.
void the_strip_jumps_to_cards() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  for (const char* key : {"a", "b", "c", "subnoise", "filter1", "filter2", "fx", "voices", "out"}) {
    require(
        item(*panel, QStringLiteral("novaSpineThumbnail:%1").arg(QLatin1String(key)))->isVisible(),
        std::string("the strip pictures ") + key);
  }
  show_page(*panel, 2);
  click(*panel, centre(item(*panel, QStringLiteral("novaSpine:filter2"))));
  require(panel->root->property("page").toInt() == 0, "a filter's item shows MAIN");
  require(item(*panel, QStringLiteral("novaModuleCard:filter2"))->property("flashing").toBool(),
          "and flashes the filter's card");
  click(*panel, centre(item(*panel, QStringLiteral("novaSpine:fx"))));
  require(panel->root->property("page").toInt() == 1, "FX opens its page");
  click(*panel, centre(item(*panel, QStringLiteral("novaSpine:voices"))));
  require(panel->root->property("page").toInt() == 5, "VOICES opens its page");
  click(*panel, centre(item(*panel, QStringLiteral("novaSpine:out"))));
  require(panel->root->property("page").toInt() == 4, "OUT opens GLOBAL");
  click(*panel, centre(item(*panel, QStringLiteral("novaSpine:subnoise"))));
  require(
      panel->root->property("page").toInt() == 0 &&
          item(*panel, QStringLiteral("novaModuleCard:subnoise"))->property("flashing").toBool(),
      "SUB · NOISE's item flashes its card");

  // A large card gives way to the card the strip jumps to.
  panel->root->setProperty("focusModule", QStringLiteral("a"));
  settle();
  click(*panel, centre(item(*panel, QStringLiteral("novaSpine:filter1"))));
  require(panel->root->property("focusModule").toString().isEmpty(),
          "the strip restores a large card before it jumps");
}

// The MODULATION card: a click on an envelope or LFO tab selects it
// (`modTab`) and the card shows its editor and knobs; a macro's tab shows
// the MACROS card; a performance source has nothing to edit.
void a_mod_tab_selects_its_source() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(panel->root->property("modTab").toString() == QStringLiteral("env.1"),
          "the amp envelope is selected first");
  lay_out_tabs(*panel);
  click(*panel, centre(item(*panel, QStringLiteral("novaSource:env.2"))));
  require(panel->root->property("modTab").toString() == QStringLiteral("env.2") &&
              item(*panel, QStringLiteral("novaEnvelopeView:env.2"))->isVisible() &&
              item(*panel, QStringLiteral("novaSourceControls:env.2"))->isVisible() &&
              item(*panel, QStringLiteral("novaSource:env.2"))->property("selected").toBool(),
          "an envelope tab shows its envelope and knobs");
  click(*panel, centre(item(*panel, QStringLiteral("novaSource:lfo.4"))));
  require(panel->root->property("modTab").toString() == QStringLiteral("lfo.4") &&
              item(*panel, QStringLiteral("novaCurveEditor:lfo.4"))->isVisible() &&
              item(*panel, QStringLiteral("novaSourceControls:lfo.4"))->isVisible(),
          "an LFO tab shows its shape and knobs");
  click(*panel, centre(item(*panel, QStringLiteral("novaSource:macro.1"))));
  require(panel->root->property("modTab").toString() == QStringLiteral("lfo.4") &&
              item(*panel, QStringLiteral("novaModuleCard:macros"))->property("flashing").toBool(),
          "a macro's tab shows the macros");
  click(*panel, centre(item(*panel, QStringLiteral("novaSource:velocity"))));
  require(panel->root->property("modTab").toString() == QStringLiteral("lfo.4"),
          "a performance source has nothing to edit: the selection stays");
}

// The zoom chips set the zoom the host lays the panel out at; the
// constellation button opens its place over the pages and closes the browser.
void zoom_and_constellation() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(std::fabs(panel->root->property("zoom").toDouble() - 1.0) < 1e-9,
          "the panel opens at 100%");
  click(*panel, centre(item(*panel, QStringLiteral("novaZoom:140"))));
  require(std::fabs(panel->root->property("zoom").toDouble() - 1.4) < 1e-9, "the 140% chip zooms");
  click(*panel, centre(item(*panel, QStringLiteral("novaZoom:90"))));
  require(std::fabs(panel->root->property("zoom").toDouble() - 0.9) < 1e-9,
          "the 90% chip zooms out");

  click(*panel, centre(item(*panel, QStringLiteral("novaPresetBrowse"))));
  require(item(*panel, QStringLiteral("novaPresetBrowser"))->isVisible(),
          "BROWSE opens the browser");
  auto* host = item(*panel, QStringLiteral("novaConstellationHost"));
  require(!host->isVisible(), "the constellation starts closed");
  click(*panel, centre(item(*panel, QStringLiteral("novaConstellationButton"))));
  require(panel->root->property("constellationOpen").toBool() && host->isVisible() &&
              !item(*panel, QStringLiteral("novaPresetBrowser"))->isVisible(),
          "✦ CONSTELLATION opens over the pages and closes the browser");
  click(*panel, centre(item(*panel, QStringLiteral("novaSpine:fx"))));
  require(!panel->root->property("constellationOpen").toBool() &&
              panel->root->property("page").toInt() == 1,
          "a strip item puts the constellation away");
}

// Reflow (ADR-0076): a page lays out at the area it gets, wider with the
// panel, and is scaled down only below its minimum. The browser drawer
// narrows the pages beside it. Below the narrow width the overview goes to
// two columns, as the mockup's "a b" "c n" "f1 f2" "r r" "mod mod"
// "mac mac", and scrolls.
void pages_reflow_with_the_panel() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  auto* overview = item(*panel, QStringLiteral("novaOverview"));
  const double designed = overview->width();
  const double margins = 2 * panel->root->property("pageMargin").toDouble();
  panel->root->setWidth(panel->root->width() + 300);
  settle();
  // The page's Loader carries its scale.
  auto* host = overview->parentItem();
  require(overview->width() > designed + 299 && host->scale() == 1.0,
          "a wider panel lays the overview out wider, unscaled");
  click(*panel, centre(item(*panel, QStringLiteral("novaPresetBrowse"))));
  require(std::fabs(overview->width() - designed) < 1.0, "the browser drawer narrows the page");
  click(*panel, centre(item(*panel, QStringLiteral("novaPresetBrowse"))));

  const auto card = [&](const char* key) {
    return item(*panel, QStringLiteral("novaModuleCard:%1").arg(QLatin1String(key)));
  };
  panel->root->setWidth(panel->root->property("overviewNarrowWidth").toDouble() - 1);
  settle();
  require(overview->property("narrow").toBool() && overview->property("columns").toInt() == 2,
          "narrow: two columns");
  require(card("a")->y() == card("b")->y() && card("b")->x() > card("a")->x() &&
              card("c")->x() == card("a")->x() && card("c")->y() > card("a")->y() &&
              card("subnoise")->y() == card("c")->y() && card("filter1")->y() > card("c")->y(),
          "narrow: A beside B, C beside SUB · NOISE, then the filters");
  for (const char* wide : {"routing", "mod", "macros"}) {
    require(std::fabs(card(wide)->width() - overview->width()) < 1.0,
            std::string("narrow: a full-width row for ") + wide);
  }
  require(card("macros")->y() > card("mod")->y() && card("mod")->y() > card("routing")->y(),
          "narrow: routing, then modulation, then macros");
  auto* scroller = item(*panel, QStringLiteral("novaOverviewScroller"));
  require(scroller->property("interactive").toBool(), "narrow: the overview scrolls");

  panel->root->setWidth(overview->property("minimumWidth").toDouble() + margins - 100);
  settle();
  require(host->scale() < 1.0 &&
              std::fabs(overview->width() - overview->property("minimumWidth").toDouble()) < 1.0,
          "below its minimum the page is laid out at it and scaled down");
}

// The key strip plays: a press on a key reports its note from the base
// (the typing keyboard's octave) with a velocity, held until released; a
// black key plays its semitone; a key lights while it is held.
void key_strip_plays_notes() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  QSignalSpy pressed(panel->root, SIGNAL(keyPressed(int, int)));
  QSignalSpy released(panel->root, SIGNAL(keyReleased(int)));
  const int base = panel->root->property("keyBase").toInt();
  auto* strip = item(*panel, QStringLiteral("novaKeyStrip"));
  const QPoint a = centre(item(*panel, QStringLiteral("novaKey:A")));
  QTest::mousePress(panel->window.get(), Qt::LeftButton, Qt::NoModifier, a);
  settle();
  require(pressed.size() == 1 && pressed.front().at(0).toInt() == base &&
              pressed.front().at(1).toInt() > 0 && pressed.front().at(1).toInt() <= 127,
          "A plays the base note with a velocity");
  require(strip->property("held").toInt() == base &&
              item(*panel, QStringLiteral("novaKey:A"))->property("on").toBool(),
          "the key lights while held");
  QTest::mouseRelease(panel->window.get(), Qt::LeftButton, Qt::NoModifier, a);
  settle();
  require(released.size() == 1 && released.front().at(0).toInt() == base, "release ends it");
  require(!item(*panel, QStringLiteral("novaKey:A"))->property("on").toBool(), "and its light");

  panel->root->setProperty("keyBase", 60);
  settle();
  click(*panel, centre(item(*panel, QStringLiteral("novaKey:W"))));
  require(pressed.size() == 2 && pressed.back().at(0).toInt() == 61 && released.size() == 2,
          "W plays the semitone above the base, which follows the host's octave");
}

// Every bound control names a real parameter, and every parameter has a
// control somewhere: on MAIN (across the oscillator types and the
// modulation tabs), the other pages and the effect buses. Route amounts, effect slot values
// and performance inputs are reached through routes, cards and MIDI.
void every_parameter_has_a_control() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  QSet<QString> ids;
  std::vector<QString> names;
  const auto sweep = [&] {
    settle();
    collect_bound(panel->root, ids, names);
  };
  // MAIN holds every sound module at once, with no card opened large: each
  // oscillator type, then each source on the MODULATION card.
  for (int type = 0; type < 6; ++type) {
    for (const char* osc : {"a", "b", "c"}) {
      controller.set_choice(QStringLiteral("nova.osc.%1.type").arg(QLatin1String(osc)), type);
    }
    sweep();
  }
  require(panel->root->property("focusModule").toString().isEmpty(), "no card was opened large");
  for (const char* source : {"env.1", "env.2", "env.3", "env.4", "lfo.1", "lfo.2", "lfo.3", "lfo.4",
                             "lfo.5", "lfo.6", "lfo.7", "lfo.8"}) {
    panel->root->setProperty("modTab", QString::fromLatin1(source));
    sweep();
  }
  panel->root->setProperty("page", 1);
  // The split crossover shows only while SPLIT is on.
  controller.set_choice(QStringLiteral("nova.fx.split"), 1);
  for (const char* chain : {"fx.main", "fx.bus.1", "fx.bus.2"}) {
    panel->root->setProperty("fxChain", QString::fromLatin1(chain));
    sweep();
  }
  for (int page : {2, 3, 4, 5}) {
    panel->root->setProperty("page", page);
    sweep();
  }
  for (const auto& name : names) {
    const QString id = name.mid(name.indexOf(QLatin1Char(':')) + 1).section(QLatin1Char(':'), 0, 0);
    require(oscillate::native_dsp::find_parameter(table(), id.toStdString()).has_value(),
            "control bound to an unknown id: " + name.toStdString());
  }
  require(find_item(panel->root, QStringLiteral("novaArpCell:nova.arp.lane.note.1")) != nullptr &&
              find_item(panel->root, QStringLiteral("novaArpCell:nova.arp.lane.chord.32")) !=
                  nullptr &&
              find_item(panel->root, QStringLiteral("novaArpLength:nova.arp.lane.mod.length")) !=
                  nullptr &&
              find_item(panel->root, QStringLiteral("novaArpLibrary")) != nullptr &&
              find_item(panel->root, QStringLiteral("novaSeqRoot")) != nullptr,
          "the arp lanes, library and sequencer root have their own controls");
  for (const auto& p : table().parameters) {
    const QString id = QString::fromUtf8(p.id.data(), static_cast<qsizetype>(p.id.size()));
    if (id.startsWith(QStringLiteral("nova.perf.")) || id.startsWith(QStringLiteral("nova.mod.")) ||
        id.startsWith(QStringLiteral("nova.arp.groove.")) ||
        id == QStringLiteral("nova.seq.root") ||
        id.contains(QRegularExpression(QStringLiteral("^nova\\.fx\\.\\d+\\."))) ||
        id.contains(QRegularExpression(QStringLiteral("^nova\\.osc\\.[abc]\\.pitch$")))) {
      continue; // Reached through routes, rack cards, MIDI, the groove list or the matrix.
    }
    require(ids.contains(id), "no control for " + id.toStdString());
  }
}

// Dropping a source's chip on a knob makes a route at 30 % in the next slot,
// the readout confirms it, the ring drag sets its depth, hovering lists it
// and the right-click menu removes it.
void drag_to_modulate_and_edit_the_route() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  lay_out_tabs(*panel);
  auto* knob = item(*panel, QStringLiteral("novaKnob:nova.filter.1.cutoff"));
  const QPoint target = dial_centre(knob);
  drag(*panel, centre(item(*panel, QStringLiteral("novaSource:lfo.1"))), target, 12);

  const auto& documents = adapter.document_submissions();
  require(!documents.empty(), "the drop submitted a document");
  const auto& dropped = documents.back();
  require(dropped.gesture == NativeSynthGesture::single, "a drop is one edit");
  require(dropped.document.routes.size() == 1, "the drop made one route");
  const auto& made = dropped.document.routes.front();
  require(made.slot == 0 && made.source == "lfo.1" && made.destination == "nova.filter.1.cutoff" &&
              !made.bipolar && made.tension == 0.0 && made.scale == 1.0,
          "the route runs LFO 1 to Filter 1 cutoff in slot 0");
  require(std::fabs(controller.value(QStringLiteral("nova.mod.0.amount")) - 0.3) < 1e-9,
          "the route's amount is 30 %");
  require(item(*panel, QStringLiteral("novaReadout"))->property("text").toString() ==
              QStringLiteral("LFO 1 → CUTOFF 30 %"),
          "the readout confirms the route");
  require(js_length(knob->property("routes")) == 1, "the knob shows its route");

  // A second source on the same knob takes the next slot.
  drag(*panel, centre(item(*panel, QStringLiteral("novaSource:velocity"))), target, 12);
  require(adapter.document().routes.size() == 2 && adapter.document().routes.back().slot == 1,
          "a second route takes slot 1");

  // Drag on the ring: the newest route's depth, as one value gesture.
  const double radius = knob->property("dial").toDouble() / 2 - 1.5;
  const QPoint ring = target + QPoint(static_cast<int>(radius), 0);
  const auto values_before = adapter.submissions().size();
  drag(*panel, ring, ring - QPoint(0, 32));
  require(controller.value(QStringLiteral("nova.mod.1.amount")) > 0.45,
          "dragging the ring up deepens the newest route");
  require(std::fabs(controller.value(QStringLiteral("nova.mod.0.amount")) - 0.3) < 1e-9,
          "the older route keeps its depth");
  require(std::fabs(controller.value(QStringLiteral("nova.filter.1.cutoff")) - 2'000) < 1e-6,
          "the ring drag leaves the cutoff alone");
  const auto& values = adapter.submissions();
  require(values.size() > values_before + 2 &&
              values[values_before].gesture == NativeSynthGesture::begin &&
              values.back().gesture == NativeSynthGesture::commit,
          "the depth drag is one gesture");

  // Hover lists the routes: leave the knob, then come back, as a pointer does.
  QTest::mouseMove(panel->window.get(), target - QPoint(0, 60));
  QTest::qWait(20);
  QTest::mouseMove(panel->window.get(), target + QPoint(1, 1));
  QTest::qWait(20);
  QTest::mouseMove(panel->window.get(), target);
  require(
      QTest::qWaitFor([&] { return item(*panel, QStringLiteral("novaRoutePopover"))->isVisible(); },
                      ui_wait_ms),
      "hovering a modulated knob lists its routes");

  // Right-click: the menu's first entry removes the oldest route.
  click(*panel, target, Qt::RightButton);
  auto* menu = item(*panel, QStringLiteral("novaMenu"));
  require(menu->isVisible(), "right-click opens the route menu");
  QTest::keyClick(panel->window.get(), Qt::Key_Down);
  QTest::keyClick(panel->window.get(), Qt::Key_Return);
  QCoreApplication::processEvents();
  require(adapter.document().routes.size() == 1 && adapter.document().routes.front().slot == 1,
          "the menu removes the LFO 1 route");
  require(controller.value(QStringLiteral("nova.mod.0.amount")) == 0.0,
          "a removed route's amount is reset");
}

// The type menu in a card's header switches an oscillator, and its
// controls follow the type.
void switching_an_oscillator_type() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  require(find_item(panel->root, QStringLiteral("novaKnob:nova.osc.b.pw")) != nullptr,
          "a classic oscillator shows its pulse width");
  click(*panel, centre(item(*panel, QStringLiteral("novaDropdown:nova.osc.b.type"))));
  require(item(*panel, QStringLiteral("novaMenu"))->isVisible(), "the type menu opens");
  QTest::keyClick(panel->window.get(), Qt::Key_Down);
  QTest::keyClick(panel->window.get(), Qt::Key_Return);
  settle();
  require(controller.choice(QStringLiteral("nova.osc.b.type")) == 1,
          "Down and Return pick Wavetable");
  require(item(*panel, QStringLiteral("novaOscDisplay:b"))->property("type").toInt() == 1,
          "the display shows the wavetable");
  require(find_item(panel->root, QStringLiteral("novaKnob:nova.osc.b.position")) != nullptr &&
              find_item(panel->root, QStringLiteral("novaKnob:nova.osc.b.pw")) == nullptr,
          "the card swaps in the wavetable's controls");
  require(item(*panel, QStringLiteral("novaEdit:b"))->isVisible(), "a wavetable can be edited");
}

// Dragging an LFO point is one document gesture: begin, updates, commit.
void editing_an_lfo_point() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  lay_out_tabs(*panel);
  click(*panel, centre(item(*panel, QStringLiteral("novaSource:lfo.3"))));
  auto* editor = item(*panel, QStringLiteral("novaCurveEditor:lfo.3"));
  const double pad = editor->property("pad").toDouble();
  const auto at = [&](double x, double y) {
    return editor
        ->mapToScene(QPointF(pad + x * (editor->width() - 2 * pad),
                             editor->height() - pad - y * (editor->height() - 2 * pad - 10)))
        .toPoint();
  };
  // The default triangle's peak, pulled down.
  const auto before = adapter.document_submissions().size();
  drag(*panel, at(0.5, 1.0), at(0.5, 0.6));
  const auto& submissions = adapter.document_submissions();
  require(submissions.size() >= before + 3, "the drag submitted a gesture");
  require(submissions[before].gesture == NativeSynthGesture::begin, "the drag begins");
  require(submissions[before + 1].gesture == NativeSynthGesture::update, "the drag updates");
  require(submissions.back().gesture == NativeSynthGesture::commit, "the release commits");
  const auto edited = adapter.document();
  const auto& curves = edited.curves;
  require(curves.size() == 1 && curves.front().id == "lfo.3", "the edit wrote LFO 3's shape");
  const auto& points = curves.front().points;
  require(points.size() == 3 && points[1].x == 0.5 && points[1].y < 0.7 && points[1].y > 0.5,
          "the peak moved down on its grid line");

  // Pressing empty space adds a point there, snapped to the grid.
  drag(*panel, at(0.26, 0.2), at(0.26, 0.2), 1);
  const auto grown = adapter.document();
  const auto& added = grown.curves.front().points;
  require(added.size() == 4 && added[1].x == 0.25, "a press adds a snapped point");
}

// A panel on the FX page whose Main chain holds these modules at uids 1, 2, ...
struct FxRig {
  InMemoryNativeSynthAdapter adapter{oscillate::native_dsp::default_values(table())};
  std::unique_ptr<NativeSynthDeviceController> controller;
  std::unique_ptr<Panel> panel;

  explicit FxRig(const std::vector<std::string>& main) {
    project::NativeSynthDocument document;
    project::NativeChain chain{"fx.main", {}};
    std::uint32_t uid = 1;
    for (const auto& module : main) {
      chain.modules.push_back({uid++, module});
    }
    document.chains = {chain};
    adapter.submit_document(document, NativeSynthGesture::single);
    controller = std::make_unique<NativeSynthDeviceController>(table(), adapter);
    panel = load_panel(*controller);
    for (const auto& slot : chain.modules) {
      QVariant values;
      QMetaObject::invokeMethod(panel->root, "moduleDefaults", Q_RETURN_ARG(QVariant, values),
                                Q_ARG(QVariant, static_cast<int>(slot.uid)),
                                Q_ARG(QVariant, QString::fromStdString(slot.module)),
                                Q_ARG(QVariant, QVariant()));
      controller->set_values(values.metaType() == QMetaType::fromType<QJSValue>()
                                 ? values.value<QJSValue>().toVariant().toMap()
                                 : values.toMap());
    }
    show_page(*panel, 1);
  }
  [[nodiscard]] std::vector<std::uint32_t> uids(std::string_view chain) const {
    std::vector<std::uint32_t> out;
    const auto document = adapter.document();
    for (const auto& c : document.chains) {
      if (c.id == chain) {
        for (const auto& slot : c.modules) {
          out.push_back(slot.uid);
        }
      }
    }
    return out;
  }
};

// Dragging a rack row onto another place reorders the chain as one edit;
// "+ Add effect" opens the browser and adds a module in a free uid with
// its defaults.
void reordering_and_adding_effects() {
  FxRig rig({"distortion", "filter", "delay"});
  auto& panel = *rig.panel;
  auto& adapter = rig.adapter;
  const auto before = adapter.document_submissions().size();
  drag(panel, centre(item(panel, QStringLiteral("novaFxRow:1"))),
       centre(item(panel, QStringLiteral("novaFxRow:3"))) + QPoint(0, 10), 10);
  require(rig.uids("fx.main") == std::vector<std::uint32_t>{2, 3, 1},
          "the distortion moved to the end, with its uid");
  require(adapter.document_submissions().size() == before + 1 &&
              adapter.document_submissions().back().gesture == NativeSynthGesture::single,
          "a reorder is one edit");

  // A click without a drag selects the row's strip.
  click(panel, centre(item(panel, QStringLiteral("novaFxRow:3"))));
  require(item(panel, QStringLiteral("novaFxPage"))->property("selected").toInt() == 3,
          "a click selects the module");

  click(panel, centre(item(panel, QStringLiteral("novaAddEffect"))));
  require(item(panel, QStringLiteral("novaFxBrowser"))->isVisible(), "the browser opens");
  click(panel, centre(item(panel, QStringLiteral("novaFxBrowserCategory:Motion"))));
  click(panel, centre(item(panel, QStringLiteral("novaFxBrowserItem:shifter"))));
  require(!item(panel, QStringLiteral("novaFxBrowser"))->isVisible(), "and closes on a pick");
  const auto added = adapter.document();
  const auto& grown = added.chains.front().modules;
  require(grown.size() == 4 && grown.back().uid == 4 && grown.back().module == "shifter",
          "a new shifter takes uid 4 at the end");
  require(std::fabs(rig.controller->value(QStringLiteral("nova.fx.4.p0")) - 0.6) < 1e-9 &&
              std::fabs(rig.controller->value(QStringLiteral("nova.fx.4.mix")) - 0.5) < 1e-9 &&
              rig.controller->value(QStringLiteral("nova.fx.4.on")) == 1.0,
          "the new slot starts from the module's defaults");
  require(find_item(panel.root, QStringLiteral("novaKnob:nova.fx.4.p0")) != nullptr,
          "the new strip shows its knobs");
}

// Dropping a row on another bus's tab moves the module there, uid and all.
void moving_an_effect_to_a_bus() {
  FxRig rig({"distortion", "filter", "delay"});
  auto& panel = *rig.panel;
  rig.controller->set_value(QStringLiteral("nova.fx.3.p1"), 0.77);
  drag(panel, centre(item(panel, QStringLiteral("novaFxRow:3"))),
       centre(item(panel, QStringLiteral("novaBus:1"))), 10);
  require(rig.uids("fx.main") == std::vector<std::uint32_t>{1, 2}, "the delay left Main");
  require(rig.uids("fx.bus.1") == std::vector<std::uint32_t>{3}, "and is on Bus 1");
  require(std::fabs(rig.controller->value(QStringLiteral("nova.fx.3.p1")) - 0.77) < 1e-9,
          "with its settings");
}

// The slot menu's Duplicate puts a copy with the same values right after.
void duplicating_an_effect() {
  FxRig rig({"reverb", "delay"});
  auto& panel = *rig.panel;
  rig.controller->set_value(QStringLiteral("nova.fx.1.p1"), 0.81);
  click(panel, centre(item(panel, QStringLiteral("novaFxMenu:1"))));
  require(item(panel, QStringLiteral("novaMenu"))->isVisible(), "⋯ opens the slot menu");
  QTest::keyClick(panel.window.get(), Qt::Key_Down);
  QTest::keyClick(panel.window.get(), Qt::Key_Return);
  settle();
  require(rig.uids("fx.main") == std::vector<std::uint32_t>{1, 3, 2},
          "the copy sits right after the original");
  const auto document = rig.adapter.document();
  require(document.chains.front().modules[1].module == "reverb", "a reverb");
  require(std::fabs(rig.controller->value(QStringLiteral("nova.fx.3.p1")) - 0.81) < 1e-9,
          "with the original's values");
}

// Clicking a strip's name opens the browser to swap the module in place.
void swapping_through_the_browser() {
  FxRig rig({"distortion", "filter"});
  auto& panel = *rig.panel;
  click(panel, centre(item(panel, QStringLiteral("novaFxModule:2"))));
  require(item(panel, QStringLiteral("novaFxBrowser"))->isVisible(), "the name opens the browser");
  click(panel, centre(item(panel, QStringLiteral("novaFxBrowserItem:reverb"))));
  const auto document = rig.adapter.document();
  const auto& modules = document.chains.front().modules;
  require(modules.size() == 2 && modules[1].uid == 2 && modules[1].module == "reverb",
          "the filter became a reverb in the same slot");
  require(std::fabs(rig.controller->value(QStringLiteral("nova.fx.2.p0")) - 0.6) < 1e-9 &&
              std::fabs(rig.controller->value(QStringLiteral("nova.fx.2.mix")) - 0.25) < 1e-9,
          "with the reverb's defaults");
}

// Dragging an EQ band's node moves its frequency and gain as one gesture.
void dragging_an_eq_node() {
  FxRig rig({"eq"});
  auto& panel = *rig.panel;
  auto& controller = *rig.controller;
  const double freq = controller.value(QStringLiteral("nova.fx.1.p5"));
  const double gain = controller.value(QStringLiteral("nova.fx.1.p6"));
  require(std::fabs(gain - 0.5) < 1e-9, "band 2 starts flat");
  const auto before = rig.adapter.submissions().size();
  auto* node = item(panel, QStringLiteral("novaFxEqNode:1:1"));
  const QPoint from = centre(node);
  drag(panel, from, from + QPoint(60, -30), 10);
  require(controller.value(QStringLiteral("nova.fx.1.p5")) > freq + 0.05,
          "dragging right raises the frequency");
  require(controller.value(QStringLiteral("nova.fx.1.p6")) > gain + 0.05,
          "dragging up raises the gain");
  const auto& values = rig.adapter.submissions();
  require(values.size() > before + 2 && values[before].gesture == NativeSynthGesture::begin &&
              values.back().gesture == NativeSynthGesture::commit,
          "the node drag is one gesture");
}

// ✎ EDIT opens the wavetable editor; DONE commits the table as one edit.
void wavetable_editor_done() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  click(*panel, centre(item(*panel, QStringLiteral("novaEdit:a"))));
  auto* editor = item(*panel, QStringLiteral("novaTableEditor"));
  require(editor->isVisible(), "the editor opens over the panel");
  editor->setProperty("table", QStringLiteral("glass_formant"));
  QCoreApplication::processEvents();
  const auto before = adapter.document_submissions().size();
  click(*panel, centre(item(*panel, QStringLiteral("novaEditorDone"))));
  const auto& submissions = adapter.document_submissions();
  require(submissions.size() == before + 1 &&
              submissions.back().gesture == NativeSynthGesture::single,
          "DONE commits one edit");
  const auto committed = adapter.document();
  const auto& media = committed.media;
  require(media.size() == 1 && media.front().id == "osc.a" && media.front().frame_length == 2048 &&
              std::get<project::NativeBuiltinMedia>(media.front().media).name == "glass_formant",
          "the oscillator plays the chosen table");
  require(find_item(panel->root, QStringLiteral("novaTableEditor")) == nullptr,
          "the editor closes");
}

// The telemetry frame lights the panel: voices, live route dots, playheads.
void telemetry_goes_live() {
  InMemoryNativeSynthAdapter adapter(demo_values());
  adapter.submit_document(demo_document(), NativeSynthGesture::single);
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  // Filter 1's cutoff knob, on MAIN, shows the routes into it.
  require(!panel->root->property("live").toBool(), "nothing plays: not live");
  require(find_item(panel->root, QStringLiteral("novaModDot:nova.filter.1.cutoff:1")) == nullptr,
          "no live dots while nothing plays");
  adapter.set_telemetry(live_frame());
  require(QTest::qWaitFor([&] { return panel->root->property("live").toBool(); }, 1000),
          "the frame arrives");
  QCoreApplication::processEvents();
  require(item(*panel, QStringLiteral("novaVoices"))->property("text").toString() ==
              QStringLiteral("3 / 16"),
          "voices in use against the polyphony");
  require(item(*panel, QStringLiteral("novaModDot:nova.filter.1.cutoff:1"))->isVisible() &&
              item(*panel, QStringLiteral("novaModDot:nova.filter.1.cutoff:2"))->isVisible(),
          "each route into the cutoff has a live dot");
  panel->root->setProperty("modTab", QStringLiteral("lfo.1"));
  settle();
  require(std::fabs(
              item(*panel, QStringLiteral("novaCurveEditor:lfo.1"))->property("phase").toDouble() -
              0.3) < 1e-6,
          "LFO 1 shows its phase");
  adapter.set_telemetry({});
  require(QTest::qWaitFor([&] { return !panel->root->property("live").toBool(); }, 1000),
          "telemetry gone: not live");
}

// The VOICES page: Age and the tolerances are bound, New synth steps the
// seed as one edit, painting a lane is one document gesture that marks only
// the painted voice, Spread writes every voice as one edit and Back to seed
// drops them; the unison lanes have one bar per unison voice, and the
// per-voice sources are named and draggable; Re-roll, Flutter and the
// voice cards are bound, and the lanes follow the cards.
void voices_page_paints_lanes() {
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table()));
  NativeSynthDeviceController controller(table(), adapter);
  auto panel = load_panel(controller);
  show_page(*panel, 5);
  for (const char* name : {"novaVoicesPage", "novaAnalogueCard", "novaAgeKnob", "novaVoiceLanes",
                           "novaVoiceCards", "novaVoiceCard:8"}) {
    require(item(*panel, QString::fromLatin1(name))->isVisible(),
            std::string("the page shows ") + name);
  }
  for (const char* name :
       {"novaSlider:nova.analogue.pitch", "novaSlider:nova.analogue.shape",
        "novaSlider:nova.analogue.drift.rate", "novaToggle:nova.analogue.freerun",
        "novaSegs:nova.analogue.scope", "novaLed:nova.analogue.on",
        "novaSlider:nova.analogue.reroll", "novaSlider:nova.analogue.flutter.amount",
        "novaSlider:nova.analogue.flutter.tone", "novaCards:nova.analogue.cards"}) {
    require(find_item(panel->root, QString::fromLatin1(name)) != nullptr,
            std::string("the Analogue card binds ") + name);
  }

  // Age: dragging the dial up raises it as one gesture and renames it.
  const QString age = QStringLiteral("nova.analogue.age");
  controller.set_value(age, 0.35);
  const double age_before = controller.value(age);
  auto* dial = item(*panel, QStringLiteral("novaAgeDial:nova.analogue.age"));
  const auto values_before = adapter.submissions().size();
  drag(*panel, centre(dial), centre(dial) - QPoint(0, 40));
  require(controller.value(age) > age_before + 0.2, "dragging Age up raises it");
  const auto& values = adapter.submissions();
  require(values.size() > values_before + 2 &&
              values[values_before].gesture == NativeSynthGesture::begin &&
              values.back().gesture == NativeSynthGesture::commit,
          "the Age drag is one gesture");
  require(item(*panel, QStringLiteral("novaAgeName"))->property("text").toString() ==
              QStringLiteral("Well-used"),
          "Age names its range");

  // New synth: another seed in range.
  const QString seed = QStringLiteral("nova.analogue.seed");
  const double seed_before = controller.value(seed);
  click(*panel, centre(item(*panel, QStringLiteral("novaNewSynth"))));
  require(controller.value(seed) != seed_before && controller.value(seed) >= 1 &&
              controller.value(seed) <= 9'999,
          "New synth picks another seed");

  // Painting voice 3 is one document gesture and edits only voice 3.
  auto* lanes = item(*panel, QStringLiteral("novaVoiceLanes"));
  require(lanes->property("count").toInt() == 8, "the note lanes have eight voices");
  const double pad = lanes->property("pad").toDouble();
  const double cell = lanes->property("cellWidth").toDouble();
  const double mid = lanes->property("mid").toDouble();
  const double amp = lanes->property("amp").toDouble();
  const auto at = [&](double voice, double value) {
    return lanes->mapToScene(QPointF(pad + (voice + 0.5) * cell, mid - value * amp)).toPoint();
  };
  const auto documents_before = adapter.document_submissions().size();
  drag(*panel, at(2, 0.1), at(2, 0.5));
  const auto& documents = adapter.document_submissions();
  require(documents.size() >= documents_before + 2 &&
              documents[documents_before].gesture == NativeSynthGesture::begin &&
              documents.back().gesture == NativeSynthGesture::commit,
          "a paint drag is one gesture");
  const auto painted = js_entry(lanes->property("bars"), 2);
  require(painted.value(QStringLiteral("edited")).toBool() &&
              std::fabs(painted.value(QStringLiteral("value")).toDouble() - 0.5) < 0.03,
          "voice 3 is painted to +0.5");
  require(!js_entry(lanes->property("bars"), 1).value(QStringLiteral("edited")).toBool() &&
              !js_entry(lanes->property("bars"), 3).value(QStringLiteral("edited")).toBool(),
          "its neighbours keep their seeded values");

  // Spread: every voice, rising, as one edit.
  const auto spread_before = adapter.document_submissions().size();
  click(*panel, centre(item(*panel, QStringLiteral("novaLaneSpread"))));
  require(adapter.document_submissions().size() > spread_before &&
              adapter.document_submissions().back().gesture == NativeSynthGesture::commit,
          "Spread is one edit");
  double previous = -2.0;
  for (int v = 0; v < 8; ++v) {
    const auto bar = js_entry(lanes->property("bars"), v);
    require(bar.value(QStringLiteral("edited")).toBool(), "Spread edits every voice");
    require(bar.value(QStringLiteral("value")).toDouble() > previous,
            "Spread rises across the voices");
    previous = bar.value(QStringLiteral("value")).toDouble();
  }

  // Back to seed drops them all.
  click(*panel, centre(item(*panel, QStringLiteral("novaLaneBackToSeed"))));
  for (int v = 0; v < 8; ++v) {
    require(!js_entry(lanes->property("bars"), v).value(QStringLiteral("edited")).toBool(),
            "Back to seed restores the seeded values");
  }

  // Osc A's unison lanes: one bar per unison voice.
  controller.set_value(QStringLiteral("nova.osc.a.unison.voices"), 5);
  click(*panel, centre(item(*panel, QStringLiteral("novaLaneVoices:1"))));
  require(lanes->property("count").toInt() == 5, "Osc A's unison lanes have five voices");

  // Voice cards: Per voice at first; six cards deal notes over six lanes
  // and six voice cards.
  click(*panel, centre(item(*panel, QStringLiteral("novaLaneVoices:0"))));
  require(lanes->property("count").toInt() == 8, "back on the note lanes");
  require(item(*panel, QStringLiteral("novaCardsValue"))->property("text").toString() ==
              QStringLiteral("Per voice"),
          "no cards reads Per voice");
  for (int i = 0; i < 6; ++i) {
    click(*panel, centre(item(*panel, QStringLiteral("novaCardsStep:up"))));
  }
  require(controller.value(QStringLiteral("nova.analogue.cards")) == 6, "six steps give six cards");
  require(item(*panel, QStringLiteral("novaCardsValue"))->property("text").toString() ==
              QStringLiteral("6"),
          "the stepper reads 6");
  require(lanes->property("count").toInt() == 6, "the note lanes show one bar per card");
  settle();
  auto* seventh = find_item(panel->root, QStringLiteral("novaVoiceCard:7"));
  require(item(*panel, QStringLiteral("novaVoiceCard:6"))->isVisible() &&
              (seventh == nullptr || !seventh->isVisible()),
          "and six voice cards");
  click(*panel, centre(item(*panel, QStringLiteral("novaCardsStep:down"))));
  require(controller.value(QStringLiteral("nova.analogue.cards")) == 5, "a step down gives five");

  // The per-voice sources.
  QVariant name;
  QMetaObject::invokeMethod(panel->root, "sourceName", Q_RETURN_ARG(QVariant, name),
                            Q_ARG(QVariant, QStringLiteral("voice.mod_a")));
  require(name.toString() == QStringLiteral("Voice Mod A"), "Voice Mod A is named");
  require(find_item(panel->root, QStringLiteral("novaHandle:voice.spread")) != nullptr,
          "Voice Spread has a handle");
}

void screenshots_if_requested() {
  const char* directory = std::getenv("NOVA_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0') {
    return;
  }
  QDir().mkpath(QString::fromLocal8Bit(directory));
  const std::array<std::pair<const char*, int>, 14> shots{{{"/nova_overview.png", 0},
                                                           {"/nova_overview_lfo.png", 0},
                                                           {"/nova_expanded_osc_a.png", 0},
                                                           {"/nova_narrow.png", 0},
                                                           {"/nova_browser.png", 0},
                                                           {"/nova_constellation.png", 0},
                                                           {"/nova_fx.png", 1},
                                                           {"/nova_fx_eq.png", 1},
                                                           {"/nova_fx_bus.png", 1},
                                                           {"/nova_matrix.png", 2},
                                                           {"/nova_arp_clip.png", 3},
                                                           {"/nova_global.png", 4},
                                                           {"/nova_voices.png", 5},
                                                           {"/nova_editor.png", 0}}};
  for (const auto& [file, page] : shots) {
    InMemoryNativeSynthAdapter adapter(demo_values());
    adapter.submit_document(demo_document(), NativeSynthGesture::single);
    NativeSynthDeviceController controller(table(), adapter);
    auto panel = load_panel(controller);
    panel->root->setProperty("page", page);
    if (std::string_view(file) == "/nova_overview_lfo.png") {
      panel->root->setProperty("modTab", QStringLiteral("lfo.1"));
    }
    if (std::string_view(file) == "/nova_expanded_osc_a.png") {
      panel->root->setProperty("focusModule", QStringLiteral("a"));
    }
    if (std::string_view(file) == "/nova_narrow.png") {
      // Two columns, scrolled to the top.
      const double narrow = panel->root->property("overviewNarrowWidth").toDouble() - 1;
      panel->root->setWidth(narrow);
      panel->window->resize(static_cast<int>(narrow), panel->window->height());
    }
    if (std::string_view(file) == "/nova_fx_bus.png") {
      panel->root->setProperty("fxChain", QStringLiteral("fx.bus.1"));
    }
    if (std::string_view(file) == "/nova_fx_eq.png") {
      // Scroll Main's strips down to the EQ.
      settle();
      auto* stack = find_item(panel->root, QStringLiteral("novaFxStack"));
      auto* eq = find_item(panel->root, QStringLiteral("novaFxStrip:7"));
      require(stack != nullptr && eq != nullptr, "the EQ strip is on Main");
      const double end = stack->property("contentHeight").toDouble() - stack->height();
      stack->setProperty("contentY", std::max(0.0, std::min(eq->y(), end)));
    }
    if (std::string_view(file) == "/nova_editor.png") {
      panel->root->setProperty("editorOsc", QStringLiteral("a"));
    }
    if (std::string_view(file) == "/nova_browser.png") {
      QMetaObject::invokeMethod(panel->root->property("presetBrowser").value<QObject*>(), "open");
    }
    if (std::string_view(file) == "/nova_constellation.png") {
      panel->root->setProperty("constellationOpen", true);
      // Analysis renders every preset once.
      QTest::qWait(8000);
    }
    adapter.set_telemetry(live_frame());
    QTest::qWait(400);
    const QString path = QString::fromLocal8Bit(directory) + QString::fromLatin1(file);
    require(panel->window->grabWindow().save(path, "PNG"), "screenshot saved");
    std::cout << "wrote " << path.toStdString() << '\n';
  }
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  // As the app: the panel's Canvas text names Inter and JetBrains Mono.
  require(oscillate::quick::register_bundled_fonts(), "the bundled fonts load");
  try {
    engine_matches_the_contract();
    panel_loads_and_visits_every_page();
    overview_shows_every_module();
    an_off_module_fades();
    expanding_a_card_in_place();
    the_strip_jumps_to_cards();
    a_mod_tab_selects_its_source();
    zoom_and_constellation();
    pages_reflow_with_the_panel();
    key_strip_plays_notes();
    every_parameter_has_a_control();
    drag_to_modulate_and_edit_the_route();
    switching_an_oscillator_type();
    editing_an_lfo_point();
    reordering_and_adding_effects();
    moving_an_effect_to_a_bus();
    duplicating_an_effect();
    swapping_through_the_browser();
    dragging_an_eq_node();
    wavetable_editor_done();
    telemetry_goes_live();
    voices_page_paints_lanes();
    screenshots_if_requested();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "nova panel contract passed\n";
  return 0;
}
