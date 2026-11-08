#include "oscillate/native_dsp/synth/concert.hpp"

#include "concert_library.hpp"
#include "concert_presets.hpp"
#include "concert_table.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/synth/sample_stream.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>
#include <vector>

namespace oscillate::native_dsp::synth::concert {
namespace {

using concert_table::index_of;
using detail::Library;
using detail::Sample;
using sample_stream::SincTable;

namespace p {
constexpr std::size_t tone = index_of("concert.tone");
constexpr std::size_t touch = index_of("concert.touch");
constexpr std::size_t dynamics = index_of("concert.dynamics");
constexpr std::size_t damper = index_of("concert.damper");
constexpr std::size_t una_corda = index_of("concert.una_corda");
constexpr std::size_t resonance = index_of("concert.resonance");
constexpr std::size_t release_noise = index_of("concert.noise.release");
constexpr std::size_t string_release = index_of("concert.noise.strings");
constexpr std::size_t pedal_noise = index_of("concert.noise.pedal");
constexpr std::size_t width = index_of("concert.width");
constexpr std::size_t perspective = index_of("concert.perspective");
constexpr std::size_t tune = index_of("concert.tune");
constexpr std::size_t level = index_of("concert.level");
} // namespace p

constexpr std::size_t max_frames = control_interval_frames;
// Every key sounding two layers, with room for retriggered notes fading.
constexpr std::size_t max_note_voices = 160;
constexpr std::size_t max_shots = 48;
constexpr std::size_t max_held = 128;

constexpr double mute_db = -59.9;
constexpr double safety_knee = 0.9;

// Interpolation: sample_stream's Kaiser-windowed sinc. A sample serves keys
// up to library::max_repitch_semitones from its root and Tune adds its
// range: the table for rising notes cuts below the band the steepest step
// leaves.
constexpr std::size_t sinc_taps = SincTable::taps;
constexpr std::ptrdiff_t sinc_left = SincTable::left;
constexpr double sinc_passband = SincTable::passband;
constexpr double cents_per_semitone = 100.0;
constexpr double max_step_semitones =
    library::max_repitch_semitones +
    concert_table::parameters[p::tune].maximum / cents_per_semitone;
constexpr std::size_t scratch_frames = 256;

// Voices.
constexpr double retrigger_fade_seconds = 0.1;
constexpr double silence = 1e-5;              // -100 dB: a voice this quiet ends.
constexpr double silence_per_t60 = 5.0 / 3.0; // T60s to reach `silence`.
constexpr double min_layer_weight = 1e-3;
constexpr double declick_seconds = 0.0015;
constexpr double max_velocity = 127.0;
// The SFZ velocity curve for the mechanism samples' amp_veltrack.
constexpr double velocity_curve = 2.0;
// Mechanism levels: 0.5 is the library's own voicing (concert_table).
constexpr double level_parameter_gain = 1.0 / concert_table::library_level;

// Dampers: they clear the strings in 5 ms and land in 20 ms. Half-pedal:
// the dampers start to lift at a fifth of the pedal's travel and clear by
// four fifths. A damped string's added decay, by register.
constexpr double damper_lift_seconds = 0.005;
constexpr double damper_fall_seconds = 0.02;
constexpr double pedal_engage = 0.2;
constexpr double pedal_clear = 0.8;
constexpr double pedal_down = 0.5; // Noises, sostenuto and damper landings.
struct RegisterPoint {
  int key;
  double seconds;
};
constexpr std::array damper_t60{RegisterPoint{first_key, 1.0}, RegisterPoint{60, 0.4},
                                RegisterPoint{88, 0.15}, RegisterPoint{last_key, 0.15}};

// Una corda: lower layers, a little quieter, and a darker blow.
constexpr double soft_velocity_shift = 12.0;
constexpr double soft_gain_db = -1.5;
constexpr double soft_shelf_db = -6.0;
constexpr double soft_shelf_hz = 1'200.0;

// Tone: a first-order tilt about this frequency, half the dB each side.
constexpr double tone_pivot_hz = 700.0;

// Sympathetic resonance: a tuned string loop per key, free while its
// damper is up, excited by everything that sounds.
constexpr std::array resonance_t60{RegisterPoint{first_key, 8.0}, RegisterPoint{last_key, 1.5}};
constexpr double resonance_damped_t60 = 0.08;
constexpr double resonance_gain_db = -12.0;
constexpr double resonance_loop_coefficient = 0.6; // The loop's one-pole lowpass.
constexpr double resonance_spread = 0.7;           // Bass left, treble right.
constexpr double resonance_awake = 1e-7;           // Mean square that keeps a loop running.

// Perspective switches through a 20 ms crossfade.
constexpr double mirror_seconds = native_dsp::dsp::switch_fade_seconds;
// Telemetry reference: the mean square of a mezzo-forte note (-20 dBFS RMS).
constexpr double telemetry_reference = 0.01;
constexpr double tail_margin_seconds = 0.25;

using native_dsp::dsp::db_to_gain;

[[nodiscard]] double key_hz(int key) noexcept {
  constexpr int a4 = 69;
  constexpr double a4_hz = 440.0;
  return a4_hz * std::exp2(static_cast<double>(key - a4) / 12.0);
}

// Log-interpolated between register points.
template <std::size_t N>
[[nodiscard]] double by_register(const std::array<RegisterPoint, N>& points, int key) noexcept {
  if (key <= points.front().key) {
    return points.front().seconds;
  }
  for (std::size_t i = 1; i < N; ++i) {
    if (key <= points[i].key) {
      const double t = static_cast<double>(key - points[i - 1].key) /
                       static_cast<double>(points[i].key - points[i - 1].key);
      return points[i - 1].seconds * std::pow(points[i].seconds / points[i - 1].seconds, t);
    }
  }
  return points.back().seconds;
}

[[nodiscard]] double pedal_lift(double pedal) noexcept {
  const double t = std::clamp((pedal - pedal_engage) / (pedal_clear - pedal_engage), 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}

[[nodiscard]] double velocity_gain(double velocity, double track) noexcept {
  return (1.0 - track) + track * std::pow(velocity / max_velocity, velocity_curve);
}

// Transparent below -1 dBFS, then a smooth knee that never exceeds 0 dBFS.
[[nodiscard]] double safety(double x) noexcept {
  const double magnitude = std::fabs(x);
  if (magnitude <= safety_knee) {
    return x;
  }
  const double over = (magnitude - safety_knee) / (1.0 - safety_knee);
  const double shaped = safety_knee + (1.0 - safety_knee) * std::tanh(over);
  return x < 0.0 ? -shaped : shaped;
}

struct Voice {
  bool active = false;
  bool note = false; // Notes follow their key's damper; one-shots do not.
  int key = 0;
  const sample_stream::Sample* sample = nullptr; // Its place in the pack.
  const std::int16_t* resident = nullptr;
  double semitones = 0.0; // From the sample's root.
  double position = 0.0;  // Source frames.
  double step = 1.0;
  double base_gain = 0.0;
  double dynamics_db = 0.0; // Peak below the key's loudest layer.
  std::size_t level = 0;    // One-shots: the parameter that scales them.
  double gain = 0.0;
  double env = 1.0;
  double env_step = 1.0;
  double fade = 1.0;
  double fade_step = 0.0;
  double shelf = 1.0; // High-frequency gain of the una corda blow.
  double shelf_coefficient = 0.0;
  double low_left = 0.0;
  double low_right = 0.0;
  std::uint32_t streamed = 0; // Stream frames on hand.
  std::uint32_t released = 0; // Stream frames handed back.
  std::uint64_t started = 0;
  double mean_square = 0.0;
};

struct Key {
  std::uint32_t holds = 0;
  bool captured = false;
  double lift = 0.0; // 0 resting .. 1 clear of the strings.
  double target = 0.0;
  double env_step = 1.0;
  std::uint8_t velocity = 0;
  std::uint64_t struck = 0;
};

struct Resonator {
  std::vector<float> line;
  std::size_t write = 0;
  std::size_t whole = 1; // The loop delay's whole frames...
  double fraction = 0.0; // ...and the rest.
  double free = 0.0;     // Loop gain with the damper up...
  double damped = 0.0;   // ...and down.
  double feedback = 0.0;
  double input = 0.0; // Normalises the loop's peak gain to one.
  double low = 0.0;
  double pan_left = 0.0;
  double pan_right = 0.0;
  // The loop's mean square over the last control interval, summed as it
  // renders and settled at each tick, so a block split never changes when
  // a string goes to sleep.
  double mean_square = 0.0;
  double square_sum = 0.0;
  std::size_t square_frames = 0;
  bool awake = false;
};

class ConcertKernel final : public SynthKernel {
public:
  ConcertKernel(double sample_rate, library::State state)
      : rate_(sample_rate), state_(state),
        streams_(std::make_unique<sample_stream::Streams>(detail::registry(), detail::pack_channels,
                                                          max_note_voices)),
        rate_ratio_(static_cast<double>(library::pack_rate) / sample_rate), falling_(sinc_passband),
        rising_((std::min(sinc_passband,
                          sinc_passband / (rate_ratio_ * std::exp2(max_step_semitones / 12.0))))) {
    tick_seconds_ = static_cast<double>(control_interval_frames) / sample_rate;
    tone_coefficient_ = native_dsp::dsp::one_pole_coefficient(tone_pivot_hz, sample_rate);
    shelf_coefficient_ = native_dsp::dsp::one_pole_coefficient(soft_shelf_hz, sample_rate);
    mirror_step_ = 1.0 / (mirror_seconds * sample_rate);
    declick_frames_ = std::max(1.0, declick_seconds * sample_rate);
    for (std::size_t k = 0; k < key_count; ++k) {
      const int key = first_key + static_cast<int>(k);
      auto& r = resonators_[k];
      const double period = sample_rate / key_hz(key);
      // The loop's lowpass delays low frequencies by (1 - c) / c frames.
      const double delay =
          std::max(2.0, period - (1.0 - resonance_loop_coefficient) / resonance_loop_coefficient);
      r.whole = static_cast<std::size_t>(delay);
      r.fraction = delay - static_cast<double>(r.whole);
      r.line.assign(r.whole + 2, 0.0F);
      const double f0 = key_hz(key);
      r.free = std::pow(10.0, -3.0 / (f0 * by_register(resonance_t60, key)));
      r.damped = std::pow(10.0, -3.0 / (f0 * resonance_damped_t60));
      r.input = 1.0 - r.free;
      const double pan = resonance_spread *
                         (2.0 * static_cast<double>(k) / static_cast<double>(key_count - 1) - 1.0);
      r.pan_left = std::sqrt(0.5 * (1.0 - pan));
      r.pan_right = std::sqrt(0.5 * (1.0 + pan));
    }
    library_ = detail::current();
    if (library_ != nullptr) {
      state_ = library::State::ready;
    }
  }

  void control(std::span<const double> values, std::uint32_t frames_to_next_tick,
               const Transport& /*transport*/) noexcept override {
    values_ = values;
    if (active_notes_ == 0 && active_shots_ == 0) {
      if (const auto* latest = detail::current(); latest != library_ && latest != nullptr) {
        library_ = latest;
        state_ = library::State::ready;
      }
    }
    update_dampers();
    update_voices();
    update_resonators();
    const double gain =
        values[p::level] <= mute_db ? 0.0 : db_to_gain(values[p::level] + headroom_db);
    gain_step_ = primed_ ? (gain - gain_) / std::max<double>(1.0, frames_to_next_tick) : 0.0;
    if (!primed_) {
      gain_ = gain;
      mirror_ = values[p::perspective];
    }
    const double half_tilt = 0.5 * values[p::tone];
    tone_low_ = db_to_gain(-half_tilt);
    tone_high_ = db_to_gain(half_tilt);
    primed_ = true;
  }

  void note_on(const NoteEvent& event) noexcept override {
    if (values_.empty() || event.key < first_key || event.key > last_key) {
      return;
    }
    if (held_count_ < max_held) {
      held_[held_count_++] = {event.clip, event.note, event.key};
    }
    auto& key = keys_[static_cast<std::size_t>(event.key - first_key)];
    ++key.holds;
    key.velocity = event.velocity;
    key.struck = clock_;
    // The damper is already clear as the hammer lands.
    key.target = 1.0;
    key.lift = 1.0;
    key.env_step = 1.0;
    if (library_ == nullptr) {
      return;
    }
    // The string's earlier sound gives way to the new blow.
    for (auto& voice : notes_) {
      if (voice.active && voice.key == event.key && voice.fade_step == 0.0) {
        voice.fade_step = 1.0 / (retrigger_fade_seconds * rate_);
      }
    }
    const double velocity = std::clamp(static_cast<double>(event.velocity), 1.0, max_velocity);
    const double touched = max_velocity * std::pow(velocity / max_velocity, values_[p::touch]);
    const double soft = soft_ * values_[p::una_corda];
    const double layered = std::max(1.0, touched - soft * soft_velocity_shift);
    const auto layers = library_->notes(event.key);
    const auto centre = [&](std::size_t i) {
      const auto& s = library_->sample(layers[i]);
      return 0.5 * static_cast<double>(s.low_velocity + s.high_velocity);
    };
    // Between the two layers whose centres surround the velocity, weighted
    // linearly: the layers' attacks are aligned and largely correlated, so
    // equal-gain weights keep the level rising smoothly.
    std::size_t lower = 0;
    double upper_weight = 0.0;
    if (layered >= centre(layers.size() - 1)) {
      lower = layers.size() - 1;
    } else if (layered > centre(0)) {
      while (lower + 1 < layers.size() && centre(lower + 1) <= layered) {
        ++lower;
      }
      upper_weight = (layered - centre(lower)) / (centre(lower + 1) - centre(lower));
    }
    const double top_db = library_->sample(layers.back()).peak_db;
    const double blow = db_to_gain(soft * soft_gain_db);
    const double shelf = db_to_gain(soft * soft_shelf_db);
    if (1.0 - upper_weight >= min_layer_weight) {
      start_note(layers[lower], event.key, (1.0 - upper_weight) * blow, top_db, shelf);
    }
    if (upper_weight >= min_layer_weight) {
      start_note(layers[lower + 1], event.key, upper_weight * blow, top_db, shelf);
    }
  }

  void note_off(const NoteEvent& event) noexcept override {
    for (std::size_t i = 0; i < held_count_; ++i) {
      if (held_[i].clip == event.clip && held_[i].note == event.note) {
        const int key_number = held_[i].key;
        auto& key = keys_[static_cast<std::size_t>(key_number - first_key)];
        key.holds = key.holds > 0 ? key.holds - 1 : 0;
        if (key.holds == 0 && library_ != nullptr) {
          // The key and hammer settling back: heard whatever the pedals do.
          const double held = static_cast<double>(clock_ - key.struck) / rate_;
          for (const auto index : library_->releases(key_number)) {
            start_shot(index, key_number, key.velocity, held, p::release_noise);
          }
        }
        held_[i] = held_[--held_count_];
        return;
      }
    }
  }

  void pedal(project::MidiPedal pedal, double value) noexcept override {
    value = std::clamp(value, 0.0, 1.0);
    switch (pedal) {
    case project::MidiPedal::sustain: {
      const bool was_down = sustain_ >= pedal_down;
      const bool down = value >= pedal_down;
      sustain_ = value;
      if (down != was_down && library_ != nullptr) {
        const auto noises = library_->pedal(down);
        if (!noises.empty()) {
          auto& turn = down ? pedal_down_turn_ : pedal_up_turn_;
          start_shot(noises[turn++ % noises.size()], 0, max_velocity, 0.0, p::pedal_noise);
        }
      }
      break;
    }
    case project::MidiPedal::sostenuto:
      if (value >= pedal_down && sostenuto_ < pedal_down) {
        for (auto& key : keys_) {
          key.captured = key.holds > 0;
        }
      } else if (value < pedal_down) {
        for (auto& key : keys_) {
          key.captured = false;
        }
      }
      sostenuto_ = value;
      break;
    case project::MidiPedal::soft:
      soft_ = value;
      break;
    }
  }

  void render(std::span<engine::StereoFrame> output) noexcept override {
    const std::size_t frames = std::min(output.size(), max_frames);
    std::array<double, max_frames> left{};
    std::array<double, max_frames> right{};
    bool starved = false;
    for (std::size_t slot = 0; slot < notes_.size(); ++slot) {
      if (notes_[slot].active) {
        starved = render_voice(notes_[slot], slot, left, right, frames) || starved;
      }
    }
    for (auto& shot : shots_) {
      if (shot.active) {
        (void)render_voice(shot, max_note_voices, left, right, frames);
      }
    }
    if (starved) {
      ++underruns_;
    }
    render_resonance(left, right, frames);
    const double width = values_.empty() ? 1.0 : values_[p::width];
    const double mirror_target = values_.empty() ? 0.0 : values_[p::perspective];
    for (std::size_t i = 0; i < frames; ++i) {
      // Tone tilt.
      tone_left_ += tone_coefficient_ * (left[i] - tone_left_);
      tone_right_ += tone_coefficient_ * (right[i] - tone_right_);
      double l = tone_left_ * tone_low_ + (left[i] - tone_left_) * tone_high_;
      double r = tone_right_ * tone_low_ + (right[i] - tone_right_) * tone_high_;
      // Width, then the perspective's mirror.
      const double mid = 0.5 * (l + r);
      const double side = 0.5 * (l - r) * width;
      l = mid + side;
      r = mid - side;
      mirror_ = mirror_ < mirror_target ? std::min(mirror_target, mirror_ + mirror_step_)
                                        : std::max(mirror_target, mirror_ - mirror_step_);
      const double ml = (1.0 - mirror_) * l + mirror_ * r;
      const double mr = (1.0 - mirror_) * r + mirror_ * l;
      gain_ += gain_step_;
      double out_left = ml * gain_;
      double out_right = mr * gain_;
      if (declick_remaining_ > 0.0) {
        const double t = declick_remaining_ / declick_frames_;
        out_left += t * declick_left_;
        out_right += t * declick_right_;
        declick_remaining_ = std::max(0.0, declick_remaining_ - 1.0);
      }
      last_left_ = out_left;
      last_right_ = out_right;
      output[i].left += static_cast<float>(safety(out_left));
      output[i].right += static_cast<float>(safety(out_right));
    }
    clock_ += frames;
  }

  void reset() noexcept override {
    for (std::size_t slot = 0; slot < notes_.size(); ++slot) {
      if (notes_[slot].active) {
        free_note(slot);
      }
    }
    for (auto& shot : shots_) {
      if (shot.active) {
        shot.active = false;
        --active_shots_;
      }
    }
    for (auto& key : keys_) {
      key = Key{};
    }
    for (auto& r : resonators_) {
      std::ranges::fill(r.line, 0.0F);
      r.low = 0.0;
      r.mean_square = 0.0;
      r.square_sum = 0.0;
      r.square_frames = 0;
      r.awake = false;
    }
    held_count_ = 0;
    sustain_ = 0.0;
    sostenuto_ = 0.0;
    soft_ = 0.0;
    tone_left_ = 0.0;
    tone_right_ = 0.0;
    declick_left_ = last_left_;
    declick_right_ = last_right_;
    declick_remaining_ = declick_frames_;
  }

  [[nodiscard]] KernelStatus status() const noexcept override {
    KernelStatus status;
    status.active_voices = static_cast<std::uint32_t>(active_notes_ + active_shots_);
    status.voice_steals = steals_;
    return status;
  }

  [[nodiscard]] std::size_t telemetry(std::span<float> out) const noexcept override {
    if (out.size() < telemetry::size) {
      return 0;
    }
    out[telemetry::sustain] = static_cast<float>(sustain_);
    out[telemetry::sostenuto] = static_cast<float>(sostenuto_);
    out[telemetry::soft] = static_cast<float>(soft_);
    std::array<double, key_count> energy{};
    for (const auto& voice : notes_) {
      if (voice.active) {
        energy[static_cast<std::size_t>(voice.key - first_key)] += voice.mean_square;
      }
    }
    const double resonance_gain = resonance_level();
    for (std::size_t k = 0; k < key_count; ++k) {
      const double total = energy[k] + resonators_[k].mean_square * resonance_gain * resonance_gain;
      const double level = total > 0.0 ? std::log10(total / telemetry_reference) : 0.0;
      out[telemetry::first_energy + k] =
          total > 0.0 ? std::max(telemetry::energy_floor, static_cast<float>(level))
                      : telemetry::energy_floor;
      out[telemetry::first_damper + k] = static_cast<float>(1.0 - keys_[k].lift);
    }
    out[telemetry::library] = static_cast<float>(static_cast<int>(state_));
    out[telemetry::underruns] = static_cast<float>(underruns_);
    return telemetry::size;
  }

  void await_content() noexcept override { streams_->service(); }

private:
  struct Held {
    foundation::IdBits clip;
    foundation::IdBits note;
    std::uint8_t key = 0;
  };

  [[nodiscard]] double resonance_level() const noexcept {
    return values_.empty() ? 0.0 : values_[p::resonance] * db_to_gain(resonance_gain_db);
  }

  [[nodiscard]] double step_for(double semitones) const noexcept {
    const double cents = values_.empty() ? 0.0 : values_[p::tune];
    return rate_ratio_ * std::exp2((semitones + cents / cents_per_semitone) / 12.0);
  }

  void start_note(std::uint32_t index, int key, double weight, double top_db,
                  double shelf) noexcept {
    std::size_t slot = notes_.size();
    for (std::size_t i = 0; i < notes_.size(); ++i) {
      if (!notes_[i].active) {
        slot = i;
        break;
      }
    }
    if (slot == notes_.size()) {
      // All busy: take the quietest.
      double quietest = 0.0;
      for (std::size_t i = 0; i < notes_.size(); ++i) {
        const auto& v = notes_[i];
        const double loudness = v.gain * v.env * v.fade;
        if (slot == notes_.size() || loudness < quietest) {
          slot = i;
          quietest = loudness;
        }
      }
      free_note(slot);
      ++steals_;
    }
    const auto& sample = library_->sample(index);
    const auto& stream = library_->stream(index);
    auto& v = notes_[slot];
    v = Voice{};
    v.active = true;
    v.note = true;
    v.key = key;
    v.sample = &stream;
    v.resident = library_->resident(index);
    v.semitones = static_cast<double>(key - sample.root_key);
    v.step = step_for(v.semitones);
    v.base_gain = weight;
    v.dynamics_db = sample.peak_db - top_db;
    v.gain = note_gain(v);
    const auto& k = keys_[static_cast<std::size_t>(key - first_key)];
    v.env_step = k.env_step;
    v.shelf = shelf;
    v.shelf_coefficient = shelf_coefficient_;
    v.started = clock_;
    ++active_notes_;
    if (stream.frames > stream.resident) {
      streams_->slot(slot).request.store(
          sample_stream::request_word(++generations_[slot], library_->serial(), index),
          std::memory_order_release);
    }
  }

  [[nodiscard]] double note_gain(const Voice& v) const noexcept {
    return v.base_gain * db_to_gain((values_[p::dynamics] - 1.0) * v.dynamics_db);
  }

  void free_note(std::size_t slot) noexcept {
    auto& v = notes_[slot];
    if (v.sample != nullptr && v.sample->frames > v.sample->resident) {
      streams_->slot(slot).request.store(
          sample_stream::request_word(++generations_[slot], 0, sample_stream::no_sample),
          std::memory_order_release);
    }
    v.active = false;
    --active_notes_;
  }

  // A release, resonance or pedal sample: resident whole, unaffected by
  // the dampers. `held` is how long its note was held, for the library's
  // per-second decay of the level.
  void start_shot(std::uint32_t index, int key, double velocity, double held,
                  std::size_t level) noexcept {
    if (values_.empty()) {
      return;
    }
    const auto& sample = library_->sample(index);
    if (velocity < sample.low_velocity || velocity > sample.high_velocity) {
      return;
    }
    Voice* slot = nullptr;
    for (auto& shot : shots_) {
      if (!shot.active) {
        slot = &shot;
        break;
      }
      if (slot == nullptr || shot.started < slot->started) {
        slot = &shot; // All busy: replace the oldest.
      }
    }
    if (!slot->active) {
      ++active_shots_;
    } else {
      ++steals_;
    }
    auto& v = *slot;
    v = Voice{};
    v.active = true;
    v.key = key;
    v.sample = &library_->stream(index);
    v.resident = library_->resident(index);
    v.semitones =
        sample.kind == detail::SampleKind::pedal_down || sample.kind == detail::SampleKind::pedal_up
            ? 0.0
            : static_cast<double>(key - sample.root_key);
    v.step = step_for(v.semitones);
    v.base_gain = db_to_gain(sample.gain_db - sample.decay_db_per_second * held) *
                  velocity_gain(velocity, sample.velocity_track);
    v.level = level;
    v.gain = v.base_gain * values_[level] * level_parameter_gain;
    v.started = clock_;
  }

  // Each key's damper moves toward where the key, sostenuto and sustain
  // pedal put it; a damper that lands on a sounding string sounds the
  // library's string release.
  void update_dampers() noexcept {
    const double pedal = pedal_lift(sustain_);
    const double up = tick_seconds_ / damper_lift_seconds;
    const double down = tick_seconds_ / damper_fall_seconds;
    const double scale = values_[p::damper];
    const int last_damped = library_ != nullptr ? library_->last_damped_key() : last_key;
    for (std::size_t k = 0; k < key_count; ++k) {
      auto& key = keys_[k];
      const int number = first_key + static_cast<int>(k);
      const bool undamped = number > last_damped;
      const double target = undamped || key.holds > 0 || key.captured ? 1.0 : pedal;
      if (target < pedal_down && key.target >= pedal_down && !undamped) {
        string_release(number);
      }
      key.target = target;
      key.lift =
          key.lift < target ? std::min(target, key.lift + up) : std::max(target, key.lift - down);
      const double sigma =
          (1.0 - key.lift) * std::numbers::ln10 * 3.0 / (by_register(damper_t60, number) * scale);
      key.env_step = std::exp(-sigma / rate_);
    }
  }

  void string_release(int number) noexcept {
    if (library_ == nullptr) {
      return;
    }
    bool sounding = false;
    for (const auto& voice : notes_) {
      sounding = sounding || (voice.active && voice.key == number &&
                              voice.gain * voice.env * voice.fade > silence);
    }
    if (!sounding) {
      return;
    }
    const auto& key = keys_[static_cast<std::size_t>(number - first_key)];
    const double held = static_cast<double>(clock_ - key.struck) / rate_;
    for (const auto index : library_->resonances(number)) {
      start_shot(index, number, key.velocity, held, p::string_release);
    }
  }

  void update_voices() noexcept {
    for (std::size_t slot = 0; slot < notes_.size(); ++slot) {
      auto& v = notes_[slot];
      if (!v.active) {
        continue;
      }
      v.step = step_for(v.semitones);
      v.gain = note_gain(v);
      v.env_step = keys_[static_cast<std::size_t>(v.key - first_key)].env_step;
      if (v.gain * v.env * v.fade < silence) {
        free_note(slot);
      }
    }
    for (auto& v : shots_) {
      if (v.active) {
        v.step = step_for(v.semitones);
        v.gain = v.base_gain * values_[v.level] * level_parameter_gain;
      }
    }
  }

  void update_resonators() noexcept {
    for (std::size_t k = 0; k < key_count; ++k) {
      auto& r = resonators_[k];
      r.mean_square =
          r.square_frames > 0 ? r.square_sum / static_cast<double>(r.square_frames) : 0.0;
      r.square_sum = 0.0;
      r.square_frames = 0;
      const double lift = keys_[k].lift;
      r.feedback = r.damped + (r.free - r.damped) * lift;
      const bool awake = lift > 0.0 || r.mean_square > resonance_awake;
      if (r.awake && !awake) {
        // Asleep: clear the loop so it wakes silent.
        std::ranges::fill(r.line, 0.0F);
        r.low = 0.0;
        r.mean_square = 0.0;
      }
      r.awake = awake;
    }
  }

  // Additive. Returns true when the voice needed streamed frames that had
  // not arrived (heard as silence for those frames).
  bool render_voice(Voice& v, std::size_t slot, std::span<double, max_frames> left,
                    std::span<double, max_frames> right, std::size_t frames) noexcept {
    const auto& sample = *v.sample;
    const bool streaming = v.note && sample.frames > sample.resident;
    const std::int16_t* ring = nullptr;
    if (streaming) {
      ring = streams_->ring(slot);
      const auto word = streams_->slot(slot).progress.load(std::memory_order_acquire);
      if (sample_stream::generation_of(word) == generations_[slot]) {
        v.streamed = sample_stream::frames_of(word);
      }
    }
    const auto& table = v.step > 1.0 ? rising_ : falling_;
    const bool exact = v.step == 1.0;
    const auto frames_per_output = static_cast<std::size_t>(std::ceil(v.step));
    const std::size_t per_pass = std::max<std::size_t>(
        1, (scratch_frames - sinc_taps - 2) / std::max<std::size_t>(1, frames_per_output));
    bool starved = false;
    double square = 0.0;
    std::size_t done = 0;
    while (done < frames) {
      const std::size_t count = std::min(frames - done, per_pass);
      const auto first =
          static_cast<std::int64_t>(std::floor(v.position)) - static_cast<std::int64_t>(sinc_left);
      const double end = v.position + static_cast<double>(count - 1) * v.step;
      const auto last = static_cast<std::int64_t>(std::floor(end)) +
                        static_cast<std::int64_t>(sinc_taps) - sinc_left - 1;
      const auto span = static_cast<std::size_t>(last - first + 1);
      starved = gather(v, ring, first, std::min(span, scratch_frames)) || starved;
      for (std::size_t i = 0; i < count; ++i) {
        const double base = std::floor(v.position);
        const double fraction = v.position - base;
        const auto at = static_cast<std::size_t>(static_cast<std::int64_t>(base) - first);
        double x_left = 0.0;
        double x_right = 0.0;
        if (exact) {
          x_left = scratch_left_[at];
          x_right = scratch_right_[at];
        } else {
          const std::size_t from = at - static_cast<std::size_t>(sinc_left);
          float sum_left = 0.0F;
          float sum_right = 0.0F;
          table.read(scratch_left_.data() + from, scratch_right_.data() + from, fraction, sum_left,
                     sum_right);
          x_left = sum_left;
          x_right = sum_right;
        }
        if (v.shelf != 1.0) {
          v.low_left += v.shelf_coefficient * (x_left - v.low_left);
          v.low_right += v.shelf_coefficient * (x_right - v.low_right);
          x_left = v.low_left + v.shelf * (x_left - v.low_left);
          x_right = v.low_right + v.shelf * (x_right - v.low_right);
        }
        const double amplitude = v.gain * v.env * v.fade;
        x_left *= amplitude;
        x_right *= amplitude;
        left[done + i] += x_left;
        right[done + i] += x_right;
        square += x_left * x_left + x_right * x_right;
        v.env *= v.env_step;
        if (v.fade_step > 0.0) {
          v.fade = std::max(0.0, v.fade - v.fade_step);
        }
        v.position += v.step;
      }
      done += count;
    }
    v.mean_square = square / (2.0 * static_cast<double>(std::max<std::size_t>(1, frames)));
    const auto next =
        static_cast<std::int64_t>(std::floor(v.position)) - static_cast<std::int64_t>(sinc_left);
    if (streaming) {
      // Hand back what no later read needs.
      const std::int64_t needed = next - static_cast<std::int64_t>(sample.resident);
      if (needed > static_cast<std::int64_t>(v.released)) {
        v.released = static_cast<std::uint32_t>(
            std::min<std::int64_t>(needed, static_cast<std::int64_t>(v.streamed)));
        streams_->slot(slot).consumed.store(sample_stream::tagged(generations_[slot], v.released),
                                            std::memory_order_release);
      }
    }
    const bool ended =
        next >= static_cast<std::int64_t>(sample.frames) || (v.fade_step > 0.0 && v.fade <= 0.0);
    if (ended) {
      if (v.note) {
        free_note(slot);
      } else {
        v.active = false;
        --active_shots_;
      }
    }
    return starved;
  }

  // Source frames [first, first + count) of a voice's sample as floats:
  // the resident start, then the voice's ring. True when a streamed frame
  // had not arrived.
  bool gather(const Voice& v, const std::int16_t* ring, std::int64_t first,
              std::size_t count) noexcept {
    return sample_stream::gather(*v.sample, detail::pack_channels, v.resident, ring, v.streamed,
                                 first, count, scratch_left_.data(), scratch_right_.data());
  }

  void render_resonance(std::span<double, max_frames> left, std::span<double, max_frames> right,
                        std::size_t frames) noexcept {
    const double level = resonance_level();
    if (level <= 0.0) {
      return;
    }
    std::array<double, max_frames> excitation{};
    for (std::size_t i = 0; i < frames; ++i) {
      excitation[i] = 0.5 * (left[i] + right[i]);
    }
    for (auto& r : resonators_) {
      if (!r.awake) {
        continue;
      }
      const std::size_t size = r.line.size();
      double square = 0.0;
      for (std::size_t i = 0; i < frames; ++i) {
        const std::size_t newer = (r.write + size - r.whole) % size;
        const std::size_t older = (newer + size - 1) % size;
        const double delayed = static_cast<double>(r.line[newer]) +
                               r.fraction * (static_cast<double>(r.line[older]) - r.line[newer]);
        r.low += resonance_loop_coefficient * (delayed - r.low);
        const double ring = r.feedback * r.low;
        r.line[r.write] = static_cast<float>(excitation[i] * r.input + ring);
        r.write = r.write + 1 == size ? 0 : r.write + 1;
        left[i] += ring * level * r.pan_left;
        right[i] += ring * level * r.pan_right;
        square += ring * ring;
      }
      r.square_sum += square;
      r.square_frames += frames;
    }
  }

  double rate_;
  library::State state_;
  std::unique_ptr<sample_stream::Streams> streams_;
  double rate_ratio_;
  SincTable falling_; // For notes read at or below their rate...
  SincTable rising_;  // ...and above it.
  const Library* library_ = nullptr;
  double tick_seconds_ = 0.0;
  std::span<const double> values_{};
  bool primed_ = false;
  std::array<Voice, max_note_voices> notes_{};
  std::array<std::uint32_t, max_note_voices> generations_{};
  std::array<Voice, max_shots> shots_{};
  std::size_t active_notes_ = 0;
  std::size_t active_shots_ = 0;
  std::uint32_t steals_ = 0;
  std::array<Key, key_count> keys_{};
  std::array<Resonator, key_count> resonators_{};
  std::array<Held, max_held> held_{};
  std::size_t held_count_ = 0;
  double sustain_ = 0.0;
  double sostenuto_ = 0.0;
  double soft_ = 0.0;
  std::size_t pedal_down_turn_ = 0;
  std::size_t pedal_up_turn_ = 0;
  std::array<float, scratch_frames> scratch_left_{};
  std::array<float, scratch_frames> scratch_right_{};
  double shelf_coefficient_ = 0.0;
  double tone_coefficient_ = 0.0;
  double tone_low_ = 1.0;
  double tone_high_ = 1.0;
  double tone_left_ = 0.0;
  double tone_right_ = 0.0;
  double mirror_ = 0.0;
  double mirror_step_ = 0.0;
  double gain_ = 1.0;
  double gain_step_ = 0.0;
  double declick_frames_ = 1.0;
  double declick_remaining_ = 0.0;
  double declick_left_ = 0.0;
  double declick_right_ = 0.0;
  double last_left_ = 0.0;
  double last_right_ = 0.0;
  std::uint64_t clock_ = 0;
  std::uint64_t underruns_ = 0;
};

std::unique_ptr<SynthKernel> create_kernel(double sample_rate) {
  // Control: the first Concert loads the library (its resident starts).
  const auto status = library::status();
  return std::make_unique<ConcertKernel>(sample_rate, status.state);
}

// The longest a released note can sound: its dampers' decay (or an
// undamped treble sample to its end), a release, resonance or pedal-up
// sample, and the resonance loops of the undamped treble.
std::uint64_t tail_frames(std::span<const double> values, double sample_rate) {
  const auto status = library::status();
  const auto* library = detail::current();
  const int last_damped = library != nullptr ? library->last_damped_key() : last_key;
  double longest = 0.0;
  for (int key = first_key; key <= last_damped; ++key) {
    longest = std::max(longest, by_register(damper_t60, key) * values[p::damper]);
  }
  longest *= silence_per_t60;
  if (values[p::resonance] > 0.0) {
    double ring = resonance_damped_t60;
    for (int key = last_damped + 1; key <= last_key; ++key) {
      ring = std::max(ring, by_register(resonance_t60, key));
    }
    longest = std::max(longest, ring * silence_per_t60);
  }
  if (library != nullptr && status.state == library::State::ready) {
    // Played up to max_step_semitones slower than recorded.
    const double slowest = std::exp2(max_step_semitones / 12.0);
    const auto seconds = [&](std::uint32_t frames) {
      return static_cast<double>(frames) / library::pack_rate * slowest;
    };
    longest = std::max(
        {longest, seconds(library->longest_undamped_note()), seconds(library->longest_one_shot())});
  }
  const double total = longest + damper_fall_seconds + tail_margin_seconds;
  return static_cast<std::uint64_t>(std::ceil(total * sample_rate));
}

const EngineDescriptor descriptor{"concert", "Concert", 1, concert_table::parameters,
                                  concert_presets::presets};
const SynthEngine concert_engine{&descriptor, &create_kernel, &tail_frames, true};

} // namespace

const SynthEngine& engine() { return concert_engine; }

} // namespace oscillate::native_dsp::synth::concert
