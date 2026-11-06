#include "oscillate/native_dsp/synth/drift_engine.hpp"

#include "drift_presets.hpp"
#include "drift_table.hpp"
#include "dsp/ramp.hpp"
#include "oscillate/native_dsp/synth/chorus.hpp"
#include "oscillate/native_dsp/synth/crossfade.hpp"
#include "oscillate/native_dsp/synth/filter.hpp"
#include "oscillate/native_dsp/synth/grain_dsp.hpp"
#include "oscillate/native_dsp/synth/modulation.hpp"
#include "oscillate/native_dsp/synth/shaping.hpp"
#include "oscillate/native_dsp/synth/tempo_sync.hpp"
#include "oscillate/native_dsp/synth/vintage_effects.hpp"
#include "oscillate/native_dsp/synth/voice_pool.hpp"
#include "oscillate/native_dsp/synth/wavetable.hpp"
#include "oscillate/native_dsp/synth/wavetable_oscillator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>

namespace oscillate::native_dsp::synth::drift {
namespace {

using drift_table::index_of;

namespace p {
constexpr std::size_t octave = index_of("drift.source.octave");
constexpr std::size_t table = index_of("drift.wt.table");
constexpr std::size_t wt_level = index_of("drift.wt.level");
constexpr std::size_t wt_position = index_of("drift.wt.position");
constexpr std::size_t wt_drift = index_of("drift.wt.drift");
constexpr std::size_t partials_level = index_of("drift.partials.level");
constexpr std::size_t tilt = index_of("drift.partials.tilt");
constexpr std::size_t odd = index_of("drift.partials.odd");
constexpr std::size_t stretch = index_of("drift.partials.stretch");
constexpr std::size_t air_level = index_of("drift.air.level");
constexpr std::size_t air_width = index_of("drift.air.width");
constexpr std::size_t size = index_of("drift.grain.size");
constexpr std::size_t density = index_of("drift.grain.density");
constexpr std::size_t chaos = index_of("drift.grain.chaos");
constexpr std::size_t shape = index_of("drift.grain.shape");
constexpr std::size_t position = index_of("drift.grain.position");
constexpr std::size_t scan = index_of("drift.grain.scan");
constexpr std::size_t spray = index_of("drift.grain.spray");
constexpr std::size_t pitch = index_of("drift.grain.pitch");
constexpr std::size_t intervals = index_of("drift.grain.intervals");
constexpr std::size_t interval_amount = index_of("drift.grain.interval_amount");
constexpr std::size_t jitter = index_of("drift.grain.jitter");
constexpr std::size_t reverse = index_of("drift.grain.reverse");
constexpr std::size_t spread = index_of("drift.grain.spread");
constexpr std::size_t blur = index_of("drift.blur");
constexpr std::size_t freeze = index_of("drift.freeze");
constexpr std::size_t sync = index_of("drift.sync");
constexpr std::size_t sync_division = index_of("drift.sync.division");
constexpr std::size_t amp_attack = index_of("drift.amp.attack");
constexpr std::size_t amp_decay = index_of("drift.amp.decay");
constexpr std::size_t amp_sustain = index_of("drift.amp.sustain");
constexpr std::size_t amp_release = index_of("drift.amp.release");
constexpr std::size_t amp_velocity = index_of("drift.amp.velocity");
constexpr std::size_t filter_type = index_of("drift.filter.type");
constexpr std::size_t cutoff = index_of("drift.filter.cutoff");
constexpr std::size_t resonance = index_of("drift.filter.resonance");
constexpr std::size_t keytrack = index_of("drift.filter.keytrack");
constexpr std::size_t lfo1_shape = index_of("drift.lfo1.shape");
constexpr std::size_t lfo1_position = index_of("drift.lfo1.position");
constexpr std::size_t lfo1_density = index_of("drift.lfo1.density");
constexpr std::size_t lfo2_shape = index_of("drift.lfo2.shape");
constexpr std::size_t lfo2_filter = index_of("drift.lfo2.filter");
constexpr std::size_t lfo2_size = index_of("drift.lfo2.size");
constexpr std::size_t bend_range = index_of("drift.bend");
constexpr std::size_t bend = index_of("drift.perf.bend");
constexpr std::size_t wheel = index_of("drift.perf.wheel");
constexpr std::size_t chorus = index_of("drift.fx.chorus");
constexpr std::size_t delay = index_of("drift.fx.delay");
constexpr std::size_t delay_time = index_of("drift.fx.delay_time");
constexpr std::size_t delay_feedback = index_of("drift.fx.delay_feedback");
constexpr std::size_t delay_sync = index_of("drift.fx.delay_sync");
constexpr std::size_t delay_division = index_of("drift.fx.delay_division");
constexpr std::size_t verb_mix = index_of("drift.verb.mix");
constexpr std::size_t verb_size = index_of("drift.verb.size");
constexpr std::size_t verb_decay = index_of("drift.verb.decay");
constexpr std::size_t verb_damping = index_of("drift.verb.damping");
constexpr std::size_t verb_shimmer = index_of("drift.verb.shimmer");
constexpr std::size_t out_level = index_of("drift.out.level");
static_assert(index_of("drift.out.level") == drift_table::parameters.size() - 1,
              "the table has no zero-filled entries");
// The four LFO controls sit in a fixed order after each shape.
constexpr std::size_t lfo_rate = 1;
constexpr std::size_t lfo_sync = 2;
constexpr std::size_t lfo_division = 3;
} // namespace p

constexpr double voice_gain = 0.35;
constexpr std::size_t max_frames = control_interval_frames;

[[nodiscard]] int as_int(double value) noexcept { return static_cast<int>(value); }
[[nodiscard]] double clamp(double value, double low, double high) noexcept {
  return value < low ? low : (value > high ? high : value);
}
[[nodiscard]] double hz(double semitones) noexcept {
  return 440.0 * std::exp2((semitones - 69.0) / 12.0);
}

using Ramp = dsp::Ramp;

[[nodiscard]] FilterMode filter_mode(int type) noexcept {
  switch (type) {
  case 1:
    return FilterMode::lowpass_24;
  case 2:
    return FilterMode::bandpass_12;
  case 3:
    return FilterMode::highpass_12;
  default:
    return FilterMode::lowpass_12;
  }
}

[[nodiscard]] LfoShape lfo_shape(int choice) noexcept {
  switch (choice) {
  case 1:
    return LfoShape::triangle;
  case 2:
    return LfoShape::saw_down;
  case 3:
    return LfoShape::square;
  case 4:
    return LfoShape::sample_hold;
  default:
    return LfoShape::sine;
  }
}

struct Voice {
  bool active = false;
  bool held = false;
  NoteEvent note{};
  std::uint8_t key = 60;
  double velocity = 1.0;
  std::uint64_t age = 0;
  std::uint32_t slot = 0;
  int filter_type = 0;

