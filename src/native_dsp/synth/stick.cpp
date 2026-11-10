#include "oscillate/native_dsp/synth/stick.hpp"

#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/parameter_control.hpp"
#include "oscillate/native_dsp/synth/sample_stream.hpp"
#include "oscillate/native_dsp/transport.hpp"
#include "stick_library.hpp"
#include "stick_presets.hpp"
#include "stick_table.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <numbers>
#include <span>
#include <vector>

namespace oscillate::native_dsp::synth::stick {
namespace {

namespace st = stick_table;
using detail::Side;
using native_dsp::dsp::db_to_gain;
using sample_stream::SincTable;

namespace p {
constexpr std::size_t kit = st::index_of("stick.kit");
constexpr std::size_t mix = st::index_of("stick.mix");
constexpr std::size_t bleed = st::index_of("stick.bleed");
constexpr std::size_t pedal = st::index_of("stick.perf.pedal");
constexpr std::size_t humanise = st::index_of("stick.humanise");
constexpr std::size_t round_robin = st::index_of("stick.round_robin");
constexpr std::size_t striker = st::index_of("stick.striker");
constexpr std::size_t level = st::index_of("stick.level");
} // namespace p

constexpr std::size_t max_frames = control_interval_frames;
// Every hit sounding: each piece's own limit below sums to this.
constexpr std::size_t max_voices = 32;
// Source frames gathered per resampling pass.
constexpr std::size_t scratch_frames = 512;
constexpr double semitones_per_octave = 12.0;
constexpr double max_velocity = 127.0;
constexpr double quarter_turn = std::numbers::pi / 2.0;

// ---- Picking a hit --------------------------------------------------------------------------
// A curve of c plays velocity v as v^(2^(curve_octaves * c)): soft
// curves (c < 0) lift light touches, hard ones (c > 0) hold them down.
constexpr double curve_octaves = 1.5;
// Velocity spans the stroke's recorded powers, or at least this many dB
// below its loudest, so a kit with few soft hits still plays quietly.
constexpr double min_span_db = 30.0;
// A hit is brought to the power its velocity asks for, by at most this
// much louder (quieter is unbounded: below the softest hit it turns down).
constexpr double max_boost_db = 3.0;
// Round robins: the hits within this many dB of the target, at most this
// many, never the hit the stroke played last.
constexpr double round_robin_window_db = 2.0;
constexpr std::size_t max_round_robins = 6;

// ---- Humanise
// ------------------------------------------------------------------------------------ At full:
// each hit up to this late and this much louder or quieter.
constexpr double humanise_delay_seconds = 0.008;
constexpr double humanise_level_db = 1.5;
// Seeded from the song position at this resolution, in a cycle of this
// many beats, so loops repeat and export equals playback.
constexpr double beat_resolution = 65'536.0;
constexpr double seed_cycle_beats = 8.0;

// ---- The hi-hat
// ----------------------------------------------------------------------------------- A stroke
// chokes ringing hat hits recorded this much more open than it.
constexpr double choke_margin = 0.2;
// The pedal closing below this chokes the open hat.
constexpr double pedal_closed = 0.15;
// A choke closes over this long.
constexpr double choke_seconds = 0.03;
// A hat played less open than its recording rings shorter: at openness o
// of a recording made at r, it reaches -60 dB at (o / r) of its length,
// and no sooner than this.
constexpr double shortest_hat_seconds = 0.1;

// ---- Each piece's hits
// ------------------------------------------------------------------------------ At most this many
// hits of a piece ring at once; a new one fades the oldest over limit_fade_seconds. Drums are
// re-struck heads; cymbals ring on together. In stick::Piece order.
constexpr std::array<std::size_t, piece_count> piece_voices{2, 3, 4, 2, 2, 2, 2, 2, 3, 3, 3, 2, 2};
constexpr double limit_fade_seconds = 0.06;
static_assert(
    [] {
      std::size_t sum = 0;
      for (const auto v : piece_voices) {
        sum += v;
      }
      return sum;
    }() == max_voices,
    "the pieces' limits fill the voices");

// ---- Mics
// -------------------------------------------------------------------------------------------- The
// mixes on the kit selector, in dB over each group's fader (in MicGroup order); the bottom of the
// range is off.
constexpr double off_db = st::off_db;
constexpr std::array<std::array<double, mic_group_count>, mix_count> mix_offsets_db{{
    // Dry: close mics, the overheads down, no rooms.
    {0, 0, 0, 0, 0, 0, 0, 0, -6, off_db, off_db, off_db, -6},
    // Studio: the faders as set.
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    // Room: close mics down, the rooms up.
    {-6, -6, -6, -6, -6, -6, -6, -6, 0, 6, 6, 6, 0},
}};
// The rooms (fed by a piece's Room send); the overheads and the china
// overhead pair take its Overheads send.
[[nodiscard]] constexpr bool ambient(MicGroup group) noexcept {
  return group == MicGroup::room || group == MicGroup::far_room || group == MicGroup::mid_side;
}
// Mutes, solos, mixes and Bleed move over about this long.
constexpr double switch_seconds = 0.01;
// Below this a gain is silence.
constexpr double silent_gain = 1e-5;

// ---- Output
// -----------------------------------------------------------------------------------------
constexpr double mute_db = st::min_level_db + 0.1;
constexpr double meter_floor_db = -120.0;
// Meters fall this fast after a peak.
constexpr double meter_release_db_per_second = 24.0;
// Piece levels read 0 at this peak, 1 at full scale.
constexpr double piece_floor_db = -48.0;
constexpr double safety_knee = 0.9;
constexpr double declick_seconds = 0.0015;
constexpr double tail_margin_seconds = 0.25;
constexpr double decibels = 20.0;
constexpr double silence_db = -60.0;

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

[[nodiscard]] double to_db(double gain) noexcept {
  return gain > 0.0 ? decibels * std::log10(gain) : meter_floor_db;
}

[[nodiscard]] double gain_of(double db) noexcept { return db <= mute_db ? 0.0 : db_to_gain(db); }

// A seeded value in [0, 1).
[[nodiscard]] double unit(std::uint64_t seed) noexcept {
  return native_dsp::dsp::unit_from_bits(native_dsp::dsp::splitmix64(seed));
}

[[nodiscard]] double signed_unit(std::uint64_t seed) noexcept { return 2.0 * unit(seed) - 1.0; }

// A frame multiplier reaching -60 dB over `frames`.
[[nodiscard]] double decay_step(double frames) noexcept {
  return std::exp(std::log(db_to_gain(silence_db)) / std::max(1.0, frames));
}

// Which channels mix a source frame, from which frame on: a voice keeps
// its last few, so frames gathered twice always mix alike whatever the
// block split.
struct Weights {
  std::int64_t from = 0;
  std::array<float, max_mics> left{};
  std::array<float, max_mics> right{};
};
constexpr std::size_t weight_history = 4;

// One hit sounding.
struct Voice {
  bool active = false;
  const detail::Library* library = nullptr;
  std::uint32_t hit = 0;
  const sample_stream::Sample* sample = nullptr;
  const std::int16_t* resident = nullptr;
  bool streaming = false;
  std::uint32_t streamed = 0; // Stream frames on hand.
  std::uint32_t released = 0; // Stream frames handed back.
  Piece piece = Piece::kick;
  double openness = 0.0;
  double position = 0.0;
  double step = 1.0;
  const SincTable* table = nullptr;
  bool exact = true;
  std::uint64_t delay = 0;
  std::uint64_t started = 0;
  double gain = 1.0;
  double length = 1.0; // Output frames the recording lasts at its speed.
  double env = 1.0;    // Decay's added envelope.
  double env_step = 1.0;
  double hat = 1.0; // A part-open hat's shortening.
  double hat_step = 1.0;
  double fade = 1.0; // A choke or the piece's limit.
  double fade_step = 0.0;
  std::array<Weights, weight_history> weights{};
  std::size_t weight_count = 0;
};

class StickKernel final : public SynthKernel {
public:
  explicit StickKernel(double sample_rate)
      : rate_(sample_rate), base_step_(static_cast<double>(library::pack_rate) / sample_rate),
        tables_(sample_stream::speed_tables()), streams_(std::make_unique<sample_stream::Streams>(
                                                    detail::registry(), max_mics, max_voices)) {
    declick_frames_ = std::max(1.0, declick_seconds * sample_rate);
    const double tick_seconds = static_cast<double>(control_interval_frames) / sample_rate;
    switch_glide_ = 1.0 - std::exp(-tick_seconds / switch_seconds);
    meter_fall_ = db_to_gain(-meter_release_db_per_second * tick_seconds);
  }

