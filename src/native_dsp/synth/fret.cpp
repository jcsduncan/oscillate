#include "oscillate/native_dsp/synth/fret.hpp"

#include "fret_library.hpp"
#include "fret_presets.hpp"
#include "fret_table.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/parameter_control.hpp"
#include "oscillate/native_dsp/synth/fret_feel.hpp"
#include "oscillate/native_dsp/synth/sample_stream.hpp"
#include "oscillate/native_dsp/transport.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>

namespace oscillate::native_dsp::synth::fret {
namespace {

namespace ft = fret_table;
using detail::Kind;
using sample_stream::SincTable;

namespace p {
constexpr std::size_t style = ft::index_of("fret.feel.style");
constexpr std::size_t amount = ft::index_of("fret.feel.amount");
constexpr std::size_t dynamics = ft::index_of("fret.feel.dynamics");
constexpr std::size_t pocket = ft::index_of("fret.feel.pocket");
constexpr std::size_t ghosts = ft::index_of("fret.feel.ghosts");
constexpr std::size_t humanize = ft::index_of("fret.feel.humanize");
constexpr std::size_t articulation = ft::index_of("fret.articulation");
constexpr std::size_t release = ft::index_of("fret.release");
constexpr std::size_t slide = ft::index_of("fret.slide");
constexpr std::size_t amp = ft::index_of("fret.tone.amp");
constexpr std::size_t tilt = ft::index_of("fret.tone.tilt");
constexpr std::size_t squash = ft::index_of("fret.tone.squash");
constexpr std::size_t grit = ft::index_of("fret.tone.grit");
constexpr std::size_t tune = ft::index_of("fret.tune");
constexpr std::size_t level = ft::index_of("fret.level");
} // namespace p

constexpr std::size_t max_frames = control_interval_frames;
constexpr std::size_t note_voices = 8;
constexpr std::size_t release_voices = 8;
constexpr std::size_t max_pending = 64;
constexpr std::size_t max_delayed = 64;
// Frames gathered per resampling pass.
constexpr std::size_t scratch_frames = 512;
constexpr double max_velocity = 127.0;
constexpr int velocity_steps = 128; // Layer splits divide this evenly.
constexpr double semitones_per_octave = 12.0;
constexpr double cents_per_semitone = 100.0;
constexpr double ms_per_second = 1'000.0;
// A slide reaches at most an octave.
constexpr int max_slide_semitones = 12;
// The library's own envelope (its SFZ): a held note fades over this after
// its note-off...
constexpr double release_seconds = 0.25;
// ...a key struck again stops its earlier sound this fast...
constexpr double retrigger_fade_seconds = 0.015;
// ...and a string release is quieter the longer the note was held.
constexpr double release_decay_db_per_second = 4.0;
constexpr double mute_db = -59.9;
constexpr double meter_floor_db = -120.0;
constexpr double safety_knee = 0.9;
constexpr double declick_seconds = 0.0015;
// Feel reads the song position at this resolution, so a transport's
// rounding under different block splits never moves a decision.
constexpr double beat_resolution = 65'536.0;
// Tone: the amp's cabinet band and saturator, the tilt pivot, and Squash.
constexpr double amp_low_hz = 80.0;
constexpr double amp_high_hz = 4'500.0;
using native_dsp::dsp::butterworth_q;
constexpr double amp_bias = 0.15; // Asymmetry: the soft side of the curve.
constexpr double amp_drive = 1.5;
constexpr double grit_drive = 8.0;
constexpr double tilt_pivot_hz = 700.0;
constexpr double squash_threshold_db = -18.0;
constexpr double squash_max_ratio = 4.0;
constexpr double squash_attack_seconds = 0.010;
constexpr double squash_release_seconds = 0.120;
// Make-up restores this share of the gain a full-scale peak loses.
constexpr double squash_makeup_share = 0.5;
constexpr double tail_margin_seconds = 0.25;
// Guards a transport without a meter.
constexpr double min_beats_per_bar = 1.0 / 64.0;

using native_dsp::dsp::db_to_gain;

[[nodiscard]] double safety(double x) noexcept {
  const double magnitude = std::fabs(x);
  if (magnitude <= safety_knee) {
    return x;
  }
  const double over = (magnitude - safety_knee) / (1.0 - safety_knee);
  const double shaped = safety_knee + (1.0 - safety_knee) * std::tanh(over);
  return x < 0.0 ? -shaped : shaped;
}

[[nodiscard]] std::size_t choice(double value, std::size_t count) noexcept {
  const auto index = static_cast<long long>(std::lround(value));
  return static_cast<std::size_t>(
      std::clamp<long long>(index, 0, static_cast<long long>(count) - 1));
}

// The layer whose velocity range holds `velocity`, and that range's top.
[[nodiscard]] std::uint32_t layer_of(int velocity, std::uint32_t layers) noexcept {
  const auto layer = static_cast<std::uint32_t>(velocity) * layers / velocity_steps;
  return std::min(layer, layers - 1);
}
[[nodiscard]] double layer_top(std::uint32_t layer, std::uint32_t layers) noexcept {
  const auto top = (layer + 1) * velocity_steps / layers - 1; // A whole velocity.
  return static_cast<double>(top);
}

// RBJ second-order section, direct form I.
struct Biquad {
  double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
  double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;

