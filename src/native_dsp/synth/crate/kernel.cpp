#include "oscillate/native_dsp/synth/crate.hpp"

#include "document.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/pedal_latch.hpp"
#include "oscillate/native_dsp/synth/filter.hpp"
#include "oscillate/native_dsp/synth/modulation.hpp"
#include "oscillate/native_dsp/synth/oversampler.hpp"
#include "oscillate/native_dsp/synth/shaping.hpp"
#include "oscillate/native_dsp/synth/tempo_sync.hpp"
#include "oscillate/warp_dsp/bandlimited.hpp"
#include "table.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <vector>

namespace oscillate::native_dsp::synth::crate {
namespace {

using detail::CrateState;
using detail::SampleData;
namespace global = detail::global;
namespace chop = detail::chop;

// Voices and their fades (spec 09 §2.3).
constexpr std::size_t voice_limit = static_cast<std::size_t>(detail::max_voices);
// Faded-out voices finish beside the ones that replace them.
constexpr std::size_t voice_slots = 2 * voice_limit;
constexpr double retrigger_fade_seconds = 0.002;
constexpr double steal_fade_seconds = 0.002;
constexpr double choke_fade_seconds = 0.005;
constexpr double loop_crossfade_seconds = 0.002;
// A chop's last frames fade over this, so its end never clicks.
constexpr double end_fade_seconds = 0.002;
// An era switch dips the output out and back in over this each way.
constexpr double era_switch_seconds = 0.003;
// A reset or a new sample ramps the last output to silence over this.
constexpr double declick_seconds = 0.0015;
// Follow Tempo's grains (Hann, two overlapping by half). Each new grain
// starts where it best continues the sounding one (WSOLA), searched within
// this far of where the tempo puts it, by correlating this many points this
// many frames apart, so tones never cancel at the joins.
constexpr double follow_grain_seconds = 0.05;
constexpr double align_search_seconds = 0.006;
constexpr std::size_t align_points = 64;
constexpr double align_stride_frames = 4.0;
constexpr double align_energy_floor = 1e-9;
// Most offsets one alignment scores at frame steps. A wider search (a fast
// read) scores a coarse grid of this many, then every frame around the best,
// so a grain restart costs about the same at any speed.
constexpr std::int64_t align_max_candidates = 1024;
// The AHDSR's decay and release land exactly on their ends with this
// exponential overshoot ratio (modulation.hpp's Adsr at curve 0.5).
constexpr double envelope_overshoot = 0.1;
constexpr double sustain_glide_seconds = 0.005;
constexpr double milliseconds = 1e-3;

// Swing: every second sixteenth late by (swing - 50) / 50 of a sixteenth.
constexpr double sixteenth_beats = 0.25;
constexpr double sixteenths_per_beat = 1.0 / sixteenth_beats;
constexpr double swing_span_percent = detail::max_swing_percent - detail::min_swing_percent;
constexpr double swing_full_delay = 0.5; // At 75 %, half a sixteenth.
// A note's position in sixteenths is snapped to this fraction of one before
// rounding to its sixteenth. Its beat comes from the transport by different
// sums under different block splits, a few ulps apart; snapping makes a note
// exactly between two sixteenths (a tie) resolve the same way under any split.
constexpr double swing_snap = 1 << 20;
constexpr double slowest_bpm = 20.0; // For the tail bound only.
constexpr std::size_t event_capacity = 256;

// Velocity: gain = 1 - s * (1 - (velocity / 127)^2), exactly 1 at 127.
constexpr double max_velocity = 127.0;
constexpr double semitones_per_octave = 12.0;
constexpr double seconds_per_minute = 60.0;

// Eras (spec 09 §2.4). Starting figures until fitted to tools/crate_reference.
constexpr double drive_range = 9.0;         // Drive 1 is +20 dB into the era's curve.
constexpr double sp12_knee = 0.8;           // Hard-ish clip: clean below, a fast knee above.
constexpr double mpc60_lowpass_hz = 14'000; // Gentle 12 dB anti-alias.
constexpr double s950_tracking = 0.42;      // Its low-pass follows the rate...
constexpr double s950_resonance = 0.3;      // ...with some resonance.
constexpr double tape_shelf_hz = 4'500.0;   // High-shelf roll-off.
constexpr double tape_shelf_db = -6.0;
constexpr double tape_bias = 0.12; // Asymmetry: even harmonics.
constexpr double tape_flutter_hz = 6.3;
constexpr double tape_flutter_cents = 3.0;
using native_dsp::dsp::butterworth_q;

// Wow: slow pitch drift from two incommensurate sines.
constexpr double wow_hz = 0.55;
constexpr double wow_drift_hz = 0.23;
constexpr double wow_drift_phase = 0.31;
constexpr double wow_drift_share = 0.35;
constexpr double wow_depth_cents = 35.0;
constexpr double cents_per_octave = 1'200.0;

// Vinyl: hiss and crackle, seeded, gated by playing.
constexpr double hiss_gain = 0.0035;         // About -49 dBFS of pink at full.
constexpr double crackles_per_second = 22.0; // At full.
constexpr double crackle_gain = 0.22;
constexpr double crackle_decay_seconds = 0.0003;
constexpr double vinyl_gate_release_seconds = 0.3;
constexpr double vinyl_gate_attack_seconds = 0.01;
constexpr std::uint32_t hiss_left_seed = 0x5EED'0001U;
constexpr std::uint32_t hiss_right_seed = 0x5EED'0002U;
constexpr std::uint32_t crackle_seed = 0x5EED'0003U;

constexpr double silence_db = -120.0;
constexpr double tiny = 1e-18; // Denormal guard for idle filters.

[[nodiscard]] double flushed(double x) noexcept { return (x + tiny) - tiny; }
[[nodiscard]] std::size_t choice(double value) noexcept {
  return static_cast<std::size_t>(std::max(0.0, std::round(value)));
}
using native_dsp::dsp::db_to_gain;
[[nodiscard]] std::uint32_t frames_of(double seconds, double rate) noexcept {
  return static_cast<std::uint32_t>(std::max(0.0, std::round(seconds * rate)));
}

enum class ReadKind : std::uint8_t { bandlimited, nearest, linear };

[[nodiscard]] ReadKind read_kind(Era era) noexcept {
  switch (era) {
  case Era::sp12:
    return ReadKind::nearest;
  case Era::mpc60:
  case Era::s950:
    return ReadKind::linear;
  default:
    return ReadKind::bandlimited;
  }
}

// Attack (linear), hold, decay and release (exponential, landing exactly),
// sustain gliding to live edits. Times are taken as each segment starts.
class Ahdsr {
public:
  enum class Stage : std::uint8_t { idle, attack, hold, decay, sustain, release };
  struct Times {
    std::uint32_t attack = 0;
    std::uint32_t hold = 0;
    std::uint32_t decay = 1;
    double sustain = 1.0;
    std::uint32_t release = 1;
  };