  void control(std::span<const double> values, std::uint32_t frames_to_next_tick,
               const Transport& transport) noexcept override {
    values_ = values;
    transport_ = transport;
    frames_since_tick_ = 0;
    tick_span_ = std::max<double>(1.0, frames_to_next_tick);
    choose_kit(static_cast<Kit>(choice(values[p::kit], kit_count)));
    // The pedal closing chokes the open hat.
    const double pedal = values[p::pedal];
    if (primed_ && pedal <= pedal_closed && pedal_ > pedal_closed) {
      choke_hats(pedal_closed + choke_margin);
    }
    pedal_ = pedal;
    mix_groups();
    if (library_ != nullptr) {
      weigh_pieces();
    }
    for (std::size_t piece = 0; piece < piece_count; ++piece) {
      const double level =
          gain_of(values[st::piece_index(static_cast<Piece>(piece), st::piece::level)]);
      level_from_[piece] = primed_ ? level_to_[piece] : level;
      level_to_[piece] = level;
    }
    const double master = gain_of(values[p::level]);
    master_from_ = primed_ ? master_to_ : master;
    master_to_ = master;
    for (auto& v : voices_) {
      if (v.active) {
        set_decay(v);
      }
    }
    for (auto& peak : mic_peak_) {
      peak *= meter_fall_;
    }
    for (auto& peak : piece_peak_) {
      peak *= meter_fall_;
    }
    output_peak_ *= meter_fall_;
    primed_ = true;
  }

