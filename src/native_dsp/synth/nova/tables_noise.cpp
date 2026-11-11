#include "oscillate/native_dsp/synth/nova/tables.hpp"

#include "dsp/offline_fft.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Nova's noise library (spec 08 §3, NOVA-OSC-11), generated in-house. Every
// factory noise is periodic by construction, so its loop has no seam at
// all: shaped noises are one inverse transform of a random spectrum the
// length of the loop; textures place their events modulo the loop; and
// every modulation runs a whole number of cycles per loop.
namespace oscillate::native_dsp::synth::nova {
namespace {

using Complex = std::complex<double>;
using foundation::Error;
using foundation::ErrorCode;
constexpr double pi = std::numbers::pi;
constexpr double two_pi = 2.0 * pi;

constexpr std::uint32_t noise_rate = 48'000;
// 2^18 frames, about 5.5 s at 48 kHz.
constexpr std::size_t loop_frames = std::size_t{1} << 18U;
// Loudness targets: an RMS of about -14 dBFS, never peaking past -0.2 dBFS.
constexpr double target_rms = 0.2;
constexpr double max_peak = 0.977;
// build_noise's loop crossfade, capped at a quarter of the audio.
constexpr double user_fade_seconds = 0.02;

using Random = native_dsp::dsp::Xorshift64Star;

// Two channels of one loop, in double while they are built.
struct Loop {
  std::vector<double> left = std::vector<double>(loop_frames);
  std::vector<double> right = std::vector<double>(loop_frames);
};

[[nodiscard]] double bin_hz(std::size_t bin) noexcept {
  return static_cast<double>(bin) * noise_rate / static_cast<double>(loop_frames);
}

// A modulator with a whole number of cycles per loop, so it closes the loop.
[[nodiscard]] double cycles(double count, std::size_t n, double phase = 0.0) noexcept {
  return std::sin(two_pi * count * static_cast<double>(n) / static_cast<double>(loop_frames) +
                  phase);
}

const dsp::OfflineFft& loop_transform() {
  static const dsp::OfflineFft transform(loop_frames);
  return transform;
}

// Gaussian noise with magnitude response shape(hz): a circular complex
// Gaussian spectrum, inverse transformed, gives two independent real noises
// with that spectrum (real and imaginary parts). correlation mixes the
// right channel towards the left.
template <typename Shape> Loop shaped_noise(std::uint64_t seed, double correlation, Shape&& shape) {
  Random random(seed);
  std::vector<Complex> bins(loop_frames);
  bins[0] = Complex{};
  for (std::size_t k = 1; k < loop_frames; ++k) {
    const std::size_t folded = k <= loop_frames / 2 ? k : loop_frames - k;
    const double gain = shape(bin_hz(folded));
    bins[k] = Complex(random.gaussian(), random.gaussian()) * gain;
  }
  loop_transform().inverse(bins);
  Loop loop;
  const double other = std::sqrt(std::max(0.0, 1.0 - correlation * correlation));
  for (std::size_t n = 0; n < loop_frames; ++n) {
    loop.left[n] = bins[n].real();
    loop.right[n] = correlation * bins[n].real() + other * bins[n].imag();
  }
  return loop;
}

// Normalises a loop to its loudness target and stores it.
std::shared_ptr<const NoiseSample> finish(std::string_view name, const Loop& loop) {
  double energy = 0.0;
  double peak = 1e-12;
  for (std::size_t n = 0; n < loop_frames; ++n) {
    energy += loop.left[n] * loop.left[n] + loop.right[n] * loop.right[n];
    peak = std::max({peak, std::fabs(loop.left[n]), std::fabs(loop.right[n])});
  }
  const double rms = std::sqrt(energy / (2.0 * static_cast<double>(loop_frames)));
  const double gain = std::min(rms > 1e-12 ? target_rms / rms : 1.0, max_peak / peak);
  auto out = std::make_shared<NoiseSample>();
  out->name = std::string(name);
  out->sample_rate = noise_rate;
  out->frames.resize(loop_frames);
  for (std::size_t n = 0; n < loop_frames; ++n) {
    out->frames[n] = {static_cast<float>(loop.left[n] * gain),
                      static_cast<float>(loop.right[n] * gain)};
  }
  return out;
}

// ---- Spectral shapes -------------------------------------------------------------------

// A second-order high-pass and low-pass magnitude.
[[nodiscard]] double high_pass(double hz, double corner) noexcept {
  const double r = hz / corner;
  return r * r / std::sqrt(1.0 + r * r * r * r);
}
[[nodiscard]] double low_pass(double hz, double corner) noexcept {
  const double r = hz / corner;
  return 1.0 / std::sqrt(1.0 + r * r * r * r);
}
// A resonant band of the given Q.
[[nodiscard]] double band(double hz, double centre, double q) noexcept {
  const double detune = hz / centre - centre / hz;
  return 1.0 / std::sqrt(1.0 + q * q * detune * detune);
}

// Adds a decaying oscillation (a droplet, a pop's thump) at frame `start`,
// wrapping round the loop.
void add_ring(std::vector<double>& channel, std::size_t start, double amplitude, double hz,
              double chirp, double decay_seconds) {
  const double decay = std::exp(-1.0 / (decay_seconds * noise_rate));
  const auto length = static_cast<std::size_t>(6.0 * decay_seconds * noise_rate) + 1;
  double envelope = amplitude;
  double phase = 0.0;
  double frequency = hz;
  for (std::size_t i = 0; i < length; ++i) {
    // A short attack keeps each event from clicking on its own.
    const double attack = std::min(1.0, static_cast<double>(i) / 12.0);
    channel[(start + i) % loop_frames] += envelope * attack * std::sin(phase);
    phase += two_pi * frequency / noise_rate;
    frequency *= chirp;
    envelope *= decay;
  }
}

// ---- Factory noises --------------------------------------------------------------------

Loop white() {
  return shaped_noise(1, 0.0, [](double) { return 1.0; });
}

Loop pink() {
  return shaped_noise(2, 0.0, [](double hz) { return high_pass(hz, 12.0) / std::sqrt(hz); });
}

Loop brown() {
  return shaped_noise(3, 0.0, [](double hz) { return high_pass(hz, 18.0) / hz; });
}

Loop blue() {
  return shaped_noise(4, 0.0, [](double hz) { return std::sqrt(hz) * low_pass(hz, 20'000.0); });
}

// Preamp and console hiss: a gently rising top, rolled off above 15 kHz,
// a little stereo bleed, and a faint 50 Hz hum with its odd harmonics.
Loop analog_hiss() {
  auto loop = shaped_noise(5, 0.25, [](double hz) {
    return high_pass(hz, 120.0) * std::pow(std::max(hz, 1.0) / 1'000.0, 0.18) *
           low_pass(hz, 15'000.0);
  });
  double level = 0.0;
  for (std::size_t n = 0; n < loop_frames; ++n) {
    level += loop.left[n] * loop.left[n];
  }
  const double hum = 0.025 * std::sqrt(level / static_cast<double>(loop_frames));
  // 273 cycles per loop is 49.99 Hz.
  for (std::size_t n = 0; n < loop_frames; ++n) {
    const double tone = cycles(273, n) + 0.4 * cycles(819, n, 0.7) + 0.2 * cycles(1365, n, 1.9);
    loop.left[n] += hum * tone;
    loop.right[n] += hum * tone;
  }
  return loop;
}

// Tape hiss: mid-heavy with a presence lift, breathing with slow wow and
// faster flutter of the modulation noise.
Loop tape() {
  auto loop = shaped_noise(6, 0.55, [](double hz) {
    return high_pass(hz, 250.0) * low_pass(hz, 11'000.0) * (1.0 + 0.8 * band(hz, 3'200.0, 0.8));
  });
  for (std::size_t n = 0; n < loop_frames; ++n) {
    const double wobble =
        1.0 + 0.12 * cycles(2, n) + 0.05 * cycles(33, n, 1.3) + 0.03 * cycles(71, n, 0.4);
    loop.left[n] *= wobble;
    loop.right[n] *= wobble;
  }
  return loop;
}

// A record's run-out: soft surface hiss, rumble turning with the platter,
// a crackle of small ticks and the odd pop with a thump.
Loop vinyl() {
  auto loop = shaped_noise(7, 0.7, [](double hz) {
    return 0.12 * high_pass(hz, 150.0) * low_pass(hz, 9'000.0) / std::pow(hz, 0.25);
  });
  const auto rumble = shaped_noise(8, 0.9, [](double hz) {
    return high_pass(hz, 8.0) * low_pass(hz, 45.0) / std::max(hz, 1.0);
  });
  double rumble_energy = 0.0;
  double hiss_energy = 0.0;
  for (std::size_t n = 0; n < loop_frames; ++n) {
    rumble_energy += rumble.left[n] * rumble.left[n];
    hiss_energy += loop.left[n] * loop.left[n];
  }
  const double rumble_gain = 0.8 * std::sqrt(hiss_energy / std::max(rumble_energy, 1e-30));
  for (std::size_t n = 0; n < loop_frames; ++n) {
    // Three turns per loop: 33 1/3 rpm.
    const double turn = 1.0 + 0.5 * cycles(3, n);
    loop.left[n] += rumble_gain * turn * rumble.left[n];
    loop.right[n] += rumble_gain * turn * rumble.right[n];
  }
  const double hiss = std::sqrt(hiss_energy / static_cast<double>(loop_frames));
  Random random(70);
  // Ticks: about 40 a second, mostly tiny, a few loud; a short
  // band-limited burst of alternating sign.
  const auto ticks = static_cast<std::size_t>(40.0 * static_cast<double>(loop_frames) / noise_rate);
  for (std::size_t i = 0; i < ticks; ++i) {
    const std::size_t at = random.index(loop_frames);
    const double size = hiss * (1.5 + 14.0 * std::pow(random.uniform(), 6.0));
    const double polarity = random.uniform() < 0.5 ? -1.0 : 1.0;
    const double pan = random.uniform();
    const std::size_t width = 2 + random.index(5);
    for (std::size_t k = 0; k < width; ++k) {
      const double shape = (k % 2 == 0 ? 1.0 : -0.6) * std::exp(-static_cast<double>(k) * 0.7);
      const double value = polarity * size * shape;
      loop.left[(at + k) % loop_frames] += value * std::sqrt(1.0 - pan);
      loop.right[(at + k) % loop_frames] += value * std::sqrt(pan);
    }
  }
  // Pops: about one a second, a sharp click over a low thump.
  const auto pops = static_cast<std::size_t>(1.1 * static_cast<double>(loop_frames) / noise_rate);
  for (std::size_t i = 0; i < pops; ++i) {
    const std::size_t at = random.index(loop_frames);
    const double size = hiss * (20.0 + 25.0 * random.uniform());
    const double pan = 0.3 + 0.4 * random.uniform();
    const double hz = 120.0 + 260.0 * random.uniform();
    for (auto* channel : {&loop.left, &loop.right}) {
      const double side = channel == &loop.left ? std::sqrt(1.0 - pan) : std::sqrt(pan);
      add_ring(*channel, at, size * side, hz, 0.9995, 0.004);
      (*channel)[at % loop_frames] += 0.8 * size * side;
      (*channel)[(at + 1) % loop_frames] -= 0.5 * size * side;
    }
  }
  return loop;
}

// Breath: an airy, formant-coloured noise drawn in and let out twice per
// loop; the in-breath is brighter and softer.
Loop breath() {
  const auto out_breath = shaped_noise(9, 0.6, [](double hz) {
    return high_pass(hz, 200.0) * low_pass(hz, 9'000.0) *
           (0.25 + band(hz, 750.0, 1.5) + 0.8 * band(hz, 1'700.0, 2.0) +
            0.5 * band(hz, 3'400.0, 2.5));
  });
  const auto in_breath = shaped_noise(10, 0.6, [](double hz) {
    return high_pass(hz, 600.0) * low_pass(hz, 14'000.0) *
           (0.2 + 0.7 * band(hz, 2'400.0, 1.5) + band(hz, 5'500.0, 2.0));
  });
  Loop loop;
  for (std::size_t n = 0; n < loop_frames; ++n) {
    // Two breaths per loop: in over the first 40 %, out over the next 60 %.
    const double phase =
        std::fmod(2.0 * static_cast<double>(n) / static_cast<double>(loop_frames), 1.0);
    const double in = phase < 0.4 ? std::pow(std::sin(pi * phase / 0.4), 2.0) : 0.0;
    const double out = phase >= 0.4 ? std::pow(std::sin(pi * (phase - 0.4) / 0.6), 1.5) : 0.0;
    // Never fully silent: a breath's floor of air.
    loop.left[n] =
        0.05 * out_breath.left[n] + 0.55 * in * in_breath.left[n] + out * out_breath.left[n];
    loop.right[n] =
        0.05 * out_breath.right[n] + 0.55 * in * in_breath.right[n] + out * out_breath.right[n];
  }
  return loop;
}

// Wind: a low roar, a mid rush and three whistling bands, each gusting on
// its own slow cycle.
Loop wind() {
  const auto roar = shaped_noise(11, 0.5, [](double hz) {
    return high_pass(hz, 25.0) * low_pass(hz, 400.0) / std::sqrt(std::max(hz, 1.0));
  });
  const auto rush = shaped_noise(
      12, 0.3, [](double hz) { return 0.06 * high_pass(hz, 300.0) * low_pass(hz, 3'500.0); });
  std::array<Loop, 3> whistles{
      shaped_noise(13, 0.0, [](double hz) { return 0.15 * band(hz, 620.0, 25.0); }),
      shaped_noise(14, 0.0, [](double hz) { return 0.12 * band(hz, 910.0, 30.0); }),
      shaped_noise(15, 0.0, [](double hz) { return 0.1 * band(hz, 1'330.0, 35.0); })};
  Loop loop;
  for (std::size_t n = 0; n < loop_frames; ++n) {
    const double gust = 0.6 + 0.25 * cycles(1, n, 0.3) + 0.15 * cycles(3, n, 2.1);
    const double swirl = 0.5 + 0.5 * cycles(2, n, 1.0);
    loop.left[n] = gust * (roar.left[n] + rush.left[n]);
    loop.right[n] = gust * (roar.right[n] + rush.right[n]);
    for (std::size_t w = 0; w < whistles.size(); ++w) {
      const double level = std::pow(
          std::max(0.0, cycles(static_cast<double>(w + 2), n, 1.7 * static_cast<double>(w))), 2.0);
      loop.left[n] += gust * level * swirl * whistles[w].left[n];
      loop.right[n] += gust * level * (1.0 - swirl) * whistles[w].right[n];
    }
  }
  return loop;
}

// Rain: a soft wash under hundreds of droplets, each a rising bubble ring.
Loop rain() {
  auto loop = shaped_noise(16, 0.2, [](double hz) {
    return high_pass(hz, 400.0) * low_pass(hz, 10'000.0) / std::pow(hz, 0.3);
  });
  double wash = 0.0;
  for (std::size_t n = 0; n < loop_frames; ++n) {
    wash += loop.left[n] * loop.left[n];
  }
  const double level = std::sqrt(wash / static_cast<double>(loop_frames));
  Random random(160);
  const auto drops =
      static_cast<std::size_t>(350.0 * static_cast<double>(loop_frames) / noise_rate);
  for (std::size_t i = 0; i < drops; ++i) {
    const std::size_t at = random.index(loop_frames);
    const double size = level * (0.6 + 6.0 * std::pow(random.uniform(), 4.0));
    const double hz = 1'400.0 * std::exp2(2.2 * random.uniform());
    const double decay = 0.002 + 0.005 * random.uniform();
    const double pan = random.uniform();
    add_ring(loop.left, at, size * std::sqrt(1.0 - pan), hz, 1.00004, decay);
    add_ring(loop.right, at, size * std::sqrt(pan), hz, 1.00004, decay);
  }
  return loop;
}

// Metal shimmer: noise through the inharmonic modes of a struck plate, two
// sets of mode levels trading places so the metal glints and moves.
Loop metal_shimmer() {
  constexpr std::array<double, 12> modes{1.0,  1.59, 2.14, 2.30, 2.65, 2.92,
                                         3.16, 3.50, 4.15, 4.60, 5.18, 5.94};
  constexpr double base_hz = 1'150.0;
  const auto bank = [&](std::uint64_t seed) {
    Random levels(seed + 1'000U);
    std::array<double, modes.size()> gain{};
    for (auto& g : gain) {
      g = 0.2 + 0.8 * levels.uniform();
    }
    return shaped_noise(seed, 0.0, [&](double hz) {
      double sum = 0.0;
      for (std::size_t m = 0; m < modes.size(); ++m) {
        sum += gain[m] * band(hz, base_hz * modes[m], 180.0) / std::sqrt(modes[m]);
      }
      return sum;
    });
  };
  const auto first = bank(17);
  const auto second = bank(18);
  Loop loop;
  for (std::size_t n = 0; n < loop_frames; ++n) {
    const double mix = 0.5 + 0.5 * cycles(3, n);
    const double glint = 0.5 + 0.5 * cycles(5, n, 1.2);
    loop.left[n] = std::sqrt(mix) * first.left[n] + std::sqrt(1.0 - mix) * second.left[n];
    loop.right[n] = std::sqrt(glint) * first.right[n] + std::sqrt(1.0 - glint) * second.right[n];
  }
  return loop;
}

// Digital crush: white noise held and bit-reduced, the hold and depth
// changing every 4096 frames (each hold divides the region, so the loop
// stays whole).
Loop digital_crush() {
  const auto source = shaped_noise(19, 0.0, [](double) { return 1.0; });
  constexpr std::size_t region = 4'096;
  constexpr std::array<std::size_t, 7> holds{1, 2, 4, 8, 16, 32, 64};
  Random random(190);
  Loop loop;
  double scale = 0.0;
  for (std::size_t n = 0; n < loop_frames; ++n) {
    scale = std::max(scale, std::fabs(source.left[n]));
  }
  const double unit = 1.0 / std::max(scale, 1e-12);
  for (std::size_t start = 0; start < loop_frames; start += region) {
    const std::size_t hold_left = holds[random.index(holds.size())];
    const std::size_t hold_right = holds[random.index(holds.size())];
    const double levels = std::exp2(2.0 + static_cast<double>(random.index(5)));
    for (std::size_t n = start; n < start + region; ++n) {
      const std::size_t held_left = n - (n - start) % hold_left;
      const std::size_t held_right = n - (n - start) % hold_right;
      loop.left[n] = std::round(source.left[held_left] * unit * levels) / levels;
      loop.right[n] = std::round(source.right[held_right] * unit * levels) / levels;
    }
  }
  return loop;
}

using Generator = Loop (*)();
struct Factory {
  std::string_view name;
  Generator generate;
};

constexpr std::array<Factory, 12> factories{{
    {"white", white},
    {"pink", pink},
    {"brown", brown},
    {"blue", blue},
    {"analog_hiss", analog_hiss},
    {"tape", tape},
    {"vinyl", vinyl},
    {"breath", breath},
    {"wind", wind},
    {"rain", rain},
    {"metal_shimmer", metal_shimmer},
    {"digital_crush", digital_crush},
}};

constexpr auto names = [] {
  std::array<std::string_view, factories.size()> out{};
  for (std::size_t i = 0; i < factories.size(); ++i) {
    out[i] = factories[i].name;
  }
  return out;
}();

struct Cache {
  std::array<std::once_flag, factories.size()> once;
  std::array<std::shared_ptr<const NoiseSample>, factories.size()> noises;
};

Cache& cache() {
  static Cache instance;
  return instance;
}

} // namespace

std::span<const std::string_view> builtin_noise_names() noexcept { return names; }

std::shared_ptr<const NoiseSample> builtin_noise(std::string_view name) {
  const auto found = std::ranges::find(names, name);
  if (found == names.end()) {
    return nullptr;
  }
  const auto index = static_cast<std::size_t>(found - names.begin());
  auto& c = cache();
  std::call_once(c.once[index], [&] {
    c.noises[index] = finish(factories[index].name, factories[index].generate());
  });
  return c.noises[index];
}

// The loop is cut short by the fade length; the cut-off tail is faded into
// the head with equal power, so the last frame runs straight on into the
// first (frame 0 is the first tail frame) and uncorrelated noise keeps its
// level through the fade.
foundation::Result<std::shared_ptr<const NoiseSample>> build_noise(const SynthMediaAsset& asset,
                                                                   std::string name) {
  using NoiseResult = foundation::Result<std::shared_ptr<const NoiseSample>>;
  if (asset.source_rate == 0) {
    return NoiseResult::failure(
        Error{ErrorCode::invalid_argument, "noise '" + name + "' has no sample rate"});
  }
  if (asset.frames.size() < 2) {
    return NoiseResult::failure(
        Error{ErrorCode::invalid_argument, "noise '" + name + "' is empty"});
  }
  const bool finite = std::ranges::all_of(asset.frames, [](const auto& frame) {
    return std::isfinite(frame.left) && std::isfinite(frame.right);
  });
  if (!finite) {
    return NoiseResult::failure(
        Error{ErrorCode::corrupt_data, "noise '" + name + "' audio is not finite"});
  }
  const std::size_t count = asset.frames.size();
  const auto fade = std::min(
      static_cast<std::size_t>(std::lround(user_fade_seconds * asset.source_rate)), count / 4);
  auto out = std::make_shared<NoiseSample>();
  out->name = std::move(name);
  out->sample_rate = asset.source_rate;
  const std::size_t length = count - fade;
  out->frames.assign(asset.frames.begin(),
                     asset.frames.begin() + static_cast<std::ptrdiff_t>(length));
  for (std::size_t i = 0; i < fade; ++i) {
    const double angle = 0.5 * pi * static_cast<double>(i) / static_cast<double>(fade);
    const auto head = static_cast<float>(std::sin(angle));
    const auto tail = static_cast<float>(std::cos(angle));
    const auto& from_tail = asset.frames[length + i];
    auto& frame = out->frames[i];
    frame.left = frame.left * head + from_tail.left * tail;
    frame.right = frame.right * head + from_tail.right * tail;
  }
  return NoiseResult::success(std::move(out));
}

} // namespace oscillate::native_dsp::synth::nova
