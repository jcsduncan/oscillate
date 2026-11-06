#include "oscillate/native_dsp/synth/prism.hpp"

#include "dsp/ramp.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/synth/chorus.hpp"
#include "oscillate/native_dsp/synth/crossfade.hpp"
#include "oscillate/native_dsp/synth/filter.hpp"
#include "oscillate/native_dsp/synth/modulation.hpp"
#include "oscillate/native_dsp/synth/oscillator.hpp"
#include "oscillate/native_dsp/synth/oversampler.hpp"
#include "oscillate/native_dsp/synth/shaping.hpp"
#include "oscillate/native_dsp/synth/tempo_sync.hpp"
#include "oscillate/native_dsp/synth/voice_pool.hpp"
#include "prism_presets.hpp"
#include "prism_table.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>

namespace oscillate::native_dsp::synth::prism {
namespace {

using prism_table::index_of;

// Parameter indices, resolved at compile time from the persisted ids.
namespace p {
constexpr std::size_t osc1_wave = index_of("prism.osc1.wave");
constexpr std::size_t osc1_octave = index_of("prism.osc1.octave");
constexpr std::size_t osc1_semi = index_of("prism.osc1.semi");
constexpr std::size_t osc1_fine = index_of("prism.osc1.fine");
constexpr std::size_t osc1_pw = index_of("prism.osc1.pw");
constexpr std::size_t osc1_level = index_of("prism.osc1.level");
constexpr std::size_t osc1_unison = index_of("prism.osc1.unison");
constexpr std::size_t osc1_detune = index_of("prism.osc1.detune");
constexpr std::size_t osc1_width = index_of("prism.osc1.width");
constexpr std::size_t osc1_phase = index_of("prism.osc1.phase");
constexpr std::size_t osc2_wave = index_of("prism.osc2.wave");
constexpr std::size_t osc2_octave = index_of("prism.osc2.octave");
constexpr std::size_t osc2_semi = index_of("prism.osc2.semi");
constexpr std::size_t osc2_fine = index_of("prism.osc2.fine");
constexpr std::size_t osc2_pw = index_of("prism.osc2.pw");
constexpr std::size_t osc2_level = index_of("prism.osc2.level");
constexpr std::size_t osc2_unison = index_of("prism.osc2.unison");
constexpr std::size_t osc2_detune = index_of("prism.osc2.detune");
constexpr std::size_t osc2_width = index_of("prism.osc2.width");
constexpr std::size_t osc2_phase = index_of("prism.osc2.phase");
constexpr std::size_t osc2_sync = index_of("prism.osc2.sync");
constexpr std::size_t osc2_fm = index_of("prism.osc2.fm");
constexpr std::size_t sub_wave = index_of("prism.sub.wave");
constexpr std::size_t sub_octave = index_of("prism.sub.octave");
constexpr std::size_t sub_level = index_of("prism.sub.level");
constexpr std::size_t noise_colour = index_of("prism.noise.colour");
constexpr std::size_t noise_level = index_of("prism.noise.level");
constexpr std::size_t ring_level = index_of("prism.ring.level");
constexpr std::size_t filter_mode = index_of("prism.filter.mode");
constexpr std::size_t cutoff = index_of("prism.filter.cutoff");
constexpr std::size_t resonance = index_of("prism.filter.resonance");
constexpr std::size_t filter_drive = index_of("prism.filter.drive");
constexpr std::size_t keytrack = index_of("prism.filter.keytrack");
constexpr std::size_t filter_env = index_of("prism.filter.env");
constexpr std::size_t filter_velocity = index_of("prism.filter.velocity");
constexpr std::size_t amp_attack = index_of("prism.amp.attack");
constexpr std::size_t amp_decay = index_of("prism.amp.decay");
constexpr std::size_t amp_sustain = index_of("prism.amp.sustain");
constexpr std::size_t amp_release = index_of("prism.amp.release");
constexpr std::size_t amp_curve = index_of("prism.amp.curve");
constexpr std::size_t amp_velocity = index_of("prism.amp.velocity");
constexpr std::size_t env2_attack = index_of("prism.env2.attack");
constexpr std::size_t env2_decay = index_of("prism.env2.decay");
constexpr std::size_t env2_sustain = index_of("prism.env2.sustain");
constexpr std::size_t env2_release = index_of("prism.env2.release");
constexpr std::size_t env2_curve = index_of("prism.env2.curve");
constexpr std::size_t lfo1_shape = index_of("prism.lfo1.shape");
constexpr std::size_t lfo1_rate = index_of("prism.lfo1.rate");
constexpr std::size_t lfo1_sync = index_of("prism.lfo1.sync");
constexpr std::size_t lfo1_division = index_of("prism.lfo1.division");
constexpr std::size_t lfo1_retrigger = index_of("prism.lfo1.retrigger");
constexpr std::size_t lfo1_fade = index_of("prism.lfo1.fade");
constexpr std::size_t lfo1_mode = index_of("prism.lfo1.mode");
constexpr std::size_t lfo1_vibrato = index_of("prism.lfo1.vibrato");
constexpr std::size_t lfo1_wheel = index_of("prism.lfo1.wheel");
constexpr std::size_t lfo2_shape = index_of("prism.lfo2.shape");
constexpr std::size_t lfo2_cutoff = index_of("prism.lfo2.cutoff");
constexpr std::size_t lfo2_pw = index_of("prism.lfo2.pw");
constexpr std::size_t mod1_source = index_of("prism.mod1.source");
constexpr std::size_t voice_mode = index_of("prism.voice.mode");
constexpr std::size_t glide = index_of("prism.voice.glide");
constexpr std::size_t glide_mode = index_of("prism.voice.glide_mode");
constexpr std::size_t glide_when = index_of("prism.voice.glide_when");
constexpr std::size_t bend_up = index_of("prism.voice.bend_up");
constexpr std::size_t bend_down = index_of("prism.voice.bend_down");
constexpr std::size_t wheel = index_of("prism.perf.wheel");
constexpr std::size_t bend = index_of("prism.perf.bend");
constexpr std::size_t pressure = index_of("prism.perf.pressure");
constexpr std::size_t fx_drive = index_of("prism.fx.drive");
constexpr std::size_t chorus = index_of("prism.fx.chorus");
constexpr std::size_t chorus_mix = index_of("prism.fx.chorus_mix");
constexpr std::size_t out_level = index_of("prism.out.level");
// LFO parameter blocks share a layout; LFO 2 starts lfo_stride after LFO 1.
constexpr std::size_t lfo_stride = index_of("prism.lfo2.shape") - lfo1_shape;
static_assert(index_of("prism.lfo2.rate") == lfo1_rate + lfo_stride);
static_assert(index_of("prism.lfo2.mode") == lfo1_mode + lfo_stride);
static_assert(index_of("prism.osc2.wave") - osc1_wave == osc2_wave - osc1_wave);
constexpr std::size_t osc_stride = osc2_wave - osc1_wave;
static_assert(osc2_phase == osc1_phase + osc_stride);
constexpr std::size_t mod_stride = 3;
static_assert(index_of("prism.mod4.amount") == mod1_source + 3 * mod_stride + 2);
} // namespace p

enum Source : std::size_t {
  src_off,
  src_env2,
  src_lfo1,
  src_lfo2,
  src_velocity,
  src_wheel,
  src_pressure,
  src_key,
  src_bend,
  src_amp,
  source_count
};
enum Target : std::size_t {
  dst_off,
  dst_pitch,
  dst_osc1_pitch,
  dst_osc2_pitch,
  dst_pw,
  dst_mix,
  dst_fm,
  dst_cutoff,
  dst_resonance,
  dst_drive,
  dst_level,
  dst_pan,
  dst_lfo1_rate,
  dst_lfo2_rate,
  dst_noise,
  target_count
};
static_assert(source_count == prism_table::mod_sources.size());
static_assert(target_count == prism_table::mod_targets.size());

constexpr double max_cutoff_hz = 20'000.0;
constexpr double min_cutoff_hz = 20.0;
constexpr double voice_gain = 0.4; // Headroom for stacked voices; the output knee catches the rest.

[[nodiscard]] int as_int(double value) noexcept { return static_cast<int>(value); }
[[nodiscard]] double clamp(double value, double low, double high) noexcept {
  return value < low ? low : (value > high ? high : value);
}
using native_dsp::dsp::db_to_gain;

// Settings that only mean something at note start (where an oscillator's
// phase begins, whether an LFO is per voice); changing them applies to the
// next note. Everything structural switches live (ADR-0038).
struct Topology {
  bool random_phase1 = true;
  bool random_phase2 = true;
  bool lfo1_poly = true;
  bool lfo2_poly = false;
  bool operator==(const Topology&) const = default;
};

[[nodiscard]] Topology topology_of(std::span<const double> v) noexcept {
  Topology t;
  t.random_phase1 = v[p::osc1_phase] == 0.0;
  t.random_phase2 = v[p::osc2_phase] == 0.0;
  t.lfo1_poly = v[p::lfo1_mode] == 0.0;
  t.lfo2_poly = v[p::lfo1_mode + p::lfo_stride] == 0.0;
  return t;
}

[[nodiscard]] std::size_t unison_of(double value) noexcept {
  return static_cast<std::size_t>(value);
}
[[nodiscard]] int sub_divider_of(double octave) noexcept { return octave == 0.0 ? 2 : 4; }

// Per-voice ramps between control ticks, all advanced once per frame.
using Ramp = dsp::Ramp;

struct LfoState {
  Lfo lfo;
  double value = 0.0;
  double fade_age = 0.0; // Seconds since the fade began.
};

// The structure-dependent part of the oscillator section: unison counts, the
// sub octave, and everything sized or normalised by them.
struct Sources {
  std::size_t unison1 = 1;
  std::size_t unison2 = 1;
  int sub_divider = 2;
  std::array<BlepOscillator, unison_limit> osc1{};
  std::array<BlepOscillator, unison_limit> osc2{};
  BlepOscillator sub{};
  std::array<Ramp, unison_limit> increment1{};
  std::array<Ramp, unison_limit> increment2{};
  std::array<double, unison_limit> left1{};
  std::array<double, unison_limit> right1{};
  std::array<double, unison_limit> left2{};
  std::array<double, unison_limit> right2{};
  Ramp sub_increment, level1, level2, level_ring;
};

struct Voice {
  bool active = false;
  bool held = false;
  NoteEvent note{};
  std::uint8_t key = 60;
  double velocity = 1.0;
  std::uint64_t age = 0;
  Topology topology{};