  void design(bool high, double hz, double rate) noexcept {
    const double w = 2.0 * std::numbers::pi * hz / rate;
    const double alpha = std::sin(w) / (2.0 * butterworth_q);
    const double c = std::cos(w);
    const double a0 = 1.0 + alpha;
    const double edge = high ? (1.0 + c) / 2.0 : (1.0 - c) / 2.0;
    b0 = edge / a0;
    b1 = (high ? -2.0 * edge : 2.0 * edge) / a0;
    b2 = edge / a0;
    a1 = -2.0 * c / a0;
    a2 = (1.0 - alpha) / a0;
  }
  [[nodiscard]] double run(double x) noexcept {
    const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1;
    x1 = x;
    y2 = y1;
    y1 = y;
    return y;
  }
  void clear() noexcept { x1 = x2 = y1 = y2 = 0.0; }
};

struct Voice {
  bool active = false;
  bool held = false;    // Before its note-off.
  bool streams = false; // Longer than its resident start.
  std::uint32_t index = 0;
  const sample_stream::Sample* sample = nullptr;
  const std::int16_t* resident = nullptr;
  int key = 0;
  foundation::IdBits clip{};
  foundation::IdBits note{};
  int velocity = 0;
  double position = 0.0;  // Source frames.
  double semitones = 0.0; // Away from the sample's own pitch (slides).
  double target = 0.0;
  double glide = 0.0; // Semitones per output frame.
  double gain = 0.0;
  double fade = 1.0;
  double fade_step = 0.0;
  std::uint64_t started = 0;
  std::uint32_t streamed = 0;
  std::uint32_t released = 0;
};

// A note Pocket plays late, or a note-off that follows it.
struct Pending {
  std::uint64_t frame = 0; // On the kernel's clock.
  bool on = true;
  int key = 0;
  int velocity = 0;
  feel::Articulation articulation = feel::Articulation::pluck;
  bool pushed = false;
  foundation::IdBits clip{};
  foundation::IdBits note{};
};

struct Delayed {
  foundation::IdBits clip{};
  foundation::IdBits note{};
  std::uint64_t frames = 0;
};

class FretKernel final : public SynthKernel {
public:
  FretKernel(double sample_rate, library::State state)
      : rate_(sample_rate), state_(state),
        streams_(std::make_unique<sample_stream::Streams>(detail::registry(),
                                                          library::pack_channels, note_voices)),
        base_step_(static_cast<double>(library::pack_rate) / sample_rate),
        falling_(SincTable::passband),
        rising_(std::min(SincTable::passband,
                         SincTable::passband /
                             (base_step_ * std::exp2((max_slide_semitones +
                                                      ft::max_tune_cents / cents_per_semitone) /
                                                     semitones_per_octave)))) {
    declick_frames_ = std::max(1.0, declick_seconds * sample_rate);
    retrigger_step_ = 1.0 / std::max(1.0, retrigger_fade_seconds * sample_rate);
    release_step_ = 1.0 / std::max(1.0, release_seconds * sample_rate);
    low_cut_.design(true, amp_low_hz, sample_rate);
    high_cut_.design(false, amp_high_hz, sample_rate);
    tilt_coefficient_ = native_dsp::dsp::one_pole_coefficient(tilt_pivot_hz, sample_rate);
    attack_ = std::exp(-1.0 / (squash_attack_seconds * sample_rate));
    release_ = std::exp(-1.0 / (squash_release_seconds * sample_rate));
    library_ = detail::current();
    if (library_ != nullptr) {
      state_ = library::State::ready;
    }
  }

