#include "oscillate/native_dsp/synth/knock.hpp"

#include "dsp/ramp.hpp"
#include "knock_presets.hpp"
#include "knock_table.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/synth/knock_striker.hpp"
#include "oscillate/native_dsp/synth/modal_dsp.hpp"
#include "oscillate/native_dsp/synth/parameter_control.hpp"
#include "oscillate/native_dsp/synth/tempo_sync.hpp"
#include "oscillate/native_dsp/synth/voice_pool.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numbers>
#include <span>
#include <vector>

namespace oscillate::native_dsp::synth::knock {
namespace {

namespace o = knock_table::offset;
namespace gl = knock_table::global;
using knock_table::pad_parameters;
using Ramp = dsp::Ramp;

static_assert(knock_table::pad_count == pad_count, "the table and the header agree on pads");
static_assert(knock_table::strikers.size() == striker_type_count);

constexpr double pi = std::numbers::pi;
constexpr double two_pi = 2.0 * std::numbers::pi;

// ---- Strikers -----------------------------------------------------------------

// Each striker type: its mass (kg), contact exponent, hysteresis as a
// fraction of its contact time, and how much longer than a stick it stays
// on the surface.
struct StrikerModel {
  double mass;
  double exponent;
  double hysteresis;
  double contact_scale;
};
constexpr std::array<StrikerModel, striker_type_count> striker_models{{
    {0.025, 2.5, 0.03, 1.0}, // Mallet: felt on a handle.
    {0.008, 1.5, 0.01, 1.0}, // Stick: hardwood, Hertzian.
    {0.03, 2.2, 0.05, 1.7},  // Finger: flesh over bone, lossy.
    {0.004, 1.8, 0.04, 1.2}, // Brush: a bundle of bristles.
}};
constexpr double soft_contact_seconds = 0.0065; // Hardness 0...
constexpr double hard_contact_ratio = 0.035;    // ...to 0.23 ms at hardness 1.
constexpr double reference_speed = 1.5;         // m/s at the reference velocity.
// Velocity scales the speed: 0.35 + 0.65 v.
constexpr double velocity_floor = 0.35;
constexpr double velocity_hardening = 0.35;
constexpr double velocity_hardening_centre = 0.8;
// A striker bouncing off a much heavier surface takes about this many
// times its momentum into it.
constexpr double momentum_ratio = 1.9;

[[nodiscard]] const StrikerModel& model(StrikerType type) noexcept {
  return striker_models[static_cast<std::size_t>(type)];
}

[[nodiscard]] double clamp01(double x) noexcept { return std::clamp(x, 0.0, 1.0); }

} // namespace

double contact_target_seconds(StrikerType type, double hardness) noexcept {
  return soft_contact_seconds * std::pow(hard_contact_ratio, clamp01(hardness)) *
         model(type).contact_scale;
}

double velocity_hardness(double hardness, double velocity, double sensitivity) noexcept {
  return clamp01(hardness +
                 sensitivity * velocity_hardening * (velocity - velocity_hardening_centre));
}

double strike_speed(double velocity, double sensitivity) noexcept {
  const double curve = (velocity_floor + (1.0 - velocity_floor) * clamp01(velocity)) /
                       (velocity_floor + (1.0 - velocity_floor) * reference_velocity_unit);
  return reference_speed * ((1.0 - sensitivity) + sensitivity * curve);
}

modal::Striker make_striker(StrikerType type, double hardness) noexcept {
  const auto& m = model(type);
  const double seconds = contact_target_seconds(type, hardness);
  modal::Striker striker;
  striker.mass = m.mass;
  striker.law.p = m.exponent;
  striker.law.k = modal::rigid_contact_stiffness(m.mass, m.exponent, reference_speed, seconds);
  striker.law.alpha = m.hysteresis * seconds;
  return striker;
}

double surface_mass(StrikerType type, double shape_power) noexcept {
  constexpr double least_shape_power = 1e-6;
  return surface_mass_ratio * model(type).mass * std::max(shape_power, least_shape_power);
}

namespace {

// ---- Materials and the mode cloud ----------------------------------------------

constexpr std::size_t max_material_modes = 14;
struct MaterialModes {
  std::array<double, max_material_modes> ratios;
  std::array<double, max_material_modes> amps;
  std::size_t count;
  double damp; // How much faster the upper modes die.
};
// Wood, Skin, Metal, Glass, Tube, Card: knock_table::materials order.
constexpr std::array<MaterialModes, 6> material_modes{{
    {{1, 2.756, 5.404, 8.933, 13.344, 18.64}, {1, .55, .32, .18, .1, .06}, 6, 2.2},
    {{1, 1.594, 2.136, 2.296, 2.653, 2.918, 3.156, 3.501, 3.6, 3.652, 4.06, 4.154},
     {1, .8, .62, .5, .45, .4, .34, .3, .27, .24, .2, .18},
     12,
     1.0},
    {{1, 1.59, 2.14, 2.65, 3.16, 3.5, 4.06, 4.6, 5.1, 5.4, 6.2, 6.8, 7.3, 8.0},
     {1, .8, .75, .6, .55, .5, .45, .4, .36, .3, .28, .24, .2, .18},
     14,
     .22},
    {{1, 2.32, 4.25, 6.63, 9.38, 12.6}, {1, .62, .42, .26, .16, .1}, 6, .3},
    {{1, 2.01, 3.03, 4.05, 5.08, 6.1, 7.15}, {1, .5, .36, .26, .2, .15, .1}, 7, .6},
    {{1, 1.37, 1.81, 2.23, 2.71, 3.3, 3.9, 4.6, 5.5, 6.4},
     {1, .9, .8, .75, .6, .55, .45, .4, .3, .25},
     10,
     3.5},
}};
static_assert(material_modes.size() == knock_table::materials.size());

// Tone 0 scatters the material's modes by up to this many octaves and adds
// this many more, so the hit reads as a knock on a thing, not a note.
constexpr double scatter_octaves = 0.45;
constexpr std::size_t max_extra_modes = 34;
constexpr double extra_top_stretch = 2.2; // Extras reach this past the top mode...
constexpr double extra_low = 0.55;        // ...and scale down to this.
constexpr double extra_amp_floor = 0.25;
constexpr std::size_t max_cloud_modes = max_material_modes + max_extra_modes;
constexpr double nyquist_fraction = 0.45; // Modes above this fraction of the rate are dropped.

// Seeds per pad and material only, so automating Tone or Pitch moves the
// same cloud rather than drawing a new one.
constexpr std::uint32_t resonator_seed = 4'421U;
constexpr std::uint32_t coupled_seed = 7'703U;
constexpr std::uint32_t material_seed = 104'729U;

// Mulberry32: a small seeded generator for the mode clouds.
struct Mulberry {
  std::uint32_t a = 0;
  [[nodiscard]] double next() noexcept {
    a += 0x6D2B79F5U;
    std::uint32_t t = a;
    t = (t ^ (t >> 15U)) * (t | 1U);
    t ^= t + (t ^ (t >> 7U)) * (t | 61U);
    return static_cast<double>(t ^ (t >> 14U)) / 4'294'967'296.0;
  }
  [[nodiscard]] double bipolar() noexcept { return next() * 2.0 - 1.0; }
};

struct Cloud {
  std::array<double, max_cloud_modes> ratio{};
  std::array<double, max_cloud_modes> amp{};
  std::size_t count = 0;
};

// The material's main modes, scattered by (1 - tone), plus round((1 - tone)
// * 34) scattered extras. A lower Tone uses a prefix of the same draws.
void mode_cloud(std::size_t material, double tone, std::uint32_t seed, Cloud& out) noexcept {
  const auto& m = material_modes[material];
  const double spread = 1.0 - tone;
  Mulberry rng{seed};
  out.count = 0;
  for (std::size_t k = 0; k < m.count; ++k) {
    double r = m.ratios[k];
    if (k > 0) {
      r *= std::exp2(rng.bipolar() * spread * scatter_octaves);
    }
    out.ratio[out.count] = r;
    out.amp[out.count] = m.amps[k];
    ++out.count;
  }
  const auto extra =
      static_cast<std::size_t>(std::floor(spread * static_cast<double>(max_extra_modes) + 0.5));
  const double top = std::log(m.ratios[m.count - 1] * extra_top_stretch);
  for (std::size_t e = 0; e < extra; ++e) {
    const double place = std::exp(rng.next() * top);
    const double scale = extra_low + (1.0 - extra_low) * rng.next();
    const double amp = (extra_amp_floor + (1.0 - extra_amp_floor) * rng.next()) * spread;
    out.ratio[out.count] = place * scale;
    out.amp[out.count] = amp;
    ++out.count;
  }
}

// Resonator decay: tau = decay (0.3 + 0.7 tone) / (1 + damp (0.3 + 1.4
// damping) max(0, r - 1) 0.5).
constexpr double tone_decay_floor = 0.3;
constexpr double damping_floor = 0.3;
constexpr double damping_span = 1.4;
constexpr double resonator_damp_rate = 0.5;
// Strike position: each mode's shape at the strike point.
constexpr double shape_floor = 0.25;
constexpr double shape_skew = 0.13;
// The coupled resonator.
constexpr double coupled_min_tau = 0.02;
constexpr double coupled_decay_stretch = 1.4;
constexpr double coupled_damp_rate = 0.4;
constexpr double coupled_drive = 1.5;
constexpr double coupled_display = 0.6;
// The body: six low modes of a box, and its air.
constexpr std::array<double, 6> body_ratios{1, 1.48, 2.1, 2.9, 3.7, 4.6};
constexpr std::size_t body_mode_count = body_ratios.size();
constexpr double body_base_hz = 60.0;
constexpr double body_size_hz = 260.0;
constexpr double body_drive = 1.2;
constexpr double body_display = 0.5;
constexpr double body_rolloff = 0.5;
constexpr double body_tau = 0.02;
constexpr double body_tau_span = 0.12;
constexpr double air_tau = 0.02;
constexpr double air_tau_span = 0.1;
constexpr double air_level = 0.12;
constexpr double air_lowpass = 0.08;
constexpr double air_lowpass_span = 0.4;
// Grit: a noise force that rubs the object rather than ringing it.
constexpr double brush_seconds = 0.035;
constexpr double min_grit_seconds = 0.002;
constexpr double grit_contacts = 4.0; // Grit lasts this many contact times.
constexpr double brush_grit = 1.2;
constexpr double grit_floor = 0.1;
constexpr double grit_tone = 0.9;
constexpr double grit_tau_fraction = 0.3;
// Grit is the surface's roughness felt through the contact, so it is no
// brighter than the contact: low-passed at this many hertz per second of
// contact (one over the contact time). A soft mallet's grit is a dull rub,
// a hard stick's a bright scratch, and a harder hit is brighter. A brush's
// bristles are the exception.
constexpr double grit_bandwidth_per_contact = 1.0;
// Click: high-passed contact noise straight to the ear.
constexpr double click_contacts = 1.6;
constexpr double click_pad_seconds = 0.0008;
constexpr double click_tau_fraction = 0.35;
constexpr double click_floor = 0.15;
constexpr double click_hardness = 0.6;
constexpr double click_scale = 0.6;
// Bend: a frequency factor max(0.05, 1 + bend 1.2 e^(-t / 45 ms)), updated on
// a grid of frames from the hit.
constexpr double bend_depth = 1.2;
constexpr double bend_seconds = 0.045;
constexpr double min_bend_factor = 0.05;
constexpr std::uint32_t bend_grid_frames = 16;
constexpr double bend_settled = 1e-4;
// Finish.
constexpr double snap_boost = 2.5;
constexpr double snap_fast_seconds = 0.005;
constexpr double snap_cut = 0.45;
constexpr double snap_slow_seconds = 0.02;
constexpr double gate_fade_seconds = 0.003;
constexpr double crush_hold_span = 14.0;
constexpr double crush_top_bits = 16.0;
constexpr double crush_bits_span = 11.0;
// Hand: per-hit jitter at full variation.
constexpr double hand_hardness = 0.15;
constexpr double hand_position = 0.08;
constexpr double hand_tune_cents = 14.0;
constexpr double hand_pan = 0.08;
constexpr double cents_per_octave = 1'200.0;
// Key: each mode's scale steps, in semitones above the tonic.
constexpr std::array major_steps{0, 2, 4, 5, 7, 9, 11};
constexpr std::array minor_steps{0, 2, 3, 5, 7, 8, 10};
// Every note lies within this many semitones of its key's scale.
constexpr int key_search_semitones = 2;
// Two candidates closer than this (in semitones) are a tie, which the lower
// wins: a midpoint pitch's log2 round trip must not tip it upward.
constexpr double key_tie_semitones = 1e-9;
constexpr std::uint32_t hand_pad_seed = 7'919U;
constexpr std::uint32_t hand_hit_seed = 104'729U;
// A pad's grit, click and air noise are the same every hit, as a sampled
// hit's would be: random noise of a few milliseconds varies by several dB
// from draw to draw, so a fresh draw per hit made "machine" (Hand 0) hits
// uneven and calibration miss. Hand varies the strike instead.
[[nodiscard]] constexpr std::uint32_t noise_seed(std::size_t pad) noexcept {
  return static_cast<std::uint32_t>(pad) * hand_pad_seed + 1U;
}
constexpr std::uint32_t grit_salt = 0x9E3779B9U;
constexpr std::uint32_t click_salt = 0x85EBCA6BU;
constexpr std::uint32_t air_salt = 0xC2B2AE35U;
constexpr std::uint32_t hit_count_wrap = 1U << 24U;
// Levels: a hit at level 0 dB and the reference velocity, without the
// hand, reaches this short-term level. Each pad is calibrated by rendering
// that hit once, with the kernel's own voice, whenever its sound changes.
// A short RMS rather than the sample peak: a click's or grit's peak is one
// random sample, so pads matched by it differed by several dB in loudness.
constexpr double target_level = 0.2; // About -14 dB short-term RMS.
// ...unless that would take its sample peak over this: a click is one or
// two samples, so it is heard quieter than its RMS says and is held here.
constexpr double peak_ceiling = 0.5; // -6 dBFS.
constexpr double calibration_rms_seconds = 0.002;
constexpr double calibration_seconds = 0.08;
constexpr double calibration_margin = 0.3; // Stop once all that rings is well below the peak...
// ...or this long after the excitation ends: the loudest short-term level
// comes during the strike or as the coupled resonator and body answer it,
// not in the ring after. It keeps a calibration to a few milliseconds.
constexpr double calibration_ring_seconds = 0.003;
// The probe's contact sub-steps: fewer than the voices' (a level, not a
// sound, is measured), so a calibration costs a fraction of a hit.
constexpr int probe_substeps = 2;
// A pad is calibrated ahead of its hit once its sound has held this many
// control ticks (about 5 ms at 48 kHz).
constexpr std::uint32_t settle_ticks = 8;
// The click and the air scale with the resonator's own peak, estimated as
// random-phase partials.
constexpr double estimate_crest = 1.8;
constexpr double brush_grit_power = 1.0; // Sine-windowed noise's power per unit amount...
constexpr double grit_power = 1.0 / 6.0; // ...and the decaying burst's.
constexpr double silent_db = -60.0;
constexpr double retire_level = 1e-5; // -100 dB.
constexpr double air_floor = 1e-7;
// A hit's whole excitation ends within this: the longest contact and grit.
constexpr double excitation_seconds = modal::max_contact_seconds + brush_seconds;
constexpr double chain_margin = 1.5;            // Driven rings outlast their drive.
constexpr double ln100000 = 11.512925464970229; // -100 dB in nepers.
constexpr double meter_release_seconds = 0.3;
constexpr double meter_floor_db = -120.0;

// ---- Width and Swing -----------------------------------------------------------

// Spread: each mode is placed at a seeded position times Spread, seeded by
// pad and mode index only, so the image
// stays put while other controls move. Modes below this stay centred, so
// the low end is mono-safe.
constexpr double mono_below_hz = 150.0;
constexpr std::uint32_t width_seed = 9'001U;
constexpr std::uint32_t width_mode_seed = 2'654'435'761U;
// Mode indices for the seed: the coupled cloud's and the body's follow the
// resonator's.
constexpr std::size_t coupled_place_index = max_cloud_modes;
constexpr std::size_t body_place_index = 2 * max_cloud_modes;
// Scatter: a seeded draw per pad and hit since the last reset.
constexpr std::uint32_t scatter_seed = 7'717U;
constexpr std::uint32_t scatter_hit_seed = 31U;
constexpr double ms_per_second = 1'000.0;
[[nodiscard]] std::size_t max_haas_frames_at(double rate) noexcept {
  return static_cast<std::size_t>(std::ceil(knock_table::max_haas_ms / ms_per_second * rate));
}
// Swing: 0.5 delays an off-beat sixteenth to the triplet, 1/12 beat late.
constexpr double triplet_swing = 0.5;
constexpr double triplet_delay_beats = 1.0 / 12.0;
constexpr double sixteenths_per_beat = 4.0;
// A note this close to an off-beat sixteenth is on it.
constexpr double swing_tolerance_beats = 1.0 / 48.0;
// The beat at a hit, rounded to this fraction of a beat (under a frame
// at any tempo), so the transport's rounding under different block splits
// never moves an Orbit.
constexpr double hit_beat_resolution = 65'536.0;
// Swung hits waiting to play. Past this many, a hit plays on time.
constexpr std::size_t max_pending_hits = 64;
// The tail bound assumes no slower tempo than this for a swung last hit.
constexpr double slowest_swing_beats_per_second = 0.5; // 30 BPM.

// ---- The room: a small shared FDN --------------------------------------------

constexpr std::size_t room_lines = 8;
constexpr std::array<double, room_lines> room_ms{19.3, 23.9, 29.1, 31.7, 37.3, 41.9, 46.1, 53.3};
constexpr double room_min_t60 = 0.2;
constexpr double room_t60_span = 2.6;
constexpr double room_damp = 0.2;
constexpr double room_damp_span = 0.4;
constexpr double room_input = 0.35;
constexpr double room_output = 0.5;
constexpr double room_send = 0.6; // What each pad feeds the room.

[[nodiscard]] double room_t60(double size) noexcept {
  return room_min_t60 + room_t60_span * clamp01(size);
}

class Room {
public:
  void prepare(double rate) {
    rate_ = rate;
    for (std::size_t line = 0; line < room_lines; ++line) {
      length_[line] = std::max<std::size_t>(
          1, static_cast<std::size_t>(std::lround(room_ms[line] * 1e-3 * rate)));
      delay_[line].assign(length_[line] + 1, 0.0);
    }
    size_ = -1.0;
    set(0.5);
    reset();
  }
  void set(double size) noexcept {
    if (size == size_) {
      return;
    }
    size_ = size;
    const double t60 = room_t60(size);
    for (std::size_t line = 0; line < room_lines; ++line) {
      feedback_[line] = std::pow(10.0, -3.0 * static_cast<double>(length_[line]) / (t60 * rate_));
    }
    damp_ = room_damp + room_damp_span * (1.0 - clamp01(size));
  }
  void reset() noexcept {
    for (auto& line : delay_) {
      std::fill(line.begin(), line.end(), 0.0);
    }
    write_.fill(0);
    state_.fill(0.0);
  }
  void process(double in_left, double in_right, double& out_left, double& out_right) noexcept {
    std::array<double, room_lines> out{};
    double sum = 0.0;
    for (std::size_t line = 0; line < room_lines; ++line) {
      const auto& buffer = delay_[line];
      std::size_t read = write_[line] + 1;
      if (read >= buffer.size()) {
        read -= buffer.size();
      }
      state_[line] += (1.0 - damp_) * (buffer[read] - state_[line]);
      out[line] = state_[line];
      sum += out[line];
    }
    const double householder = 2.0 / static_cast<double>(room_lines) * sum;
    double left = 0.0;
    double right = 0.0;
    for (std::size_t line = 0; line < room_lines; ++line) {
      auto& buffer = delay_[line];
      const double input = line % 2 == 0 ? in_left : in_right;
      buffer[write_[line]] = feedback_[line] * (out[line] - householder) + room_input * input;
      write_[line] = write_[line] + 1 == buffer.size() ? 0 : write_[line] + 1;
      (line % 2 == 0 ? left : right) += out[line];
    }
    out_left = room_output * left;
    out_right = room_output * right;
  }

private:
  double rate_ = 48'000.0;
  double size_ = -1.0;
  std::array<std::vector<double>, room_lines> delay_{};
  std::array<std::size_t, room_lines> length_{};
  std::array<std::size_t, room_lines> write_{};
  std::array<double, room_lines> state_{};
  std::array<double, room_lines> feedback_{};
  double damp_ = 0.5;
};

// ---- Hit design: one source of truth for the kernel and the panel ---------------

[[nodiscard]] std::size_t choice(double value, std::size_t count) noexcept {
  const auto index = static_cast<std::size_t>(std::max(0.0, std::round(value)));
  return std::min(index, count - 1);
}

[[nodiscard]] double db_gain(double db) noexcept {
  return db <= silent_db ? 0.0 : oscillate::native_dsp::dsp::db_to_gain(db);
}

// The spectrum of a half-sine contact of `seconds` at `hz`, 1 at DC.
[[nodiscard]] double pulse_gain(double hz, double seconds) noexcept {
  constexpr double near_null = 1e-3;
  const double x = 2.0 * hz * seconds;
  const double d = 1.0 - x * x;
  return std::fabs(d) < near_null ? pi / 4.0 : std::fabs(std::cos(pi * x / 2.0) / d);
}

// Lift's two one-pole high-passes, y = a (y1 + x - x1), at hz.
[[nodiscard]] double lift_coefficient(double lift_hz, double rate) noexcept {
  return std::exp(-two_pi * lift_hz / rate);
}

[[nodiscard]] std::uint32_t cloud_seed(std::uint32_t base, std::size_t pad,
                                       std::size_t material) noexcept {
  return base * static_cast<std::uint32_t>(pad + 1) +
         material_seed * static_cast<std::uint32_t>(material);
}

// Per-hit jitter, -1..1 times the pad's Hand.
struct Hand {
  double hardness = 0.0;
  double position = 0.0;
  double tune = 0.0;
  double pan = 0.0;
};

// place: the mode's seeded position in the field, -1..1, before Spread; 0
// below mono_below_hz.
struct ResonatorMode {
  double hz;
  double t60;
  double shape;
  double amp;
  double display;
  double place;
};
struct DrivenMode {
  double hz;
  double t60;
  double drive;
  double display;
  double place;
};

[[nodiscard]] double mode_place(std::size_t pad, std::size_t index, double hz) noexcept {
  if (hz < mono_below_hz) {
    return 0.0;
  }
  Mulberry rng{width_seed * static_cast<std::uint32_t>(pad + 1) +
               width_mode_seed * static_cast<std::uint32_t>(index + 1)};
  return rng.bipolar();
}

struct HitDesign {
  std::array<ResonatorMode, max_cloud_modes> res{};
  std::size_t res_count = 0;
  std::array<DrivenMode, max_cloud_modes> coupled{};
  std::size_t coupled_count = 0;
  std::array<DrivenMode, body_mode_count> body{};
  std::size_t body_count = 0;
  Cloud cloud{}; // Scratch.
  StrikerType type = StrikerType::stick;
  double hardness = 0.5;    // After hand and velocity.
  double contact = 0.001;   // Target contact time (s).
  double speed = 1.0;       // Impact speed (m/s).
  double speed_ratio = 1.0; // To the reference speed.
  bool brush = false;
  double grit = 0.0; // Grit amount (units of the contact's momentum).
  std::uint32_t grit_frames = 0;
  std::uint32_t click_frames = 0;
  double click = 0.0;   // Click amplitude, before the hit's gain.
  double air = 0.0;     // Air amplitude, before the hit's gain.
  double air_tau = 0.0; // Seconds.
  double air_lowpass = 0.0;
  double bend = 0.0;
  double snap = 0.0;
  bool gated = false;
  std::uint32_t gate_frames = 0;
  double crush = 0.0;
  double pan = 0.0; // The hit's pan, hand included.
};

void design_hit(std::span<const double> values, std::size_t pad, double rate, const Hand& hand,
                double velocity, HitDesign& d) noexcept {
  const double* v = values.data() + pad * pad_parameters;
  const double sensitivity = values[gl::velocity];
  const double decay_scale = values[gl::decay] / knock_table::decay_unity;
  d.type = static_cast<StrikerType>(choice(v[o::type], striker_type_count));
  d.brush = d.type == StrikerType::brush;
  const double nominal_hardness = clamp01(v[o::hardness] + hand.hardness * hand_hardness);
  d.hardness = velocity_hardness(nominal_hardness, velocity, sensitivity);
  const double position = std::clamp(v[o::position] + hand.position * hand_position, 0.02, 0.5);
  const double tune = std::exp2(hand.tune * hand_tune_cents / cents_per_octave);
  d.contact = contact_target_seconds(d.type, d.hardness);
  const double nominal_contact = contact_target_seconds(d.type, nominal_hardness);
  d.speed = strike_speed(velocity, sensitivity);
  d.speed_ratio = d.speed / strike_speed(reference_velocity_unit, sensitivity);

  const double pitch = v[o::pitch];
  const double tone = clamp01(v[o::tone]);
  const double decay = v[o::decay];
  const double nyquist = nyquist_fraction * rate;
  const double tone_decay = tone_decay_floor + (1.0 - tone_decay_floor) * tone;

  // The resonator: the struck object.
  const std::size_t material = choice(v[o::material], material_modes.size());
  const auto& a = material_modes[material];
  const double damp_scale = damping_floor + damping_span * clamp01(v[o::damping]);
  mode_cloud(material, tone, cloud_seed(resonator_seed, pad, material), d.cloud);
  d.res_count = 0;
  for (std::size_t k = 0; k < d.cloud.count; ++k) {
    const double r = d.cloud.ratio[k];
    const double hz = pitch * tune * r;
    if (hz >= nyquist) {
      continue;
    }
    const double shape =
        shape_floor +
        (1.0 - shape_floor) * std::fabs(std::sin(static_cast<double>(k + 1) * pi * position *
                                                 (1.0 + shape_skew * static_cast<double>(k % 3))));
    const double tau = decay * tone_decay /
                       (1.0 + a.damp * damp_scale * std::max(0.0, r - 1.0) * resonator_damp_rate);
    d.res[d.res_count++] = {hz,
                            tau * decay_scale * modal::ln1000,
                            shape,
                            d.cloud.amp[k],
                            d.cloud.amp[k] * shape,
                            mode_place(pad, k, hz)};
  }

  // The coupled resonator, rung by the first.
  const double amount = clamp01(v[o::amount]);
  d.coupled_count = 0;
  if (amount > 0.0) {
    const std::size_t coupled = choice(v[o::couple_material], material_modes.size());
    const auto& b = material_modes[coupled];
    mode_cloud(coupled, tone, cloud_seed(coupled_seed, pad, coupled), d.cloud);
    for (std::size_t k = 0; k < d.cloud.count; ++k) {
      const double r = d.cloud.ratio[k];
      const double hz = pitch * tune * v[o::tuning] * r;
      if (hz >= nyquist) {
        continue;
      }
      const double tau = std::max(coupled_min_tau, decay * coupled_decay_stretch) * tone_decay /
                         (1.0 + b.damp * std::max(0.0, r - 1.0) * coupled_damp_rate);
      d.coupled[d.coupled_count++] = {
          hz, tau * decay_scale * modal::ln1000, amount * d.cloud.amp[k] * coupled_drive,
          amount * coupled_display * d.cloud.amp[k], mode_place(pad, coupled_place_index + k, hz)};
    }
  }

  // The body: a box's low modes, rung by both.
  const double hollow = clamp01(v[o::hollow]);
  const double size = clamp01(v[o::size]);
  d.body_count = 0;
  if (hollow > 0.0) {
    const double base = (body_base_hz + (1.0 - size) * body_size_hz) * tune;
    const double tau = (body_tau + body_tau_span * hollow * size) * decay_scale;
    for (std::size_t k = 0; k < body_mode_count; ++k) {
      const double hz = base * body_ratios[k];
      if (hz >= nyquist) {
        continue;
      }
      const double rolloff = 1.0 + body_rolloff * static_cast<double>(k);
      d.body[d.body_count++] = {hz, tau * modal::ln1000, hollow * body_drive / rolloff,
                                hollow * body_display / rolloff,
                                mode_place(pad, body_place_index + k, hz)};
    }
  }

  // The exciter's grit and click.
  d.grit = (d.brush ? brush_grit : grit_floor) + grit_tone * (1.0 - tone);
  d.grit_frames = static_cast<std::uint32_t>(std::lround(
      rate * (d.brush ? brush_seconds : std::max(min_grit_seconds, d.contact * grit_contacts))));
  d.click_frames = static_cast<std::uint32_t>(std::floor(
      rate * (d.brush ? brush_seconds : d.contact * click_contacts + click_pad_seconds)));

  // The resonator's peak before the finish, for the click's and the air's
  // levels. The contact meets each mode where Bend has it at the start.
  d.bend = std::clamp(v[o::bend], -1.0, 1.0);
  const double onset = std::max(min_bend_factor, 1.0 + d.bend * bend_depth);
  const double grit_share = (d.brush ? brush_grit_power : grit_power) * d.grit * d.grit;
  double sum = 0.0;
  double power = 0.0;
  for (std::size_t k = 0; k < d.res_count; ++k) {
    const auto& m = d.res[k];
    const double pulse = pulse_gain(m.hz * onset, nominal_contact);
    const double level = m.display * std::sqrt(pulse * pulse + grit_share);
    sum += level;
    power += level * level;
  }
  const double resonator_peak = std::min(sum, estimate_crest * std::sqrt(power)) * d.speed_ratio;
  d.click = clamp01(v[o::click]) * (click_floor + click_hardness * d.hardness) * click_scale *
            resonator_peak;
  d.snap = clamp01(v[o::snap]);
  d.pan = std::clamp(v[o::pan] + hand.pan * hand_pan, -1.0, 1.0);
  d.air = hollow * air_level * resonator_peak;
  d.air_tau = (air_tau + air_tau_span * hollow * size) * decay_scale;
  d.air_lowpass = air_lowpass + air_lowpass_span * (1.0 - size);

  const double length = v[o::length];
  d.gated = length < knock_table::length_off;
  d.gate_frames =
      d.gated ? static_cast<std::uint32_t>(std::lround(length * std::min(1.0, decay_scale) * rate))
              : 0U;
  d.crush = clamp01(v[o::crush]);
}

// ---- The kernel -----------------------------------------------------------------

struct Voice {
  bool active = false;
  std::size_t pad = 0;
  std::uint32_t frame = 0; // Since the hit.
  double loudness = 0.0;   // At the last control tick, for choosing which voice a hit takes.
  modal::ModalBank resonator{};
  modal::ModalBank coupled{};
  modal::ModalBank body{};
  modal::Striker striker{};
  double gain = 1.0;
  double pan_jitter = 0.0;
  // Width: each bank mode's place before Spread, the Spread last applied
  // (negative: none yet), Motion's pan offset for this hit, and Haas.
  std::array<double, max_cloud_modes> res_place{};
  std::array<double, max_cloud_modes> coupled_place{};
  std::array<double, body_mode_count> body_place{};
  double spread = -1.0;
  bool wide = false;
  double motion = 0.0;
  bool haas_left = false; // The far ear: away from where the hit lands.
  std::size_t haas_frames = 0;
  std::vector<double> haas_line; // max_haas_frames_at + 1, sized with the kernel.
  std::size_t haas_write = 0;
  // Grit.
  Mulberry grit_noise{};
  std::uint32_t grit_frames = 0;
  double grit_force = 0.0;
  double grit_envelope = 1.0;
  double grit_decay = 1.0;
  double grit_smoothing = 0.0; // One-pole low-pass coefficient...
  double grit_makeup = 1.0;    // ...and the gain that keeps its power.
  double grit_state = 0.0;
  bool brush = false;
  // Click.
  Mulberry click_noise{};
  std::uint32_t click_frames = 0;
  double click = 0.0;
  double click_envelope = 1.0;
  double click_decay = 1.0;
  double click_last = 0.0;
  // Air.
  Mulberry air_noise{};
  bool air_on = false;
  double air = 0.0;
  double air_decay = 1.0;
  double air_lowpass = 0.0;
  double air_left = 0.0;
  double air_right = 0.0;
  // Bend.
  bool bending = false;
  double bend = 0.0;
  // Finish.
  std::array<double, 4> lift_left{}; // x1, y1, x2, y2.
  std::array<double, 4> lift_right{};
  double snap = 0.0;
  double snap_fast = 1.0;
  double snap_slow = 1.0;
  bool gated = false;
  std::uint32_t gate_frames = 0;
  double crush = 0.0;
  std::uint32_t crush_hold = 1;
  double crush_steps = 1.0;
  double held_left = 0.0;
  double held_right = 0.0;
  // Live.
  Ramp level{};
  Ramp left{};
  Ramp right{};
  Ramp room{};
  Ramp lift{};
  double last_left = 0.0;
  double last_right = 0.0;
};

[[nodiscard]] double lift_step(std::array<double, 4>& s, double x, double a) noexcept {
  const double h1 = a * (s[1] + x - s[0]);
  s[0] = x;
  s[1] = h1;
  const double h2 = a * (s[3] + h1 - s[2]);
  s[2] = h1;
  s[3] = h2;
  return h2;
}

class KnockKernel final : public SynthKernel {
public:
  explicit KnockKernel(double sample_rate) : rate_(sample_rate) {
    voices_ = std::make_unique<std::array<Voice, pad_count * voices_per_pad>>();
    const std::size_t haas_line = max_haas_frames_at(rate_) + 1;
    for (auto& voice : *voices_) {
      voice.resonator.prepare(max_cloud_modes, rate_);
      voice.coupled.prepare(max_cloud_modes, rate_);
      voice.body.prepare(body_mode_count, rate_);
      voice.haas_line.assign(haas_line, 0.0);
    }
    design_ = std::make_unique<HitDesign>();
    // The probe measures the source mono and without Haas: no line.
    probe_ = std::make_unique<Voice>();
    probe_->resonator.prepare(max_cloud_modes, rate_, probe_substeps);
    probe_->coupled.prepare(max_cloud_modes, rate_, probe_substeps);
    probe_->body.prepare(body_mode_count, rate_, probe_substeps);
    room_.prepare(rate_);
    snap_fast_decay_ = std::exp(-1.0 / (snap_fast_seconds * rate_));
    snap_slow_decay_ = std::exp(-1.0 / (snap_slow_seconds * rate_));
    fade_frames_ = static_cast<std::uint32_t>(std::lround(gate_fade_seconds * rate_));
    meter_release_ = std::exp(-1.0 / (meter_release_seconds * rate_));
    reset_voices();
  }