  double pitch = 60.0; // Semitones, gliding toward glide_target.
  double glide_target = 60.0;
  double glide_rate = 0.0; // Semitones per tick; zero is no glide.

  // The oscillator section, and a warm copy of it in its previous structure
  // while a unison or sub-octave change crossfades (ADR-0038).
  Sources sources{};
  Sources fading_sources{};
  SwitchFade source_fade{};
  bool fresh_sources = false; // The new section's ramps jump on the next tick.
  NoiseSource noise{};
  Ramp pw1, pw2, level_sub, level_noise, fm;
  Ramp g, k1, k2, feedback, drive, makeup, gain, pan;

  Adsr amp{};
  Adsr env2{};
  std::array<LfoState, 2> lfo{};
  StereoUpsampler2x up{};
  StereoDownsampler2x down{};
  FilterMode filter_mode = FilterMode::lowpass_24;
  StereoMultimodeFilter filter{};
  // A mode change runs the old mode from the same state and crossfades.
  StereoMultimodeFilter fading_filter{};
  FilterCoefficients fading_coefficients{};
  SwitchFade filter_fade{};
  double last_left = 0.0;
  double last_right = 0.0;
};

class PrismKernel final : public SynthKernel {
public:
  explicit PrismKernel(double sample_rate)
      : rate_(sample_rate), control_rate_(sample_rate / control_interval_frames) {
    voices_ = std::make_unique<std::array<Voice, voice_limit>>();
    for (auto& voice : *voices_) {
      voice.amp.prepare(rate_);
      voice.env2.prepare(rate_);
      voice.source_fade.prepare(rate_);
      voice.filter_fade.prepare(rate_);
      for (auto& state : voice.lfo) {
        state.lfo.prepare(control_rate_);
      }
    }
    for (auto& state : global_lfo_) {
      state.lfo.prepare(control_rate_);
    }
    chorus_.prepare(rate_);
    dc_[0].prepare(rate_);
    dc_[1].prepare(rate_);
    reset_state();
  }