  void start(const Times& times, double sustain_glide) noexcept {
    times_ = times;
    glide_ = sustain_glide;
    value_ = 0.0;
    if (times.attack == 0) {
      value_ = 1.0;
      begin_hold();
      return;
    }
    stage_ = Stage::attack;
    remaining_ = times.attack;
    step_ = 1.0 / times.attack;
  }
  void set(const Times& times) noexcept { times_ = times; }
  void release() noexcept {
    if (stage_ == Stage::idle || stage_ == Stage::release)
      return;
    exponential(Stage::release, 0.0, times_.release);
  }
  [[nodiscard]] double next() noexcept {
    switch (stage_) {
    case Stage::idle:
      return 0.0;
    case Stage::sustain:
      value_ += (times_.sustain - value_) * glide_;
      return value_;
    case Stage::attack:
      if (--remaining_ == 0) {
        value_ = 1.0;
        begin_hold();
      } else {
        value_ += step_;
      }
      return value_;
    case Stage::hold:
      if (--remaining_ == 0)
        exponential(Stage::decay, times_.sustain, times_.decay);
      return value_;
    case Stage::decay:
    case Stage::release:
      if (--remaining_ == 0) {
        value_ = end_;
        stage_ = stage_ == Stage::decay ? Stage::sustain : Stage::idle;
        return value_;
      }
      value_ = flushed(target_ + (value_ - target_) * coefficient_);
      return value_;
    }
    return 0.0;
  }
  [[nodiscard]] Stage stage() const noexcept { return stage_; }
  [[nodiscard]] bool active() const noexcept { return stage_ != Stage::idle; }
  void reset() noexcept {
    stage_ = Stage::idle;
    value_ = 0.0;
  }

private:
  void begin_hold() noexcept {
    if (times_.hold == 0) {
      exponential(Stage::decay, times_.sustain, times_.decay);
      return;
    }
    stage_ = Stage::hold;
    remaining_ = times_.hold;
  }
  // Approaches an overshoot target so the curve lands on `end` after exactly
  // `frames` frames: |end - target| = r |range| and c^N = r / (1 + r).
  void exponential(Stage stage, double end, std::uint32_t frames) noexcept {
    stage_ = stage;
    end_ = end;
    remaining_ = std::max<std::uint32_t>(1, frames);
    target_ = end + envelope_overshoot * (end - value_);
    coefficient_ = std::pow(envelope_overshoot / (1.0 + envelope_overshoot),
                            1.0 / static_cast<double>(remaining_));
  }

  Times times_{};
  Stage stage_ = Stage::idle;
  double value_ = 0.0;
  double step_ = 0.0;
  double end_ = 0.0;
  double target_ = 0.0;
  double coefficient_ = 0.0;
  double glide_ = 0.0;
  std::uint32_t remaining_ = 0;
};

// A linear fade to silence, then the voice is free.
struct Fade {
  double gain = 1.0;
  double step = 0.0;
  [[nodiscard]] bool fading() const noexcept { return step > 0.0; }
  void start(std::uint32_t frames) noexcept {
    const double next = gain / std::max<std::uint32_t>(1, frames);
    step = fading() ? std::max(step, next) : next;
  }
  // False once silent.
  bool next() noexcept {
    gain = std::max(0.0, gain - step);
    return gain > 0.0;
  }
};

struct Grain {
  double position = 0.0;
  std::uint32_t age = 0;
  bool live = false;
};

struct Voice {
  bool active = false;
  std::uint64_t serial = 0;
  foundation::IdBits clip{};
  foundation::IdBits note{};
  std::uint8_t key = 0;
  std::uint8_t velocity = 0;
  // The chop, as compiled when the note started.
  std::size_t slot = 0;
  std::int64_t chop_start = 0;
  std::int64_t chop_end = 0;
  // Captured at note-on (next-note parameters).
  ChopMode mode = ChopMode::oneshot;
  ChopFilter filter = ChopFilter::off;
  TimeMode time = TimeMode::repitch;
  bool reverse = false;
  std::size_t choke = 0;
  bool released = false;
  // Live bounds in source frames, from start and length.
  double lo = 0.0;
  double hi = 0.0;
  double direction = 1.0;
  // Repitch: the playhead. Follow: the analysis point and two grains.
  double position = 0.0;
  double analysis = 0.0;
  bool analysis_done = false;
  std::array<Grain, 2> grains{};
  // Source frames per output frame.
  LinearRamp speed;        // Pitch speed, before wow.
  double time_speed = 0.0; // Follow's analysis speed.
  // Level and pan.
  LinearRamp amp;
  LinearRamp pan_left;
  LinearRamp pan_right;
  // Filter.
  LinearRamp filter_g;
  LinearRamp filter_k;
  BasicZdfSvf<StereoSample> svf;
  Ahdsr envelope;
  Fade fade;
  // Era sample-and-hold.
  double era_phase = 1.0;
  StereoSample held{};
};

// The engine's character stage after the voices: the era's filter and
// curve, oversampled where it saturates smoothly.
struct EraStage {
  BasicZdfSvf<StereoSample> lowpass;
  StereoSample shelf_state{};
  StereoUpsampler2x up;
  StereoDownsampler2x down;
  void reset() noexcept {
    lowpass.reset();
    shelf_state = {};
    up.reset();
    down.reset();
  }
};

[[nodiscard]] double knee_clip(double x) noexcept {
  const double magnitude = std::fabs(x);
  if (magnitude <= sp12_knee)
    return x;
  const double over = soft_clip_cubic((magnitude - sp12_knee) / (1.0 - sp12_knee));
  return std::copysign(sp12_knee + (1.0 - sp12_knee) * over, x);
}

struct QueuedEvent {
  std::uint64_t due = 0;
  std::uint64_t sequence = 0;
  NoteEvent event{};
  bool on = false;
};
struct DelayedNote {
  foundation::IdBits clip{};
  foundation::IdBits note{};
  std::uint64_t delay = 0;
};

class CrateKernel final : public SynthKernel {
public:
  explicit CrateKernel(double sample_rate)
      : rate_(sample_rate),
        kernel_(warp_dsp::BandlimitedKernel::for_tier(warp_dsp::QualityTier::q0)),
        max_reach_(warp_dsp::bandlimited_reach(*kernel_, detail::max_read_speed)),
        grain_frames_(
            std::max<std::uint32_t>(2, frames_of(follow_grain_seconds, sample_rate) & ~1U)),
        values_(detail::parameter_count, 0.0) {
    (void)halfband_coefficients(); // Built on control, before the callback.
    window_.resize(grain_frames_);
    for (std::uint32_t i = 0; i < grain_frames_; ++i) {
      const double s = std::sin(std::numbers::pi * static_cast<double>(i) / grain_frames_);
      window_[i] = s * s;
    }
    const auto& table = detail::descriptor();
    for (std::size_t i = 0; i < values_.size(); ++i)
      values_[i] = table.parameters[i].default_value;
    sustain_glide_ = 1.0 - std::exp(-1.0 / (sustain_glide_seconds * rate_));
    crackle_decay_ = std::exp(-1.0 / (crackle_decay_seconds * rate_));
    gate_release_ = std::exp(-1.0 / (vinyl_gate_release_seconds * rate_));
    gate_attack_ = 1.0 - std::exp(-1.0 / (vinyl_gate_attack_seconds * rate_));
    reseed();
    output_.reset(1.0);
    era_fade_.reset(1.0);
    wow_.reset(1.0);
  }

