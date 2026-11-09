#include "oscillate/native_dsp/synth/bow.hpp"

#include "bow_library.hpp"
#include "bow_presets.hpp"
#include "bow_table.hpp"
#include "dsp/biquad.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/effects/expanse.hpp"
#include "oscillate/native_dsp/parameter_control.hpp"
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
#include <string_view>
#include <vector>

namespace oscillate::native_dsp::synth::bow {
namespace {

namespace bt = bow_table;
using detail::Recorded;
using sample_stream::SincTable;

namespace p {
constexpr std::size_t section = bt::index_of("bow.section");
constexpr std::size_t mode = bt::index_of("bow.mode");
constexpr std::size_t articulation = bt::index_of("bow.articulation");
constexpr std::size_t wheel = bt::index_of("bow.perf.wheel");
constexpr std::size_t vibrato = bt::index_of("bow.vibrato");
constexpr std::size_t tone = bt::index_of("bow.tone");
constexpr std::size_t players = bt::index_of("bow.players");
constexpr std::size_t humanise = bt::index_of("bow.humanise");
constexpr std::size_t legato = bt::index_of("bow.legato");
constexpr std::size_t release = bt::index_of("bow.release");
constexpr std::size_t space = bt::index_of("bow.space");
constexpr std::size_t tune = bt::index_of("bow.tune");
constexpr std::size_t level = bt::index_of("bow.level");
} // namespace p

constexpr std::size_t max_frames = control_interval_frames;
// Every player of every note, held or releasing: 16 notes of 4 players.
constexpr std::size_t max_voices = 64;
// Frames gathered per resampling pass.
constexpr std::size_t scratch_frames = 512;
constexpr double semitones_per_octave = 12.0;
constexpr double cents_per_semitone = 100.0;
constexpr double ms_per_second = 1'000.0;
constexpr double quarter_turn = std::numbers::pi / 2.0;
constexpr double max_velocity = 127.0;

// ---- Dynamics --------------------------------------------------------------------------
// Below this much wheel the bow lifts toward silence, by up to niente_db
// at the bottom, so a swell can start from nothing.
constexpr double niente_span = 0.15;
constexpr double niente_db = -24.0;
// The bow's pressure brightens the sound: a high shelf this far down at
// the wheel's bottom, flat at its top.
constexpr double wheel_shelf_db = -5.0;
// A soft note's sustain eases in over up to this long (velocity 1); at full
// velocity the recording's own attack plays.
constexpr double soft_attack_seconds = 0.14;

// ---- Players -----------------------------------------------------------------------------
// Ensemble: each layer after the first plays a neighbouring take, detuned
// and a little late, from its own seat.
constexpr std::array<int, max_ensemble_layers> layer_key_shifts{0, 2, -2, 3};
constexpr double ensemble_detune_cents = 7.0;
constexpr double ensemble_delay_seconds = 0.022;
constexpr double layer_pan_spread = 0.22;
// Humanise at full: each player's tuning, timing and level spread.
constexpr double humanise_cents = 8.0;
constexpr double humanise_delay_seconds = 0.02;
constexpr double humanise_level_db = 1.5;
// Violins II read the violin takes a whole tone up, so doubled with
// Violins I they never play the same recording at the same pitch.
constexpr int second_violins_shift = 2;
// Seats across the stage (-1 left .. 1 right), in Instrument order, and
// Violins II's.
constexpr std::array<double, instrument_count> seats{-0.45, 0.15, 0.4, 0.6};
constexpr double second_violins_seat = -0.15;
// Ensemble splits the keyboard by register: basses below the first,
// cellos below the second, violas below the third, violins above.
constexpr std::array<int, 3> ensemble_splits{40, 53, 65};

// ---- Vibrato -------------------------------------------------------------------------------
// Added to the recorded vibrato: up to this deep at full, around this rate
// (each player a little different), blooming after the note settles.
constexpr double vibrato_cents = 18.0;
constexpr double vibrato_hz = 5.4;
constexpr double vibrato_rate_spread = 0.08;
constexpr double vibrato_delay_seconds = 0.25;
constexpr double vibrato_bloom_seconds = 0.6;

// ---- Legato and release --------------------------------------------------------------------
// The note before bends to the new pitch over this share of the legato
// time, unless the interval is wider than an octave (then it only fades).
constexpr double bend_share = 1.0 / 3.0;
constexpr double max_bend_semitones = 12.0;
// A release falls this far over the Release time, then the voice stops.
constexpr double release_floor_db = -60.0;

// ---- Tone ------------------------------------------------------------------------------------
// Toward sul tasto (s, 0..1) the bow plays over the fingerboard: darker and
// rounder; toward sul ponticello (p) it plays by the bridge: the
// fundamental thins, the upper partials glassy. Three bands, their gains
// in dB per unit of s and p.
constexpr double low_hz = 220.0;
constexpr double low_tasto_db = 1.5;
constexpr double low_ponticello_db = -6.0;
constexpr double presence_hz = 4'500.0;
constexpr double presence_q = 0.9;
constexpr double presence_tasto_db = -3.0;
constexpr double presence_ponticello_db = 6.0;
constexpr double air_hz = 7'000.0;
constexpr double air_tasto_db = -10.0;
constexpr double air_ponticello_db = 4.0;

// ---- Output ------------------------------------------------------------------------------------
// Space: a fixed hall on Expanse, its mix the Space control.
constexpr double room_size = 0.55;
constexpr double room_decay_seconds = 2.8;
constexpr double room_predelay_ms = 18.0;
constexpr double mute_db = -59.9;
constexpr double meter_floor_db = -120.0;
constexpr double safety_knee = 0.9;
constexpr double declick_seconds = 0.0015;
constexpr double tail_margin_seconds = 0.25;
// Envelopes (fades, dynamics, pitch) are evaluated every this many frames
// of a voice's life and interpolated between: on the voice's own clock, so
// any block split renders the same.
constexpr std::uint64_t envelope_step = 16;
// Humanise seeds from the song position at this resolution, in a cycle of
// this many beats, so loops repeat and export equals playback.
constexpr double beat_resolution = 65'536.0;
constexpr double seed_cycle_beats = 8.0;
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

[[nodiscard]] Recorded recorded_for(Articulation articulation) noexcept {
  switch (articulation) {
  case Articulation::staccato:
    return Recorded::staccato;
  case Articulation::pizzicato:
    return Recorded::pizzicato;
  case Articulation::tremolo:
    return Recorded::tremolo;
  case Articulation::sustain:
  case Articulation::legato:
    break;
  }
  return Recorded::sustain;
}

// A seeded value in [-1, 1).
[[nodiscard]] double signed_unit(std::uint64_t seed) noexcept {
  return 2.0 * native_dsp::dsp::unit_from_bits(native_dsp::dsp::splitmix64(seed)) - 1.0;
}

// The resampler's tables, shared by every Bow: built once on control.
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

// One recorded layer of a player, read from memory; a sustain loops.
struct Reader {
  const sample_stream::Sample* stream = nullptr;
  const std::int16_t* pcm = nullptr;
  std::uint32_t channels = 1;
  std::int64_t loop_start = 0;
  std::int64_t loop_end = 0;
  bool looping = false;
  double position = 0.0;
  double ratio = 1.0;    // Reading speed before the voice's pitch.
  double level_db = 0.0; // The take's steady level.
};

enum class Fade : std::uint8_t { none, linear, equal_power, release };

// A point on a voice's envelope: its age, and the frames since the control
// tick (where the wheel is read).
struct Moment {
  double age = 0.0;
  double since_tick = 0.0;
};

// Where a read writes its left and right frames (null: it only moves on).
struct Frames {
  float* left = nullptr;
  float* right = nullptr;
};

// A read's per-frame reading speeds, and the fastest each frame's grid
// point allows (which picks the resampler's table).
struct Speeds {
  const double* steps = nullptr;
  const double* peaks = nullptr;
};

// One player of one note.
struct Voice {
  bool active = false;
  bool held = false;
  bool one_shot = false;
  int key = 0;
  foundation::IdBits clip{};
  foundation::IdBits note{};
  std::uint32_t group = 0; // The note: its players share it.
  std::array<Reader, max_layers> readers{};
  std::uint32_t layers = 0;
  double velocity_dynamics = 0.0; // One-shots play their velocity, not the wheel.
  double pan_left = 1.0;
  double pan_right = 1.0;
  double gain = 1.0;
  double cents = 0.0;
  std::uint64_t delay = 0; // Frames before it sounds.
  double vibrato_phase = 0.0;
  double vibrato_step = 0.0; // Radians per frame.
  Fade in = Fade::none;
  double in_frames = 1.0;
  Fade out = Fade::none;
  double out_frames = 1.0;
  std::uint64_t out_age = 0;
  // A legato's bend toward the next note.
  double bend_target = 0.0;
  std::uint64_t bend_from = 0;
  double bend_frames = 1.0;
  std::uint64_t age = 0;
  std::uint64_t started = 0;
  // Envelope values now and their steps per frame to the next grid point.
  std::array<double, max_layers> weight{};
  std::array<double, max_layers> weight_step{};
  double speed = 1.0;
  double speed_step = 0.0;
  // The fastest the voice reads until the next grid point: it picks the
  // resampler's table there, so the choice never depends on the block split.
  double speed_peak = 1.0;
};

class BowKernel final : public SynthKernel {
public:
  BowKernel(double sample_rate, library::State state)
      : rate_(sample_rate), state_(state),
        base_step_(static_cast<double>(library::pack_rate) / sample_rate), tables_(tables()),
        room_(effects::expanse::engine().create_kernel(room_defaults(), sample_rate)) {
    room_values_ = room_defaults();
    declick_frames_ = std::max(1.0, declick_seconds * sample_rate);
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
    if (std::ranges::none_of(voices_, &Voice::active)) {
      adopt_library();
    }
    // A changed articulation on the panel wins over a latched keyswitch.
    const auto panel = choice(values[p::articulation], articulation_count);
    if (panel != panel_articulation_) {
      panel_articulation_ = panel;
      latched_ = false;
    }
    peak_ = 0.0;
    const double steps = std::max<double>(1.0, frames_to_next_tick);
    const double gain = values[p::level] <= mute_db ? 0.0 : db_to_gain(values[p::level]);
    gain_step_ = primed_ ? (gain - gain_) / steps : 0.0;
    // The wheel glides from the last tick's value to this one's over the
    // interval, as a function of the frame, so any block split reads alike.
    wheel_from_ = primed_ ? wheel_to_ : values[p::wheel];
    wheel_to_ = values[p::wheel];
    wheel_span_ = steps;
    if (!primed_) {
      gain_ = gain;
    }
    if (!primed_ || values[p::tone] != tone_ || values[p::wheel] != shelf_wheel_) {
      tone_ = values[p::tone];
      shelf_wheel_ = values[p::wheel];
      set_tone();
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
    if (values_.empty()) {
      return;
    }
    if (is_keyswitch(event.key)) {
      latched_articulation_ = static_cast<Articulation>(event.key - first_keyswitch);
      latched_ = true;
      return;
    }
    if (library_ == nullptr) {
      return;
    }
    const auto section = static_cast<Section>(choice(values_[p::section], section_count));
    const auto range = playable(section);
    if (event.key < range.low || event.key > range.high) {
      return;
    }
    const auto mode = static_cast<Mode>(choice(values_[p::mode], bt::modes.size()));
    const auto articulation = playing();
    const auto recorded = recorded_for(articulation);
    const auto instrument = instrument_for(section, event.key);
    if (library_->layers(mode, instrument, recorded) == 0) {
      return;
    }
    ++notes_;
    ++group_;
    const std::uint32_t round_robin = round_robin_++;
    const double legato_frames = frames_of_ms(values_[p::legato]);
    // Legato: a note over a held one joins it past its own attack.
    const std::uint32_t from = articulation == Articulation::legato ? held_group() : 0;
    if (from != 0) {
      const double interval = static_cast<double>(event.key - group_key(from));
      for (auto& v : voices_) {
        if (v.active && v.held && v.group == from) {
          if (std::fabs(interval) <= max_bend_semitones) {
            v.bend_target = interval;
            v.bend_from = v.age;
            v.bend_frames = std::max(1.0, bend_share * legato_frames);
          }
          fade_out(v, Fade::equal_power, legato_frames);
        }
      }
    } else if (articulation == Articulation::legato) {
      // A new phrase: whatever still sounds of the last one lets go.
      for (auto& v : voices_) {
        if (v.active && v.held) {
          fade_out(v, Fade::release, release_frames());
        }
      }
    }

    const auto layers = ensemble_layers(values_[p::players]);
    const double humanise = values_[p::humanise];
    const auto seed_base = seed_of(event.key);
    const double velocity = static_cast<double>(event.velocity) / max_velocity;
    const double layer_gain = 1.0 / std::sqrt(static_cast<double>(layers));
    const double seat = section == Section::violins_2 ? second_violins_seat
                                                      : seats[static_cast<std::size_t>(instrument)];
    const int section_shift = section == Section::violins_2 ? second_violins_shift : 0;
    for (std::size_t j = 0; j < layers; ++j) {
      const auto seed = seed_base + j * max_voices;
      Voice& v = free_voice();
      v = Voice{};
      v.active = true;
      v.held = true;
      v.key = event.key;
      v.clip = event.clip;
      v.note = event.note;
      v.group = group_;
      v.started = clock_;
      v.one_shot = !detail::looped(recorded);
      v.velocity_dynamics = velocity;
      // Tuning: the player's own detune, then Humanise.
      const double detune = j == 0 ? 0.0 : ensemble_detune_cents * signed_unit(seed + 1);
      v.cents = detune + humanise * humanise_cents * signed_unit(seed + 2);
      v.gain = layer_gain * db_to_gain(humanise * humanise_level_db * signed_unit(seed + 3));
      const double pan =
          std::clamp(seat + (j == 0 ? 0.0 : layer_pan_spread * signed_unit(seed + 4)), -1.0, 1.0);
      const double theta = (pan + 1.0) * std::numbers::pi / 4.0;
      v.pan_left = std::cos(theta) * std::numbers::sqrt2;
      v.pan_right = std::sin(theta) * std::numbers::sqrt2;
      v.vibrato_phase = std::numbers::pi * (signed_unit(seed + 5) + 1.0);
      v.vibrato_step = 2.0 * std::numbers::pi * vibrato_hz *
                       (1.0 + vibrato_rate_spread * signed_unit(seed + 6)) / rate_;
      // Fill the readers: one per recorded layer, the round robin by note.
      const auto shift = section_shift + layer_key_shifts[j];
      const auto recorded_layers = library_->layers(mode, instrument, recorded);
      double start = 0.0;
      for (std::uint32_t l = 0; l < recorded_layers; ++l) {
        const auto& pick = library_->pick(mode, instrument, recorded, event.key + shift, l);
        if (pick.count == 0) {
          continue;
        }
        const auto index = pick.samples[(round_robin + j) % pick.count];
        if (from != 0 && l == 0) {
          start = library_->sample(index).attack_end;
        }
        set_reader(v.readers[v.layers++], index, event.key, from != 0, start);
      }
      if (v.layers == 0) {
        v.active = false;
        continue;
      }
      if (from != 0) {
        // Past the attack, crossfading in: no timing spread, so the join
        // lands where it was played.
        v.in = Fade::equal_power;
        v.in_frames = legato_frames;
      } else {
        v.delay = static_cast<std::uint64_t>(std::llround(
            rate_ * ((j == 0 ? 0.0 : ensemble_delay_seconds * (signed_unit(seed + 7) + 1.0) / 2.0) +
                     humanise * humanise_delay_seconds * (signed_unit(seed + 8) + 1.0) / 2.0)));
        if (!v.one_shot) {
          const double soft = soft_attack_seconds * (1.0 - velocity) * rate_;
          if (soft >= 1.0) {
            v.in = Fade::linear;
            v.in_frames = soft;
          }
        }
      }
      prime(v);
    }
  }

  void note_off(const NoteEvent& event) noexcept override {
    if (is_keyswitch(event.key)) {
      return;
    }
    const double frames = release_frames();
    for (auto& v : voices_) {
      if (!v.active || !v.held || !(v.clip == event.clip) || !(v.note == event.note)) {
        continue;
      }
      if (v.one_shot) {
        v.held = false; // Plays out.
      } else {
        fade_out(v, Fade::release, frames);
      }
    }
  }

  void render(std::span<engine::StereoFrame> output) noexcept override {
    for (std::size_t offset = 0; offset < output.size(); offset += max_frames) {
      const std::size_t frames = std::min(max_frames, output.size() - offset);
      std::fill_n(mix_left_.begin(), frames, 0.0);
      std::fill_n(mix_right_.begin(), frames, 0.0);
      chunk_since_tick_ = static_cast<double>(frames_since_tick_ + offset);
      for (auto& v : voices_) {
        if (v.active) {
          render_voice(v, frames);
        }
      }
      finish(output.subspan(offset, frames));
    }
    clock_ += output.size();
    frames_since_tick_ += output.size();
  }

  void reset() noexcept override {
    declick_ = last_output_;
    declick_left_ = declick_frames_;
    for (auto& v : voices_) {
      v.active = false;
    }
    latched_ = false;
    round_robin_ = 0;
    for (auto& band : bands_) {
      band.reset();
    }
    if (room_ != nullptr) {
      room_->reset();
    }
  }

  // Offline renders: the library's first read finishes before any block.
  void await_content() noexcept override {
    if (library_ == nullptr) {
      library::wait();
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
    out[telemetry::articulation] = static_cast<float>(playing());
    out[telemetry::latched] = latched_ ? 1.0F : 0.0F;
    out[telemetry::dynamics] = static_cast<float>(wheel_to_);
    out[telemetry::voices] = static_cast<float>(status().active_voices);
    const double peak_db = peak_ > 0.0 ? 20.0 * std::log10(peak_) : meter_floor_db;
    out[telemetry::output_peak_db] = static_cast<float>(std::max(meter_floor_db, peak_db));
    // Held keys, each once, and the newest held note's vibrato.
    std::size_t held = 0;
    const Voice* newest = nullptr;
    for (const auto& v : voices_) {
      if (!v.active || !v.held) {
        continue;
      }
      if (newest == nullptr || v.started > newest->started) {
        newest = &v;
      }
      const float* const first = out.data() + telemetry::held;
      const float* const last = first + held;
      if (held < telemetry::held_slots &&
          std::find(first, last, static_cast<float>(v.key)) == last) {
        out[telemetry::held + held++] = static_cast<float>(v.key);
      }
    }
    out[telemetry::held_count] = static_cast<float>(held);
    for (std::size_t i = held; i < telemetry::held_slots; ++i) {
      out[telemetry::held + i] = -1.0F;
    }
    out[telemetry::vibrato] =
        newest == nullptr || values_.empty()
            ? 0.0F
            : static_cast<float>(values_[p::vibrato] * bloom(static_cast<double>(newest->age)));
    return telemetry::size;
  }

private:
  static constexpr std::uint32_t telemetry_wrap = (1U << 24U) - 1U;

  // An idle kernel takes up a library that has loaded since; without one it
  // reports what the last look found (a background read may have ended
  // missing or invalid).
  void adopt_library() noexcept {
    if (const auto* latest = detail::current(); latest != nullptr && latest != library_) {
      library_ = latest;
      state_ = library::State::ready;
    } else if (library_ == nullptr) {
      state_ = detail::published_state();
    }
  }

  [[nodiscard]] Articulation playing() const noexcept {
    return latched_ ? latched_articulation_ : static_cast<Articulation>(panel_articulation_);
  }

  [[nodiscard]] double frames_of_ms(double ms) const noexcept {
    return std::max(1.0, ms * rate_ / ms_per_second);
  }
  [[nodiscard]] double release_frames() const noexcept {
    return std::max(1.0, values_[p::release] * rate_);
  }

  // A seed from the note's place in an eight-beat cycle and its key.
  [[nodiscard]] std::uint64_t seed_of(int key) const noexcept {
    const double per_frame = sync_beats_per_second(transport_, rate_) / rate_;
    const double beat =
        transport_.beat_at_block_start + static_cast<double>(frames_since_tick_) * per_frame;
    const double place = std::fmod(std::max(0.0, beat), seed_cycle_beats);
    const auto step = static_cast<std::uint64_t>(std::llround(place * beat_resolution));
    return native_dsp::dsp::splitmix64(step * key_count + static_cast<std::uint64_t>(key));
  }

  // The newest note still held, or 0.
  [[nodiscard]] std::uint32_t held_group() const noexcept {
    const Voice* newest = nullptr;
    for (const auto& v : voices_) {
      if (v.active && v.held && !v.one_shot && (newest == nullptr || v.started > newest->started)) {
        newest = &v;
      }
    }
    return newest == nullptr ? 0 : newest->group;
  }
  [[nodiscard]] int group_key(std::uint32_t group) const noexcept {
    for (const auto& v : voices_) {
      if (v.active && v.group == group) {
        return v.key;
      }
    }
    return 0;
  }

  // Fades a voice out from now: a legato crossfade or a release.
  static void fade_out(Voice& v, Fade fade, double frames) noexcept {
    v.held = false;
    v.out = fade;
    v.out_frames = std::max(1.0, frames);
    v.out_age = v.age;
  }

  // A free voice, else the oldest released one, else the oldest.
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

  void set_reader(Reader& r, std::uint32_t index, int key, bool legato,
                  double start) const noexcept {
    const auto& info = library_->sample(index);
    r.stream = &library_->stream(index);
    r.pcm = library_->resident(index);
    r.channels = info.channels;
    r.loop_start = info.loop_start;
    r.loop_end = info.loop_end;
    r.looping = info.loop_end > info.loop_start;
    // A legato note reads every layer from the first one's attack end.
    r.position = legato ? std::min(start, static_cast<double>(r.stream->frames - 1)) : 0.0;
    const double offset = static_cast<double>(key - info.key) - info.cents / cents_per_semitone;
    r.ratio = base_step_ * std::exp2(offset / semitones_per_octave);
    r.level_db = info.level_db;
  }

  // ---- Envelopes ------------------------------------------------------------------------

  [[nodiscard]] double bloom(double age) const noexcept {
    const double delay = vibrato_delay_seconds * rate_;
    return std::clamp((age - delay) / (vibrato_bloom_seconds * rate_), 0.0, 1.0);
  }

  [[nodiscard]] static double envelope(const Voice& v, double age) noexcept {
    double in = 1.0;
    if (v.in != Fade::none && age < v.in_frames) {
      const double u = age / v.in_frames;
      in = v.in == Fade::equal_power ? std::sin(quarter_turn * u) : u;
    }
    double out = 1.0;
    if (v.out != Fade::none) {
      const double u = std::clamp((age - static_cast<double>(v.out_age)) / v.out_frames, 0.0, 1.0);
      switch (v.out) {
      case Fade::equal_power:
        out = std::cos(quarter_turn * u);
        break;
      case Fade::release:
        out = u >= 1.0 ? 0.0 : db_to_gain(release_floor_db * u);
        break;
      case Fade::linear:
        out = 1.0 - u;
        break;
      case Fade::none:
        break;
      }
    }
    return v.gain * in * out;
  }

  // The wheel `since` frames after the tick.
  [[nodiscard]] double wheel_at(double since) const noexcept {
    const double u = std::clamp(since / wheel_span_, 0.0, 1.0);
    return std::clamp(wheel_from_ + u * (wheel_to_ - wheel_from_), 0.0, 1.0);
  }

  // The dynamics a voice plays: one-shots their velocity, the rest the wheel.
  [[nodiscard]] double dynamics(const Voice& v, double since) const noexcept {
    return v.one_shot ? v.velocity_dynamics : wheel_at(since);
  }

  // Each layer's gain: an equal-power crossfade between the two layers the
  // dynamics fall between, each brought to the level the dynamics play at
  // (interpolated in dB between the layers' own), lifting toward silence at
  // the wheel's bottom.
  void layer_weights(const Voice& v, Moment at,
                     std::array<double, max_layers>& out) const noexcept {
    out.fill(0.0);
    const double d = dynamics(v, at.since_tick);
    const double e = envelope(v, at.age);
    if (v.layers == 1) {
      out[0] = e * niente(d);
      return;
    }
    const double x = d * static_cast<double>(v.layers - 1);
    const auto low = std::min<std::uint32_t>(static_cast<std::uint32_t>(x), v.layers - 2);
    const double f = x - static_cast<double>(low);
    const double target_db =
        v.readers[low].level_db + f * (v.readers[low + 1].level_db - v.readers[low].level_db);
    const double shared = e * niente(d);
    out[low] =
        shared * std::cos(quarter_turn * f) * db_to_gain(target_db - v.readers[low].level_db);
    out[low + 1] =
        shared * std::sin(quarter_turn * f) * db_to_gain(target_db - v.readers[low + 1].level_db);
  }

  [[nodiscard]] static double niente(double d) noexcept {
    return db_to_gain(niente_db * std::max(0.0, 1.0 - d / niente_span));
  }

  // The voice's reading speed relative to its readers' own: Tune, its
  // tuning, the legato bend and the added vibrato.
  [[nodiscard]] double speed(const Voice& v, double age) const noexcept {
    double semitones = (values_[p::tune] + v.cents) / cents_per_semitone;
    if (v.bend_target != 0.0) {
      semitones += v.bend_target *
                   std::clamp((age - static_cast<double>(v.bend_from)) / v.bend_frames, 0.0, 1.0);
    }
    const double depth = values_[p::vibrato] * vibrato_cents * bloom(age) / cents_per_semitone;
    semitones += depth * std::sin(v.vibrato_phase + v.vibrato_step * age);
    return std::exp2(semitones / semitones_per_octave);
  }

  // The envelope from its first frame.
  void prime(Voice& v) const noexcept {
    layer_weights(v, {0.0, static_cast<double>(frames_since_tick_)}, v.weight);
    v.speed = speed(v, 0.0);
    v.speed_peak = v.speed;
  }

  // The envelope's next grid point, `since` frames after the tick.
  void retarget(Voice& v, double since) const noexcept {
    const double next = static_cast<double>(v.age + envelope_step);
    constexpr auto steps = static_cast<double>(envelope_step);
    std::array<double, max_layers> target{};
    layer_weights(v, {next, since + steps}, target);
    for (std::uint32_t l = 0; l < max_layers; ++l) {
      v.weight_step[l] = (target[l] - v.weight[l]) / steps;
    }
    const double target_speed = speed(v, next);
    v.speed_step = (target_speed - v.speed) / steps;
    v.speed_peak = std::max(v.speed, target_speed);
  }

  // ---- Reading ----------------------------------------------------------------------------

  // Source frames [first, first + count) of a take as floats: looped when it
  // loops, silence outside the recording. Mono copies left to right.
  void gather(const Reader& r, std::int64_t first, std::size_t count) noexcept {
    const float scale = r.stream->scale;
    const auto frames = static_cast<std::int64_t>(r.stream->frames);
    const std::int64_t length = r.loop_end - r.loop_start;
    for (std::size_t j = 0; j < count; ++j) {
      std::int64_t i = first + static_cast<std::int64_t>(j);
      if (r.looping && i >= r.loop_end) {
        i = r.loop_start + (i - r.loop_end) % length;
      }
      if (i < 0 || i >= frames) {
        scratch_left_[j] = 0.0F;
        scratch_right_[j] = 0.0F;
        continue;
      }
      const auto at = static_cast<std::size_t>(i) * r.channels;
      scratch_left_[j] = static_cast<float>(r.pcm[at]) * scale;
      scratch_right_[j] =
          r.channels == 1 ? scratch_left_[j] : static_cast<float>(r.pcm[at + 1]) * scale;
    }
  }

  // Reads `count` frames of a take into `left`/`right` (null: only moves on),
  // stepping by `steps` per frame. A looping take jumps back to its loop start
  // on the frame it reaches its loop end, whatever the pass.
  void read(Reader& r, Speeds speeds, std::size_t count, Frames out) noexcept {
    float* const left = out.left;
    float* const right = out.right;
    double fastest = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
      fastest = std::max(fastest, speeds.steps[i]);
    }
    double peak = -1.0;
    const SincTable* table = &tables_.front();
    const auto per_output = static_cast<std::size_t>(std::ceil(fastest));
    const std::size_t per_pass = std::max<std::size_t>(1, (scratch_frames - SincTable::taps - 2) /
                                                              std::max<std::size_t>(1, per_output));
    const double loop_length = static_cast<double>(r.loop_end - r.loop_start);
    std::size_t done = 0;
    while (done < count) {
      const std::size_t n = std::min(count - done, per_pass);
      std::int64_t first = 0;
      if (left != nullptr) {
        first = static_cast<std::int64_t>(std::floor(r.position)) -
                static_cast<std::int64_t>(SincTable::left);
        const double reach = r.position + static_cast<double>(n) * fastest;
        const auto last = static_cast<std::int64_t>(std::floor(reach)) +
                          static_cast<std::int64_t>(SincTable::taps) - SincTable::left;
        gather(r, first, std::min(static_cast<std::size_t>(last - first + 1), scratch_frames));
      }
      std::size_t i = 0;
      while (i < n) {
        if (left != nullptr) {
          const double base = std::floor(r.position);
          const auto at = static_cast<std::size_t>(static_cast<std::int64_t>(base) - first) -
                          static_cast<std::size_t>(SincTable::left);
          if (speeds.peaks[done + i] != peak) {
            peak = speeds.peaks[done + i];
            table = &table_for(tables_, r.ratio * peak);
          }
          table->read(scratch_left_.data() + at, scratch_right_.data() + at, r.position - base,
                      left[done + i], right[done + i]);
        }
        r.position += speeds.steps[done + i];
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

  void render_voice(Voice& v, std::size_t frames) noexcept {
    std::size_t first = 0;
    if (v.delay > 0) {
      const auto waited = std::min<std::uint64_t>(v.delay, frames);
      v.delay -= waited;
      first = static_cast<std::size_t>(waited);
      if (first == frames) {
        return;
      }
    }
    const std::size_t count = frames - first;
    // The speed per frame on the envelope grid, then each layer read at it.
    std::array<double, max_frames> speeds{};
    std::array<double, max_frames> peaks{};
    std::array<std::array<double, max_frames>, max_layers> weights{};
    for (std::size_t i = 0; i < count; ++i) {
      if (v.age % envelope_step == 0) {
        retarget(v, chunk_since_tick_ + static_cast<double>(first + i));
      }
      speeds[i] = v.speed;
      peaks[i] = v.speed_peak;
      for (std::uint32_t l = 0; l < v.layers; ++l) {
        weights[l][i] = v.weight[l];
        v.weight[l] += v.weight_step[l];
      }
      v.speed += v.speed_step;
      ++v.age;
    }
    std::array<double, max_frames> steps{};
    std::array<float, max_frames> left{};
    std::array<float, max_frames> right{};
    bool ended = true;
    for (std::uint32_t l = 0; l < v.layers; ++l) {
      auto& r = v.readers[l];
      for (std::size_t i = 0; i < count; ++i) {
        steps[i] = r.ratio * speeds[i];
      }
      const bool heard = std::any_of(weights[l].begin(), weights[l].begin() + count,
                                     [](double w) { return w != 0.0; });
      read(r, {steps.data(), peaks.data()}, count,
           heard ? Frames{left.data(), right.data()} : Frames{});
      if (heard) {
        for (std::size_t i = 0; i < count; ++i) {
          mix_left_[first + i] += weights[l][i] * left[i] * v.pan_left;
          mix_right_[first + i] += weights[l][i] * right[i] * v.pan_right;
        }
      }
      const double past = static_cast<double>(r.stream->frames + SincTable::taps);
      ended = ended && !r.looping && r.position >= past;
    }
    bool faded = false;
    if (v.out != Fade::none) {
      faded = static_cast<double>(v.age - v.out_age) >=
              v.out_frames + static_cast<double>(envelope_step);
    }
    if (faded || ended) {
      v.active = false;
    }
  }

  void set_tone() noexcept {
    const double tasto = std::max(0.0, (bt::ordinario - tone_) / bt::ordinario);
    const double ponticello = std::max(0.0, (tone_ - bt::ordinario) / (1.0 - bt::ordinario));
    const double low_db = low_tasto_db * tasto + low_ponticello_db * ponticello;
    const double presence_db = presence_tasto_db * tasto + presence_ponticello_db * ponticello;
    const double air_db = air_tasto_db * tasto + air_ponticello_db * ponticello +
                          wheel_shelf_db * (1.0 - std::clamp(shelf_wheel_, 0.0, 1.0));
    const auto low = dsp::rbj_shelf(false, db_to_gain(low_db), {.hz = low_hz, .rate = rate_});
    const auto presence =
        dsp::rbj_peak(db_to_gain(presence_db), {.hz = presence_hz, .rate = rate_}, presence_q);
    const auto air = dsp::rbj_shelf(true, db_to_gain(air_db), {.hz = air_hz, .rate = rate_});
    for (std::size_t channel = 0; channel < channels; ++channel) {
      bands_[channel * band_count].set(low);
      bands_[channel * band_count + 1].set(presence);
      bands_[channel * band_count + 2].set(air);
    }
  }

  [[nodiscard]] double band_pass(std::size_t channel, double x) noexcept {
    for (std::size_t b = 0; b < band_count; ++b) {
      x = bands_[channel * band_count + b].process(x);
    }
    return x;
  }

  // Tone, the hall, Level and the safety knee, into the output.
  void finish(std::span<engine::StereoFrame> output) noexcept {
    std::array<engine::StereoFrame, max_frames> wet{};
    for (std::size_t i = 0; i < output.size(); ++i) {
      wet[i] = {static_cast<float>(band_pass(0, mix_left_[i])),
                static_cast<float>(band_pass(1, mix_right_[i]))};
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

  static constexpr std::size_t channels = 2;
  static constexpr std::size_t band_count = 3;

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
  std::array<Voice, max_voices> voices_{};
  std::uint32_t group_ = 0;
  std::uint32_t round_robin_ = 0;
  std::size_t panel_articulation_ = articulation_count; // None read yet.
  Articulation latched_articulation_ = Articulation::sustain;
  bool latched_ = false;
  std::array<float, scratch_frames> scratch_left_{};
  std::array<float, scratch_frames> scratch_right_{};
  std::array<double, max_frames> mix_left_{};
  std::array<double, max_frames> mix_right_{};
  std::array<dsp::Biquad, channels * band_count> bands_{};
  double tone_ = bt::ordinario;
  double shelf_wheel_ = 1.0;
  double wheel_from_ = 0.0;
  double wheel_to_ = 0.0;
  double wheel_span_ = 1.0;
  double chunk_since_tick_ = 0.0;
  double gain_ = 0.0;
  double gain_step_ = 0.0;
  bool primed_ = false;
  double declick_frames_ = 1.0;
  double declick_left_ = 0.0;
  double declick_ = 0.0;
  double last_output_ = 0.0;
  double peak_ = 0.0;
  std::uint32_t notes_ = 0;
};

std::unique_ptr<SynthKernel> create_kernel(double sample_rate) {
  // Control: the first Bow starts reading the library, and the shared tables.
  const auto status = library::status();
  (void)tables();
  return std::make_unique<BowKernel>(sample_rate, status.state);
}

// The longest a released note can sound: its release (or a one-shot's
// whole length), then the hall's tail when Space is up.
std::uint64_t tail_frames(std::span<const double> values, double sample_rate) {
  double seconds = static_cast<double>(detail::max_sample_seconds);
  if (values.size() == bt::parameters.size()) {
    seconds = std::max(seconds, values[p::release]);
  } else {
    seconds = std::max(seconds, bt::max_release_seconds);
  }
  seconds += declick_seconds + tail_margin_seconds;
  auto frames = static_cast<std::uint64_t>(std::ceil(seconds * sample_rate));
  if (values.size() == bt::parameters.size() && values[p::space] > 0.0) {
    auto room = BowKernel::room_defaults();
    if (const auto mix = find_parameter(*effects::expanse::engine().descriptor, "expanse.mix")) {
      room[*mix] = values[p::space];
    }
    frames += effects::expanse::engine().tail_frames(room, sample_rate);
  }
  return frames;
}

const EngineDescriptor descriptor{"bow", "Bow", 1, bt::parameters, bow_presets::presets};
// The runtime's generic pedals hold Bow's notes.
const SynthEngine bow_engine{&descriptor, &create_kernel, &tail_frames, false};

} // namespace

const SynthEngine& engine() { return bow_engine; }

KeyRange playable(Section section) noexcept {
  switch (section) {
  case Section::violins_1:
  case Section::violins_2:
    return instrument_ranges[static_cast<std::size_t>(Instrument::violin)];
  case Section::violas:
    return instrument_ranges[static_cast<std::size_t>(Instrument::viola)];
  case Section::cellos:
    return instrument_ranges[static_cast<std::size_t>(Instrument::cello)];
  case Section::basses:
    return instrument_ranges[static_cast<std::size_t>(Instrument::bass)];
  case Section::ensemble:
    break;
  }
  return {first_key, last_key};
}

Instrument instrument_for(Section section, int key) noexcept {
  switch (section) {
  case Section::violins_1:
  case Section::violins_2:
    return Instrument::violin;
  case Section::violas:
    return Instrument::viola;
  case Section::cellos:
    return Instrument::cello;
  case Section::basses:
    return Instrument::bass;
  case Section::ensemble:
    break;
  }
  if (key < ensemble_splits[0]) {
    return Instrument::bass;
  }
  if (key < ensemble_splits[1]) {
    return Instrument::cello;
  }
  if (key < ensemble_splits[2]) {
    return Instrument::viola;
  }
  return Instrument::violin;
}

std::size_t ensemble_layers(double players) noexcept {
  const auto count = std::clamp(static_cast<int>(std::lround(players)), min_players, max_players);
  return static_cast<std::size_t>((count + players_per_layer - 1) / players_per_layer);
}

std::string_view dynamic_name(double wheel) noexcept {
  std::size_t at = 0;
  while (at < dynamic_tops.size() && wheel > dynamic_tops[at]) {
    ++at;
  }
  return dynamic_names[at];
}

} // namespace oscillate::native_dsp::synth::bow