  void control(std::span<const double> values, std::uint32_t frames_to_next_tick,
               const SynthTransport& transport) noexcept override {
    values_ = values;
    frames_to_tick_ = frames_to_next_tick;
    beats_per_second_ = transport.beats_per_frame > 0.0 ? transport.beats_per_frame * rate_ : 2.0;
    transport_ = transport;
    tempo_known_ = transport.beats_per_frame > 0.0;
    topology_ = topology_of(values);
    voice_mode_ = as_int(values[p::voice_mode]);
    advance_global_lfos();

    AdsrSettings amp{values[p::amp_attack], values[p::amp_decay], values[p::amp_sustain],
                     values[p::amp_release], values[p::amp_curve]};
    AdsrSettings env2{values[p::env2_attack], values[p::env2_decay], values[p::env2_sustain],
                      values[p::env2_release], values[p::env2_curve]};
    for (auto& voice : *voices_) {
      if (voice.active) {
        voice.amp.set(amp);
        voice.env2.set(env2);
        control_voice(voice, false);
      }
    }
    chorus_.set(static_cast<ChorusMode>(as_int(values[p::chorus])), values[p::chorus_mix]);
    const double drive = db_to_gain(values[p::fx_drive]);
    post_drive_.to(drive, frames_to_tick_, !primed_);
    post_makeup_.to((1.0 + std::log(drive)) / drive, frames_to_tick_, !primed_);
    const double level = values[p::out_level] <= -59.9 ? 0.0 : db_to_gain(values[p::out_level]);
    out_gain_.to(level, frames_to_tick_, !primed_);
    primed_ = true;
  }

  void note_on(const NoteEvent& event) noexcept override {
    if (values_.empty()) {
      return;
    }
    const bool mono = voice_mode_ != 0;
    if (mono) {
      mono_note_on(event);
      return;
    }
    if (mono_active_) {
      // Leaving mono: the held mono note continues as an ordinary voice.
      mono_active_ = false;
      mono_stack_size_ = 0;
    }
    Voice& voice = allocate(event.key);
    start_voice(voice, event, last_pitch_, true);
    last_pitch_ = event.key;
  }

  void note_off(const NoteEvent& event) noexcept override {
    if (mono_active_) {
      mono_note_off(event);
    }
    // Ordinary voices, including poly notes still held after a mode change.
    for (std::size_t i = mono_active_ ? 1 : 0; i < voices_->size(); ++i) {
      auto& voice = (*voices_)[i];
      if (voice.active && voice.held && voice.note.clip == event.clip &&
          voice.note.note == event.note) {
        voice.held = false;
        voice.amp.gate_off();
        voice.env2.gate_off();
      }
    }
  }

  void render(std::span<engine::StereoFrame> output) noexcept override {
    const std::size_t frames = output.size();
    Chunk left{};
    Chunk right{};
    for (auto& voice : *voices_) {
      if (voice.active) {
        render_voice(voice, left, right, frames);
      }
    }
    for (std::size_t i = 0; i < frames; ++i) {
      double l = left[i];
      double r = right[i];
      declick_.add(l, r);
      // Post drive, 2x oversampled.
      const double drive = post_drive_.next();
      const double makeup = post_makeup_.next();
      const StereoSample driven = oversampled_drive({l, r}, drive, makeup);
      const auto wet = chorus_.process(driven.left, driven.right);
      const double gain = out_gain_.next();
      l = safety(dc_[0].process(wet.left) * gain);
      r = safety(dc_[1].process(wet.right) * gain);
      output[i].left += static_cast<float>(l);
      output[i].right += static_cast<float>(r);
    }
    frames_to_tick_ -= static_cast<std::uint32_t>(std::min<std::size_t>(frames, frames_to_tick_));
  }

  void reset() noexcept override {
    for (const auto& voice : *voices_) {
      if (voice.active) {
        capture_declick(voice);
      }
    }
    reset_state();
  }

  [[nodiscard]] KernelStatus status() const noexcept override {
    KernelStatus status;
    status.voice_steals = steals_;
    for (const auto& voice : *voices_) {
      if (voice.active) {
        ++status.active_voices;
        status.deferred_changes = status.deferred_changes || voice.topology != topology_;
      }
    }
    return status;
  }

private:
  void reset_state() noexcept {
    for (auto& voice : *voices_) {
      voice.active = false;
      voice.held = false;
      voice.source_fade.stop();
      voice.filter_fade.stop();
    }
    for (std::size_t i = 0; i < global_lfo_.size(); ++i) {
      global_lfo_[i].lfo.reset(0.0, static_cast<std::uint32_t>(101 + i));
      global_lfo_[i].fade_age = 1e9;
    }
    phase_random_.reset(0x5EED1234U);
    mono_active_ = false;
    mono_stack_size_ = 0;
    last_pitch_ = -1.0;
    clock_ = 0;
    // The post chain (drive, chorus, DC) keeps its state across a seek so
    // its short tail and the voice declick ring out without a step.
  }

  // ---- Allocation ------------------------------------------------------------

  Voice& allocate(std::uint8_t key) noexcept {
    // A key already ringing is retriggered in place, like hardware polys.
    Voice* chosen = nullptr;
    for (auto& voice : *voices_) {
      if (voice.active && !voice.held && voice.key == key) {
        chosen = &voice;
        break;
      }
    }
    const bool same_key = chosen != nullptr;
    if (chosen == nullptr) {
      for (auto& voice : *voices_) {
        if (!voice.active) {
          return voice;
        }
      }
      // Steal the oldest released voice, else the oldest held one.
      for (auto& voice : *voices_) {
        if (!voice.held && (chosen == nullptr || voice.age < chosen->age)) {
          chosen = &voice;
        }
      }
      if (chosen == nullptr) {
        for (auto& voice : *voices_) {
          if (chosen == nullptr || voice.age < chosen->age) {
            chosen = &voice;
          }
        }
      }
    }
    capture_declick(*chosen);
    if (!same_key) {
      ++steals_;
    }
    chosen->active = false;
    return *chosen;
  }