  void control(std::span<const double> values, std::uint32_t frames_to_next_tick,
               const SynthTransport& transport) noexcept override {
    const bool first = !controlled_;
    controlled_ = true;
    std::copy(values.begin(), values.end(), values_.begin());
    tick_beat_ = transport.beat_at_block_start;
    beats_per_frame_ = transport.beats_per_frame;
    tick_frame_ = now_;
    const std::uint32_t ramp = first ? 0 : frames_to_next_tick;

    // Values arrive sanitised: choices are in range.
    const auto era = static_cast<Era>(choice(values_[global::era]));
    if (first) {
      era_ = era;
    } else if (era != era_ && era != pending_era_) {
      pending_era_ = era;
      era_fade_.set_target(0.0, frames_of(era_switch_seconds, rate_));
    }
    output_.set_target(db_to_gain(values_[global::output]), ramp);
    drive_.set_target(values_[global::drive], ramp);
    project_bpm_ = sync_beats_per_second(transport, rate_) * seconds_per_minute;
    wow_.set_target(wow_factor(now_ + frames_to_next_tick), first ? 0 : frames_to_next_tick);
    era_step_ = values_[global::rate] / rate_;
    bits_scale_ =
        values_[global::bits] < bypass_bits ? std::exp2(values_[global::bits] - 1.0) : 0.0;
    update_era_filter();
    for (auto& voice : voices_) {
      if (voice.active)
        update_voice(voice, ramp);
    }
  }

  void note_on(const NoteEvent& event) noexcept override {
    const PedalLatch::Note held{event.clip, event.note, event.key};
    NoteEvent note = event;
    note.velocity = latch_.note_on(
        held, event.velocity, [this](const PedalLatch::Note& earlier) { release_now(earlier); });
    const auto delay = swing_delay();
    if (delay == 0 || !enqueue(note, true, delay)) {
      start(note);
      return;
    }
    remember_delay(note, delay);
  }

  void note_off(const NoteEvent& event) noexcept override {
    const PedalLatch::Note held{event.clip, event.note, event.key};
    if (latch_.note_off(held))
      release_now(held);
  }

  void pedal(project::MidiPedal pedal, double value) noexcept override {
    latch_.pedal(pedal, value, [this](const PedalLatch::Note& note) { release_now(note); });
  }

  void render(std::span<engine::StereoFrame> output) noexcept override {
    std::size_t frame = 0;
    while (frame < output.size()) {
      while (queued_ > 0 && queue_[0].due <= now_)
        dispatch_front();
      std::size_t end = output.size();
      if (queued_ > 0)
        end = std::min<std::size_t>(end, frame + static_cast<std::size_t>(queue_[0].due - now_));
      render_span(output.subspan(frame, end - frame));
      frame = end;
    }
  }

  void reset() noexcept override {
    begin_declick();
    for (auto& voice : voices_)
      voice.active = false;
    queued_ = 0;
    delayed_count_ = 0;
    latch_.reset();
    now_ = 0;
    tick_frame_ = 0;
    era_stage_.reset();
    if (pending_era_) {
      era_ = *pending_era_;
      pending_era_.reset();
    }
    era_fade_.reset(1.0);
    reseed();
  }

  [[nodiscard]] KernelStatus status() const noexcept override {
    KernelStatus status;
    status.voice_steals = steals_;
    for (const auto& voice : voices_) {
      if (!voice.active)
        continue;
      ++status.active_voices;
      if (voice.fade.fading())
        continue;
      const auto at = [&](std::size_t control) {
        return values_[detail::chop_index(voice.slot, control)];
      };
      status.deferred_changes =
          status.deferred_changes ||
          static_cast<TimeMode>(choice(values_[global::time])) != voice.time ||
          static_cast<ChopMode>(choice(at(chop::mode))) != voice.mode ||
          static_cast<ChopFilter>(choice(at(chop::filter))) != voice.filter ||
          (choice(at(chop::reverse)) == 1) != voice.reverse ||
          choice(at(chop::choke)) != voice.choke;
    }
    return status;
  }

  [[nodiscard]] std::size_t telemetry(std::span<float> out) const noexcept override {
    if (out.size() < telemetry::size)
      return 0;
    std::array<std::uint64_t, max_chops> newest{};
    for (std::size_t slot = 0; slot < max_chops; ++slot)
      out[telemetry::playheads + slot] = -1.0F;
    for (const auto& voice : voices_) {
      if (!voice.active || voice.fade.fading() || voice.serial < newest[voice.slot])
        continue;
      newest[voice.slot] = voice.serial;
      const double length =
          static_cast<double>(std::max<std::int64_t>(1, voice.chop_end - voice.chop_start));
      const double at = voice.time == TimeMode::follow ? voice.analysis : voice.position;
      out[telemetry::playheads + voice.slot] = static_cast<float>(
          std::clamp((at - static_cast<double>(voice.chop_start)) / length, 0.0, 1.0));
    }
    out[telemetry::last_slot] = static_cast<float>(last_slot_);
    out[telemetry::last_velocity] = static_cast<float>(last_velocity_);
    out[telemetry::output_peak_db] = static_cast<float>(
        peak_ > 1e-6 ? std::max(silence_db, 20.0 * std::log10(peak_)) : silence_db);
    peak_ = 0.0;
    return telemetry::size;
  }

