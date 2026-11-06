#include "oscillate/media/source_rate_reader.hpp"

#if OSCILLATE_HAVE_SOURCE_RESAMPLER
#include <samplerate.h>
#endif

#include <algorithm>
#include <array>
#include <limits>
#include <numeric>
#include <utility>

namespace oscillate::media {
namespace {
using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;

[[nodiscard]] bool supported_project_rate(std::uint32_t rate) noexcept {
  return rate == 44'100 || rate == 48'000 || rate == 88'200 || rate == 96'000;
}

} // namespace

Result<std::uint64_t> source_rate_frame_count(std::uint64_t frames, std::uint32_t source_rate,
                                              std::uint32_t project_rate) {
  if (source_rate < 8'000 || source_rate > 384'000 || !supported_project_rate(project_rate)) {
    return Result<std::uint64_t>::failure(
        {ErrorCode::invalid_argument, "Source or project sample rate is unsupported"});
  }
  const auto whole = frames / source_rate;
  const auto remainder = frames % source_rate;
  if (whole > std::numeric_limits<std::uint64_t>::max() / project_rate) {
    return Result<std::uint64_t>::failure(
        {ErrorCode::resource_exhausted, "Source-rate duration exceeds project frame range"});
  }
  const auto fractional = (remainder * project_rate + source_rate / 2U) / source_rate;
  if (whole * project_rate > std::numeric_limits<std::uint64_t>::max() - fractional) {
    return Result<std::uint64_t>::failure(
        {ErrorCode::resource_exhausted, "Source-rate duration exceeds project frame range"});
  }
  return Result<std::uint64_t>::success(whole * project_rate + fractional);
}

Result<void> source_rate_supported(std::uint32_t source_rate, std::uint32_t project_rate) {
  if (source_rate < 8'000 || source_rate > 384'000 || !supported_project_rate(project_rate)) {
    return Result<void>::failure(
        {ErrorCode::invalid_argument, "Source or project sample rate is unsupported"});
  }
#if !OSCILLATE_HAVE_SOURCE_RESAMPLER
  if (source_rate != project_rate) {
    return Result<void>::failure(
        {ErrorCode::unsupported,
         "Source-rate conversion requires the optional libsamplerate build"});
  }
#else
  if (source_rate != project_rate) {
    const auto divisor = std::gcd(source_rate, project_rate);
    if (source_rate / divisor > 4'096 || project_rate / divisor > 4'096) {
      return Result<void>::failure(
          {ErrorCode::unsupported, "Source-rate phase period exceeds bounded seek window"});
    }
  }
#endif
  return Result<void>::success();
}

#if OSCILLATE_HAVE_SOURCE_RESAMPLER
namespace {
[[nodiscard]] std::uint64_t source_floor(std::uint64_t project_frame, std::uint32_t source_rate,
                                         std::uint32_t project_rate) noexcept {
  return (project_frame / project_rate) * source_rate +
         ((project_frame % project_rate) * source_rate) / project_rate;
}
} // namespace
#endif

struct SourceRateReader::Backend {
  struct Destination {
    std::uint32_t rate;
    std::uint64_t frames;
  };
  Backend(AudioReader owned_source, Destination destination)
      : source(std::move(owned_source)), project_rate(destination.rate),
        frames(destination.frames) {}
  AudioReader source;
  std::uint32_t project_rate;
  std::uint64_t frames;
#if OSCILLATE_HAVE_SOURCE_RESAMPLER
  struct ConverterDeleter {
    void operator()(SRC_STATE* state) const noexcept { (void)src_delete(state); }
  };
  std::unique_ptr<SRC_STATE, ConverterDeleter> converter;
  static constexpr std::size_t capacity = 16'384;
  static constexpr std::uint64_t preroll = 256;
  static constexpr std::uint64_t postroll = 256;
  std::array<StereoFrame, capacity> decoded{};
  std::array<float, capacity * 2> input{};
  std::array<float, capacity * 2> converted{};
#endif
};

SourceRateReader::SourceRateReader(std::unique_ptr<Backend> backend) noexcept
    : backend_(std::move(backend)) {}
SourceRateReader::SourceRateReader(SourceRateReader&&) noexcept = default;
SourceRateReader& SourceRateReader::operator=(SourceRateReader&&) noexcept = default;
SourceRateReader::~SourceRateReader() = default;

Result<SourceRateReader> SourceRateReader::open(AudioReader source, std::uint32_t project_rate) {
  if (source.channels() < 1 || source.channels() > 2) {
    return Result<SourceRateReader>::failure(
        {ErrorCode::invalid_argument, "Source or project sample rate/channel layout unsupported"});
  }
  auto supported = source_rate_supported(source.sample_rate(), project_rate);
  if (!supported)
    return Result<SourceRateReader>::failure(supported.error());
  auto duration = source_rate_frame_count(source.frame_count(), source.sample_rate(), project_rate);
  if (!duration) {
    return Result<SourceRateReader>::failure(duration.error());
  }
  auto backend = std::make_unique<Backend>(std::move(source),
                                           Backend::Destination{project_rate, duration.value()});
#if OSCILLATE_HAVE_SOURCE_RESAMPLER
  if (backend->source.sample_rate() != project_rate) {
    int error = 0;
    backend->converter.reset(src_new(SRC_SINC_MEDIUM_QUALITY, 2, &error));
    if (!backend->converter) {
      return Result<SourceRateReader>::failure(
          {ErrorCode::resource_exhausted, "Cannot prepare source-rate converter"});
    }
  }
#endif
  return Result<SourceRateReader>::success(SourceRateReader(std::move(backend)));
}

Result<std::size_t> SourceRateReader::read_at(std::uint64_t project_frame,
                                              std::span<StereoFrame> output,
                                              const std::stop_token& stop) {
  if (output.size() > max_read_frames ||
      project_frame > std::numeric_limits<std::uint64_t>::max() - output.size()) {
    return Result<std::size_t>::failure(
        {ErrorCode::invalid_argument, "Source-rate read exceeds bounded project frame range"});
  }
  if (stop.stop_requested()) {
    return Result<std::size_t>::failure({ErrorCode::cancelled, "Source-rate read cancelled"});
  }
  if (project_frame >= backend_->frames || output.empty()) {
    return Result<std::size_t>::success(0);
  }
  const auto wanted = static_cast<std::size_t>(
      std::min<std::uint64_t>(output.size(), backend_->frames - project_frame));
  if (backend_->source.sample_rate() == backend_->project_rate) {
    return backend_->source.read_at(project_frame, output.first(wanted));
  }
#if OSCILLATE_HAVE_SOURCE_RESAMPLER
  const auto source_rate = backend_->source.sample_rate();
  // Construction validated both rates; retain nonzero divisors even if a
  // corrupted reader violates that invariant after construction.
  const auto divisor = std::max(1U, std::gcd(source_rate, backend_->project_rate));
  const auto source_period = std::max(1U, source_rate / divisor);
  const auto project_period = backend_->project_rate / divisor;
  const auto nominal_source = source_floor(project_frame, source_rate, backend_->project_rate);
  const auto anchor = nominal_source > Backend::preroll
                          ? ((nominal_source - Backend::preroll) / source_period) * source_period
                          : 0;
  const auto base_project = (anchor / source_period) * project_period;
  const auto offset = project_frame - base_project;
  const auto source_last =
      source_floor(project_frame + wanted, source_rate, backend_->project_rate);
  if (source_last > std::numeric_limits<std::uint64_t>::max() - Backend::postroll ||
      source_last + Backend::postroll < anchor) {
    return Result<std::size_t>::failure(
        {ErrorCode::resource_exhausted, "Source-rate read range overflow"});
  }
  const auto input_frames = source_last + Backend::postroll - anchor;
  if (input_frames > Backend::capacity || offset > Backend::capacity ||
      wanted > Backend::capacity - offset) {
    return Result<std::size_t>::failure(
        {ErrorCode::resource_exhausted, "Source-rate conversion window exceeded fixed capacity"});
  }
  const auto available =
      anchor < backend_->source.frame_count()
          ? std::min<std::uint64_t>(input_frames, backend_->source.frame_count() - anchor)
          : 0;
  if (available != 0) {
    auto got = backend_->source.read_at(
        anchor, std::span(backend_->decoded).first(static_cast<std::size_t>(available)));
    if (!got) {
      return Result<std::size_t>::failure(got.error());
    }
    if (got.value() != available) {
      return Result<std::size_t>::failure(
          {ErrorCode::io_failure, "Verified source ended during rate conversion"});
    }
  }
  for (std::size_t index = 0; index < static_cast<std::size_t>(input_frames); ++index) {
    const auto frame = index < available ? backend_->decoded[index] : StereoFrame{};
    backend_->input[index * 2] = frame.left;
    backend_->input[index * 2 + 1] = frame.right;
  }
  if (stop.stop_requested()) {
    return Result<std::size_t>::failure({ErrorCode::cancelled, "Source-rate read cancelled"});
  }
  SRC_DATA data{};
  data.data_in = backend_->input.data();
  data.input_frames = static_cast<long>(input_frames);
  data.data_out = backend_->converted.data();
  data.output_frames = static_cast<long>(Backend::capacity);
  data.src_ratio = static_cast<double>(backend_->project_rate) / source_rate;
  data.end_of_input = 1;
  if (src_reset(backend_->converter.get()) != 0 ||
      src_process(backend_->converter.get(), &data) != 0 || data.output_frames_gen < 0 ||
      static_cast<std::uint64_t>(data.output_frames_gen) < offset + wanted) {
    return Result<std::size_t>::failure(
        {ErrorCode::corrupt_data, "Source-rate converter could not fill requested window"});
  }
  if (stop.stop_requested()) {
    return Result<std::size_t>::failure({ErrorCode::cancelled, "Source-rate read cancelled"});
  }
  for (std::size_t index = 0; index < wanted; ++index) {
    const auto converted_index = static_cast<std::size_t>(offset) + index;
    output[index] = {backend_->converted[converted_index * 2],
                     backend_->converted[converted_index * 2 + 1]};
  }
  return Result<std::size_t>::success(wanted);
#else
  return Result<std::size_t>::failure(
      {ErrorCode::unsupported, "Source-rate conversion requires the optional libsamplerate build"});
#endif
}

std::uint32_t SourceRateReader::source_rate() const noexcept {
  return backend_->source.sample_rate();
}
std::uint32_t SourceRateReader::project_rate() const noexcept { return backend_->project_rate; }
std::uint64_t SourceRateReader::frame_count() const noexcept { return backend_->frames; }

} // namespace oscillate::media