  void project_key(std::optional<project::MusicalKey> key) noexcept override { key_ = key; }

  void control(std::span<const double> values, std::uint32_t frames_to_next_tick,
               const SynthTransport& transport) noexcept override {
    values_ = keyed(values);
    frames_ = frames_to_next_tick;
    transport_ = transport;
    frames_since_tick_ = 0;
    for (auto& voice : *voices_) {
      if (!voice.active) {
        continue;
      }
      aim(voice, !primed_);
      retire(voice);
    }
    // Calibrate ahead of the hits: one settled pad whose sound changed per
    // tick, so a kit change costs a few milliseconds spread over its first
    // ticks instead of all at its first beat. A pad whose knob is moving is
    // left to its next hit, which calibrates it once: recalibrating on every
    // tick of a turn cost more than a core. The result depends on the values
    // alone, so when it is done never changes what plays.
    for (std::size_t pad = 0; pad < pad_count; ++pad) {
      const double* v = values.data() + pad * pad_parameters;
      bool same = true;
      for (std::size_t i = 0; same && i < pad_parameters; ++i) {
        same = !calibrated_control(i) || last_key_[pad][i] == v[i];
      }
      std::copy_n(v, pad_parameters, last_key_[pad].begin());
      settled_ticks_[pad] =
          same ? settled_ticks_[pad] + (settled_ticks_[pad] < settle_ticks ? 1U : 0U) : 0U;
    }
    for (std::size_t i = 0; i < pad_count; ++i) {
      const std::size_t pad = (next_calibration_ + i) % pad_count;
      if (settled_ticks_[pad] >= settle_ticks && !calibration_current(pad)) {
        (void)calibrated_scale(pad);
        next_calibration_ = (pad + 1) % pad_count;
        break;
      }
    }
    room_.set(values[gl::room_size]);
    room_gain_.to(db_gain(values[gl::room_level]), frames_, !primed_);
    out_gain_.to(db_gain(values[gl::out_level]), frames_, !primed_);
    primed_ = true;
  }