  void capture_declick(const Voice& voice) noexcept {
    declick_.capture(voice.last_left, voice.last_right);
  }

  void start_voice(Voice& voice, const NoteEvent& event, double from_pitch,
                   bool retrigger) noexcept {
    const auto v = values_;
    const bool continuing = voice.active;
    const bool others_held = held_voices(&voice) > 0;
    voice.active = true;
    voice.held = true;
    voice.note = event;
    voice.key = event.key;
    voice.velocity = event.velocity / 127.0;
    voice.age = ++clock_;
    voice.glide_target = event.key;

    const double glide_time = v[p::glide];
    const bool legato_only = v[p::glide_when] != 0.0;
    const bool may_glide =
        glide_time > 0.0 && from_pitch >= 0.0 && (!legato_only || others_held || continuing);
    if (!continuing || !may_glide) {
      voice.pitch = may_glide ? from_pitch : event.key;
    }
    if (may_glide) {
      const double ticks = std::max(1.0, glide_time * control_rate_);
      const double distance = std::fabs(voice.glide_target - voice.pitch);
      // Time: any interval takes the glide time. Rate: an octave does.
      voice.glide_rate = (v[p::glide_mode] == 0.0 ? distance : 12.0) / ticks;
    } else {
      voice.pitch = event.key;
      voice.glide_rate = 0.0;
    }

    if (!continuing) {
      voice.topology = topology_;
      auto& s = voice.sources;
      s.unison1 = unison_of(v[p::osc1_unison]);
      s.unison2 = unison_of(v[p::osc2_unison]);
      s.sub_divider = sub_divider_of(v[p::sub_octave]);
      reset_lanes(voice, s, 0, 0);
      s.sub.reset(0.0, v[p::sub_wave] == 0.0 ? Waveform::sine : Waveform::pulse, 0.5);
      voice.source_fade.stop();
      voice.fresh_sources = false;
      voice.noise.reset(static_cast<std::uint32_t>(voice.age * 2654435761U + 1U));
      voice.filter_mode = static_cast<FilterMode>(as_int(v[p::filter_mode]));
      voice.filter.reset();
      voice.filter_fade.stop();
      voice.up.reset();
      voice.down.reset();
      voice.amp.reset();
      voice.env2.reset();
      voice.last_left = 0.0;
      voice.last_right = 0.0;
    }
    const AdsrSettings amp{v[p::amp_attack], v[p::amp_decay], v[p::amp_sustain], v[p::amp_release],
                           v[p::amp_curve]};
    const AdsrSettings env2{v[p::env2_attack], v[p::env2_decay], v[p::env2_sustain],
                            v[p::env2_release], v[p::env2_curve]};
    voice.amp.set(amp);
    voice.env2.set(env2);
    if (retrigger || !continuing) {
      voice.amp.gate_on();
      voice.env2.gate_on();
      for (std::size_t i = 0; i < 2; ++i) {
        const std::size_t base = i * p::lfo_stride;
        if (v[p::lfo1_retrigger + base] != 0.0) {
          voice.lfo[i].lfo.reset(0.0, static_cast<std::uint32_t>(voice.age + i));
        } else if (!continuing) {
          voice.lfo[i].lfo.reset(global_lfo_[i].lfo.phase(),
                                 static_cast<std::uint32_t>(voice.age + i));
        }
        voice.lfo[i].fade_age = 0.0;
        voice.lfo[i].value = 0.0;
      }
      if (!others_held) {
        for (std::size_t i = 0; i < 2; ++i) {
          if (v[p::lfo1_retrigger + i * p::lfo_stride] != 0.0) {
            global_lfo_[i].lfo.reset(0.0, static_cast<std::uint32_t>(101 + i));
          }
          global_lfo_[i].fade_age = 0.0;
        }
      }
    }
    control_voice(voice, !continuing);
  }

  // Starts the oscillators of lanes from `first1` and `first2` up, with the
  // note-start phase policy; lanes below them keep running.
  void reset_lanes(const Voice& voice, Sources& s, std::size_t first1,
                   std::size_t first2) noexcept {
    const auto v = values_;
    const auto& t = voice.topology;
    const auto wave1 = static_cast<Waveform>(as_int(v[p::osc1_wave]));
    const auto wave2 = static_cast<Waveform>(as_int(v[p::osc2_wave]));
    for (std::size_t u = first1; u < s.unison1; ++u) {
      s.osc1[u].reset(t.random_phase1 ? phase_random_.white() * 0.5 + 0.5 : 0.0, wave1,
                      v[p::osc1_pw]);
    }
    for (std::size_t u = first2; u < s.unison2; ++u) {
      s.osc2[u].reset(t.random_phase2 ? phase_random_.white() * 0.5 + 0.5 : 0.0, wave2,
                      v[p::osc2_pw]);
    }
  }

  // A unison or sub-octave change: the old section keeps sounding as a warm
  // copy and crossfades out. Lanes both structures share keep their phase,
  // so the two are strongly correlated; added lanes start fresh. A change
  // arriving during a fade waits for it to finish.
  void switch_sources(Voice& voice, std::size_t unison1, std::size_t unison2,
                      int sub_divider) noexcept {
    voice.fading_sources = voice.sources;
    auto& s = voice.sources;
    const std::size_t kept1 = std::min(s.unison1, unison1);
    const std::size_t kept2 = std::min(s.unison2, unison2);
    s.unison1 = unison1;
    s.unison2 = unison2;
    s.sub_divider = sub_divider;
    reset_lanes(voice, s, kept1, kept2);
    voice.fresh_sources = true;
    voice.source_fade.start();
  }

