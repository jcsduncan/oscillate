#include "oscillate/native_dsp/effects/dust.hpp"

#include "dust/age.hpp"
#include "dust/beds.hpp"
#include "dust/duck.hpp"
#include "dust/hum.hpp"
#include "dust/parts.hpp"
#include "dust/radio.hpp"
#include "dust/rain.hpp"
#include "dust/tape.hpp"
#include "dust/vinyl.hpp"
#include "dust_presets.hpp"
#include "dust_table.hpp"
#include "effect_dsp.hpp"
#include "oscillate/native_dsp/effects/expanse.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

// Dust (ADR-0069). Every layer is synthesised from its own seeded
// generator, reseeded from the seed and the project frame at every start or
// seek, so a render is the same for any block split and an export is what
// playback plays from the same point. Choices that change state (a layer
// switching off, the rain's room going quiet) are made at control ticks,
// which sit on fixed project frames. Each layer's stationary noise is
// normalised by its filters' exact noise power (the analogue prototypes,
// which the TPT filters match at prewarped frequencies), and each event
// layer (pops, drops) by its expected energy at the default settings, so a
// layer at Level 0 dB sits at layer_reference_dbfs whatever its colour.
//
// The layers live in dust/ (vinyl, tape, hum, radio, rain, and the wind,
// room and machine beds), with the input's age and the duck; this file is
// the kernel that runs them on one clock and mixes them.
namespace oscillate::native_dsp::effects::dust {
namespace {

using namespace detail;

// ---- Shared timing -------------------------------------------------------------------------

// A layer switched on or off fades over this long.
constexpr double layer_fade_seconds = native_dsp::dsp::switch_fade_seconds;
// After a start or seek the bed fades in, and the last output fades out.
constexpr double bed_fade_in_seconds = 0.01;
constexpr double declick_seconds = 0.005;
constexpr double meter_hold_seconds = 0.3;
constexpr double peak_hold_seconds = 1.0;
constexpr std::uint32_t pop_count_mask = (1U << 24U) - 1U;
// With the transport stopped, tempo-locked parts keep this tempo.
constexpr double fallback_beats_per_second = 2.0;
// The tick's beat is held to this grid, so block splits (which sum the
// beat differently) agree exactly.
constexpr double beat_grid = 16'777'216.0; // 2^24 steps a beat
// Salts that keep every generator's sequence its own.
constexpr std::uint64_t layer_salt = 0x1A7E2ULL;
constexpr std::uint64_t age_salt = 0xA6E5ULL;
constexpr std::uint64_t frame_salt = 0xF2A3E5ULL;
// The duck's meter reads no deeper than this gain (-120 dB).
constexpr double deepest_duck = 1e-6;
// The rates Dust runs at.
constexpr double lowest_rate = 8'000.0;
constexpr double highest_rate = 384'000.0;

// ---- Output ---------------------------------------------------------------------------------

// Output safety: above 0 dBFS the output bends smoothly towards this ceiling.
constexpr double safety_knee = 1.0;
constexpr double safety_ceiling = 2.0;

// ---- The rain's room ------------------------------------------------------------------------

constexpr double rain_room_size = 0.4;
constexpr double rain_room_decay_s = 1.2;
constexpr double rain_room_predelay_ms = 12.0;
constexpr double rain_room_mode = 0.0; // Room
constexpr double rain_room_wet = 1.0;  // All room: the dry rain is the layer.

std::vector<double> room_values() {
  const auto& room = expanse::engine();
  std::vector<double> values(room.descriptor->parameters.size());
  for (std::size_t i = 0; i < values.size(); ++i) {
    const auto& parameter = room.descriptor->parameters[i];
    const std::string_view id = parameter.id;
    values[i] = id == "expanse.mode"       ? rain_room_mode
                : id == "expanse.size"     ? rain_room_size
                : id == "expanse.decay"    ? rain_room_decay_s
                : id == "expanse.predelay" ? rain_room_predelay_ms
                : id == "expanse.mix"      ? rain_room_wet
                                           : parameter.default_value;
  }
  return values;
}

[[nodiscard]] std::uint32_t latency_of(double rate) noexcept {
  return static_cast<std::uint32_t>(std::lround(latency_seconds * rate));
}

[[nodiscard]] float to_db(double gain) noexcept {
  return static_cast<float>(std::max(meter_floor_db, db_per_decade * std::log10(gain + log_floor)));
}

// ---- The kernel -----------------------------------------------------------------------------

class DustKernel final : public EffectKernel {
public:
  DustKernel(std::span<const double> values, double rate)
      : rate_(rate), latency_(latency_of(rate)), reference_(gain_of(layer_reference_dbfs)) {
    const LayerFormat format{.rate = rate, .reference = reference_};
    vinyl_.prepare(format);
    tape_.prepare(format);
    hum_.prepare(format);
    radio_.prepare(format);
    rain_.prepare(format);
    wind_.prepare(format);
    room_.prepare(format);
    machine_.prepare(format);
    age_.prepare(rate, latency_);
    duck_.prepare(rate);
    fade_frames_ = layer_fade_seconds * rate;
    bed_fade_step_ = 1.0 / (bed_fade_in_seconds * rate);
    declick_frames_ = static_cast<std::uint32_t>(std::lround(declick_seconds * rate));
    meter_hold_frames_ = static_cast<std::uint32_t>(std::lround(meter_hold_seconds * rate));
    peak_hold_frames_ = static_cast<std::uint32_t>(std::lround(peak_hold_seconds * rate));
    room_values_ = room_values();
    const auto& room = expanse::engine();
    room_tail_ = room.tail_frames(room_values_, rate);
    room_kernel_ = room.create_kernel(room_values_, rate);
    (void)values;
    reset_state();
  }

