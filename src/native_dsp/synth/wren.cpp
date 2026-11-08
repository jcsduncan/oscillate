#include "oscillate/native_dsp/synth/wren.hpp"

#include "dsp/biquad.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/effects/expanse.hpp"
#include "oscillate/native_dsp/parameter_control.hpp"
#include "oscillate/native_dsp/synth/sample_stream.hpp"
#include "oscillate/native_dsp/synth/wren_breath.hpp"
#include "oscillate/native_dsp/transport.hpp"
#include "wren_library.hpp"
#include "wren_presets.hpp"
#include "wren_table.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace oscillate::native_dsp::synth::wren {
namespace {

namespace wt = wren_table;
using detail::Kind;
using sample_stream::SincTable;

namespace p {
constexpr std::size_t style = wt::index_of("wren.breath.style");
constexpr std::size_t amount = wt::index_of("wren.breath.amount");
constexpr std::size_t dynamics = wt::index_of("wren.breath.dynamics");
constexpr std::size_t vibrato = wt::index_of("wren.breath.vibrato");
constexpr std::size_t bloom = wt::index_of("wren.breath.bloom");
constexpr std::size_t swell = wt::index_of("wren.breath.swell");
constexpr std::size_t ornaments = wt::index_of("wren.breath.ornaments");
constexpr std::size_t humanize = wt::index_of("wren.breath.humanize");
constexpr std::size_t articulation = wt::index_of("wren.articulation");
constexpr std::size_t slur = wt::index_of("wren.slur");
constexpr std::size_t voices = wt::index_of("wren.voices");
constexpr std::size_t air = wt::index_of("wren.tone.air");
constexpr std::size_t warmth = wt::index_of("wren.tone.warmth");
constexpr std::size_t space = wt::index_of("wren.tone.space");
constexpr std::size_t tune = wt::index_of("wren.tune");
constexpr std::size_t level = wt::index_of("wren.level");
} // namespace p

constexpr std::size_t max_frames = control_interval_frames;
// Poly 4's held notes, plus voices crossfading or releasing out.
constexpr std::size_t max_voices = 8;
constexpr std::size_t poly_voices = 4;
constexpr std::size_t max_pending = 64;
// Frames gathered per resampling pass.
constexpr std::size_t scratch_frames = 512;
constexpr double semitones_per_octave = 12.0;
constexpr double cents_per_semitone = 100.0;
constexpr double ms_per_second = 1'000.0;
constexpr double quarter_turn = std::numbers::pi / 2.0;

// Velocity picks the layer by hard splits (pp below 60, mf below 100, ff
// from 100) and trims within it, by up to this much at its bottom.
constexpr std::array<int, layer_count + 1> layer_floors{0, 60, 100, 127};
constexpr double layer_trim_db = -6.0;
// Note-off: a release while the loop holds; shorter for staccato.
constexpr double release_seconds = 0.090;
constexpr double staccato_release_seconds = 0.040;
// Under the Staccato articulation every note is also this much lighter.
// Chosen by ear on the prototype's renders: not verified on the library.
constexpr double staccato_light_db = -3.0;
// A slurred-from voice bends to the new pitch over this share of the slur,
// unless the interval is wider than an octave (then it only fades).
constexpr double bend_share = 1.0 / 3.0;
constexpr double max_bend_semitones = 12.0;
// Swell (the prototype's curve): over the note's length u (0..1) the gain
// leans in by Swell x (sin(pi min(1, speed u))^shape - centre) dB: up past
// the middle, eased off before the end, centred so the note keeps its
// level on average.
constexpr double swell_speed = 1.1;
constexpr double swell_shape = 1.5;
constexpr double swell_centre = 0.5;
// A flute brightens as it gets louder: Air follows the swell at this many
// dB per dB, on each voice's own first-order shelf.
constexpr double air_per_swell_db = 0.5;
// Envelopes (fades, swell, the vibrato blend) are evaluated every this many
// frames of a voice's life and interpolated between: on the voice's own
// clock, so any block split renders the same.
constexpr std::uint64_t envelope_step = 16;
// Tone: the Air and Warmth shelves' corners.
constexpr double air_hz = 5'000.0;
constexpr double warmth_hz = 350.0;
// Space: a fixed small hall on Expanse, its mix the Space control.
constexpr double room_size = 0.4;
constexpr double room_decay_seconds = 2.6;
constexpr double room_predelay_ms = 22.0;
constexpr double mute_db = -59.9;
constexpr double meter_floor_db = -120.0;
constexpr double safety_knee = 0.9;
constexpr double declick_seconds = 0.0015;
constexpr double tail_margin_seconds = 0.25;
// Breath reads the song position at this resolution, so a transport's
// rounding under different block splits never moves a decision.
constexpr double beat_resolution = 65'536.0;
// Guards a transport without a meter, or without a tempo.
constexpr double min_beats_per_bar = 1.0 / 64.0;
constexpr double min_beats_per_frame = 1e-12;
// The resampler's tables: one per sixth of an octave of reading speed up to
// five octaves, each cut off so reading that fast cannot alias.
constexpr std::size_t tables_per_octave = 6;
constexpr std::size_t table_octaves = 5;
constexpr std::size_t table_count = tables_per_octave * table_octaves + 1;

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

[[nodiscard]] std::uint32_t layer_of(int velocity) noexcept {
  std::uint32_t layer = 0;
  while (layer + 1 < layer_count && velocity >= layer_floors[layer + 1]) {
    ++layer;
  }
  return layer;
}

// Up to layer_trim_db at the layer's bottom, unity at its top.
[[nodiscard]] double layer_trim(int velocity, std::uint32_t layer) noexcept {
  const double floor = layer_floors[layer];
  const double span = layer_floors[layer + 1] - floor;
  const double within = std::clamp((static_cast<double>(velocity) - floor) / span, 0.0, 1.0);
  return db_to_gain(layer_trim_db * (1.0 - within));
}

// The resampler's tables, shared by every Wren: built once on control.
const std::vector<SincTable>& tables() {
  static const auto* built = [] {
    auto* all = new std::vector<SincTable>();
    all->reserve(table_count);
    for (std::size_t t = 0; t < table_count; ++t) {
      all->emplace_back(SincTable::passband /
                        std::exp2(static_cast<double>(t) / static_cast<double>(tables_per_octave)));
    }
    return all;
  }();
  return *built;
}

[[nodiscard]] const SincTable& table_for(const std::vector<SincTable>& all, double step) noexcept {
  if (step <= 1.0) {
    return all.front();
  }
  const auto t =
      static_cast<std::size_t>(std::ceil(std::log2(step) * static_cast<double>(tables_per_octave)));
  return all[std::min(t, all.size() - 1)];
}

// One take of a note, read from memory and looped while the note is held.
struct Reader {
  const sample_stream::Sample* stream = nullptr;
  const std::int16_t* pcm = nullptr;
  std::int64_t loop_start = 0;
  std::int64_t loop_end = 0;
  double position = 0.0;
  double offset = 0.0; // Semitones from the sample's own pitch to the note's.
  bool looping = true;
};

enum class Fade : std::uint8_t { none, linear, equal_power };

struct Voice {
  bool active = false;
  bool held = false;
  int key = 0;
  foundation::IdBits clip{};
  foundation::IdBits note{};
  Reader plain;
  Reader vibrato;
  bool has_vibrato = false;
  double depth = 0.0; // Breath's vibrato share, before the Vibrato control.
  double delay_frames = 0.0;
  double bloom_frames = 1.0;
  double swell_db = 0.0;
  double length_frames = 0.0; // 0 when unknown.
  double gain = 0.0;
  double cents = 0.0; // Humanize.
  // Fades in from its start and out from out_age.
  Fade in = Fade::none;
  double in_frames = 1.0;
  Fade out = Fade::none;
  double out_frames = 1.0;
  std::uint64_t out_age = 0;
  // A slur's bend toward the next note's pitch.
  double bend = 0.0;
  double bend_target = 0.0;
  double bend_step = 0.0;
  std::uint64_t age = 0;
  std::uint64_t started = 0;
  // Envelope values now and their steps per frame to the next grid point.
  double level = 0.0;
  double level_step = 0.0;
  double plain_weight = 1.0;
  double plain_step = 0.0;
  double vibrato_weight = 0.0;
  double vibrato_step = 0.0;
  double air = 1.0;
  double air_step = 0.0;
  double air_low = 0.0; // The voice's shelf state.
};

struct Start {
  int key = 0;
  int velocity = 0;
  breath::Decision decision{};
  double length_frames = 0.0;
  foundation::IdBits clip{};
  foundation::IdBits note{};
};

// A note that starts after its grace, or a staccato note's end.
struct Pending {
  std::uint64_t frame = 0; // On the kernel's clock.
  bool start = true;
  Start note{};
};

class WrenKernel final : public SynthKernel {
public:
  WrenKernel(double sample_rate, library::State state)
      : rate_(sample_rate), state_(state),
        base_step_(static_cast<double>(library::pack_rate) / sample_rate), tables_(tables()),
        room_(effects::expanse::engine().create_kernel(room_defaults(), sample_rate)) {
    room_values_ = room_defaults();
    declick_frames_ = std::max(1.0, declick_seconds * sample_rate);
    air_coefficient_ = native_dsp::dsp::one_pole_coefficient(air_hz, sample_rate);
    library_ = detail::current();
    if (library_ != nullptr) {
      state_ = library::State::ready;
    }
  }