  [[nodiscard]] std::uint32_t held_voices(const Voice* except) const noexcept {
    std::uint32_t count = 0;
    for (const auto& voice : *voices_) {
      count += voice.active && voice.held && &voice != except ? 1U : 0U;
    }
    return count;
  }

  // ---- Mono and legato -------------------------------------------------------

  void mono_note_on(const NoteEvent& event) noexcept {
    Voice& voice = (*voices_)[0];
    // Leaving poly: any other ringing voices release naturally.
    if (mono_stack_size_ < mono_stack_.size()) {
      mono_stack_[mono_stack_size_++] = event;
    }
    const bool legato = mono_active_ && voice.active && voice.held;
    if (!legato && voice.active && !mono_active_) {
      capture_declick(voice);
      voice.active = false;
    }
    mono_active_ = true;
    const bool retrigger = !legato || voice_mode_ == 1;
    start_voice(voice, event, voice.active ? voice.pitch : last_pitch_, retrigger);
    last_pitch_ = event.key;
  }

  void mono_note_off(const NoteEvent& event) noexcept {
    std::size_t kept = 0;
    for (std::size_t i = 0; i < mono_stack_size_; ++i) {
      if (!(mono_stack_[i].clip == event.clip && mono_stack_[i].note == event.note)) {
        mono_stack_[kept++] = mono_stack_[i];
      }
    }
    mono_stack_size_ = kept;
    Voice& voice = (*voices_)[0];
    if (!(voice.active && voice.note.clip == event.clip && voice.note.note == event.note)) {
      return;
    }
    if (mono_stack_size_ > 0) {
      // Return to the most recent still-held note, legato.
      start_voice(voice, mono_stack_[mono_stack_size_ - 1], voice.pitch, voice_mode_ == 1);
      return;
    }
    voice.held = false;
    voice.amp.gate_off();
    voice.env2.gate_off();
    mono_active_ = false;
  }

  // ---- Modulation ------------------------------------------------------------

  [[nodiscard]] double lfo_increment(std::size_t lfo, double rate_scale) const noexcept {
    const std::size_t base = lfo * p::lfo_stride;
    double hz = values_[p::lfo1_rate + base];
    if (values_[p::lfo1_sync + base] != 0.0) {
      const auto division = static_cast<std::size_t>(values_[p::lfo1_division + base]);
      hz = sync_hz(division, transport_, rate_);
    }
    return hz * rate_scale / control_rate_;
  }

  void advance_global_lfos() noexcept {
    for (std::size_t i = 0; i < 2; ++i) {
      auto& state = global_lfo_[i];
      state.lfo.set_shape(
          static_cast<LfoShape>(as_int(values_[p::lfo1_shape + i * p::lfo_stride])));
      const std::size_t base = i * p::lfo_stride;
      if (values_[p::lfo1_sync + base] != 0.0 && values_[p::lfo1_retrigger + base] == 0.0 &&
          tempo_known_) {
        // Free-running synced LFOs lock to the song position.
        const auto division = static_cast<std::size_t>(values_[p::lfo1_division + base]);
        state.value = state.lfo.next_at(sync_cycles(division, transport_));
      } else {
        state.value = state.lfo.next(lfo_increment(i, 1.0));
      }
      state.fade_age += 1.0 / control_rate_;
    }
  }

  [[nodiscard]] double faded(const LfoState& state, std::size_t lfo) const noexcept {
    const double fade = values_[p::lfo1_fade + lfo * p::lfo_stride];
    return fade <= 0.0 ? state.value : state.value * std::min(1.0, state.fade_age / fade);
  }

