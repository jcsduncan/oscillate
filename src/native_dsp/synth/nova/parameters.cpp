#include "oscillate/native_dsp/synth/nova/parameters.hpp"

#include "nova_presets.hpp"
#include "oscillate/native_dsp/synth/lock.hpp"
#include "oscillate/native_dsp/synth/nova/analogue.hpp"

#include <array>
#include <cstddef>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

// The table follows contract 08a §2 row by row; a change there is made
// there first.
namespace oscillate::native_dsp::synth::nova {
namespace {

using Choices = std::span<const std::string_view>;

constexpr std::array<std::string_view, 2> off_on{"Off", "On"};
constexpr std::array<std::string_view, 6> osc_types{"Classic",     "Wavetable", "Sample",
                                                    "Multisample", "Granular",  "Spectral"};
constexpr std::array<std::string_view, 5> classic_shapes{"Sine", "Triangle", "Saw", "Square",
                                                         "Pulse"};
constexpr std::array<std::string_view, 2> interpolations{"Crossfade", "Spectral"};
constexpr std::array<std::string_view, 16> warp_modes{
    "Off", "Bend +", "Bend −", "Sync", "Window Sync", "Mirror", "Asym", "Quantise",
    "PWM", "Flip",   "Remap",  "Fold", "FM",          "PM",     "AM",   "RM"};
constexpr std::array<std::string_view, 5> warp_sources{"Osc A", "Osc B", "Osc C", "Sub", "Noise"};
constexpr std::array<std::string_view, 5> unison_curves{"Linear", "Exponential", "Super", "Inverse",
                                                        "Random"};
constexpr std::array<std::string_view, 6> unison_stacks{"Off",   "Octave",         "2 Octaves",
                                                        "Fifth", "Fifth + Octave", "Chord"};
constexpr std::array<std::string_view, 3> source_routes{"Filter 1", "Filter 2", "Direct"};
constexpr std::array<std::string_view, 4> loop_modes{"Off", "Forward", "Ping-pong", "Reverse"};
constexpr std::array<std::string_view, 4> grain_windows{"Hann", "Gauss", "Tukey", "Triangle"};
constexpr std::array<std::string_view, 4> fft_sizes{"1024", "2048", "4096", "8192"};
constexpr std::array<std::string_view, 4> sub_shapes{"Sine", "Triangle", "Saw", "Square"};
constexpr std::array<std::string_view, 3> sub_octaves{"−2", "−1", "0"};
constexpr std::array<std::string_view, 2> noise_modes{"Loop", "One-shot"};
constexpr std::array<std::string_view, 22> filter_models{
    "Ladder 24", "Ladder 12", "Diode",   "MG Low",   "OTA",       "LP 12",   "LP 24", "BP 12",
    "BP 24",     "HP 12",     "HP 24",   "Notch",    "Peak",      "Formant", "Vowel", "Comb +",
    "Comb −",    "Phaser",    "Flanger", "LP→BP→HP", "Dual Peak", "Ring"};
constexpr std::array<std::string_view, 3> routings{"Serial", "Parallel", "Split"};
constexpr std::array<std::string_view, lfo_sync_count> lfo_syncs{
    "Hz",     "8 bars", "4 bars", "2 bars", "1 bar", "1/2 D",  "1/2",
    "1/2 T",  "1/4 D",  "1/4",    "1/4 T",  "1/8 D", "1/8",    "1/8 T",
    "1/16 D", "1/16",   "1/16 T", "1/32 D", "1/32",  "1/32 T", "1/64"};
constexpr std::array<std::string_view, 3> lfo_modes{"Trig", "Env", "Free"};
constexpr std::array<std::string_view, 2> lfo_scopes{"Voice", "Global"};
constexpr std::array<std::string_view, 3> oversamples{"Off", "2×", "4×"};
constexpr std::array<std::string_view, 2> send_points{"Pre", "Post"};
constexpr std::array<std::string_view, 15> arp_rates{"1/1",   "1/2",    "1/2 T", "1/4 D",  "1/4",
                                                     "1/4 T", "1/8 D",  "1/8",   "1/8 T",  "1/16 D",
                                                     "1/16",  "1/16 T", "1/32",  "1/32 T", "1/64"};
constexpr std::array<std::string_view, 2> arp_velocities{"Played", "Pattern"};
constexpr std::array<std::string_view, 2> arp_note_modes{"Held", "Scale"};
constexpr std::array<std::string_view, 4> seq_clips{"Clip 1", "Clip 2", "Clip 3", "Clip 4"};
constexpr std::array<std::string_view, 2> seq_triggers{"Transport", "Note"};
constexpr std::array<std::string_view, 3> voicings{"Poly", "Mono", "Legato"};
constexpr std::array<std::string_view, 2> portamento_modes{"Legato only", "Always"};
constexpr std::array<std::string_view, 2> portamento_rates{"Constant time", "Constant rate"};
constexpr std::array<std::string_view, portamento_division_count> portamento_divisions{
    "1/1",    "1/2",  "1/4 D",  "1/4",    "1/4 T", "1/8 D",  "1/8",  "1/8 T",
    "1/16 D", "1/16", "1/16 T", "1/32 D", "1/32",  "1/32 T", "1/64", "1/128"};
constexpr std::array<std::string_view, 3> qualities{"Eco", "High", "Ultra"};
constexpr std::array<std::string_view, 2> tunings{"12-TET", "Scale file"};
constexpr std::array<std::string_view, 3> analogue_scopes{"Notes + unison", "Notes only",
                                                          "Unison only"};
constexpr std::array<std::string_view, 4> noise_am_sources{"Off", "Osc A", "Osc B", "Osc C"};

constexpr double glide = synth_glide_seconds;
constexpr double pitch_glide = 0.005;

class Builder {
public:
  // A continuous, automatable modulation destination.
  std::size_t lin(std::string id, std::string_view name, std::string_view group,
                  std::string_view unit, double min, double max, double def, double skew = 1.0,
                  double glide_seconds = glide) {
    return add(std::move(id), {{},
                               name,
                               group,
                               unit,
                               ParameterScale::linear,
                               min,
                               max,
                               def,
                               {},
                               ParameterTiming::live,
                               glide_seconds,
                               true,
                               skew});
  }
  std::size_t log(std::string id, std::string_view name, std::string_view group,
                  std::string_view unit, double min, double max, double def) {
    return add(std::move(id), {{},
                               name,
                               group,
                               unit,
                               ParameterScale::logarithmic,
                               min,
                               max,
                               def,
                               {},
                               ParameterTiming::live,
                               glide,
                               true,
                               1.0});
  }
  // Continuous but not automatable (pattern data, editor settings).
  std::size_t fixed(std::string id, std::string_view name, std::string_view group,
                    std::string_view unit, double min, double max, double def) {
    return add(std::move(id), {{},
                               name,
                               group,
                               unit,
                               ParameterScale::linear,
                               min,
                               max,
                               def,
                               {},
                               ParameterTiming::live,
                               glide,
                               false,
                               1.0});
  }
  std::size_t step(std::string id, std::string_view name, std::string_view group,
                   std::string_view unit, double min, double max, double def) {
    return add(std::move(id), {{},
                               name,
                               group,
                               unit,
                               ParameterScale::stepped,
                               min,
                               max,
                               def,
                               {},
                               ParameterTiming::live,
                               0.0,
                               false,
                               1.0});
  }
  std::size_t choice(std::string id, std::string_view name, std::string_view group, Choices choices,
                     double def) {
    return add(std::move(id), {{},
                               name,
                               group,
                               "",
                               ParameterScale::choice,
                               0.0,
                               static_cast<double>(choices.size() - 1),
                               def,
                               choices,
                               ParameterTiming::live,
                               0.0,
                               false,
                               1.0});
  }