  GrainBuffer buffer{};
  GrainCloud cloud{};
  WavetableOscillator wavetable{};
  PartialBank partials{};
  NoiseSource noise{};
  ZdfSvf air{};
  SvfCoefficients air_coefficients{};
  Adsr amp{};
  StereoMultimodeFilter filter{};
  // A filter-type change on a sounding note: the old filter keeps running,
  // warm, and crossfades out (ADR-0038).
  StereoMultimodeFilter fading_filter{};
  FilterCoefficients fading_coefficients{};
  int fading_type = 0;
  SwitchFade fade{};
  Ramp increment, wt_level, wt_position, partials_level, air_level, gain, g, k1, k2;
  double wander = 0.0; // Slow per-voice table-position drift phase.
  bool frozen = false;
  std::uint64_t frozen_count = 0;
  std::uint64_t write_until = 0;
  StereoSample last{};
};

class DriftKernel final : public SynthKernel {
public:
  explicit DriftKernel(double sample_rate)
      : rate_(sample_rate), control_rate_(sample_rate / control_interval_frames),
        tables_(&wavetables()) {
    voices_ = std::make_unique<std::array<Voice, voice_limit>>();
    const auto frames = static_cast<std::size_t>(buffer_seconds * rate_);
    for (std::uint32_t i = 0; i < voice_limit; ++i) {
      auto& voice = (*voices_)[i];
      voice.slot = i;
      voice.buffer.prepare(frames);
      voice.amp.prepare(rate_);
      voice.fade.prepare(rate_);
    }
    lfo1_.prepare(control_rate_);
    lfo2_.prepare(control_rate_);
    chorus_.prepare(rate_);
    delay_.prepare(rate_);
    reverb_.prepare(rate_);
    reset_voices();
  }