  void note_on(const NoteEvent& event) noexcept override {
    if (values_.empty() || library_ == nullptr) {
      return;
    }
    const auto mapped = map_key(event.key, library_->contents().toms);
    if (!mapped) {
      return;
    }
    const Piece piece = mapped->piece;
    const auto seed = seed_of(event.key);
    const double curve = values_[st::piece_index(piece, st::piece::curve)];
    const double velocity = std::pow(static_cast<double>(event.velocity) / max_velocity,
                                     std::exp2(curve_octaves * curve));
    const double humanise = values_[p::humanise];
    Strike strike;
    strike.velocity = velocity;
    strike.seed = seed;
    strike.delay = static_cast<std::uint64_t>(
        std::llround(rate_ * humanise * humanise_delay_seconds * unit(seed + 1)));
    strike.level_db = humanise * humanise_level_db * signed_unit(seed + 2);
    strike.striker = static_cast<Striker>(choice(values_[p::striker], striker_count));
    bool started = false;
    if (piece == Piece::hihat && mapped->articulation != Articulation::pedal) {
      const double openness =
          mapped->at_pedal ? pedal_ : openness_of(mapped->articulation).value_or(0.0);
      started = strike_hat(openness, strike);
    } else {
      if (mapped->articulation == Articulation::pedal) {
        choke_hats(choke_margin);
      }
      started = start(piece, mapped->articulation, strike, 1.0);
    }
    if (started) {
      ++hits_;
      last_piece_ = static_cast<int>(piece);
    }
  }

  // Drums play out: a note-off does nothing.
  void note_off(const NoteEvent& /*event*/) noexcept override {}

  void render(std::span<engine::StereoFrame> output) noexcept override {
    for (std::size_t offset = 0; offset < output.size(); offset += max_frames) {
      const std::size_t frames = std::min(max_frames, output.size() - offset);
      std::fill_n(mix_left_.begin(), frames, 0.0);
      std::fill_n(mix_right_.begin(), frames, 0.0);
      const double since = static_cast<double>(frames_since_tick_ + offset);
      for (std::size_t slot = 0; slot < voices_.size(); ++slot) {
        if (voices_[slot].active) {
          render_voice(slot, frames, since);
        }
      }
      finish(output.subspan(offset, frames), since);
    }
    clock_ += output.size();
    frames_since_tick_ += output.size();
  }

  void reset() noexcept override {
    declick_ = last_output_;
    declick_left_ = declick_frames_;
    for (std::size_t slot = 0; slot < voices_.size(); ++slot) {
      if (voices_[slot].active) {
        free_voice(slot);
      }
    }
    last_hit_.fill(no_hit);
  }

  // Offline renders: the kit's read finishes before any block, and its
  // streams fill before each.
  void await_content() noexcept override {
    if (library_ == nullptr) {
      library::wait();
    }
    streams_->service();
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
    out[telemetry::kit] = static_cast<float>(kit_);
    out[telemetry::hits] = static_cast<float>(hits_ & telemetry_wrap);
    out[telemetry::voices] = static_cast<float>(status().active_voices);
    out[telemetry::output_peak_db] =
        static_cast<float>(std::max(meter_floor_db, to_db(output_peak_)));
    out[telemetry::pedal] = static_cast<float>(pedal_);
    out[telemetry::last_piece] = static_cast<float>(last_piece_);
    out[telemetry::underruns] = static_cast<float>(underruns_ & telemetry_wrap);
    for (std::size_t piece = 0; piece < piece_count; ++piece) {
      const double level = (to_db(piece_peak_[piece]) - piece_floor_db) / -piece_floor_db;
      out[telemetry::piece_level + piece] = static_cast<float>(std::clamp(level, 0.0, 1.0));
    }
    std::array<double, mic_group_count> groups{};
    if (library_ != nullptr) {
      const auto mics = library_->mics();
      for (std::size_t c = 0; c < mics.size(); ++c) {
        auto& group = groups[static_cast<std::size_t>(mics[c].group)];
        group = std::max(group, mic_peak_[c]);
      }
    }
    for (std::size_t g = 0; g < mic_group_count; ++g) {
      out[telemetry::mic_db + g] = static_cast<float>(std::max(meter_floor_db, to_db(groups[g])));
    }
    return telemetry::size;
  }

private:
  static constexpr std::uint32_t telemetry_wrap = (1U << 24U) - 1U;
  static constexpr std::uint32_t no_hit = sample_stream::no_sample;

  // What a note asks of a hit.
  struct Strike {
    double velocity = 1.0;
    std::uint64_t seed = 0;
    std::uint64_t delay = 0;
    double level_db = 0.0;
    Striker striker = Striker::stick;
    double openness = 0.0; // A hat's, as played.
  };

  // ---- The kit and its mics ------------------------------------------------------------------

  // A new kit fades what the last one plays; the same kit's library is
  // taken up once loaded (a reloaded one once idle).
  void choose_kit(Kit kit) noexcept {
    const auto* latest = detail::current(kit);
    if (kit != kit_) {
      kit_ = kit;
      for (auto& v : voices_) {
        if (v.active) {
          fade(v, choke_seconds);
        }
      }
      adopt(latest);
    } else if (latest != library_ &&
               (library_ == nullptr || std::ranges::none_of(voices_, &Voice::active))) {
      adopt(latest);
    }
    state_ = library_ != nullptr ? library::State::ready : detail::published_state(kit_);
  }

  void adopt(const detail::Library* library) noexcept {
    library_ = library;
    owned_.fill(false);
    if (library_ != nullptr) {
      for (const auto& mic : library_->mics()) {
        for (std::size_t piece = 0; piece < piece_count; ++piece) {
          owned_[piece] = owned_[piece] || mic.hears(static_cast<Piece>(piece));
        }
      }
    }
    last_hit_.fill(no_hit);
    weighed_ = false;
  }