  void adopt_key_notes(std::span<const KeyNote> notes) noexcept override { duck_.adopt(notes); }

  void locate(std::uint64_t project_frame) noexcept override {
    const auto frame = static_cast<std::int64_t>(project_frame);
    if (frame != frame_) {
      duck_.moved();
    }
    frame_ = frame;
    if (tick_pending_) {
      tick_frame_ = frame;
      tick_pending_ = false;
    }
    if (reseed_pending_) {
      reseed(project_frame);
      reseed_pending_ = false;
    }
  }

  void control(std::span<const double> v, std::uint32_t frames,
               const SynthTransport& transport) noexcept override {
    const bool jump = !primed_;
    const double steps = std::max(1.0, static_cast<double>(frames));
    const auto seed = static_cast<std::uint64_t>(v[p::mix_seed]);
    if (seed != seed_) {
      seed_ = seed;
      seed_key_ = splitmix64(seed_);
      vinyl_.press(seed_key_);
      radio_.tune(seed_key_);
      machine_.build(seed_key_);
      if (primed_) {
        reseed_pending_ = true;
      }
    }
    // The transport, held to a grid so any block split agrees.
    // Stopped, it keeps a fallback tempo from the project frame.
    free_running_ = !(transport.beats_per_frame > 0.0);
    beats_per_frame_ =
        free_running_ ? fallback_beats_per_second / rate_ : transport.beats_per_frame;
    tick_beat_ = std::round(transport.beat_at_block_start * beat_grid) / beat_grid;
    tick_pending_ = true;
    const double seconds_per_beat = 1.0 / (beats_per_frame_ * rate_);

    control_layers(v, steps, jump, seconds_per_beat);
    control_room(frames, transport);
    age_.set(v, steps, jump);
    duck_.set(v, steps, jump);
    input_gain_.to(gain_of(v[p::mix_input]), steps, jump);
    bed_gain_.to(gain_of(v[p::mix_bed]), steps, jump);
    output_gain_.to(gain_of(v[p::mix_output]), steps, jump);
    primed_ = true;
  }