  void control(std::span<const double> values, std::uint32_t frames_to_next_tick,
               const SynthTransport& transport) noexcept override {
    values_ = values;
    frames_ = frames_to_next_tick;
    beats_per_frame_ = transport.beats_per_frame > 0.0 ? transport.beats_per_frame : 2.0 / rate_;
    transport_ = transport;
    lfo1_value_ = run_lfo(lfo1_, p::lfo1_shape, transport);
    lfo2_value_ = run_lfo(lfo2_, p::lfo2_shape, transport);
    for (auto& voice : *voices_) {
      if (voice.active) {
        control_voice(voice, false);
      }
    }
    const auto& v = values;
    const auto ramp = frames_;
    chorus_.set(static_cast<ChorusMode>(as_int(v[p::chorus])), 1.0);
    const double delay_seconds =
        v[p::delay_sync] != 0.0
            ? sync_seconds(static_cast<std::size_t>(v[p::delay_division]), transport, rate_)
            : v[p::delay_time];
    delay_.set(delay_seconds, v[p::delay_feedback], v[p::delay], true, ramp);
    reverb_.set(v[p::verb_size], v[p::verb_decay], v[p::verb_damping], v[p::verb_shimmer],
                v[p::verb_mix]);
    const double level = v[p::out_level] <= -59.9 ? 0.0 : std::pow(10.0, v[p::out_level] / 20.0);
    out_gain_.to(level, ramp, !primed_);
    primed_ = true;
  }