  void control_voice(Voice& voice, bool jump) noexcept {
    const auto v = values_;
    const auto& t = voice.topology;
    const std::uint32_t frames = frames_to_tick_;

    // Glide one tick toward the target.
    if (voice.glide_rate > 0.0) {
      const double distance = voice.glide_target - voice.pitch;
      if (std::fabs(distance) <= voice.glide_rate) {
        voice.pitch = voice.glide_target;
        voice.glide_rate = 0.0;
      } else {
        voice.pitch += distance > 0.0 ? voice.glide_rate : -voice.glide_rate;
      }
    }

    // Sources.
    std::array<double, source_count> source{};
    for (std::size_t i = 0; i < 2; ++i) {
      auto& state = voice.lfo[i];
      const bool poly = i == 0 ? t.lfo1_poly : t.lfo2_poly;
      if (poly) {
        state.lfo.set_shape(static_cast<LfoShape>(as_int(v[p::lfo1_shape + i * p::lfo_stride])));
        state.value = state.lfo.next(lfo_increment(i, rate_mod_[i]));
        state.fade_age += 1.0 / control_rate_;
        source[src_lfo1 + i] = faded(state, i);
      } else {
        source[src_lfo1 + i] = faded(global_lfo_[i], i);
      }
    }
    source[src_env2] = voice.env2.value();
    source[src_velocity] = voice.velocity;
    source[src_wheel] = v[p::wheel];
    source[src_pressure] = v[p::pressure];
    source[src_key] = (voice.key - 60.0) / 60.0;
    source[src_bend] = v[p::bend];
    source[src_amp] = voice.amp.value();

    std::array<double, target_count> mod{};
    for (std::size_t slot = 0; slot < 4; ++slot) {
      const std::size_t base = p::mod1_source + slot * p::mod_stride;
      const auto from = static_cast<std::size_t>(v[base]);
      const auto to = static_cast<std::size_t>(v[base + 1]);
      if (from != src_off && to != dst_off) {
        mod[to] += v[base + 2] * source[from];
      }
    }
    rate_mod_[0] = std::exp2(4.0 * mod[dst_lfo1_rate]);
    rate_mod_[1] = std::exp2(4.0 * mod[dst_lfo2_rate]);

    // Pitch.
    const double bend_st = v[p::bend] * (v[p::bend] >= 0.0 ? v[p::bend_up] : v[p::bend_down]);
    const double vibrato_ct =
        source[src_lfo1] * (v[p::lfo1_vibrato] + v[p::wheel] * v[p::lfo1_wheel]);
    const double common = voice.pitch + bend_st + vibrato_ct / 100.0 + 24.0 * mod[dst_pitch];
    const double pitch1 = common + 12.0 * v[p::osc1_octave] + v[p::osc1_semi] +
                          v[p::osc1_fine] / 100.0 + 24.0 * mod[dst_osc1_pitch];
    const double pitch2 = common + 12.0 * v[p::osc2_octave] + v[p::osc2_semi] +
                          v[p::osc2_fine] / 100.0 + 24.0 * mod[dst_osc2_pitch];
    const double lfo_pw = source[src_lfo2] * v[p::lfo2_pw] + 0.45 * mod[dst_pw];
    const double pw1 = clamp(v[p::osc1_pw] + lfo_pw, 0.05, 0.95);
    const double pw2 = clamp(v[p::osc2_pw] + lfo_pw, 0.05, 0.95);
    const double mix = clamp(mod[dst_mix], -1.0, 1.0);
    const SourceTargets targets{pitch1,
                                pitch2,
                                pw1,
                                pw2,
                                v[p::osc1_level] * (1.0 - std::max(mix, 0.0)),
                                v[p::osc2_level] * (1.0 + std::min(mix, 0.0))};

    // Unison counts and the sub octave switch live (ADR-0038).
    const std::size_t unison1 = unison_of(v[p::osc1_unison]);
    const std::size_t unison2 = unison_of(v[p::osc2_unison]);
    const int sub_divider = sub_divider_of(v[p::sub_octave]);
    auto& s = voice.sources;
    if (!jump && !voice.source_fade.active() &&
        (unison1 != s.unison1 || unison2 != s.unison2 || sub_divider != s.sub_divider)) {
      switch_sources(voice, unison1, unison2, sub_divider);
    }
    control_sources(s, targets, frames, jump || voice.fresh_sources);
    voice.fresh_sources = false;
    if (voice.source_fade.active()) {
      control_sources(voice.fading_sources, targets, frames, false);
    }
    voice.pw1.to(pw1, frames, jump);
    voice.pw2.to(pw2, frames, jump);

    // Mixer.
    voice.level_sub.to(v[p::sub_level], frames, jump);
    voice.level_noise.to(clamp(v[p::noise_level] + mod[dst_noise], 0.0, 1.0) * 0.5, frames, jump);
    voice.fm.to(clamp(v[p::osc2_fm] + mod[dst_fm], 0.0, 1.0), frames, jump);

    // Filter: cutoff in semitones above the knob, then to the oversampled rate.
    const double semitones = source[src_env2] * v[p::filter_env] +
                             v[p::keytrack] * (voice.key - 60.0) +
                             48.0 * v[p::filter_velocity] * voice.velocity +
                             source[src_lfo2] * v[p::lfo2_cutoff] + 96.0 * mod[dst_cutoff];
    const double cutoff_hz =
        clamp(v[p::cutoff] * std::exp2(semitones / 12.0), min_cutoff_hz, max_cutoff_hz);
    const double drive_db = clamp(v[p::filter_drive] + 24.0 * mod[dst_drive], 0.0, 24.0);
    const double resonance = clamp(v[p::resonance] + mod[dst_resonance], 0.0, 1.0);
    // The mode switches live: the old mode runs on from the same state and
    // crossfades out (ADR-0038).
    const auto mode = static_cast<FilterMode>(as_int(v[p::filter_mode]));
    if (!jump && !voice.filter_fade.active() && mode != voice.filter_mode) {
      voice.fading_filter = voice.filter;
      voice.fading_coefficients.mode = voice.filter_mode;
      voice.filter_mode = mode;
      voice.filter_fade.start();
    }
    if (voice.filter_fade.active()) {
      voice.fading_coefficients =
          make_filter_coefficients(voice.fading_coefficients.mode, cutoff_hz / (2.0 * rate_),
                                   resonance, db_to_gain(drive_db));
    }
    const auto c = make_filter_coefficients(voice.filter_mode, cutoff_hz / (2.0 * rate_), resonance,
                                            db_to_gain(drive_db));
    voice.g.to(c.g, frames, jump);
    voice.k1.to(c.k1, frames, jump);
    voice.k2.to(c.k2, frames, jump);
    voice.feedback.to(c.feedback, frames, jump);
    voice.drive.to(c.drive_gain, frames, jump);
    voice.makeup.to(c.drive_makeup, frames, jump);

    // Amplitude.
    const double sensitivity = v[p::amp_velocity];
    const double velocity_gain = 1.0 - sensitivity + sensitivity * voice.velocity * voice.velocity;
    voice.gain.to(voice_gain * velocity_gain * clamp(1.0 + mod[dst_level], 0.0, 2.0), frames, jump);
    voice.pan.to(clamp(mod[dst_pan], -1.0, 1.0), frames, jump);
  }

  [[nodiscard]] static double hz(double semitones) noexcept {
    return 440.0 * std::exp2((semitones - 69.0) / 12.0);
  }

  void set_unison(std::array<Ramp, unison_limit>& increments,
                  std::array<double, unison_limit>& left, std::array<double, unison_limit>& right,
                  std::size_t count, double pitch, double detune, double width,
                  std::uint32_t frames, bool jump) const noexcept {
    for (std::size_t u = 0; u < count; ++u) {
      const double position =
          count == 1 ? 0.0 : -1.0 + 2.0 * static_cast<double>(u) / static_cast<double>(count - 1);
      const double cents = detune * position;
      increments[u].to(hz(pitch + cents / 100.0) / rate_, frames, jump);
      // Alternate sides so adjacent detune steps spread wide.
      const double pan = width * position * (u % 2 == 0 ? 1.0 : -1.0);
      left[u] = std::cos((pan + 1.0) * std::numbers::pi / 4.0);
      right[u] = std::sin((pan + 1.0) * std::numbers::pi / 4.0);
    }
  }

  struct SourceTargets {
    double pitch1;
    double pitch2;
    double pw1;
    double pw2;
    double level1; // Before unison normalisation, which is per structure.
    double level2;
  };