  void adopt_document(const SynthDocumentState& state) noexcept override {
    const auto& next = static_cast<const CrateState&>(state);
    const SampleData* sample = next.sample.get();
    if (sample != sample_) {
      // The old sample may be released once this adoption is acknowledged.
      bool sounding = false;
      for (auto& voice : voices_) {
        sounding = sounding || voice.active;
        voice.active = false;
      }
      if (sounding)
        begin_declick();
    }
    state_ = &next;
    sample_ = sample;
  }

private:
  // ---- Events -------------------------------------------------------------

  [[nodiscard]] std::uint64_t swing_delay() const noexcept {
    const double swing = values_[global::swing] - detail::min_swing_percent;
    if (swing <= 0.0 || beats_per_frame_ <= 0.0)
      return 0;
    const double beat = tick_beat_ + beats_per_frame_ * static_cast<double>(now_ - tick_frame_);
    const double snapped = std::round(beat * sixteenths_per_beat * swing_snap) / swing_snap;
    const auto sixteenth = std::llround(snapped);
    if (sixteenth % 2 == 0)
      return 0;
    const double frames =
        swing / swing_span_percent * swing_full_delay * sixteenth_beats / beats_per_frame_;
    return static_cast<std::uint64_t>(std::llround(frames));
  }

  bool enqueue(const NoteEvent& event, bool on, std::uint64_t delay) noexcept {
    if (queued_ == event_capacity)
      return false;
    const QueuedEvent queued{now_ + delay, sequence_++, event, on};
    std::size_t at = queued_;
    while (at > 0 && (queue_[at - 1].due > queued.due)) {
      queue_[at] = queue_[at - 1];
      --at;
    }
    queue_[at] = queued;
    ++queued_;
    return true;
  }

  void dispatch_front() noexcept {
    const QueuedEvent front = queue_[0];
    for (std::size_t i = 1; i < queued_; ++i)
      queue_[i - 1] = queue_[i];
    --queued_;
    if (front.on)
      start(front.event);
    else
      release_voice(front.event.clip, front.event.note);
  }

  void remember_delay(const NoteEvent& note, std::uint64_t delay) noexcept {
    if (delayed_count_ < delayed_.size())
      delayed_[delayed_count_++] = {note.clip, note.note, delay};
  }

  // A note-off the pedals let through: late by its note-on's swing, if any.
  void release_now(const PedalLatch::Note& note) noexcept {
    for (std::size_t i = 0; i < delayed_count_; ++i) {
      if (delayed_[i].clip == note.clip && delayed_[i].note == note.note) {
        const auto delay = delayed_[i].delay;
        delayed_[i] = delayed_[--delayed_count_];
        if (enqueue({note.clip, note.note, note.key, 0}, false, delay))
          return;
        break;
      }
    }
    release_voice(note.clip, note.note);
  }

  void release_voice(foundation::IdBits clip, foundation::IdBits note) noexcept {
    for (auto& voice : voices_) {
      if (!voice.active || voice.released || voice.clip != clip || voice.note != note)
        continue;
      voice.released = true;
      if (voice.mode != ChopMode::oneshot)
        voice.envelope.release();
    }
  }

  // ---- Voices -------------------------------------------------------------

  [[nodiscard]] double chop_value(std::size_t slot, std::size_t control) const noexcept {
    return values_[detail::chop_index(slot, control)];
  }

  void start(const NoteEvent& event) noexcept {
    if (state_ == nullptr || sample_ == nullptr || state_->chops.empty())
      return;
    const auto layout = static_cast<Layout>(choice(values_[global::layout]));
    std::size_t index = 0;
    if (layout == Layout::pads) {
      if (event.key < first_pad_key)
        return;
      index = static_cast<std::size_t>(event.key - first_pad_key);
      if (index >= state_->chops.size())
        return;
    } else {
      const auto mapped = state_->chop_of_slot[std::min<std::size_t>(
          choice(values_[global::keys_chop]), max_chops - 1)];
      if (mapped == detail::no_chop)
        return;
      index = static_cast<std::size_t>(mapped);
    }
    const auto& compiled = state_->chops[index];
    const std::size_t slot = compiled.slot;
    const auto mode = static_cast<ChopMode>(choice(chop_value(slot, chop::mode)));
    const auto choke = choice(chop_value(slot, chop::choke));

    // Choke group: release its voices with a short fade.
    if (choke > 0) {
      for (auto& voice : voices_) {
        if (voice.active && voice.choke == choke)
          voice.fade.start(frames_of(choke_fade_seconds, rate_));
      }
    }
    // A retriggered chop restarts; the old voice fades under the new one.
    for (auto& voice : voices_) {
      if (voice.active && voice.key == event.key && voice.slot == slot &&
          voice.mode != ChopMode::loop)
        voice.fade.start(frames_of(retrigger_fade_seconds, rate_));
    }
    // Voice limit: the oldest released voice goes first, else the oldest.
    const auto limit = std::min<std::size_t>(choice(values_[global::voices]), voice_limit);
    std::size_t sounding = 0;
    Voice* victim = nullptr;
    Voice* victim_released = nullptr;
    for (auto& voice : voices_) {
      if (!voice.active || voice.fade.fading())
        continue;
      ++sounding;
      if (victim == nullptr || voice.serial < victim->serial)
        victim = &voice;
      const bool released = voice.released || voice.envelope.stage() == Ahdsr::Stage::release;
      if (released && (victim_released == nullptr || voice.serial < victim_released->serial))
        victim_released = &voice;
    }
    if (sounding >= limit) {
      Voice* stolen = victim_released != nullptr ? victim_released : victim;
      if (stolen != nullptr) {
        stolen->fade.start(frames_of(steal_fade_seconds, rate_));
        ++steals_;
      }
    }
    Voice* voice = nullptr;
    for (auto& candidate : voices_) {
      if (!candidate.active) {
        voice = &candidate;
        break;
      }
    }
    if (voice == nullptr) {
      // Every slot is busy fading: cut the oldest.
      voice = &voices_[0];
      for (auto& candidate : voices_) {
        if (candidate.serial < voice->serial)
          voice = &candidate;
      }
    }

    *voice = Voice{};
    voice->active = true;
    voice->serial = ++serial_;
    voice->clip = event.clip;
    voice->note = event.note;
    voice->key = event.key;
    voice->velocity = event.velocity;
    voice->slot = slot;
    voice->chop_start = compiled.start;
    voice->chop_end = compiled.end;
    voice->mode = mode;
    voice->filter = static_cast<ChopFilter>(choice(chop_value(slot, chop::filter)));
    voice->time = static_cast<TimeMode>(choice(values_[global::time]));
    voice->reverse = choice(chop_value(slot, chop::reverse)) == 1;
    voice->choke = choke;
    voice->direction = voice->reverse ? -1.0 : 1.0;
    update_voice(*voice, 0);
    voice->position = voice->reverse ? voice->hi - 1.0 : voice->lo;
    voice->analysis = voice->position;
    if (voice->time == TimeMode::follow) {
      // Both grains start on the chop's first frame, one at the peak of its
      // window and one at its foot, so the attack is the source itself.
      voice->grains[0] = {voice->position, grain_frames_ / 2, true};
      voice->grains[1] = {voice->position, 0, true};
    }
    voice->envelope.start(envelope_times(slot), sustain_glide_);
    last_slot_ = static_cast<int>(slot);
    last_velocity_ = event.velocity;
  }

