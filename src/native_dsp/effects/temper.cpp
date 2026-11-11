#include "oscillate/native_dsp/effects/temper.hpp"

#include "dsp/vector_math.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/warp_dsp/real_fft.hpp"
#include "temper_presets.hpp"
#include "temper_table.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

namespace oscillate::native_dsp::effects::temper {
namespace {

namespace t = temper_table;
using t::index_of;
using Buffer = warp_dsp::AlignedBuffer;

namespace p {
constexpr std::size_t depth = index_of("temper.depth");
constexpr std::size_t detail = index_of("temper.detail");
constexpr std::size_t mode = index_of("temper.mode");
constexpr std::size_t max_cut = index_of("temper.max_cut");
constexpr std::size_t tilt = index_of("temper.tilt");
constexpr std::size_t attack = index_of("temper.attack");
constexpr std::size_t release = index_of("temper.release");
constexpr std::size_t attacks = index_of("temper.attacks");
constexpr std::size_t sustains = index_of("temper.sustains");
constexpr std::size_t separate = index_of("temper.separate");
constexpr std::size_t stereo = index_of("temper.stereo");
constexpr std::size_t link = index_of("temper.link");
constexpr std::size_t phase = index_of("temper.phase");
constexpr std::size_t unmask = index_of("temper.unmask");
constexpr std::size_t unmask_amount = index_of("temper.unmask_amount");
constexpr std::size_t unmask_focus = index_of("temper.unmask_focus");
constexpr std::size_t mix = index_of("temper.mix");
constexpr std::size_t trim = index_of("temper.trim");
constexpr std::size_t delta = index_of("temper.delta");
} // namespace p

constexpr std::size_t parameter_count = t::parameters.size();
using Values = std::array<double, parameter_count>;

// The analysis grid: 1/24 octave from 20 Hz over ten octaves.
constexpr double grid_low_hz = 20.0;
constexpr double grid_per_octave = 24.0;
constexpr std::size_t grid = 240;
// The panel's display: 20 Hz to 20 kHz.
constexpr double display_low_hz = 20.0;
constexpr double display_span = 1'000.0;
// Grid bands at or above this share of the rate are never cut.
constexpr double top_of_rate = native_dsp::dsp::nyquist_guard;

// Frame sizes at 48 kHz; they double with each doubling of the rate.
constexpr double base_rate = 48'000.0;
constexpr std::size_t max_scale = 4;
constexpr std::size_t base_window = 2'048;
constexpr std::size_t base_hop = 256;
constexpr std::size_t base_filter = 2'048;
constexpr std::size_t base_low_window = 1'024;
constexpr std::size_t base_low_hop = 128;
constexpr std::size_t base_low_filter = 256;
// Natural looks this share of a window ahead of the block it filters, so
// the analysis is centred near the block it shapes.
constexpr std::size_t natural_lookahead_divisor = 4;

constexpr double silence_db = -160.0;
// Bands quieter than this are never cut: room noise is not a resonance.
constexpr double floor_db = -96.0;
// Neighbourhood half-widths (grid bands) at Detail 0 and 100; the peak
// envelope's half-width, which bridges the gaps between a sound's partials
// so that a resonance is a peak in the envelope, not every harmonic; and
// the half-width the cut is smoothed over.
constexpr double reference_wide = 36.0;
constexpr double reference_narrow = 6.0;
constexpr double envelope_wide = 6.0;
constexpr double envelope_narrow = 2.0;
constexpr double cut_wide = 8.0;
constexpr double cut_narrow = 0.0;
// Detail moves this far across the spectrum at full Tilt.
constexpr double tilt_detail_span = 0.5;
// At full Tilt the far ends' times differ by this factor each way.
constexpr double tilt_time_factor = 2.0;
// Soft: an adaptive threshold (this share of the neighbourhood's mean
// deviation: a busy spectrum must stand out further) and a knee; Hard: a
// fixed threshold and more weight.
constexpr double soft_spread = 0.5;
constexpr double soft_knee_db = 6.0;
constexpr double hard_threshold_db = 1.0;
constexpr double hard_weight = 1.5;
// Depth 10 removes the whole excess.
constexpr double depth_unity = 10.0;
// A sensitivity of +6 dB doubles the cut.
constexpr double sensitivity_doubling_db = 6.0;
// Split: an attack is a band this far above its own slow average; Separate
// narrows it from wide to narrow.
constexpr double transient_slow_seconds = 0.1;
constexpr double transient_offset_db = 1.5;
constexpr double transient_wide_db = 12.0;
constexpr double transient_narrow_db = 3.0;
// Unmask: the deepest cut, the key's floor and the span over which its
// presence and its dominance fade in, and the dominance margin at Focus 0
// and 100.
constexpr double unmask_max_db = 12.0;
constexpr double unmask_floor_db = -72.0;
constexpr double unmask_fade_db = 18.0;
constexpr double unmask_dominance_db = 12.0;
constexpr double unmask_loose_db = 18.0;
constexpr double unmask_strict_db = -6.0;
// Display smoothing and meter hold.
constexpr double display_seconds = 0.05;
constexpr double hold_seconds = 1.0;
// Trim glides over this to stay click-free.
constexpr double trim_seconds = 0.01;
// The minimum-phase design floors the gain here before its log, and takes a
// fully wet gain's log straight from its dB.
constexpr double gain_floor = 1e-12;
const double log_floor = std::log(gain_floor);
constexpr double db_to_log = std::numbers::ln10 / 20.0;
// An ordered real spectrum keeps DC and Nyquist, both real, in its first
// two floats.
constexpr std::size_t dc_slot = 0;
constexpr std::size_t nyquist_slot = 1;

[[nodiscard]] std::size_t scale_for(double rate) noexcept {
  std::size_t scale = 1;
  while (scale < max_scale && rate >= base_rate * static_cast<double>(scale) * 1.5) {
    scale *= 2;
  }
  return scale;
}

[[nodiscard]] t::Phase phase_of(std::span<const double> v) noexcept {
  return static_cast<t::Phase>(std::lround(v[p::phase]));
}

// Powers at or below this read as silence.
constexpr double silent_power = 1e-16;
// 10 log10(x) = ln x times this.
constexpr double ln_to_db = 10.0 / std::numbers::ln10;
[[nodiscard]] double to_db(double power) noexcept {
  return power > silent_power ? 10.0 * std::log10(power) : silence_db;
}
[[nodiscard]] double lerp(double a, double b, double u) noexcept { return a + (b - a) * u; }
// Link's blend toward the louder channel: fully linked is exactly the
// louder, so linked channels design one filter.
[[nodiscard]] double linked(double own, double most, double link) noexcept {
  return link >= 1.0 ? most : lerp(own, most, link);
}
// Grid band b spans [grid_hz(b), grid_hz(b + 1)]; centre_hz(b) is its
// middle in log frequency.
[[nodiscard]] double grid_hz(double edge) noexcept {
  return grid_low_hz * std::exp2(edge / grid_per_octave);
}
[[nodiscard]] double centre_hz(std::size_t band) noexcept {
  return grid_hz(static_cast<double>(band) + 0.5);
}

// A dynamic band's response in dB at f for its full range: the analogue
// prototypes of the classic bell and shelves (no bilinear warping: the
// filter is designed from this curve directly).
[[nodiscard]] double band_db(int kind, double f0, double q, double gain_db, double f) noexcept {
  const double a = std::pow(10.0, gain_db / 40.0);
  const double x = f / f0;
  const double x2 = x * x;
  double num = 0.0;
  double den = 0.0;
  if (kind == t::Kind::bell) {
    const double re = (1.0 - x2) * (1.0 - x2);
    num = re + (x * a / q) * (x * a / q);
    den = re + (x / (a * q)) * (x / (a * q));
  } else {
    // H(s) = A (s^2 + sqrt(A)/Q s + A) / (A s^2 + sqrt(A)/Q s + 1), and the
    // high shelf with s -> 1/s.
    const double y2 = kind == t::Kind::low_shelf ? x2 : 1.0 / x2;
    const double y = std::sqrt(y2);
    const double k = std::sqrt(a) / q;
    const double nr = a - y2;
    const double dr = 1.0 - a * y2;
    num = a * a * (nr * nr + (k * y) * (k * y));
    den = dr * dr + (k * y) * (k * y);
  }
  return 10.0 * std::log10(num / den);
}

class TemperKernel final : public EffectKernel {
public:
  TemperKernel(std::span<const double> values, double rate)
      : rate_(rate), frames_(frames_for(values, rate)), phase_(phase_of(values)),
        hold_(std::exp(std::log(1e-3) / (hold_seconds * rate))),
        trim_step_(1.0 - std::exp(-1.0 / (trim_seconds * rate))), analysis_(frames_.window),
        design_(frames_.filter * 2), block_(frames_.hop * 2) {
    std::ranges::copy(values, values_.begin());
    const std::size_t w = frames_.window;
    const std::size_t d = design_.size();
    // Input history: the analysis window, the lookahead and the block, plus
    // the latency the dry path is read at.
    history_ = 1;
    while (history_ < w + frames_.lookahead + frames_.hop + frames_.latency + frames_.filter) {
      history_ *= 2;
    }
    for (auto& ring : ring_) {
      ring.assign(history_, 0.0);
    }
    key_ring_.assign(history_, 0.0);
    window_.resize(w);
    double sum = 0.0;
    for (std::size_t i = 0; i < w; ++i) {
      window_[i] = oscillate::native_dsp::dsp::hann(static_cast<double>(i), static_cast<double>(w));
      sum += window_[i];
    }
    // A full-scale sine reads 0 dB at its peak bin.
    power_scale_ = (2.0 / sum) * (2.0 / sum);
    for (auto& frame : frame_) {
      frame = Buffer(w);
    }
    analysis_spectrum_ = Buffer(w);
    for (auto& power : power_) {
      power.resize(w / 2 + 1);
    }
    key_power_.resize(w / 2 + 1);
    prepare_grid(w);
    // Design bins: where each falls on the grid.
    design_at_.resize(d / 2 + 1);
    design_hz_.resize(d / 2 + 1);
    for (std::size_t k = 0; k <= d / 2; ++k) {
      const double hz = static_cast<double>(k) * rate / static_cast<double>(d);
      design_hz_[k] = hz;
      // Fractional grid band, measured between band centres: below the
      // first centre the first band's cut, above the last none.
      const double u = hz > 0.0 ? std::log2(hz / grid_low_hz) * grid_per_octave - 0.5 : -1.0;
      auto& at = design_at_[k];
      if (u <= 0.0) {
        at = {0, 0.0, 1.0};
      } else if (u < static_cast<double>(grid - 1)) {
        const auto b = static_cast<std::size_t>(u);
        at = {b, u - static_cast<double>(b), 1.0};
      } else {
        at = {0, 0.0, 0.0};
      }
    }
    for (auto& shape : band_shape_) {
      shape.assign(d / 2 + 1, 0.0);
    }
    for (auto& weight : band_weight_) {
      weight.assign(w / 2 + 1, 0.0);
    }
    for (auto& gains : design_gain_) {
      gains.resize(d / 2 + 1);
    }
    design_values_ = Buffer(d / 2 + 1);
    design_work_ = Buffer(d);
    const std::size_t k = block_.size();
    block_time_ = Buffer(k);
    filter_time_ = Buffer(k);
    filter_spectrum_ = Buffer(k);
    for (std::size_t c = 0; c < 2; ++c) {
      block_spectrum_[c] = Buffer(k);
      product_[c] = Buffer(k);
    }
    landing_.resize(frames_.filter / frames_.hop);
    for (auto& landing : landing_) {
      for (auto& channel : landing) {
        channel = Buffer(k);
      }
    }
    overlap_a_.assign(frames_.hop, 0.0);
    overlap_b_.assign(frames_.hop, 0.0);
    out_.assign(frames_.hop, {});
    for (auto& h : fir_) {
      h.assign(frames_.filter, 0.0);
    }
    for (auto& h : fir_previous_) {
      h.assign(frames_.filter, 0.0);
    }
    taper_.resize(frames_.filter);
    prepare_taper();
    prepare_display();
    reset_state();
  }