  void control(std::span<const double> values, std::uint32_t frames_to_next_tick,
               const Transport& transport) noexcept override {
    values_ = values;
    transport_ = transport;
    frames_since_tick_ = 0;
    if (active_notes() == 0 && active_releases() == 0) {
      if (const auto* latest = detail::current(); latest != nullptr && latest != library_) {
        library_ = latest;
        state_ = library::State::ready;
      }
    }
    peak_ = 0.0;
    const double gain =
        values[p::level] <= mute_db ? 0.0 : db_to_gain(values[p::level] + headroom_db);
    gain_step_ = primed_ ? (gain - gain_) / std::max<double>(1.0, frames_to_next_tick) : 0.0;
    if (!primed_) {
      gain_ = gain;
    }
    const double half_tilt = 0.5 * values[p::tilt];
    tilt_low_ = db_to_gain(-half_tilt);
    tilt_high_ = db_to_gain(half_tilt);
    const double squash = values[p::squash];
    ratio_ = 1.0 + (squash_max_ratio - 1.0) * squash;
    makeup_ = db_to_gain((1.0 - 1.0 / ratio_) * -squash_threshold_db * squash_makeup_share);
    drive_ = amp_drive + grit_drive * values[p::grit];
    primed_ = true;
  }

  void note_on(const NoteEvent& event) noexcept override {
    if (values_.empty() || library_ == nullptr || event.key < first_key || event.key > last_key) {
      return;
    }
    const double per_frame = beats_per_frame();
    const double bars =
        transport_.bar_at_block_start + static_cast<double>(frames_since_tick_) * per_frame /
                                            std::max(transport_.beats_per_bar, min_beats_per_bar);
    const double bar = std::floor(bars);
    feel::Note note{beat_now(),
                    rounded((bars - bar) * transport_.beats_per_bar),
                    static_cast<std::int64_t>(bar),
                    static_cast<double>(event.frames) * per_frame,
                    event.key,
                    event.velocity};
    const auto decision = feel::decide(note, previous_, feel_settings(values_));
    previous_ = feel::after(note, decision);
    const auto articulation = played_articulation(values_, decision.articulation);
    const Pending start{clock_,          true,       event.key, decision.velocity, articulation,
                        decision.pushed, event.clip, event.note};
    const auto delay =
        static_cast<std::uint64_t>(std::llround(decision.delay_ms * rate_ / ms_per_second));
    if (delay > 0 && pending_count_ < max_pending && delayed_count_ < max_delayed) {
      auto late = start;
      late.frame = clock_ + delay;
      enqueue(late);
      delayed_[delayed_count_++] = {event.clip, event.note, delay};
      return;
    }
    begin(start);
  }