  void process(std::span<engine::StereoFrame> audio,
               std::span<const engine::StereoFrame> sidechain) noexcept override {
    duck_.seek(frame_ - static_cast<std::int64_t>(latency_));
    for (std::size_t done = 0; done < audio.size(); done += control_interval_frames) {
      const std::size_t n = std::min<std::size_t>(control_interval_frames, audio.size() - done);
      render(audio.subspan(done, n), sidechain.empty() ? sidechain : sidechain.subspan(done, n));
    }
  }

  void reset() noexcept override {
    declick_ = last_;
    declick_left_ = declick_frames_;
    reset_state();
  }

  [[nodiscard]] std::size_t telemetry(std::span<float> out) const noexcept override {
    if (out.size() < telemetry::size) {
      return 0;
    }
    for (std::size_t l = 0; l < layer_count; ++l) {
      out[telemetry::first_layer + l] = to_db(layer_meter_[l].held);
    }
    out[telemetry::duck_db] = static_cast<float>(duck_db_);
    out[telemetry::rotation] = static_cast<float>(rotation_);
    out[telemetry::pops] = static_cast<float>(pops_ & pop_count_mask);
    out[telemetry::output_peak] = to_db(peak_meter_[0].held);
    out[telemetry::output_peak + 1] = to_db(peak_meter_[1].held);
    return telemetry::size;
  }

private:
  struct LayerState {
    dsp::Ramp on;
    dsp::Ramp level;
    bool active = false;
  };
  struct Meter {
    double held = 0.0;
    std::uint32_t left = 0;
    void add(double peak, std::uint32_t frames, std::uint32_t hold) noexcept {
      if (peak >= held || left <= frames) {
        held = peak;
        left = hold;
      } else {
        left -= frames;
      }
    }
  };
  // One chunk of the bed: the layers summed, tape's dropouts (heard on the
  // input too while Tape sounds) and each layer's peak.
  struct Bed {
    std::array<double, control_interval_frames> left{};
    std::array<double, control_interval_frames> right{};
    std::array<double, control_interval_frames> dip{};
    std::array<double, layer_count> peak{};
  };

  // Each layer fades in or out as it is switched, and while it sounds takes
  // its settings.
  void control_layers(std::span<const double> v, double steps, bool jump,
                      double seconds_per_beat) noexcept {
    for (std::size_t l = 0; l < layer_count; ++l) {
      auto& layer = layers_[l];
      const std::size_t on = t::layer_on[l];
      const double target = v[on] != 0.0 ? 1.0 : 0.0;
      layer.on.settle(target, std::max(steps, fade_frames_ * std::fabs(target - layer.on.value)),
                      jump);
      layer.level.to(gain_of(v[on + t::level_offset]), steps, jump);
      const bool sounding = target > 0.0 || layer.on.value > 0.0;
      if (!sounding && layer.active) {
        reset_layer(l);
      }
      layer.active = sounding;
      if (sounding) {
        const LayerShape shape{.tone = v[on + t::tone_offset], .width = v[on + t::width_offset]};
        switch (static_cast<Layer>(l)) {
        case Layer::vinyl:
          vinyl_.set(v, shape);
          frames_per_rev_ = vinyl_.frames_per_revolution(v);
          break;
        case Layer::tape:
          tape_.set(v, shape);
          break;
        case Layer::hum:
          hum_.set(v, shape);
          break;
        case Layer::radio:
          radio_.set(v, shape, {.steps = steps, .seconds_per_beat = seconds_per_beat});
          break;
        case Layer::rain:
          rain_.set(v, shape);
          break;
        case Layer::wind:
          wind_.set(v, shape, steps, jump);
          break;
        case Layer::room:
          room_.set(v, shape);
          break;
        case Layer::machine:
          machine_.set(v, shape);
          break;
        }
      }
    }
    if (!layers_[static_cast<std::size_t>(Layer::vinyl)].active) {
      frames_per_rev_ = vinyl_.frames_per_revolution(v);
    }
  }

