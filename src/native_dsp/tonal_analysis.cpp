#include "oscillate/native_dsp/tonal_analysis.hpp"

#include "dsp/offline_fft.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace oscillate::native_dsp::tonal {
namespace {

using foundation::Error;
using foundation::ErrorCode;

constexpr double fine_per_octave = bands_per_octave * static_cast<double>(fine_per_band);
// Other rates than the design rate take the nearest power of two that keeps
// the window's length in seconds.
constexpr std::size_t smallest_window = 1'024;
// Levels are written to a hundredth of a dB.
constexpr int written_decimals = 2;

[[nodiscard]] std::size_t window_size(double rate) noexcept {
  const double wanted = static_cast<double>(meter_window_frames) * rate / meter_design_rate;
  std::size_t size = smallest_window;
  while (static_cast<double>(size) * std::numbers::sqrt2 < wanted) {
    size *= 2;
  }
  return size;
}

// Band sum over [low, high) in bins from running sums over bin edges (bin k
// spans k - 0.5 to k + 0.5), linear inside a bin.
[[nodiscard]] double band_sum(const std::vector<double>& c, double low, double high) noexcept {
  const auto at = [&](double x) {
    const double edge = std::clamp(x + 0.5, 0.0, static_cast<double>(c.size() - 2));
    const auto whole = static_cast<std::size_t>(edge);
    const double frac = edge - static_cast<double>(whole);
    return c[whole] + frac * (c[whole + 1] - c[whole]);
  };
  return std::max(0.0, at(high) - at(low));
}

[[nodiscard]] Error invalid(std::string message) {
  return Error{ErrorCode::corrupt_data, std::move(message)};
}

void write_number(std::string& out, double value) {
  std::array<char, 32> text{};
  const auto written = std::to_chars(text.data(), text.data() + text.size(), value,
                                     std::chars_format::fixed, written_decimals);
  out.append(text.data(), written.ptr);
}

void write_array(std::string& out, std::string_view key, std::span<const double> values) {
  out += "  \"";
  out += key;
  out += "\": [";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0) {
      out += ", ";
    }
    write_number(out, values[i]);
  }
  out += ']';
}

[[nodiscard]] bool read_curve(const nlohmann::json& document, std::string_view key, Curve& out) {
  const auto found = document.find(std::string(key));
  if (found == document.end() || !found->is_array() || found->size() != bands) {
    return false;
  }
  for (std::size_t b = 0; b < bands; ++b) {
    const auto& value = (*found)[b];
    if (!value.is_number()) {
      return false;
    }
    out[b] = value.get<double>();
    if (!std::isfinite(out[b]) || out[b] < level_floor_db || out[b] > level_ceiling_db) {
      return false;
    }
  }
  return true;
}

} // namespace

double fine_hz(std::size_t fine) noexcept {
  // Fine band i sits (i % 4 + 0.5) / 12 octave above its grid band's low edge.
  const std::size_t band = fine / fine_per_band;
  const double step = static_cast<double>(fine % fine_per_band) + 0.5;
  return band_low_hz(band) * std::exp2(step / fine_per_octave);
}

double fine_low_hz(std::size_t fine) noexcept {
  return fine_hz(fine) * std::exp2(-0.5 / fine_per_octave);
}

double fine_high_hz(std::size_t fine) noexcept {
  return fine_hz(fine) * std::exp2(0.5 / fine_per_octave);
}

Curve grid_levels(const FineCurve& power) noexcept {
  Curve out{};
  for (std::size_t b = 0; b < bands; ++b) {
    double sum = 0.0;
    for (std::size_t i = 0; i < fine_per_band; ++i) {
      sum += power[b * fine_per_band + i];
    }
    out[b] = dsp::power_to_db(sum);
  }
  return out;
}

FineCurve fine_levels(const FineCurve& power) noexcept {
  FineCurve out{};
  for (std::size_t i = 0; i < fine_bands; ++i) {
    out[i] = dsp::power_to_db(power[i]);
  }
  return out;
}

double fine_loudness_db(const FineCurve& levels) noexcept {
  double sum = 0.0;
  for (std::size_t i = 0; i < fine_bands; ++i) {
    sum += std::pow(10.0, (levels[i] + k_weighting_db(fine_hz(i))) / 10.0);
  }
  return 10.0 * std::log10(sum);
}

