#include "oscillate/native_dsp/synth/pulse.hpp"

#include "dsp/ramp.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/synth/drum_dsp.hpp"
#include "oscillate/native_dsp/synth/filter.hpp"
#include "oscillate/native_dsp/synth/modulation.hpp"
#include "oscillate/native_dsp/synth/oscillator.hpp"
#include "oscillate/native_dsp/synth/shaping.hpp"
#include "oscillate/native_dsp/synth/vintage_effects.hpp"
#include "oscillate/native_dsp/synth/voice_pool.hpp"
#include "pulse_presets.hpp"
#include "pulse_table.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>

namespace oscillate::native_dsp::synth::pulse {
namespace {

using pulse_table::index_of;

enum Pad : std::uint8_t {
  kick,
  kick2,
  snare,
  rim,
  clap,
  tom_low,
  chh,
  tom_mid,
  cowbell,
  tom_high,
  ohh,
  cymbal
};

namespace p {
constexpr std::size_t level = 0;
constexpr std::size_t pan = 1;
constexpr std::size_t tune = 2;
constexpr std::size_t decay = 3;
constexpr std::size_t tone = 4;
constexpr std::size_t drive = 5;
constexpr std::size_t velocity = 6;
constexpr std::size_t character = 7;
static_assert(index_of("pulse.cymbal.character") == 11 * pulse_table::pad_parameters + character);
constexpr std::size_t glue = index_of("pulse.bus.glue");
constexpr std::size_t release = index_of("pulse.bus.release");
constexpr std::size_t saturation = index_of("pulse.bus.saturation");
constexpr std::size_t out_level = index_of("pulse.out.level");
} // namespace p

constexpr double pad_gain = 0.8;

[[nodiscard]] double clamp(double value, double low, double high) noexcept {
  return value < low ? low : (value > high ? high : value);
}

using Ramp = dsp::Ramp;

// One mono pad voice; which parts run depends on the pad.
struct Voice {
  bool active = false;
  std::uint32_t hits = 0;
  double velocity = 1.0;
  double gain = 0.0; // Velocity-scaled, fixed per hit.
  double amp = 0.0;  // Main envelope.
  double amp_coefficient = 0.0;
  double body_amp = 0.0; // Snare body / cowbell fast stage.
  double body_coefficient = 0.0;
  double click = 0.0;
  double click_coefficient = 0.0;
  bool choked = false;
  ResonantBody body{};
  ResonantBody body2{};
  MetalSource metal{};
  std::array<BlepOscillator, 2> bell{};
  NoiseSource noise{};
  BurstEnvelope burst{};
  ZdfSvf band{};
  ZdfSvf high{};
  ZdfSvf low{};
  SvfCoefficients band_coefficients{};
  SvfCoefficients high_coefficients{};
  SvfCoefficients low_coefficients{};
  Ramp level{};
  Ramp pan{};
  double last_left = 0.0;
  double last_right = 0.0;
};

[[nodiscard]] SvfCoefficients svf(double hz, double q, double rate) noexcept {
  return SvfCoefficients::make(
      prewarp(clamp(hz, 20.0, native_dsp::dsp::nyquist_guard * rate) / rate), 1.0 / q);
}

class PulseKernel final : public SynthKernel {
public:
  explicit PulseKernel(double sample_rate) : rate_(sample_rate) {
    voices_ = std::make_unique<std::array<Voice, pad_count>>();
    for (auto& voice : *voices_) {
      voice.body.prepare(rate_);
      voice.body2.prepare(rate_);
      voice.burst.prepare(rate_);
    }
    glue_.prepare(rate_);
    dc_[0].prepare(rate_);
    dc_[1].prepare(rate_);
    reset_voices();
  }