  // Each group's gain: its fader, mute, solo and the mix, gliding.
  void mix_groups() noexcept {
    const auto mix = choice(values_[p::mix], mix_count);
    bool any_solo = false;
    for (std::size_t g = 0; g < mic_group_count; ++g) {
      any_solo = any_solo || values_[st::mic_index(static_cast<MicGroup>(g), st::mic::solo)] >= 0.5;
    }
    for (std::size_t g = 0; g < mic_group_count; ++g) {
      const auto group = static_cast<MicGroup>(g);
      const bool muted = values_[st::mic_index(group, st::mic::mute)] >= 0.5;
      const bool soloed = values_[st::mic_index(group, st::mic::solo)] >= 0.5;
      const double offset = mix_offsets_db[mix][g];
      double target = 0.0;
      if (!muted && (!any_solo || soloed) && offset > off_db) {
        target = gain_of(values_[st::mic_index(group, st::mic::level)]) * db_to_gain(offset);
      }
      glide(group_gain_[g], target);
    }
    glide(bleed_, values_[p::bleed] >= 0.5 ? 1.0 : 0.0);
  }

  void glide(double& value, double target) const noexcept {
    if (!primed_) {
      value = target;
      return;
    }
    value += (target - value) * switch_glide_;
    if (std::fabs(target - value) < silent_gain) {
      value = target;
    }
  }

  // Each piece's mix of every channel, and the voices told of changes.
  void weigh_pieces() noexcept {
    const auto mics = library_->mics();
    for (std::size_t piece = 0; piece < piece_count; ++piece) {
      const auto as = static_cast<Piece>(piece);
      const double overheads = gain_of(values_[st::piece_index(as, st::piece::overheads)]);
      const double room = gain_of(values_[st::piece_index(as, st::piece::room)]);
      // A piece without its own mic (a cymbal) pans by balancing the pair.
      const double balance = owned_[piece] ? 0.0 : values_[st::piece_index(as, st::piece::pan)];
      Weights w;
      for (std::size_t c = 0; c < mics.size(); ++c) {
        const auto& mic = mics[c];
        double gain = group_gain_[static_cast<std::size_t>(mic.group)];
        double left = 0.0;
        double right = 0.0;
        if (mic.side == Side::mono) {
          // Its own piece placed by that piece's pan; bleed by its first piece's.
          const bool own = mic.hears(as);
          if (!own) {
            gain *= bleed_;
          }
          const double pan = values_[st::piece_index(own ? as : mic.piece, st::piece::pan)];
          const double theta = (pan + 1.0) * std::numbers::pi / 4.0;
          left = std::cos(theta) * std::numbers::sqrt2;
          right = std::sin(theta) * std::numbers::sqrt2;
        } else {
          gain *= ambient(mic.group) ? room : overheads;
          if (!ambient(mic.group) && mic.side != Side::centre) {
            gain *= mic.side == Side::left ? std::min(1.0, 1.0 - balance)
                                           : std::min(1.0, 1.0 + balance);
          }
          left = mic.side == Side::right ? 0.0 : 1.0;
          right = mic.side == Side::left ? 0.0 : 1.0;
        }
        w.left[c] = static_cast<float>(gain * left);
        w.right[c] = static_cast<float>(gain * right);
      }
      const bool changed = !weighed_ || w.left != piece_weights_[piece].left ||
                           w.right != piece_weights_[piece].right;
      piece_weights_[piece] = w;
      if (changed) {
        for (auto& v : voices_) {
          if (v.active && v.piece == as && v.library == library_) {
            remember(v, w);
          }
        }
      }
    }
    weighed_ = true;
  }

  // New weights for a voice, from the first frame it has not gathered.
  static void remember(Voice& v, const Weights& w) noexcept {
    Weights entry = w;
    entry.from = static_cast<std::int64_t>(std::floor(v.position)) +
                 static_cast<std::int64_t>(SincTable::taps) + 1;
    if (v.weight_count == weight_history) {
      std::move(v.weights.begin() + 1, v.weights.end(), v.weights.begin());
      --v.weight_count;
    }
    v.weights[v.weight_count++] = entry;
  }

  // ---- Striking
  // ----------------------------------------------------------------------------------

  // The hi-hat at an openness: the two recorded openings around it,
  // crossfaded equal-power. Less open hats choke more open ones.
  bool strike_hat(double openness, const Strike& strike) noexcept {
    constexpr std::array strokes{Articulation::closed, Articulation::half, Articulation::open};
    Articulation below = Articulation::closed;
    Articulation above = Articulation::closed;
    bool have_below = false;
    bool have_above = false;
    for (const auto a : strokes) {
      if (library_->stroke(Piece::hihat, a, strike.striker).count == 0) {
        continue;
      }
      const double at = openness_of(a).value_or(0.0);
      if (at <= openness && (!have_below || at > openness_of(below).value_or(0.0))) {
        below = a;
        have_below = true;
      }
      if (at >= openness && (!have_above || at < openness_of(above).value_or(0.0))) {
        above = a;
        have_above = true;
      }
    }
    if (!have_below && !have_above) {
      return false;
    }
    if (!have_below) {
      below = above;
    } else if (!have_above) {
      above = below;
    }
    choke_hats(openness + choke_margin);
    Strike hat = strike;
    hat.openness = openness;
    const double low = openness_of(below).value_or(0.0);
    const double high = openness_of(above).value_or(0.0);
    const double f = high > low ? (openness - low) / (high - low) : 0.0;
    bool started = false;
    if (f < 1.0) {
      started = start(Piece::hihat, below, hat, std::cos(quarter_turn * f));
    }
    if (f > 0.0 && above != below) {
      started = start(Piece::hihat, above, hat, std::sin(quarter_turn * f)) || started;
    }
    return started;
  }