// ---- BandMeter ---------------------------------------------------------------------

struct BandMeter::Plan {
  explicit Plan(std::size_t size) : fft(size) {}
  dsp::OfflineFft fft;
};

BandMeter::BandMeter(double sample_rate)
    : size_(window_size(sample_rate)), hop_(size_ / 2), plan_(std::make_unique<Plan>(size_)),
      window_(size_), work_(size_) {
  double squares = 0.0;
  for (std::size_t i = 0; i < size_; ++i) {
    window_[i] = dsp::hann(static_cast<double>(i), static_cast<double>(size_));
    squares += window_[i] * window_[i];
  }
  // One-sided: the band powers of a window sum to its weighted mean square.
  scale_ = 2.0 / (static_cast<double>(size_) * squares);
  for (auto& ring : ring_) {
    ring.assign(size_, 0.0);
  }
  for (auto& c : cumulative_) {
    c.assign(size_ / 2 + 2, 0.0);
  }
  const double bin_hz = sample_rate / static_cast<double>(size_);
  for (std::size_t i = 0; i < fine_bands; ++i) {
    low_bin_[i] = fine_low_hz(i) / bin_hz;
    high_bin_[i] = std::min(fine_high_hz(i), sample_rate / 2.0) / bin_hz;
  }
  reset();
}

BandMeter::~BandMeter() = default;
BandMeter::BandMeter(BandMeter&&) noexcept = default;
BandMeter& BandMeter::operator=(BandMeter&&) noexcept = default;

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the first signal, then the second.
void BandMeter::push(double a, double b) noexcept {
  ring_[0][write_] = a;
  ring_[1][write_] = b;
  write_ = write_ + 1 == size_ ? 0 : write_ + 1;
  filled_ = std::min(filled_ + 1, size_);
  if (++since_ >= hop_ && filled_ == size_) {
    since_ = 0;
    analyse();
  }
}

void BandMeter::reset() noexcept {
  for (auto& ring : ring_) {
    std::ranges::fill(ring, 0.0);
  }
  write_ = 0;
  filled_ = 0;
  since_ = 0;
  windows_ = 0;
  for (auto& mean : mean_) {
    mean.fill(0.0);
  }
}

void BandMeter::analyse() noexcept {
  // Oldest first: the ring's write position is its oldest frame.
  for (std::size_t i = 0; i < size_; ++i) {
    const std::size_t at = (write_ + i) % size_;
    work_[i] = {ring_[0][at] * window_[i], ring_[1][at] * window_[i]};
  }
  plan_->fft.forward(work_);
  auto& ca = cumulative_[0];
  auto& cb = cumulative_[1];
  ca[0] = 0.0;
  cb[0] = 0.0;
  for (std::size_t k = 0; k <= size_ / 2; ++k) {
    const auto x = work_[k];
    const auto y = std::conj(work_[(size_ - k) % size_]);
    const auto a = 0.5 * (x + y);
    const auto b = std::complex<double>(0.0, -0.5) * (x - y);
    // DC and Nyquist have no mirror image.
    const double weight = k == 0 || k == size_ / 2 ? 0.5 * scale_ : scale_;
    ca[k + 1] = ca[k] + std::norm(a) * weight;
    cb[k + 1] = cb[k] + std::norm(b) * weight;
  }
  ++windows_;
  const double step = 1.0 / static_cast<double>(windows_);
  for (std::size_t s = 0; s < 2; ++s) {
    for (std::size_t i = 0; i < fine_bands; ++i) {
      const double power = band_sum(cumulative_[s], low_bin_[i], high_bin_[i]);
      mean_[s][i] += step * (power - mean_[s][i]);
    }
  }
}

// ---- Targets ----------------------------------------------------------------------

double percentile(std::span<double> values, double p) noexcept {
  if (values.empty()) {
    return 0.0;
  }
  std::ranges::sort(values);
  const double at = std::clamp(p, 0.0, 1.0) * static_cast<double>(values.size() - 1);
  const auto below = static_cast<std::size_t>(std::floor(at));
  const std::size_t above = std::min(below + 1, values.size() - 1);
  const double frac = at - static_cast<double>(below);
  return values[below] + frac * (values[above] - values[below]);
}