  [[nodiscard]] Ahdsr::Times envelope_times(std::size_t slot) const noexcept {
    Ahdsr::Times times;
    times.attack = frames_of(chop_value(slot, chop::attack) * milliseconds, rate_);
    times.hold = frames_of(chop_value(slot, chop::hold) * milliseconds, rate_);
    times.decay =
        std::max<std::uint32_t>(1, frames_of(chop_value(slot, chop::decay) * milliseconds, rate_));
    times.sustain = chop_value(slot, chop::sustain);
    times.release = std::max<std::uint32_t>(
        1, frames_of(chop_value(slot, chop::release) * milliseconds, rate_));
    return times;
  }

  // Live values into a voice, ramped over `ramp` frames.
  void update_voice(Voice& voice, std::uint32_t ramp) noexcept {
    const std::size_t slot = voice.slot;
    const auto set = [ramp](LinearRamp& r, double target) {
      if (ramp == 0)
        r.reset(target);
      else
        r.set_target(target, ramp);
    };
    // Bounds.
    const double length = static_cast<double>(voice.chop_end - voice.chop_start);
    voice.lo = static_cast<double>(voice.chop_start) + chop_value(slot, chop::start) * length;
    voice.hi = std::min(static_cast<double>(voice.chop_end),
                        voice.lo + std::max(1.0, chop_value(slot, chop::length) * length));
    if (voice.hi - voice.lo < 1.0)
      voice.lo = voice.hi - 1.0;
    // Pitch.
    double semitones =
        chop_value(slot, chop::pitch) + chop_value(slot, chop::fine) / detail::cents_per_semitone;
    if (static_cast<Layout>(choice(values_[global::layout])) == Layout::keys)
      semitones += static_cast<double>(voice.key) - values_[global::root];
    const double source = static_cast<double>(sample_->source_rate) / rate_;
    set(voice.speed, std::exp2(semitones / semitones_per_octave) * source);
    voice.time_speed = source;
    if (state_->sample_bpm > 0.0)
      voice.time_speed *= project_bpm_ / state_->sample_bpm;
    // Level.
    const double velocity = values_[global::velocity];
    const double v = static_cast<double>(voice.velocity) / max_velocity;
    set(voice.amp, db_to_gain(chop_value(slot, chop::gain)) * (1.0 - velocity * (1.0 - v * v)));
    const double pan = chop_value(slot, chop::pan);
    set(voice.pan_left, pan > 0.0 ? 1.0 - pan : 1.0);
    set(voice.pan_right, pan < 0.0 ? 1.0 + pan : 1.0);
    // Filter.
    if (voice.filter != ChopFilter::off) {
      set(voice.filter_g, prewarp(chop_value(slot, chop::cutoff) / rate_));
      set(voice.filter_k, 1.0 / resonance_q(chop_value(slot, chop::resonance), butterworth_q));
    }
    voice.envelope.set(envelope_times(slot));
  }

  // One source read at a fractional position, silent outside the sample.
  [[nodiscard]] StereoSample read(double position, ReadKind kind,
                                  const warp_dsp::BandlimitedReadPlan& plan) const noexcept {
    const SampleData& s = *sample_;
    const double whole = std::floor(position);
    const auto frame = static_cast<std::int64_t>(whole);
    if (frame < max_reach_ - s.padding || frame + max_reach_ >= s.frames + s.padding)
      return {};
    const double fraction = position - whole;
    switch (kind) {
    case ReadKind::nearest: {
      const auto at = frame + (fraction >= 0.5 ? 1 : 0);
      return {*s.left_at(at), *s.right_at(at)};
    }
    case ReadKind::linear: {
      const double l0 = *s.left_at(frame);
      const double r0 = *s.right_at(frame);
      return {l0 + (*s.left_at(frame + 1) - l0) * fraction,
              r0 + (*s.right_at(frame + 1) - r0) * fraction};
    }
    case ReadKind::bandlimited:
      break;
    }
    const auto q16 =
        static_cast<std::uint32_t>(fraction * static_cast<double>(warp_dsp::source_unit));
    const auto value =
        warp_dsp::bandlimited_read(s.left_at(frame), s.right_at(frame), q16, *kernel_, plan);
    return {value.left, value.right};
  }

  // 0..1: how far a read is from the edge it plays toward, over the end fade.
  [[nodiscard]] static double edge_gain(const Voice& voice, double position, double fade) noexcept {
    const double distance = voice.reverse ? position - voice.lo : voice.hi - position;
    return std::clamp(distance / fade, 0.0, 1.0);
  }

  // The voice's source at its current position(s); advances nothing.
  [[nodiscard]] StereoSample source(const Voice& voice, double speed,
                                    ReadKind kind) const noexcept {
    const auto plan = warp_dsp::make_bandlimited_read_plan(speed);
    const double fade = std::max(1.0, end_fade_seconds * rate_ * speed);
    if (voice.time == TimeMode::follow) {
      StereoSample sum{};
      for (const auto& grain : voice.grains) {
        if (!grain.live || grain.age == 0)
          continue;
        double weight = window_[grain.age];
        if (voice.mode != ChopMode::loop)
          weight *= edge_gain(voice, grain.position, fade);
        if (weight > 0.0)
          sum = sum + weight * (voice.mode == ChopMode::loop
                                    ? loop_read(voice, grain.position, speed, kind, plan)
                                    : read(grain.position, kind, plan));
      }
      return sum;
    }
    if (voice.mode == ChopMode::loop)
      return loop_read(voice, voice.position, speed, kind, plan);
    const double gain = edge_gain(voice, voice.position, fade);
    if (gain <= 0.0)
      return {};
    const auto value = read(voice.position, kind, plan);
    return gain == 1.0 ? value : gain * value;
  }