  void control_sources(Sources& s, const SourceTargets& targets, std::uint32_t frames,
                       bool jump) const noexcept {
    const auto v = values_;
    set_unison(s.increment1, s.left1, s.right1, s.unison1, targets.pitch1, v[p::osc1_detune],
               v[p::osc1_width], frames, jump);
    set_unison(s.increment2, s.left2, s.right2, s.unison2, targets.pitch2, v[p::osc2_detune],
               v[p::osc2_width], frames, jump);
    s.sub_increment.to(hz(targets.pitch1) / rate_ / static_cast<double>(s.sub_divider), frames,
                       jump);

    // Shapes switch live; the oscillators band-limit the change.
    const auto wave1 = static_cast<Waveform>(as_int(v[p::osc1_wave]));
    const auto wave2 = static_cast<Waveform>(as_int(v[p::osc2_wave]));
    for (std::size_t u = 0; u < s.unison1; ++u) {
      s.osc1[u].set_waveform(wave1, targets.pw1);
    }
    for (std::size_t u = 0; u < s.unison2; ++u) {
      s.osc2[u].set_waveform(wave2, targets.pw2);
    }
    s.sub.set_waveform(v[p::sub_wave] == 0.0 ? Waveform::sine : Waveform::pulse, 0.5);

    s.level1.to(targets.level1 / std::sqrt(static_cast<double>(s.unison1)), frames, jump);
    s.level2.to(targets.level2 / std::sqrt(static_cast<double>(s.unison2)), frames, jump);
    s.level_ring.to(v[p::ring_level] / std::sqrt(static_cast<double>(s.unison2)), frames, jump);
  }

  // ---- Rendering -------------------------------------------------------------

  using Chunk = std::array<double, control_interval_frames>;

  struct SourceChunks {
    Chunk pw1, pw2, fm, sub_level;
    bool sync, use_fm, use_ring, use_sub;
  };

  // The oscillators of one section into in_l/in_r, stage by stage.
  static void render_sources(Sources& s, const SourceChunks& c, Chunk& in_l, Chunk& in_r,
                             std::size_t frames) noexcept {
    std::array<Chunk, unison_limit> out1;
    std::array<std::array<SyncPoint, control_interval_frames>, unison_limit> wraps;

    Chunk level;
    s.level1.fill(level.data(), frames);
    for (std::size_t u = 0; u < s.unison1; ++u) {
      auto& osc = s.osc1[u];
      auto& increment = s.increment1[u];
      auto& out = out1[u];
      auto& wrap = wraps[u];
      const double pan_l = s.left1[u];
      const double pan_r = s.right1[u];
      // Locals keep the ramp out of memory the oscillator might alias.
      double step = increment.value;
      const double slope = increment.step;
      if (c.sync) {
        for (std::size_t i = 0; i < frames; ++i) {
          step += slope;
          out[i] = osc.next(step, c.pw1[i], {}, &wrap[i]);
        }
      } else {
        for (std::size_t i = 0; i < frames; ++i) {
          step += slope;
          out[i] = osc.next(step, c.pw1[i], {}, nullptr);
        }
      }
      increment.value = step;
      for (std::size_t i = 0; i < frames; ++i) {
        in_l[i] += out[i] * pan_l * level[i];
        in_r[i] += out[i] * pan_r * level[i];
      }
    }
    Chunk ring_level;
    s.level2.fill(level.data(), frames);
    s.level_ring.fill(ring_level.data(), frames);
    for (std::size_t u = 0; u < s.unison2; ++u) {
      auto& osc = s.osc2[u];
      auto& increment = s.increment2[u];
      const std::size_t master = u % s.unison1;
      const auto& modulator = out1[master];
      const auto& wrap = wraps[master];
      const double pan_l = s.left2[u];
      const double pan_r = s.right2[u];
      Chunk out;
      double step = increment.value;
      const double slope = increment.step;
      if (c.sync || c.use_fm) {
        for (std::size_t i = 0; i < frames; ++i) {
          step += slope;
          out[i] = osc.next(c.use_fm ? step * (1.0 + c.fm[i] * modulator[i]) : step, c.pw2[i],
                            c.sync ? wrap[i] : SyncPoint{}, nullptr);
        }
      } else {
        for (std::size_t i = 0; i < frames; ++i) {
          step += slope;
          out[i] = osc.next(step, c.pw2[i], {}, nullptr);
        }
      }
      increment.value = step;
      for (std::size_t i = 0; i < frames; ++i) {
        const double mixed =
            out[i] * (level[i] + (c.use_ring ? ring_level[i] * modulator[i] : 0.0));
        in_l[i] += mixed * pan_l;
        in_r[i] += mixed * pan_r;
      }
    }
    if (c.use_sub) {
      for (std::size_t i = 0; i < frames; ++i) {
        const double value = s.sub.next(s.sub_increment.next(), 0.5, {}, nullptr) * c.sub_level[i];
        in_l[i] += value;
        in_r[i] += value;
      }
    } else {
      s.sub_increment.skip(frames);
    }
  }