  // Starts a hit of a stroke: the one recorded nearest the power its
  // velocity asks for, or a seeded round robin among its neighbours.
  // `weight` scales it (a hat's share of a crossfade).
  bool start(Piece piece, Articulation articulation, const Strike& strike, double weight) noexcept {
    const auto& stroke = library_->stroke(piece, articulation, strike.striker);
    if (stroke.count == 0 || weight <= silent_gain) {
      return false;
    }
    const double span = std::max(stroke.loudest_db - stroke.softest_db, min_span_db);
    const double target = stroke.loudest_db - (1.0 - strike.velocity) * span;
    // The nearest by power, then its neighbours within the window.
    std::uint32_t nearest = 0;
    double best = 0.0;
    for (std::uint32_t i = 0; i < stroke.count; ++i) {
      const double distance =
          std::fabs(library_->hit(library_->order(stroke.first + i)).power_db - target);
      if (i == 0 || distance < best) {
        nearest = i;
        best = distance;
      }
    }
    std::array<std::uint32_t, max_round_robins> candidates{};
    std::size_t count = 0;
    candidates[count++] = library_->order(stroke.first + nearest);
    const bool robins = values_[p::round_robin] >= 0.5;
    for (std::uint32_t distance = 1; robins && count < max_round_robins && distance < stroke.count;
         ++distance) {
      for (const std::int64_t at : {static_cast<std::int64_t>(nearest) - distance,
                                    static_cast<std::int64_t>(nearest) + distance}) {
        if (at < 0 || at >= stroke.count || count == max_round_robins) {
          continue;
        }
        const auto index = library_->order(stroke.first + static_cast<std::uint32_t>(at));
        if (std::fabs(library_->hit(index).power_db - target) <= round_robin_window_db) {
          candidates[count++] = index;
        }
      }
    }
    const auto stroke_slot = detail::stroke_index(piece, articulation, strike.striker);
    std::uint32_t chosen = candidates[0];
    if (count > 1) {
      // Never the stroke's last hit.
      const auto last = last_hit_[stroke_slot];
      const auto end = std::remove(candidates.begin(), candidates.begin() + count, last);
      const auto left = static_cast<std::size_t>(end - candidates.begin());
      chosen =
          candidates[static_cast<std::size_t>(unit(strike.seed + 3) * static_cast<double>(left))];
    }
    last_hit_[stroke_slot] = chosen;
    const double correction =
        std::min(target - library_->hit(chosen).power_db, max_boost_db) + strike.level_db;
    limit(piece);
    const std::size_t slot = free_slot();
    auto& v = voices_[slot];
    v = Voice{};
    v.active = true;
    v.library = library_;
    v.hit = chosen;
    v.sample = &library_->stream(chosen);
    v.resident = library_->resident(chosen);
    v.piece = piece;
    v.openness = strike.openness;
    const double tune = values_[st::piece_index(piece, st::piece::tune)];
    v.step = base_step_ * std::exp2(tune / semitones_per_octave);
    v.exact = v.step == 1.0;
    v.table = &sample_stream::speed_table(tables_, v.step);
    v.delay = strike.delay;
    v.started = clock_;
    v.gain = weight * db_to_gain(correction);
    v.length = static_cast<double>(v.sample->frames) / v.step;
    set_decay(v);
    // A hat played less open than its recording rings shorter.
    const double recorded = openness_of(articulation).value_or(0.0);
    if (piece == Piece::hihat && recorded > strike.openness) {
      const double share = strike.openness / recorded;
      v.hat_step = decay_step(std::max(shortest_hat_seconds * rate_, share * v.length));
    }
    v.weights[0] = piece_weights_[static_cast<std::size_t>(piece)];
    v.weights[0].from = std::numeric_limits<std::int64_t>::min();
    v.weight_count = 1;
    v.streaming = v.sample->frames > v.sample->resident;
    if (v.streaming) {
      streams_->slot(slot).request.store(
          sample_stream::request_word(++generations_[slot], library_->serial(), chosen),
          std::memory_order_release);
    }
    return true;
  }

  void set_decay(Voice& v) const noexcept {
    const double decay = values_[st::piece_index(v.piece, st::piece::decay)];
    v.env_step = decay >= st::natural_decay ? 1.0 : decay_step(decay * v.length);
  }

  // Chokes ringing hats recorded or played more open than `above`.
  void choke_hats(double above) noexcept {
    for (auto& v : voices_) {
      if (v.active && v.piece == Piece::hihat && v.openness > above) {
        fade(v, choke_seconds);
      }
    }
  }

  // Fades the piece's oldest hit when it has as many as it may.
  void limit(Piece piece) noexcept {
    std::size_t sounding = 0;
    Voice* oldest = nullptr;
    for (auto& v : voices_) {
      if (v.active && v.piece == piece && v.fade_step == 0.0) {
        ++sounding;
        if (oldest == nullptr || v.started < oldest->started) {
          oldest = &v;
        }
      }
    }
    if (oldest != nullptr && sounding >= piece_voices[static_cast<std::size_t>(piece)]) {
      fade(*oldest, limit_fade_seconds);
    }
  }

  void fade(Voice& v, double seconds) const noexcept {
    const double step = 1.0 / std::max(1.0, seconds * rate_);
    if (v.fade_step == 0.0 || step > v.fade_step) {
      v.fade_step = step;
    }
  }