  void control(std::span<const double> values, std::uint32_t frames_to_next_tick,
               const Transport&) noexcept override {
    values_ = values;
    frames_ = frames_to_next_tick;
    for (std::size_t pad = 0; pad < pad_count; ++pad) {
      auto& voice = (*voices_)[pad];
      voice.level.to(value(pad, p::level), frames_, !primed_);
      voice.pan.to(value(pad, p::pan), frames_, !primed_);
      if (voice.active) {
        // Decay and tone stay live on a ringing pad.
        update_pad(pad, voice);
      }
    }
    glue_.set(values[p::glue], values[p::release]);
    saturation_.to(values[p::saturation], frames_, !primed_);
    out_gain_.to(values[p::out_level] <= -59.9 ? 0.0 : std::pow(10.0, values[p::out_level] / 20.0),
                 frames_, !primed_);
    primed_ = true;
  }

  void note_on(const NoteEvent& event) noexcept override {
    if (values_.empty() || event.key < first_key || event.key >= first_key + pad_count) {
      return;
    }
    const std::size_t pad = event.key - first_key;
    auto& voice = (*voices_)[pad];
    if (voice.active) {
      capture_declick(voice);
    }
    if (pad == chh) {
      // The closed hat chokes a ringing open hat with a 5 ms release.
      auto& open = (*voices_)[ohh];
      if (open.active) {
        open.choked = true;
        open.amp_coefficient = decay_coefficient(0.005, rate_);
      }
    }
    trigger(pad, voice, event.velocity / 127.0);
  }

  // Drums are one-shots: note-offs are ignored.
  void note_off(const NoteEvent&) noexcept override {}

  void render(std::span<engine::StereoFrame> output) noexcept override {
    const std::size_t frames = output.size();
    Chunk left{};
    Chunk right{};
    for (std::size_t pad = 0; pad < pad_count; ++pad) {
      auto& voice = (*voices_)[pad];
      if (voice.active) {
        render_pad(pad, voice, left, right, frames);
      } else {
        voice.level.value += voice.level.step * static_cast<double>(frames);
        voice.pan.value += voice.pan.step * static_cast<double>(frames);
      }
    }
    for (std::size_t i = 0; i < frames; ++i) {
      StereoSample frame{left[i], right[i]};
      declick_.add(frame);
      double l = frame.left;
      double r = frame.right;
      const auto glued = glue_.process(l, r);
      const double saturation = saturation_.next();
      double sl = glued.left;
      double sr = glued.right;
      if (saturation > 0.0) {
        const double drive = 1.0 + 4.0 * saturation;
        const double makeup = (1.0 + std::log(drive)) / drive;
        sl = soft_clip(sl * drive) * makeup;
        sr = soft_clip(sr * drive) * makeup;
      }
      const double gain = out_gain_.next();
      output[i].left += static_cast<float>(safety(dc_[0].process(sl) * gain));
      output[i].right += static_cast<float>(safety(dc_[1].process(sr) * gain));
    }
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
    out[telemetry::gain_reduction] = static_cast<float>(glue_.gain_reduction_db());
    for (std::size_t pad = 0; pad < pad_count; ++pad) {
      const auto& voice = (*voices_)[pad];
      out[telemetry::first_pad + pad] = voice.active ? static_cast<float>(voice.amp) : 0.0F;
    }
    return telemetry::size;
  }

  [[nodiscard]] KernelStatus status() const noexcept override {
    KernelStatus status;
    for (const auto& voice : *voices_) {
      status.active_voices += voice.active ? 1U : 0U;
    }
    return status;
  }

private:
  using Chunk = std::array<double, control_interval_frames>;

  [[nodiscard]] double value(std::size_t pad, std::size_t parameter) const noexcept {
    return values_[pad * pulse_table::pad_parameters + parameter];
  }
  [[nodiscard]] double tune_scale(std::size_t pad) const noexcept {
    return std::exp2(value(pad, p::tune) / 12.0);
  }

  void reset_voices() noexcept {
    for (std::size_t pad = 0; pad < pad_count; ++pad) {
      auto& voice = (*voices_)[pad];
      voice.active = false;
      voice.hits = 0;
      voice.metal.reset();
    }
  }

  void capture_declick(const Voice& voice) noexcept {
    declick_.capture(voice.last_left, voice.last_right);
  }

