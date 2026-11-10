#include "oscillate/native_dsp/effects/lens.hpp"

#include "capture_queue.hpp"
#include "dsp/fft.hpp"
#include "lens_presets.hpp"
#include "lens_table.hpp"
#include "oscillate/native_dsp/tonal_analysis.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>
#include <vector>

namespace oscillate::native_dsp::effects::lens {
namespace {

using lens_table::index_of;
using lens_table::View;

namespace p {
constexpr std::size_t view = index_of("lens.view");
constexpr std::size_t resolution = index_of("lens.resolution");
constexpr std::size_t average = index_of("lens.average");
constexpr std::size_t smoothing = index_of("lens.smoothing");
constexpr std::size_t tilt = index_of("lens.tilt");
constexpr std::size_t hold = index_of("lens.hold");
constexpr std::size_t hold_decay = index_of("lens.hold_decay");
constexpr std::size_t overlay = index_of("lens.overlay");
constexpr std::size_t freeze = index_of("lens.freeze");
constexpr std::size_t match_source = index_of("lens.match_source");
constexpr std::size_t match_amount = index_of("lens.match_amount");
constexpr std::size_t match_smoothing = index_of("lens.match_smoothing");
constexpr std::size_t match_low = index_of("lens.match_low");
constexpr std::size_t match_high = index_of("lens.match_high");
constexpr std::size_t match_boost = index_of("lens.match_boost");
constexpr std::size_t match_cut = index_of("lens.match_cut");
} // namespace p

// Window sizes at 48 kHz for each resolution, longest first; other rates
// scale them to the nearest power of two so the resolution in hertz holds.
constexpr std::array<std::array<std::size_t, 3>, 3> sizes_48k{
    {{4'096, 1'024, 256}, {16'384, 4'096, 1'024}, {32'768, 8'192, 2'048}}};
constexpr std::size_t max_size = std::size_t{32'768} * 8; // 384 kHz Precise.
// One hop is 512 frames at 48 kHz (10.7 ms).
constexpr double hop_seconds = 512.0 / 48'000.0;
// A 4-term Blackman-Harris spreads a sine over +-3 bins, 99.97% of it in
// +-2. A band at least five bins wide is read as band power (summed over
// the band, over the window's noise bandwidth). A narrower band is read
// from the window's own spectrum at the point: a parabola in log power
// through the nearest bins (the main lobe is near Gaussian), so a sine
// reads its level at its frequency and close tones stay apart. The two
// readings crossfade as the band grows from half a lobe to a whole one.
constexpr double lobe_bins = 5.0;
constexpr double silent_db = -200.0;
// The callback keeps this much input for the reader (ADR-0125): a reader
// that falls further behind starts its analysis afresh.
constexpr double capture_seconds = 2.0;
// Resonances: local maxima standing 6 dB over their third-octave
// surroundings.
constexpr double peak_prominence_db = 6.0;
constexpr double peak_reach_octaves = 1.0 / 3.0;

[[nodiscard]] int as_int(double value) noexcept { return static_cast<int>(value); }

[[nodiscard]] std::size_t scaled_size(std::size_t size_48k, double rate) noexcept {
  const double wanted = static_cast<double>(size_48k) * rate / 48'000.0;
  std::size_t size = 64;
  while (static_cast<double>(size) * 1.41421356 < wanted) {
    size *= 2;
  }
  return std::min(size, max_size);
}

// One window size: its FFT, window, power spectra of up to three signals
// and their running sums over bin edges.
struct Resolution {
  std::size_t size = 0;
  std::unique_ptr<native_dsp::dsp::Fft> fft;
  std::vector<double> window;
  double enbw = 1.0;  // Equivalent noise bandwidth, in bins.
  double scale = 1.0; // (2 / sum(w))^2: a bin-centred sine's peak bin = A^2.
  std::array<std::vector<double>, 3> cumulative; // a, b, sidechain.
  // Updated every `stride` hops: 87.5% overlap, since a long window barely
  // changes from one 10.7 ms hop to the next.
  std::uint64_t stride = 1;
};

// One frame as the callback captures it: the input, and the sidechain
// (silent without one).
struct Captured {
  float left = 0.0F;
  float right = 0.0F;
  float side_left = 0.0F;
  float side_right = 0.0F;
};

// The callback only captures its input and the values; the analysis runs
// when the telemetry is read (EffectKernel::analyse_telemetry), on the control
// thread, and only then.
class LensKernel final : public EffectKernel {
public:
  explicit LensKernel(double sample_rate)
      : rate_(sample_rate), meter_(sample_rate),
        hop_(std::max<std::size_t>(
            64, static_cast<std::size_t>(std::lround(hop_seconds * sample_rate)))),
        queue_(static_cast<std::size_t>(std::ceil(capture_seconds * sample_rate))) {
    // Every size any resolution may use at this rate.
    std::vector<std::size_t> all;
    for (const auto& set : sizes_48k) {
      for (const auto size : set) {
        all.push_back(scaled_size(size, sample_rate));
      }
    }
    std::ranges::sort(all);
    all.erase(std::ranges::unique(all).begin(), all.end());
    for (const auto size : all) {
      auto& r = resolutions_.emplace_back();
      r.size = size;
      r.fft = std::make_unique<native_dsp::dsp::Fft>(size);
      r.window.resize(size);
      double sum = 0.0;
      double squares = 0.0;
      for (std::size_t i = 0; i < size; ++i) {
        const double t =
            2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(size);
        const double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2.0 * t) -
                         0.01168 * std::cos(3.0 * t);
        r.window[i] = w;
        sum += w;
        squares += w * w;
      }
      r.enbw = static_cast<double>(size) * squares / (sum * sum);
      r.stride = std::max<std::uint64_t>(1, size / (8 * hop_));
      r.scale = (2.0 / sum) * (2.0 / sum);
      for (auto& c : r.cumulative) {
        c.assign(size / 2 + 2, 0.0);
      }
      largest_ = std::max(largest_, size);
    }
    ring_mask_ = 1;
    while (ring_mask_ < largest_) {
      ring_mask_ <<= 1U;
    }
    for (auto& ring : rings_) {
      ring.assign(ring_mask_, 0.0);
    }
    ring_mask_ -= 1;
    work_.resize(largest_);
    for (std::size_t j = 0; j < points; ++j) {
      hz_[j] = point_hz(j);
    }
    restart();
  }

  // Callback only: the values, for the reader.
  void control(std::span<const double> v, std::uint32_t, const Transport&) noexcept override {
    for (std::size_t i = 0; i < values_.size(); ++i) {
      values_[i].store(v[i], std::memory_order_relaxed);
    }
  }

  // Callback only: audio passes untouched; the reader analyses a copy.
  void process(std::span<engine::StereoFrame> audio,
               std::span<const engine::StereoFrame> sidechain) noexcept override {
    (void)queue_.write(audio.size(), [&](std::size_t i) {
      const auto side = sidechain.empty() ? engine::StereoFrame{} : sidechain[i];
      return Captured{audio[i].left, audio[i].right, side.left, side.right};
    });
  }

  // Callback only: the reader starts afresh from here.
  void reset() noexcept override { queue_.restart(); }

  [[nodiscard]] bool analyses_off_callback() const noexcept override { return true; }

  // Control only: the values the callback last saw, then every captured
  // frame through the analysis, then the telemetry.
  [[nodiscard]] std::size_t analyse_telemetry(std::span<float> out) noexcept override {
    if (out.size() < telemetry::size) {
      return 0;
    }
    adopt_values();
    queue_.read([this] { restart(); }, [this](const Captured& frame) { take(frame); });
    if (!captured_) {
      return 0;
    }
    write_telemetry(out);
    return telemetry::size;
  }

private:
  // The values as the callback last saw them, with what they imply.
  void adopt_values() noexcept {
    std::array<double, lens_table::parameters.size()> v{};
    for (std::size_t i = 0; i < v.size(); ++i) {
      v[i] = values_[i].load(std::memory_order_relaxed);
    }
    const int view = as_int(v[p::view]);
    const int resolution = as_int(v[p::resolution]);
    const int smoothing = as_int(v[p::smoothing]);
    if (!primed_ || view != view_ || resolution != resolution_ || smoothing != smoothing_) {
      view_ = view;
      resolution_ = resolution;
      smoothing_ = smoothing;
      plan();
      // A new view or band layout starts its averages afresh.
      clear_averages();
    }
    const double seconds = lens_table::average_seconds[static_cast<std::size_t>(v[p::average])];
    infinite_ = seconds <= 0.0;
    if (!infinite_) {
      alpha_ = 1.0 - std::exp(-static_cast<double>(hop_) / (seconds * rate_));
    }
    tilt_ = lens_table::tilt_db[static_cast<std::size_t>(v[p::tilt])];
    hold_on_ = v[p::hold] != 0.0;
    hold_step_ = v[p::hold_decay] * static_cast<double>(hop_) / rate_;
    overlay_ = v[p::overlay] != 0.0;
    freeze_ = v[p::freeze] != 0.0;
    primed_ = true;
  }

  // One captured frame into the analysis rings and the tonal meter.
  void take(const Captured& frame) noexcept {
    captured_ = true;
    const std::size_t at = write_ & ring_mask_;
    rings_[0][at] = frame.left;
    rings_[1][at] = frame.right;
    rings_[2][at] = 0.5 * (static_cast<double>(frame.side_left) + frame.side_right);
    // The tonal meter reads the mid of the input and the sidechain, whatever
    // the view, and runs while frozen.
    meter_.push(0.5 * (static_cast<double>(frame.left) + frame.right), rings_[2][at]);
    ++write_;
    if (++since_hop_ == hop_) {
      since_hop_ = 0;
      if (!freeze_) {
        analyse_hop();
      }
    }
  }

  // A new run (a seek, or a reader that fell behind): from silence.
  void restart() noexcept {
    for (auto& ring : rings_) {
      std::ranges::fill(ring, 0.0);
    }
    write_ = 0;
    since_hop_ = 0;
    hops_ = 0;
    computed_.fill(false);
    clear_averages();
    meter_.reset();
  }

  void write_telemetry(std::span<float> out) const noexcept {
    for (std::size_t j = 0; j < points; ++j) {
      const double tilt = tilt_ * std::log2(hz_[j] / 1'000.0);
      out[telemetry::curve_a + j] = static_cast<float>(display(average_[0][j], tilt));
      out[telemetry::curve_b + j] = has_b_ ? static_cast<float>(display(average_[1][j], tilt))
                                           : static_cast<float>(silent_db);
      out[telemetry::overlay + j] = overlay_ ? static_cast<float>(display(average_[2][j], tilt))
                                             : static_cast<float>(silent_db);
      out[telemetry::hold + j] =
          static_cast<float>(hold_on_ ? hold_[j] + tilt : display(average_[0][j], tilt));
    }
    for (std::size_t k = 0; k < peak_count; ++k) {
      out[telemetry::peaks + 2 * k] = static_cast<float>(peaks_[k][0]);
      out[telemetry::peaks + 2 * k + 1] = static_cast<float>(peaks_[k][1]);
    }
    out[telemetry::hops] = static_cast<float>(hops_);
    for (std::size_t i = 0; i < tonal::fine_bands; ++i) {
      out[telemetry::tonal_input + i] = static_cast<float>(meter_.mean_a()[i]);
      out[telemetry::tonal_sidechain + i] = static_cast<float>(meter_.mean_b()[i]);
    }
    out[telemetry::tonal_windows] = static_cast<float>(meter_.windows());
  }

  // How each point is measured: up to two window sizes, their weights and
  // band edges in bins.
  struct Plan {
    std::array<std::size_t, 2> resolution{};
    std::array<double, 2> weight{};
    std::array<double, 2> low{};
    std::array<double, 2> high{};
    std::array<double, 2> centre{}; // The point, in bins.
    std::array<double, 2> narrow{}; // Weight of the narrow reading.
  };

  [[nodiscard]] static double display(double power, double tilt) noexcept {
    return power > 0.0 ? 10.0 * std::log10(power) + tilt : silent_db;
  }

  [[nodiscard]] std::size_t resolution_index(std::size_t size) const noexcept {
    for (std::size_t r = 0; r < resolutions_.size(); ++r) {
      if (resolutions_[r].size == size) {
        return r;
      }
    }
    return 0;
  }

  // For each point: the shortest window whose main lobe fits the band,
  // crossfaded with the next longer one while it only just fits.
  void plan() noexcept {
    const auto& set = sizes_48k[static_cast<std::size_t>(resolution_)];
    std::array<std::size_t, 3> chosen{}; // Longest first.
    for (std::size_t k = 0; k < 3; ++k) {
      chosen[k] = resolution_index(scaled_size(set[k], rate_));
    }
    const double octaves = lens_table::smoothing_octaves[static_cast<std::size_t>(smoothing_)];
    for (std::size_t j = 0; j < points; ++j) {
      const double f = hz_[j];
      const double band = f * (std::exp2(octaves / 2.0) - std::exp2(-octaves / 2.0));
      // fit[k]: the band over half this window's main lobe. A window may
      // be up to twice as coarse as the band; the narrow reading keeps a
      // sine exact, and the shorter window follows transients.
      std::array<double, 3> fit{};
      for (std::size_t k = 0; k < 3; ++k) {
        fit[k] =
            band / (0.5 * lobe_bins * rate_ / static_cast<double>(resolutions_[chosen[k]].size));
      }
      auto& plan = plans_[j];
      // Shortest first: the first that fits by two takes it all; between one
      // and two it shares with the next longer.
      std::size_t pick = 0;
      double share = 1.0;
      for (std::size_t k = 3; k-- > 0;) {
        if (fit[k] >= 1.0) {
          pick = k;
          share = k == 0 ? 1.0 : std::clamp(fit[k] - 1.0, 0.0, 1.0);
          break;
        }
      }
      plan.resolution = {chosen[pick], chosen[pick == 0 ? 0 : pick - 1]};
      plan.weight = {share, 1.0 - share};
      for (std::size_t s = 0; s < 2; ++s) {
        const auto& r = resolutions_[plan.resolution[s]];
        const double bin_hz = rate_ / static_cast<double>(r.size);
        // Band power over at least the main lobe, centred on the point,
        // or the narrow reading where the band is under a lobe.
        const double width = std::max(band, lobe_bins * bin_hz);
        const double low = std::max(0.0, f - width / 2.0);
        plan.low[s] = low / bin_hz;
        plan.high[s] = std::min(f + width / 2.0, rate_ / 2.0) / bin_hz;
        plan.centre[s] = f / bin_hz;
        const double ratio = band / (lobe_bins * bin_hz);
        plan.narrow[s] = std::clamp((1.0 - ratio) / 0.5, 0.0, 1.0);
      }
    }
    has_b_ = view_ == View::mid_side || view_ == View::left_right;
  }

  void clear_averages() noexcept {
    for (auto& a : average_) {
      a.fill(0.0);
    }
    hold_.fill(silent_db);
    count_ = 0;
    peaks_ = {};
  }

  // Band sum over [low, high) in bins, from the running sums over bin
  // edges (bin k spans k - 0.5 to k + 0.5), linear inside a bin.
  [[nodiscard]] static double band_sum(const std::vector<double>& c, double low,
                                       double high) noexcept {
    const auto at = [&](double x) {
      const double edge = std::clamp(x + 0.5, 0.0, static_cast<double>(c.size() - 2));
      const auto whole = static_cast<std::size_t>(edge);
      const double frac = edge - static_cast<double>(whole);
      return c[whole] + frac * (c[whole + 1] - c[whole]);
    };
    return std::max(0.0, at(high) - at(low));
  }

  // The window's spectrum at fractional bin x: a parabola in log power
  // through the three bins around it. Bin powers are the running sums'
  // differences.
  [[nodiscard]] static double narrow_reading(const std::vector<double>& c, double x) noexcept {
    const std::size_t bins = c.size() - 1;
    const auto power = [&](std::size_t k) { return std::max(1e-30, c[k + 1] - c[k]); };
    const auto middle =
        static_cast<std::size_t>(std::clamp(std::round(x), 1.0, static_cast<double>(bins - 2)));
    const double before = std::log(power(middle - 1));
    const double here = std::log(power(middle));
    const double after = std::log(power(middle + 1));
    const double t = x - static_cast<double>(middle);
    const double curve = 0.5 * (before - 2.0 * here + after);
    const double slope = 0.5 * (after - before);
    return std::exp(here + slope * t + curve * t * t);
  }

  // The two view signals at ring offset i (0 the oldest of `size`).
  [[nodiscard]] std::array<double, 2> views(std::size_t at) const noexcept {
    const double l = rings_[0][at];
    const double r = rings_[1][at];
    switch (view_) {
    case View::side:
      return {0.5 * (l - r), 0.0};
    case View::left:
      return {l, 0.0};
    case View::right:
      return {r, 0.0};
    case View::mid_side:
      return {0.5 * (l + r), 0.5 * (l - r)};
    case View::left_right:
      return {l, r};
    default:
      return {0.5 * (l + r), 0.0};
    }
  }

  void spectra(Resolution& r) noexcept {
    const std::size_t n = r.size;
    const std::size_t start = write_ - n;
    std::span<std::complex<double>> data(work_.data(), n);
    // The view pair packed as a + jb in one transform.
    for (std::size_t i = 0; i < n; ++i) {
      const auto v = views((start + i) & ring_mask_);
      data[i] = {v[0] * r.window[i], v[1] * r.window[i]};
    }
    r.fft->forward(data);
    auto& ca = r.cumulative[0];
    auto& cb = r.cumulative[1];
    ca[0] = 0.0;
    cb[0] = 0.0;
    for (std::size_t k = 0; k <= n / 2; ++k) {
      const auto x = data[k];
      // NOLINTNEXTLINE(clang-analyzer-core.DivideZero): every resolution has a transform size.
      const auto y = std::conj(data[(n - k) % n]);
      const auto a = 0.5 * (x + y);
      const auto b = std::complex<double>(0.0, -0.5) * (x - y);
      ca[k + 1] = ca[k] + std::norm(a) * r.scale;
      cb[k + 1] = cb[k] + std::norm(b) * r.scale;
    }
    if (overlay_) {
      for (std::size_t i = 0; i < n; ++i) {
        data[i] = {rings_[2][(start + i) & ring_mask_] * r.window[i], 0.0};
      }
      r.fft->forward(data);
      auto& cs = r.cumulative[2];
      cs[0] = 0.0;
      for (std::size_t k = 0; k <= n / 2; ++k) {
        cs[k + 1] = cs[k] + std::norm(data[k]) * r.scale;
      }
    }
  }

  void analyse_hop() noexcept {
    // Only once the longest window holds signal from this run.
    std::array<bool, 16> fresh{};
    for (std::size_t j = 0; j < points; ++j) {
      for (std::size_t s = 0; s < 2; ++s) {
        if (plans_[j].weight[s] > 0.0) {
          fresh[plans_[j].resolution[s]] = true;
        }
      }
    }
    for (std::size_t r = 0; r < resolutions_.size(); ++r) {
      // On the absolute hop grid, so any block split analyses alike; every
      // size is computed on the first hop of a run.
      if (fresh[r] && (hops_ % resolutions_[r].stride == 0 || !computed_[r])) {
        spectra(resolutions_[r]);
        computed_[r] = true;
      }
    }
    ++hops_;
    ++count_;
    const double step = infinite_ ? 1.0 / static_cast<double>(count_) : alpha_;
    for (std::size_t j = 0; j < points; ++j) {
      const auto& plan = plans_[j];
      for (std::size_t c = 0; c < 3; ++c) {
        if ((c == 1 && !has_b_) || (c == 2 && !overlay_)) {
          continue;
        }
        double power = 0.0;
        for (std::size_t s = 0; s < 2; ++s) {
          if (plan.weight[s] > 0.0) {
            const auto& r = resolutions_[plan.resolution[s]];
            double reading = 0.0;
            if (plan.narrow[s] < 1.0) {
              reading += (1.0 - plan.narrow[s]) *
                         band_sum(r.cumulative[c], plan.low[s], plan.high[s]) / r.enbw;
            }
            if (plan.narrow[s] > 0.0) {
              reading += plan.narrow[s] * narrow_reading(r.cumulative[c], plan.centre[s]);
            }
            power += plan.weight[s] * reading;
          }
        }
        average_[c][j] += step * (power - average_[c][j]);
      }
      const double level = display(average_[0][j], 0.0);
      hold_[j] = std::max(level, hold_[j] - hold_step_);
    }
    find_peaks();
  }

  // A resonance's true frequency and level: display points can sit
  // further apart than the window's bins, so the window's own spectrum is
  // searched finely around the point (the narrow reading is exact for a
  // sine).
  [[nodiscard]] std::array<double, 2> refine(std::size_t j, double step) const noexcept {
    const auto& plan = plans_[j];
    const auto& r = resolutions_[plan.resolution[0]];
    const double bin_hz = rate_ / static_cast<double>(r.size);
    const double spacing = point_hz(j) * (std::exp2(step) - 1.0) / bin_hz;
    const double reach = std::max(1.0, 1.5 * spacing);
    double best_x = plan.centre[0];
    double best = 0.0;
    constexpr int samples = 48;
    for (int k = 0; k <= samples; ++k) {
      const double x = plan.centre[0] - reach + 2.0 * reach * k / samples;
      const double power = narrow_reading(r.cumulative[0], x);
      if (power > best) {
        best = power;
        best_x = x;
      }
    }
    return {best_x * bin_hz, display(best, 0.0)};
  }

  // The strongest local maxima of the first curve that stand out from
  // their third-octave surroundings, refined on the spectrum itself.
  void find_peaks() noexcept {
    peaks_ = {};
    std::array<std::array<double, 2>, points> found{};
    std::size_t count = 0;
    const double step = std::log2(highest_hz / lowest_hz) / static_cast<double>(points - 1);
    const auto reach = static_cast<std::size_t>(std::ceil(peak_reach_octaves / step));
    for (std::size_t j = 1; j + 1 < points; ++j) {
      const double here = display(average_[0][j], 0.0);
      if (here <= silent_db + 1.0 || here < display(average_[0][j - 1], 0.0) ||
          here < display(average_[0][j + 1], 0.0)) {
        continue;
      }
      double surround = here;
      for (std::size_t k = j > reach ? j - reach : 0; k < std::min(points, j + reach + 1); ++k) {
        surround = std::min(surround, display(average_[0][k], 0.0));
      }
      if (here - surround < peak_prominence_db) {
        continue;
      }
      found[count++] = refine(j, step);
    }
    std::sort(found.begin(), found.begin() + static_cast<std::ptrdiff_t>(count),
              [](const auto& a, const auto& b) { return a[1] > b[1]; });
    for (std::size_t k = 0; k < std::min(count, peak_count); ++k) {
      peaks_[k] = found[k];
    }
  }

  // Shared: the callback's values for the reader, and the captured input.
  std::array<std::atomic<double>, lens_table::parameters.size()> values_{};
  // The rest is the reader's.
  double rate_;
  tonal::BandMeter meter_;
  std::size_t hop_;
  CaptureQueue<Captured> queue_;
  bool captured_ = false;
  std::vector<Resolution> resolutions_;
  std::size_t largest_ = 0;
  std::array<std::vector<double>, 3> rings_; // left, right, sidechain mid
  std::size_t ring_mask_ = 0;
  std::vector<std::complex<double>> work_;
  std::size_t write_ = 0;
  std::size_t since_hop_ = 0;
  std::array<double, points> hz_{};
  std::array<Plan, points> plans_{};
  std::array<std::array<double, points>, 3> average_{};
  std::array<double, points> hold_{};
  std::array<std::array<double, 2>, peak_count> peaks_{};
  std::uint64_t count_ = 0;
  std::uint64_t hops_ = 0;
  std::array<bool, 16> computed_{};
  int view_ = View::mid;
  int resolution_ = 1;
  int smoothing_ = 1;
  bool has_b_ = false;
  bool infinite_ = false;
  double alpha_ = 1.0;
  double tilt_ = 0.0;
  bool hold_on_ = true;
  double hold_step_ = 0.0;
  bool overlay_ = false;
  bool freeze_ = false;
  bool primed_ = false;
};

std::unique_ptr<EffectKernel> create_kernel(std::span<const double>, double sample_rate) {
  return std::make_unique<LensKernel>(sample_rate);
}

// Lens passes audio through unchanged: no tail and no latency.
std::uint64_t tail_frames(std::span<const double>, double) { return 1; }
std::uint32_t latency_frames(std::span<const double>, double) { return 0; }

const EngineDescriptor descriptor{"lens", "Lens", 1, lens_table::parameters, lens_presets::presets};
const EffectEngine lens_engine{&descriptor, &create_kernel, &tail_frames, &latency_frames, true};

} // namespace

double point_hz(std::size_t point) noexcept {
  return lowest_hz * std::pow(highest_hz / lowest_hz,
                              static_cast<double>(point) / static_cast<double>(points - 1));
}

const EffectEngine& engine() { return lens_engine; }

match_eq::Settings match_settings(std::span<const double> v) noexcept {
  const auto smoothing = std::min(static_cast<std::size_t>(v[p::match_smoothing]),
                                  lens_table::match_smoothing_octaves.size() - 1);
  return {v[p::match_amount], lens_table::match_smoothing_octaves[smoothing],
          v[p::match_low],    v[p::match_high],
          v[p::match_boost],  v[p::match_cut]};
}

bool matches_target(std::span<const double> v) noexcept {
  return as_int(v[p::match_source]) == lens_table::MatchSource::target;
}

} // namespace oscillate::native_dsp::effects::lens