  // A loop's join: the crossfade's length and how far the playhead jumps.
  // With audio before the loop's start (after its end, reversed), the pass
  // there fades in, a whole loop back; at the sample's edge the loop's own
  // first frames fade in instead, and the jump skips them.
  struct LoopJoin {
    double cross = 0.0;
    double jump = 0.0;
  };
  [[nodiscard]] LoopJoin loop_join(const Voice& voice, double speed) const noexcept {
    const double span = voice.hi - voice.lo;
    const double cross = std::min(0.5 * span, loop_crossfade_seconds * rate_ * speed);
    const bool room = voice.reverse ? voice.hi + cross <= static_cast<double>(sample_->frames)
                                    : voice.lo - cross >= 0.0;
    return {cross, room || cross < 1.0 ? span : span - cross};
  }

  // A looping read at `position`: across the join it crossfades this pass
  // into the next, which the join reaches. Plain reads and grains share it.
  [[nodiscard]] StereoSample loop_read(const Voice& voice, double position, double speed,
                                       ReadKind kind,
                                       const warp_dsp::BandlimitedReadPlan& plan) const noexcept {
    const auto join = loop_join(voice, speed);
    const double into =
        voice.reverse ? voice.lo + join.cross - position : position - (voice.hi - join.cross);
    if (join.cross >= 1.0 && into > 0.0) {
      // Equal gain from this pass into the next.
      const double t = std::min(1.0, into / join.cross);
      const double partner = position - voice.direction * join.jump;
      return (1.0 - t) * read(position, kind, plan) + t * read(partner, kind, plan);
    }
    return read(position, kind, plan);
  }

  // A looping grain's position after a step: past the loop's end it jumps
  // back as the plain loop does, landing where the crossfade left off.
  [[nodiscard]] double loop_step(const Voice& voice, double position, double speed) const noexcept {
    if (voice.mode != ChopMode::loop)
      return position;
    const double jump = loop_join(voice, speed).jump;
    if (!voice.reverse && position >= voice.hi)
      return position - jump;
    if (voice.reverse && position < voice.lo)
      return position + jump;
    return position;
  }

  // Both channels summed at the frame below a position; 0 outside the sample.
  [[nodiscard]] double mono_at(double position) const noexcept {
    const SampleData& s = *sample_;
    const auto frame = static_cast<std::int64_t>(std::floor(position));
    if (frame < 0 || frame >= s.frames)
      return 0.0;
    return static_cast<double>(*s.left_at(frame)) + static_cast<double>(*s.right_at(frame));
  }

  // A position folded into a looping chop's region; others pass through.
  [[nodiscard]] static double looped(const Voice& voice, double position) noexcept {
    const double span = voice.hi - voice.lo;
    if (voice.mode != ChopMode::loop || span <= 0.0)
      return position;
    if (position >= voice.lo && position < voice.hi)
      return position;
    const double folded = std::fmod(position - voice.lo, span);
    return voice.lo + (folded < 0.0 ? folded + span : folded);
  }

  // Where a new grain best continues `other` near `target` (WSOLA): the
  // offset whose next few milliseconds correlate best with what `other` is
  // about to play. Stays inside the chop; a loop's reads wrap round it.
  [[nodiscard]] double align(const Voice& voice, const Grain& other, double target,
                             double speed) const noexcept {
    if (!other.live)
      return target;
    const double stride = voice.direction * speed * align_stride_frames;
    std::array<double, align_points> shape{};
    double shape_energy = 0.0;
    for (std::size_t j = 0; j < align_points; ++j) {
      shape[j] = mono_at(looped(voice, other.position + static_cast<double>(j) * stride));
      shape_energy += shape[j] * shape[j];
    }
    if (shape_energy <= align_energy_floor)
      return target;
    const auto radius = static_cast<std::int64_t>(std::ceil(align_search_seconds * rate_ * speed));
    double best = target;
    double best_score = 0.0;
    bool found = false;
    // Scores one offset; true when it is the best so far.
    const auto score_at = [&](std::int64_t k) {
      const double candidate = looped(voice, target + static_cast<double>(k));
      if (voice.mode != ChopMode::loop && (candidate < voice.lo || candidate >= voice.hi))
        return false;
      double dot = 0.0;
      double energy = 0.0;
      for (std::size_t j = 0; j < align_points; ++j) {
        const double value = mono_at(looped(voice, candidate + static_cast<double>(j) * stride));
        dot += shape[j] * value;
        energy += value * value;
      }
      const double score = dot / std::sqrt(energy + align_energy_floor);
      if (!found || score > best_score) {
        found = true;
        best_score = score;
        best = candidate;
        return true;
      }
      return false;
    };
    // Frame steps up to the candidate budget; beyond it a coarse grid, then
    // every frame within one coarse step of its best.
    const std::int64_t step =
        std::max<std::int64_t>(1, (2 * radius + align_max_candidates) / align_max_candidates);
    std::int64_t best_k = 0;
    for (std::int64_t k = -radius; k <= radius; k += step) {
      if (score_at(k))
        best_k = k;
    }
    if (step > 1 && found) {
      const std::int64_t centre = best_k;
      for (std::int64_t k = std::max(-radius, centre - step + 1);
           k <= std::min(radius, centre + step - 1); ++k) {
        if (k != centre)
          score_at(k);
      }
    }
    return best;
  }

  // Moves the voice's reads on one frame; false when it has played out.
  bool advance(Voice& voice, double speed) noexcept {
    const double step = voice.direction * speed;
    if (voice.time == TimeMode::follow) {
      voice.analysis += voice.direction * voice.time_speed;
      const bool past = voice.reverse ? voice.analysis <= voice.lo : voice.analysis >= voice.hi;
      if (past) {
        if (voice.mode == ChopMode::loop)
          voice.analysis += voice.reverse ? voice.hi - voice.lo : voice.lo - voice.hi;
        else
          voice.analysis_done = true;
      }
      bool any = false;
      for (std::size_t i = 0; i < voice.grains.size(); ++i) {
        auto& grain = voice.grains[i];
        if (!grain.live)
          continue;
        grain.position = loop_step(voice, grain.position + step, speed);
        if (++grain.age >= grain_frames_) {
          grain.age = 0;
          grain.live = !voice.analysis_done;
          if (grain.live)
            grain.position = align(voice, voice.grains[i ^ 1U], voice.analysis, speed);
        }
        any = any || grain.live;
      }
      return any;
    }
    voice.position += step;
    if (voice.mode == ChopMode::loop) {
      const double jump = loop_join(voice, speed).jump;
      if (!voice.reverse && voice.position >= voice.hi)
        voice.position -= jump;
      else if (voice.reverse && voice.position < voice.lo)
        voice.position += jump;
      return true;
    }
    return voice.reverse ? voice.position > voice.lo : voice.position < voice.hi;
  }