foundation::Result<Target> build_target(std::string id, std::string name,
                                        std::span<const Curve> curves) {
  if (curves.empty()) {
    return foundation::Result<Target>::failure(
        {ErrorCode::invalid_argument, "A target needs at least one reference"});
  }
  Target target;
  target.id = std::move(id);
  target.name = std::move(name);
  target.references = curves.size();
  std::vector<double> column(curves.size());
  for (std::size_t b = 0; b < bands; ++b) {
    for (std::size_t c = 0; c < curves.size(); ++c) {
      column[c] = curves[c][b];
    }
    target.low[b] = percentile(column, lower_quartile);
    target.median[b] = percentile(column, 0.5);
    target.high[b] = percentile(column, upper_quartile);
  }
  return foundation::Result<Target>::success(std::move(target));
}

std::string serialise_target(const Target& target) {
  std::string out = "{\n  \"format\": \"";
  out += file_format;
  out += "\",\n  \"version\": ";
  out += std::to_string(file_version);
  out += ",\n  \"name\": ";
  out += nlohmann::json(target.name).dump();
  out += ",\n  \"references\": ";
  out += std::to_string(target.references);
  out += ",\n";
  Curve centres{};
  for (std::size_t b = 0; b < bands; ++b) {
    centres[b] = band_hz(b);
  }
  write_array(out, "centres_hz", centres);
  out += ",\n";
  write_array(out, "median_db", target.median);
  out += ",\n";
  write_array(out, "low_db", target.low);
  out += ",\n";
  write_array(out, "high_db", target.high);
  out += "\n}\n";
  return out;
}

foundation::Result<Target> parse_target(std::string_view text, std::string id) {
  using R = foundation::Result<Target>;
  if (text.size() > max_file_bytes) {
    return R::failure(invalid("The target file is too large"));
  }
  const auto document = nlohmann::json::parse(text, nullptr, false);
  if (document.is_discarded() || !document.is_object()) {
    return R::failure(invalid("The target file is not JSON"));
  }
  const auto format = document.find("format");
  if (format == document.end() || !format->is_string() ||
      format->get<std::string>() != file_format) {
    return R::failure(invalid("Not a tonal target file"));
  }
  const auto version = document.find("version");
  if (version == document.end() || !version->is_number_integer() ||
      version->get<int>() != file_version) {
    return R::failure({ErrorCode::unsupported, "The tonal target file's version is not supported"});
  }
  Target target;
  target.id = std::move(id);
  const auto name = document.find("name");
  if (name == document.end() || !name->is_string() || name->get<std::string>().empty() ||
      name->get<std::string>().size() > max_name_bytes) {
    return R::failure(
        invalid("The target needs a name of 1 to " + std::to_string(max_name_bytes) + " bytes"));
  }
  target.name = name->get<std::string>();
  const auto references = document.find("references");
  if (references == document.end() || !references->is_number_unsigned() ||
      references->get<std::size_t>() < 1) {
    return R::failure(invalid("The target needs at least one reference"));
  }
  target.references = references->get<std::size_t>();
  const auto centres = document.find("centres_hz");
  if (centres == document.end() || !centres->is_array() || centres->size() != bands) {
    return R::failure(invalid("The target is not on the third-octave grid"));
  }
  for (std::size_t b = 0; b < bands; ++b) {
    const auto& centre = (*centres)[b];
    if (!centre.is_number() ||
        std::abs(centre.get<double>() / band_hz(b) - 1.0) > centre_tolerance) {
      return R::failure(invalid("The target is not on the third-octave grid"));
    }
  }
  if (!read_curve(document, "median_db", target.median) ||
      !read_curve(document, "low_db", target.low) ||
      !read_curve(document, "high_db", target.high)) {
    return R::failure(invalid("Each curve needs " + std::to_string(bands) +
                              " finite levels within the level range"));
  }
  for (std::size_t b = 0; b < bands; ++b) {
    if (target.low[b] > target.median[b] || target.median[b] > target.high[b]) {
      return R::failure(invalid("Each band's range must hold its median"));
    }
  }
  return R::success(std::move(target));
}

} // namespace oscillate::native_dsp::tonal