  // The hall Space plays through: Expanse's defaults with the fixed hall's
  // size, decay and pre-delay. The mix follows Space.
  static std::vector<double> room_defaults() {
    const auto& room = *effects::expanse::engine().descriptor;
    auto values = default_values(room);
    const auto set = [&](std::string_view id, double value) {
      if (const auto at = find_parameter(room, id)) {
        values[*at] = value;
      }
    };
    set("expanse.size", room_size);
    set("expanse.decay", room_decay_seconds);
    set("expanse.predelay", room_predelay_ms);
    set("expanse.mix", 0.0);
    return values;
  }

  void control(std::span<const double> values, std::uint32_t frames_to_next_tick,
               const Transport& transport) noexcept override {
    values_ = values;
    transport_ = transport;
    frames_since_tick_ = 0;
    const bool idle = std::ranges::none_of(voices_, &Voice::active) && pending_count_ == 0;
    if (idle) {
      if (const auto* latest = detail::current(); latest != nullptr && latest != library_) {
        library_ = latest;
        state_ = library::State::ready;
      }
      poly_ =
          choice(values[p::voices], wt::voice_modes.size()) == static_cast<std::size_t>(wt::poly);
    }
    peak_ = 0.0;
    const double gain =
        values[p::level] <= mute_db ? 0.0 : db_to_gain(values[p::level] + headroom_db);
    gain_step_ = primed_ ? (gain - gain_) / std::max<double>(1.0, frames_to_next_tick) : 0.0;
    if (!primed_) {
      gain_ = gain;
    }
    if (values[p::air] != air_db_ || !primed_) {
      air_db_ = values[p::air];
      air_shelf_.set(dsp::rbj_shelf(true, db_to_gain(air_db_), {.hz = air_hz, .rate = rate_}));
    }
    if (values[p::warmth] != warmth_db_ || !primed_) {
      warmth_db_ = values[p::warmth];
      warmth_shelf_.set(
          dsp::rbj_shelf(false, db_to_gain(warmth_db_), {.hz = warmth_hz, .rate = rate_}));
    }
    // Space: the hall runs while its mix is above zero or gliding there.
    const double space = values[p::space];
    if (room_ != nullptr) {
      if (space > 0.0 && !room_running_) {
        room_->reset();
        room_running_ = true;
      } else if (space <= 0.0 && room_target_ <= 0.0) {
        room_running_ = false;
      }
      room_target_ = space;
      if (room_running_) {
        room_values_[room_mix_] = space;
        room_->control(room_values_, frames_to_next_tick, transport);
      }
    }
    primed_ = true;
  }