  [[nodiscard]] StereoSample quantise(StereoSample x) const noexcept {
    if (bits_scale_ <= 0.0)
      return x;
    return {std::nearbyint(x.left * bits_scale_) / bits_scale_,
            std::nearbyint(x.right * bits_scale_) / bits_scale_};
  }

  // Adds the voice into `mix`; returns how many frames it sounded for.
  std::size_t render_voice(Voice& voice, std::span<StereoSample> mix) noexcept {
    const ReadKind kind = read_kind(era_);
    const bool hold = kind != ReadKind::bandlimited || era_step_ < 1.0;
    LinearRamp wow = wow_;
    for (std::size_t i = 0; i < mix.size(); ++i) {
      auto& out = mix[i];
      const double speed = std::min(detail::max_read_speed, voice.speed.next() * wow.next());
      StereoSample x;
      if (hold) {
        voice.era_phase += era_step_;
        if (voice.era_phase >= 1.0) {
          voice.era_phase -= std::floor(voice.era_phase);
          voice.held = quantise(source(voice, speed, kind));
        }
        x = voice.held;
      } else {
        x = quantise(source(voice, speed, kind));
      }
      if (voice.filter != ChopFilter::off) {
        const auto c = SvfCoefficients::make(voice.filter_g.next(), voice.filter_k.next());
        const auto y = voice.svf.process(x, c);
        switch (voice.filter) {
        case ChopFilter::lowpass:
          x = y.low;
          break;
        case ChopFilter::highpass:
          x = y.high;
          break;
        case ChopFilter::bandpass:
          x = y.band;
          break;
        default:
          x = y.low + y.high;
          break;
        }
      }
      double gain = voice.envelope.next() * voice.amp.next();
      if (voice.fade.fading()) {
        if (!voice.fade.next())
          voice.active = false;
        gain *= voice.fade.gain;
      }
      out = out + StereoSample{x.left * gain * voice.pan_left.next(),
                               x.right * gain * voice.pan_right.next()};
      if (!advance(voice, speed) || !voice.envelope.active())
        voice.active = false;
      if (!voice.active) {
        voice.svf.reset();
        return i + 1;
      }
    }
    return mix.size();
  }

  // ---- The character stage ------------------------------------------------

  [[nodiscard]] double wow_factor(std::uint64_t frame) const noexcept {
    const double seconds = static_cast<double>(frame) / rate_;
    double cents = 0.0;
    const double depth = values_[global::wow];
    if (depth > 0.0) {
      const double slow = sin_cycle(wow_hz * seconds);
      const double drift = sin_cycle(wow_drift_hz * seconds + wow_drift_phase);
      cents += depth * wow_depth_cents * ((1.0 - wow_drift_share) * slow + wow_drift_share * drift);
    }
    if (era_ == Era::tape)
      cents += tape_flutter_cents * sin_cycle(tape_flutter_hz * seconds);
    return std::exp2(cents / cents_per_octave);
  }

  void update_era_filter() noexcept {
    switch (era_) {
    case Era::mpc60:
      era_filter_ = SvfCoefficients::make(prewarp(mpc60_lowpass_hz / rate_), 1.0 / butterworth_q);
      break;
    case Era::s950:
      era_filter_ = SvfCoefficients::make(prewarp(s950_tracking * values_[global::rate] / rate_),
                                          1.0 / resonance_q(s950_resonance, butterworth_q));
      break;
    case Era::tape:
      shelf_pole_ = std::exp(-2.0 * std::numbers::pi * tape_shelf_hz / rate_);
      shelf_gain_ = db_to_gain(tape_shelf_db);
      break;
    default:
      break;
    }
  }

  [[nodiscard]] StereoSample saturate(StereoSample x, double pre) const noexcept {
    const double makeup = (1.0 + std::log(pre)) / pre;
    if (era_ == Era::tape) {
      const double offset = soft_clip(tape_bias);
      return {(soft_clip(x.left * pre + tape_bias) - offset) * makeup,
              (soft_clip(x.right * pre + tape_bias) - offset) * makeup};
    }
    return soft_clip(x * pre) * makeup;
  }

  [[nodiscard]] StereoSample character(StereoSample x, double drive) noexcept {
    auto& s = era_stage_;
    switch (era_) {
    case Era::mpc60:
    case Era::s950:
      x = s.lowpass.process(x, era_filter_).low;
      break;
    case Era::tape: {
      // A one-pole split: the lows pass, the highs are shelved down.
      s.shelf_state = x + shelf_pole_ * (s.shelf_state - x);
      s.shelf_state = {flushed(s.shelf_state.left), flushed(s.shelf_state.right)};
      x = s.shelf_state + shelf_gain_ * (x - s.shelf_state);
      break;
    }
    default:
      break;
    }
    const double pre = 1.0 + drive * drive_range;
    if (era_ == Era::sp12)
      return {knee_clip(x.left * pre), knee_clip(x.right * pre)};
    if (era_ == Era::clean && drive <= 0.0)
      return x;
    // Smooth curves run at twice the rate so their harmonics do not fold.
    StereoSample a;
    StereoSample b;
    s.up.process(x, a, b);
    return s.down.process(saturate(a, pre), saturate(b, pre));
  }

  [[nodiscard]] StereoSample vinyl(bool playing) noexcept {
    gate_ = playing ? gate_ + (1.0 - gate_) * gate_attack_ : flushed(gate_ * gate_release_);
    const double hiss_l = hiss_left_.next(NoiseColour::pink);
    const double hiss_r = hiss_right_.next(NoiseColour::pink);
    const double chance = 0.5 * (crackle_noise_.white() + 1.0);
    const double size = 0.5 * (crackle_noise_.white() + 1.0);
    const double side = 0.5 * (crackle_noise_.white() + 1.0);
    const double amount = values_[global::vinyl];
    double impulse = 0.0;
    if (chance < amount * crackles_per_second / rate_)
      impulse = (size < 0.5 ? -1.0 : 1.0) * crackle_gain * (0.3 + 0.7 * size * size);
    crackle_ = {flushed(crackle_.left * crackle_decay_ + impulse * side),
                flushed(crackle_.right * crackle_decay_ + impulse * (1.0 - side))};
    const double level = amount * gate_;
    return {level * (hiss_gain * hiss_l + crackle_.left),
            level * (hiss_gain * hiss_r + crackle_.right)};
  }

