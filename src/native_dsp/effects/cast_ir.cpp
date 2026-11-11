#include "cast_ir.hpp"

#include "cast_convolver.hpp"
#include "dsp/offline_fft.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <map>
#include <numbers>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace oscillate::native_dsp::effects::cast_ir {
namespace {

using native_dsp::dsp::raised_cosine;

constexpr double pi = std::numbers::pi;
constexpr double two_pi = 2.0 * pi;
// 60 dB of amplitude, as the natural log of its ratio: RT60 decays use it.
const double ln_thousand = std::log(1'000.0);

// ---- Resampling ------------------------------------------------------------------------

constexpr std::size_t sinc_zero_crossings = 32;
constexpr std::size_t sinc_table_resolution = 512; // Points per zero crossing.
constexpr double sinc_kaiser_beta = 10.0;
// The pass band's edge as a fraction of the lower Nyquist frequency.
constexpr double resample_passband = 0.95;

struct SincTable {
  std::vector<double> values;
  SincTable() : values(sinc_zero_crossings * sinc_table_resolution + 2, 0.0) {
    const auto half = static_cast<double>(sinc_zero_crossings);
    for (std::size_t i = 0; i < values.size(); ++i) {
      const double x = static_cast<double>(i) / static_cast<double>(sinc_table_resolution);
      const double sinc = x == 0.0 ? 1.0 : std::sin(pi * x) / (pi * x);
      values[i] = sinc * native_dsp::dsp::kaiser(x, half, sinc_kaiser_beta);
    }
  }
  [[nodiscard]] double at(double x) const noexcept {
    const double position = std::fabs(x) * static_cast<double>(sinc_table_resolution);
    const auto index = static_cast<std::size_t>(position);
    if (index + 1 >= values.size()) {
      return 0.0;
    }
    const double t = position - static_cast<double>(index);
    return values[index] + t * (values[index + 1] - values[index]);
  }
};

const SincTable& sinc_table() {
  static const SincTable table;
  return table;
}

// ---- Shared modelling ------------------------------------------------------------------

constexpr double speed_of_sound = 343.0; // m/s at 20 C.
// Octave bands the models are specified in.
constexpr std::array<double, 7> band_hz{125, 250, 500, 1'000, 2'000, 4'000, 8'000};
constexpr std::size_t band_count = band_hz.size();
using Bands = std::array<double, band_count>;
// Air's intensity attenuation per metre at 20 C and 50 % humidity (ISO
// 9613-1), per band.
constexpr Bands air_per_metre{1.0e-4, 3.0e-4, 6.3e-4, 1.07e-3, 2.27e-3, 6.77e-3, 2.42e-2};
// Sabine's constant, 24 ln(10) / c, in s/m.
constexpr double sabine = 0.161;
constexpr std::size_t response_count = cast_dsp::max_responses;

// A per-band value at `hz`, interpolated in log frequency and held flat
// past the outer bands.
[[nodiscard]] double band_value(const Bands& values, double hz) noexcept {
  if (hz <= band_hz.front()) {
    return values.front();
  }
  if (hz >= band_hz.back()) {
    return values.back();
  }
  for (std::size_t b = 1; b < band_count; ++b) {
    if (hz <= band_hz[b]) {
      const double t = std::log(hz / band_hz[b - 1]) / std::log(band_hz[b] / band_hz[b - 1]);
      return values[b - 1] + t * (values[b] - values[b - 1]);
    }
  }
  return values.back();
}

void normalise_energy(Responses& responses) {
  double energy = 0.0;
  for (const auto& response : responses) {
    for (const float x : response) {
      energy += static_cast<double>(x) * static_cast<double>(x);
    }
  }
  if (energy <= 0.0) {
    return;
  }
  const auto scale = static_cast<float>(std::sqrt(static_cast<double>(responses.size()) / energy));
  for (auto& response : responses) {
    for (float& x : response) {
      x *= scale;
    }
  }
}

// ---- The diffuse tail ------------------------------------------------------------------

constexpr std::size_t tail_frame = 1'024;
constexpr std::size_t tail_hop = tail_frame / 4;
// The Hann window's mean square over its overlaps, per hop ratio: 3/8.
constexpr double hann_mean_square = 3.0 / 8.0;

struct TailModel {
  Bands rt60{};
  // Power per frame of the diffuse field at t = 0, before onset.
  double power = 1.0;
  // Tail amplitude rises from 0 at `onset_start` to 1 at `onset_end`.
  double onset_start = 0.0;
  double onset_end = 0.0;
  // Coherence between the two outputs of one input, by frequency: a
  // spaced pair (diffuse-field sinc) or a fixed value.
  double mic_spacing = 0.0;
  double fixed_coherence = 0.0;
  // A spectral tilt in dB per octave from 1 kHz, for brightness.
  double tilt_db_per_octave = 0.0;
  // Frequency-dependent onset (a plate's dispersion): extra delay at
  // 125 Hz, falling as 1/sqrt(f).
  double dispersion_seconds = 0.0;
  std::uint64_t seed = 0;
};

// Gaussian noise shaped frame by frame: each bin's amplitude decays at its
// band's RT60. Writes `frames` frames of the four responses, added to them.
void add_tail(Responses& out, std::size_t frames, const TailModel& model) {
  constexpr std::size_t bins = tail_frame / 2;
  cast_dsp::Fft fft(tail_frame);
  cast_dsp::Floats spectrum(tail_frame);
  cast_dsp::Floats time(tail_frame);
  std::vector<double> window(tail_frame);
  for (std::size_t n = 0; n < tail_frame; ++n) {
    window[n] = native_dsp::dsp::hann(static_cast<double>(n), static_cast<double>(tail_frame));
  }
  const double overlap =
      hann_mean_square * static_cast<double>(tail_frame) / static_cast<double>(tail_hop);
  // An unscaled inverse of bins with real and imaginary parts of variance
  // s^2 has variance 2 s^2 N per sample; the window's overlap adds `overlap`.
  const double unit = 1.0 / std::sqrt(2.0 * static_cast<double>(tail_frame) * overlap);
  std::vector<double> coherence(bins);
  std::vector<double> decay_rate(bins); // Amplitude nepers per second.
  std::vector<double> colour(bins);
  std::vector<double> delay(bins);
  constexpr double reference_hz = 1'000.0;
  constexpr double lowest_band_hz = band_hz.front();
  for (std::size_t k = 0; k < bins; ++k) {
    const double hz = static_cast<double>(k) * design_rate / static_cast<double>(tail_frame);
    const double kd = two_pi * hz * model.mic_spacing / speed_of_sound;
    coherence[k] =
        model.mic_spacing > 0.0 ? (kd == 0.0 ? 1.0 : std::sin(kd) / kd) : model.fixed_coherence;
    decay_rate[k] = ln_thousand / band_value(model.rt60, std::max(hz, lowest_band_hz));
    colour[k] = native_dsp::dsp::db_to_gain(model.tilt_db_per_octave *
                                            std::log2(std::max(hz, lowest_band_hz) / reference_hz));
    delay[k] = model.dispersion_seconds * std::sqrt(lowest_band_hz / std::max(hz, lowest_band_hz));
  }
  native_dsp::dsp::Xorshift64Star random(model.seed + 1);
  const double sigma = std::sqrt(model.power);
  const std::size_t hops = frames / tail_hop + tail_frame / tail_hop;
  std::array<std::vector<std::complex<double>>, 2> shared;
  for (auto& input : shared) {
    input.resize(bins);
  }
  for (std::size_t hop = 0; hop < hops; ++hop) {
    // The frame's centre, in seconds.
    const double t = static_cast<double>(hop * tail_hop) / design_rate;
    for (auto& input : shared) {
      for (auto& value : input) {
        value = {random.gaussian(), random.gaussian()};
      }
    }
    for (std::size_t r = 0; r < response_count; ++r) {
      const std::size_t input = r / 2;
      spectrum.zero();
      float* bins_out = spectrum.data();
      for (std::size_t k = 1; k < bins; ++k) {
        const double local = t - delay[k];
        double onset = 1.0;
        if (local < model.onset_start) {
          onset = 0.0;
        } else if (local < model.onset_end) {
          onset = std::sqrt(
              raised_cosine((local - model.onset_start) / (model.onset_end - model.onset_start)));
        }
        if (onset == 0.0) {
          continue;
        }
        const double rho = coherence[k];
        const std::complex<double> own{random.gaussian(), random.gaussian()};
        const std::complex<double> value =
            rho * shared[input][k] + std::sqrt(std::max(0.0, 1.0 - rho * rho)) * own;
        const double amplitude =
            sigma * unit * onset * colour[k] * std::exp(-decay_rate[k] * std::max(0.0, local));
        bins_out[2 * k] = static_cast<float>(value.real() * amplitude);
        bins_out[2 * k + 1] = static_cast<float>(value.imag() * amplitude);
      }
      fft.inverse(spectrum.data(), time.data());
      const std::size_t start = hop * tail_hop;
      for (std::size_t n = 0; n < tail_frame; ++n) {
        // Frames start half a frame early so the first is centred at 0.
        const std::ptrdiff_t at =
            static_cast<std::ptrdiff_t>(start + n) - static_cast<std::ptrdiff_t>(tail_frame / 2);
        if (at >= 0 && static_cast<std::size_t>(at) < frames) {
          out[r][static_cast<std::size_t>(at)] +=
              static_cast<float>(window[n] * static_cast<double>(time.data()[n]));
        }
      }
    }
  }
}

// ---- Rooms -----------------------------------------------------------------------------

struct Room {
  std::array<double, 3> size{}; // Width, depth, height in metres.
  Bands absorption{};           // Mean absorption coefficient per band.
  double source_distance = 0.0; // From the listener, ahead.
  double source_spread = 0.0;   // Between the left and right sources.
  double seconds = 0.0;         // Length of the IR.
  std::uint64_t seed = 0;
};

// ORTF: two cardioids 17 cm apart, 110 degrees between them.
constexpr double mic_spacing = 0.17;
constexpr double mic_half_angle = 55.0 * pi / 180.0;
// The listener's place in the room: across the middle, this far back of
// the room's depth, at ear height (or mid-height in a low room).
constexpr double listener_depth = 0.65;
constexpr double ear_height = 1.5;
constexpr double source_height_above_ear = 0.3;
constexpr double wall_margin = 0.5;
// Image sources are rendered to the mixing time (Polack: sqrt(V) ms) and at
// least this long; the tail takes over from half the mixing time.
constexpr double min_early_seconds = 0.08;
constexpr double milliseconds_per_root_volume = 1e-3;
constexpr std::size_t max_order_bucket = 12;
constexpr double distance_bucket_metres = 10.0;
// Each reflection's spectrum: a linear-phase FIR from its band gains.
constexpr std::size_t reflection_taps = 256;
// A cardioid's mean power gain over the sphere.
constexpr double cardioid_diffuse_power = 1.0 / 3.0;

// The reflection filter for `order` wall hits over `distance` metres.
std::vector<double> reflection_kernel(const Room& room, std::size_t order, double distance) {
  const native_dsp::dsp::OfflineFft fft(reflection_taps);
  std::vector<std::complex<double>> spectrum(reflection_taps);
  Bands gains{};
  for (std::size_t b = 0; b < band_count; ++b) {
    gains[b] = std::pow(std::sqrt(1.0 - room.absorption[b]), static_cast<double>(order)) *
               std::exp(-air_per_metre[b] * distance / 2.0);
  }
  for (std::size_t k = 0; k <= reflection_taps / 2; ++k) {
    const double hz = static_cast<double>(k) * design_rate / static_cast<double>(reflection_taps);
    const double gain = band_value(gains, hz);
    spectrum[k] = gain;
    if (k > 0 && k < reflection_taps / 2) {
      spectrum[reflection_taps - k] = gain;
    }
  }
  fft.inverse(spectrum);
  std::vector<double> kernel(reflection_taps);
  for (std::size_t n = 0; n < reflection_taps; ++n) {
    const std::size_t from = (n + reflection_taps / 2) % reflection_taps;
    kernel[n] =
        spectrum[from].real() * native_dsp::dsp::hann(static_cast<double>(n) + 1.0,
                                                      static_cast<double>(reflection_taps) + 1.0);
  }
  return kernel;
}

Responses room_responses(const Room& room) {
  const auto frames = static_cast<std::size_t>(room.seconds * design_rate);
  Responses out(response_count, std::vector<float>(frames, 0.0F));
  const auto [width, depth, height] = room.size;
  const double volume = width * depth * height;
  const double surface = 2.0 * (width * depth + width * height + depth * height);
  Bands rt60{};
  for (std::size_t b = 0; b < band_count; ++b) {
    rt60[b] = sabine * volume /
              (-surface * std::log(1.0 - room.absorption[b]) + 4.0 * air_per_metre[b] * volume);
  }
  const double mixing = std::sqrt(volume) * milliseconds_per_root_volume;
  const double early_end = std::max(min_early_seconds, mixing);

  const std::array<double, 3> listener{width / 2.0, depth * listener_depth,
                                       std::min(ear_height, height / 2.0)};
  const auto inside = [&](double value, double extent) {
    return std::clamp(value, wall_margin, extent - wall_margin);
  };
  const std::array<std::array<double, 3>, 2> sources{
      {{inside(listener[0] - room.source_spread / 2.0, width),
        inside(listener[1] - room.source_distance, depth),
        inside(listener[2] + source_height_above_ear, height)},
       {inside(listener[0] + room.source_spread / 2.0, width),
        inside(listener[1] - room.source_distance, depth),
        inside(listener[2] + source_height_above_ear, height)}}};
  // Mics face the sources (-y), turned out by the half angle.
  const std::array<std::array<double, 3>, 2> mics{
      {{listener[0] - mic_spacing / 2.0, listener[1], listener[2]},
       {listener[0] + mic_spacing / 2.0, listener[1], listener[2]}}};
  const std::array<std::array<double, 2>, 2> axes{
      {{-std::sin(mic_half_angle), -std::cos(mic_half_angle)},
       {std::sin(mic_half_angle), -std::cos(mic_half_angle)}}};

  std::map<std::pair<std::size_t, std::size_t>, std::vector<double>> kernels;
  const double reach = early_end * speed_of_sound;
  for (std::size_t s = 0; s < 2; ++s) {
    const auto& source = sources[s];
    std::array<int, 3> limit{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
      limit[axis] = static_cast<int>(std::ceil(reach / (2.0 * room.size[axis]))) + 1;
    }
    for (int nx = -limit[0]; nx <= limit[0]; ++nx) {
      for (int ny = -limit[1]; ny <= limit[1]; ++ny) {
        for (int nz = -limit[2]; nz <= limit[2]; ++nz) {
          for (int parity = 0; parity < 8; ++parity) {
            const std::array<int, 3> n{nx, ny, nz};
            const std::array<int, 3> q{parity & 1, (parity >> 1) & 1, (parity >> 2) & 1};
            std::array<double, 3> image{};
            std::size_t order = 0;
            for (std::size_t axis = 0; axis < 3; ++axis) {
              image[axis] = (1.0 - 2.0 * q[axis]) * source[axis] + 2.0 * n[axis] * room.size[axis];
              order += static_cast<std::size_t>(std::abs(n[axis] - q[axis]) + std::abs(n[axis]));
            }
            for (std::size_t m = 0; m < 2; ++m) {
              const std::array<double, 3> d{image[0] - mics[m][0], image[1] - mics[m][1],
                                            image[2] - mics[m][2]};
              const double distance = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
              if (distance > reach || distance <= 0.0) {
                continue;
              }
              const double cosine = (d[0] * axes[m][0] + d[1] * axes[m][1]) / distance;
              const double gain = 0.5 * (1.0 + cosine) / distance;
              const std::size_t bucket_order = std::min(order, max_order_bucket);
              const auto bucket_distance =
                  static_cast<std::size_t>(distance / distance_bucket_metres);
              auto& kernel = kernels[{bucket_order, bucket_distance}];
              if (kernel.empty()) {
                kernel = reflection_kernel(room, bucket_order,
                                           (static_cast<double>(bucket_distance) + 0.5) *
                                               distance_bucket_metres);
              }
              const double arrival = distance / speed_of_sound;
              // Early sound fades out over the second half of the mixing
              // time as the tail fades in.
              double weight = 1.0;
              if (arrival > mixing / 2.0) {
                const double x = (arrival - mixing / 2.0) / (mixing / 2.0);
                weight = x >= 1.0 ? 0.0 : std::sqrt(1.0 - raised_cosine(x));
              }
              if (weight == 0.0 && order > 0) {
                continue;
              }
              const auto centre = static_cast<std::ptrdiff_t>(std::lround(arrival * design_rate));
              auto& response = out[s * 2 + m];
              for (std::size_t tap = 0; tap < reflection_taps; ++tap) {
                const std::ptrdiff_t at = centre + static_cast<std::ptrdiff_t>(tap) -
                                          static_cast<std::ptrdiff_t>(reflection_taps / 2);
                if (at >= 0 && static_cast<std::size_t>(at) < frames) {
                  response[static_cast<std::size_t>(at)] +=
                      static_cast<float>(gain * (order == 0 ? 1.0 : weight) * kernel[tap]);
                }
              }
            }
          }
        }
      }
    }
  }
  // The diffuse field's power per frame at t = 0: 4 pi c / V per second
  // for a unit source at 1 m, through a cardioid.
  TailModel tail;
  tail.rt60 = rt60;
  tail.power = 4.0 * pi * speed_of_sound / volume / design_rate * cardioid_diffuse_power;
  tail.onset_start = mixing / 2.0;
  tail.onset_end = mixing;
  tail.mic_spacing = mic_spacing;
  tail.seed = room.seed;
  add_tail(out, frames, tail);
  return out;
}

// ---- Plate and spring ------------------------------------------------------------------

struct Plate {
  Bands rt60{};
  double tilt_db_per_octave = 0.0;
  double dispersion_seconds = 0.0;
  double seconds = 0.0;
  std::uint64_t seed = 0;
};

// Two pickups on a steel plate: no early reflections, a dense field from
// the start with the lows arriving last, decorrelated outputs.
constexpr double plate_coherence = 0.1;
constexpr double plate_onset_seconds = 0.002;

Responses plate_responses(const Plate& plate) {
  const auto frames = static_cast<std::size_t>(plate.seconds * design_rate);
  Responses out(response_count, std::vector<float>(frames, 0.0F));
  TailModel tail;
  tail.rt60 = plate.rt60;
  tail.power = 1.0;
  tail.onset_start = 0.0;
  tail.onset_end = plate_onset_seconds;
  tail.fixed_coherence = plate_coherence;
  tail.tilt_db_per_octave = plate.tilt_db_per_octave;
  tail.dispersion_seconds = plate.dispersion_seconds;
  tail.seed = plate.seed;
  add_tail(out, frames, tail);
  return out;
}

struct Spring {
  std::array<double, response_count> loop_seconds{};
  double rt60 = 0.0;
  double seconds = 0.0;
};

// A spring's dispersion: a cascade of first-order allpasses whose negative
// coefficient delays the lows most, so each echo is a falling chirp.
constexpr std::size_t spring_allpasses = 128;
constexpr double spring_allpass = -0.7;
constexpr double spring_loop_damping_hz = 5'000.0;
constexpr double spring_output_low_pass_hz = 4'500.0;
constexpr double spring_output_high_pass_hz = 80.0;

Responses spring_responses(const Spring& spring) {
  const auto frames = static_cast<std::size_t>(spring.seconds * design_rate);
  Responses out(response_count, std::vector<float>(frames, 0.0F));
  const double damping = native_dsp::dsp::one_pole_coefficient(spring_loop_damping_hz, design_rate);
  const double low_pass =
      native_dsp::dsp::one_pole_coefficient(spring_output_low_pass_hz, design_rate);
  const double high_pass =
      native_dsp::dsp::one_pole_coefficient(spring_output_high_pass_hz, design_rate);
  for (std::size_t r = 0; r < response_count; ++r) {
    const auto loop = static_cast<std::size_t>(std::lround(spring.loop_seconds[r] * design_rate));
    const double feedback = std::exp(-ln_thousand * spring.loop_seconds[r] / spring.rt60);
    std::vector<double> line(loop, 0.0);
    std::array<double, spring_allpasses> state{};
    double damped = 0.0;
    double smooth = 0.0;
    double floor = 0.0;
    std::size_t at = 0;
    for (std::size_t n = 0; n < frames; ++n) {
      double x = line[at];
      for (double& s : state) {
        const double y = spring_allpass * x + s;
        s = x - spring_allpass * y;
        x = y;
      }
      damped += damping * (x - damped);
      const double input = n == 0 ? 1.0 : 0.0;
      line[at] = input + feedback * damped;
      at = (at + 1) % loop;
      smooth += low_pass * (damped - smooth);
      floor += high_pass * (smooth - floor);
      out[r][n] = static_cast<float>(smooth - floor);
    }
  }
  return out;
}

// ---- The bundled set -------------------------------------------------------------------

struct Model {
  std::string_view id;
  enum class Kind : std::uint8_t { room, plate, spring } kind = Kind::room;
  Room room{};
  Plate plate{};
  Spring spring{};
};

// Absorption per band (125 Hz to 8 kHz) of the rooms' surfaces, as mean
// coefficients of typical finishes.
constexpr Bands booth_absorption{0.25, 0.4, 0.6, 0.7, 0.7, 0.7, 0.65};
constexpr Bands drywall_room_absorption{0.25, 0.18, 0.15, 0.14, 0.14, 0.15, 0.17};
constexpr Bands wood_room_absorption{0.15, 0.12, 0.1, 0.09, 0.09, 0.1, 0.12};
constexpr Bands plaster_chamber_absorption{0.04, 0.04, 0.045, 0.05, 0.055, 0.06, 0.07};
constexpr Bands hall_absorption{0.18, 0.19, 0.2, 0.21, 0.22, 0.24, 0.26};
// A stone nave with wooden pews, people and hangings.
constexpr Bands stone_absorption{0.1, 0.1, 0.11, 0.12, 0.13, 0.14, 0.15};
// Bare concrete with racking and stock.
constexpr Bands concrete_absorption{0.08, 0.08, 0.09, 0.1, 0.11, 0.12, 0.14};
// An EMT 140-style plate's decay per band, and a brighter, shorter one.
constexpr Bands plate_rt60{1.9, 2.3, 2.6, 2.6, 2.4, 2.0, 1.6};
constexpr Bands bright_plate_rt60{1.0, 1.2, 1.4, 1.4, 1.35, 1.25, 1.1};

const std::array<Model, 10> models{{
    {"booth", Model::Kind::room, {{2.2, 1.8, 2.4}, booth_absorption, 1.0, 0.6, 0.4, 11}},
    {"small_room",
     Model::Kind::room,
     {{4.2, 3.6, 2.7}, drywall_room_absorption, 1.8, 1.2, 1.0, 12}},
    {"live_room", Model::Kind::room, {{9.0, 7.0, 4.0}, wood_room_absorption, 3.0, 2.0, 1.6, 13}},
    {"chamber",
     Model::Kind::room,
     {{6.5, 4.8, 3.6}, plaster_chamber_absorption, 2.5, 1.5, 3.0, 14}},
    {"hall", Model::Kind::room, {{32.0, 42.0, 17.0}, hall_absorption, 12.0, 5.0, 3.8, 15}},
    {"cathedral", Model::Kind::room, {{30.0, 85.0, 28.0}, stone_absorption, 18.0, 6.0, 9.5, 16}},
    {"warehouse", Model::Kind::room, {{28.0, 40.0, 9.0}, concrete_absorption, 10.0, 4.0, 6.0, 17}},
    {"plate", Model::Kind::plate, {}, {plate_rt60, 1.0, 0.012, 3.5, 18}},
    {"bright_plate", Model::Kind::plate, {}, {bright_plate_rt60, 2.0, 0.008, 2.0, 19}},
    {"spring", Model::Kind::spring, {}, {}, {{0.0353, 0.0419, 0.0386, 0.0451}, 2.2, 3.0}},
}};

} // namespace

std::vector<float> resample(std::span<const float> in, double from, double to,
                            std::size_t max_frames) {
  if (in.empty() || !(from > 0.0) || !(to > 0.0)) {
    return {};
  }
  if (from == to) {
    return {in.begin(), in.begin() + static_cast<std::ptrdiff_t>(std::min(in.size(), max_frames))};
  }
  const auto& table = sinc_table();
  const double ratio = from / to;
  const double cutoff = resample_passband * std::min(1.0, to / from);
  const double half = static_cast<double>(sinc_zero_crossings) / cutoff;
  const auto length = std::min(
      max_frames, static_cast<std::size_t>(std::ceil(static_cast<double>(in.size()) / ratio)));
  std::vector<float> out(length, 0.0F);
  const auto last = static_cast<std::ptrdiff_t>(in.size()) - 1;
  for (std::size_t n = 0; n < length; ++n) {
    const double t = static_cast<double>(n) * ratio;
    const auto first =
        std::max<std::ptrdiff_t>(0, static_cast<std::ptrdiff_t>(std::ceil(t - half)));
    const auto end = std::min(last, static_cast<std::ptrdiff_t>(std::floor(t + half)));
    double sum = 0.0;
    for (std::ptrdiff_t k = first; k <= end; ++k) {
      sum += static_cast<double>(in[static_cast<std::size_t>(k)]) *
             table.at((t - static_cast<double>(k)) * cutoff);
    }
    out[n] = static_cast<float>(sum * cutoff);
  }
  return out;
}

Responses synthesise(std::string_view id) {
  const auto* model = std::ranges::find(models, id, &Model::id);
  if (model == models.end()) {
    return {};
  }
  Responses responses;
  switch (model->kind) {
  case Model::Kind::room:
    responses = room_responses(model->room);
    break;
  case Model::Kind::plate:
    responses = plate_responses(model->plate);
    break;
  case Model::Kind::spring:
    responses = spring_responses(model->spring);
    break;
  }
  normalise_energy(responses);
  return responses;
}

// ---- Shaping ---------------------------------------------------------------------------

namespace {

// A cut end fades out over this long (or a quarter of what is left).
constexpr double end_fade_seconds = 0.05;
constexpr std::size_t end_fade_divisor = 4;
// The phase vocoder that stretches: 2048-point frames every 256 output
// frames (Hann, 8x overlap).
constexpr std::size_t stretch_frame = 2'048;
constexpr std::size_t stretch_hop = stretch_frame / 8;
// The first few milliseconds (the direct sound) are kept unstretched and
// crossfade into the stretched IR.
constexpr double direct_keep_seconds = 0.0025;
constexpr double direct_fade_seconds = 0.0025;
constexpr double stretch_tolerance = 1e-9;
// Trailing samples this far under the peak are dropped.
constexpr double tail_floor = 1e-6;
constexpr double floor_db = -120.0;
// Schroeder's fit points (dB) for T20, and for T10 when T20 is out of reach.
constexpr double fit_start_db = -5.0;
constexpr double fit_t20_db = -25.0;
constexpr double fit_t10_db = -15.0;
constexpr double t20_scale = 3.0;
constexpr double t10_scale = 6.0;

void fade_end(std::vector<float>& response, std::size_t frames) {
  const std::size_t size = response.size();
  frames = std::min(frames, size);
  for (std::size_t i = 0; i < frames; ++i) {
    const double x = static_cast<double>(i + 1) / static_cast<double>(frames + 1);
    response[size - 1 - i] *= static_cast<float>(raised_cosine(x));
  }
}

[[nodiscard]] double wrap_phase(double phase) noexcept {
  return phase - two_pi * std::round(phase / two_pi);
}

std::vector<float> stretch_response(const std::vector<float>& in, double factor,
                                    std::size_t max_frames, double rate) {
  constexpr std::size_t bins = stretch_frame / 2;
  cast_dsp::Fft fft(stretch_frame);
  cast_dsp::Floats frame(stretch_frame);
  cast_dsp::Floats spectrum(stretch_frame);
  std::vector<double> window(stretch_frame);
  for (std::size_t n = 0; n < stretch_frame; ++n) {
    window[n] = native_dsp::dsp::hann(static_cast<double>(n), static_cast<double>(stretch_frame));
  }
  const std::size_t length = std::min(
      max_frames, static_cast<std::size_t>(std::lround(static_cast<double>(in.size()) * factor)));
  const std::size_t pad = stretch_frame / 2;
  std::vector<double> out(length + stretch_frame, 0.0);
  std::vector<double> analysis_phase(bins, 0.0);
  std::vector<double> synthesis_phase(bins, 0.0);
  const double analysis_hop = static_cast<double>(stretch_hop) / factor;
  const double overlap =
      hann_mean_square * static_cast<double>(stretch_frame) / static_cast<double>(stretch_hop);
  const double scale = 1.0 / (overlap * static_cast<double>(stretch_frame));
  std::ptrdiff_t previous_start = 0;
  const std::size_t frames = (length + pad) / stretch_hop + 1;
  for (std::size_t m = 0; m < frames; ++m) {
    // Analysis frame m starts here in the input padded by `pad` zeros.
    const auto start =
        static_cast<std::ptrdiff_t>(std::lround(static_cast<double>(m) * analysis_hop));
    for (std::size_t n = 0; n < stretch_frame; ++n) {
      const std::ptrdiff_t at =
          start + static_cast<std::ptrdiff_t>(n) - static_cast<std::ptrdiff_t>(pad);
      const double x = at >= 0 && static_cast<std::size_t>(at) < in.size()
                           ? static_cast<double>(in[static_cast<std::size_t>(at)])
                           : 0.0;
      frame.data()[n] = static_cast<float>(x * window[n]);
    }
    fft.forward(frame.data(), spectrum.data());
    float* bins_data = spectrum.data();
    const auto advance = static_cast<double>(start - previous_start);
    for (std::size_t k = 1; k < bins; ++k) {
      const double re = bins_data[2 * k];
      const double im = bins_data[2 * k + 1];
      const double magnitude = std::hypot(re, im);
      const double phase = std::atan2(im, re);
      const double omega = two_pi * static_cast<double>(k) / static_cast<double>(stretch_frame);
      if (m == 0) {
        synthesis_phase[k] = phase;
      } else {
        const double deviation = wrap_phase(phase - analysis_phase[k] - omega * advance);
        const double frequency = advance > 0.0 ? omega + deviation / advance : omega;
        synthesis_phase[k] += frequency * static_cast<double>(stretch_hop);
      }
      analysis_phase[k] = phase;
      bins_data[2 * k] = static_cast<float>(magnitude * std::cos(synthesis_phase[k]));
      bins_data[2 * k + 1] = static_cast<float>(magnitude * std::sin(synthesis_phase[k]));
    }
    previous_start = start;
    fft.inverse(spectrum.data(), frame.data());
    const std::size_t at = m * stretch_hop;
    for (std::size_t n = 0; n < stretch_frame && at + n < out.size(); ++n) {
      out[at + n] += static_cast<double>(frame.data()[n]) * window[n] * scale;
    }
  }
  std::vector<float> result(length);
  const auto keep = static_cast<std::size_t>(direct_keep_seconds * rate);
  const auto fade = static_cast<std::size_t>(direct_fade_seconds * rate);
  for (std::size_t n = 0; n < length; ++n) {
    const double stretched = n + pad < out.size() ? out[n + pad] : 0.0;
    const double original = n < in.size() ? static_cast<double>(in[n]) : 0.0;
    double weight = 1.0;
    if (n < keep) {
      weight = 0.0;
    } else if (n < keep + fade && fade > 0) {
      weight = raised_cosine(static_cast<double>(n - keep) / static_cast<double>(fade));
    }
    result[n] = static_cast<float>(original + weight * (stretched - original));
  }
  return result;
}

[[nodiscard]] std::vector<double> energy_decay(const Responses& responses) {
  std::size_t length = 0;
  for (const auto& response : responses) {
    length = std::max(length, response.size());
  }
  std::vector<double> edc(length + 1, 0.0);
  for (std::size_t i = length; i-- > 0;) {
    double power = 0.0;
    for (const auto& response : responses) {
      if (i < response.size()) {
        power += static_cast<double>(response[i]) * static_cast<double>(response[i]);
      }
    }
    edc[i] = edc[i + 1] + power;
  }
  return edc;
}

} // namespace

Responses trim_and_stretch(const Responses& source, double rate, const Trim& trim,
                           std::size_t max_frames) {
  Responses out;
  if (source.empty() || source.front().empty() || max_frames == 0) {
    return out;
  }
  const std::size_t size = source.front().size();
  const std::size_t start =
      std::min(size - 1, static_cast<std::size_t>(std::lround(trim.start_seconds * rate)));
  const std::size_t rest = size - start;
  const auto keep =
      std::clamp<std::size_t>(static_cast<std::size_t>(std::ceil(std::clamp(trim.length, 0.0, 1.0) *
                                                                 static_cast<double>(rest))),
                              1, rest);
  const bool stretched = std::fabs(trim.stretch - 1.0) > stretch_tolerance;
  // Only what can survive the cap is kept (and stretched).
  const auto cap =
      stretched
          ? static_cast<std::size_t>(std::ceil(static_cast<double>(max_frames) / trim.stretch)) + 1
          : max_frames;
  const std::size_t kept = std::min(keep, cap);
  const auto fade =
      std::min(static_cast<std::size_t>(end_fade_seconds * rate), kept / end_fade_divisor);
  for (const auto& response : source) {
    std::vector<float> part(response.begin() + static_cast<std::ptrdiff_t>(start),
                            response.begin() + static_cast<std::ptrdiff_t>(start + kept));
    if (kept < rest) {
      fade_end(part, fade);
    }
    if (stretched) {
      part = stretch_response(part, trim.stretch, max_frames, rate);
    }
    out.push_back(std::move(part));
  }
  return out;
}

double rt60(const Responses& responses, double rate) {
  const auto edc = energy_decay(responses);
  if (edc.empty() || edc.front() <= 0.0) {
    return 0.0;
  }
  const double total = edc.front();
  const auto crossing = [&](double db) -> std::optional<std::size_t> {
    const double level = total * std::pow(10.0, db / 10.0);
    for (std::size_t i = 0; i < edc.size(); ++i) {
      if (edc[i] <= level) {
        return i;
      }
    }
    return std::nullopt;
  };
  const auto start = crossing(fit_start_db);
  const auto t20 = crossing(fit_t20_db);
  if (start && t20) {
    return t20_scale * static_cast<double>(*t20 - *start) / rate;
  }
  const auto t10 = crossing(fit_t10_db);
  if (start && t10) {
    return t10_scale * static_cast<double>(*t10 - *start) / rate;
  }
  return static_cast<double>(edc.size() - 1) / rate;
}

void shape(Responses& responses, double rate, const Envelope& envelope) {
  if (responses.empty() || responses.front().empty()) {
    return;
  }
  const std::size_t size = responses.front().size();
  if (envelope.decay < 1.0) {
    const double measured = rt60(responses, rate);
    if (measured > 0.0) {
      const double rate_per_frame = ln_thousand * (1.0 / envelope.decay - 1.0) / (measured * rate);
      for (auto& response : responses) {
        for (std::size_t i = 0; i < response.size(); ++i) {
          response[i] *= static_cast<float>(std::exp(-rate_per_frame * static_cast<double>(i)));
        }
      }
    }
  }
  const auto fade_in = std::min(size, static_cast<std::size_t>(envelope.fade_in_seconds * rate));
  for (auto& response : responses) {
    for (std::size_t i = 0; i < fade_in; ++i) {
      response[i] *=
          static_cast<float>(raised_cosine(static_cast<double>(i) / static_cast<double>(fade_in)));
    }
    if (envelope.reverse) {
      std::ranges::reverse(response);
    }
  }
  // Drop the silent end.
  float peak = 0.0F;
  for (const auto& response : responses) {
    for (const float x : response) {
      peak = std::max(peak, std::fabs(x));
    }
  }
  std::size_t end = 1;
  const auto floor = static_cast<float>(tail_floor) * peak;
  for (const auto& response : responses) {
    for (std::size_t i = response.size(); i-- > end;) {
      if (std::fabs(response[i]) > floor) {
        end = i + 1;
        break;
      }
    }
  }
  for (auto& response : responses) {
    response.resize(end);
  }
  if (!envelope.normalise) {
    return;
  }
  // Each output's power from uncorrelated unit-power inputs, averaged.
  const auto routing = cast_dsp::routing_for(responses.size());
  std::array<double, 2> power{};
  for (const auto& route : cast_dsp::routes(routing)) {
    for (const float x : responses[route.response]) {
      power[route.output] += static_cast<double>(x) * static_cast<double>(x);
    }
  }
  const double mean = (power[0] + power[1]) / 2.0;
  if (mean > 0.0) {
    const auto scale = static_cast<float>(1.0 / std::sqrt(mean));
    for (auto& response : responses) {
      for (float& x : response) {
        x *= scale;
      }
    }
  }
}

Overview overview(const Responses& responses, std::size_t points) {
  Overview result{std::vector<float>(points, static_cast<float>(floor_db)),
                  std::vector<float>(points, static_cast<float>(floor_db))};
  const auto edc = energy_decay(responses);
  const std::size_t length = edc.empty() ? 0 : edc.size() - 1;
  if (length == 0 || points == 0 || edc.front() <= 0.0) {
    return result;
  }
  for (std::size_t p = 0; p < points; ++p) {
    const std::size_t begin = p * length / points;
    const std::size_t end = std::max(begin + 1, (p + 1) * length / points);
    float peak = 0.0F;
    for (const auto& response : responses) {
      for (std::size_t i = begin; i < end && i < response.size(); ++i) {
        peak = std::max(peak, std::fabs(response[i]));
      }
    }
    result.envelope[p] = static_cast<float>(native_dsp::dsp::gain_to_db(peak, floor_db));
    result.decay_curve[p] =
        static_cast<float>(native_dsp::dsp::power_to_db(edc[begin] / edc.front(), floor_db));
  }
  return result;
}

} // namespace oscillate::native_dsp::effects::cast_ir