  void control(std::span<const double> v, std::uint32_t, const Transport&) noexcept override {
    std::ranges::copy(v, values_.begin());
  }

  void process(std::span<engine::StereoFrame> audio,
               std::span<const engine::StereoFrame> sidechain) noexcept override {
    keyed_ = !sidechain.empty();
    const double trim_target = std::pow(10.0, values_[p::trim] / 20.0);
    for (std::size_t i = 0; i < audio.size(); ++i) {
      const double l = audio[i].left;
      const double r = audio[i].right;
      input_peak_[0] = std::max(input_peak_[0] * hold_, std::fabs(l));
      input_peak_[1] = std::max(input_peak_[1] * hold_, std::fabs(r));
      push(l, r, keyed_ ? 0.5 * (sidechain[i].left + sidechain[i].right) : 0.0);
      ++written_;
      auto [out_l, out_r] = phase_ == t::Phase::low_latency ? direct() : out_[position_];
      trim_ += (trim_target - trim_) * trim_step_;
      out_l *= trim_;
      out_r *= trim_;
      audio[i] = {static_cast<float>(out_l), static_cast<float>(out_r)};
      output_peak_[0] = std::max(output_peak_[0] * hold_, std::fabs(out_l));
      output_peak_[1] = std::max(output_peak_[1] * hold_, std::fabs(out_r));
      if (++position_ == frames_.hop) {
        position_ = 0;
        hop();
      }
    }
    max_cut_held_ *= std::pow(hold_, static_cast<double>(audio.size()));
  }

  void reset() noexcept override { reset_state(); }