  // A free voice, else the one fading out longest, else the oldest.
  [[nodiscard]] std::size_t free_slot() noexcept {
    std::size_t faded = voices_.size();
    std::size_t oldest = 0;
    for (std::size_t slot = 0; slot < voices_.size(); ++slot) {
      const auto& v = voices_[slot];
      if (!v.active) {
        return slot;
      }
      if (v.fade_step > 0.0 && (faded == voices_.size() || v.fade < voices_[faded].fade)) {
        faded = slot;
      }
      if (v.started < voices_[oldest].started) {
        oldest = slot;
      }
    }
    const std::size_t slot = faded != voices_.size() ? faded : oldest;
    free_voice(slot);
    return slot;
  }

  void free_voice(std::size_t slot) noexcept {
    auto& v = voices_[slot];
    if (v.streaming) {
      streams_->slot(slot).request.store(
          sample_stream::request_word(++generations_[slot], 0, sample_stream::no_sample),
          std::memory_order_release);
    }
    v.active = false;
  }

  // A seed from the note's place in an eight-beat cycle and its key.
  [[nodiscard]] std::uint64_t seed_of(int key) const noexcept {
    const double per_frame = sync_beats_per_second(transport_, rate_) / rate_;
    const double beat =
        transport_.beat_at_block_start + static_cast<double>(frames_since_tick_) * per_frame;
    const double place = std::fmod(std::max(0.0, beat), seed_cycle_beats);
    const auto step = static_cast<std::uint64_t>(std::llround(place * beat_resolution));
    constexpr auto keys = static_cast<std::uint64_t>(last_key) + 1;
    return native_dsp::dsp::splitmix64(step * keys + static_cast<std::uint64_t>(key));
  }

  // ---- Rendering -------------------------------------------------------------------------------

  // Source frames [first, first + count) of a voice, every channel mixed by
  // its weights into the scratch pair. Returns how many streamed frames had
  // not arrived (heard as silence).
  std::uint32_t gather(Voice& v, std::size_t slot, std::int64_t first, std::size_t count) noexcept {
    const auto channels = v.library->channels();
    const auto frames = static_cast<std::int64_t>(v.sample->frames);
    const auto resident = static_cast<std::int64_t>(v.sample->resident);
    const std::int16_t* ring = v.streaming ? streams_->ring(slot) : nullptr;
    const float scale = v.sample->scale;
    std::size_t entry = 0;
    while (entry + 1 < v.weight_count && v.weights[entry + 1].from <= first) {
      ++entry;
    }
    std::uint32_t starved = 0;
    for (std::size_t j = 0; j < count; ++j) {
      const std::int64_t i = first + static_cast<std::int64_t>(j);
      while (entry + 1 < v.weight_count && v.weights[entry + 1].from <= i) {
        ++entry;
      }
      const std::int16_t* frame = nullptr;
      if (i >= 0 && i < frames) {
        if (i < resident) {
          frame = v.resident + static_cast<std::size_t>(i) * channels;
        } else if (const auto f = static_cast<std::uint32_t>(i - resident); f < v.streamed) {
          frame = ring + static_cast<std::size_t>(f & sample_stream::ring_mask) * channels;
        } else {
          ++starved;
        }
      }
      float left = 0.0F;
      float right = 0.0F;
      if (frame != nullptr) {
        const auto& w = v.weights[entry];
        for (std::uint32_t c = 0; c < channels; ++c) {
          const float x = static_cast<float>(frame[c]) * scale;
          const float l = x * w.left[c];
          const float r = x * w.right[c];
          left += l;
          right += r;
          mic_peak_[c] = std::max<double>(mic_peak_[c], std::max(std::fabs(l), std::fabs(r)));
        }
      }
      scratch_left_[j] = left;
      scratch_right_[j] = right;
    }
    return starved;
  }

  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a voice's slot, then a frame count.
  void render_voice(std::size_t slot, std::size_t frames, double since) noexcept {
    auto& v = voices_[slot];
    std::size_t first = 0;
    if (v.delay > 0) {
      const auto waited = std::min<std::uint64_t>(v.delay, frames);
      v.delay -= waited;
      first = static_cast<std::size_t>(waited);
      if (first == frames) {
        return;
      }
    }
    if (v.streaming) {
      const auto word = streams_->slot(slot).progress.load(std::memory_order_acquire);
      if (sample_stream::generation_of(word) == generations_[slot]) {
        v.streamed = sample_stream::frames_of(word);
      }
    }
    const auto piece = static_cast<std::size_t>(v.piece);
    const double level_from = level_from_[piece];
    const double level_change = level_to_[piece] - level_from;
    const auto per_output = static_cast<std::size_t>(std::ceil(v.step));
    const std::size_t per_pass = std::max<std::size_t>(1, (scratch_frames - SincTable::taps - 2) /
                                                              std::max<std::size_t>(1, per_output));
    double peak = 0.0;
    // It ends on the frame it falls silent or runs out, whatever the block
    // split, so a piece's limit always counts the same hits.
    const auto past = static_cast<double>(v.sample->frames) +
                      (v.exact ? 0.0 : static_cast<double>(SincTable::left));
    bool ended = false;
    std::size_t done = first;
    while (done < frames && !ended) {
      const std::size_t count = std::min(frames - done, per_pass);
      std::int64_t from = static_cast<std::int64_t>(std::floor(v.position));
      std::int64_t to = static_cast<std::int64_t>(
          std::floor(v.position + static_cast<double>(count - 1) * v.step));
      if (!v.exact) {
        from -= SincTable::left;
        to += static_cast<std::int64_t>(SincTable::taps) - SincTable::left - 1;
      }
      const auto span = static_cast<std::size_t>(to - from + 1);
      underruns_ += gather(v, slot, from, std::min(span, scratch_frames));
      for (std::size_t i = 0; i < count; ++i) {
        const double base = std::floor(v.position);
        const auto at = static_cast<std::size_t>(static_cast<std::int64_t>(base) - from);
        float left = 0.0F;
        float right = 0.0F;
        if (v.exact) {
          left = scratch_left_[at];
          right = scratch_right_[at];
        } else {
          const std::size_t tap = at - static_cast<std::size_t>(SincTable::left);
          v.table->read(scratch_left_.data() + tap, scratch_right_.data() + tap, v.position - base,
                        left, right);
        }
        const double u = std::clamp((since + static_cast<double>(done + i)) / tick_span_, 0.0, 1.0);
        const double amplitude = v.gain * v.env * v.hat * v.fade * (level_from + u * level_change);
        const double l = amplitude * left;
        const double r = amplitude * right;
        mix_left_[done + i] += l;
        mix_right_[done + i] += r;
        peak = std::max({peak, std::fabs(l), std::fabs(r)});
        v.env *= v.env_step;
        v.hat *= v.hat_step;
        if (v.fade_step > 0.0) {
          v.fade = std::max(0.0, v.fade - v.fade_step);
        }
        v.position += v.step;
        if (v.gain * v.env * v.hat * v.fade < silent_gain || std::floor(v.position) >= past) {
          ended = true;
          break;
        }
      }
      done += count;
    }
    piece_peak_[piece] = std::max(piece_peak_[piece], peak);
    const auto next = static_cast<std::int64_t>(std::floor(v.position)) -
                      (v.exact ? 0 : static_cast<std::int64_t>(SincTable::left));
    if (v.streaming) {
      // Hand back what no later read needs.
      const std::int64_t needed = next - static_cast<std::int64_t>(v.sample->resident);
      if (needed > static_cast<std::int64_t>(v.released)) {
        v.released = static_cast<std::uint32_t>(
            std::min<std::int64_t>(needed, static_cast<std::int64_t>(v.streamed)));
        streams_->slot(slot).consumed.store(sample_stream::tagged(generations_[slot], v.released),
                                            std::memory_order_release);
      }
    }
    if (ended) {
      free_voice(slot);
    }
  }