  void note_off(const NoteEvent& event) noexcept override {
    for (std::size_t i = 0; i < delayed_count_; ++i) {
      if (delayed_[i].clip == event.clip && delayed_[i].note == event.note) {
        Pending off;
        off.frame = clock_ + delayed_[i].frames;
        off.on = false;
        off.clip = event.clip;
        off.note = event.note;
        delayed_[i] = delayed_[--delayed_count_];
        if (pending_count_ < max_pending) {
          enqueue(off);
          return;
        }
        break;
      }
    }
    end(event.clip, event.note);
  }

  // Split at each late note due inside the span, so it lands on its frame
  // however the host splits its blocks.
  void render(std::span<engine::StereoFrame> output) noexcept override {
    std::size_t done = 0;
    while (done < output.size()) {
      while (pending_count_ > 0 && pending_[0].frame <= clock_) {
        const Pending due = pending_[0];
        std::copy(pending_.begin() + 1,
                  pending_.begin() + static_cast<std::ptrdiff_t>(pending_count_), pending_.begin());
        --pending_count_;
        if (due.on) {
          begin(due);
        } else {
          end(due.clip, due.note);
        }
      }
      std::size_t length = output.size() - done;
      if (pending_count_ > 0) {
        length =
            static_cast<std::size_t>(std::min<std::uint64_t>(length, pending_[0].frame - clock_));
      }
      render_span(output.subspan(done, length));
      done += length;
      clock_ += length;
      frames_since_tick_ += length;
    }
  }

  void reset() noexcept override {
    declick_ = last_output_;
    declick_left_ = declick_frames_;
    for (std::size_t slot = 0; slot < note_voices; ++slot) {
      free_note(slot);
    }
    for (auto& v : releases_) {
      v.active = false;
    }
    pending_count_ = 0;
    delayed_count_ = 0;
    previous_ = {};
    robins_ = {};
    low_cut_.clear();
    high_cut_.clear();
    tilt_low_state_ = 0.0;
    envelope_ = 0.0;
  }

  [[nodiscard]] KernelStatus status() const noexcept override {
    KernelStatus status;
    status.active_voices = static_cast<std::uint32_t>(active_notes() + active_releases());
    return status;
  }

  [[nodiscard]] std::size_t telemetry(std::span<float> out) const noexcept override {
    if (out.size() < telemetry::size) {
      return 0;
    }
    out[telemetry::library] = static_cast<float>(state_);
    out[telemetry::underruns] = static_cast<float>(underruns_);
    out[telemetry::notes] = static_cast<float>(notes_ & telemetry_wrap);
    out[telemetry::last_key] = static_cast<float>(last_key_);
    out[telemetry::last_velocity] = static_cast<float>(last_velocity_);
    out[telemetry::last_articulation] = static_cast<float>(last_articulation_);
    out[telemetry::last_pushed] = last_pushed_ ? 1.0F : 0.0F;
    const double peak_db = peak_ > 0.0 ? 20.0 * std::log10(peak_) : meter_floor_db;
    out[telemetry::output_peak_db] = static_cast<float>(std::max(meter_floor_db, peak_db));
    return telemetry::size;
  }

  void await_content() noexcept override { streams_->service(); }

private:
  static constexpr std::uint32_t telemetry_wrap = (1U << 24U) - 1U;

  [[nodiscard]] double beats_per_frame() const noexcept {
    return sync_beats_per_second(transport_, rate_) / rate_;
  }
  [[nodiscard]] static double rounded(double beat) noexcept {
    return std::round(beat * beat_resolution) / beat_resolution;
  }
  [[nodiscard]] double beat_now() const noexcept {
    return rounded(transport_.beat_at_block_start +
                   static_cast<double>(frames_since_tick_) * beats_per_frame());
  }

  [[nodiscard]] std::size_t active_notes() const noexcept {
    return static_cast<std::size_t>(std::ranges::count_if(notes_v_, &Voice::active));
  }
  [[nodiscard]] std::size_t active_releases() const noexcept {
    return static_cast<std::size_t>(std::ranges::count_if(releases_, &Voice::active));
  }