  void note_on(const NoteEvent& event) noexcept override {
    if (values_.empty() || library_ == nullptr || event.key < first_key || event.key > last_key) {
      return;
    }
    const double per_frame = beats_per_frame();
    const double beats_per_bar = std::max(transport_.beats_per_bar, min_beats_per_bar);
    const double bars = transport_.bar_at_block_start +
                        static_cast<double>(frames_since_tick_) * per_frame / beats_per_bar;
    const double bar = std::floor(bars);
    breath::Context context;
    context.beat = beat_now();
    context.bar_beat = rounded((bars - bar) * beats_per_bar);
    context.bar = static_cast<std::int64_t>(bar);
    context.beats_per_bar = beats_per_bar;
    context.length_beats = static_cast<double>(event.frames) * per_frame;
    context.key = event.key;
    context.velocity = event.velocity;
    context.scale = scale_;
    auto decision = breath::decide(context, previous_, breath_settings(values_));
    // The panel's lane previews through the same rules (Poly 4 tongues every
    // note; a grace above the top key is dropped).
    const Played play = played(values_, event.key, context.length_beats, decision);
    decision.tongue = play.tongue;
    decision.grace = play.grace;
    previous_ = breath::after(context, decision);
    previous_clip_ = event.clip;
    previous_note_ = event.note;

    Start start{event.key,  decision.velocity, decision, static_cast<double>(event.frames),
                event.clip, event.note};
    last_key_ = event.key;
    last_velocity_ = decision.velocity;
    last_tongue_ = static_cast<int>(decision.tongue);
    last_staccato_ = decision.staccato;
    last_vibrato_ = play.vibrato > 0.0;
    last_grace_ = false;
    ++notes_;

    // A grace queues up to a staccato stop, its own stop and the note.
    constexpr std::size_t grace_queue_entries = 3;
    const bool graced = decision.grace && pending_count_ + grace_queue_entries <= max_pending;
    const auto grace_frames =
        graced ? static_cast<std::uint64_t>(std::llround(decision.grace_ms * rate_ / ms_per_second))
               : std::uint64_t{0};
    if (forced_staccato()) {
      // Every note short: held a third of a beat at most, and stopped only
      // after a grace hands over, so the main note always gets its stop.
      const auto hold = std::max<std::uint64_t>(
          static_cast<std::uint64_t>(
              std::llround(breath::staccato_beats / std::max(per_frame, min_beats_per_frame))),
          graced ? grace_frames + 1 : 0);
      if (event.frames == 0 || event.frames > hold) {
        start.length_frames = static_cast<double>(hold);
        Pending stop;
        stop.frame = clock_ + hold;
        stop.start = false;
        stop.note = start;
        (void)enqueue(stop);
      }
    }
    const int grace_key = event.key + decision.grace_step;
    if (graced) {
      // The grace on the beat, then the note slurred from it.
      Start grace = start;
      grace.key = grace_key;
      grace.decision.vibrato = 0.0;
      grace.decision.swell_db = 0.0;
      grace.length_frames = static_cast<double>(grace_frames);
      begin(grace);
      if (poly_) {
        // Poly 4 does not slur, so the grace ends where the note begins
        // (queued first, it stops before the note starts on that frame).
        Pending grace_stop;
        grace_stop.frame = clock_ + grace_frames;
        grace_stop.start = false;
        grace_stop.note = grace;
        (void)enqueue(grace_stop);
      }
      Pending main;
      main.frame = clock_ + grace_frames;
      main.note = start;
      // Mono slurs from the grace; Poly 4 never slurs, so the note keeps its
      // tongued attack, as the panel's preview shows.
      if (!poly_) {
        main.note.decision.tongue = breath::Tongue::slur;
      }
      main.note.length_frames =
          std::max(0.0, start.length_frames - static_cast<double>(grace_frames));
      (void)enqueue(main);
      last_grace_ = true;
      return;
    }
    begin(start);
  }