  void note_on(const NoteEvent& event) noexcept override {
    if (values_.empty() || event.key < first_key || event.key >= first_key + pad_count) {
      return;
    }
    const auto pad = static_cast<std::size_t>(event.key - first_key);
    const double velocity = event.velocity / 127.0;
    const double beat = beat_now();
    // Swing: an off-beat sixteenth waits, then plays from render().
    if (swung(pad, beat)) {
      const double delay = swing_delay_beats(values_[gl::swing]);
      const auto frames = static_cast<std::uint64_t>(std::llround(delay / beats_per_frame()));
      if (frames > 0 && pending_count_ < max_pending_hits) {
        enqueue({clock_ + frames, pad, velocity, beat + delay});
        return;
      }
    }
    hit(pad, velocity, beat);
  }

  // Percussion: note-offs are ignored.
  void note_off(const NoteEvent&) noexcept override {}

  // Split at each swung hit due inside the span, so it lands on its frame
  // however the host splits its blocks.
  void render(std::span<engine::StereoFrame> output) noexcept override {
    std::size_t done = 0;
    while (done < output.size()) {
      while (pending_count_ > 0 && pending_[0].frame <= clock_) {
        const PendingHit due = pending_[0];
        std::copy(pending_.begin() + 1,
                  pending_.begin() + static_cast<std::ptrdiff_t>(pending_count_), pending_.begin());
        --pending_count_;
        hit(due.pad, due.velocity, due.beat);
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
    // The room's last output fades with the voices'.
    capture_declick(last_wet_left_, last_wet_right_);
    for (const auto& voice : *voices_) {
      if (voice.active) {
        capture_declick(voice.last_left, voice.last_right);
      }
    }
    last_wet_left_ = 0.0;
    last_wet_right_ = 0.0;
    room_.reset();
    reset_voices();
    // A seek or loop drops the swung hits still waiting.
    pending_count_ = 0;
  }

  [[nodiscard]] std::size_t telemetry(std::span<float> out) const noexcept override {
    if (out.size() < telemetry::size) {
      return 0;
    }
    out[telemetry::last_pad] = static_cast<float>(last_pad_);
    out[telemetry::hits] = static_cast<float>(hits_);
    for (std::size_t pad = 0; pad < pad_count; ++pad) {
      out[telemetry::first_level + pad] = static_cast<float>(std::min(1.0, pad_peak_[pad]));
      out[telemetry::first_place + pad] = static_cast<float>(place_[pad]);
    }
    const double peak_db = output_peak_ > 0.0 ? 20.0 * std::log10(output_peak_) : meter_floor_db;
    out[telemetry::output_peak_db] = static_cast<float>(std::max(meter_floor_db, peak_db));
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

  struct PendingHit {
    std::uint64_t frame = 0; // On the kernel's clock.
    std::size_t pad = 0;
    double velocity = 0.0;
    double beat = 0.0; // Where it lands, for Orbit.
  };

  // Quarter notes per frame: the transport's tempo, or 120 BPM when none is
  // known (as SynthTransport defines).
  [[nodiscard]] double beats_per_frame() const noexcept {
    return sync_beats_per_second(transport_, rate_) / rate_;
  }

  // The song position at the frame about to render.
  [[nodiscard]] double beat_now() const noexcept {
    const double beat = transport_.beat_at_block_start +
                        static_cast<double>(frames_since_tick_) * beats_per_frame();
    return std::round(beat * hit_beat_resolution) / hit_beat_resolution;
  }

  // Does this pad's note at `beat` swing: Swing on, the pad following it,
  // and the note on an off-beat sixteenth?
  [[nodiscard]] bool swung(std::size_t pad, double beat) const noexcept {
    if (values_[gl::swing] <= 0.0 ||
        choice(value(pad, o::swing_follow), knock_table::swing_follows.size()) !=
            static_cast<std::size_t>(knock_table::swing_on)) {
      return false;
    }
    const double sixteenths = beat * sixteenths_per_beat;
    const double nearest = std::round(sixteenths);
    if (std::fabs(sixteenths - nearest) > swing_tolerance_beats * sixteenths_per_beat) {
      return false;
    }
    return (static_cast<std::int64_t>(nearest) & 1) != 0;
  }

  // Keeps the queue in release order; hits due on the same frame keep the
  // order they came in.
  void enqueue(const PendingHit& pending) noexcept {
    std::size_t at = pending_count_;
    while (at > 0 && pending_[at - 1].frame > pending.frame) {
      pending_[at] = pending_[at - 1];
      --at;
    }
    pending_[at] = pending;
    ++pending_count_;
  }

  // Motion's pan offset for this pad's next hit.
  [[nodiscard]] double motion_offset(std::size_t pad, double beat) const noexcept {
    const double depth = clamp01(value(pad, o::depth));
    const std::uint32_t n = pad_hits_[pad];
    switch (static_cast<knock_table::Motion>(
        choice(value(pad, o::motion), knock_table::motions.size()))) {
    case knock_table::ping_pong:
      return n % 2U == 1U ? depth : -depth;
    case knock_table::scatter: {
      Mulberry rng{scatter_seed * static_cast<std::uint32_t>(pad + 1) + scatter_hit_seed * n};
      return rng.bipolar() * depth;
    }
    case knock_table::orbit: {
      const double bar = transport_.beats_per_bar > 0.0 ? transport_.beats_per_bar
                                                        : SynthTransport{}.beats_per_bar;
      const double bars =
          knock_table::orbit_bars[choice(values_[gl::orbit], knock_table::orbit_bars.size())];
      return depth * std::sin(two_pi * beat / (bars * bar));
    }
    case knock_table::fixed:
      break;
    }
    return 0.0;
  }

  // A hit of the pad now, landing at `beat` in the song.
  void hit(std::size_t pad, double velocity, double beat) noexcept {
    // The quieter of the pad's voices takes the hit.
    Voice* chosen = nullptr;
    for (std::size_t i = 0; i < voices_per_pad; ++i) {
      Voice& voice = (*voices_)[pad * voices_per_pad + i];
      if (!voice.active) {
        chosen = &voice;
        break;
      }
      if (chosen == nullptr || voice.loudness < chosen->loudness) {
        chosen = &voice;
      }
    }
    if (chosen->active) {
      capture_declick(chosen->last_left, chosen->last_right);
    }
    // Calibrate first: it uses the shared design scratch.
    const double scale = calibrated_scale(pad);
    trigger(*chosen, pad, velocity, scale, beat);
    last_pad_ = static_cast<int>(pad);
    hits_ = (hits_ + 1U) % hit_count_wrap;
  }

  // Renders a span that holds no hit: at most one control interval.
  void render_span(std::span<engine::StereoFrame> output) noexcept {
    const std::size_t frames = std::min<std::size_t>(output.size(), control_interval_frames);
    Chunk left{};
    Chunk right{};
    Chunk send_left{};
    Chunk send_right{};
    std::array<double, pad_count> chunk_peak{};
    for (auto& voice : *voices_) {
      if (voice.active) {
        render_voice(voice, left, right, send_left, send_right, frames, chunk_peak[voice.pad]);
      }
    }
    const double release = std::pow(meter_release_, static_cast<double>(frames));
    for (std::size_t pad = 0; pad < pad_count; ++pad) {
      pad_peak_[pad] = std::max(chunk_peak[pad], pad_peak_[pad] * release);
    }
    for (std::size_t i = 0; i < frames; ++i) {
      double wet_left = 0.0;
      double wet_right = 0.0;
      room_.process(room_send * send_left[i], room_send * send_right[i], wet_left, wet_right);
      const double room = room_gain_.next();
      double l = left[i] + room * wet_left;
      double r = right[i] + room * wet_right;
      last_wet_left_ = room * wet_left;
      last_wet_right_ = room * wet_right;
      declick_.add(l, r);
      const double gain = out_gain_.next();
      l = safety(l * gain);
      r = safety(r * gain);
      output_peak_ = std::max({std::fabs(l), std::fabs(r), output_peak_ * meter_release_});
      output[i].left += static_cast<float>(l);
      output[i].right += static_cast<float>(r);
    }
  }

  [[nodiscard]] double value(std::size_t pad, std::size_t parameter) const noexcept {
    return values_[pad * pad_parameters + parameter];
  }

  void reset_voices() noexcept {
    for (auto& voice : *voices_) {
      voice.active = false;
      voice.last_left = 0.0;
      voice.last_right = 0.0;
      voice.resonator.clear();
      voice.coupled.clear();
      voice.body.clear();
    }
    pad_hits_.fill(0);
    hits_ = 0;
  }

  // Places each bank mode at its seeded position times Spread, at equal
  // power and unity at the centre (sqrt 2 times cos and sin), so a spread
  // pad keeps its loudness.
  static void place_bank(modal::ModalBank& bank, const double* places, double spread) noexcept {
    for (std::size_t k = 0; k < bank.size(); ++k) {
      const double theta = (places[k] * spread + 1.0) * pi / 4.0;
      bank.place(k, std::numbers::sqrt2 * std::cos(theta), std::numbers::sqrt2 * std::sin(theta));
    }
  }

  void spread_modes(Voice& voice, double spread) noexcept {
    voice.spread = spread;
    voice.wide = spread > 0.0;
    if (!voice.wide) {
      return;
    }
    place_bank(voice.resonator, voice.res_place.data(), spread);
    place_bank(voice.coupled, voice.coupled_place.data(), spread);
    place_bank(voice.body, voice.body_place.data(), spread);
  }

  // Folds a last output into the fade that is already running.
  void capture_declick(double left, double right) noexcept { declick_.capture(left, right); }

  // Level, pan, room, lift and Spread follow the panel on ringing voices.
  void aim(Voice& voice, bool jump) noexcept {
    const std::size_t pad = voice.pad;
    const double pan = std::clamp(value(pad, o::pan) + voice.pan_jitter + voice.motion, -1.0, 1.0);
    const double spread = clamp01(value(pad, o::spread));
    if (spread != voice.spread) {
      spread_modes(voice, spread);
    }
    const double theta = (pan + 1.0) * pi / 4.0;
    voice.level.to(db_gain(value(pad, o::level)), frames_, jump);
    voice.left.to(std::cos(theta), frames_, jump);
    voice.right.to(std::sin(theta), frames_, jump);
    voice.room.to(value(pad, o::room), frames_, jump);
    voice.lift.to(lift_coefficient(value(pad, o::lift), rate_), frames_, jump);
  }

  [[nodiscard]] static bool exciting(const Voice& voice) noexcept {
    return voice.striker.active || voice.grit_frames > 0 || voice.click_frames > 0;
  }
  // A bound on what the voice can still put out, before its level and pan.
  [[nodiscard]] static double ringing(const Voice& voice) noexcept {
    const double ring =
        voice.resonator.amplitude() + voice.coupled.amplitude() + voice.body.amplitude();
    return (ring + (voice.air_on ? voice.air : 0.0)) * voice.gain;
  }

  void retire(Voice& voice) noexcept {
    if (exciting(voice)) {
      return;
    }
    const double loudness = ringing(voice) * voice.level.value;
    voice.loudness = loudness;
    if (loudness < retire_level) {
      voice.active = false;
      voice.last_left = 0.0;
      voice.last_right = 0.0;
    }
  }

  // The pad's source sound, which calibration measures: everything but its
  // level, pan, room and hand, and Lift and Length, which act as a filter
  // and a gate on the calibrated hit (turning them changes the level, as a
  // filter or gate would). The global Decay is left out too, and so is
  // Width (Spread, Motion, Depth, Haas) and Swing: the probe hears the pad
  // mono and without Haas, and Spread places each mode at equal power, so
  // width moves the sound without changing its level. So turning the knobs
  // most often moved while a loop plays never recalibrates.
  [[nodiscard]] static bool calibrated_control(std::size_t control) noexcept {
    return control != o::level && control != o::pan && control != o::room && control != o::hand &&
           control != o::lift && control != o::length && control != o::spread &&
           control != o::motion && control != o::depth && control != o::haas &&
           control != o::swing_follow && control != o::key_follow;
  }

  // The values with each following pad's Pitch moved into the key, or the
  // values themselves when no pad moves. Calibration and every hit read the
  // result, so a key change recalibrates exactly as turning Pitch would.
  [[nodiscard]] std::span<const double> keyed(std::span<const double> values) noexcept {
    if (!key_ || values.size() != keyed_values_.size()) {
      return values;
    }
    bool any = false;
    for (std::size_t pad = 0; pad < pad_count; ++pad) {
      any = any || follows_key(values, pad);
    }
    if (!any) {
      return values;
    }
    std::ranges::copy(values, keyed_values_.begin());
    for (std::size_t pad = 0; pad < pad_count; ++pad) {
      if (follows_key(values, pad)) {
        auto& pitch = keyed_values_[pad * pad_parameters + o::pitch];
        pitch = in_key(pitch, key_);
      }
    }
    return keyed_values_;
  }

  [[nodiscard]] static bool follows_key(std::span<const double> values, std::size_t pad) noexcept {
    return choice(values[pad * pad_parameters + o::key_follow], knock_table::key_follows.size()) ==
           static_cast<std::size_t>(knock_table::key_on);
  }

  // The gain that brings the pad's hit at level 0 dB and the reference
  // velocity, without the hand, to target_level short-term RMS (the loudest
  // calibration_rms_seconds), or to peak_ceiling if its peak gets there
  // first. Rendered once by the kernel's own voice whenever the pad's sound
  // changes; a function of the values alone, so export equals playback.
  [[nodiscard]] bool calibration_current(std::size_t pad) const noexcept {
    const auto& c = calibration_[pad];
    const double* v = values_.data() + pad * pad_parameters;
    bool same = c.valid && c.velocity == values_[gl::velocity];
    for (std::size_t i = 0; same && i < pad_parameters; ++i) {
      same = !calibrated_control(i) || c.key[i] == v[i];
    }
    return same;
  }

  [[nodiscard]] double calibrated_scale(std::size_t pad) noexcept {
    auto& c = calibration_[pad];
    if (calibration_current(pad)) {
      return c.scale;
    }
    const double* v = values_.data() + pad * pad_parameters;
    std::copy_n(v, pad_parameters, c.key.begin());
    c.velocity = values_[gl::velocity];
    c.valid = true;

    // The probe hears the source alone: no Lift, no gate, Decay at unity,
    // mono and without Haas, so what calibration measures is exactly what
    // its key holds.
    std::copy(values_.begin(), values_.end(), probe_values_.begin());
    probe_values_[gl::decay] = knock_table::decay_unity;
    probe_values_[pad * pad_parameters + o::lift] = knock_table::lift_off;
    probe_values_[pad * pad_parameters + o::length] = knock_table::length_off;
    probe_values_[pad * pad_parameters + o::spread] = 0.0;
    probe_values_[pad * pad_parameters + o::haas] = 0.0;
    const auto playing = values_;
    values_ = probe_values_;
    Voice& probe = *probe_;
    start(probe, pad, reference_velocity_unit, Hand{}, 0.0, noise_seed(pad));
    probe.gain = 1.0;
    // Unity level, centred, nothing to the room.
    const double centre = std::cos(pi / 4.0);
    probe.level.to(1.0, 0.0, true);
    probe.left.to(centre, 0.0, true);
    probe.right.to(centre, 0.0, true);
    probe.room.to(0.0, 0.0, true);
    Chunk left{};
    Chunk right{};
    Chunk send_left{};
    Chunk send_right{};
    double peak = 0.0;
    double power = 0.0;
    double loudest = 0.0;
    double sample_peak = 0.0;
    const double smoothing = std::exp(-1.0 / (calibration_rms_seconds * rate_));
    const auto limit = static_cast<std::uint32_t>(std::lround(calibration_seconds * rate_));
    const auto ring_limit =
        static_cast<std::uint32_t>(std::lround(calibration_ring_seconds * rate_));
    std::uint32_t ringing_for = 0;
    for (std::uint32_t done = 0; done < limit && probe.active; done += control_interval_frames) {
      left.fill(0.0);
      right.fill(0.0);
      render_voice(probe, left, right, send_left, send_right, control_interval_frames, peak);
      for (std::size_t i = 0; i < control_interval_frames; ++i) {
        const double x = 0.5 * (left[i] + right[i]) / centre;
        power = smoothing * power + (1.0 - smoothing) * x * x;
        loudest = std::max(loudest, power);
        sample_peak = std::max(sample_peak, std::fabs(x));
      }
      // Bend moves the pitch, not the level: a bending hit is measured
      // like any other (waiting for a slow bend ran the probe 80 ms).
      if (!exciting(probe)) {
        ringing_for += control_interval_frames;
        if (ringing_for >= ring_limit || ringing(probe) * centre < calibration_margin * peak) {
          break;
        }
      }
    }
    probe.active = false;
    values_ = playing;
    constexpr double least_level = 1e-9;
    c.scale = std::min(target_level / std::max(least_level, std::sqrt(loudest)),
                       peak_ceiling / std::max(least_level, sample_peak));
    return c.scale;
  }

  void trigger(Voice& voice, std::size_t pad, double velocity, double scale, double beat) noexcept {
    // Hand and Motion: seeded by pad and hit since the last reset (Orbit by
    // the song position), so export equals playback.
    const std::uint32_t seed =
        static_cast<std::uint32_t>(pad) * hand_pad_seed + pad_hits_[pad] * hand_hit_seed + 1U;
    const double motion = motion_offset(pad, beat);
    ++pad_hits_[pad];
    Mulberry jitter{seed};
    const double hand_amount = clamp01(value(pad, o::hand));
    Hand hand;
    hand.hardness = jitter.bipolar() * hand_amount;
    hand.position = jitter.bipolar() * hand_amount;
    hand.tune = jitter.bipolar() * hand_amount;
    hand.pan = jitter.bipolar() * hand_amount;
    start(voice, pad, velocity, hand, motion, noise_seed(pad));
    // The calibrated hit, panned where this one lands.
    place_[pad] = scratch_pan_;
    const double theta = (scratch_pan_ + 1.0) * pi / 4.0;
    voice.gain = scale / std::max(std::cos(theta), std::sin(theta));
    voice.loudness = target_level * db_gain(value(pad, o::level));
  }

  // Sets a voice going from the pad's values: the design, the banks, the
  // striker, the exciters and the finish. The caller sets its gain.
  // Motion is the hit's pan offset (motion_offset).
  void start(Voice& voice, std::size_t pad, double velocity, const Hand& hand, double motion,
             std::uint32_t seed) noexcept {
    auto& d = *design_;
    design_hit(values_, pad, rate_, hand, velocity, d);
    // Where the hit lands: pan, hand and Motion.
    scratch_pan_ = std::clamp(d.pan + motion, -1.0, 1.0);

    voice.active = true;
    voice.pad = pad;
    voice.frame = 0;
    voice.gain = 1.0;
    voice.pan_jitter = hand.pan * hand_pan;
    voice.motion = motion;
    // Width: Spread is applied by aim() below; Haas delays the ear away
    // from where the hit lands (the right when it lands in the centre).
    voice.spread = -1.0;
    voice.haas_left = scratch_pan_ > 0.0;
    voice.haas_frames = 0;
    if (!voice.haas_line.empty()) {
      const double haas = std::clamp(value(pad, o::haas), 0.0, knock_table::max_haas_ms);
      voice.haas_frames =
          std::min(static_cast<std::size_t>(std::lround(haas / ms_per_second * rate_)),
                   voice.haas_line.size() - 1);
      if (voice.haas_frames > 0) {
        std::fill(voice.haas_line.begin(), voice.haas_line.end(), 0.0);
        voice.haas_write = 0;
      }
    }

    // The resonator meets the striker as a heavy surface.
    voice.striker = make_striker(d.type, d.hardness);
    double shape_power = 0.0;
    for (std::size_t k = 0; k < d.res_count; ++k) {
      shape_power += d.res[k].shape * d.res[k].shape;
    }
    const double mass = surface_mass(d.type, shape_power);
    const double reference_momentum = momentum_ratio * voice.striker.mass * reference_speed;
    // Each mode's place is kept by its index in the bank.
    voice.resonator.clear();
    for (std::size_t k = 0; k < d.res_count; ++k) {
      const auto& m = d.res[k];
      // An impulse of the reference momentum rings each mode at amp x shape.
      const double gain = m.amp * mass * two_pi * m.hz / reference_momentum;
      if (voice.resonator.add({m.hz, m.t60, m.shape, mass, gain, 0.0})) {
        voice.res_place[voice.resonator.size() - 1] = m.place;
      }
    }
    voice.coupled.clear();
    for (std::size_t k = 0; k < d.coupled_count; ++k) {
      const auto& m = d.coupled[k];
      if (voice.coupled.add({m.hz, m.t60, 0.0, 1.0, 1.0, m.drive})) {
        voice.coupled_place[voice.coupled.size() - 1] = m.place;
      }
    }
    voice.body.clear();
    for (std::size_t k = 0; k < d.body_count; ++k) {
      const auto& m = d.body[k];
      if (voice.body.add({m.hz, m.t60, 0.0, 1.0, 1.0, m.drive})) {
        voice.body_place[voice.body.size() - 1] = m.place;
      }
    }
    voice.striker.launch(0.0, d.speed);

    // Grit: per frame, a momentum of grit / sqrt(effective length) of the
    // contact's, as noise.
    voice.brush = d.brush;
    voice.grit_noise = Mulberry{seed ^ grit_salt};
    voice.grit_frames = d.grit_frames;
    const double grit_length = static_cast<double>(std::max<std::uint32_t>(1, d.grit_frames));
    const double effective = d.brush ? 0.5 * grit_length : grit_tau_fraction * grit_length;
    voice.grit_force =
        momentum_ratio * voice.striker.mass * d.speed * d.grit / std::sqrt(effective) * rate_;
    voice.grit_envelope = 1.0;
    voice.grit_decay = std::exp(-1.0 / (grit_tau_fraction * grit_length));
    // A brush's bristles scrape on their own: its grit stays full band.
    const double grit_hz =
        d.brush ? nyquist_fraction * rate_
                : std::min(grit_bandwidth_per_contact / d.contact, nyquist_fraction * rate_);
    voice.grit_smoothing = std::exp(-two_pi * grit_hz / rate_);
    voice.grit_makeup = std::sqrt((1.0 + voice.grit_smoothing) / (1.0 - voice.grit_smoothing));
    voice.grit_state = 0.0;
    // Click.
    voice.click_noise = Mulberry{seed ^ click_salt};
    voice.click_frames = d.click_frames;
    voice.click = d.click;
    voice.click_envelope = 1.0;
    const double click_length = static_cast<double>(std::max<std::uint32_t>(1, d.click_frames));
    voice.click_decay = std::exp(-1.0 / (click_tau_fraction * click_length));
    voice.click_last = 0.0;
    // Air.
    voice.air_noise = Mulberry{seed ^ air_salt};
    voice.air_on = d.air > 0.0;
    voice.air = d.air;
    voice.air_decay = d.air_tau > 0.0 ? std::exp(-1.0 / (d.air_tau * rate_)) : 0.0;
    voice.air_lowpass = d.air_lowpass;
    voice.air_left = 0.0;
    voice.air_right = 0.0;
    // Bend.
    voice.bend = d.bend;
    voice.bending = d.bend != 0.0;
    // Finish.
    voice.lift_left.fill(0.0);
    voice.lift_right.fill(0.0);
    voice.snap = d.snap;
    voice.snap_fast = 1.0;
    voice.snap_slow = 1.0;
    voice.gated = d.gated;
    voice.gate_frames = d.gate_frames;
    voice.crush = d.crush;
    voice.crush_hold =
        1U + static_cast<std::uint32_t>(std::lround(d.crush * d.crush * crush_hold_span));
    voice.crush_steps = std::exp2(crush_top_bits - crush_bits_span * d.crush);
    voice.held_left = 0.0;
    voice.held_right = 0.0;
    aim(voice, true);
  }

  void update_bend(Voice& voice) noexcept {
    const double t = static_cast<double>(voice.frame) / rate_;
    const double swing = voice.bend * bend_depth * std::exp(-t / bend_seconds);
    if (std::fabs(swing) < bend_settled) {
      voice.bending = false;
      voice.resonator.retune(1.0);
      return;
    }
    voice.resonator.retune(std::max(min_bend_factor, 1.0 + swing));
  }

  void render_voice(Voice& voice, Chunk& left, Chunk& right, Chunk& send_left, Chunk& send_right,
                    std::size_t frames, double& peak) noexcept {
    for (std::size_t i = 0; i < frames; ++i) {
      if (voice.bending && voice.frame % bend_grid_frames == 0) {
        update_bend(voice);
      }
      double force = 0.0;
      if (voice.grit_frames > 0) {
        const double envelope = voice.brush
                                    ? std::sin(pi * static_cast<double>(voice.frame) /
                                               static_cast<double>(voice.frame + voice.grit_frames))
                                    : voice.grit_envelope;
        voice.grit_state = (1.0 - voice.grit_smoothing) * voice.grit_noise.bipolar() +
                           voice.grit_smoothing * voice.grit_state;
        force = voice.grit_force * voice.grit_makeup * voice.grit_state * envelope;
        voice.grit_envelope *= voice.grit_decay;
        --voice.grit_frames;
      }
      const float struck = voice.striker.active
                               ? voice.resonator.step_contact(voice.striker, force, 0.0)
                               : voice.resonator.step(force, 0.0);
      double mono = struck;
      float rung = 0.0F;
      if (voice.coupled.size() > 0) {
        rung = voice.coupled.step(0.0, struck);
        mono += rung;
      }
      if (voice.body.size() > 0) {
        mono += voice.body.step(0.0, static_cast<double>(struck) + static_cast<double>(rung));
      }
      // Spread: each mode from its own place. The banks still ring each
      // other mono, as one object does.
      double modes_left = mono;
      double modes_right = mono;
      if (voice.wide) {
        float a = 0.0F;
        float b = 0.0F;
        voice.resonator.stereo_output(a, b);
        modes_left = a;
        modes_right = b;
        if (voice.coupled.size() > 0) {
          voice.coupled.stereo_output(a, b);
          modes_left += a;
          modes_right += b;
        }
        if (voice.body.size() > 0) {
          voice.body.stereo_output(a, b);
          modes_left += a;
          modes_right += b;
        }
      }
      // The click stays where the stick lands.
      double click = 0.0;
      if (voice.click_frames > 0) {
        const double x = voice.click_noise.bipolar();
        const double high = x - voice.click_last;
        voice.click_last = x;
        const double envelope =
            voice.brush ? std::sin(pi * static_cast<double>(voice.frame) /
                                   static_cast<double>(voice.frame + voice.click_frames))
                        : voice.click_envelope;
        click = high * envelope * voice.click;
        voice.click_envelope *= voice.click_decay;
        --voice.click_frames;
      }
      double l = (modes_left + click) * voice.gain * voice.left.next();
      double r = (modes_right + click) * voice.gain * voice.right.next();
      // Haas: the far ear hears the hit a few milliseconds late.
      if (voice.haas_frames > 0) {
        double& far = voice.haas_left ? l : r;
        auto& line = voice.haas_line;
        line[voice.haas_write] = far;
        std::size_t read = voice.haas_write + line.size() - voice.haas_frames;
        if (read >= line.size()) {
          read -= line.size();
        }
        far = line[read];
        voice.haas_write = voice.haas_write + 1 == line.size() ? 0 : voice.haas_write + 1;
      }
      if (voice.air_on) {
        const double envelope = voice.air * voice.gain;
        voice.air_left += voice.air_lowpass * (voice.air_noise.bipolar() - voice.air_left);
        voice.air_right += voice.air_lowpass * (voice.air_noise.bipolar() - voice.air_right);
        l += voice.air_left * envelope;
        r += voice.air_right * envelope;
        voice.air *= voice.air_decay;
        voice.air_on = voice.air > air_floor;
      }
      // Finish: Lift, Snap, Length.
      const double a = voice.lift.next();
      l = lift_step(voice.lift_left, l, a);
      r = lift_step(voice.lift_right, r, a);
      double shaped = voice.level.next();
      if (voice.snap > 0.0) {
        shaped *= (1.0 + snap_boost * voice.snap * voice.snap_fast) *
                  (1.0 - snap_cut * voice.snap * (1.0 - voice.snap_slow));
        voice.snap_fast *= snap_fast_decay_;
        voice.snap_slow *= snap_slow_decay_;
      }
      bool ended = false;
      if (voice.gated && voice.frame >= voice.gate_frames) {
        const std::uint32_t into = voice.frame - voice.gate_frames;
        shaped *=
            std::max(0.0, 1.0 - static_cast<double>(into) / static_cast<double>(fade_frames_));
        ended = into + 1 >= fade_frames_;
      }
      l *= shaped;
      r *= shaped;
      // Crush: after the level, a sample-and-hold from the hit and a coarse
      // quantiser.
      if (voice.crush > 0.0) {
        if (voice.frame % voice.crush_hold == 0) {
          voice.held_left = l;
          voice.held_right = r;
        }
        l = std::round(voice.held_left * voice.crush_steps) / voice.crush_steps;
        r = std::round(voice.held_right * voice.crush_steps) / voice.crush_steps;
      }
      const double room = voice.room.next();
      left[i] += l;
      right[i] += r;
      send_left[i] += l * room;
      send_right[i] += r * room;
      voice.last_left = l;
      voice.last_right = r;
      peak = std::max({peak, std::fabs(l), std::fabs(r)});
      ++voice.frame;
      if (ended) {
        voice.active = false;
        voice.last_left = 0.0;
        voice.last_right = 0.0;
        return;
      }
    }
  }

  [[nodiscard]] static double safety(double x) noexcept {
    constexpr double knee = 0.9;
    const double magnitude = std::fabs(x);
    if (magnitude <= knee) {
      return x;
    }
    const double over = (magnitude - knee) / (1.0 - knee);
    const double shaped = knee + (1.0 - knee) * std::tanh(over);
    return x < 0.0 ? -shaped : shaped;
  }

  double rate_;
  std::unique_ptr<std::array<Voice, pad_count * voices_per_pad>> voices_;
  std::unique_ptr<HitDesign> design_;
  // The voice that renders calibration hits, and each pad's calibration.
  struct Calibration {
    bool valid = false;
    std::array<double, pad_parameters> key{};
    double velocity = 0.0;
    double scale = 1.0;
  };
  std::unique_ptr<Voice> probe_;
  std::array<Calibration, pad_count> calibration_{};
  std::size_t next_calibration_ = 0;
  // The values a calibration plays (see calibrated_scale).
  std::array<double, knock_table::parameter_count> probe_values_{};
  // The project's key, and the values with following pads moved into it.
  std::optional<project::MusicalKey> key_{};
  std::array<double, knock_table::parameter_count> keyed_values_{};
  // Each pad's calibrated values at the last control tick, and how many
  // ticks they have held: only a settled pad is calibrated ahead of its hit.
  std::array<std::array<double, pad_parameters>, pad_count> last_key_{};
  std::array<std::uint32_t, pad_count> settled_ticks_{};
  double scratch_pan_ = 0.0;
  // Where each pad's latest hit landed, for telemetry.
  std::array<double, pad_count> place_{};
  // The transport at the last control tick, frames rendered since, and
  // frames rendered since the kernel was made: swung hits wait on it.
  SynthTransport transport_{};
  std::size_t frames_since_tick_ = 0;
  std::uint64_t clock_ = 0;
  std::array<PendingHit, max_pending_hits> pending_{};
  std::size_t pending_count_ = 0;
  Room room_{};
  std::span<const double> values_{};
  std::uint32_t frames_ = 0;
  bool primed_ = false;
  double snap_fast_decay_ = 0.0;
  double snap_slow_decay_ = 0.0;
  std::uint32_t fade_frames_ = 1;
  double meter_release_ = 0.0;
  Ramp room_gain_{};
  Ramp out_gain_{};
  std::array<std::uint32_t, pad_count> pad_hits_{};
  std::uint32_t hits_ = 0;
  int last_pad_ = -1;
  std::array<double, pad_count> pad_peak_{};
  double output_peak_ = 0.0;
  double last_wet_left_ = 0.0;
  double last_wet_right_ = 0.0;
  Declick declick_;
};

std::unique_ptr<SynthKernel> create_kernel(double sample_rate) {
  return std::make_unique<KnockKernel>(sample_rate);
}

// The longest pad's ring to -100 dB with the global Decay, a little more for
// driven rings, its excitation, cut by its gate; then the room's tail.
std::uint64_t tail_frames(std::span<const double> values, double sample_rate) {
  const double decay_scale = values[gl::decay] / knock_table::decay_unity;
  double longest = 0.0;
  bool roomy = false;
  for (std::size_t pad = 0; pad < pad_count; ++pad) {
    const double* v = values.data() + pad * pad_parameters;
    const double tone_decay = tone_decay_floor + (1.0 - tone_decay_floor) * clamp01(v[o::tone]);
    double tau = v[o::decay] * tone_decay;
    if (v[o::amount] > 0.0) {
      tau = std::max(tau,
                     std::max(coupled_min_tau, v[o::decay] * coupled_decay_stretch) * tone_decay);
    }
    if (v[o::hollow] > 0.0) {
      const double box = v[o::hollow] * v[o::size];
      tau = std::max({tau, body_tau + body_tau_span * box, air_tau + air_tau_span * box});
    }
    double seconds = tau * decay_scale * ln100000 * chain_margin + excitation_seconds;
    if (v[o::length] < knock_table::length_off) {
      seconds = std::min(seconds, v[o::length] * std::min(1.0, decay_scale) + gate_fade_seconds);
    }
    // Haas holds the far ear back.
    seconds += std::clamp(v[o::haas], 0.0, knock_table::max_haas_ms) / ms_per_second;
    longest = std::max(longest, seconds);
    roomy = roomy || v[o::room] > 0.0;
  }
  if (roomy && values[gl::room_level] > silent_db) {
    longest += room_t60(values[gl::room_size]) * ln100000 / modal::ln1000 + room_ms.back() * 1e-3;
  }
  // A swung last hit starts after its note.
  longest += swing_delay_beats(values[gl::swing]) / slowest_swing_beats_per_second;
  return static_cast<std::uint64_t>(std::ceil(longest * sample_rate)) + declick_frames;
}

const EngineDescriptor descriptor{"knock", "Knock", 1, knock_table::parameters,
                                  knock_presets::presets};
// Percussion ignores the pedals: the kernel models them by doing nothing,
// as Anvil's does, so the runtime's generic sustain never holds a hit.
const SynthEngine knock_engine{&descriptor, &create_kernel, &tail_frames, true};

} // namespace

std::vector<ModeView> design_pad(std::span<const double> values, std::size_t pad,
                                 double sample_rate) {
  std::vector<ModeView> modes;
  if (pad >= pad_count || values.size() < knock_table::parameter_count || !(sample_rate > 0.0)) {
    return modes;
  }
  auto d = std::make_unique<HitDesign>();
  design_hit(values, pad, sample_rate, Hand{}, reference_velocity_unit, *d);
  modes.reserve(d->res_count + d->coupled_count + d->body_count);
  double strongest = 0.0;
  for (std::size_t k = 0; k < d->res_count; ++k) {
    modes.push_back({d->res[k].hz, d->res[k].display, d->res[k].t60, ModePart::resonator});
  }
  for (std::size_t k = 0; k < d->coupled_count; ++k) {
    modes.push_back(
        {d->coupled[k].hz, d->coupled[k].display, d->coupled[k].t60, ModePart::coupled});
  }
  for (std::size_t k = 0; k < d->body_count; ++k) {
    modes.push_back({d->body[k].hz, d->body[k].display, d->body[k].t60, ModePart::body});
  }
  for (const auto& mode : modes) {
    strongest = std::max(strongest, mode.level);
  }
  if (strongest > 0.0) {
    for (auto& mode : modes) {
      mode.level /= strongest;
    }
  }
  return modes;
}

PadRole pad_role(std::span<const double> values, std::size_t pad) noexcept {
  if (pad >= pad_count || values.size() < knock_table::parameter_count) {
    return PadRole::mid;
  }
  const double* v = values.data() + pad * pad_parameters;
  return knock_table::role_of(static_cast<int>(choice(v[o::type], striker_type_count)),
                              v[o::pitch]);
}

double in_key(double hz, std::optional<project::MusicalKey> key) noexcept {
  if (!key || !(hz > 0.0) || !std::isfinite(hz)) {
    return hz;
  }
  const double note = tuning_a4_key + semitones_per_octave * std::log2(hz / tuning_a4_hz);
  const auto& steps = key->mode == project::KeyMode::minor ? minor_steps : major_steps;
  const auto below = static_cast<int>(std::floor(note));
  double nearest = note;
  double distance = std::numeric_limits<double>::infinity();
  // Ascending, so of two equally near the lower wins.
  for (int candidate = below - key_search_semitones; candidate <= below + key_search_semitones;
       ++candidate) {
    const int degree = ((candidate - key->tonic) % semitones_per_octave + semitones_per_octave) %
                       semitones_per_octave;
    if (std::ranges::find(steps, degree) == steps.end()) {
      continue;
    }
    if (const double d = std::abs(note - candidate); d < distance - key_tie_semitones) {
      distance = d;
      nearest = candidate;
    }
  }
  return note_hz(nearest);
}

double note_hz(double key) noexcept {
  return tuning_a4_hz * std::exp2((key - tuning_a4_key) / semitones_per_octave);
}

double swing_delay_beats(double swing) noexcept {
  return std::clamp(swing, 0.0, knock_table::max_swing) / triplet_swing * triplet_delay_beats;
}

const SynthEngine& engine() { return knock_engine; }

} // namespace oscillate::native_dsp::synth::knock