  void enqueue(const Pending& pending) noexcept {
    std::size_t at = pending_count_;
    while (at > 0 && pending_[at - 1].frame > pending.frame) {
      pending_[at] = pending_[at - 1];
      --at;
    }
    pending_[at] = pending;
    ++pending_count_;
  }

  // Starts a note: a slide when one is held near enough, else a pluck.
  void begin(const Pending& n) noexcept {
    if (library_ == nullptr) {
      return;
    }
    last_key_ = n.key;
    last_velocity_ = n.velocity;
    last_articulation_ = static_cast<int>(n.articulation);
    last_pushed_ = n.pushed;
    ++notes_;
    const double slide_ms = values_[p::slide];
    if (slide_ms > 0.0) {
      for (auto& v : notes_v_) {
        // The new target counts from where the last slide was heading (its
        // key), and stays within the octave the rising filter is sized for.
        const double target = v.target + static_cast<double>(n.key - v.key);
        if (v.active && v.held && v.key != n.key &&
            std::fabs(target) <= static_cast<double>(max_slide_semitones)) {
          v.target = target;
          v.glide = (v.target - v.semitones) / std::max(1.0, slide_ms * rate_ / ms_per_second);
          v.key = n.key;
          v.clip = n.clip;
          v.note = n.note;
          return;
        }
      }
    }
    // A key struck again stops its earlier sound.
    for (auto& v : notes_v_) {
      if (v.active && v.key == n.key && v.fade_step < retrigger_step_) {
        v.held = false;
        v.fade_step = retrigger_step_;
      }
    }
    auto kind = detail::kind_of(n.articulation);
    if (library_->layers(kind, n.key) == 0) {
      kind = Kind::pluck;
    }
    const auto layers = library_->layers(kind, n.key);
    const auto layer = layer_of(n.velocity, layers);
    const auto robins = library_->round_robins(kind, n.key, layer);
    auto& counter =
        robins_[static_cast<std::size_t>(kind)][static_cast<std::size_t>(n.key - first_key)];
    const auto index = robins[counter++ % robins.size()];
    const std::size_t slot = free_slot();
    auto& v = notes_v_[slot];
    v = Voice{};
    v.active = true;
    v.held = true;
    v.index = index;
    v.sample = &library_->stream(index);
    v.resident = library_->resident(index);
    v.streams = v.sample->frames > v.sample->resident;
    v.key = n.key;
    v.clip = n.clip;
    v.note = n.note;
    v.velocity = n.velocity;
    // The pack's scale already restores each recording's level (peak_db only
    // records it), so layers keep their recorded balance.
    v.gain = static_cast<double>(n.velocity) / layer_top(layer, layers);
    v.started = clock_;
    ++generations_[slot];
    streams_->slot(slot).request.store(
        sample_stream::request_word(generations_[slot], v.streams ? library_->serial() : 0,
                                    v.streams ? index : sample_stream::no_sample),
        std::memory_order_release);
  }

  // A free note voice, else the oldest one's.
  [[nodiscard]] std::size_t free_slot() noexcept {
    std::size_t oldest = 0;
    for (std::size_t slot = 0; slot < note_voices; ++slot) {
      if (!notes_v_[slot].active) {
        return slot;
      }
      if (notes_v_[slot].started < notes_v_[oldest].started) {
        oldest = slot;
      }
    }
    return oldest;
  }

  void free_note(std::size_t slot) noexcept {
    notes_v_[slot].active = false;
    ++generations_[slot];
    streams_->slot(slot).request.store(
        sample_stream::request_word(generations_[slot], 0, sample_stream::no_sample),
        std::memory_order_release);
  }

  // The note's note-off: it fades, and its string's release sounds.
  void end(foundation::IdBits clip, foundation::IdBits note) noexcept {
    for (auto& v : notes_v_) {
      if (!v.active || !v.held || !(v.clip == clip) || !(v.note == note)) {
        continue;
      }
      v.held = false;
      v.fade_step = std::max(v.fade_step, release_step_);
      release_noise(v);
      return;
    }
  }

