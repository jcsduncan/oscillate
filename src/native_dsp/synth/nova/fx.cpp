#include "oscillate/native_dsp/synth/nova/fx.hpp"

#include "fx_modules.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/warp_dsp/real_fft.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <new>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Nova's effect rack (spec 08 §7; contract 08a §2.9, §2.10, §3.3, §5, §6).
namespace oscillate::native_dsp::synth::nova {

using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;

namespace {

constexpr std::array<std::string_view, fx_kind_count> kind_ids{
    "distortion", "filter", "chorus",    "flanger", "phaser",  "delay",    "reverb",  "compressor",
    "multiband",  "eq",     "dimension", "hyper",   "utility", "convolve", "shifter", "flyby"};

constexpr std::array<std::string_view, fx_chain_count> chain_ids{"fx.main", "fx.bus.1", "fx.bus.2"};

} // namespace

std::string_view fx_kind_id(FxKind kind) noexcept {
  const auto index = static_cast<std::size_t>(kind);
  return index < kind_ids.size() ? kind_ids[index] : std::string_view{};
}

std::optional<FxKind> fx_kind_from_id(std::string_view id) noexcept {
  for (std::size_t i = 0; i < kind_ids.size(); ++i) {
    if (kind_ids[i] == id) {
      return static_cast<FxKind>(i);
    }
  }
  return std::nullopt;
}

FxDefaults fx_defaults(FxKind kind) noexcept {
  switch (kind) {
  case FxKind::distortion:
    return {{0.3, 0.5, 0.5, 0, 0, 0, 0, 0}, 1.0};
  case FxKind::filter:
    return {{0.6, 0.2, 0, 0, 0, 0, 0, 0}, 1.0};
  case FxKind::chorus:
    return {{0.45, 0.4, 0, 0.3, 0, 0, 0, 0}, 0.5};
  case FxKind::flanger:
    return {{0.35, 0.5, 0.7, 0, 0, 0, 0, 0}, 0.5};
  case FxKind::phaser:
    return {{0.35, 0.6, 0.4, 0.5, 0, 0, 0, 0.5}, 0.5};
  case FxKind::delay:
    return {{8.0 / 14.0, 0.4, 0.5, 0, 0, 1, 0, 0.5}, 0.25};
  case FxKind::reverb:
    return {{0.6, 0.5, 0.4, 0.1, 0, 0, 0, 0}, 0.25};
  case FxKind::compressor:
    return {{0.7, 0.4, 0.5, 0.5, 0, 0, 0, 0}, 1.0};
  case FxKind::multiband:
    return {{0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0, 0}, 1.0};
  case FxKind::eq:
    return {fx_detail::eq_defaults(), 1.0};
  case FxKind::dimension:
    return {{0.5, 0.7, 0.3, 0, 0, 0, 0, 0}, 0.4};
  case FxKind::hyper:
    return {{0.3, 0.4, 0.8, 0, 0, 0, 0, 0.6}, 0.5};
  case FxKind::utility:
    return {{0.5, 0.5, 0, 0.5, 0, 0, 0, 0}, 1.0};
  case FxKind::convolve:
    return {{0, 0, 1, 0, 0, 0, 0, 0}, 0.3};
  case FxKind::shifter:
    return {{0.6, 0, 0, 0, 0, 0, 0, 0}, 0.5};
  case FxKind::flyby:
    // Left to right, 14 m in front, 90 km/h, looping; 2 Bars when synced;
    // the physics at 100%; a third of a room.
    return {{0, std::log(14.0) / std::log(60.0), std::log(90.0) / std::log(1'000.0), 0, 0.5, 0, 0.5,
             0, 0.25, 0.35, 0},
            1.0};
  }
  return {};
}

Result<std::unique_ptr<FxModule>> make_fx_module(FxKind kind, double sample_rate,
                                                 std::size_t max_frames,
                                                 std::shared_ptr<const SynthMediaAsset> ir) {
  using R = Result<std::unique_ptr<FxModule>>;
  if (!std::isfinite(sample_rate) || sample_rate < 8'000.0 || sample_rate > 384'000.0 ||
      max_frames == 0 || max_frames > 65'536) {
    return R::failure({ErrorCode::invalid_argument, "FX module needs a valid rate and block"});
  }
  if (static_cast<std::size_t>(kind) >= fx_kind_count) {
    return R::failure({ErrorCode::invalid_argument, "unknown FX module kind"});
  }
  std::unique_ptr<FxModule> module;
  try {
    switch (kind) {
    case FxKind::distortion:
      module = fx_detail::make_distortion(sample_rate, max_frames);
      break;
    case FxKind::filter:
      module = fx_detail::make_filter(sample_rate, max_frames);
      break;
    case FxKind::chorus:
      module = fx_detail::make_chorus(sample_rate, max_frames);
      break;
    case FxKind::flanger:
      module = fx_detail::make_flanger(sample_rate, max_frames);
      break;
    case FxKind::phaser:
      module = fx_detail::make_phaser(sample_rate, max_frames);
      break;
    case FxKind::delay:
      module = fx_detail::make_delay(sample_rate, max_frames);
      break;
    case FxKind::reverb:
      module = fx_detail::make_reverb(sample_rate, max_frames);
      break;
    case FxKind::compressor:
      module = fx_detail::make_compressor(sample_rate, max_frames);
      break;
    case FxKind::multiband:
      module = fx_detail::make_multiband(sample_rate, max_frames);
      break;
    case FxKind::eq:
      module = fx_detail::make_eq(sample_rate, max_frames);
      break;
    case FxKind::dimension:
      module = fx_detail::make_dimension(sample_rate, max_frames);
      break;
    case FxKind::hyper:
      module = fx_detail::make_hyper(sample_rate, max_frames);
      break;
    case FxKind::utility:
      module = fx_detail::make_utility(sample_rate, max_frames);
      break;
    case FxKind::convolve:
      module = fx_detail::make_convolve(sample_rate, max_frames, std::move(ir), std::string{});
      break;
    case FxKind::shifter:
      module = fx_detail::make_shifter(sample_rate, max_frames);
      break;
    case FxKind::flyby:
      module = fx_detail::make_flyby(sample_rate, max_frames);
      break;
    }
  } catch (const std::bad_alloc&) {
    return R::failure({ErrorCode::resource_exhausted, "FX module allocation failed"});
  }
  if (!module) {
    return R::failure({ErrorCode::internal, "FX module kernel could not be made"});
  }
  return R::success(std::move(module));
}

// ---- Layout -------------------------------------------------------------------------

Result<std::shared_ptr<const FxLayout>>
build_fx_layout(const project::NativeSynthDocument& document, double sample_rate,
                std::size_t max_frames, const SynthMedia& media, const FxLayout* previous) {
  using R = Result<std::shared_ptr<const FxLayout>>;
  auto layout = std::make_shared<FxLayout>();
  std::array<bool, fx_slot_uids + 1> used{};
  std::array<bool, fx_chain_count> seen{};
  for (const auto& chain : document.chains) {
    const auto at = std::ranges::find(chain_ids, chain.id);
    if (at == chain_ids.end()) {
      return R::failure({ErrorCode::invalid_argument, "unknown FX chain id: " + chain.id});
    }
    const auto c = static_cast<std::size_t>(at - chain_ids.begin());
    if (seen[c]) {
      return R::failure({ErrorCode::invalid_argument, "duplicate FX chain: " + chain.id});
    }
    seen[c] = true;
    if (chain.modules.size() > fx_max_modules_per_chain) {
      return R::failure({ErrorCode::invalid_argument, "too many modules in " + chain.id});
    }
    for (const auto& slot : chain.modules) {
      if (slot.uid < 1 || slot.uid > fx_slot_uids || used[slot.uid]) {
        return R::failure(
            {ErrorCode::invalid_argument, "FX slot uid out of range or repeated in " + chain.id});
      }
      used[slot.uid] = true;
      const auto kind = fx_kind_from_id(slot.module);
      if (!kind) {
        return R::failure({ErrorCode::invalid_argument, "unknown FX module: " + slot.module});
      }
      std::shared_ptr<const SynthMediaAsset> ir;
      std::string identity;
      if (*kind == FxKind::convolve) {
        const std::string slot_id = "fx." + std::to_string(slot.uid) + ".ir";
        for (const auto& entry : media) {
          if (entry.slot == slot_id) {
            ir = entry.asset;
            identity = entry.sha256;
          }
        }
      }
      std::shared_ptr<FxModule> module;
      if (previous != nullptr) {
        for (const auto& old_chain : previous->chains) {
          for (const auto& old : old_chain) {
            if (old.uid == slot.uid && old.kind == *kind && old.module &&
                (*kind != FxKind::convolve ||
                 fx_detail::convolve_identity(*old.module) == identity)) {
              module = old.module;
            }
          }
        }
      }
      if (!module) {
        if (*kind == FxKind::convolve) {
          if (!std::isfinite(sample_rate) || sample_rate < 8'000.0 || sample_rate > 384'000.0 ||
              max_frames == 0 || max_frames > 65'536) {
            return R::failure({ErrorCode::invalid_argument, "FX module needs a valid rate"});
          }
          try {
            module = fx_detail::make_convolve(sample_rate, max_frames, ir, identity);
          } catch (const std::bad_alloc&) {
            return R::failure({ErrorCode::resource_exhausted, "convolve allocation failed"});
          }
        } else {
          auto made = make_fx_module(*kind, sample_rate, max_frames, nullptr);
          if (!made) {
            return R::failure(made.error());
          }
          module = std::move(made.value());
        }
      }
      layout->chains[c].push_back({slot.uid, *kind, std::move(module)});
    }
  }
  if (previous != nullptr) {
    for (const auto& old_chain : previous->chains) {
      for (const auto& old : old_chain) {
        bool kept = false;
        for (const auto& chain : layout->chains) {
          for (const auto& slot : chain) {
            kept = kept || slot.module == old.module;
          }
        }
        if (!kept && old.module) {
          layout->retiring.push_back(old);
        }
      }
    }
  }
  return R::success(std::shared_ptr<const FxLayout>(std::move(layout)));
}

// ---- Rack ---------------------------------------------------------------------------

namespace {

using fx_detail::Glide;
using fx_detail::Resampler;
using Frame = engine::StereoFrame;

constexpr std::size_t max_runs = 2 * fx_max_modules_per_chain;
// Slot meters fall this fast, like the bus meters.
constexpr double meter_fall_db_per_second = 24.0;
// Slot spectra (08a §6): each placed slot's output is analysed this often
// over a window this long at 48 kHz (longer at higher rates, so the
// resolution in hertz stays), and falls this fast between peaks.
constexpr double scope_hz = 30.0;
constexpr std::size_t scope_window_48k = 1'024;
constexpr double scope_reference_rate = 48'000.0;
constexpr double scope_fall_db_per_second = 36.0;
constexpr double scope_low_hz = 20.0;
constexpr double scope_span = 1'000.0; // 20 Hz to 20 kHz.
constexpr float floor_db = -120.0F;
constexpr double fade_seconds = 0.02;
constexpr double duck_seconds = 0.005;
constexpr std::uint32_t split_interval = 16;
constexpr std::size_t main_chain = 0;

[[nodiscard]] std::uint32_t valid_factor(std::uint32_t factor) noexcept {
  return factor == 4U ? 4U : (factor == 2U ? 2U : 1U);
}

// One placed module as the rack runs it: its instance (null while only a
// removed module fades out), an entry fade for a newly added module, and
// the instance it replaced or that left, fading out in parallel on the
// same input.
struct Run {
  FxModule* module = nullptr;
  std::uint32_t uid = 0;
  Glide entry;
  FxModule* old = nullptr;
  Glide old_weight;
  double old_mix = 0.0;
  bool asleep = false;
};

struct Chain {
  std::array<Run, max_runs> runs{};
  std::size_t count = 0;
  std::array<Run, max_runs> pending{};
  std::size_t pending_count = 0;
  bool has_pending = false;
  bool ducked = false;
  std::uint32_t factor = 1;
  std::uint32_t requested = 1;
  Resampler io;  // Up into the chain, down out of it.
  Resampler pre; // Main only: the pre-module tap, down.
  std::vector<Frame> buffer;
};

struct Slot {
  Glide mix;
  Glide on;
  bool on_target = true;
};

void fresh_run(Run& run, FxModule* module, std::uint32_t uid) noexcept {
  run = Run{};
  run.module = module;
  run.uid = uid;
  run.entry.jump(1.0);
  run.old_weight.jump(0.0);
}

} // namespace

struct FxRack::State {
  double rate = 48'000.0;
  std::size_t max_frames = 0;
  std::array<Chain, fx_chain_count> chains{};
  std::array<Slot, fx_slot_uids> slots{};
  FxRackSettings settings{};
  double beats_per_second = 2.0;
  bool has_settings = false;
  bool adopted = false;

  // Engine-rate buffers.
  std::vector<Frame> main_out;
  std::vector<Frame> pre_out;
  std::array<std::vector<Frame>, 2> send;
  std::array<std::vector<Frame>, 2> bus_out;
  std::vector<double> duck_gain;
  // Chain-rate scratch.
  std::vector<Frame> dry;
  std::vector<Frame> old;
  std::vector<Frame> low;
  std::vector<Frame> pre_high;

  // Split.
  fx_detail::Lr4 split;
  SvfCoefficients split_c{};
  Glide split_weight;
  Glide split_octave;
  bool split_target = false;
  std::uint32_t split_tick = 0;

  // Sends and returns.
  std::array<Glide, 2> send_level{};
  std::array<Glide, 2> post_weight{};
  std::array<bool, 2> post_target{true, true};
  std::array<Glide, 2> return_gain{};

  // Phase compensation: Main through both buses' resamplers, each bus
  // through the other's, so every path into the sum has the same phase.
  std::array<Resampler, 2> main_compensation{};
  std::array<Resampler, 2> bus_compensation{};

  // Structural ducks.
  enum class Duck : std::uint8_t { none, out, in };
  Duck duck = Duck::none;
  Glide duck_level;
  bool silence = false;
  std::uint32_t duck_frames = 240;

  // Meters: L and R of Main, Bus 1, Bus 2.
  std::array<double, 6> peaks{};
  double peak_fall = 0.9999;

  // Per slot uid: peaks in and out, and the output spectrum.
  struct Scope {
    std::vector<float> ring;
    std::size_t write = 0;
    std::int64_t countdown = 0;
    bool fed = false;
    std::array<float, fx_spectrum_bands> bands{};
  };
  std::array<double, fx_slot_uids> slot_in{};
  std::array<double, fx_slot_uids> slot_out{};
  std::array<Scope, fx_slot_uids> scopes{};
  std::unique_ptr<warp_dsp::RealFft> scope_fft;
  warp_dsp::AlignedBuffer scope_buffer;
  std::vector<double> scope_window;
  double scope_gain = 1.0; // Bin magnitude to a full-scale sine's amplitude.
  std::int64_t scope_interval = 1'600;
  float scope_fall_db = 1.2F;
  struct BandBins {
    std::size_t first = 0; // Bins [first, end) lie in the band...
    std::size_t end = 0;
    double centre = 0.0; // ...else read between bins at its centre.
  };
  std::array<BandBins, fx_spectrum_bands> band_bins{};

  void prepare_scopes() {
    std::size_t size = scope_window_48k;
    while (static_cast<double>(size) * scope_reference_rate <
           static_cast<double>(scope_window_48k) * rate * 0.75) {
      size *= 2;
    }
    scope_fft = std::make_unique<warp_dsp::RealFft>(size);
    scope_buffer = warp_dsp::AlignedBuffer(size);
    scope_window.resize(size);
    double sum = 0.0;
    for (std::size_t i = 0; i < size; ++i) {
      scope_window[i] =
          oscillate::native_dsp::dsp::hann(static_cast<double>(i), static_cast<double>(size));
      sum += scope_window[i];
    }
    scope_gain = 2.0 / sum;
    scope_interval = std::max<std::int64_t>(1, std::llround(rate / scope_hz));
    scope_fall_db = static_cast<float>(scope_fall_db_per_second / scope_hz);
    const double bin_hz = rate / static_cast<double>(size);
    for (std::size_t b = 0; b < fx_spectrum_bands; ++b) {
      const double n = static_cast<double>(fx_spectrum_bands);
      const double lo = scope_low_hz * std::pow(scope_span, static_cast<double>(b) / n);
      const double hi = scope_low_hz * std::pow(scope_span, static_cast<double>(b + 1) / n);
      const double mid = scope_low_hz * std::pow(scope_span, (static_cast<double>(b) + 0.5) / n);
      auto& bins = band_bins[b];
      bins.first = static_cast<std::size_t>(std::ceil(lo / bin_hz));
      bins.end = std::min(size / 2, static_cast<std::size_t>(std::ceil(hi / bin_hz)));
      bins.centre = mid / bin_hz;
    }
    for (std::size_t u = 0; u < fx_slot_uids; ++u) {
      auto& scope = scopes[u];
      scope.ring.assign(size, 0.0F);
      scope.write = 0;
      // Staggered, so the slots' analyses fall in different blocks.
      scope.countdown = scope_interval * static_cast<std::int64_t>(u + 1) /
                        static_cast<std::int64_t>(fx_slot_uids);
      scope.fed = false;
      scope.bands.fill(floor_db);
    }
  }

  // The newest window of a slot's output to its bands, falling smoothly.
  void analyse(Scope& scope) noexcept {
    const std::size_t size = scope.ring.size();
    for (std::size_t i = 0; i < size; ++i) {
      const float x = scope.ring[(scope.write + i) % size];
      scope_buffer[i] = static_cast<float>(static_cast<double>(x) * scope_window[i]);
    }
    scope_fft->forward(scope_buffer.data(), scope_buffer.data());
    const auto magnitude = [&](std::size_t k) {
      return std::sqrt(static_cast<double>(
                 warp_dsp::bin_power(scope_buffer.data(), size, std::min(k, size / 2)))) *
             scope_gain;
    };
    for (std::size_t b = 0; b < fx_spectrum_bands; ++b) {
      const auto& bins = band_bins[b];
      double level = 0.0;
      if (bins.first < bins.end) {
        for (std::size_t k = bins.first; k < bins.end; ++k) {
          level = std::max(level, magnitude(k));
        }
      } else {
        const auto k = static_cast<std::size_t>(bins.centre);
        const double t = bins.centre - static_cast<double>(k);
        level = magnitude(k) + t * (magnitude(k + 1) - magnitude(k));
      }
      const auto db = static_cast<float>(std::max<double>(floor_db, fx_detail::gain_to_db(level)));
      scope.bands[b] = std::max(db, std::max(floor_db, scope.bands[b] - scope_fall_db));
    }
  }

  // After a chunk of `frames` engine frames: analyse the slots that are due.
  void run_scopes(std::size_t frames) noexcept {
    const double fall = std::pow(peak_fall, static_cast<double>(frames));
    for (std::size_t u = 0; u < fx_slot_uids; ++u) {
      auto& scope = scopes[u];
      if (!scope.fed) {
        slot_in[u] *= fall;
        slot_out[u] *= fall;
        continue;
      }
      scope.fed = false;
      scope.countdown -= static_cast<std::int64_t>(frames);
      if (scope.countdown <= 0) {
        scope.countdown += scope_interval;
        analyse(scope);
      }
    }
  }

  [[nodiscard]] double chain_rate(std::size_t c) const noexcept {
    return rate * static_cast<double>(chains[c].factor);
  }
  [[nodiscard]] FxContext context(std::size_t c) const noexcept {
    return {beats_per_second, chain_rate(c)};
  }
  [[nodiscard]] std::uint32_t fade_frames(std::size_t c) const noexcept {
    return static_cast<std::uint32_t>(std::lround(fade_seconds * chain_rate(c)));
  }
  [[nodiscard]] double effective_mix(const Run& run) const noexcept {
    const auto& slot = slots[run.uid - 1];
    return slot.mix.value * slot.on.value * run.entry.value;
  }

  // Sends the last settings to a module at its chain's rate, at once.
  void prime(FxModule& module, std::uint32_t uid, std::size_t c) noexcept {
    if (has_settings) {
      module.set(settings.slots[uid - 1].p, context(c), 0);
    }
  }

  void apply_factors() noexcept {
    for (std::size_t c = 0; c < fx_chain_count; ++c) {
      auto& chain = chains[c];
      if (chain.requested == chain.factor) {
        continue;
      }
      chain.factor = chain.requested;
      chain.io.set_factor(chain.factor);
      chain.pre.set_factor(chain.factor);
      if (c == main_chain) {
        split.reset();
        split_tick = 0;
      }
      for (std::size_t r = 0; r < chain.count; ++r) {
        if (chain.runs[r].module != nullptr) {
          prime(*chain.runs[r].module, chain.runs[r].uid, c);
        }
      }
    }
    main_compensation[0].set_factor(chains[1].factor);
    main_compensation[1].set_factor(chains[2].factor);
    bus_compensation[0].set_factor(chains[2].factor);
    bus_compensation[1].set_factor(chains[1].factor);
  }

  // The bottom of a duck: install pending orders and chain factors.
  void switch_structure() noexcept {
    for (std::size_t c = 0; c < fx_chain_count; ++c) {
      auto& chain = chains[c];
      if (chain.has_pending) {
        chain.runs = chain.pending;
        chain.count = chain.pending_count;
        chain.has_pending = false;
        chain.pending_count = 0;
      }
    }
    apply_factors();
  }

  void start_duck() noexcept {
    if (duck != Duck::out) {
      duck = Duck::out;
      duck_level.to(0.0, static_cast<std::uint32_t>(
                             std::lround(duck_level.value * static_cast<double>(duck_frames))));
    }
  }

  void compact(Chain& chain) noexcept {
    std::size_t k = 0;
    for (std::size_t r = 0; r < chain.count; ++r) {
      if (chain.runs[r].module != nullptr || chain.runs[r].old != nullptr) {
        chain.runs[k++] = chain.runs[r];
      }
    }
    chain.count = k;
  }

  // A slot's input and output peaks over this chunk, and its output into
  // the scope at the engine's rate (every factor-th frame).
  void watch(std::uint32_t uid, std::span<const Frame> in, std::span<const Frame> out,
             std::uint32_t factor) noexcept {
    const std::size_t u = uid - 1;
    const std::size_t steps = out.size() / factor; // Whole engine-rate frames.
    const double fall = std::pow(peak_fall, static_cast<double>(steps));
    double in_peak = 0.0;
    double out_peak = 0.0;
    for (std::size_t i = 0; i < out.size(); ++i) {
      in_peak = std::max({in_peak, std::fabs(static_cast<double>(in[i].left)),
                          std::fabs(static_cast<double>(in[i].right))});
      out_peak = std::max({out_peak, std::fabs(static_cast<double>(out[i].left)),
                           std::fabs(static_cast<double>(out[i].right))});
    }
    slot_in[u] = std::max(slot_in[u] * fall, in_peak);
    slot_out[u] = std::max(slot_out[u] * fall, out_peak);
    auto& scope = scopes[u];
    if (scope.ring.empty()) {
      return;
    }
    const std::size_t size = scope.ring.size();
    for (std::size_t i = 0; i < out.size(); i += factor) {
      scope.ring[scope.write] = 0.5F * (out[i].left + out[i].right);
      scope.write = (scope.write + 1) % size;
    }
    scope.fed = true;
  }

  // One chain on `frames` frames at its rate, in place.
  void run_chain(Chain& chain, std::span<Frame> audio) noexcept {
    const std::size_t m = audio.size();
    for (std::size_t r = 0; r < chain.count; ++r) {
      Run& run = chain.runs[r];
      std::copy_n(audio.begin(), m, dry.begin());
      if (run.module != nullptr) {
        auto& slot = slots[run.uid - 1];
        const bool off = slot.on.value == 0.0 && !slot.on.moving() && !slot.on_target;
        if (off) {
          // Bypassed and faded: skip the module, and start it clean when it
          // comes back.
          if (!run.asleep) {
            run.module->reset();
            run.asleep = true;
          }
          for (std::size_t i = 0; i < m; ++i) {
            (void)slot.mix.next();
            (void)run.entry.next();
          }
        } else {
          run.asleep = false;
          run.module->process(audio);
          for (std::size_t i = 0; i < m; ++i) {
            const double g = slot.mix.next() * slot.on.next() * run.entry.next();
            const Frame d = dry[i];
            const Frame w = audio[i];
            audio[i] = {static_cast<float>(d.left + g * (w.left - d.left)),
                        static_cast<float>(d.right + g * (w.right - d.right))};
          }
        }
      }
      if (run.old != nullptr) {
        std::copy_n(dry.begin(), m, old.begin());
        run.old->process(std::span<Frame>(old.data(), m));
        for (std::size_t i = 0; i < m; ++i) {
          const double w = run.old_weight.next();
          const Frame d = dry[i];
          const double ol = d.left + run.old_mix * (old[i].left - d.left);
          const double orr = d.right + run.old_mix * (old[i].right - d.right);
          audio[i] = {static_cast<float>(audio[i].left + w * (ol - audio[i].left)),
                      static_cast<float>(audio[i].right + w * (orr - audio[i].right))};
        }
        if (!run.old_weight.moving()) {
          run.old = nullptr;
        }
      }
      for (auto& frame : audio) {
        if (!std::isfinite(frame.left) || !std::isfinite(frame.right)) {
          frame = {0.0F, 0.0F};
        }
      }
      if (run.module != nullptr) {
        watch(run.uid, std::span<const Frame>(dry.data(), m), audio, chain.factor);
      }
    }
    compact(chain);
  }

  void process_chunk(std::span<Frame> io) noexcept {
    const std::size_t n = io.size();
    for (std::size_t i = 0; i < n; ++i) {
      duck_gain[i] = duck == Duck::none ? 1.0 : duck_level.next();
    }

    // Main: up, split, modules, low band back, down; the pre tap goes down
    // through its own (identical) stage so it stays phase-aligned.
    auto& main = chains[main_chain];
    const std::size_t fm = main.factor;
    const std::size_t mm = n * fm;
    main.io.up(io.data(), n, main.buffer.data());
    const double main_rate = chain_rate(main_chain);
    for (std::size_t i = 0; i < mm; ++i) {
      const double weight = split_weight.next();
      const double octave = split_octave.next();
      if ((split_tick++ % split_interval) == 0) {
        split_c = fx_detail::Lr4::coefficients(std::exp2(octave), main_rate);
      }
      const Frame u = main.buffer[i];
      pre_high[i] = u;
      StereoSample low_band;
      StereoSample high_band;
      split.process({u.left, u.right}, split_c, low_band, high_band);
      main.buffer[i] = {static_cast<float>(u.left + weight * (high_band.left - u.left)),
                        static_cast<float>(u.right + weight * (high_band.right - u.right))};
      low[i] = {static_cast<float>(weight * low_band.left),
                static_cast<float>(weight * low_band.right)};
    }
    run_chain(main, std::span<Frame>(main.buffer.data(), mm));
    for (std::size_t i = 0; i < mm; ++i) {
      main.buffer[i].left += low[i].left;
      main.buffer[i].right += low[i].right;
    }
    main.io.down(main.buffer.data(), n, main_out.data());
    main.pre.down(pre_high.data(), n, pre_out.data());
    if (main.ducked) {
      for (std::size_t i = 0; i < n; ++i) {
        const double d = duck_gain[i];
        Frame& y = main_out[i];
        if (silence) {
          y = {static_cast<float>(y.left * d), static_cast<float>(y.right * d)};
        } else {
          // A reorder ducks Main toward its own input, never to a gap.
          const Frame x = pre_out[i];
          y = {static_cast<float>(x.left + d * (y.left - x.left)),
               static_cast<float>(x.right + d * (y.right - x.right))};
        }
      }
    }

    // Buses, fed from Main's send points.
    for (std::size_t b = 0; b < 2; ++b) {
      auto& bus = chains[b + 1];
      for (std::size_t i = 0; i < n; ++i) {
        const double level = send_level[b].next();
        const double post = post_weight[b].next();
        const Frame x = pre_out[i];
        const Frame y = main_out[i];
        send[b][i] = {static_cast<float>(level * (x.left + post * (y.left - x.left))),
                      static_cast<float>(level * (x.right + post * (y.right - x.right)))};
      }
      const std::size_t mb = n * bus.factor;
      bus.io.up(send[b].data(), n, bus.buffer.data());
      run_chain(bus, std::span<Frame>(bus.buffer.data(), mb));
      bus.io.down(bus.buffer.data(), n, bus_out[b].data());
      for (std::size_t i = 0; i < n; ++i) {
        const double g = return_gain[b].next() * (bus.ducked ? duck_gain[i] : 1.0);
        bus_out[b][i] = {static_cast<float>(bus_out[b][i].left * g),
                         static_cast<float>(bus_out[b][i].right * g)};
      }
    }

    // Sum, phase-matched, and meter.
    for (std::size_t i = 0; i < n; ++i) {
      Frame y = main_compensation[1].pass(main_compensation[0].pass(main_out[i]));
      const Frame b1 = bus_compensation[0].pass(bus_out[0][i]);
      const Frame b2 = bus_compensation[1].pass(bus_out[1][i]);
      const std::array<Frame, 3> sources{y, b1, b2};
      for (std::size_t s = 0; s < 3; ++s) {
        peaks[2 * s] =
            std::max(peaks[2 * s] * peak_fall, std::fabs(static_cast<double>(sources[s].left)));
        peaks[2 * s + 1] = std::max(peaks[2 * s + 1] * peak_fall,
                                    std::fabs(static_cast<double>(sources[s].right)));
      }
      y.left += b1.left + b2.left;
      y.right += b1.right + b2.right;
      io[i] = {std::isfinite(y.left) ? y.left : 0.0F, std::isfinite(y.right) ? y.right : 0.0F};
    }
    run_scopes(n);
  }

  // Is `module` held by the layout (placed or retiring)?
  [[nodiscard]] static bool held(const FxLayout& layout, const FxModule* module) noexcept {
    for (const auto& chain : layout.chains) {
      for (const auto& slot : chain) {
        if (slot.module.get() == module) {
          return true;
        }
      }
    }
    for (const auto& slot : layout.retiring) {
      if (slot.module.get() == module) {
        return true;
      }
    }
    return false;
  }

  // Forgets every instance the new layout no longer keeps alive.
  void purge(const FxLayout& layout) noexcept {
    for (auto& chain : chains) {
      for (std::size_t r = 0; r < chain.count; ++r) {
        auto& run = chain.runs[r];
        if (run.module != nullptr && !held(layout, run.module)) {
          run.module = nullptr;
        }
        if (run.old != nullptr && !held(layout, run.old)) {
          run.old = nullptr;
        }
      }
      compact(chain);
      std::size_t k = 0;
      for (std::size_t r = 0; r < chain.pending_count; ++r) {
        if (chain.pending[r].module != nullptr && held(layout, chain.pending[r].module)) {
          chain.pending[k++] = chain.pending[r];
        }
      }
      chain.pending_count = k;
    }
  }

  // Where `module` runs as a primary instance, or fx_chain_count.
  [[nodiscard]] std::size_t chain_of(const FxModule* module) const noexcept {
    for (std::size_t c = 0; c < fx_chain_count; ++c) {
      const auto& chain = chains[c];
      const auto& list = chain.has_pending ? chain.pending : chain.runs;
      const std::size_t count = chain.has_pending ? chain.pending_count : chain.count;
      for (std::size_t r = 0; r < count; ++r) {
        if (list[r].module == module) {
          return c;
        }
      }
    }
    return fx_chain_count;
  }

  // The placed order of a layout chain as fresh runs.
  void fresh_list(const std::vector<FxSlot>& slots_in, std::array<Run, max_runs>& out,
                  std::size_t& count) noexcept {
    count = 0;
    for (const auto& slot : slots_in) {
      if (count < max_runs && slot.module) {
        fresh_run(out[count++], slot.module.get(), slot.uid);
      }
    }
  }

  // Adds, replacements and removals with the shared modules in the same
  // order: per-slot 20 ms fades, no duck.
  void merge(std::size_t c, const std::vector<FxSlot>& next) noexcept {
    auto& chain = chains[c];
    const std::uint32_t fade = fade_frames(c);
    std::array<Run, max_runs> out{};
    std::array<std::size_t, max_runs> out_of{}; // Current run index -> out index + 1.
    std::array<bool, max_runs> consumed{};
    std::size_t k = 0;
    for (const auto& slot : next) {
      if (!slot.module || k >= max_runs) {
        continue;
      }
      FxModule* module = slot.module.get();
      std::size_t found = max_runs;
      for (std::size_t r = 0; r < chain.count; ++r) {
        if (chain.runs[r].module == module) {
          found = r;
        }
      }
      if (found < max_runs) {
        out[k] = chain.runs[found];
        consumed[found] = true;
        out_of[found] = k + 1;
        ++k;
        continue;
      }
      Run run;
      fresh_run(run, module, slot.uid);
      std::size_t replaced = max_runs;
      for (std::size_t r = 0; r < chain.count; ++r) {
        if (!consumed[r] && chain.runs[r].module != nullptr && chain.runs[r].uid == slot.uid) {
          replaced = r;
        }
      }
      if (replaced < max_runs) {
        // The old module keeps sounding on the same input as the new one
        // fades in over it.
        run.old = chain.runs[replaced].module;
        run.old_mix = effective_mix(chain.runs[replaced]);
        run.old_weight.jump(1.0);
        run.old_weight.to(0.0, fade);
        consumed[replaced] = true;
        out_of[replaced] = k + 1;
      } else {
        run.entry.jump(0.0);
        run.entry.to(1.0, fade);
      }
      prime(*module, slot.uid, c);
      out[k++] = run;
    }
    // Modules that left fade to dry where they were.
    for (std::size_t r = 0; r < chain.count; ++r) {
      if (consumed[r]) {
        continue;
      }
      Run ghost = chain.runs[r];
      if (ghost.module != nullptr) {
        ghost.old = ghost.module;
        ghost.old_mix = effective_mix(ghost);
        ghost.old_weight.jump(1.0);
        ghost.old_weight.to(0.0, fade);
        ghost.module = nullptr;
      }
      if (ghost.old == nullptr || k >= max_runs) {
        continue;
      }
      std::size_t at = 0;
      for (std::size_t q = r; q-- > 0;) {
        if (out_of[q] != 0) {
          at = out_of[q];
          break;
        }
      }
      for (std::size_t q = k; q > at; --q) {
        out[q] = out[q - 1];
      }
      for (auto& index : out_of) {
        if (index > at) {
          ++index;
        }
      }
      out[at] = ghost;
      out_of[r] = at + 1;
      ++k;
    }
    chain.runs = out;
    chain.count = k;
  }

  void adopt(const FxLayout& layout) noexcept {
    purge(layout);
    if (!adopted) {
      for (std::size_t c = 0; c < fx_chain_count; ++c) {
        auto& chain = chains[c];
        fresh_list(layout.chains[c], chain.runs, chain.count);
        for (std::size_t r = 0; r < chain.count; ++r) {
          prime(*chain.runs[r].module, chain.runs[r].uid, c);
        }
      }
      adopted = true;
      return;
    }
    // A chain whose shared modules changed order, or that gains or loses a
    // module to another chain, ducks and swaps at the bottom.
    std::array<bool, fx_chain_count> reorder{};
    for (std::size_t c = 0; c < fx_chain_count; ++c) {
      const auto& chain = chains[c];
      std::size_t last = 0;
      bool first = true;
      for (const auto& slot : layout.chains[c]) {
        const std::size_t home = chain_of(slot.module.get());
        if (home != fx_chain_count && home != c) {
          reorder[c] = true;
          reorder[home] = true;
        }
        for (std::size_t r = 0; r < chain.count; ++r) {
          if (chain.runs[r].module == slot.module.get()) {
            if (!first && r < last) {
              reorder[c] = true;
            }
            last = r;
            first = false;
          }
        }
      }
      reorder[c] = reorder[c] || chain.has_pending;
    }
    for (std::size_t c = 0; c < fx_chain_count; ++c) {
      auto& chain = chains[c];
      if (reorder[c]) {
        fresh_list(layout.chains[c], chain.pending, chain.pending_count);
        chain.has_pending = true;
        chain.ducked = true;
        for (std::size_t r = 0; r < chain.pending_count; ++r) {
          prime(*chain.pending[r].module, chain.pending[r].uid, c);
        }
        start_duck();
      } else {
        merge(c, layout.chains[c]);
      }
    }
  }
};

FxRack::FxRack() : state_(std::make_unique<State>()) {}
FxRack::~FxRack() = default;

void FxRack::prepare(double sample_rate, std::size_t max_frames) {
  auto& s = *state_;
  s.rate = sample_rate;
  s.max_frames = std::max<std::size_t>(1, max_frames);
  (void)halfband_coefficients();
  const std::size_t high = s.max_frames * fx_detail::max_factor;
  for (auto& chain : s.chains) {
    chain.buffer.assign(high, {0.0F, 0.0F});
  }
  s.main_out.assign(s.max_frames, {0.0F, 0.0F});
  s.pre_out.assign(s.max_frames, {0.0F, 0.0F});
  for (std::size_t b = 0; b < 2; ++b) {
    s.send[b].assign(s.max_frames, {0.0F, 0.0F});
    s.bus_out[b].assign(s.max_frames, {0.0F, 0.0F});
  }
  s.duck_gain.assign(s.max_frames, 1.0);
  s.dry.assign(high, {0.0F, 0.0F});
  s.old.assign(high, {0.0F, 0.0F});
  s.low.assign(high, {0.0F, 0.0F});
  s.pre_high.assign(high, {0.0F, 0.0F});
  s.duck_frames = static_cast<std::uint32_t>(std::lround(duck_seconds * sample_rate));
  s.peak_fall = std::pow(10.0, -meter_fall_db_per_second / 20.0 / sample_rate);
  s.prepare_scopes();
  const FxRackSettings defaults{};
  for (std::size_t c = 0; c < fx_chain_count; ++c) {
    s.chains[c].requested = valid_factor(defaults.oversample[c]);
    s.chains[c].factor = 0; // Forces apply_factors() to set every stage.
  }
  s.apply_factors();
  s.split_octave.jump(std::log2(defaults.split_hz));
  s.split_weight.jump(0.0);
  for (std::size_t b = 0; b < 2; ++b) {
    s.send_level[b].jump(0.0);
    s.post_weight[b].jump(1.0);
    s.return_gain[b].jump(1.0);
  }
  for (auto& slot : s.slots) {
    slot.mix.jump(1.0);
    slot.on.jump(1.0);
  }
  s.duck_level.jump(1.0);
  reset();
}

void FxRack::reset() noexcept {
  auto& s = *state_;
  for (std::size_t c = 0; c < fx_chain_count; ++c) {
    auto& chain = s.chains[c];
    if (chain.has_pending) {
      chain.runs = chain.pending;
      chain.count = chain.pending_count;
      chain.has_pending = false;
      chain.pending_count = 0;
    }
    for (std::size_t r = 0; r < chain.count; ++r) {
      auto& run = chain.runs[r];
      run.old = nullptr;
      run.entry.jump(1.0);
      run.old_weight.jump(0.0);
      run.asleep = false;
      if (run.module != nullptr) {
        run.module->reset();
      }
    }
    s.compact(chain);
    chain.ducked = false;
  }
  s.apply_factors();
  for (auto& chain : s.chains) {
    chain.io.reset();
    chain.pre.reset();
  }
  for (std::size_t b = 0; b < 2; ++b) {
    s.main_compensation[b].reset();
    s.bus_compensation[b].reset();
  }
  s.split.reset();
  s.split_tick = 0;
  s.duck = State::Duck::none;
  s.duck_level.jump(1.0);
  s.silence = false;
  s.peaks = {};
  s.slot_in = {};
  s.slot_out = {};
  for (auto& scope : s.scopes) {
    std::ranges::fill(scope.ring, 0.0F);
    scope.bands.fill(floor_db);
    scope.fed = false;
  }
  for (auto& slot : s.slots) {
    slot.mix.jump(slot.mix.target);
    slot.on.jump(slot.on.target);
  }
}

void FxRack::adopt(const FxLayout& layout) noexcept { state_->adopt(layout); }

void FxRack::set(const FxRackSettings& settings, const FxContext& context,
                 std::uint32_t frames) noexcept {
  auto& s = *state_;
  const bool first = !s.has_settings;
  if (first) {
    frames = 0;
  }
  s.settings = settings;
  s.beats_per_second = context.beats_per_second;
  s.has_settings = true;

  // Chain factors: at once before anything sounds, else under a duck.
  bool factor_change = false;
  for (std::size_t c = 0; c < fx_chain_count; ++c) {
    const std::uint32_t wanted = valid_factor(settings.oversample[c]);
    if (wanted != s.chains[c].requested) {
      s.chains[c].requested = wanted;
    }
    factor_change = factor_change || wanted != s.chains[c].factor;
  }
  if (factor_change) {
    if (first || !s.adopted) {
      s.apply_factors();
    } else {
      s.silence = true;
      for (auto& chain : s.chains) {
        chain.ducked = true;
      }
      s.start_duck();
    }
  }

  // Slots: mix and bypass glides at their chain's rate, then the modules.
  std::array<std::size_t, fx_slot_uids> home{};
  home.fill(main_chain);
  for (std::size_t c = 0; c < fx_chain_count; ++c) {
    const auto& chain = s.chains[c];
    for (std::size_t r = 0; r < chain.count; ++r) {
      home[chain.runs[r].uid - 1] = c;
    }
  }
  for (std::size_t u = 0; u < fx_slot_uids; ++u) {
    auto& slot = s.slots[u];
    const auto& wanted = settings.slots[u];
    const std::uint32_t factor = s.chains[home[u]].factor;
    slot.mix.to(fx_detail::clamp01(wanted.mix), frames * factor);
    if (wanted.on != slot.on_target || first) {
      slot.on_target = wanted.on;
      slot.on.to(wanted.on ? 1.0 : 0.0, first ? 0U : s.fade_frames(home[u]));
    }
  }
  for (std::size_t c = 0; c < fx_chain_count; ++c) {
    auto& chain = s.chains[c];
    const FxContext at = s.context(c);
    for (std::size_t r = 0; r < chain.count; ++r) {
      if (chain.runs[r].module != nullptr) {
        chain.runs[r].module->set(settings.slots[chain.runs[r].uid - 1].p, at,
                                  frames * chain.factor);
      }
    }
    for (std::size_t r = 0; r < chain.pending_count; ++r) {
      chain.pending[r].module->set(settings.slots[chain.pending[r].uid - 1].p, at,
                                   frames * chain.factor);
    }
  }

  // Split, sends and returns.
  const std::uint32_t main_frames = frames * s.chains[main_chain].factor;
  s.split_octave.to(std::log2(std::clamp(settings.split_hz, 40.0, 2'000.0)), main_frames);
  if (settings.split != s.split_target || first) {
    s.split_target = settings.split;
    s.split_weight.to(settings.split ? 1.0 : 0.0, first ? 0U : s.fade_frames(main_chain));
  }
  const auto engine_fade = static_cast<std::uint32_t>(std::lround(fade_seconds * s.rate));
  for (std::size_t b = 0; b < 2; ++b) {
    s.send_level[b].to(fx_detail::clamp01(settings.bus_send[b]), frames);
    if (settings.bus_send_post[b] != s.post_target[b] || first) {
      s.post_target[b] = settings.bus_send_post[b];
      s.post_weight[b].to(settings.bus_send_post[b] ? 1.0 : 0.0, first ? 0U : engine_fade);
    }
    const double db = std::isfinite(settings.bus_return_db[b])
                          ? std::clamp(settings.bus_return_db[b], -60.0, 6.0)
                          : 0.0;
    s.return_gain[b].to(fx_detail::db_to_gain(db), frames);
  }
}

void FxRack::process(std::span<engine::StereoFrame> io) noexcept {
  auto& s = *state_;
  if (s.max_frames == 0) {
    return;
  }
  std::size_t done = 0;
  while (done < io.size()) {
    if (s.duck == State::Duck::out && !s.duck_level.moving()) {
      s.switch_structure();
      s.duck = State::Duck::in;
      s.duck_level.to(1.0, s.duck_frames);
    }
    std::size_t n = std::min(io.size() - done, s.max_frames);
    if (s.duck == State::Duck::out) {
      n = std::min<std::size_t>(n, s.duck_level.left);
    }
    s.process_chunk(io.subspan(done, n));
    done += n;
    if (s.duck == State::Duck::in && !s.duck_level.moving()) {
      s.duck = State::Duck::none;
      s.silence = false;
      for (auto& chain : s.chains) {
        chain.ducked = false;
      }
    }
  }
}

void FxRack::telemetry(Telemetry& out) const noexcept {
  const auto& s = *state_;
  out.live.fill(0.0F);
  out.input.fill(floor_db);
  out.output.fill(floor_db);
  out.detail.fill(0.0F);
  out.spectrum.fill(floor_db);
  const auto db = [](double peak) {
    return static_cast<float>(std::max<double>(floor_db, fx_detail::gain_to_db(peak)));
  };
  for (const auto& chain : s.chains) {
    for (std::size_t r = 0; r < chain.count; ++r) {
      const auto& run = chain.runs[r];
      if (run.module == nullptr) {
        continue;
      }
      const std::size_t u = run.uid - 1;
      const float value = run.module->telemetry();
      out.live[u] = std::isfinite(value) ? value : 0.0F;
      out.input[u] = db(s.slot_in[u]);
      out.output[u] = db(s.slot_out[u]);
      const std::span<float, fx_detail_values> detail(out.detail.data() + u * fx_detail_values,
                                                      fx_detail_values);
      run.module->detail(detail);
      for (auto& v : detail) {
        v = std::isfinite(v) ? v : 0.0F;
      }
      std::copy(s.scopes[u].bands.begin(), s.scopes[u].bands.end(),
                out.spectrum.begin() + static_cast<std::ptrdiff_t>(u * fx_spectrum_bands));
    }
  }
  for (std::size_t i = 0; i < out.meters.size(); ++i) {
    out.meters[i] = db(s.peaks[i]);
  }
}

std::uint64_t FxRack::tail_frames() const noexcept {
  const auto& s = *state_;
  std::uint64_t longest = 0;
  for (const auto& chain : s.chains) {
    const std::uint64_t factor = std::max<std::uint32_t>(1, chain.factor);
    for (std::size_t r = 0; r < chain.count; ++r) {
      const auto& run = chain.runs[r];
      if (run.module != nullptr) {
        longest = std::max(longest, run.module->tail_frames() / factor);
      }
      if (run.old != nullptr) {
        longest = std::max(longest, run.old->tail_frames() / factor);
      }
    }
  }
  // The resamplers' and crossover's own ring-out.
  return longest + 64;
}

} // namespace oscillate::native_dsp::synth::nova