  std::vector<ParameterDescriptor> parameters;

private:
  std::size_t add(std::string id, ParameterDescriptor descriptor) {
    ids_.push_back(std::move(id));
    descriptor.id = ids_.back();
    parameters.push_back(descriptor);
    return parameters.size() - 1;
  }
  std::deque<std::string> ids_; // Stable storage for the ids' views.
};

struct Table {
  std::vector<ParameterDescriptor> parameters;
  ParameterIndex index{};
  EngineDescriptor descriptor{};
};

std::string join(std::string_view a, std::string_view b) { return std::string(a) + std::string(b); }

const Table& table() {
  static const Table built = [] {
    // Group names and ids outlive the builder: static storage below.
    static std::deque<std::string> groups;
    const auto group = [](std::string name) -> std::string_view {
      groups.push_back(std::move(name));
      return groups.back();
    };
    static Builder b;
    Table t;
    auto& x = t.index;
    x.master = b.lin("nova.master", "Master", "Master", "dB", -60.0, 6.0, 0.0);

    constexpr std::array<std::string_view, 3> osc_names{"a", "b", "c"};
    for (std::size_t o = 0; o < oscillator_count; ++o) {
      const auto p = join("nova.osc.", osc_names[o]) + ".";
      const auto g = group("Osc " + std::string(1, static_cast<char>('A' + o)));
      auto& i = x.osc[o];
      i.on = b.choice(p + "on", "On", g, off_on, o == 0 ? 1.0 : 0.0);
      i.type = b.choice(p + "type", "Type", g, osc_types, o == 0 ? 1.0 : 0.0);
      i.level = b.lin(p + "level", "Level", g, "%", 0.0, 1.0, 0.75);
      i.pan = b.lin(p + "pan", "Pan", g, "%", -1.0, 1.0, 0.0);
      i.position = b.lin(p + "position", "Position", g, "%", 0.0, 1.0, 0.0);
      i.shape = b.choice(p + "shape", "Shape", g, classic_shapes, 2.0);
      i.pw = b.lin(p + "pw", "Pulse Width", g, "%", 0.01, 0.99, 0.5);
      i.sync = b.lin(p + "sync", "Sync", g, "×", 1.0, 8.0, 1.0);
      i.drift = b.lin(p + "drift", "Drift", g, "%", 0.0, 1.0, 0.15);
      i.interpolation = b.choice(p + "interpolation", "Interpolation", g, interpolations, 0.0);
      for (std::size_t w = 0; w < warp_slots; ++w) {
        const auto wp = p + "warp." + std::to_string(w + 1) + ".";
        i.warp_mode[w] = b.choice(wp + "mode", "Warp Mode", g, warp_modes, 0.0);
        i.warp_amount[w] = b.lin(wp + "amount", "Warp", g, "%", 0.0, 1.0, 0.0);
        i.warp_source[w] = b.choice(wp + "source", "Warp Source", g, warp_sources,
                                    static_cast<double>((o + 1) % oscillator_count));
      }
      const auto u = p + "unison.";
      i.unison_voices = b.step(u + "voices", "Voices", g, "", 1.0, 16.0, 1.0);
      i.unison_detune = b.lin(u + "detune", "Detune", g, "ct", 0.0, 1'200.0, 18.0, 3.0);
      i.unison_curve = b.choice(u + "curve", "Detune Curve", g, unison_curves, 2.0);
      i.unison_blend = b.lin(u + "blend", "Blend", g, "%", 0.0, 1.0, 0.6);
      i.unison_width = b.lin(u + "width", "Width", g, "%", 0.0, 1.0, 0.8);
      i.unison_stack = b.choice(u + "stack", "Stack", g, unison_stacks, 0.0);
      i.unison_position_spread =
          b.lin(u + "position_spread", "Position Spread", g, "%", 0.0, 1.0, 0.0);
      i.unison_warp_spread = b.lin(u + "warp_spread", "Warp Spread", g, "%", 0.0, 1.0, 0.0);
      i.octave = b.step(p + "octave", "Octave", g, "", -4.0, 4.0, 0.0);
      i.semitone = b.step(p + "semitone", "Semitone", g, "st", -24.0, 24.0, 0.0);
      i.fine = b.lin(p + "fine", "Fine", g, "ct", -100.0, 100.0, 0.0, 1.0, pitch_glide);
      i.pitch = b.lin(p + "pitch", "Pitch", g, "st", -48.0, 48.0, 0.0, 1.0, pitch_glide);
      i.phase = b.lin(p + "phase", "Phase", g, "°", 0.0, 360.0, 0.0);
      i.random_phase =
          b.lin(p + "random_phase", "Random Phase", g, "%", 0.0, 1.0, o == 1 ? 0.3 : 1.0);
      i.route = b.choice(p + "route", "Route", g, source_routes, 0.0);
      const auto s = p + "sample.";
      i.sample_root = b.step(s + "root", "Root", g, "", 0.0, 127.0, 60.0);
      i.sample_keytrack = b.choice(s + "keytrack", "Keytrack", g, off_on, 1.0);
      i.sample_start = b.lin(s + "start", "Start", g, "%", 0.0, 1.0, 0.0);
      i.sample_loop = b.choice(s + "loop", "Loop", g, loop_modes, 1.0);
      i.sample_loop_start = b.lin(s + "loop_start", "Loop Start", g, "%", 0.0, 1.0, 0.0);
      i.sample_loop_end = b.lin(s + "loop_end", "Loop End", g, "%", 0.0, 1.0, 1.0);
      i.sample_crossfade = b.lin(s + "crossfade", "Loop Crossfade", g, "%", 0.0, 1.0, 0.05);
      const auto gr = p + "grain.";
      i.grain_size = b.log(gr + "size", "Grain Size", g, "ms", 10.0, 500.0, 80.0);
      i.grain_density = b.log(gr + "density", "Density", g, "Hz", 1.0, 200.0, 24.0);
      i.grain_scan_rate = b.lin(gr + "scan_rate", "Scan Rate", g, "×", -2.0, 2.0, 0.0);
      i.grain_spray = b.lin(gr + "spray", "Spray", g, "%", 0.0, 1.0, 0.2);
      i.grain_pitch_spray = b.lin(gr + "pitch_spray", "Pitch Spray", g, "st", 0.0, 24.0, 0.0);
      i.grain_window = b.choice(gr + "window", "Window", g, grain_windows, 0.0);
      i.grain_reverse = b.lin(gr + "reverse", "Reverse", g, "%", 0.0, 1.0, 0.0);
      const auto sp = p + "spectral.";
      i.spectral_fft = b.choice(sp + "fft", "FFT Size", g, fft_sizes, 2.0);
      i.spectral_shift = b.lin(sp + "shift", "Shift", g, "st", -24.0, 24.0, 0.0);
      i.spectral_stretch = b.lin(sp + "stretch", "Stretch", g, "×", 0.5, 2.0, 1.0);
      i.spectral_smear = b.lin(sp + "smear", "Smear", g, "%", 0.0, 1.0, 0.0);
      i.spectral_tilt = b.lin(sp + "tilt", "Tilt", g, "dB", -12.0, 12.0, 0.0);
      i.spectral_freeze = b.choice(sp + "freeze", "Freeze", g, off_on, 0.0);
    }

    x.sub_on = b.choice("nova.sub.on", "On", "Sub", off_on, 0.0);
    x.sub_shape = b.choice("nova.sub.shape", "Shape", "Sub", sub_shapes, 0.0);
    x.sub_octave = b.choice("nova.sub.octave", "Octave", "Sub", sub_octaves, 1.0);
    x.sub_level = b.lin("nova.sub.level", "Level", "Sub", "%", 0.0, 1.0, 0.5);
    x.sub_pan = b.lin("nova.sub.pan", "Pan", "Sub", "%", -1.0, 1.0, 0.0);
    x.sub_route = b.choice("nova.sub.route", "Route", "Sub", source_routes, 2.0);
    x.noise_on = b.choice("nova.noise.on", "On", "Noise", off_on, 0.0);
    x.noise_level = b.lin("nova.noise.level", "Level", "Noise", "%", 0.0, 1.0, 0.15);
    x.noise_pitch = b.lin("nova.noise.pitch", "Pitch", "Noise", "st", -48.0, 48.0, 0.0);
    x.noise_keytrack = b.choice("nova.noise.keytrack", "Keytrack", "Noise", off_on, 0.0);
    x.noise_tone = b.lin("nova.noise.tone", "Tone", "Noise", "%", -1.0, 1.0, 0.0);
    x.noise_mode = b.choice("nova.noise.mode", "Mode", "Noise", noise_modes, 0.0);
    x.noise_pan = b.lin("nova.noise.pan", "Pan", "Noise", "%", -1.0, 1.0, 0.0);
    x.noise_route = b.choice("nova.noise.route", "Route", "Noise", source_routes, 0.0);

    for (std::size_t f = 0; f < filter_count; ++f) {
      const auto p = "nova.filter." + std::to_string(f + 1) + ".";
      const auto g = group("Filter " + std::to_string(f + 1));
      auto& i = x.filter[f];
      i.on = b.choice(p + "on", "On", g, off_on, f == 0 ? 1.0 : 0.0);
      i.model = b.choice(p + "model", "Model", g, filter_models, f == 0 ? 0.0 : 13.0);
      i.cutoff = b.log(p + "cutoff", "Cutoff", g, "Hz", 8.0, 22'000.0, f == 0 ? 2'000.0 : 800.0);
      i.resonance = b.lin(p + "resonance", "Resonance", g, "%", 0.0, 1.0, 0.2);
      i.drive = b.lin(p + "drive", "Drive", g, "dB", 0.0, 24.0, 0.0);
      i.fat = b.lin(p + "fat", "Fat", g, "%", 0.0, 1.0, 0.0);
      i.keytrack = b.lin(p + "keytrack", "Keytrack", g, "%", -1.0, 2.0, 0.0);
      i.pan = b.lin(p + "pan", "Pan", g, "%", -1.0, 1.0, 0.0);
      i.mix = b.lin(p + "mix", "Mix", g, "%", 0.0, 1.0, 1.0);
    }
    x.filter_routing = b.choice("nova.filter.routing", "Routing", "Filters", routings, 0.0);

    for (std::size_t e = 0; e < envelope_count; ++e) {
      const auto p = "nova.env." + std::to_string(e + 1) + ".";
      const auto g = group("Env " + std::to_string(e + 1));
      const bool amp = e == 0;
      auto& i = x.env[e];
      i.attack = b.lin(p + "attack", "Attack", g, "s", 0.0, 20.0, amp ? 0.002 : 0.01, 4.0);
      i.hold = b.lin(p + "hold", "Hold", g, "s", 0.0, 10.0, 0.0, 4.0);
      i.decay = b.lin(p + "decay", "Decay", g, "s", 0.0, 20.0, amp ? 0.3 : 0.5, 4.0);
      i.sustain = b.lin(p + "sustain", "Sustain", g, "%", 0.0, 1.0, amp ? 1.0 : 0.4);
      i.release = b.lin(p + "release", "Release", g, "s", 0.0, 20.0, amp ? 0.15 : 0.4, 4.0);
      i.attack_curve = b.lin(p + "attack_curve", "Attack Curve", g, "%", -1.0, 1.0, 0.0);
      i.decay_curve = b.lin(p + "decay_curve", "Decay Curve", g, "%", -1.0, 1.0, 0.4);
      i.release_curve = b.lin(p + "release_curve", "Release Curve", g, "%", -1.0, 1.0, 0.4);
      i.loop = b.choice(p + "loop", "Loop", g, off_on, 0.0);
      i.legato = b.choice(p + "legato", "Legato", g, off_on, 0.0);
      i.velocity = b.lin(p + "velocity", "Velocity", g, "%", 0.0, 1.0, amp ? 0.7 : 0.0);
    }

    for (std::size_t l = 0; l < lfo_count; ++l) {
      const auto p = "nova.lfo." + std::to_string(l + 1) + ".";
      const auto g = group("LFO " + std::to_string(l + 1));
      auto& i = x.lfo[l];
      i.rate = b.log(p + "rate", "Rate", g, "Hz", 0.01, 50.0, 2.0);
      i.sync = b.choice(p + "sync", "Sync", g, lfo_syncs, 9.0);
      i.mode = b.choice(p + "mode", "Mode", g, lfo_modes, 0.0);
      i.rise = b.lin(p + "rise", "Rise", g, "s", 0.0, 10.0, 0.0, 3.0);
      i.delay = b.lin(p + "delay", "Delay", g, "s", 0.0, 10.0, 0.0, 3.0);
      i.smooth = b.lin(p + "smooth", "Smooth", g, "%", 0.0, 1.0, 0.0);
      i.phase = b.lin(p + "phase", "Phase", g, "°", 0.0, 360.0, 0.0);
      i.scope = b.choice(p + "scope", "Scope", g, lfo_scopes, 0.0);
      i.grid = b.step(p + "grid", "Grid", g, "", 1.0, 32.0, 8.0);
    }

    for (std::size_t m = 0; m < macro_count; ++m)
      x.macro[m] = b.lin("nova.macro." + std::to_string(m + 1),
                         group("Macro " + std::to_string(m + 1)), "Macros", "%", 0.0, 1.0, 0.0);
    for (std::size_t c = 0; c < chaos_count; ++c) {
      const auto p = "nova.chaos." + std::to_string(c + 1) + ".";
      const auto g = group("Chaos " + std::to_string(c + 1));
      x.chaos_rate[c] = b.log(p + "rate", "Rate", g, "Hz", 0.01, 50.0, 1.0);
      x.chaos_smooth[c] = b.lin(p + "smooth", "Smooth", g, "%", 0.0, 1.0, 0.5);
    }
    for (std::size_t s = 0; s < route_slots; ++s)
      x.route_amount[s] =
          b.lin("nova.mod." + std::to_string(s) + ".amount",
                group("Route " + std::to_string(s) + " Amount"), "Matrix", "%", -1.0, 1.0, 0.0);

    x.fx_split = b.choice("nova.fx.split", "Split", "FX", off_on, 0.0);
    x.fx_split_frequency =
        b.log("nova.fx.split_frequency", "Split Frequency", "FX", "Hz", 40.0, 2'000.0, 150.0);
    x.fx_main_oversample =
        b.choice("nova.fx.main.oversample", "Main Oversampling", "FX", oversamples, 1.0);
    for (std::size_t bus = 0; bus < 2; ++bus) {
      const auto p = "nova.fx.bus." + std::to_string(bus + 1) + ".";
      const auto g = group("FX Bus " + std::to_string(bus + 1));
      x.fx_bus_oversample[bus] = b.choice(p + "oversample", "Oversampling", g, oversamples, 0.0);
      x.fx_bus_send[bus] = b.lin(p + "send", "Send", g, "%", 0.0, 1.0, 0.0);
      x.fx_bus_send_point[bus] = b.choice(p + "send_point", "Send Point", g, send_points, 1.0);
      x.fx_bus_return[bus] = b.lin(p + "return", "Return", g, "dB", -60.0, 6.0, 0.0);
    }
    for (std::size_t u = 0; u < fx_uids; ++u) {
      const auto p = "nova.fx." + std::to_string(u + 1) + ".";
      const auto g = group("FX " + std::to_string(u + 1));
      auto& i = x.fx_slot[u];
      for (std::size_t k = 0; k < fx_slot_params; ++k)
        i.p[k] = b.lin(p + "p" + std::to_string(k), group("P" + std::to_string(k)), g, "", 0.0, 1.0,
                       0.0);
      i.mix = b.lin(p + "mix", "Mix", g, "%", 0.0, 1.0, 1.0);
      i.on = b.choice(p + "on", "On", g, off_on, 1.0);
    }

    x.arp_on = b.choice("nova.arp.on", "On", "Arpeggiator", off_on, 0.0);
    x.arp_mode = b.choice("nova.arp.mode", "Mode", "Arpeggiator", arp_mode_names, 0.0);
    x.arp_rate = b.choice("nova.arp.rate", "Rate", "Arpeggiator", arp_rates,
                          static_cast<double>(arp_default_rate));
    x.arp_gate = b.lin("nova.arp.gate", "Gate", "Arpeggiator", "%", 0.0, 1.0, 0.6);
    x.arp_swing = b.lin("nova.arp.swing", "Swing", "Arpeggiator", "%", 0.0, arp_max_swing, 0.0);
    x.arp_octaves = b.step("nova.arp.octaves", "Octaves", "Arpeggiator", "", 1.0,
                           static_cast<double>(arp_max_octaves), 1.0);
    x.arp_velocity = b.choice("nova.arp.velocity", "Velocity", "Arpeggiator", arp_velocities, 0.0);
    x.arp_retrigger = b.choice("nova.arp.retrigger", "Retrigger", "Arpeggiator", off_on, 1.0);
    x.arp_latch = b.choice("nova.arp.latch", "Latch", "Arpeggiator", off_on, 0.0);
    // The pattern engine (ADR-0078).
    x.arp_note_mode =
        b.choice("nova.arp.note_mode", "Note Mode", "Arpeggiator", arp_note_modes, 0.0);
    x.arp_scale = b.choice("nova.arp.scale", "Scale", "Arpeggiator", arp_scale_names, 0.0);
    x.arp_root = b.choice("nova.arp.root", "Root", "Arpeggiator", arp_root_names, 0.0);
    x.arp_passing = b.choice("nova.arp.passing", "Passing Notes", "Arpeggiator", off_on, 0.0);
    x.arp_strum = b.lin("nova.arp.strum", "Strum", "Arpeggiator", "%", 0.0, 1.0, arp_default_strum);
    x.arp_euclid_hits =
        b.step("nova.arp.euclid.hits", "Hits", "Arpeggiator", "", 0.0,
               static_cast<double>(arp_max_steps), static_cast<double>(arp_default_euclid_hits));
    x.arp_euclid_steps =
        b.step("nova.arp.euclid.steps", "Steps", "Arpeggiator", "", 1.0,
               static_cast<double>(arp_max_steps), static_cast<double>(arp_default_length));
    x.arp_euclid_rotate = b.step("nova.arp.euclid.rotate", "Rotate", "Arpeggiator", "", 0.0,
                                 static_cast<double>(arp_max_steps - 1), 0.0);
    x.arp_fill = b.lin("nova.arp.fill", "Fill", "Arpeggiator", "%", 0.0, 1.0, 0.0);
    x.arp_variation =
        b.choice("nova.arp.variation", "Variation", "Arpeggiator", arp_variation_names, 0.0);
    x.arp_seed = b.step("nova.arp.seed", "Seed", "Arpeggiator", "", 0.0, arp_max_seed, 0.0);
    x.arp_groove_amount =
        b.lin("nova.arp.groove_amount", "Groove", "Arpeggiator", "%", 0.0, 1.0, 1.0);
    {
      const auto g = group("Arp Groove");
      x.arp_groove_length = b.step("nova.arp.groove.steps", "Groove Steps", g, "", 0.0,
                                   static_cast<double>(arp_groove_steps), 0.0);
      x.arp_groove_grid = b.choice("nova.arp.groove.grid", "Groove Grid", g, arp_groove_grid_names,
                                   static_cast<double>(arp_default_groove_grid));
      for (std::size_t n = 0; n < arp_groove_steps; ++n) {
        const auto p = "nova.arp.groove." + std::to_string(n + 1) + ".";
        const auto label = std::to_string(n + 1);
        x.arp_groove_timing[n] = b.fixed(p + "timing", group("Timing " + label), g, "",
                                         -arp_max_groove_timing, arp_max_groove_timing, 0.0);
        x.arp_groove_velocity[n] = b.fixed(p + "velocity", group("Velocity " + label), g, "", 0.0,
                                           arp_max_groove_velocity, 1.0);
      }
    }
    for (std::size_t l = 0; l < arp_lane_count; ++l) {
      const auto& spec = arp_lane_specs[l];
      const auto p = "nova.arp.lane." + std::string(spec.id) + ".";
      const auto g = group("Arp " + std::string(spec.name));
      auto& lane = x.arp_lane[l];
      lane.length = b.step(p + "length", "Length", g, "", 1.0, static_cast<double>(arp_max_steps),
                           static_cast<double>(arp_default_length));
      for (std::size_t n = 0; n < arp_max_steps; ++n) {
        const auto id = p + std::to_string(n + 1);
        const auto name = group(std::string(spec.name) + " " + std::to_string(n + 1));
        switch (spec.kind) {
        case ArpLaneKind::level:
          lane.step[n] = b.fixed(id, name, g, "%", spec.minimum, spec.maximum, spec.fallback);
          break;
        case ArpLaneKind::integer:
          lane.step[n] = b.step(id, name, g, "", spec.minimum, spec.maximum, spec.fallback);
          break;
        case ArpLaneKind::toggle:
          lane.step[n] = b.choice(id, name, g, off_on, spec.fallback);
          break;
        case ArpLaneKind::choice:
          lane.step[n] =
              b.choice(id, name, g,
                       static_cast<ArpLane>(l) == ArpLane::condition ? Choices(arp_condition_names)
                                                                     : Choices(arp_chord_names),
                       spec.fallback);
          break;
        }
      }
    }
    x.seq_on = b.choice("nova.seq.on", "On", "Sequencer", off_on, 0.0);
    x.seq_clip = b.choice("nova.seq.clip", "Clip", "Sequencer", seq_clips, 0.0);
    x.seq_trigger = b.choice("nova.seq.trigger", "Trigger", "Sequencer", seq_triggers, 0.0);
    x.seq_root = b.step("nova.seq.root", "Root", "Sequencer", "", 0.0, 127.0, 24.0);
    x.seq_follow = b.choice("nova.seq.follow", "Follow", "Sequencer", off_on, 1.0);

    x.voicing = b.choice("nova.global.voicing", "Voicing", "Global", voicings, 0.0);
    x.polyphony = b.step("nova.global.polyphony", "Polyphony", "Global", "", 1.0, 32.0, 16.0);
    x.portamento =
        b.lin("nova.global.portamento", "Portamento", "Global", "s", 0.0, 10.0, 0.0, 4.0);
    x.portamento_curve =
        b.lin("nova.global.portamento_curve", "Portamento Curve", "Global", "%", -1.0, 1.0, 0.0);
    x.portamento_mode =
        b.choice("nova.global.portamento_mode", "Portamento Mode", "Global", portamento_modes, 0.0);
    x.portamento_rate =
        b.choice("nova.global.portamento_rate", "Portamento Rate", "Global", portamento_rates, 0.0);
    x.drift = b.lin("nova.global.drift", "Drift", "Global", "%", 0.0, 1.0, 0.1);
    x.bend_range = b.step("nova.global.bend_range", "Bend Range", "Global", "st", 0.0, 48.0, 2.0);
    x.velocity_curve =
        b.fixed("nova.global.velocity_curve", "Velocity Curve", "Global", "%", -1.0, 1.0, 0.0);
    x.mpe = b.choice("nova.global.mpe", "MPE", "Global", off_on, 0.0);
    x.mpe_range = b.step("nova.global.mpe_range", "MPE Range", "Global", "st", 0.0, 96.0, 48.0);
    x.quality = b.choice("nova.global.quality", "Quality", "Global", qualities, 1.0);
    x.tuning = b.choice("nova.global.tuning", "Tuning", "Global", tunings, 0.0);
    x.a4 = b.fixed("nova.global.a4", "A4", "Global", "Hz", 400.0, 480.0, 440.0);
    x.perf_bend =
        b.lin("nova.perf.bend", "Pitch Bend", "Performance", "", -1.0, 1.0, 0.0, 1.0, pitch_glide);
    x.perf_modwheel = b.lin("nova.perf.wheel", "Mod Wheel", "Performance", "%", 0.0, 1.0, 0.0);
    x.perf_aftertouch = b.lin("nova.perf.pressure", "Pressure", "Performance", "%", 0.0, 1.0, 0.0);
    // Kick & Bass Lock (ADR-0072): below the crossover, the output dips under
    // each followed kick's own amplitude and folds to mono.
    x.lock_on = b.choice(std::string(lock_bass_parameter), "Lock", "Lock", off_on, lock_choice_off);
    x.lock_duck = b.lin("nova.lock.duck", "Duck", "Lock", "%", 0.0, 1.0, 0.65);
    x.lock_crossover = b.log("nova.lock.crossover", "Crossover", "Lock", "Hz", 60.0, 250.0, 120.0);
    x.lock_mono = b.choice("nova.lock.mono", "Mono Lows", "Lock", off_on, 1.0);
    // Analogue (ADR-0073): every voice its own fixed offsets from the seed,
    // scaled by its part's tolerance and Age, plus a slow drift. Appended, so
    // earlier indices keep their place.
    auto& a = x.analogue;
    const std::string_view ag = "Analogue";
    a.on = b.choice("nova.analogue.on", "Analogue", ag, off_on, 0.0);
    a.age = b.lin("nova.analogue.age", "Age", ag, "%", 0.0, 1.0, 0.35);
    a.seed = b.step("nova.analogue.seed", "Seed", ag, "", analogue_seed_min, analogue_seed_max,
                    analogue_seed_default);
    a.pitch = b.lin("nova.analogue.pitch", "Pitch", ag, "%", 0.0, 1.0, 0.6);
    a.cutoff = b.lin("nova.analogue.cutoff", "Cutoff", ag, "%", 0.0, 1.0, 0.5);
    a.resonance = b.lin("nova.analogue.resonance", "Resonance", ag, "%", 0.0, 1.0, 0.3);
    a.env = b.lin("nova.analogue.env", "Env Times", ag, "%", 0.0, 1.0, 0.5);
    a.level = b.lin("nova.analogue.level", "Level", ag, "%", 0.0, 1.0, 0.4);
    a.pan = b.lin("nova.analogue.pan", "Pan", ag, "%", 0.0, 1.0, 0.4);
    a.shape = b.lin("nova.analogue.shape", "Osc Shape", ag, "%", 0.0, 1.0, 0.3);
    a.drift_amount = b.lin("nova.analogue.drift.amount", "Drift", ag, "%", 0.0, 1.0, 0.4);
    a.drift_rate = b.lin("nova.analogue.drift.rate", "Drift Rate", ag, "%", 0.0, 1.0, 0.3);
    a.drift_cutoff =
        b.lin("nova.analogue.drift.cutoff", "Drift on Cutoff", ag, "%", 0.0, 1.0, 0.25);
    a.freerun = b.choice("nova.analogue.freerun", "Free-running", ag, off_on, 1.0);
    a.scope = b.choice("nova.analogue.scope", "Applies To", ag, analogue_scopes, 0.0);
    // Voice cards, re-roll per note and flutter (ADR-0073): all off by
    // default.
    a.cards = b.step("nova.analogue.cards", "Voice Cards", ag, "", 0.0, analogue_cards_max, 0.0);
    a.reroll = b.lin("nova.analogue.reroll", "Re-roll", ag, "%", 0.0, 1.0, 0.0);
    a.flutter_amount = b.lin("nova.analogue.flutter.amount", "Flutter", ag, "%", 0.0, 1.0, 0.0);
    a.flutter_tone =
        b.log("nova.analogue.flutter.tone", "Flutter Tone", ag, "Hz", analogue_flutter_tone_min_hz,
              analogue_flutter_tone_max_hz, analogue_flutter_tone_default_hz);
    // The noise layer's level follows an oscillator of the same voice at
    // audio rate (08a §2.12c). Off by default.
    x.noise_am_source =
        b.choice("nova.noise.am.source", "AM Source", "Noise", noise_am_sources, 0.0);
    x.noise_am_amount = b.lin("nova.noise.am.amount", "AM Amount", "Noise", "%", 0.0, 1.0, 1.0);
    x.noise_am_curve = b.lin("nova.noise.am.curve", "AM Curve", "Noise", "%", -1.0, 1.0, 0.0);
    // Portamento in note values of the project tempo instead of seconds.
    // Off by default.
    x.portamento_sync =
        b.choice("nova.global.portamento_sync", "Portamento Sync", "Global", off_on, 0.0);
    x.portamento_division = b.choice("nova.global.portamento_division", "Portamento Division",
                                     "Global", portamento_divisions, portamento_division_default);
    // Analogue's glide tolerance: each voice card slides a little faster or
    // slower (ADR-0073).
    x.analogue.glide =
        b.lin("nova.analogue.glide", "Glide", "Analogue", "%", 0.0, 1.0, analogue_glide_default);

    t.parameters = b.parameters;
    t.descriptor = {"nova", "Nova", 1, t.parameters, factory_presets()};
    return t;
  }();
  return built;
}

} // namespace

const EngineDescriptor& descriptor() { return table().descriptor; }
const ParameterIndex& index() { return table().index; }

} // namespace oscillate::native_dsp::synth::nova
