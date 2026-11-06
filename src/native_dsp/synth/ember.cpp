#include "oscillate/native_dsp/synth/ember.hpp"

#include "dsp/ramp.hpp"
#include "ember_presets.hpp"
#include "ember_table.hpp"
#include "oscillate/native_dsp/synth/chorus.hpp"
#include "oscillate/native_dsp/synth/crossfade.hpp"
#include "oscillate/native_dsp/synth/drift.hpp"
#include "oscillate/native_dsp/synth/ember_circuit.hpp"
#include "oscillate/native_dsp/synth/modulation.hpp"
#include "oscillate/native_dsp/synth/oscillator.hpp"
#include "oscillate/native_dsp/synth/oversampler.hpp"
#include "oscillate/native_dsp/synth/shaping.hpp"
#include "oscillate/native_dsp/synth/tempo_sync.hpp"
#include "oscillate/native_dsp/synth/vintage_effects.hpp"
#include "oscillate/native_dsp/synth/vintage_filters.hpp"
#include "oscillate/native_dsp/synth/voice_pool.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace oscillate::native_dsp::synth::ember {
namespace {

using ember_table::index_of;

namespace p {
constexpr std::size_t dco_range = index_of("ember.dco.range");
constexpr std::size_t dco_saw = index_of("ember.dco.saw");
constexpr std::size_t dco_pulse = index_of("ember.dco.pulse");
constexpr std::size_t dco_sub = index_of("ember.dco.sub");
constexpr std::size_t dco_noise = index_of("ember.dco.noise");
constexpr std::size_t dco_pw = index_of("ember.dco.pw");
constexpr std::size_t pwm_source = index_of("ember.dco.pwm_source");
constexpr std::size_t dco_pwm = index_of("ember.dco.pwm");
constexpr std::size_t vco_wave = index_of("ember.vco.wave");
constexpr std::size_t vco_level = index_of("ember.vco.level");
constexpr std::size_t vco_semi = index_of("ember.vco.semi");
constexpr std::size_t vco_fine = index_of("ember.vco.fine");
constexpr std::size_t vco_pw = index_of("ember.vco.pw");
constexpr std::size_t vco_sync = index_of("ember.vco.sync");
constexpr std::size_t vco_low = index_of("ember.vco.low");
constexpr std::size_t pm_env = index_of("ember.pm.env");
constexpr std::size_t pm_osc = index_of("ember.pm.osc");
constexpr std::size_t pm_freq = index_of("ember.pm.freq");
constexpr std::size_t pm_pw = index_of("ember.pm.pw");
constexpr std::size_t pm_cutoff = index_of("ember.pm.cutoff");
constexpr std::size_t hpf = index_of("ember.hpf");
constexpr std::size_t filter_type = index_of("ember.filter.type");
constexpr std::size_t cutoff = index_of("ember.filter.cutoff");
constexpr std::size_t resonance = index_of("ember.filter.resonance");
constexpr std::size_t morph = index_of("ember.filter.morph");
constexpr std::size_t filter_env = index_of("ember.filter.env");
constexpr std::size_t filter_lfo = index_of("ember.filter.lfo");
constexpr std::size_t keytrack = index_of("ember.filter.keytrack");
constexpr std::size_t filter_velocity = index_of("ember.filter.velocity");
constexpr std::size_t filter_drive = index_of("ember.filter.drive");
constexpr std::size_t env_attack = index_of("ember.env.attack");
constexpr std::size_t env_decay = index_of("ember.env.decay");
constexpr std::size_t env_sustain = index_of("ember.env.sustain");
constexpr std::size_t env_release = index_of("ember.env.release");
constexpr std::size_t amp_mode = index_of("ember.amp.mode");
constexpr std::size_t amp_attack = index_of("ember.amp.attack");
constexpr std::size_t amp_decay = index_of("ember.amp.decay");
constexpr std::size_t amp_sustain = index_of("ember.amp.sustain");
constexpr std::size_t amp_release = index_of("ember.amp.release");
constexpr std::size_t amp_velocity = index_of("ember.amp.velocity");
constexpr std::size_t lfo_shape = index_of("ember.lfo.shape");
constexpr std::size_t lfo_rate = index_of("ember.lfo.rate");
constexpr std::size_t lfo_sync = index_of("ember.lfo.sync");
constexpr std::size_t lfo_division = index_of("ember.lfo.division");
constexpr std::size_t lfo_delay = index_of("ember.lfo.delay");
constexpr std::size_t lfo_pitch = index_of("ember.lfo.pitch");
constexpr std::size_t age = index_of("ember.age");
constexpr std::size_t tuning = index_of("ember.circuit.tuning");
constexpr std::size_t bleed = index_of("ember.circuit.bleed");
constexpr std::size_t hum = index_of("ember.circuit.hum");
constexpr std::size_t mains = index_of("ember.circuit.mains");
constexpr std::size_t unit = index_of("ember.circuit.unit");
constexpr std::size_t voice_mode = index_of("ember.voice.mode");
constexpr std::size_t chord = index_of("ember.voice.chord");
constexpr std::size_t detune = index_of("ember.voice.detune");
constexpr std::size_t glide = index_of("ember.voice.glide");
constexpr std::size_t bend_range = index_of("ember.voice.bend");
constexpr std::size_t wheel = index_of("ember.perf.wheel");
constexpr std::size_t bend = index_of("ember.perf.bend");
constexpr std::size_t pressure = index_of("ember.perf.pressure");
constexpr std::size_t ensemble = index_of("ember.fx.ensemble");
constexpr std::size_t chorus = index_of("ember.fx.chorus");
constexpr std::size_t hiss = index_of("ember.fx.hiss");
constexpr std::size_t delay = index_of("ember.fx.delay");
constexpr std::size_t delay_time = index_of("ember.fx.delay_time");
constexpr std::size_t delay_feedback = index_of("ember.fx.delay_feedback");
constexpr std::size_t delay_sync = index_of("ember.fx.delay_sync");
constexpr std::size_t delay_division = index_of("ember.fx.delay_division");
constexpr std::size_t delay_pingpong = index_of("ember.fx.delay_pingpong");
constexpr std::size_t tape = index_of("ember.fx.tape");
constexpr std::size_t wobble = index_of("ember.fx.wobble");
constexpr std::size_t out_level = index_of("ember.out.level");
} // namespace p

enum class FilterType : std::uint8_t { ota, sem };
enum class VoiceMode : std::uint8_t { poly, unison, chord };
enum class AmpMode : std::uint8_t { shared, gate, own };

// Intervals per chord shape; zero entries after the root are unused.
constexpr std::array<std::array<int, 4>, 7> chord_intervals{{
    {0, 4, 7, -1},  // Major
    {0, 3, 7, -1},  // Minor
    {0, 4, 7, 11},  // Maj 7
    {0, 3, 7, 10},  // Min 7
    {0, 5, 7, -1},  // Sus 4
    {0, 7, -1, -1}, // Fifths
    {0, 12, -1, -1} // Octaves
}};

constexpr double voice_gain =
    0.75; // After the filter; four saturating OTA stages compress about 2 dB.
constexpr double analog_curve = 0.75; // RC-like envelope segments.
// VCO B's fast jitter wanders this fast (Hz); the slow drift is pitch_drift.
constexpr double jitter_hz = 8.0;
constexpr std::array<double, 2> mains_hz{50.0, 60.0};

[[nodiscard]] int as_int(double value) noexcept { return static_cast<int>(value); }
[[nodiscard]] double clamp(double value, double low, double high) noexcept {
  return value < low ? low : (value > high ? high : value);
}
[[nodiscard]] double hz(double semitones) noexcept {
  return 440.0 * std::exp2((semitones - 69.0) / 12.0);
}

using Ramp = dsp::Ramp;

struct Voice {
  bool active = false;
  bool held = false;
  bool unison = false;
  NoteEvent note{};
  std::uint8_t key = 60;
  double velocity = 1.0;
  std::uint64_t age = 0;
  std::uint32_t slot = 0;
  FilterType filter = FilterType::ota;
  // A filter-type change on a sounding note: the old filter keeps running,
  // warm, and crossfades out while the new one starts from rest (ADR-0038).
  FilterType fading_filter = FilterType::ota;
  SwitchFade filter_fade{};
  double unison_position = 0.0; // -1..1 in unison, 0 otherwise.