  // The rain's room runs while the rain sounds and until its tail ends.
  void control_room(std::uint32_t frames, const SynthTransport& transport) noexcept {
    const bool rain = layers_[static_cast<std::size_t>(Layer::rain)].active;
    if (rain) {
      room_left_ = room_tail_;
    } else if (room_left_ > frames) {
      room_left_ -= frames;
    } else if (room_left_ > 0) {
      room_left_ = 0;
      if (room_kernel_) {
        room_kernel_->reset();
      }
    }
    room_running_ = room_kernel_ != nullptr && room_left_ > 0;
    if (room_running_) {
      room_kernel_->control(room_values_, frames, transport);
    }
  }

  void reset_layer(std::size_t l) noexcept {
    switch (static_cast<Layer>(l)) {
    case Layer::vinyl:
      vinyl_.reset();
      break;
    case Layer::tape:
      tape_.reset();
      break;
    case Layer::hum:
      hum_.reset();
      break;
    case Layer::radio:
      radio_.reset();
      break;
    case Layer::rain:
      rain_.reset();
      break;
    case Layer::wind:
      wind_.reset();
      break;
    case Layer::room:
      room_.reset();
      break;
    case Layer::machine:
      machine_.reset();
      break;
    }
  }

  void reset_state() noexcept {
    for (std::size_t l = 0; l < layer_count; ++l) {
      reset_layer(l);
    }
    age_.reset();
    if (room_kernel_) {
      room_kernel_->reset();
    }
    room_left_ = 0;
    room_running_ = false;
    duck_.reset();
    bed_fade_ = 0.0;
    reseed_pending_ = true;
  }

  // Every generator from the seed and where the transport starts.
  void reseed(std::uint64_t frame) noexcept {
    const std::uint64_t base = seed_key_ ^ splitmix64(frame ^ frame_salt);
    vinyl_.reseed(base + layer_salt * 1U);
    tape_.reseed(base + layer_salt * 2U);
    hum_.reseed(base + layer_salt * 3U);
    radio_.reseed(base + layer_salt * 4U);
    rain_.reseed(base + layer_salt * 5U);
    wind_.reseed(base + layer_salt * 6U);
    room_.reseed(base + layer_salt * 7U);
    machine_.reseed(base + layer_salt * 8U);
    age_.reseed(base ^ age_salt);
  }

  [[nodiscard]] Pair layer_next(Layer layer, std::int64_t frame, std::int64_t time, double rotation,
                                double beat) noexcept {
    switch (layer) {
    case Layer::vinyl:
      return vinyl_.next(rotation);
    case Layer::tape:
      return tape_.next();
    case Layer::hum:
      return hum_.next(static_cast<std::uint64_t>(frame));
    case Layer::radio:
      return radio_.next(beat);
    case Layer::rain:
      return rain_.next();
    case Layer::wind:
      return wind_.next();
    case Layer::room:
      return room_.next();
    case Layer::machine:
      return machine_.next(time);
    }
    return {};
  }

  // One chunk of at most a control tick.
  void render(std::span<engine::StereoFrame> audio,
              std::span<const engine::StereoFrame> sidechain) noexcept {
    Bed bed;
    render_bed(audio.size(), bed);
    pops_ += vinyl_.take_pops();
    mix(audio, sidechain, bed);
  }