  void note_off(const NoteEvent& event) noexcept override {
    if (previous_.valid && !previous_.ended && previous_clip_ == event.clip &&
        previous_note_ == event.note) {
      previous_.ended = true;
      previous_.end_beat = beat_now();
    }
    // A note still waiting for its grace to end never starts.
    for (std::size_t i = 0; i < pending_count_;) {
      if (pending_[i].start && pending_[i].note.clip == event.clip &&
          pending_[i].note.note == event.note) {
        std::copy(pending_.begin() + static_cast<std::ptrdiff_t>(i + 1),
                  pending_.begin() + static_cast<std::ptrdiff_t>(pending_count_),
                  pending_.begin() + static_cast<std::ptrdiff_t>(i));
        --pending_count_;
      } else {
        ++i;
      }
    }
    end(event.clip, event.note);
  }

  // Split at each pending event due inside the span, so it lands on its
  // frame however the host splits its blocks.
  void render(std::span<engine::StereoFrame> output) noexcept override {
    std::size_t done = 0;
    while (done < output.size()) {
      while (pending_count_ > 0 && pending_[0].frame <= clock_) {
        const Pending due = pending_[0];
        std::copy(pending_.begin() + 1,
                  pending_.begin() + static_cast<std::ptrdiff_t>(pending_count_), pending_.begin());
        --pending_count_;
        if (due.start) {
          begin(due.note);
        } else {
          end(due.note.clip, due.note.note);
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
    for (auto& v : voices_) {
      v.active = false;
    }
    pending_count_ = 0;
    previous_ = {};
    air_shelf_.reset();
    warmth_shelf_.reset();
    if (room_ != nullptr) {
      room_->reset();
    }
  }

  [[nodiscard]] KernelStatus status() const noexcept override {
    KernelStatus status;
    status.active_voices =
        static_cast<std::uint32_t>(std::ranges::count_if(voices_, &Voice::active));
    return status;
  }

  [[nodiscard]] std::size_t telemetry(std::span<float> out) const noexcept override {
    if (out.size() < telemetry::size) {
      return 0;
    }
    out[telemetry::library] = static_cast<float>(state_);
    out[telemetry::notes] = static_cast<float>(notes_ & telemetry_wrap);
    out[telemetry::last_key] = static_cast<float>(last_key_);
    out[telemetry::last_velocity] = static_cast<float>(last_velocity_);
    out[telemetry::last_tongue] = static_cast<float>(last_tongue_);
    out[telemetry::last_staccato] = last_staccato_ ? 1.0F : 0.0F;
    out[telemetry::last_grace] = last_grace_ ? 1.0F : 0.0F;
    out[telemetry::vibrato] = last_vibrato_ ? 1.0F : 0.0F;
    const double peak_db = peak_ > 0.0 ? 20.0 * std::log10(peak_) : meter_floor_db;
    out[telemetry::output_peak_db] = static_cast<float>(std::max(meter_floor_db, peak_db));
    return telemetry::size;
  }

  void project_key(std::optional<project::MusicalKey> key) noexcept override {
    scale_ = breath::scale_of(key);
  }

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
  [[nodiscard]] bool forced_staccato() const noexcept {
    return choice(values_[p::articulation], wt::articulations.size()) ==
           static_cast<std::size_t>(breath::Articulation::staccato);
  }
  [[nodiscard]] double frames_of_ms(double ms) const noexcept {
    return std::max(1.0, ms * rate_ / ms_per_second);
  }

  bool enqueue(const Pending& pending) noexcept {
    if (pending_count_ >= max_pending) {
      return false;
    }
    std::size_t at = pending_count_;
    while (at > 0 && pending_[at - 1].frame > pending.frame) {
      pending_[at] = pending_[at - 1];
      --at;
    }
    pending_[at] = pending;
    ++pending_count_;
    return true;
  }

  // The newest held voice, or null.
  [[nodiscard]] Voice* sounding() noexcept {
    Voice* newest = nullptr;
    for (auto& v : voices_) {
      if (v.active && v.held && (newest == nullptr || v.started >= newest->started)) {
        newest = &v;
      }
    }
    return newest;
  }

  // Fades a voice out from now: a slur's crossfade or a release.
  static void fade_out(Voice& v, Fade fade, double frames) noexcept {
    v.held = false;
    v.out = fade;
    v.out_frames = std::max(1.0, frames);
    v.out_age = v.age;
  }

  // A free voice, else the oldest released one's, else the oldest.
  [[nodiscard]] Voice& free_voice() noexcept {
    Voice* oldest_released = nullptr;
    Voice* oldest = &voices_.front();
    for (auto& v : voices_) {
      if (!v.active) {
        return v;
      }
      if (!v.held && (oldest_released == nullptr || v.started < oldest_released->started)) {
        oldest_released = &v;
      }
      if (v.started < oldest->started) {
        oldest = &v;
      }
    }
    return oldest_released != nullptr ? *oldest_released : *oldest;
  }

  void set_reader(Reader& r, std::uint32_t index, int key, double position) const noexcept {
    const auto& info = library_->sample(index);
    r.stream = &library_->stream(index);
    r.pcm = library_->resident(index);
    r.loop_start = info.loop_start;
    r.loop_end = info.loop_end;
    r.position = std::min(position, static_cast<double>(r.stream->frames - 1));
    r.offset = static_cast<double>(key - info.key) - info.cents / cents_per_semitone;
    r.looping = true;
  }

  // Starts a note: slurred from the sounding note (or past its attack when
  // none sounds), or tongued.
  void begin(const Start& n) noexcept {
    if (library_ == nullptr || n.key < first_key || n.key > last_key) {
      return;
    }
    const auto& d = n.decision;
    const double slur_frames = frames_of_ms(values_[p::slur]);
    Voice* from = sounding();
    const bool slurred = d.tongue == breath::Tongue::slur;
    if (slurred && from != nullptr && !poly_) {
      const double interval = static_cast<double>(n.key - from->key);
      if (std::fabs(interval) <= max_bend_semitones) {
        from->bend_target = from->bend + interval;
        from->bend_step = interval / std::max(1.0, bend_share * slur_frames);
      }
      fade_out(*from, Fade::equal_power, slur_frames);
    } else if (!poly_) {
      // Mono: a tongued note stops the one before.
      for (auto& v : voices_) {
        if (v.active && v.held) {
          fade_out(v, Fade::equal_power, slur_frames);
        }
      }
    } else {
      // Poly 4: the oldest held note gives way to a fifth.
      std::size_t held = 0;
      Voice* oldest = nullptr;
      for (auto& v : voices_) {
        if (v.active && v.held) {
          ++held;
          if (oldest == nullptr || v.started < oldest->started) {
            oldest = &v;
          }
        }
      }
      if (held >= poly_voices && oldest != nullptr) {
        fade_out(*oldest, Fade::equal_power, slur_frames);
      }
    }

    const auto layer = layer_of(n.velocity);
    const auto plain = library_->pick(Kind::plain, n.key, layer);
    Voice& v = free_voice();
    v = Voice{};
    v.active = true;
    v.held = true;
    v.key = n.key;
    v.clip = n.clip;
    v.note = n.note;
    v.started = clock_;
    double gain = layer_trim(n.velocity, layer);
    double start = 0.0;
    switch (d.tongue) {
    case breath::Tongue::slur:
      // Past the attack, crossfading in.
      start = library_->sample(plain).attack_end;
      v.in = Fade::equal_power;
      v.in_frames = slur_frames;
      break;
    case breath::Tongue::k:
      // The softer back of the tongue: its first moments faded in.
      start = breath::k_onset_ms * library::pack_rate / ms_per_second;
      v.in = Fade::linear;
      v.in_frames = frames_of_ms(breath::k_onset_ms);
      gain *= db_to_gain(breath::k_tongue_db);
      break;
    case breath::Tongue::t:
      break;
    }
    if (forced_staccato()) {
      gain *= db_to_gain(staccato_light_db);
    }
    v.gain = gain;
    set_reader(v.plain, plain, n.key, start);
    v.depth = d.vibrato;
    v.has_vibrato = d.vibrato > 0.0;
    if (v.has_vibrato) {
      // Read from the same position as the plain take.
      set_reader(v.vibrato, library_->pick(Kind::vibrato, n.key, layer), n.key, start);
      v.delay_frames = d.vibrato_delay_ms * rate_ / ms_per_second;
      v.bloom_frames = frames_of_ms(d.bloom_ms);
    }
    v.swell_db = n.length_frames > 0.0 ? d.swell_db : 0.0;
    v.length_frames = n.length_frames;
    v.cents = d.cents;
    v.out_frames = (d.staccato ? staccato_release_seconds : release_seconds) * rate_;
    // The envelope from its first frame.
    v.level = level_at(v, 0.0);
    v.plain_weight = plain_weight_at(v, 0.0);
    v.vibrato_weight = vibrato_weight_at(v, 0.0);
    v.air = air_at(v, 0.0);
  }

  // The note's note-off: it fades over its release, still looping (past the
  // loop end the recording is unbaked: a seam the loop never reaches).
  void end(foundation::IdBits clip, foundation::IdBits note) noexcept {
    for (auto& v : voices_) {
      if (!v.active || !v.held || !(v.clip == clip) || !(v.note == note)) {
        continue;
      }
      fade_out(v, Fade::linear, v.out_frames);
    }
  }

  // ---- Envelopes ------------------------------------------------------------------------

  [[nodiscard]] static double swell_db_at(const Voice& v, double age) noexcept {
    if (v.swell_db <= 0.0 || v.length_frames <= 0.0) {
      return 0.0;
    }
    const double u = std::min(1.0, swell_speed * age / v.length_frames);
    return v.swell_db * (std::pow(std::sin(std::numbers::pi * u), swell_shape) - swell_centre);
  }

  [[nodiscard]] static double level_at(const Voice& v, double age) noexcept {
    double in = 1.0;
    if (v.in != Fade::none && age < v.in_frames) {
      const double u = age / v.in_frames;
      in = v.in == Fade::equal_power ? std::sin(quarter_turn * u) : u;
    }
    double out = 1.0;
    if (v.out != Fade::none) {
      const double u = std::clamp((age - static_cast<double>(v.out_age)) / v.out_frames, 0.0, 1.0);
      out = v.out == Fade::equal_power ? std::cos(quarter_turn * u) : 1.0 - u;
    }
    return v.gain * in * out * db_to_gain(swell_db_at(v, age));
  }

  // The vibrato take's share: rising as a square over Bloom after the delay.
  [[nodiscard]] double blend_at(const Voice& v, double age) const noexcept {
    if (!v.has_vibrato) {
      return 0.0;
    }
    const double rise = std::clamp((age - v.delay_frames) / v.bloom_frames, 0.0, 1.0);
    return v.depth * values_[p::vibrato] * rise * rise;
  }
  [[nodiscard]] double plain_weight_at(const Voice& v, double age) const noexcept {
    return std::cos(quarter_turn * blend_at(v, age));
  }
  [[nodiscard]] double vibrato_weight_at(const Voice& v, double age) const noexcept {
    return std::sin(quarter_turn * blend_at(v, age));
  }
  [[nodiscard]] static double air_at(const Voice& v, double age) noexcept {
    return db_to_gain(air_per_swell_db * swell_db_at(v, age));
  }

  void retarget(Voice& v) const noexcept {
    const double next = static_cast<double>(v.age + envelope_step);
    constexpr auto steps = static_cast<double>(envelope_step);
    v.level_step = (level_at(v, next) - v.level) / steps;
    v.plain_step = (plain_weight_at(v, next) - v.plain_weight) / steps;
    v.vibrato_step = (vibrato_weight_at(v, next) - v.vibrato_weight) / steps;
    v.air_step = (air_at(v, next) - v.air) / steps;
  }

  // ---- Reading ----------------------------------------------------------------------------

  // Source frames [first, first + count) of a take as floats: looped while
  // held, silence outside the recording.
  static void gather(const Reader& r, std::int64_t first, std::size_t count, float* out) noexcept {
    const float scale = r.stream->scale;
    const auto frames = static_cast<std::int64_t>(r.stream->frames);
    const std::int64_t length = r.loop_end - r.loop_start;
    for (std::size_t j = 0; j < count; ++j) {
      std::int64_t i = first + static_cast<std::int64_t>(j);
      if (r.looping && i >= r.loop_end) {
        i = r.loop_start + (i - r.loop_end) % length;
      }
      out[j] = i >= 0 && i < frames ? static_cast<float>(r.pcm[i]) * scale : 0.0F;
    }
  }

  // Reads `count` frames of a take into `out` (null: only moves on),
  // stepping by `steps` per frame. A held take jumps back to its loop start
  // on the frame it reaches its loop end, whatever the pass.
  void read(Reader& r, const double* steps, std::size_t count, float* out) noexcept {
    double fastest = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
      fastest = std::max(fastest, steps[i]);
    }
    const auto& table = table_for(tables_, fastest);
    const auto per_output = static_cast<std::size_t>(std::ceil(fastest));
    const std::size_t per_pass = std::max<std::size_t>(1, (scratch_frames - SincTable::taps - 2) /
                                                              std::max<std::size_t>(1, per_output));
    const double loop_length = static_cast<double>(r.loop_end - r.loop_start);
    std::size_t done = 0;
    while (done < count) {
      const std::size_t n = std::min(count - done, per_pass);
      std::int64_t first = 0;
      if (out != nullptr) {
        first = static_cast<std::int64_t>(std::floor(r.position)) -
                static_cast<std::int64_t>(SincTable::left);
        const double reach = r.position + static_cast<double>(n) * fastest;
        const auto last = static_cast<std::int64_t>(std::floor(reach)) +
                          static_cast<std::int64_t>(SincTable::taps) - SincTable::left;
        gather(r, first, std::min(static_cast<std::size_t>(last - first + 1), scratch_frames),
               scratch_.data());
      }
      std::size_t i = 0;
      while (i < n) {
        if (out != nullptr) {
          const double base = std::floor(r.position);
          const auto at = static_cast<std::size_t>(static_cast<std::int64_t>(base) - first);
          out[done + i] = table.read(
              scratch_.data() + at - static_cast<std::size_t>(SincTable::left), r.position - base);
        }
        r.position += steps[done + i];
        ++i;
        if (r.looping && r.position >= static_cast<double>(r.loop_end)) {
          r.position -= loop_length;
          break; // The next pass gathers from the loop's start.
        }
      }
      done += i;
    }
  }

  // ---- Rendering ----------------------------------------------------------------------------

  void render_span(std::span<engine::StereoFrame> output) noexcept {
    if (output.empty()) {
      return;
    }
    std::array<double, max_frames> mix{};
    for (std::size_t offset = 0; offset < output.size(); offset += max_frames) {
      const std::size_t frames = std::min(max_frames, output.size() - offset);
      std::fill_n(mix.begin(), frames, 0.0);
      for (auto& v : voices_) {
        if (v.active) {
          render_voice(v, mix, frames);
        }
      }
      tone(mix, output.subspan(offset, frames));
    }
  }

  void render_voice(Voice& v, std::array<double, max_frames>& mix, std::size_t frames) noexcept {
    // Pitch: Tune and Humanize, and a slur's bend frame by frame.
    const double pitch = (values_[p::tune] + v.cents) / cents_per_semitone;
    std::array<double, max_frames> bends{};
    for (std::size_t i = 0; i < frames; ++i) {
      bends[i] = v.bend;
      if (v.bend_step != 0.0) {
        v.bend += v.bend_step;
        if ((v.bend_step > 0.0 && v.bend >= v.bend_target) ||
            (v.bend_step < 0.0 && v.bend <= v.bend_target)) {
          v.bend = v.bend_target;
          v.bend_step = 0.0;
        }
      }
    }
    const auto steps_of = [&](const Reader& r, std::array<double, max_frames>& steps) {
      const bool bending = bends[0] != bends[frames - 1] || v.bend_step != 0.0;
      const double fixed =
          base_step_ * std::exp2((r.offset + pitch + bends[0]) / semitones_per_octave);
      for (std::size_t i = 0; i < frames; ++i) {
        steps[i] =
            bending ? base_step_ * std::exp2((r.offset + pitch + bends[i]) / semitones_per_octave)
                    : fixed;
      }
    };
    std::array<double, max_frames> steps{};
    std::array<float, max_frames> plain{};
    std::array<float, max_frames> vibrato{};
    steps_of(v.plain, steps);
    read(v.plain, steps.data(), frames, plain.data());
    bool vibrato_heard = false;
    if (v.has_vibrato) {
      // Before the bloom the vibrato take only moves on with the plain one.
      vibrato_heard = v.vibrato_weight != 0.0 || v.vibrato_step != 0.0 ||
                      blend_at(v, static_cast<double>(v.age + frames)) > 0.0;
      steps_of(v.vibrato, steps);
      read(v.vibrato, steps.data(), frames, vibrato_heard ? vibrato.data() : nullptr);
    }
    const bool swells = v.swell_db > 0.0;
    for (std::size_t i = 0; i < frames; ++i) {
      if (v.age % envelope_step == 0) {
        retarget(v);
      }
      double x = v.plain_weight * plain[i];
      if (vibrato_heard) {
        x += v.vibrato_weight * vibrato[i];
      }
      if (swells) {
        // The voice's Air follows its swell: a first-order high shelf.
        v.air_low += air_coefficient_ * (x - v.air_low);
        x = v.air_low + v.air * (x - v.air_low);
        v.air += v.air_step;
      }
      mix[i] += x * v.level;
      v.level += v.level_step;
      v.plain_weight += v.plain_step;
      v.vibrato_weight += v.vibrato_step;
      ++v.age;
    }
    const bool faded = v.out != Fade::none && static_cast<double>(v.age - v.out_age) >=
                                                  v.out_frames + static_cast<double>(envelope_step);
    const double past = static_cast<double>(v.plain.stream->frames + SincTable::taps);
    if (faded || (!v.plain.looping && v.plain.position >= past)) {
      v.active = false;
    }
  }

  // The tone stage: the shelves on the mono mix, the hall, Level and the
  // safety knee, into the stereo output.
  void tone(const std::array<double, max_frames>& mix,
            std::span<engine::StereoFrame> output) noexcept {
    std::array<engine::StereoFrame, max_frames> wet{};
    for (std::size_t i = 0; i < output.size(); ++i) {
      const auto x = static_cast<float>(warmth_shelf_.process(air_shelf_.process(mix[i])));
      wet[i] = {x, x};
    }
    if (room_running_) {
      room_->process(std::span<engine::StereoFrame>(wet.data(), output.size()), {});
    }
    for (std::size_t i = 0; i < output.size(); ++i) {
      double left = safety(static_cast<double>(wet[i].left) * gain_);
      double right = safety(static_cast<double>(wet[i].right) * gain_);
      gain_ += gain_step_;
      if (declick_left_ > 0.0) {
        const double fade = declick_ * (declick_left_ / declick_frames_);
        left += fade;
        right += fade;
        declick_left_ -= 1.0;
      }
      last_output_ = 0.5 * (left + right);
      peak_ = std::max({peak_, std::fabs(left), std::fabs(right)});
      output[i].left += static_cast<float>(left);
      output[i].right += static_cast<float>(right);
    }
  }

  double rate_;
  library::State state_;
  double base_step_;
  const std::vector<SincTable>& tables_;
  std::unique_ptr<effects::EffectKernel> room_;
  std::vector<double> room_values_;
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access): Expanse always has its mix control.
  std::size_t room_mix_ = *find_parameter(*effects::expanse::engine().descriptor, "expanse.mix");
  bool room_running_ = false;
  double room_target_ = 0.0;
  const detail::Library* library_ = nullptr;
  std::span<const double> values_{};
  Transport transport_{};
  std::size_t frames_since_tick_ = 0;
  std::uint64_t clock_ = 0;
  bool poly_ = false;
  std::array<Voice, max_voices> voices_{};
  std::array<Pending, max_pending> pending_{};
  std::size_t pending_count_ = 0;
  breath::Previous previous_{};
  foundation::IdBits previous_clip_{};
  foundation::IdBits previous_note_{};
  std::optional<breath::Scale> scale_;
  std::array<float, scratch_frames> scratch_{};
  double air_coefficient_ = 0.0;
  dsp::Biquad air_shelf_;
  dsp::Biquad warmth_shelf_;
  double air_db_ = 0.0;
  double warmth_db_ = 0.0;
  double gain_ = 0.0;
  double gain_step_ = 0.0;
  bool primed_ = false;
  double declick_frames_ = 1.0;
  double declick_left_ = 0.0;
  double declick_ = 0.0;
  double last_output_ = 0.0;
  double peak_ = 0.0;
  std::uint32_t notes_ = 0;
  int last_key_ = -1;
  int last_velocity_ = 0;
  int last_tongue_ = 0;
  bool last_staccato_ = false;
  bool last_grace_ = false;
  bool last_vibrato_ = false;
};

std::unique_ptr<SynthKernel> create_kernel(double sample_rate) {
  // Control: the first Wren loads the library, and the shared tables.
  const auto status = library::status();
  (void)tables();
  return std::make_unique<WrenKernel>(sample_rate, status.state);
}

// The longest a released note can sound: its release, then the hall's tail
// when Space is up.
std::uint64_t tail_frames(std::span<const double> values, double sample_rate) {
  double seconds = release_seconds + declick_seconds + tail_margin_seconds;
  auto frames = static_cast<std::uint64_t>(std::ceil(seconds * sample_rate));
  if (values.size() == wt::parameters.size() && values[p::space] > 0.0) {
    auto room = WrenKernel::room_defaults();
    if (const auto mix = find_parameter(*effects::expanse::engine().descriptor, "expanse.mix")) {
      room[*mix] = values[p::space];
    }
    frames += effects::expanse::engine().tail_frames(room, sample_rate);
  }
  return frames;
}

const EngineDescriptor descriptor{"wren", "Wren", 1, wt::parameters, wren_presets::presets};
// A flute ignores the pedals: the kernel models them by doing nothing.
const SynthEngine wren_engine{&descriptor, &create_kernel, &tail_frames, true};

} // namespace

const SynthEngine& engine() { return wren_engine; }

breath::Settings breath_settings(std::span<const double> values) noexcept {
  breath::Settings settings;
  if (values.size() != wt::parameters.size()) {
    return settings;
  }
  settings.style = static_cast<breath::Style>(choice(values[p::style], wt::styles.size()));
  settings.amount = values[p::amount];
  settings.dynamics = values[p::dynamics];
  settings.swell_db = values[p::swell];
  settings.bloom_ms = values[p::bloom];
  settings.ornaments =
      static_cast<breath::Ornaments>(choice(values[p::ornaments], wt::ornaments.size()));
  settings.humanize = values[p::humanize];
  settings.articulation =
      static_cast<breath::Articulation>(choice(values[p::articulation], wt::articulations.size()));
  return settings;
}

} // namespace oscillate::native_dsp::synth::wren