  [[nodiscard]] std::size_t telemetry(std::span<float> out) const noexcept override {
    if (out.size() < telemetry::size) {
      return 0;
    }
    for (std::size_t c = 0; c < 2; ++c) {
      out[telemetry::input_peak + c] =
          static_cast<float>(native_dsp::dsp::gain_to_db(input_peak_[c]));
      out[telemetry::output_peak + c] =
          static_cast<float>(native_dsp::dsp::gain_to_db(output_peak_[c]));
    }
    out[telemetry::max_cut] = static_cast<float>(max_cut_held_);
    out[telemetry::keyed] = keyed_ ? 1.0F : 0.0F;
    out[telemetry::transient] = static_cast<float>(transient_now_);
    out[telemetry::latency_ms] = static_cast<float>(1'000.0 * frames_.latency / rate_);
    for (std::size_t j = 0; j < bands; ++j) {
      out[telemetry::band_gain + j] = static_cast<float>(band_now_[0][j]);
      out[telemetry::band_level + j] = static_cast<float>(band_level_[j]);
    }
    for (std::size_t i = 0; i < display_bands; ++i) {
      out[telemetry::input_spectrum + i] = static_cast<float>(display_in_[i]);
      out[telemetry::output_spectrum + i] = static_cast<float>(display_out_[i]);
      out[telemetry::sidechain_spectrum + i] = static_cast<float>(display_key_[i]);
      out[telemetry::gain + i] = static_cast<float>(display_gain_[i]);
    }
    return telemetry::size;
  }

private:
  [[nodiscard]] t::StereoMode stereo_mode() const noexcept {
    return static_cast<t::StereoMode>(std::lround(values_[p::stereo]));
  }

  // The processed pair (a, b) and the passing component for this mode.
  struct Split {
    double a = 0.0;
    double b = 0.0;
  };
  [[nodiscard]] static Split split(t::StereoMode mode, double l, double r) noexcept {
    switch (mode) {
    case t::StereoMode::mid:
      return {0.5 * (l + r), 0.5 * (l - r)};
    case t::StereoMode::side:
      return {0.5 * (l - r), 0.5 * (l + r)};
    case t::StereoMode::left:
      return {l, r};
    case t::StereoMode::right:
      return {r, l};
    case t::StereoMode::stereo:
      break;
    }
    return {l, r};
  }
  [[nodiscard]] static std::pair<double, double> join(t::StereoMode mode, double a,
                                                      double b) noexcept {
    switch (mode) {
    case t::StereoMode::mid:
      return {a + b, a - b};
    case t::StereoMode::side:
      return {b + a, b - a};
    case t::StereoMode::left:
      return {a, b};
    case t::StereoMode::right:
      return {b, a};
    case t::StereoMode::stereo:
      break;
    }
    return {a, b};
  }
  [[nodiscard]] std::size_t processed() const noexcept {
    return mode_ == t::StereoMode::stereo ? 2 : 1;
  }

  void push(double l, double r, double key) noexcept {
    const std::size_t at = written_ & (history_ - 1);
    ring_[0][at] = l;
    ring_[1][at] = r;
    key_ring_[at] = key;
  }
  // The sample `back` frames before the newest one (0 is the newest).
  [[nodiscard]] double past(const std::vector<double>& ring, std::size_t back) const noexcept {
    return ring[(written_ - 1 - back) & (history_ - 1)];
  }

  void prepare_grid(std::size_t w) {
    const double bin_hz = rate_ / static_cast<double>(w);
    const double top = top_of_rate * rate_;
    for (std::size_t b = 0; b < grid; ++b) {
      const double lo = grid_hz(static_cast<double>(b));
      const double hi = grid_hz(static_cast<double>(b + 1));
      const double centre = centre_hz(b);
      auto& g = grid_[b];
      g.live = centre < top;
      g.first = static_cast<std::size_t>(std::ceil(lo / bin_hz));
      g.last = static_cast<std::size_t>(std::floor(hi / bin_hz));
      g.last = std::min(g.last, w / 2);
      const double at = centre / bin_hz;
      g.bin = std::min(static_cast<std::size_t>(at), w / 2 - 1);
      g.frac = std::min(1.0, at - static_cast<double>(g.bin));
    }
  }

  // Each display band's centre and the grid bands whose centres it holds.
  void prepare_display() noexcept {
    const auto grid_at = [](double hz) {
      return std::log2(hz / grid_low_hz) * grid_per_octave - 0.5;
    };
    const auto display_hz = [](double edge) {
      return display_low_hz * std::pow(display_span, edge / static_cast<double>(display_bands));
    };
    for (std::size_t i = 0; i < display_bands; ++i) {
      const double hz = display_hz(static_cast<double>(i) + 0.5);
      display_[i] = {hz, grid_at(hz),
                     std::max(0.0, std::ceil(grid_at(display_hz(static_cast<double>(i))))),
                     std::min(static_cast<double>(grid - 1),
                              std::floor(grid_at(display_hz(static_cast<double>(i + 1)))))};
    }
  }

  void prepare_taper() {
    const std::size_t m = frames_.filter;
    if (phase_ == t::Phase::linear) {
      for (std::size_t n = 0; n < m; ++n) {
        taper_[n] =
            oscillate::native_dsp::dsp::hann(static_cast<double>(n), static_cast<double>(m));
      }
      return;
    }
    // Minimum phase: whole, then a half-Hann over the last quarter.
    const std::size_t fade = m / 4;
    for (std::size_t n = 0; n < m; ++n) {
      const std::size_t into = n + fade >= m ? n + fade - m : 0;
      taper_[n] = n + fade < m ? 1.0
                               : 0.5 + 0.5 * std::cos(std::numbers::pi * static_cast<double>(into) /
                                                      static_cast<double>(fade));
    }
  }

  void reset_state() noexcept {
    for (auto& ring : ring_) {
      std::ranges::fill(ring, 0.0);
    }
    std::ranges::fill(key_ring_, 0.0);
    for (auto& landing : landing_) {
      for (auto& channel : landing) {
        channel.zero();
      }
    }
    landed_ = 0;
    std::ranges::fill(overlap_a_, 0.0);
    std::ranges::fill(overlap_b_, 0.0);
    std::ranges::fill(out_, std::pair<double, double>{});
    for (std::size_t c = 0; c < 2; ++c) {
      std::fill(cut_[c].begin(), cut_[c].end(), 0.0);
      std::fill(slow_[c].begin(), slow_[c].end(), silence_db);
      std::fill(band_now_[c].begin(), band_now_[c].end(), 0.0);
      // An identity filter until the first hop designs one.
      std::fill(fir_[c].begin(), fir_[c].end(), 0.0);
      fir_[c][phase_ == t::Phase::linear ? frames_.filter / 2 : 0] = 1.0;
      fir_previous_[c] = fir_[c];
    }
    display_in_.fill(silence_db);
    display_out_.fill(silence_db);
    display_key_.fill(silence_db);
    display_gain_.fill(0.0);
    band_level_.fill(silence_db);
    input_peak_ = {};
    output_peak_ = {};
    max_cut_held_ = 0.0;
    transient_now_ = 0.0;
    position_ = 0;
    written_ = 0;
    // The mode a hop runs in; the first hop adopts the current one.
    mode_ = stereo_mode();
    trim_ = std::pow(10.0, values_[p::trim] / 20.0);
  }

  // ---- analysis -----------------------------------------------------------

  // Levels on the grid of the newest window of the processed channels (and
  // the key), in dB.
  void analyse() noexcept {
    const std::size_t w = frames_.window;
    const std::size_t channels = processed();
    for (std::size_t i = 0; i < w; ++i) {
      const std::size_t back = w - 1 - i;
      const auto s = split(mode_, past(ring_[0], back), past(ring_[1], back));
      frame_[0][i] = static_cast<float>(s.a * window_[i]);
      frame_[1][i] = static_cast<float>(s.b * window_[i]);
    }
    for (std::size_t c = 0; c < channels; ++c) {
      spectrum_power(frame_[c], power_[c]);
      levels(power_[c], level_[c]);
    }
    if (keyed_) {
      for (std::size_t i = 0; i < w; ++i) {
        frame_[0][i] = static_cast<float>(past(key_ring_, w - 1 - i) * window_[i]);
      }
      spectrum_power(frame_[0], key_power_);
      levels(key_power_, key_level_);
    } else {
      key_level_.fill(silence_db);
    }
  }

  // A windowed frame's power per bin, a full-scale sine 0 dB at its peak.
  void spectrum_power(const Buffer& frame, std::vector<double>& power) noexcept {
    const std::size_t w = frames_.window;
    analysis_.forward(frame.data(), analysis_spectrum_.data());
    for (std::size_t k = 0; k <= w / 2; ++k) {
      power[k] =
          static_cast<double>(warp_dsp::bin_power(analysis_spectrum_.data(), w, k)) * power_scale_;
    }
  }

  // The mean power of the bins inside each grid band, or the power read
  // between the two bins around its centre when it holds none.
  // The logarithms run four at a time.
  void levels(const std::vector<double>& power, std::array<double, grid>& out) noexcept {
    for (std::size_t b = 0; b < grid; ++b) {
      const auto& g = grid_[b];
      double mean = 0.0;
      if (g.last >= g.first) {
        for (std::size_t k = g.first; k <= g.last; ++k) {
          mean += power[k];
        }
        mean /= static_cast<double>(g.last - g.first + 1);
      } else {
        mean = lerp(power[g.bin], power[g.bin + 1], g.frac);
      }
      out[b] = mean;
      level_log_[b] = static_cast<float>(std::max(mean, silent_power));
    }
    native_dsp::dsp::vector_math::log_all(level_log_);
    for (std::size_t b = 0; b < grid; ++b) {
      out[b] = out[b] > silent_power ? static_cast<double>(level_log_[b]) * ln_to_db : silence_db;
    }
  }

  // ---- gains --------------------------------------------------------------

  using Nodes = std::array<std::pair<double, double>, t::sensitivity_nodes>;

  // The sensitivity nodes (frequency, dB), sorted by frequency.
  [[nodiscard]] Nodes nodes() const noexcept {
    Nodes out{};
    for (std::size_t n = 0; n < t::sensitivity_nodes; ++n) {
      const std::size_t base = t::sensitivity_base + n * t::per_node;
      out[n] = {values_[base + t::node_freq], values_[base + t::node_gain]};
    }
    std::ranges::sort(out);
    return out;
  }

  // Cosine steps between the nodes in log frequency; flat beyond the ends.
  [[nodiscard]] static double sensitivity_db(const Nodes& nodes, double hz) noexcept {
    if (hz <= nodes.front().first) {
      return nodes.front().second;
    }
    if (hz >= nodes.back().first) {
      return nodes.back().second;
    }
    for (std::size_t n = 0; n + 1 < nodes.size(); ++n) {
      if (hz <= nodes[n + 1].first) {
        const double a = std::log(nodes[n].first);
        const double b = std::log(nodes[n + 1].first);
        const double u = b > a ? (std::log(hz) - a) / (b - a) : 1.0;
        const double k = 0.5 - 0.5 * std::cos(u * std::numbers::pi);
        return lerp(nodes[n].second, nodes[n + 1].second, k);
      }
    }
    return nodes.back().second;
  }

  // Where band b sits across the grid, from -0.5 (lowest) to 0.5 (highest).
  [[nodiscard]] static double position_of(std::size_t b) noexcept {
    return static_cast<double>(b) / static_cast<double>(grid - 1) - 0.5;
  }

  // The summed distance of `values` from `centre`, in four running sums
  // (one chain of additions would wait on each in turn).
  [[nodiscard]] static double deviation(std::span<const double> values, double centre) noexcept {
    constexpr std::size_t lanes = 4;
    std::array<double, lanes> sums{};
    std::size_t j = 0;
    for (; j + lanes <= values.size(); j += lanes) {
      for (std::size_t l = 0; l < lanes; ++l) {
        sums[l] += std::fabs(values[j + l] - centre);
      }
    }
    for (; j < values.size(); ++j) {
      sums[0] += std::fabs(values[j] - centre);
    }
    return (sums[0] + sums[1]) + (sums[2] + sums[3]);
  }

  // The suppressor's wanted cut per channel on the grid (dB, positive).
  void suppress() noexcept {
    const std::size_t channels = processed();
    const double tilt = values_[p::tilt];
    const bool hard = std::lround(values_[p::mode]) == t::Mode::hard;
    const double depth = values_[p::depth] / depth_unity;
    const double max_cut = values_[p::max_cut];
    const double slow =
        1.0 - std::exp(-static_cast<double>(frames_.hop) / (transient_slow_seconds * rate_));
    const double transient_span =
        lerp(transient_wide_db, transient_narrow_db, values_[p::separate]);
    const double attacks = values_[p::attacks];
    const double sustains = values_[p::sustains];
    // The sensitivity curve, again only when its nodes move.
    const Nodes sorted = nodes();
    if (!sensitivity_known_ || sorted != sensitivity_nodes_) {
      for (std::size_t b = 0; b < grid; ++b) {
        sensitivity_[b] = std::exp2(sensitivity_db(sorted, centre_hz(b)) / sensitivity_doubling_db);
      }
      sensitivity_nodes_ = sorted;
      sensitivity_known_ = true;
    }
    const auto& sensitivity = sensitivity_;
    double transient_sum = 0.0;
    std::size_t transient_count = 0;
    // Each band's Detail and the widths it sets, the same for every channel,
    // again only when Detail or Tilt move.
    const Widths widths{values_[p::detail], tilt};
    if (!(widths == widths_)) {
      for (std::size_t b = 0; b < grid; ++b) {
        const double detail =
            std::clamp(widths.detail + tilt * tilt_detail_span * position_of(b) * 2.0, 0.0, 1.0);
        envelope_reach_[b] =
            static_cast<std::size_t>(std::lround(lerp(envelope_wide, envelope_narrow, detail)));
        reference_half_[b] =
            static_cast<std::size_t>(std::lround(lerp(reference_wide, reference_narrow, detail)));
        smooth_half_[b] = lerp(cut_wide, cut_narrow, detail);
        smooth_reach_[b] = static_cast<std::size_t>(std::ceil(smooth_half_[b]));
      }
      smooth_reach_most_ = std::ranges::max(smooth_reach_);
      widths_ = widths;
    }
    for (std::size_t c = 0; c < channels; ++c) {
      const auto& level = level_[c];
      auto& envelope = envelope_[c];
      for (std::size_t b = 0; b < grid; ++b) {
        const std::size_t reach = envelope_reach_[b];
        double most = silence_db;
        for (std::size_t j = b > reach ? b - reach : 0; j < std::min(grid, b + reach + 1); ++j) {
          most = std::max(most, level[j]);
        }
        envelope[b] = most;
      }
      std::array<double, grid + 1> sum{};
      for (std::size_t b = 0; b < grid; ++b) {
        sum[b + 1] = sum[b] + envelope[b];
      }
      auto& over = want_[c];
      for (std::size_t b = 0; b < grid; ++b) {
        const std::size_t half = reference_half_[b];
        const std::size_t lo = b > half ? b - half : 0;
        const std::size_t hi = std::min(grid, b + half + 1);
        const double n = static_cast<double>(hi - lo);
        const double reference = (sum[hi] - sum[lo]) / n;
        // The band's own level against the envelope's mean: a peak of the
        // envelope is cut where it actually sounds, not across its width.
        const double excess = level[b] - reference;
        double cut = 0.0;
        if (grid_[b].live && level[b] > floor_db && excess > 0.0) {
          if (hard) {
            cut = std::max(0.0, excess - hard_threshold_db) * hard_weight;
          } else {
            const double spread = deviation(std::span(envelope).subspan(lo, hi - lo), reference);
            const double above = std::max(0.0, excess - soft_spread * spread / n);
            cut = above * above / (above + soft_knee_db);
          }
        }
        // Split: attacks against sustains, per band.
        slow_[c][b] += (level[b] - slow_[c][b]) * slow;
        const double rise = level[b] - slow_[c][b] - transient_offset_db;
        const double transient = std::clamp(rise / transient_span, 0.0, 1.0);
        if (level[b] > floor_db) {
          transient_sum += transient;
          ++transient_count;
        }
        over[b] = cut * depth * sensitivity[b] * lerp(sustains, attacks, transient);
      }
      // Widen the cut across frequency (by Detail) with tapered skirts: each
      // band keeps the deepest of its neighbours' cuts, scaled down with
      // distance, so a one-band resonance keeps its full depth. Only bands
      // with a cut reach their neighbours.
      auto& smoothed = smoothed_[c];
      smoothed.fill(0.0);
      for (std::size_t j = 0; j < grid; ++j) {
        if (!(over[j] > 0.0)) {
          continue;
        }
        const std::size_t from = j > smooth_reach_most_ ? j - smooth_reach_most_ : 0;
        const std::size_t to = std::min(grid, j + smooth_reach_most_ + 1);
        for (std::size_t b = from; b < to; ++b) {
          const std::size_t distance = b > j ? b - j : j - b;
          if (distance > smooth_reach_[b]) {
            continue;
          }
          const double d = static_cast<double>(distance);
          smoothed[b] =
              std::max(smoothed[b], over[j] * std::max(0.0, 1.0 - d / (smooth_half_[b] + 1.0)));
        }
      }
      for (double& cut : smoothed) {
        cut = std::min(max_cut, cut);
      }
    }
    transient_now_ =
        transient_count > 0 ? transient_sum / static_cast<double>(transient_count) : 0.0;
    // Link the two channels' cuts.
    if (channels == 2) {
      const double link = values_[p::link];
      for (std::size_t b = 0; b < grid; ++b) {
        const double most = std::max(smoothed_[0][b], smoothed_[1][b]);
        smoothed_[0][b] = linked(smoothed_[0][b], most, link);
        smoothed_[1][b] = linked(smoothed_[1][b], most, link);
      }
    }
    // Unmask: cut where the key lives and is not drowned out.
    if (keyed_ && std::lround(values_[p::unmask]) != 0) {
      const double amount = values_[p::unmask_amount] * unmask_max_db;
      const double margin = lerp(unmask_loose_db, unmask_strict_db, values_[p::unmask_focus]);
      for (std::size_t c = 0; c < channels; ++c) {
        for (std::size_t b = 0; b < grid; ++b) {
          if (!grid_[b].live) {
            continue;
          }
          const double present =
              std::clamp((key_level_[b] - unmask_floor_db) / unmask_fade_db, 0.0, 1.0);
          const double dominant =
              std::clamp((key_level_[b] - level_[c][b] + margin) / unmask_dominance_db, 0.0, 1.0);
          smoothed_[c][b] += amount * present * dominant;
        }
      }
    }
    // Ballistics, leaned by Tilt: each band's coefficients, again only when
    // the times or the tilt move.
    const Ballistics now{values_[p::attack], values_[p::release], tilt};
    if (!(now == ballistics_)) {
      const double attack_s = now.attack_ms / 1'000.0;
      const double release_s = now.release_ms / 1'000.0;
      const double hop = static_cast<double>(frames_.hop);
      for (std::size_t b = 0; b < grid; ++b) {
        const double skew = std::pow(tilt_time_factor, -tilt * position_of(b) * 2.0);
        attack_coefficient_[b] = 1.0 - std::exp(-hop / ((attack_s * skew) * rate_));
        release_coefficient_[b] = 1.0 - std::exp(-hop / ((release_s * skew) * rate_));
      }
      ballistics_ = now;
    }
    double deepest = 0.0;
    for (std::size_t c = 0; c < channels; ++c) {
      for (std::size_t b = 0; b < grid; ++b) {
        const double want = smoothed_[c][b];
        const double coefficient =
            want > cut_[c][b] ? attack_coefficient_[b] : release_coefficient_[b];
        cut_[c][b] += (want - cut_[c][b]) * coefficient;
        deepest = std::max(deepest, cut_[c][b]);
      }
    }
    max_cut_held_ = std::max(max_cut_held_, deepest);
  }

  // Each dynamic band's shape (dB at its full range) on the design bins and
  // its detection weights on the analysis bins, when its settings change.
  void shape_bands() noexcept {
    const std::size_t w = frames_.window;
    const double nyquist_safe = top_of_rate * rate_;
    for (std::size_t j = 0; j < bands; ++j) {
      const std::size_t base = t::band_base + j * t::per_band;
      BandSettings now{std::lround(values_[base + t::on]) != 0,
                       static_cast<int>(std::lround(values_[base + t::kind])),
                       std::min(values_[base + t::freq], nyquist_safe), values_[base + t::q],
                       values_[base + t::range]};
      if (now == band_settings_[j]) {
        continue;
      }
      band_settings_[j] = now;
      if (!now.on || now.range == 0.0) {
        continue;
      }
      for (std::size_t k = 0; k < design_hz_.size(); ++k) {
        const double hz = std::max(design_hz_[k], grid_low_hz / 2.0);
        band_shape_[j][k] = band_db(now.kind, now.freq, now.q, now.range, hz);
      }
      for (std::size_t k = 0; k <= w / 2; ++k) {
        const double hz =
            std::max(static_cast<double>(k) * rate_ / static_cast<double>(w), grid_low_hz / 2.0);
        band_weight_[j][k] =
            std::clamp(band_db(now.kind, now.freq, now.q, now.range, hz) / now.range, 0.0, 1.0);
      }
    }
  }

  void dynamic_bands() noexcept {
    const std::size_t channels = processed();
    const std::size_t w = frames_.window;
    const double hop = static_cast<double>(frames_.hop);
    const double attack = 1.0 - std::exp(-hop / (values_[p::attack] / 1'000.0 * rate_));
    const double release = 1.0 - std::exp(-hop / (values_[p::release] / 1'000.0 * rate_));
    const double link = values_[p::link];
    for (std::size_t j = 0; j < bands; ++j) {
      const auto& band = band_settings_[j];
      if (!band.on || band.range == 0.0) {
        band_now_[0][j] = 0.0;
        band_now_[1][j] = 0.0;
        band_level_[j] = silence_db;
        continue;
      }
      const std::size_t base = t::band_base + j * t::per_band;
      const double threshold = values_[base + t::threshold];
      const double slope = 1.0 - 1.0 / values_[base + t::ratio];
      std::array<double, 2> level{silence_db, silence_db};
      for (std::size_t c = 0; c < channels; ++c) {
        double energy = 0.0;
        for (std::size_t k = 0; k <= w / 2; ++k) {
          energy += band_weight_[j][k] * power_[c][k];
        }
        level[c] = to_db(energy);
      }
      if (channels == 2) {
        const double most = std::max(level[0], level[1]);
        level[0] = linked(level[0], most, link);
        level[1] = linked(level[1], most, link);
      }
      band_level_[j] = std::max(level[0], channels == 2 ? level[1] : silence_db);
      for (std::size_t c = 0; c < channels; ++c) {
        const double magnitude =
            std::min(std::fabs(band.range), std::max(0.0, level[c] - threshold) * slope);
        const double target = std::copysign(magnitude, band.range);
        double& now = band_now_[c][j];
        now += (target - now) * (std::fabs(target) > std::fabs(now) ? attack : release);
      }
    }
  }

  // ---- filters ------------------------------------------------------------

  // The gain (dB, unmixed) per design bin for channel c.
  void gains_db(std::size_t c) noexcept {
    auto& out = design_gain_[c];
    const auto& cut = cut_[c];
    for (std::size_t k = 0; k < out.size(); ++k) {
      const auto& at = design_at_[k];
      out[k] = -at.weight * lerp(cut[at.band], cut[at.band + 1], at.frac);
    }
    for (std::size_t j = 0; j < bands; ++j) {
      const auto& band = band_settings_[j];
      if (!band.on || band.range == 0.0) {
        continue;
      }
      const double share = band_now_[c][j] / band.range;
      const auto& shape = band_shape_[j];
      for (std::size_t k = 0; k < out.size(); ++k) {
        out[k] += shape[k] * share;
      }
    }
  }

  // Each design bin's applied gain for channel c into design_values_:
  // linear and mixed, or (`as_log`) its natural log, floored. Fully wet, the
  // log comes straight from the dB.
  void applied_gains(std::size_t c, bool as_log) noexcept {
    const auto& gain_db = design_gain_[c];
    const auto values = design_values_.span();
    const double mix = values_[p::mix];
    if (as_log && mix == 1.0) {
      for (std::size_t k = 0; k < gain_db.size(); ++k) {
        values[k] = static_cast<float>(std::max(gain_db[k] * db_to_log, log_floor));
      }
      return;
    }
    for (std::size_t k = 0; k < gain_db.size(); ++k) {
      values[k] = static_cast<float>(gain_db[k] * db_to_log);
    }
    native_dsp::dsp::vector_math::exp_all(values);
    const auto dry = static_cast<float>(1.0 - mix);
    const auto wet = static_cast<float>(mix);
    const auto floor = static_cast<float>(gain_floor);
    for (float& value : values) {
      value = dry + wet * value;
      if (as_log) {
        value = std::max(value, floor);
      }
    }
    if (as_log) {
      native_dsp::dsp::vector_math::log_all(values);
    }
  }

  // An ordered spectrum of real, even values from design_values_ (bins 0
  // to d / 2) in design_work_.
  void even_spectrum() noexcept {
    const std::size_t d = design_.size();
    float* const z = design_work_.data();
    z[dc_slot] = design_values_[0];
    z[nyquist_slot] = design_values_[d / 2];
    for (std::size_t k = 1; k < d / 2; ++k) {
      z[2 * k] = design_values_[k];
      z[2 * k + 1] = 0.0F;
    }
  }

  // Designs each processed channel's filter into fir_; linked channels
  // with the same cuts share one.
  void design() noexcept {
    const std::size_t channels = shared_ ? 1 : processed();
    const std::size_t d = design_.size();
    const std::size_t m = frames_.filter;
    const double unscale = 1.0 / static_cast<double>(d);
    float* const z = design_work_.data();
    for (std::size_t c = 0; c < channels; ++c) {
      gains_db(c);
      auto& fir = fir_[c];
      if (phase_ == t::Phase::linear) {
        applied_gains(c, false);
        even_spectrum();
        design_.inverse(z, z);
        for (std::size_t n = 0; n < m; ++n) {
          fir[n] = static_cast<double>(z[(n + d - m / 2) % d]) * unscale * taper_[n];
        }
        continue;
      }
      // Minimum phase by the folded cepstrum: the log gains' cepstrum, its
      // negative times folded onto the positive, back to a log spectrum,
      // exponentiated, and back to time.
      applied_gains(c, true);
      even_spectrum();
      design_.inverse(z, z);
      const auto once = static_cast<float>(unscale);
      const auto twice = static_cast<float>(2.0 * unscale);
      z[0] *= once;
      for (std::size_t n = 1; n < d / 2; ++n) {
        z[n] *= twice;
      }
      z[d / 2] *= once;
      std::fill(z + d / 2 + 1, z + d, 0.0F);
      design_.forward(z, z);
      const float dc = std::exp(z[dc_slot]);
      const float nyquist = std::exp(z[nyquist_slot]);
      native_dsp::dsp::vector_math::complex_exp(design_work_.span());
      z[dc_slot] = dc;
      z[nyquist_slot] = nyquist;
      design_.inverse(z, z);
      for (std::size_t n = 0; n < m; ++n) {
        fir[n] = static_cast<double>(z[n]) * unscale * taper_[n];
      }
    }
    if (shared_) {
      fir_[1] = fir_[0];
    }
  }

  // Low Latency: both channels' current filters, crossfaded from the last
  // hop's over this one, run sample by sample.
  [[nodiscard]] std::pair<double, double> direct() noexcept {
    const std::size_t m = frames_.filter;
    const double fade = static_cast<double>(position_ + 1) / static_cast<double>(frames_.hop);
    double now_a = 0.0;
    double now_b = 0.0;
    double was_a = 0.0;
    double was_b = 0.0;
    const std::size_t mask = history_ - 1;
    const std::size_t newest = written_ - 1;
    const std::size_t channels = processed();
    double passing = 0.0;
    for (std::size_t k = 0; k < m; ++k) {
      const std::size_t at = (newest - k) & mask;
      const auto s = split(mode_, ring_[0][at], ring_[1][at]);
      now_a += fir_[0][k] * s.a;
      was_a += fir_previous_[0][k] * s.a;
      if (channels == 2) {
        now_b += fir_[1][k] * s.b;
        was_b += fir_previous_[1][k] * s.b;
      } else if (k == 0) {
        passing = s.b;
      }
    }
    const double a = lerp(was_a, now_a, fade);
    const double b = channels == 2 ? lerp(was_b, now_b, fade) : passing;
    const auto dry = split(mode_, ring_[0][newest & mask], ring_[1][newest & mask]);
    return finish(dry, a, b);
  }

  // Delta and the return to left and right.
  [[nodiscard]] std::pair<double, double> finish(Split dry, double a, double b) const noexcept {
    if (std::lround(values_[p::delta]) != 0) {
      a = dry.a - a;
      b = processed() == 2 ? dry.b - b : 0.0;
    }
    return join(mode_, a, b);
  }

  // Natural and Linear: filter the hop's block (lookahead behind the newest
  // input) by overlap-add and queue it as the next hop's output. The filter
  // runs in partitions a hop long, each through a transform of two hops:
  // the block's spectrum times each partition's goes into the spectrum of
  // the hop its product lands in, and each hop's sum comes back to time
  // once. That is the block's whole convolution with this hop's filter, as
  // one transform of the filter's length would give it, in a third of the
  // transforms' work.
  void overlap_add() noexcept {
    const std::size_t k = block_.size();
    const std::size_t h = frames_.hop;
    const std::size_t parts = frames_.filter / h;
    const std::size_t channels = processed();
    // The block's newest sample is `lookahead` behind the newest input.
    const std::size_t first_back = frames_.lookahead + h - 1;
    // The products carry the inverse's 1 / k.
    const float unscale = 1.0F / static_cast<float>(k);
    for (std::size_t c = 0; c < channels; ++c) {
      for (std::size_t n = 0; n < h; ++n) {
        const std::size_t back = first_back - n;
        const auto s = split(mode_, past(ring_[0], back), past(ring_[1], back));
        block_time_[n] = static_cast<float>(c == 0 ? s.a : s.b);
      }
      std::fill(block_time_.data() + h, block_time_.data() + k, 0.0F);
      block_.forward_unordered(block_time_.data(), block_spectrum_[c].data());
    }
    const std::size_t filters = shared_ ? 1 : channels;
    for (std::size_t part = 0; part < parts; ++part) {
      // The hop this part's product lands in, from the hop now.
      const std::size_t lands = landed_ + part < parts ? landed_ + part : landed_ + part - parts;
      for (std::size_t f = 0; f < filters; ++f) {
        for (std::size_t n = 0; n < h; ++n) {
          filter_time_[n] = static_cast<float>(fir_[f][part * h + n]);
        }
        std::fill(filter_time_.data() + h, filter_time_.data() + k, 0.0F);
        block_.forward_unordered(filter_time_.data(), filter_spectrum_.data());
        for (std::size_t c = f; c < (shared_ ? channels : f + 1); ++c) {
          block_.multiply_accumulate(block_spectrum_[c].data(), filter_spectrum_.data(),
                                     landing_[lands][c].data(), unscale);
        }
      }
    }
    // This hop's sum back to time: its first hop is out now, its second
    // overlaps the next.
    for (std::size_t c = 0; c < channels; ++c) {
      auto& now = landing_[landed_][c];
      block_.inverse_unordered(now.data(), product_[c].data());
      now.zero();
    }
    if (++landed_ == parts) {
      landed_ = 0;
    }
    // The next hop's output. Its frame n leaves n frames after the newest
    // input; the dry it is heard against is the input a latency earlier.
    const std::size_t latency = frames_.latency;
    for (std::size_t n = 0; n < h; ++n) {
      const std::size_t back = latency - 1 - n;
      const auto dry = split(mode_, past(ring_[0], back), past(ring_[1], back));
      const double a = static_cast<double>(product_[0][n]) + overlap_a_[n];
      const double b = channels == 2 ? static_cast<double>(product_[1][n]) + overlap_b_[n] : dry.b;
      out_[n] = finish(dry, a, b);
      overlap_a_[n] = product_[0][h + n];
      overlap_b_[n] = channels == 2 ? product_[1][h + n] : 0.0F;
    }
  }

  // ---- the hop --------------------------------------------------------------

  void hop() noexcept {
    mode_ = stereo_mode();
    analyse();
    shape_bands();
    suppress();
    dynamic_bands();
    if (processed() == 1) {
      cut_[1] = cut_[0];
      band_now_[1] = band_now_[0];
    }
    // Low Latency crossfades from the last hop's filters.
    if (phase_ == t::Phase::low_latency) {
      fir_previous_ = fir_;
    }
    shared_ = processed() == 2 && cut_[0] == cut_[1] && band_now_[0] == band_now_[1];
    design();
    if (phase_ != t::Phase::low_latency) {
      overlap_add();
    }
    show();
  }

  // Each display band shows the loudest grid band (and the deepest cut)
  // whose centre lies inside it, or the grid read at its centre when it
  // holds none. The grid spans 20 Hz to 20.48 kHz, the display 20 Hz to
  // 20 kHz.
  void show() noexcept {
    const double step =
        1.0 - std::exp(-static_cast<double>(frames_.hop) / (display_seconds * rate_));
    const std::size_t channels = processed();
    const double mix = values_[p::mix];
    const auto sample = [](const std::array<double, grid>& on, double u) {
      if (u <= 0.0) {
        return on[0];
      }
      if (u >= static_cast<double>(grid - 1)) {
        return on[grid - 1];
      }
      const auto b = static_cast<std::size_t>(u);
      return lerp(on[b], on[b + 1], u - static_cast<double>(b));
    };
    for (std::size_t i = 0; i < display_bands; ++i) {
      const auto& [hz, centre, first, last] = display_[i];
      double in = silence_db;
      double key = silence_db;
      double cut = 0.0;
      const auto take = [&](const auto& read) {
        for (std::size_t c = 0; c < channels; ++c) {
          in = std::max(in, read(level_[c]));
          cut = std::max(cut, read(cut_[c]));
        }
        key = std::max(key, read(key_level_));
      };
      if (last >= first) {
        for (auto b = static_cast<std::size_t>(first); b <= static_cast<std::size_t>(last); ++b) {
          take([b](const std::array<double, grid>& on) { return on[b]; });
        }
      } else {
        take([&](const std::array<double, grid>& on) { return sample(on, centre); });
      }
      double gain = -cut;
      for (std::size_t j = 0; j < bands; ++j) {
        const auto& band = band_settings_[j];
        if (band.on && band.range != 0.0) {
          gain += band_db(band.kind, band.freq, band.q, band.range, hz) *
                  (band_now_[0][j] / band.range);
        }
      }
      // As heard: Mix blends the gain (fully wet, the gain itself).
      const double applied =
          mix == 1.0 ? gain
                     : native_dsp::dsp::gain_to_db(
                           (1.0 - mix) + mix * oscillate::native_dsp::dsp::db_to_gain(gain));
      display_in_[i] += (in - display_in_[i]) * step;
      display_key_[i] += (key - display_key_[i]) * step;
      display_out_[i] += (in + applied - display_out_[i]) * step;
      display_gain_[i] = applied;
    }
  }

  struct GridBand {
    std::size_t first = 1;
    std::size_t last = 0;
    std::size_t bin = 0;
    double frac = 0.0;
    bool live = false;
  };
  struct BandSettings {
    bool on = false;
    int kind = t::Kind::bell;
    double freq = 0.0;
    double q = 0.0;
    double range = 0.0;
    bool operator==(const BandSettings&) const = default;
  };

  double rate_;
  Frames frames_;
  t::Phase phase_;
  double hold_;
  double trim_step_;
  Values values_{};
  t::StereoMode mode_ = t::StereoMode::stereo;

  warp_dsp::RealFft analysis_;
  warp_dsp::RealFft design_;
  warp_dsp::RealFft block_;
  std::size_t history_ = 0;
  std::array<std::vector<double>, 2> ring_;
  std::vector<double> key_ring_;
  std::vector<double> window_;
  double power_scale_ = 1.0;
  // The processed pair's windowed frames (the key's in the first), and a
  // frame's spectrum.
  std::array<Buffer, 2> frame_;
  Buffer analysis_spectrum_;
  std::array<std::vector<double>, 2> power_;
  std::vector<double> key_power_;
  std::array<GridBand, grid> grid_{};
  std::array<std::array<double, grid>, 2> level_{};
  std::array<float, grid> level_log_{};
  std::array<double, grid> key_level_{};
  std::array<std::array<double, grid>, 2> slow_{};
  std::array<std::array<double, grid>, 2> want_{};
  std::array<std::array<double, grid>, 2> smoothed_{};
  std::array<double, grid> smooth_half_{};
  std::array<std::size_t, grid> envelope_reach_{};
  std::array<std::size_t, grid> smooth_reach_{};
  std::size_t smooth_reach_most_ = 0;
  std::array<std::size_t, grid> reference_half_{};
  std::array<std::array<double, grid>, 2> envelope_{};
  std::array<std::array<double, grid>, 2> cut_{};

  // What suppress() keeps between hops while its settings stay.
  struct Ballistics {
    double attack_ms = -1.0;
    double release_ms = -1.0;
    double tilt = 0.0;
    bool operator==(const Ballistics&) const = default;
  };
  Ballistics ballistics_{};
  struct Widths {
    double detail = -1.0;
    double tilt = 0.0;
    bool operator==(const Widths&) const = default;
  };
  Widths widths_{};
  std::array<double, grid> attack_coefficient_{};
  std::array<double, grid> release_coefficient_{};
  Nodes sensitivity_nodes_{};
  std::array<double, grid> sensitivity_{};
  bool sensitivity_known_ = false;

  std::array<BandSettings, bands> band_settings_{};
  std::array<std::vector<double>, bands> band_shape_;
  std::array<std::vector<double>, bands> band_weight_;
  std::array<std::array<double, bands>, 2> band_now_{};
  std::array<double, bands> band_level_{};

  // Where each design bin reads the grid's cut: between two bands, each
  // weighted; the weight is 0 above the grid.
  struct GridPoint {
    std::size_t band = 0;
    double frac = 0.0;
    double weight = 0.0;
  };
  std::vector<GridPoint> design_at_;
  std::vector<double> design_hz_;
  std::array<std::vector<double>, 2> design_gain_;
  // One channel's gains or logs per design bin, and the design's transform.
  Buffer design_values_;
  Buffer design_work_;
  std::vector<double> taper_;
  std::array<std::vector<double>, 2> fir_;
  std::array<std::vector<double>, 2> fir_previous_;
  // Both processed channels have this hop's one filter.
  bool shared_ = false;

  // The block and a filter partition in time, padded to two hops; each
  // channel's block spectrum and hop of output (unordered spectra).
  Buffer block_time_;
  Buffer filter_time_;
  Buffer filter_spectrum_;
  std::array<Buffer, 2> block_spectrum_;
  std::array<Buffer, 2> product_;
  // Each coming hop's sum of products (a ring from the hop now), per
  // processed channel.
  std::vector<std::array<Buffer, 2>> landing_;
  std::size_t landed_ = 0;
  // The second half of the last hop's sum, which overlaps this one.
  std::vector<double> overlap_a_;
  std::vector<double> overlap_b_;
  std::vector<std::pair<double, double>> out_;

  struct DisplayBand {
    double hz = 0.0;
    double centre = 0.0; // On the grid, fractional.
    double first = 0.0;
    double last = 0.0;
  };
  std::array<DisplayBand, display_bands> display_{};
  std::array<double, display_bands> display_in_{};
  std::array<double, display_bands> display_out_{};
  std::array<double, display_bands> display_key_{};
  std::array<double, display_bands> display_gain_{};
  std::array<double, 2> input_peak_{};
  std::array<double, 2> output_peak_{};
  double max_cut_held_ = 0.0;
  double transient_now_ = 0.0;
  double trim_ = 1.0;
  std::size_t position_ = 0;
  std::size_t written_ = 0;
  bool keyed_ = false;
};

std::unique_ptr<EffectKernel> create_kernel(std::span<const double> values, double sample_rate) {
  if (values.size() != parameter_count || !(sample_rate > 0.0)) {
    return nullptr;
  }
  return std::make_unique<TemperKernel>(values, sample_rate);
}

// After the (separately reported) latency, the filter rings for its length.
std::uint64_t tail_frames(std::span<const double> values, double sample_rate) {
  const auto f = frames_for(values, sample_rate);
  return f.filter + f.hop;
}

std::uint32_t latency_frames(std::span<const double> values, double sample_rate) {
  return frames_for(values, sample_rate).latency;
}

const EngineDescriptor descriptor{"temper", "Temper", 1, t::parameters, temper_presets::presets};
const EffectEngine temper_engine{&descriptor, &create_kernel, &tail_frames, &latency_frames, true};

} // namespace

Frames frames_for(std::span<const double> values, double sample_rate) noexcept {
  const std::size_t scale = scale_for(sample_rate);
  Frames f;
  switch (phase_of(values)) {
  case t::Phase::low_latency:
    f.window = base_low_window * scale;
    f.hop = base_low_hop * scale;
    f.filter = base_low_filter * scale;
    f.lookahead = 0;
    f.latency = 0;
    break;
  case t::Phase::natural:
    f.window = base_window * scale;
    f.hop = base_hop * scale;
    f.filter = base_filter * scale;
    f.lookahead = f.window / natural_lookahead_divisor;
    f.latency = static_cast<std::uint32_t>(f.lookahead + f.hop);
    break;
  case t::Phase::linear:
    f.window = base_window * scale;
    f.hop = base_hop * scale;
    f.filter = base_filter * scale;
    f.lookahead = 0;
    f.latency = static_cast<std::uint32_t>(f.hop + f.filter / 2);
    break;
  }
  return f;
}

const EffectEngine& engine() { return temper_engine; }

} // namespace oscillate::native_dsp::effects::temper