  // Level, the safety knee and the reset's declick, into the output.
  void finish(std::span<engine::StereoFrame> output, double since) noexcept {
    for (std::size_t i = 0; i < output.size(); ++i) {
      const double u = std::clamp((since + static_cast<double>(i)) / tick_span_, 0.0, 1.0);
      const double master = master_from_ + u * (master_to_ - master_from_);
      double left = safety(mix_left_[i] * master);
      double right = safety(mix_right_[i] * master);
      if (declick_left_ > 0.0) {
        const double fade = declick_ * (declick_left_ / declick_frames_);
        left += fade;
        right += fade;
        declick_left_ -= 1.0;
      }
      last_output_ = 0.5 * (left + right);
      output_peak_ = std::max({output_peak_, std::fabs(left), std::fabs(right)});
      output[i].left += static_cast<float>(left);
      output[i].right += static_cast<float>(right);
    }
  }

  double rate_;
  double base_step_;
  const std::vector<SincTable>& tables_;
  std::unique_ptr<sample_stream::Streams> streams_;
  std::array<std::uint32_t, max_voices> generations_{};
  const detail::Library* library_ = nullptr;
  Kit kit_ = Kit::naked;
  library::State state_ = library::State::missing;
  std::array<bool, piece_count> owned_{};
  std::span<const double> values_{};
  Transport transport_{};
  std::size_t frames_since_tick_ = 0;
  double tick_span_ = 1.0;
  std::uint64_t clock_ = 0;
  std::array<Voice, max_voices> voices_{};
  std::array<std::uint32_t, detail::stroke_count> last_hit_ = [] {
    std::array<std::uint32_t, detail::stroke_count> none{};
    none.fill(no_hit);
    return none;
  }();
  std::array<Weights, piece_count> piece_weights_{};
  bool weighed_ = false;
  std::array<double, mic_group_count> group_gain_{};
  double bleed_ = 1.0;
  double switch_glide_ = 1.0;
  std::array<double, piece_count> level_from_{};
  std::array<double, piece_count> level_to_{};
  double master_from_ = 0.0;
  double master_to_ = 0.0;
  double pedal_ = 0.0;
  bool primed_ = false;
  std::array<float, scratch_frames> scratch_left_{};
  std::array<float, scratch_frames> scratch_right_{};
  std::array<double, max_frames> mix_left_{};
  std::array<double, max_frames> mix_right_{};
  double declick_frames_ = 1.0;
  double declick_left_ = 0.0;
  double declick_ = 0.0;
  double last_output_ = 0.0;
  double meter_fall_ = 1.0;
  std::array<double, max_mics> mic_peak_{};
  std::array<double, piece_count> piece_peak_{};
  double output_peak_ = 0.0;
  std::uint32_t hits_ = 0;
  std::uint32_t underruns_ = 0;
  int last_piece_ = -1;
};

std::unique_ptr<SynthKernel> create_kernel(double sample_rate) {
  // Control: the shared tables before any kernel reads them.
  (void)sample_stream::speed_tables();
  return std::make_unique<StickKernel>(sample_rate);
}

// Control: the kit these values play starts loading. A failure to start
// leaves the kit as it was, to be fetched again at the next offer.
void fetch_content(std::span<const double> values) noexcept {
  if (values.size() != st::parameters.size()) {
    return;
  }
  try {
    detail::fetch(static_cast<Kit>(choice(values[p::kit], kit_count)));
  } catch (const std::exception&) {
    return;
  }
}

// The longest a hit can sound: the kit's longest recording, slowed by the
// lowest Tune, after the latest Humanise.
std::uint64_t tail_frames(std::span<const double> values, double sample_rate) {
  double frames = static_cast<double>(detail::max_hit_frames);
  double slowest = std::exp2(max_tune_semitones / semitones_per_octave);
  if (values.size() == st::parameters.size()) {
    const auto kit = static_cast<Kit>(choice(values[p::kit], kit_count));
    if (const auto* library = detail::current(kit)) {
      frames = 0.0;
      for (std::uint32_t i = 0; i < library->status().samples; ++i) {
        frames = std::max(frames, static_cast<double>(library->stream(i).frames));
      }
    }
    slowest = 1.0;
    for (std::size_t piece = 0; piece < piece_count; ++piece) {
      const double tune = values[st::piece_index(static_cast<Piece>(piece), st::piece::tune)];
      slowest = std::max(slowest, std::exp2(-tune / semitones_per_octave));
    }
  }
  const double seconds = frames / static_cast<double>(library::pack_rate) * slowest +
                         humanise_delay_seconds + declick_seconds + tail_margin_seconds;
  return static_cast<std::uint64_t>(std::ceil(seconds * sample_rate));
}

const EngineDescriptor descriptor{"stick", "Stick", 1, st::parameters, stick_presets::presets};
const SynthEngine stick_engine{.descriptor = &descriptor,
                               .create_kernel = &create_kernel,
                               .tail_frames = &tail_frames,
                               .fetch_content = &fetch_content};

// The General MIDI drum keys, from B1 (35).
struct KeyMap {
  int key = 0;
  Piece piece = Piece::kick;
  Articulation articulation = Articulation::hit;
  bool at_pedal = false;
};
constexpr std::array<KeyMap, 15> gm_keys{{
    {35, Piece::kick, Articulation::alt},
    {36, Piece::kick, Articulation::hit},
    {37, Piece::snare, Articulation::sidestick},
    {38, Piece::snare, Articulation::hit},
    // E2, the electric snare in General MIDI: the kit's second snare.
    {40, Piece::snare, Articulation::alt},
    {42, Piece::hihat, Articulation::closed, true},
    {44, Piece::hihat, Articulation::pedal},
    {46, Piece::hihat, Articulation::open},
    {49, Piece::crash1, Articulation::hit},
    {51, Piece::ride, Articulation::hit},
    {52, Piece::china, Articulation::hit},
    {53, Piece::ride, Articulation::bell},
    {55, Piece::splash, Articulation::hit},
    {57, Piece::crash2, Articulation::hit},
    // B3, the second ride in General MIDI: the kit's ride.
    {59, Piece::ride, Articulation::hit},
}};
// The six tom keys, high to low: D3, C3, B2, A2, G2, F2 (50 down to 41).
constexpr std::array<int, 6> tom_keys{50, 48, 47, 45, 43, 41};

} // namespace

const SynthEngine& engine() { return stick_engine; }

std::optional<double> openness_of(Articulation articulation) noexcept {
  constexpr double half_open = 0.5;
  switch (articulation) {
  case Articulation::closed:
    return 0.0;
  case Articulation::half:
    return half_open;
  case Articulation::open:
    return 1.0;
  case Articulation::hit:
  case Articulation::alt:
  case Articulation::sidestick:
  case Articulation::bell:
  case Articulation::pedal:
    break;
  }
  return std::nullopt;
}

bool plays(Piece piece, Articulation articulation) noexcept {
  switch (piece) {
  case Piece::kick:
    return articulation == Articulation::hit || articulation == Articulation::alt;
  case Piece::snare:
    return articulation == Articulation::hit || articulation == Articulation::alt ||
           articulation == Articulation::sidestick;
  case Piece::hihat:
    return openness_of(articulation).has_value() || articulation == Articulation::pedal;
  case Piece::ride:
    return articulation == Articulation::hit || articulation == Articulation::bell;
  case Piece::tom1:
  case Piece::tom2:
  case Piece::tom3:
  case Piece::tom4:
  case Piece::tom5:
  case Piece::crash1:
  case Piece::crash2:
  case Piece::splash:
  case Piece::china:
    break;
  }
  return articulation == Articulation::hit;
}

std::optional<Mapped> map_key(int key, std::size_t toms) noexcept {
  for (const auto& entry : gm_keys) {
    if (entry.key == key) {
      return Mapped{entry.piece, entry.articulation, entry.at_pedal};
    }
  }
  const auto* found = std::ranges::find(tom_keys, key);
  if (found == tom_keys.end() || toms == 0) {
    return std::nullopt;
  }
  // The six keys split evenly over the toms, high to low.
  const auto at = static_cast<std::size_t>(found - tom_keys.begin());
  const std::size_t tom = std::min(toms, max_toms) * at / tom_keys.size();
  return Mapped{static_cast<Piece>(static_cast<std::size_t>(Piece::tom1) + tom), Articulation::hit,
                false};
}

} // namespace oscillate::native_dsp::synth::stick