  void release_noise(const Voice& note) noexcept {
    const double level = values_[p::release] / ft::library_level;
    if (level <= 0.0 || library_ == nullptr || library_->layers(Kind::release, note.key) == 0) {
      return;
    }
    const auto robins = library_->round_robins(Kind::release, note.key, 0);
    auto& counter = robins_[static_cast<std::size_t>(Kind::release)]
                           [static_cast<std::size_t>(note.key - first_key)];
    const auto index = robins[counter++ % robins.size()];
    auto* free = std::ranges::find_if(releases_, [](const Voice& v) { return !v.active; });
    if (free == releases_.end()) {
      free = std::ranges::min_element(releases_, {}, &Voice::started);
    }
    const double held = static_cast<double>(clock_ - note.started) / rate_;
    auto& v = *free;
    v = Voice{};
    v.active = true;
    v.index = index;
    v.sample = &library_->stream(index);
    v.resident = library_->resident(index);
    v.key = note.key;
    v.gain = level * static_cast<double>(note.velocity) / max_velocity *
             db_to_gain(-release_decay_db_per_second * held);
    v.started = clock_;
  }

  void render_span(std::span<engine::StereoFrame> output) noexcept {
    if (output.empty()) {
      return;
    }
    std::array<double, max_frames> mix{};
    for (std::size_t offset = 0; offset < output.size(); offset += max_frames) {
      const std::size_t frames = std::min(max_frames, output.size() - offset);
      std::fill_n(mix.begin(), frames, 0.0);
      for (std::size_t slot = 0; slot < note_voices; ++slot) {
        if (notes_v_[slot].active && render_voice(notes_v_[slot], slot, mix, frames)) {
          ++underruns_;
        }
      }
      for (auto& v : releases_) {
        if (v.active) {
          (void)render_voice(v, note_voices, mix, frames);
        }
      }
      tone(mix, output.subspan(offset, frames));
    }
  }

  // Additive. True when the voice needed streamed frames that had not
  // arrived (heard as silence for those frames).
  bool render_voice(Voice& v, std::size_t slot, std::array<double, max_frames>& mix,
                    std::size_t frames) noexcept {
    const auto& sample = *v.sample;
    const bool streaming = slot < note_voices && v.streams;
    const std::int16_t* ring = nullptr;
    if (streaming) {
      ring = streams_->ring(slot);
      const auto word = streams_->slot(slot).progress.load(std::memory_order_acquire);
      if (sample_stream::generation_of(word) == generations_[slot]) {
        v.streamed = sample_stream::frames_of(word);
      }
    }
    const double tune = values_[p::tune] / cents_per_semitone;
    const auto step_at = [&](double semitones) {
      return base_step_ * std::exp2((semitones + tune) / semitones_per_octave);
    };
    const double fastest = std::max(step_at(v.semitones), step_at(v.target));
    const auto& table = fastest > 1.0 ? rising_ : falling_;
    const bool exact = v.semitones == 0.0 && v.glide == 0.0 && tune == 0.0 && base_step_ == 1.0;
    const auto per_output = static_cast<std::size_t>(std::ceil(fastest));
    const std::size_t per_pass = std::max<std::size_t>(1, (scratch_frames - SincTable::taps - 2) /
                                                              std::max<std::size_t>(1, per_output));
    bool starved = false;
    std::size_t done = 0;
    while (done < frames) {
      const std::size_t count = std::min(frames - done, per_pass);
      const auto first = static_cast<std::int64_t>(std::floor(v.position)) -
                         static_cast<std::int64_t>(SincTable::left);
      const double reach = v.position + static_cast<double>(count) * fastest;
      const auto last = static_cast<std::int64_t>(std::floor(reach)) +
                        static_cast<std::int64_t>(SincTable::taps) - SincTable::left;
      const auto span = std::min(static_cast<std::size_t>(last - first + 1), scratch_frames);
      starved = sample_stream::gather(sample, library::pack_channels, v.resident, ring, v.streamed,
                                      first, span, scratch_.data(), scratch_right_.data()) ||
                starved;
      for (std::size_t i = 0; i < count; ++i) {
        const double base = std::floor(v.position);
        const auto at = static_cast<std::size_t>(static_cast<std::int64_t>(base) - first);
        double x = 0.0;
        if (exact) {
          x = scratch_[at];
        } else {
          x = table.read(scratch_.data() + at - static_cast<std::size_t>(SincTable::left),
                         v.position - base);
        }
        mix[done + i] += x * v.gain * v.fade;
        if (v.fade_step > 0.0) {
          v.fade = std::max(0.0, v.fade - v.fade_step);
        }
        if (v.glide != 0.0) {
          v.semitones += v.glide;
          if ((v.glide > 0.0 && v.semitones >= v.target) ||
              (v.glide < 0.0 && v.semitones <= v.target)) {
            v.semitones = v.target;
            v.glide = 0.0;
          }
        }
        v.position += step_at(v.semitones);
      }
      done += count;
    }
    const auto next = static_cast<std::int64_t>(std::floor(v.position)) -
                      static_cast<std::int64_t>(SincTable::left);
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
    if (next >= static_cast<std::int64_t>(sample.frames) || (v.fade_step > 0.0 && v.fade <= 0.0)) {
      if (slot < note_voices) {
        free_note(slot);
      } else {
        v.active = false;
      }
    }
    return starved;
  }