  // A pad's body pitches from the current Tune and Tone: the sweep's start
  // and end, and the snare's second body. Pads without a body read zero.
  struct BodyTuning {
    double start = 0.0;
    double end = 0.0;
    double second = 0.0;
  };
  [[nodiscard]] BodyTuning body_tuning(std::size_t pad) const noexcept {
    const double scale = tune_scale(pad);
    const double tone = value(pad, p::tone);
    switch (pad) {
    case kick: {
      const double end = 50.0 * scale;
      return {end * (1.0 + 6.0 * tone), end, 0.0};
    }
    case kick2: {
      const double end = 55.0 * scale;
      return {end * (1.0 + 10.0 * tone), end, 0.0};
    }
    case snare:
      return {185.0 * scale * 1.3, 185.0 * scale, 330.0 * scale};
    case rim:
      return {1'650.0 * scale, 1'650.0 * scale, 0.0};
    case tom_low:
    case tom_mid:
    case tom_high: {
      const double base = pad == tom_low ? 90.0 : (pad == tom_mid ? 135.0 : 200.0);
      return {base * scale * (1.0 + 0.8 * tone), base * scale, 0.0};
    }
    default:
      return {};
    }
  }

  // Envelope rates and filters from the current panel values; on a ringing
  // pad its body follows Tune and Tone and a clap's tail follows Decay.
  void update_pad(std::size_t pad, Voice& voice) noexcept {
    const double decay = value(pad, p::decay);
    const double tone = value(pad, p::tone);
    const double scale = tune_scale(pad);
    // Velocity brightens the filters a little when Velocity is up.
    const double bright = 1.0 + 0.4 * value(pad, p::velocity) * (voice.velocity - 0.7);
    if (!voice.choked) {
      voice.amp_coefficient = decay_coefficient(decay, rate_);
      if (pad == clap)
        voice.burst.set_tail(decay);
    }
    if (const auto tuning = body_tuning(pad); tuning.end > 0.0) {
      voice.body.retune(tuning.start, tuning.end);
      if (pad == snare)
        voice.body2.retune(tuning.second, tuning.second);
    }
    switch (pad) {
    case snare:
      voice.body_coefficient = decay_coefficient(0.5 * decay, rate_);
      // The snares' noise falls at 24 dB/oct past its tone: snap, not fizz.
      voice.band_coefficients = svf((4'000.0 + 8'000.0 * tone) * bright, 0.54, rate_);
      voice.low_coefficients = svf((4'000.0 + 8'000.0 * tone) * bright, 1.31, rate_);
      voice.high_coefficients = svf(1'200.0, 0.7, rate_);
      break;
    case rim:
      voice.high_coefficients = svf(600.0, 0.7, rate_);
      break;
    case clap:
      voice.band_coefficients = svf(1'100.0 * scale * (0.7 + 0.6 * tone) * bright, 1.5, rate_);
      // A band-pass skirt falls only 6 dB/oct: close the top above the clap.
      voice.low_coefficients = svf(5'000.0 * scale * (0.7 + 0.6 * tone) * bright, 0.7, rate_);
      break;
    case chh:
    case ohh:
      voice.band_coefficients = svf(7'000.0 * scale * (0.6 + 0.8 * tone) * bright, 1.0, rate_);
      voice.high_coefficients = svf(6'500.0 * scale, 0.7, rate_);
      break;
    case cymbal:
      voice.band_coefficients = svf(4'500.0 * scale * (0.6 + 0.8 * tone) * bright, 0.8, rate_);
      voice.high_coefficients = svf(3'000.0 * scale, 0.7, rate_);
      break;
    case cowbell:
      voice.body_coefficient = decay_coefficient(0.04, rate_);
      voice.band_coefficients = svf(2'640.0 * scale * (0.8 + 0.4 * tone), 2.0, rate_);
      break;
    // A tom's skin noise sits in a 300 Hz-4 kHz band and fades three times
    // faster than its tone, so it colours the hit without hissing under it.
    case tom_low:
    case tom_mid:
    case tom_high:
      voice.body_coefficient = decay_coefficient(decay / 3.0, rate_);
      voice.high_coefficients = svf(300.0, 0.7, rate_);
      voice.band_coefficients = svf(4'000.0, 0.7, rate_);
      break;
    // The kicks' beater click is noise in a knock band, never full-band
    // hiss: 1.5-6 kHz for the 808, a brighter 2-9 kHz for the 909.
    case kick:
      voice.high_coefficients = svf(1'500.0, 0.7, rate_);
      voice.band_coefficients = svf(6'000.0, 0.7, rate_);
      break;
    case kick2:
      voice.high_coefficients = svf(2'000.0, 0.7, rate_);
      voice.band_coefficients = svf(9'000.0, 0.7, rate_);
      break;
    default:
      break;
    }
  }

  void trigger(std::size_t pad, Voice& voice, double velocity) noexcept {
    voice.active = true;
    voice.choked = false;
    voice.velocity = velocity;
    const double sensitivity = value(pad, p::velocity);
    voice.gain = pad_gain * (1.0 - sensitivity + sensitivity * std::pow(velocity, 1.5));
    voice.amp = 1.0;
    voice.body_amp = 1.0;
    voice.click = value(pad, p::character) * velocity;
    voice.click_coefficient = decay_coefficient(0.003, rate_);
    voice.band.reset();
    voice.high.reset();
    voice.low.reset();
    // Noise is seeded by pad and hit since the last reset: deterministic.
    voice.noise.reset(static_cast<std::uint32_t>(
        pad * 7'919U + static_cast<std::size_t>(voice.hits * 104'729U) + 1U));
    ++voice.hits;
    const auto tuning = body_tuning(pad);
    switch (pad) {
    case kick:
      voice.body.trigger(tuning.start, tuning.end, 0.03);
      break;
    case kick2:
      voice.body.trigger(tuning.start, tuning.end, 0.012);
      break;
    case snare:
      voice.body.trigger(tuning.start, tuning.end, 0.01);
      voice.body2.trigger(tuning.second, tuning.second, 0.01);
      break;
    case rim:
      voice.body.trigger(tuning.start, tuning.end, 0.01);
      break;
    case clap:
      // Bursts 5-11 ms apart (8 ms at the default character), as on the
      // hardware: the last lands within about 25 ms of the beat.
      voice.burst.trigger(0.005 + 0.006 * value(pad, p::character), 0.006, value(pad, p::decay));
      break;
    case tom_low:
    case tom_mid:
    case tom_high:
      voice.body.trigger(tuning.start, tuning.end, 0.06);
      break;
    case cowbell:
      voice.bell[0].reset(0.0, Waveform::pulse, 0.5);
      voice.bell[1].reset(0.0, Waveform::pulse, 0.5);
      break;
    default:
      break;
    }
    update_pad(pad, voice);
  }

  [[nodiscard]] double pad_sample(std::size_t pad, Voice& voice) noexcept {
    const double character = value(pad, p::character);
    double x = 0.0;
    switch (pad) {
    case kick:
    case kick2: {
      const double knock = voice.high.process(voice.noise.white(), voice.high_coefficients).high;
      x = voice.body.next() * voice.amp +
          voice.click * voice.band.process(knock, voice.band_coefficients).low;
      break;
    }
    case snare: {
      const double body = 0.6 * voice.body.next() + 0.4 * voice.body2.next();
      const double hissed = voice.high.process(voice.noise.white(), voice.high_coefficients).high;
      const double noise = voice.low
                               .process(voice.band.process(hissed, voice.band_coefficients).low,
                                        voice.low_coefficients)
                               .low;
      x = body * voice.body_amp * (1.0 - 0.5 * character) + noise * voice.amp * (0.3 + character);
      voice.body_amp *= voice.body_coefficient;
      break;
    }
    case rim:
      x = voice.high
              .process(voice.body.next() * voice.amp + voice.click * voice.noise.white(),
                       voice.high_coefficients)
              .high;
      break;
    case clap: {
      const double envelope = voice.burst.next();
      const double clapped = voice.band.process(voice.noise.white(), voice.band_coefficients).band;
      x = 2.0 * voice.low.process(clapped, voice.low_coefficients).low * envelope;
      voice.amp = voice.burst.active() ? std::max(envelope, 1e-4) : 0.0;
      break;
    }
    case tom_low:
    case tom_mid:
    case tom_high: {
      const double skin = voice.high.process(voice.noise.white(), voice.high_coefficients).high;
      x = voice.body.next() * voice.amp +
          0.3 * character * voice.band.process(skin, voice.band_coefficients).low * voice.body_amp;
      voice.body_amp *= voice.body_coefficient;
      break;
    }
    case chh:
    case ohh:
    case cymbal: {
      const double source = voice.metal.next(tune_scale(pad), rate_) * (1.0 - 0.5 * character) +
                            0.5 * character * voice.noise.white();
      const double band = voice.band.process(source, voice.band_coefficients).band;
      x = 3.0 * voice.high.process(band, voice.high_coefficients).high * voice.amp;
      break;
    }
    case cowbell: {
      const double scale = tune_scale(pad);
      const double source = voice.bell[0].next(540.0 * scale / rate_, 0.5, {}, nullptr) +
                            voice.bell[1].next(800.0 * scale / rate_, 0.5, {}, nullptr);
      const double shaped = voice.band.process(source, voice.band_coefficients).band;
      x = 1.5 * shaped * (0.6 * voice.body_amp + 0.4 * voice.amp) * (0.6 + 0.8 * character);
      voice.body_amp *= voice.body_coefficient;
      break;
    }
    default:
      break;
    }
    if (pad != clap) {
      voice.amp *= voice.amp_coefficient;
    }
    voice.click *= voice.click_coefficient;
    return x;
  }

  void render_pad(std::size_t pad, Voice& voice, Chunk& left, Chunk& right,
                  std::size_t frames) noexcept {
    const double drive_amount = value(pad, p::drive);
    const double drive = 1.0 + 9.0 * drive_amount;
    const double makeup = (1.0 + std::log(drive)) / drive;
    for (std::size_t i = 0; i < frames; ++i) {
      double x = pad_sample(pad, voice) * voice.gain;
      if (drive_amount > 0.0) {
        x = soft_clip(x * drive) * makeup;
      }
      const double level = voice.level.next();
      const double pan = voice.pan.next();
      voice.last_left = x * level * std::cos((pan + 1.0) * std::numbers::pi / 4.0);
      voice.last_right = x * level * std::sin((pan + 1.0) * std::numbers::pi / 4.0);
      left[i] += voice.last_left;
      right[i] += voice.last_right;
    }
    // A pad ends once its envelopes fall below -100 dB.
    const bool body_quiet = voice.body_amp < 1e-5 || (pad != snare && pad != cowbell);
    if (voice.amp < 1e-5 && body_quiet && voice.click < 1e-5) {
      voice.active = false;
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
  std::unique_ptr<std::array<Voice, pad_count>> voices_;
  std::span<const double> values_{};
  std::uint32_t frames_ = 0;
  Declick declick_;
  GlueCompressor glue_{};
  Ramp saturation_{};
  Ramp out_gain_{};
  bool primed_ = false;
  std::array<DcBlocker, 2> dc_{};
};

std::unique_ptr<SynthKernel> create_kernel(double sample_rate) {
  return std::make_unique<PulseKernel>(sample_rate);
}

std::uint64_t tail_frames(std::span<const double> values, double sample_rate) {
  // The longest pad decays to -100 dB in 5/3 of its -60 dB time; the clap
  // adds its bursts; the bus adds the compressor release.
  double longest = 0.0;
  for (std::size_t pad = 0; pad < pad_count; ++pad) {
    longest = std::max(longest, values[pad * pulse_table::pad_parameters + p::decay]);
  }
  const double seconds = longest * 5.0 / 3.0 + 0.04 + values[p::release] + 0.05;
  return static_cast<std::uint64_t>(std::ceil(seconds * sample_rate)) +
         BlepOscillator::latency_frames;
}

const EngineDescriptor descriptor{"pulse", "Pulse", 1, pulse_table::parameters,
                                  pulse_presets::presets};
const SynthEngine pulse_engine{&descriptor, &create_kernel, &tail_frames};

} // namespace

const SynthEngine& engine() { return pulse_engine; }

} // namespace oscillate::native_dsp::synth::pulse