  void note_on(const NoteEvent& event) noexcept override {
    if (values_.empty()) {
      return;
    }
    auto& voice = allocate();
    voice.active = true;
    voice.held = true;
    voice.note = event;
    voice.key = event.key;
    voice.velocity = event.velocity / 127.0;
    voice.age = ++clock_;
    voice.filter_type = as_int(values_[p::filter_type]);
    // Every random choice is seeded by the slot and the note's age, so live
    // playback and export scatter identically.
    voice.cloud.reset(static_cast<std::uint32_t>(voice.slot * 7'919U + voice.age * 104'729U + 1U));
    voice.noise.reset(static_cast<std::uint32_t>(voice.age * 2'654'435'761U + voice.slot + 1U));
    voice.buffer.reset();
    voice.wavetable.reset(0.0);
    voice.partials.reset();
    voice.air.reset();
    voice.filter.reset();
    voice.fade.stop();
    voice.amp.reset();
    voice.wander = 0.173 * static_cast<double>(voice.slot);
    voice.frozen = false;
    voice.last = {};
    set_envelope(voice);
    voice.amp.gate_on();
    control_voice(voice, true);
  }

  void note_off(const NoteEvent& event) noexcept override {
    for (auto& voice : *voices_) {
      if (voice.active && voice.held && voice.note.clip == event.clip &&
          voice.note.note == event.note) {
        voice.held = false;
        voice.amp.gate_off();
      }
    }
  }

  void render(std::span<engine::StereoFrame> output) noexcept override {
    const std::size_t frames = output.size();
    std::array<double, max_frames> left{};
    std::array<double, max_frames> right{};
    const bool ends_tick = frames >= frames_;
    for (auto& voice : *voices_) {
      if (voice.active) {
        render_voice(voice, left, right, frames, ends_tick);
      }
    }
    for (std::size_t i = 0; i < frames; ++i) {
      double l = left[i];
      double r = right[i];
      declick_.add(l, r);
      const auto chorused = chorus_.process(l, r);
      const auto delayed = delay_.process(chorused.left, chorused.right);
      const auto wet = reverb_.process(delayed.left, delayed.right);
      const double gain = out_gain_.next();
      output[i].left += static_cast<float>(safety(wet.left * gain));
      output[i].right += static_cast<float>(safety(wet.right * gain));
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
    if (out.size() < telemetry::first_grain) {
      return 0;
    }
    const std::size_t capacity =
        std::min(telemetry::max_grains, (out.size() - telemetry::first_grain) / telemetry::stride);
    std::size_t grains = 0;
    for (const auto& voice : *voices_) {
      if (!voice.active) {
        continue;
      }
      const double head =
          static_cast<double>(voice.frozen ? voice.frozen_count : voice.buffer.count());
      voice.cloud.for_each_grain([&](double position, double rate, bool reverse, double phase) {
        if (grains == capacity) {
          return;
        }
        float* record = &out[telemetry::first_grain + grains * telemetry::stride];
        record[telemetry::key] = static_cast<float>(voice.key);
        record[telemetry::behind] = static_cast<float>((head - position) / rate_);
        record[telemetry::semitones] = static_cast<float>(12.0 * std::log2(rate));
        record[telemetry::reverse] = reverse ? 1.0F : 0.0F;
        record[telemetry::phase] = static_cast<float>(phase);
        record[telemetry::frozen] = voice.frozen ? 1.0F : 0.0F;
        ++grains;
      });
    }
    out[telemetry::count] = static_cast<float>(grains);
    return telemetry::first_grain + grains * telemetry::stride;
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
  [[nodiscard]] double run_lfo(Lfo& lfo, std::size_t base,
                               const SynthTransport& transport) noexcept {
    const auto& v = values_;
    lfo.set_shape(lfo_shape(as_int(v[base])));
    const auto division = static_cast<std::size_t>(v[base + p::lfo_division]);
    if (v[base + p::lfo_sync] != 0.0 && transport.beats_per_frame > 0.0) {
      return lfo.next_at(sync_cycles(division, transport));
    }
    const double hz =
        v[base + p::lfo_sync] != 0.0 ? sync_hz(division, transport, rate_) : v[base + p::lfo_rate];
    return lfo.next(hz / control_rate_);
  }

  void reset_voices() noexcept {
    for (auto& voice : *voices_) {
      voice.active = false;
      voice.held = false;
    }
    lfo1_.reset(0.0, 0xD1F7U);
    lfo2_.reset(0.25, 0xD2F7U);
    chorus_.reset();
    delay_.reset();
    reverb_.reset();
    next_slot_ = 0;
    clock_ = 0;
  }

  Voice& allocate() noexcept {
    return allocate_voice(*voices_, next_slot_, [this](const Voice& voice) {
      capture_declick(voice);
      ++steals_;
    });
  }

  void capture_declick(const Voice& voice) noexcept { declick_.capture(voice.last); }

  void set_envelope(Voice& voice) noexcept {
    const auto& v = values_;
    voice.amp.set({v[p::amp_attack], v[p::amp_decay], v[p::amp_sustain], v[p::amp_release], 0.6});
  }

  // ---- Control rate ----------------------------------------------------------

  void control_voice(Voice& voice, bool jump) noexcept {
    const auto& v = values_;
    const std::uint32_t frames = frames_;
    set_envelope(voice);
    const double note = voice.key + 12.0 * v[p::octave] + v[p::bend] * v[p::bend_range];
    const double f = hz(note);
    const double increment = f / rate_;

    // Source.
    const auto& table = (*tables_)[static_cast<std::size_t>(v[p::table])];
    voice.wavetable.set_band(table, increment, WarpMode::off, 0.0);
    voice.increment.to(increment, frames, jump);
    voice.wander += (0.05 + 0.01 * voice.slot) / control_rate_;
    voice.wander -= std::floor(voice.wander);
    const double wandered = v[p::wt_position] + 0.35 * v[p::wt_drift] * sin_cycle(voice.wander);
    voice.wt_position.to(clamp(wandered, 0.0, 1.0), frames, jump);
    voice.wt_level.to(v[p::wt_level], frames, jump);
    voice.partials.set(increment, v[p::tilt], v[p::odd], v[p::stretch]);
    voice.partials_level.to(v[p::partials_level], frames, jump);
    voice.air_level.to(0.5 * v[p::air_level], frames, jump);
    const double air_hz = clamp(2.0 * f, 40.0, 0.4 * rate_);
    voice.air_coefficients =
        SvfCoefficients::make(prewarp(air_hz / rate_), 0.15 + 2.5 * v[p::air_width]);

    // Grains. Blur (and the mod wheel) deepen spray, jitter, reverse and chaos.
    const double blur_total = clamp(v[p::blur] + v[p::wheel], 0.0, 1.0);
    GrainSettings g;
    g.size_frames = v[p::size] * std::exp2(lfo2_value_ * v[p::lfo2_size]) * rate_;
    const double density = v[p::density] * std::exp2(2.0 * lfo1_value_ * v[p::lfo1_density]);
    g.interval_frames = rate_ / density;
    g.chaos = clamp(v[p::chaos] + 0.5 * blur_total, 0.0, 1.0);
    g.synced = v[p::sync] != 0.0;
    g.frames_per_division =
        sync_beats(static_cast<std::size_t>(v[p::sync_division]), transport_) / beats_per_frame_;
    g.shape = static_cast<GrainShape>(as_int(v[p::shape]));
    g.lag_frames = std::max(0.0, v[p::position] + lfo1_value_ * v[p::lfo1_position]) * rate_;
    g.scan = v[p::scan];
    g.spray_frames = (v[p::spray] + 0.5 * blur_total) * rate_;
    g.rate = std::exp2(v[p::pitch] / 12.0);
    g.intervals = static_cast<GrainIntervals>(as_int(v[p::intervals]));
    g.interval_probability = v[p::interval_amount];
    g.jitter_cents = v[p::jitter] + 50.0 * blur_total;
    g.reverse_probability = clamp(v[p::reverse] + 0.4 * blur_total, 0.0, 1.0);
    g.spread = v[p::spread];

    // Freeze: a note started while frozen captures first; after freezing,
    // the writer runs on for one in-flight grain span.
    const double guard =
        std::min(4.0 * g.size_frames + 64.0, 0.5 * static_cast<double>(voice.buffer.length()));
    const auto capture = static_cast<std::uint64_t>(capture_seconds * rate_);
    if (v[p::freeze] != 0.0 && !voice.frozen && voice.buffer.count() >= capture) {
      voice.frozen = true;
      voice.frozen_count = voice.buffer.count();
      voice.write_until = voice.frozen_count + static_cast<std::uint64_t>(guard);
    } else if (v[p::freeze] == 0.0 && voice.frozen) {
      voice.frozen = false;
    }
    g.reserved_frames = voice.frozen ? guard : 0.0;
    voice.cloud.set(g);

    // Filter and level.
    const double semitones =
        v[p::keytrack] * (voice.key - 60.0) + 36.0 * lfo2_value_ * v[p::lfo2_filter];
    const double cutoff_hz = clamp(v[p::cutoff] * std::exp2(semitones / 12.0), 20.0, 20'000.0);
    const int type = as_int(v[p::filter_type]);
    if (!jump && type != voice.filter_type) {
      voice.fading_filter = voice.filter; // Same state: the new mode starts warm.
      voice.fading_type = voice.filter_type;
      voice.filter_type = type;
      voice.fade.start();
    }
    if (voice.fade.active()) {
      voice.fading_coefficients = make_filter_coefficients(filter_mode(voice.fading_type),
                                                           cutoff_hz / rate_, v[p::resonance], 1.0);
    }
    const auto c = make_filter_coefficients(filter_mode(voice.filter_type), cutoff_hz / rate_,
                                            v[p::resonance], 1.0);
    voice.g.to(c.g, frames, jump);
    voice.k1.to(c.k1, frames, jump);
    voice.k2.to(c.k2, frames, jump);
    const double sensitivity = v[p::amp_velocity];
    const double velocity_gain = 1.0 - sensitivity + sensitivity * voice.velocity * voice.velocity;
    voice.gain.to(voice_gain * velocity_gain, frames, jump);
  }

  // ---- Audio rate ----------------------------------------------------------------

  void render_voice(Voice& voice, std::array<double, max_frames>& left,
                    std::array<double, max_frames>& right, std::size_t frames,
                    bool ends_tick) noexcept {
    // Source into the buffer (not while frozen, after the in-flight guard).
    const std::uint64_t before = voice.buffer.count();
    // A frozen voice past its in-flight guard writes nothing, so it skips
    // its source entirely (the ramps still advance).
    const bool writing = !voice.frozen || voice.buffer.count() < voice.write_until;
    for (std::size_t i = 0; i < frames && !writing; ++i) {
      (void)voice.increment.next();
      (void)voice.wt_position.next();
      (void)voice.wt_level.next();
      (void)voice.partials_level.next();
      (void)voice.air_level.next();
    }
    for (std::size_t i = 0; i < frames && writing; ++i) {
      const double increment = voice.increment.next();
      double x = voice.wavetable.next(increment, voice.wt_position.next(), WarpMode::off, 0.0) *
                 voice.wt_level.next();
      x += voice.partials.next() * voice.partials_level.next();
      x += voice.air.process(voice.noise.white(), voice.air_coefficients).band *
           voice.air_level.next();
      if (!voice.frozen || voice.buffer.count() < voice.write_until) {
        voice.buffer.write(x);
      }
    }

    std::array<double, max_frames> l{};
    std::array<double, max_frames> r{};
    voice.cloud.render(voice.buffer, voice.frozen, voice.frozen ? voice.frozen_count : before,
                       l.data(), r.data(), frames);

    const auto mode = filter_mode(voice.filter_type);
    for (std::size_t i = 0; i < frames; ++i) {
      FilterCoefficients c;
      c.mode = mode;
      c.g = voice.g.next();
      c.k1 = voice.k1.next();
      c.k2 = voice.k2.next();
      c.finalise();
      StereoSample y = voice.filter.process({l[i], r[i]}, c);
      if (voice.fade.active()) {
        const StereoSample old =
            voice.fading_filter.process({l[i], r[i]}, voice.fading_coefficients);
        const double weight = voice.fade.next();
        y = y + (old - y) * weight;
      }
      const double level = voice.amp.next() * voice.gain.next();
      voice.last = y * level;
      left[i] += voice.last.left;
      right[i] += voice.last.right;
    }
    // Retire on the control grid, so it never depends on block splits.
    if (ends_tick && !voice.amp.active()) {
      voice.active = false;
      voice.held = false;
      voice.last = {};
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
  const std::array<Wavetable, wavetable_count>* tables_;
  std::unique_ptr<std::array<Voice, voice_limit>> voices_;
  std::span<const double> values_{};
  std::uint32_t frames_ = 0;
  double beats_per_frame_ = 0.0;
  SynthTransport transport_{};
  Lfo lfo1_{};
  Lfo lfo2_{};
  double lfo1_value_ = 0.0;
  double lfo2_value_ = 0.0;
  std::uint32_t next_slot_ = 0;
  std::uint64_t clock_ = 0;
  std::uint32_t steals_ = 0;
  Declick declick_;
  Chorus chorus_{};
  BbdDelay delay_{};
  ShimmerReverb reverb_{};
  Ramp out_gain_{};
  bool primed_ = false;
};

std::unique_ptr<SynthKernel> create_kernel(double sample_rate) {
  return std::make_unique<DriftKernel>(sample_rate);
}

std::uint64_t tail_frames(std::span<const double> values, double sample_rate) {
  const double delay_seconds =
      values[p::delay] > 0.0
          ? BbdDelay::tail_seconds(BbdDelay::max_seconds, values[p::delay_feedback])
          : 0.0;
  const double reverb_seconds =
      values[p::verb_mix] > 0.0 ? ShimmerReverb::tail_seconds(values[p::verb_decay]) : 0.0;
  const double seconds =
      1.15 * values[p::amp_release] + delay_seconds + reverb_seconds + Chorus::tail_seconds + 0.1;
  return static_cast<std::uint64_t>(std::ceil(seconds * sample_rate));
}

const EngineDescriptor descriptor{"drift", "Drift", 1, drift_table::parameters,
                                  drift_presets::presets};
const SynthEngine drift_engine{&descriptor, &create_kernel, &tail_frames};

} // namespace

const SynthEngine& engine() { return drift_engine; }

} // namespace oscillate::native_dsp::synth::drift