  void render_span(std::span<engine::StereoFrame> output) noexcept {
    const std::size_t n = output.size();
    std::array<StereoSample, control_interval_frames> mix{};
    std::size_t done = 0;
    while (done < n) {
      const std::size_t count = std::min(n - done, mix.size());
      std::fill_n(mix.begin(), count, StereoSample{});
      const auto chunk = std::span(mix).first(count);
      // Frames of this chunk any voice sounded in: the vinyl's gate follows
      // them frame by frame, whatever the block split.
      std::size_t sounding = 0;
      if (sample_ != nullptr) {
        for (auto& voice : voices_) {
          if (voice.active)
            sounding = std::max(sounding, render_voice(voice, chunk));
        }
      }
      for (std::size_t i = 0; i < count; ++i) {
        (void)wow_.next();
        StereoSample x = character(mix[i], drive_.next());
        // The noise runs every frame, so where it is heard never changes it.
        const StereoSample noise = vinyl(i < sounding);
        if (values_[global::vinyl] > 0.0)
          x = x + noise;
        const double era_gain = era_fade_.next();
        if (pending_era_ && era_fade_.settled() && era_gain == 0.0) {
          era_ = *pending_era_;
          pending_era_.reset();
          era_stage_.reset();
          update_era_filter();
          era_fade_.set_target(1.0, frames_of(era_switch_seconds, rate_));
        }
        x = x * (output_.next() * era_gain);
        if (declick_remaining_ > 0) {
          const double t = static_cast<double>(declick_remaining_--) / declick_frames_;
          x = x + t * declick_from_;
        }
        auto& frame = output[done + i];
        frame.left += static_cast<float>(x.left);
        frame.right += static_cast<float>(x.right);
        peak_ = std::max({peak_, std::fabs(x.left), std::fabs(x.right)});
        last_ = x;
        ++now_;
      }
      done += count;
    }
  }

  void begin_declick() noexcept {
    declick_frames_ = std::max<std::uint32_t>(1, frames_of(declick_seconds, rate_));
    declick_remaining_ = declick_frames_;
    declick_from_ = last_;
    last_ = {};
  }

  void reseed() noexcept {
    hiss_left_.reset(hiss_left_seed);
    hiss_right_.reset(hiss_right_seed);
    crackle_noise_.reset(crackle_seed);
    crackle_ = {};
    gate_ = 0.0;
  }

  double rate_;
  std::shared_ptr<const warp_dsp::BandlimitedKernel> kernel_;
  std::int64_t max_reach_;
  std::uint32_t grain_frames_;
  std::vector<double> window_;
  std::vector<double> values_;
  const CrateState* state_ = nullptr;
  const SampleData* sample_ = nullptr;
  std::array<Voice, voice_slots> voices_{};
  std::uint64_t serial_ = 0;
  std::uint32_t steals_ = 0;
  bool controlled_ = false;
  // Time: frames rendered since the last reset, and the transport at the
  // last control tick.
  std::uint64_t now_ = 0;
  std::uint64_t tick_frame_ = 0;
  double tick_beat_ = 0.0;
  double beats_per_frame_ = 0.0;
  double project_bpm_ = 120.0;
  // Swung notes: queued events by due frame, and the delays their
  // note-offs follow.
  std::array<QueuedEvent, event_capacity> queue_{};
  std::size_t queued_ = 0;
  std::uint64_t sequence_ = 0;
  std::array<DelayedNote, event_capacity> delayed_{};
  std::size_t delayed_count_ = 0;
  PedalLatch latch_;
  // Character.
  Era era_ = Era::clean;
  std::optional<Era> pending_era_;
  LinearRamp era_fade_;
  EraStage era_stage_;
  SvfCoefficients era_filter_{};
  double shelf_pole_ = 0.0;
  double shelf_gain_ = 1.0;
  double era_step_ = 1.0;
  double bits_scale_ = 0.0;
  LinearRamp drive_;
  LinearRamp output_;
  LinearRamp wow_;
  double sustain_glide_ = 0.0;
  // Vinyl.
  NoiseSource hiss_left_;
  NoiseSource hiss_right_;
  NoiseSource crackle_noise_;
  StereoSample crackle_{};
  double crackle_decay_ = 0.0;
  double gate_ = 0.0;
  double gate_attack_ = 0.0;
  double gate_release_ = 0.0;
  // Output.
  StereoSample last_{};
  StereoSample declick_from_{};
  std::uint32_t declick_frames_ = 1;
  std::uint32_t declick_remaining_ = 0;
  mutable double peak_ = 0.0;
  int last_slot_ = -1;
  int last_velocity_ = 0;
};

std::unique_ptr<SynthKernel> create_kernel(double sample_rate) {
  return std::make_unique<CrateKernel>(sample_rate);
}

// One-shots play to their chop's end whatever the note-off; without the
// document their length is unknown, so a sustaining one-shot is bounded here.
constexpr double oneshot_tail_bound_seconds = 16.0;
constexpr double ring_decades = 11.5; // ln(1e5): a resonance's ring to -100 dB, in time constants.

std::uint64_t tail_frames(std::span<const double> values, double sample_rate) {
  double longest = 0.0;
  for (std::size_t slot = 0; slot < max_chops; ++slot) {
    const auto at = [&](std::size_t control) { return values[detail::chop_index(slot, control)]; };
    double seconds = at(chop::release) * milliseconds;
    if (static_cast<ChopMode>(choice(at(chop::mode))) == ChopMode::oneshot) {
      seconds = at(chop::sustain) > 0.0
                    ? oneshot_tail_bound_seconds
                    : (at(chop::attack) + at(chop::hold) + at(chop::decay)) * milliseconds;
    }
    if (static_cast<ChopFilter>(choice(at(chop::filter))) != ChopFilter::off) {
      const double q = resonance_q(at(chop::resonance), butterworth_q);
      seconds += ring_decades * q / (std::numbers::pi * at(chop::cutoff));
    }
    longest = std::max(longest, seconds);
  }
  const double swing = (values[global::swing] - detail::min_swing_percent) / swing_span_percent *
                       swing_full_delay * sixteenth_beats * seconds_per_minute / slowest_bpm;
  const double seconds =
      longest + swing + choke_fade_seconds + era_switch_seconds +
      (values[global::vinyl] > 0.0 ? ring_decades * vinyl_gate_release_seconds : 0.0);
  return static_cast<std::uint64_t>(std::ceil(seconds * sample_rate));
}

} // namespace

const SynthEngine& engine() {
  // Crate latches its pedals itself (ADR-0035's generic latch, inside the
  // kernel) so a held note-off still follows its swung note-on.
  static const SynthEngine crate_engine{&detail::descriptor(),
                                        &create_kernel,
                                        &tail_frames,
                                        true,
                                        &detail::validate_document,
                                        &detail::prepare_document,
                                        &detail::preset_document};
  return crate_engine;
}

} // namespace oscillate::native_dsp::synth::crate