  // The tone stage on the mono mix, centred into the output.
  void tone(const std::array<double, max_frames>& mix,
            std::span<engine::StereoFrame> output) noexcept {
    const double amp = values_[p::amp];
    const bool squashing = ratio_ > 1.0;
    const double threshold = db_to_gain(squash_threshold_db);
    const double bias = std::tanh(drive_ * amp_bias);
    for (std::size_t i = 0; i < output.size(); ++i) {
      double x = mix[i];
      if (amp > 0.0) {
        const double band = high_cut_.run(low_cut_.run(x));
        const double driven = (std::tanh(drive_ * (band + amp_bias)) - bias) / drive_;
        x = (1.0 - amp) * x + amp * driven;
      }
      tilt_low_state_ += tilt_coefficient_ * (x - tilt_low_state_);
      x = tilt_low_ * tilt_low_state_ + tilt_high_ * (x - tilt_low_state_);
      if (squashing) {
        const double magnitude = std::fabs(x);
        const double coefficient = magnitude > envelope_ ? attack_ : release_;
        envelope_ = magnitude + coefficient * (envelope_ - magnitude);
        if (envelope_ > threshold) {
          const double over_db = 20.0 * std::log10(envelope_ / threshold);
          x *= db_to_gain(-over_db * (1.0 - 1.0 / ratio_));
        }
        x *= makeup_;
      }
      x = safety(x * gain_);
      gain_ += gain_step_;
      if (declick_left_ > 0.0) {
        x += declick_ * (declick_left_ / declick_frames_);
        declick_left_ -= 1.0;
      }
      last_output_ = x;
      peak_ = std::max(peak_, std::fabs(x));
      const auto out = static_cast<float>(x);
      output[i].left += out;
      output[i].right += out;
    }
  }