  // The bed, on the compensated clock: this frame sounds at time - latency.
  void render_bed(std::size_t n, Bed& bed) noexcept {
    std::array<engine::StereoFrame, control_interval_frames> room{};
    const auto rain_index = static_cast<std::size_t>(Layer::rain);
    const auto tape_index = static_cast<std::size_t>(Layer::tape);
    for (std::size_t i = 0; i < n; ++i) {
      const std::int64_t frame = frame_ + static_cast<std::int64_t>(i);
      const std::int64_t time = frame - static_cast<std::int64_t>(latency_);
      double rotation = std::fmod(static_cast<double>(time), frames_per_rev_) / frames_per_rev_;
      if (rotation < 0.0) {
        rotation += 1.0;
      }
      rotation_ = rotation;
      const double beat =
          free_running_ ? beats_per_frame_ * static_cast<double>(time)
                        : tick_beat_ + beats_per_frame_ * static_cast<double>(time - tick_frame_);
      double left = 0.0;
      double right = 0.0;
      for (std::size_t l = 0; l < layer_count; ++l) {
        auto& layer = layers_[l];
        if (!layer.active) {
          continue;
        }
        const double g = std::max(0.0, layer.on.next()) * layer.level.next();
        const Pair y = layer_next(static_cast<Layer>(l), frame, time, rotation, beat);
        bed.peak[l] = std::max({bed.peak[l], std::fabs(y.left * g), std::fabs(y.right * g)});
        left += y.left * g;
        right += y.right * g;
        if (l == tape_index) {
          bed.dip[i] = std::max(0.0, layer.on.value) * (1.0 - tape_.envelope());
        }
        if (l == rain_index && room_running_) {
          const double feed = 0.5 * (y.left + y.right) * g * rain_.send();
          room[i] = {static_cast<float>(feed), static_cast<float>(feed)};
        }
      }
      bed.left[i] = left;
      bed.right[i] = right;
    }
    if (room_running_) {
      room_kernel_->process(std::span<engine::StereoFrame>(room.data(), n), {});
      for (std::size_t i = 0; i < n; ++i) {
        bed.left[i] += static_cast<double>(room[i].left);
        bed.right[i] += static_cast<double>(room[i].right);
      }
    }
  }

  // The aged input and the ducked bed, mixed into the output.
  void mix(std::span<engine::StereoFrame> audio, std::span<const engine::StereoFrame> sidechain,
           const Bed& bed) noexcept {
    const std::size_t n = audio.size();
    double peak_left = 0.0;
    double peak_right = 0.0;
    double most_duck = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      // The duck, for the bed only.
      const double duck = duck_.next(frame_ - static_cast<std::int64_t>(latency_),
                                     sidechain.empty() ? nullptr : &sidechain[i]);
      most_duck = std::max(most_duck, -db_per_decade * std::log10(std::max(duck, deepest_duck)));
      bed_fade_ = std::min(1.0, bed_fade_ + bed_fade_step_);
      const double bed_level = duck * bed_gain_.next() * bed_fade_;

      std::array<double, channels> x{static_cast<double>(audio[i].left),
                                     static_cast<double>(audio[i].right)};
      age_.next(x, frame_, bed.dip[i]);
      const double input = input_gain_.next();
      const double out_gain = output_gain_.next();
      double left = (x[0] * input + bed.left[i] * bed_level) * out_gain;
      double right = (x[1] * input + bed.right[i] * bed_level) * out_gain;
      if (declick_left_ > 0) {
        const double w = static_cast<double>(declick_left_) / static_cast<double>(declick_frames_);
        left += declick_.left * w;
        right += declick_.right * w;
        --declick_left_;
      }
      left = safe(left);
      right = safe(right);
      last_ = {left, right};
      peak_left = std::max(peak_left, std::fabs(left));
      peak_right = std::max(peak_right, std::fabs(right));
      audio[i].left = static_cast<float>(left);
      audio[i].right = static_cast<float>(right);
      ++frame_;
    }
    const auto frames = static_cast<std::uint32_t>(n);
    for (std::size_t l = 0; l < layer_count; ++l) {
      layer_meter_[l].add(bed.peak[l], frames, meter_hold_frames_);
    }
    peak_meter_[0].add(peak_left, frames, peak_hold_frames_);
    peak_meter_[1].add(peak_right, frames, peak_hold_frames_);
    duck_db_ = most_duck;
  }