  double pitch = 60.0;
  double glide_target = 60.0;
  double glide_rate = 0.0;
  double seconds = 0.0; // Since note start, for the LFO delay.

  BlepOscillator saw{};
  BlepOscillator pulse{};
  BlepOscillator sub{};
  BlepOscillator vco{};
  NoiseSource noise{};
  DriftSource pitch_drift{};
  DriftSource cutoff_drift{};
  DriftSource jitter{};
  ember_circuit::Card card{};
  double dco_level = 1.0;
  // For telemetry: how far each oscillator sits from its ideal pitch now.
  double vco_cents = 0.0;
  double dco_cents = 0.0;
  // After its envelope ends, the VCA's leak fades out over this many frames.
  bool leaking = false;
  std::uint32_t leak_remaining = 0;
  Ramp increment_a, increment_b, width_a, width_b, level_saw, level_pulse, level_sub, level_noise,
      level_vco, pm_osc, g, cut, res, morph_amount, drive, gain, pan;
  Adsr env{};
  Adsr amp{};
  Upsampler2x up{};
  Downsampler2x down{};
  OtaLadder ota{};
  SemFilter sem{};
  double last_left = 0.0;
  double last_right = 0.0;
};

class EmberKernel final : public SynthKernel {
public:
  explicit EmberKernel(double sample_rate)
      : rate_(sample_rate), control_rate_(sample_rate / control_interval_frames) {
    voices_ = std::make_unique<std::array<Voice, voice_limit>>();
    for (std::uint32_t i = 0; i < voice_limit; ++i) {
      auto& voice = (*voices_)[i];
      voice.slot = i;
      voice.env.prepare(rate_);
      voice.filter_fade.prepare(rate_);
      voice.amp.prepare(rate_);
      voice.pitch_drift.prepare(control_rate_, 0.35);
      voice.cutoff_drift.prepare(control_rate_, 0.2);
      voice.jitter.prepare(control_rate_, jitter_hz);
    }
    hum_.prepare(rate_);
    hum_attack_ = 1.0 / (ember_circuit::hum_attack_seconds * rate_);
    hum_release_ = 1.0 / (ember_circuit::hum_release_seconds * rate_);
    leak_frames_ = static_cast<std::uint32_t>(std::lround(ember_circuit::vca_leak_seconds * rate_));
    lfo_.prepare(control_rate_);
    hpf_.prepare(rate_);
    ensemble_.prepare(rate_);
    chorus_.prepare(rate_);
    delay_.prepare(rate_);
    tape_.prepare(rate_);
    dc_[0].prepare(rate_);
    dc_[1].prepare(rate_);
    hiss_tone_ = 1.0 - std::exp(-2.0 * 3.14159265358979323846 * 6'000.0 / rate_);
    follower_release_ = std::exp(-1.0 / (0.15 * rate_));
    reset_voices();
  }

  void control(std::span<const double> values, std::uint32_t frames_to_next_tick,
               const SynthTransport& transport) noexcept override {
    values_ = values;
    frames_ = frames_to_next_tick;
    beats_per_second_ = transport.beats_per_frame > 0.0 ? transport.beats_per_frame * rate_ : 2.0;
    const bool synced = values[p::lfo_sync] != 0.0;
    const auto division = static_cast<std::size_t>(values[p::lfo_division]);
    lfo_.set_shape(lfo_shape());
    if (synced && transport.beats_per_frame > 0.0) {
      lfo_value_ = lfo_.next_at(sync_cycles(division, transport));
    } else {
      const double rate = synced ? sync_hz(division, transport, rate_) : values[p::lfo_rate];
      lfo_value_ = lfo_.next(rate / control_rate_);
    }
    for (auto& voice : *voices_) {
      if (voice.active) {
        set_envelopes(voice);
        control_voice(voice, false);
      }
    }
    // Effects follow the panel smoothly.
    const auto ramp = frames_;
    hpf_.set_position(as_int(values[p::hpf]));
    ensemble_.set_amount(values[p::ensemble], ramp);
    chorus_.set(static_cast<ChorusMode>(as_int(values[p::chorus])), 1.0);
    hiss_.to(values[p::chorus] != 0.0 ? values[p::hiss] : 0.0, ramp, !primed_);
    const double delay_seconds =
        values[p::delay_sync] != 0.0
            ? sync_seconds(static_cast<std::size_t>(values[p::delay_division]), transport, rate_)
            : values[p::delay_time];
    delay_.set(delay_seconds, values[p::delay_feedback], values[p::delay],
               values[p::delay_pingpong] != 0.0, ramp);
    tape_.set(values[p::tape], values[p::wobble], ramp);
    const double level =
        values[p::out_level] <= -59.9 ? 0.0 : std::pow(10.0, values[p::out_level] / 20.0);
    out_gain_.to(level, ramp, !primed_);
    primed_ = true;
  }

  void note_on(const NoteEvent& event) noexcept override {
    if (values_.empty()) {
      return;
    }
    const auto mode = static_cast<VoiceMode>(as_int(values_[p::voice_mode]));
    if (mode == VoiceMode::unison) {
      unison_on(event);
    } else if (mode == VoiceMode::chord) {
      const auto& shape = chord_intervals[static_cast<std::size_t>(values_[p::chord])];
      for (const int interval : shape) {
        if (interval >= 0 && event.key + interval <= 127) {
          NoteEvent note = event;
          note.key = static_cast<std::uint8_t>(event.key + interval);
          start(allocate(), note, 0.0, true);
        }
      }
      last_pitch_ = event.key;
    } else {
      start(allocate(), event, 0.0, true);
      last_pitch_ = event.key;
    }
  }

  void note_off(const NoteEvent& event) noexcept override {
    const auto same_note = [&event](const NoteEvent& note) {
      return note.clip == event.clip && note.note == event.note;
    };
    if (unison_active_ &&
        std::any_of(unison_stack_.begin(), unison_stack_.begin() + unison_stack_size_, same_note)) {
      unison_off(event);
      return;
    }
    for (auto& voice : *voices_) {
      if (voice.active && voice.held && voice.note.clip == event.clip &&
          voice.note.note == event.note) {
        voice.held = false;
        voice.env.gate_off();
        voice.amp.gate_off();
      }
    }
  }

  void render(std::span<engine::StereoFrame> output) noexcept override {
    const std::size_t frames = output.size();
    Chunk left{};
    Chunk right{};
    bool sounding = false;
    double load = 0.0;
    for (auto& voice : *voices_) {
      if (voice.active) {
        render_voice(voice, left, right, frames);
        sounding = true;
        load += voice.amp.value();
      }
    }
    // The supply carries every sounding card; the next tick's VCOs sag with it.
    load_ = load / static_cast<double>(voice_limit);
    const double hum_amount = values_.empty() ? 0.0 : ember_circuit::hum_level * values_[p::hum];
    const double mains = values_.empty()
                             ? mains_hz[0]
                             : mains_hz[static_cast<std::size_t>(as_int(values_[p::mains]))];
    for (std::size_t i = 0; i < frames; ++i) {
      StereoSample x{left[i], right[i]};
      // Mains hum, there while the instrument sounds (the gate stands in for
      // the hum being masked when nothing plays).
      hum_gate_ = sounding ? std::min(1.0, hum_gate_ + hum_attack_)
                           : std::max(0.0, hum_gate_ - hum_release_);
      if (hum_gate_ > 0.0 && hum_amount > 0.0) {
        const double h = hum_amount * hum_gate_ * hum_.next(mains);
        x.left += h;
        x.right += h;
      }
      declick_.add(x.left, x.right);
      x = hpf_.process(x);
      auto ensembled = ensemble_.process(x.left, x.right);
      auto chorused = chorus_.process(ensembled.left, ensembled.right);
      // The BBD compander hides hiss in silence: it follows the signal.
      const double level = std::max(std::fabs(ensembled.left), std::fabs(ensembled.right));
      follower_ = level > follower_ ? level : follower_ * follower_release_;
      const double hiss = hiss_.next() * 0.0025 * std::min(1.0, 20.0 * follower_);
      if (hiss > 1e-9) {
        hiss_left_ += (hiss_noise_.white() - hiss_left_) * hiss_tone_;
        hiss_right_ += (hiss_noise_.white() - hiss_right_) * hiss_tone_;
        chorused.left += hiss * hiss_left_;
        chorused.right += hiss * hiss_right_;
      }
      const auto delayed = delay_.process(chorused.left, chorused.right);
      const auto taped = tape_.process(delayed.left, delayed.right);
      const double gain = out_gain_.next();
      output[i].left += static_cast<float>(safety(dc_[0].process(taped.left) * gain));
      output[i].right += static_cast<float>(safety(dc_[1].process(taped.right) * gain));
    }
    frames_ -= static_cast<std::uint32_t>(std::min<std::size_t>(frames, frames_));
  }

  void reset() noexcept override {
    for (const auto& voice : *voices_) {
      if (voice.active) {
        capture_declick(voice);
      }
    }
    reset_voices();
  }

  [[nodiscard]] std::size_t telemetry(std::span<float> out) const noexcept override {
    if (out.size() < telemetry::size) {
      return 0;
    }
    std::fill_n(out.begin(), telemetry::size, 0.0F);
    out[telemetry::version] = 1.0F;
    for (const auto& voice : *voices_) {
      if (voice.slot >= ember_circuit::cards) {
        continue;
      }
      out[telemetry::card_active + voice.slot] = voice.active ? 1.0F : 0.0F;
      out[telemetry::card_vco_cents + voice.slot] = static_cast<float>(voice.vco_cents);
      out[telemetry::card_dco_cents + voice.slot] = static_cast<float>(voice.dco_cents);
      out[telemetry::card_cutoff + voice.slot] = static_cast<float>(voice.card.cutoff);
    }
    out[telemetry::hum_gate] = static_cast<float>(hum_gate_);
    out[telemetry::load] = static_cast<float>(load_);
    return telemetry::size;
  }

  [[nodiscard]] KernelStatus status() const noexcept override {
    KernelStatus status;
    status.voice_steals = steals_;
    for (const auto& voice : *voices_) {
      if (voice.active) {
        ++status.active_voices;
      }
    }
    return status;
  }

private:
  using Chunk = std::array<double, control_interval_frames>;

  [[nodiscard]] FilterType filter_type() const noexcept {
    return static_cast<FilterType>(as_int(values_[p::filter_type]));
  }
  [[nodiscard]] LfoShape lfo_shape() const noexcept {
    switch (as_int(values_[p::lfo_shape])) {
    case 1:
      return LfoShape::square;
    case 2:
      return LfoShape::sample_hold;
    case 3:
      return LfoShape::saw_up;
    default:
      return LfoShape::triangle;
    }
  }

  void reset_voices() noexcept {
    for (auto& voice : *voices_) {
      voice.active = false;
      voice.held = false;
      voice.unison = false;
      // Component drift restarts from the same seeds, so playback after a
      // seek and an export from the same point age identically.
      voice.pitch_drift.reset(voice.slot * 7'919U + 11U);
      voice.cutoff_drift.reset(voice.slot * 104'729U + 13U);
      voice.jitter.reset(voice.slot * 31'337U + 17U);
      voice.leaking = false;
      voice.leak_remaining = 0;
    }
    hum_.reset();
    hum_gate_ = 0.0;
    load_ = 0.0;
    lfo_.reset(0.0, 77U);
    phase_random_.reset(0xEB3E2026U);
    hiss_noise_.reset(4'242U);
    next_slot_ = 0;
    clock_ = 0;
    unison_active_ = false;
    unison_stack_size_ = 0;
    last_pitch_ = -1.0;
  }

  // ---- Allocation ------------------------------------------------------------

  // Round-robin from the slot after the last one used, like classic polys,
  // so consecutive notes land on different (differently aged) voice cards.
  Voice& allocate() noexcept {
    return allocate_voice(*voices_, next_slot_, [this](const Voice& voice) {
      capture_declick(voice);
      ++steals_;
    });
  }

  void capture_declick(const Voice& voice) noexcept {
    declick_.capture(voice.last_left, voice.last_right);
  }

  // This card's character at the panel's settings now.
  [[nodiscard]] ember_circuit::Card circuit_card(const Voice& voice) const noexcept {
    return ember_circuit::card(static_cast<std::uint32_t>(as_int(values_[p::unit])), voice.slot,
                               values_[p::age], values_[p::tuning], values_[p::bleed]);
  }

  void set_envelopes(Voice& voice) noexcept {
    const auto v = values_;
    // Each voice card's envelopes run a little fast or slow with Age.
    const double speed = circuit_card(voice).envelope;
    voice.env.set({v[p::env_attack] * speed, v[p::env_decay] * speed, v[p::env_sustain],
                   v[p::env_release] * speed, analog_curve});
    switch (static_cast<AmpMode>(as_int(v[p::amp_mode]))) {
    case AmpMode::shared:
      voice.amp.set({v[p::env_attack] * speed, v[p::env_decay] * speed, v[p::env_sustain],
                     v[p::env_release] * speed, analog_curve});
      break;
    case AmpMode::gate:
      voice.amp.set({0.001, 0.01, 1.0, 0.004, 0.0});
      break;
    case AmpMode::own:
      voice.amp.set({v[p::amp_attack] * speed, v[p::amp_decay] * speed, v[p::amp_sustain],
                     v[p::amp_release] * speed, analog_curve});
      break;
    }
  }

  void start(Voice& voice, const NoteEvent& event, double unison_position,
             bool retrigger) noexcept {
    const auto v = values_;
    const bool continuing = voice.active && !voice.leaking;
    voice.active = true;
    voice.leaking = false;
    voice.leak_remaining = 0;
    voice.held = true;
    voice.unison = false;
    voice.note = event;
    voice.key = event.key;
    voice.velocity = event.velocity / 127.0;
    voice.age = ++clock_;
    voice.unison_position = unison_position;
    voice.glide_target = event.key;
    const double glide_time = v[p::glide];
    if (glide_time > 0.0 && (continuing || last_pitch_ >= 0.0)) {
      if (!continuing) {
        voice.pitch = last_pitch_;
      }
      voice.glide_rate =
          std::fabs(voice.glide_target - voice.pitch) / std::max(1.0, glide_time * control_rate_);
    } else {
      voice.pitch = event.key;
      voice.glide_rate = 0.0;
    }
    if (!continuing) {
      voice.filter = filter_type();
      // Age 0 starts every oscillator in phase; age adds free-running jitter.
      const double jitter = v[p::age];
      const double phase = jitter > 0.0 ? jitter * (0.5 + 0.5 * phase_random_.white()) : 0.0;
      voice.saw.reset(phase, Waveform::saw);
      voice.pulse.reset(phase, Waveform::pulse, 0.5);
      voice.sub.reset(phase * 0.5, Waveform::pulse, 0.5);
      voice.vco.reset(jitter > 0.0 ? jitter * (0.5 + 0.5 * phase_random_.white()) : 0.0, vco_wave(),
                      v[p::vco_pw]);
      voice.noise.reset(static_cast<std::uint32_t>(voice.age * 2'654'435'761U + voice.slot + 1U));
      voice.up.reset();
      voice.down.reset();
      voice.ota.reset();
      voice.sem.reset();
      voice.filter_fade.stop();
      voice.env.reset();
      voice.amp.reset();
      voice.last_left = 0.0;
      voice.last_right = 0.0;
    }
    set_envelopes(voice);
    if (retrigger || !continuing) {
      voice.env.gate_on();
      voice.amp.gate_on();
      voice.seconds = 0.0;
    }
    control_voice(voice, !continuing);
  }

  [[nodiscard]] Waveform vco_wave() const noexcept {
    switch (as_int(values_[p::vco_wave])) {
    case 1:
      return Waveform::pulse;
    case 2:
      return Waveform::triangle;
    default:
      return Waveform::saw;
    }
  }

  // ---- Unison ----------------------------------------------------------------

  void unison_on(const NoteEvent& event) noexcept {
    if (unison_stack_size_ == unison_stack_.size())
      return;
    unison_stack_[unison_stack_size_++] = event;
    const bool legato = unison_active_ && (*voices_)[0].held;
    unison_active_ = true;
    play_unison(event, !legato);
    last_pitch_ = event.key;
  }

  void play_unison(const NoteEvent& event, bool retrigger) noexcept {
    for (std::uint32_t i = 0; i < voice_limit; ++i) {
      auto& voice = (*voices_)[i];
      const double position = -1.0 + 2.0 * static_cast<double>(i) / (voice_limit - 1);
      if (voice.active && !voice.held && retrigger) {
        capture_declick(voice);
        voice.active = false;
      }
      start(voice, event, position, retrigger);
      voice.unison = true;
    }
  }

  void unison_off(const NoteEvent& event) noexcept {
    const auto same_note = [&event](const NoteEvent& note) {
      return note.clip == event.clip && note.note == event.note;
    };
    const bool was_lead =
        unison_stack_size_ > 0 && same_note(unison_stack_[unison_stack_size_ - 1]);
    std::size_t kept = 0;
    for (std::size_t i = 0; i < unison_stack_size_; ++i) {
      if (!same_note(unison_stack_[i])) {
        unison_stack_[kept++] = unison_stack_[i];
      }
    }
    unison_stack_size_ = kept;
    if (!was_lead) {
      return;
    }
    if (unison_stack_size_ > 0) {
      const auto previous = unison_stack_[unison_stack_size_ - 1];
      for (auto& voice : *voices_) {
        if (voice.active && voice.unison && same_note(voice.note)) {
          start(voice, previous, voice.unison_position, false);
          voice.unison = true;
        }
      }
      return;
    }
    for (auto& voice : *voices_) {
      if (voice.active && voice.unison && same_note(voice.note)) {
        voice.held = false;
        voice.env.gate_off();
        voice.amp.gate_off();
      }
    }
    unison_active_ = false;
  }

  // ---- Control rate ----------------------------------------------------------

  void control_voice(Voice& voice, bool jump) noexcept {
    const auto v = values_;
    const std::uint32_t frames = frames_;
    const auto type = filter_type();
    if (!jump && !voice.filter_fade.active() && type != voice.filter) {
      voice.fading_filter = voice.filter;
      voice.filter = type;
      // The new topology starts from rest; the fade hides its settling.
      if (type == FilterType::ota) {
        voice.ota.reset();
      } else {
        voice.sem.reset();
      }
      voice.filter_fade.start();
    }
    if (voice.glide_rate > 0.0) {
      const double distance = voice.glide_target - voice.pitch;
      if (std::fabs(distance) <= voice.glide_rate) {
        voice.pitch = voice.glide_target;
        voice.glide_rate = 0.0;
      } else {
        voice.pitch += distance > 0.0 ? voice.glide_rate : -voice.glide_rate;
      }
    }
    voice.seconds += 1.0 / control_rate_;
    const double delay = v[p::lfo_delay];
    const double lfo = lfo_value_ * (delay <= 0.0 ? 1.0 : std::min(1.0, voice.seconds / delay));
    const double env = voice.env.value();
    const double a = v[p::age];
    const double tuning = v[p::tuning];
    const double bleed = v[p::bleed];
    const auto card = circuit_card(voice);
    voice.card = card;

    // Pitch: bend, LFO (deepened by the wheel) and unison are played; the
    // circuit adds the rest. DCO A is crystal-divided: it never drifts, it
    // only lands on the nearest clock division.
    const double vibrato = lfo * (v[p::lfo_pitch] + 0.5 * v[p::wheel]) * 2.0;
    const double common = voice.pitch + v[p::bend] * v[p::bend_range] + vibrato +
                          voice.unison_position * v[p::detune] / 100.0;
    const double range = 12.0 * (v[p::dco_range] - 1.0);
    const double poly_freq = v[p::pm_freq] != 0.0 ? 24.0 * v[p::pm_env] * env : 0.0;
    const double dco_ideal = hz(common + range + poly_freq);
    const double dco = ember_circuit::dco_hz(dco_ideal);
    voice.dco_cents = 1'200.0 * std::log2(dco / dco_ideal);
    voice.increment_a.to(dco / rate_, frames, jump);
    voice.dco_level =
        ember_circuit::dco_level(static_cast<std::uint32_t>(as_int(v[p::unit])), voice.key, a);
    // VCO B wanders with temperature (slow drift and fast jitter), sags with
    // the supply's load, and tracks through its card's converter.
    const double wander =
        (a * ember_circuit::age_drift_cents + tuning * ember_circuit::tuning_drift_cents) *
            voice.pitch_drift.next() +
        tuning * ember_circuit::jitter_cents * voice.jitter.next();
    const double sag = -a * ember_circuit::sag_cents * load_;
    (void)voice.cutoff_drift.next();
    const double vco_semitones = common + v[p::vco_semi] + v[p::vco_fine] / 100.0;
    double vco_hz = 0.0;
    if (v[p::vco_low] != 0.0) {
      vco_hz = 2.0 * std::exp2((v[p::vco_semi] + v[p::vco_fine] / 100.0) / 12.0);
      voice.vco_cents = 0.0;
    } else {
      const double ideal = hz(vco_semitones);
      vco_hz =
          ember_circuit::vco_hz(ideal * std::exp2((wander + sag) / 1'200.0), vco_semitones, card);
      voice.vco_cents = 1'200.0 * std::log2(vco_hz / ideal);
    }
    voice.increment_b.to(vco_hz / rate_, frames, jump);

    // Pulse width: manual, LFO or envelope PWM, plus poly-mod from the envelope.
    double pwm = 0.0;
    switch (as_int(v[p::pwm_source])) {
    case 1:
      pwm = 0.5 + 0.5 * lfo;
      break;
    case 2:
      pwm = env;
      break;
    default:
      break;
    }
    double width_amount = clamp(v[p::dco_pw] + v[p::dco_pwm] * pwm, 0.0, 1.0);
    if (v[p::pm_pw] != 0.0) {
      width_amount = clamp(width_amount + v[p::pm_env] * env, 0.0, 1.0);
    }
    voice.width_a.to(clamp(0.5 - 0.45 * width_amount + card.pulse, 0.05, 0.95), frames, jump);
    voice.width_b.to(clamp(v[p::vco_pw] + card.pulse, 0.05, 0.95), frames, jump);
    voice.vco.set_waveform(vco_wave(), v[p::vco_pw]);

    // Each oscillator bleeds into the mixer however far its level is down.
    const double leak = bleed * ember_circuit::oscillator_leak;
    voice.level_saw.to((v[p::dco_saw] + leak) * voice.dco_level, frames, jump);
    voice.level_pulse.to((v[p::dco_pulse] + leak) * voice.dco_level, frames, jump);
    voice.level_sub.to((v[p::dco_sub] + leak) * voice.dco_level, frames, jump);
    voice.level_noise.to(v[p::dco_noise] * 0.6, frames, jump);
    voice.level_vco.to(v[p::vco_low] != 0.0 ? 0.0 : v[p::vco_level] + leak, frames, jump);
    voice.pm_osc.to(v[p::pm_osc], frames, jump);

    // Filter: cutoff in semitones above the knob; runs at twice the rate.
    const double semitones =
        v[p::filter_env] * 84.0 * env + v[p::filter_lfo] * 36.0 * lfo +
        v[p::keytrack] * (voice.key - 60.0) + v[p::filter_velocity] * 36.0 * voice.velocity +
        v[p::pressure] * 24.0 + card.cutoff +
        a * ember_circuit::cutoff_drift_semitones * voice.cutoff_drift.value() +
        (v[p::pm_cutoff] != 0.0 ? 48.0 * v[p::pm_env] * env : 0.0);
    const double cutoff_hz = clamp(v[p::cutoff] * std::exp2(semitones / 12.0), 20.0, 20'000.0);
    voice.g.to(prewarp(cutoff_hz / (2.0 * rate_)), frames, jump);
    voice.cut.to(cutoff_hz / (2.0 * rate_), frames, jump);
    voice.res.to(clamp(v[p::resonance] * card.resonance, 0.0, 1.0), frames, jump);
    voice.morph_amount.to(v[p::morph], frames, jump);
    voice.drive.to(0.6 + 3.4 * v[p::filter_drive], frames, jump);

    const double sensitivity = v[p::amp_velocity];
    const double velocity_gain = 1.0 - sensitivity + sensitivity * voice.velocity * voice.velocity;
    voice.gain.to(voice_gain * velocity_gain, frames, jump);
    // Unison spreads the voice cards across the stereo field.
    voice.pan.to(0.8 * voice.unison_position, frames, jump);
  }

  // ---- Audio rate ------------------------------------------------------------

  void render_voice(Voice& voice, Chunk& left, Chunk& right, std::size_t frames) noexcept {
    const auto v = values_;
    const bool sync = v[p::vco_sync] != 0.0;
    const bool pm_freq = v[p::pm_freq] != 0.0 && !voice.pm_osc.silent();
    const bool pm_pw = v[p::pm_pw] != 0.0 && !voice.pm_osc.silent();
    const bool pm_cutoff = v[p::pm_cutoff] != 0.0 && !voice.pm_osc.silent();
    const bool use_pulse = !voice.level_pulse.silent();
    const bool use_sub = !voice.level_sub.silent();
    const bool use_noise = !voice.level_noise.silent();
    const double noise_floor = 2e-5 * v[p::age];

    Chunk b{};
    std::array<SyncPoint, control_interval_frames> wraps{};
    // The saw core's ramp bows as its capacitor charges.
    const double curvature = vco_wave() == Waveform::saw ? voice.card.curvature : 0.0;
    for (std::size_t i = 0; i < frames; ++i) {
      b[i] = ember_circuit::bow(voice.vco.next(voice.increment_b.next(), voice.width_b.next(), {},
                                               sync ? &wraps[i] : nullptr),
                                curvature);
    }
    Chunk mix{};
    Chunk pm{};
    for (std::size_t i = 0; i < frames; ++i) {
      const double depth = voice.pm_osc.next();
      pm[i] = depth * b[i];
      double increment = voice.increment_a.next();
      if (pm_freq) {
        increment *= std::exp2(2.0 * pm[i]);
      }
      double width = voice.width_a.next();
      if (pm_pw) {
        width = clamp(width + 0.4 * pm[i], 0.05, 0.95);
      }
      const SyncPoint reset = sync ? wraps[i] : SyncPoint{};
      double x = voice.saw.next(increment, 0.5, reset, nullptr) * voice.level_saw.next();
      const double pulse_level = voice.level_pulse.next();
      if (use_pulse) {
        x += voice.pulse.next(increment, width, reset, nullptr) * pulse_level;
      }
      const double sub_level = voice.level_sub.next();
      if (use_sub) {
        x += voice.sub.next(0.5 * increment, 0.5, {}, nullptr) * sub_level;
      }
      const double noise_level = voice.level_noise.next();
      if (use_noise || noise_floor > 0.0) {
        x += voice.noise.next(NoiseColour::white) * (noise_level + noise_floor);
      }
      x += b[i] * voice.level_vco.next();
      mix[i] = x;
    }

    const bool ota = voice.filter == FilterType::ota;
    const bool fading = voice.filter_fade.active();
    // A little input gets past the filter however far it is closed.
    const double feedthrough = v[p::bleed] * ember_circuit::filter_leak;
    for (std::size_t i = 0; i < frames; ++i) {
      double g = voice.g.next();
      const double cut = voice.cut.next();
      if (pm_cutoff) {
        // Audio-rate cutoff modulation from VCO B, up to two octaves.
        g = prewarp(cut * std::exp2(2.0 * pm[i]));
      }
      const double res = voice.res.next();
      const double morph = voice.morph_amount.next();
      const double drive = voice.drive.next();
      double a = 0.0;
      double c = 0.0;
      voice.up.process(mix[i], a, c);
      const double in_a = a;
      const double in_c = c;
      if (fading) {
        // Both topologies run from the same input; the old one fades out.
        const auto ko = OtaCoefficients::make(g, res, drive);
        const auto ks = SemCoefficients::make(g, res, morph);
        const double oa = voice.ota.process(a, ko);
        const double oc = voice.ota.process(c, ko);
        const double sa = voice.sem.process(soft_clip(a * drive) / drive, ks);
        const double sc = voice.sem.process(soft_clip(c * drive) / drive, ks);
        const double weight = voice.filter_fade.next(); // Of the old filter.
        const double w_ota = ota ? 1.0 - weight : weight;
        a = sa + (oa - sa) * w_ota;
        c = sc + (oc - sc) * w_ota;
      } else if (ota) {
        const auto k = OtaCoefficients::make(g, res, drive);
        a = voice.ota.process(a, k);
        c = voice.ota.process(c, k);
      } else {
        const auto k = SemCoefficients::make(g, res, morph);
        a = voice.sem.process(soft_clip(a * drive) / drive, k);
        c = voice.sem.process(soft_clip(c * drive) / drive, k);
      }
      mix[i] = voice.down.process(a + feedthrough * in_a, c + feedthrough * in_c);
    }

    // The VCA never quite shuts (its leak fades once the voice has ended),
    // and its control offset passes a little of the envelope itself: the
    // thump on a fast attack.
    const auto amp_mode = static_cast<AmpMode>(as_int(v[p::amp_mode]));
    const double vca_leak = v[p::bleed] * ember_circuit::vca_leak;
    for (std::size_t i = 0; i < frames; ++i) {
      const double env = voice.env.next();
      const double amp = voice.amp.next();
      const double shape = amp_mode == AmpMode::shared ? env : amp;
      double floor = vca_leak;
      if (voice.leaking) {
        const std::uint32_t left_frames =
            voice.leak_remaining > i ? static_cast<std::uint32_t>(voice.leak_remaining - i) : 0U;
        floor *= static_cast<double>(left_frames) / static_cast<double>(leak_frames_);
      }
      const double gain = voice.gain.next();
      const double level = shape * gain + floor;
      const double y = mix[i] * level + voice.card.vca_offset * shape * gain;
      const double pan = voice.pan.next();
      voice.last_left = y * (pan > 0.0 ? 1.0 - pan : 1.0);
      voice.last_right = y * (pan < 0.0 ? 1.0 + pan : 1.0);
      left[i] += voice.last_left;
      right[i] += voice.last_right;
    }
    if (voice.leaking) {
      voice.leak_remaining -=
          std::min<std::uint32_t>(voice.leak_remaining, static_cast<std::uint32_t>(frames));
    }
    const bool silent = amp_mode == AmpMode::shared ? !voice.env.active() : !voice.amp.active();
    if (silent && !voice.leaking && vca_leak > 0.0) {
      voice.leaking = true;
      voice.held = false;
      voice.leak_remaining = leak_frames_;
    } else if (silent && (!voice.leaking || voice.leak_remaining == 0)) {
      voice.active = false;
      voice.held = false;
      voice.leaking = false;
      voice.last_left = 0.0;
      voice.last_right = 0.0;
    }
  }

  [[nodiscard]] static double safety(double x) noexcept {
    constexpr double knee = 0.9;
    const double magnitude = std::fabs(x);
    if (magnitude <= knee) {
      return x;
    }
    const double shaped = knee + (1.0 - knee) * soft_clip((magnitude - knee) / (1.0 - knee) * 3.0);
    return x < 0.0 ? -shaped : shaped;
  }

  double rate_;
  double control_rate_;
  std::unique_ptr<std::array<Voice, voice_limit>> voices_;
  std::span<const double> values_{};
  std::uint32_t frames_ = 0;
  double beats_per_second_ = 2.0;
  Lfo lfo_{};
  double lfo_value_ = 0.0;
  NoiseSource phase_random_{};
  std::uint32_t next_slot_ = 0;
  std::uint64_t clock_ = 0;
  std::uint32_t steals_ = 0;
  bool unison_active_ = false;
  std::array<NoteEvent, 16> unison_stack_{};
  std::size_t unison_stack_size_ = 0;
  double last_pitch_ = -1.0;
  Declick declick_;
  StereoHighPass4 hpf_{};
  Ensemble ensemble_{};
  Chorus chorus_{};
  BbdDelay delay_{};
  TapeStage tape_{};
  NoiseSource hiss_noise_{};
  double hiss_left_ = 0.0;
  double hiss_right_ = 0.0;
  double hiss_tone_ = 0.5;
  double follower_ = 0.0;
  double follower_release_ = 0.9999;
  Ramp hiss_{};
  Ramp out_gain_{};
  bool primed_ = false;
  std::array<DcBlocker, 2> dc_{};
  ember_circuit::Hum hum_{};
  double hum_gate_ = 0.0;
  double hum_attack_ = 0.0;
  double hum_release_ = 0.0;
  double load_ = 0.0;
  std::uint32_t leak_frames_ = 1;
};

std::unique_ptr<SynthKernel> create_kernel(double sample_rate) {
  return std::make_unique<EmberKernel>(sample_rate);
}

std::uint64_t tail_frames(std::span<const double> values, double sample_rate) {
  const bool shared = as_int(values[p::amp_mode]) == 0;
  const double release =
      1.15 * std::max(values[p::env_release], shared ? 0.0 : values[p::amp_release]);
  const double delay_seconds =
      values[p::delay] > 0.0
          ? BbdDelay::tail_seconds(BbdDelay::max_seconds, values[p::delay_feedback])
          : 0.0;
  const double seconds = release + delay_seconds + Ensemble::tail_seconds + Chorus::tail_seconds +
                         TapeStage::latency_seconds + TapeStage::tail_seconds +
                         ember_circuit::vca_leak_seconds + ember_circuit::hum_release_seconds +
                         0.05;
  return static_cast<std::uint64_t>(std::ceil(seconds * sample_rate)) +
         BlepOscillator::latency_frames;
}

const EngineDescriptor descriptor{"ember", "Ember", 1, ember_table::parameters,
                                  ember_presets::presets};
const SynthEngine ember_engine{&descriptor, &create_kernel, &tail_frames};

} // namespace

const SynthEngine& engine() { return ember_engine; }

} // namespace oscillate::native_dsp::synth::ember