  // Stage by stage over a chunk of at most one control interval: each
  // oscillator and filter runs a tight loop with its state hot, instead of
  // interleaving every stage per frame. Output is identical either way.
  void render_voice(Voice& voice, Chunk& left, Chunk& right, std::size_t frames) noexcept {
    // Silent sources are skipped; the decision is per control interval, so it
    // never depends on how the host splits blocks.
    SourceChunks c;
    c.sync = values_[p::osc2_sync] != 0.0;
    c.use_sub = !voice.level_sub.silent();
    c.use_ring = !voice.sources.level_ring.silent() ||
                 (voice.source_fade.active() && !voice.fading_sources.level_ring.silent());
    c.use_fm = !voice.fm.silent();
    const bool use_noise = !voice.level_noise.silent();

    voice.pw1.fill(c.pw1.data(), frames);
    voice.pw2.fill(c.pw2.data(), frames);
    voice.fm.fill(c.fm.data(), frames);
    if (c.use_sub) {
      voice.level_sub.fill(c.sub_level.data(), frames);
    } else {
      voice.level_sub.skip(frames);
    }
    Chunk in_l{};
    Chunk in_r{};
    render_sources(voice.sources, c, in_l, in_r, frames);
    if (voice.source_fade.active()) {
      Chunk old_l{};
      Chunk old_r{};
      render_sources(voice.fading_sources, c, old_l, old_r, frames);
      for (std::size_t i = 0; i < frames; ++i) {
        const double weight = voice.source_fade.next();
        in_l[i] += (old_l[i] - in_l[i]) * weight;
        in_r[i] += (old_r[i] - in_r[i]) * weight;
      }
    }
    if (use_noise) {
      const auto colour = static_cast<NoiseColour>(as_int(values_[p::noise_colour]));
      for (std::size_t i = 0; i < frames; ++i) {
        const double value = voice.noise.next(colour) * voice.level_noise.next();
        in_l[i] += value;
        in_r[i] += value;
      }
    } else {
      voice.level_noise.skip(frames);
    }

    std::array<FilterCoefficients, control_interval_frames> coefficients;
    for (std::size_t i = 0; i < frames; ++i) {
      auto& k = coefficients[i];
      k.mode = voice.filter_mode;
      k.g = voice.g.next();
      k.k1 = voice.k1.next();
      k.k2 = voice.k2.next();
      k.feedback = voice.feedback.next();
      k.drive_gain = voice.drive.next();
      k.drive_makeup = voice.makeup.next();
      k.finalise();
    }
    for (std::size_t i = 0; i < frames; ++i) {
      StereoSample a;
      StereoSample b;
      voice.up.process({in_l[i], in_r[i]}, a, b);
      const StereoSample x_a = a;
      const StereoSample x_b = b;
      a = voice.filter.process(a, coefficients[i]);
      b = voice.filter.process(b, coefficients[i]);
      if (voice.filter_fade.active()) {
        // Both modes at the oversampled rate, then one downsampler.
        const StereoSample old_a = voice.fading_filter.process(x_a, voice.fading_coefficients);
        const StereoSample old_b = voice.fading_filter.process(x_b, voice.fading_coefficients);
        const double weight = voice.filter_fade.next();
        a = a + (old_a - a) * weight;
        b = b + (old_b - b) * weight;
      }
      const StereoSample y = voice.down.process(a, b);
      in_l[i] = y.left;
      in_r[i] = y.right;
    }

    for (std::size_t i = 0; i < frames; ++i) {
      const double gain = voice.amp.next() * voice.gain.next();
      (void)voice.env2.next();
      const double pan = voice.pan.next();
      const double pan_l = pan > 0.0 ? 1.0 - pan : 1.0;
      const double pan_r = pan < 0.0 ? 1.0 + pan : 1.0;
      voice.last_left = in_l[i] * gain * pan_l;
      voice.last_right = in_r[i] * gain * pan_r;
      left[i] += voice.last_left;
      right[i] += voice.last_right;
    }
    if (!voice.amp.active()) {
      voice.active = false;
      voice.held = false;
      voice.last_left = 0.0;
      voice.last_right = 0.0;
    }
  }

  // Always oversampled, so turning drive on or off never shifts phase.
  [[nodiscard]] StereoSample oversampled_drive(StereoSample x, double drive,
                                               double makeup) noexcept {
    StereoSample a;
    StereoSample b;
    post_up_.process(x, a, b);
    if (drive > 1.0 + 1e-9) {
      a = soft_clip(a * drive) * makeup;
      b = soft_clip(b * drive) * makeup;
    }
    return post_down_.process(a, b);
  }

  // Transparent below -1 dBFS, then a smooth knee that never exceeds 0 dBFS.
  [[nodiscard]] static double safety(double x) noexcept {
    constexpr double knee = 0.9;
    const double magnitude = std::fabs(x);
    if (magnitude <= knee) {
      return x;
    }
    const double over = (magnitude - knee) / (1.0 - knee);
    const double shaped = knee + (1.0 - knee) * soft_clip(over * 3.0);
    return x < 0.0 ? -shaped : shaped;
  }

  double rate_;
  double control_rate_;
  std::unique_ptr<std::array<Voice, voice_limit>> voices_;
  std::span<const double> values_{};
  Topology topology_{};
  int voice_mode_ = 0;
  std::uint32_t frames_to_tick_ = 0;
  double beats_per_second_ = 2.0;
  SynthTransport transport_{};
  bool tempo_known_ = false;
  std::array<LfoState, 2> global_lfo_{};
  std::array<double, 2> rate_mod_{1.0, 1.0};
  NoiseSource phase_random_{};
  bool mono_active_ = false;
  std::array<NoteEvent, 16> mono_stack_{};
  std::size_t mono_stack_size_ = 0;
  double last_pitch_ = -1.0;
  std::uint64_t clock_ = 0;
  std::uint32_t steals_ = 0;
  Declick declick_;
  StereoUpsampler2x post_up_{};
  StereoDownsampler2x post_down_{};
  Ramp post_drive_{1.0, 0.0};
  Ramp post_makeup_{1.0, 0.0};
  Ramp out_gain_{0.0, 0.0};
  bool primed_ = false;
  Chorus chorus_;
  std::array<DcBlocker, 2> dc_{};
};

std::unique_ptr<SynthKernel> create_kernel(double sample_rate) {
  return std::make_unique<PrismKernel>(sample_rate);
}

std::uint64_t tail_frames(std::span<const double> values, double sample_rate) {
  // A released note lasts its release; chorus, drive and DC filters add a
  // short tail. The oscillator latency delays everything slightly.
  const double seconds = values[p::amp_release] + Chorus::tail_seconds + 0.05;
  return static_cast<std::uint64_t>(std::ceil(seconds * sample_rate)) +
         BlepOscillator::latency_frames;
}

const EngineDescriptor descriptor{"prism", "Prism", 1, prism_table::parameters,
                                  prism_presets::presets};
const SynthEngine prism_engine{&descriptor, &create_kernel, &tail_frames};

} // namespace

const SynthEngine& engine() { return prism_engine; }

} // namespace oscillate::native_dsp::synth::prism