  // Output safety: untouched to 0 dBFS, then bent smoothly towards the
  // ceiling; anything not finite is silenced.
  [[nodiscard]] static double safe(double x) noexcept {
    if (!std::isfinite(x)) {
      return 0.0;
    }
    const double a = std::fabs(x);
    if (a <= safety_knee) {
      return x;
    }
    const double room = safety_ceiling - safety_knee;
    return std::copysign(safety_knee + room * std::tanh((a - safety_knee) / room), x);
  }

  double rate_;
  std::uint32_t latency_;
  double reference_;
  Vinyl vinyl_;
  Tape tape_;
  Hum hum_;
  Radio radio_;
  Rain rain_;
  Wind wind_;
  Room room_;
  Machine machine_;
  Age age_;
  Duck duck_;
  std::array<LayerState, layer_count> layers_{};
  std::array<Meter, layer_count> layer_meter_{};
  std::array<Meter, channels> peak_meter_{};
  double duck_db_ = 0.0;

  // The rain's room.
  std::vector<double> room_values_;
  std::unique_ptr<EffectKernel> room_kernel_;
  std::uint64_t room_tail_ = 0;
  std::uint64_t room_left_ = 0;
  bool room_running_ = false;

  // Mix.
  dsp::Ramp input_gain_;
  dsp::Ramp bed_gain_;
  dsp::Ramp output_gain_;
  double bed_fade_ = 0.0;
  double bed_fade_step_ = 1.0;
  Pair last_;
  Pair declick_;
  std::uint32_t declick_frames_ = 1;
  std::uint32_t declick_left_ = 0;

  // Clock and seed.
  std::int64_t frame_ = 0;
  std::int64_t tick_frame_ = 0;
  double tick_beat_ = 0.0;
  double beats_per_frame_ = 0.0;
  bool free_running_ = false;
  bool tick_pending_ = false;
  double frames_per_rev_ = 1.0;
  double rotation_ = 0.0;
  double fade_frames_ = 1.0;
  std::uint64_t seed_ = 0;
  std::uint64_t seed_key_ = 0;
  bool reseed_pending_ = true;
  bool primed_ = false;
  std::uint64_t pops_ = 0;
  std::uint32_t meter_hold_frames_ = 1;
  std::uint32_t peak_hold_frames_ = 1;
};

std::unique_ptr<EffectKernel> create_kernel(std::span<const double> values, double sample_rate) {
  if (!std::isfinite(sample_rate) || sample_rate < lowest_rate || sample_rate > highest_rate) {
    return nullptr;
  }
  return std::make_unique<DustKernel>(values, sample_rate);
}

// A generator: its tail is the fade after it is switched off, the aged
// input's delay, and the rain's room ringing out.
std::uint64_t tail_frames(std::span<const double> values, double sample_rate) {
  std::uint64_t frames =
      static_cast<std::uint64_t>(std::ceil((layer_fade_seconds + declick_seconds) * sample_rate)) +
      std::uint64_t{2} * latency_of(sample_rate);
  if (values[t::layer_on[static_cast<std::size_t>(Layer::rain)]] != 0.0) {
    frames += expanse::engine().tail_frames(room_values(), sample_rate);
  }
  return frames;
}

std::uint32_t latency_frames(std::span<const double>, double sample_rate) {
  return latency_of(sample_rate);
}

const synth::EngineDescriptor descriptor{"dust", "Dust", 1, t::parameters, dust_presets::presets};
// A generator that reads a sidechain and is keyed by notes, as Pump is.
const EffectEngine dust_engine{&descriptor, &create_kernel, &tail_frames, &latency_frames, true,
                               true,        nullptr,        nullptr,      nullptr,         true};

} // namespace

const EffectEngine& engine() { return dust_engine; }

} // namespace oscillate::native_dsp::effects::dust