  double rate_;
  library::State state_;
  std::unique_ptr<sample_stream::Streams> streams_;
  double base_step_;
  SincTable falling_;
  SincTable rising_;
  const detail::Library* library_ = nullptr;
  std::span<const double> values_{};
  Transport transport_{};
  std::size_t frames_since_tick_ = 0;
  std::uint64_t clock_ = 0;
  std::array<Voice, note_voices> notes_v_{};
  std::array<Voice, release_voices> releases_{};
  std::array<std::uint32_t, note_voices> generations_{};
  std::array<std::array<std::uint32_t, key_count>, detail::kind_count> robins_{};
  std::array<Pending, max_pending> pending_{};
  std::size_t pending_count_ = 0;
  std::array<Delayed, max_delayed> delayed_{};
  std::size_t delayed_count_ = 0;
  feel::Previous previous_{};
  std::array<float, scratch_frames> scratch_{};
  std::array<float, scratch_frames> scratch_right_{};
  double retrigger_step_ = 0.0;
  double release_step_ = 0.0;
  Biquad low_cut_;
  Biquad high_cut_;
  double tilt_coefficient_ = 0.0;
  double tilt_low_state_ = 0.0;
  double tilt_low_ = 1.0;
  double tilt_high_ = 1.0;
  double attack_ = 0.0;
  double release_ = 0.0;
  double envelope_ = 0.0;
  double ratio_ = 1.0;
  double makeup_ = 1.0;
  double drive_ = amp_drive;
  double gain_ = 0.0;
  double gain_step_ = 0.0;
  bool primed_ = false;
  double declick_frames_ = 1.0;
  double declick_left_ = 0.0;
  double declick_ = 0.0;
  double last_output_ = 0.0;
  double peak_ = 0.0;
  std::uint64_t underruns_ = 0;
  std::uint32_t notes_ = 0;
  int last_key_ = -1;
  int last_velocity_ = 0;
  int last_articulation_ = 0;
  bool last_pushed_ = false;
};

std::unique_ptr<SynthKernel> create_kernel(double sample_rate) {
  // Control: the first Fret loads the library (its resident starts).
  const auto status = library::status();
  return std::make_unique<FretKernel>(sample_rate, status.state);
}

// The longest a released note can sound: its release fade or its string's
// release sample, played late by Pocket and slid down an octave at most.
std::uint64_t tail_frames(std::span<const double> /*values*/, double sample_rate) {
  (void)library::status();
  double longest = release_seconds;
  if (const auto* library = detail::current(); library != nullptr) {
    const double slowest = std::exp2(
        (max_slide_semitones + ft::max_tune_cents / cents_per_semitone) / semitones_per_octave);
    longest = std::max(longest, static_cast<double>(library->longest_release()) /
                                    library::pack_rate * slowest);
  }
  const double total =
      longest + feel::max_pocket_ms / ms_per_second + declick_seconds + tail_margin_seconds;
  return static_cast<std::uint64_t>(std::ceil(total * sample_rate));
}

const EngineDescriptor descriptor{"fret", "Fret", 1, ft::parameters, fret_presets::presets};
// A bass ignores the pedals: the kernel models them by doing nothing.
const SynthEngine fret_engine{&descriptor, &create_kernel, &tail_frames, true};

} // namespace

const SynthEngine& engine() { return fret_engine; }

feel::Settings feel_settings(std::span<const double> values) noexcept {
  feel::Settings settings;
  if (values.size() != ft::parameters.size()) {
    return settings;
  }
  settings.style = static_cast<feel::Style>(choice(values[p::style], ft::styles.size()));
  settings.amount = values[p::amount];
  settings.dynamics = values[p::dynamics];
  settings.ghosts = choice(values[p::ghosts], ft::ghost_modes.size()) ==
                    static_cast<std::size_t>(ft::ghosts_auto);
  settings.humanize = values[p::humanize];
  settings.pocket_ms = values[p::pocket];
  return settings;
}

feel::Articulation played_articulation(std::span<const double> values,
                                       feel::Articulation decided) noexcept {
  if (values.size() != ft::parameters.size()) {
    return decided;
  }
  switch (static_cast<ft::ArticulationMode>(
      choice(values[p::articulation], ft::articulations.size()))) {
  case ft::auto_articulation:
    break;
  case ft::pluck:
    return feel::Articulation::pluck;
  case ft::staccato:
    return feel::Articulation::staccato;
  case ft::ghost:
    return feel::Articulation::ghost;
  case ft::bridge:
    return feel::Articulation::bridge;
  }
  return decided;
}

} // namespace oscillate::native_dsp::synth::fret
