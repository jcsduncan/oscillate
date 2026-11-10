#include "oscillate/export/master_output.hpp"

#include "oscillate/timeline/timing.hpp"

#if OSCILLATE_HAVE_CODEC
#include "encoded_writer.hpp"
#endif
#if OSCILLATE_HAVE_EXPORT_RESAMPLER
#include <samplerate.h>
#endif

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <variant>

namespace oscillate::exporting::detail {
namespace {

// Matches the native master render block: each converter call consumes at
// most this many project-rate frames into a 4x output buffer.
constexpr std::size_t chunk_frames = 1'024;

#if OSCILLATE_HAVE_CODEC
using Writer = std::variant<WavWriter, EncodedWriter>;
#else
using Writer = std::variant<WavWriter>;
#endif

#if OSCILLATE_HAVE_EXPORT_RESAMPLER
struct SrcDeleter {
  void operator()(SRC_STATE* state) const noexcept { (void)src_delete(state); }
};
#endif

[[nodiscard]] foundation::Result<Writer> begin_writer(const MasterOutputOptions& options,
                                                      std::uint32_t sample_rate,
                                                      std::uint64_t expected_frames) {
  if (const auto* wav = std::get_if<MasterRenderOptions>(&options)) {
    auto started = WavWriter::begin(wav->target, WavSpec{sample_rate, wav->format, expected_frames,
                                                         wav->dither_seed, wav->dither});
    if (!started) {
      return foundation::Result<Writer>::failure(started.error());
    }
    return foundation::Result<Writer>::success(
        Writer{std::in_place_type<WavWriter>, std::move(started).value()});
  }
#if OSCILLATE_HAVE_CODEC
  auto started =
      EncodedWriter::begin(std::get<EncodedMasterOptions>(options), sample_rate, expected_frames);
  if (!started) {
    return foundation::Result<Writer>::failure(started.error());
  }
  return foundation::Result<Writer>::success(
      Writer{std::in_place_type<EncodedWriter>, std::move(started).value()});
#else
  return foundation::Result<Writer>::failure(
      {foundation::ErrorCode::unsupported, "Encoded export requires codec adapter build"});
#endif
}

} // namespace

struct MasterOutput::State {
  State(Writer started, MasterOutput::Observer observe, std::uint32_t input_rate,
        std::uint32_t target_rate, std::uint64_t input_frames, std::uint64_t target_frames)
      : writer(std::move(started)), observer(std::move(observe)), project_rate(input_rate),
        output_rate(target_rate), project_frames(input_frames), output_frames(target_frames) {}

  Writer writer;
  MasterOutput::Observer observer;
  std::uint32_t project_rate;
  std::uint32_t output_rate;
  std::uint64_t project_frames;
  std::uint64_t output_frames;
  std::uint64_t consumed = 0;
#if OSCILLATE_HAVE_EXPORT_RESAMPLER
  std::unique_ptr<SRC_STATE, SrcDeleter> converter;
  std::array<float, chunk_frames * 4 * 2> converted{};
  std::uint64_t converted_written = 0;
  std::uint64_t converted_generated = 0;
#endif

  [[nodiscard]] foundation::Result<void> emit(std::span<const float> samples) {
    auto wrote =
        std::visit([&](auto& active) { return active.write_interleaved_stereo(samples); }, writer);
    if (!wrote) {
      return wrote;
    }
    if (observer && !observer(samples)) {
      return foundation::Result<void>::failure(
          {foundation::ErrorCode::resource_exhausted, "Export measurement exceeded limit"});
    }
    return foundation::Result<void>::success();
  }

#if OSCILLATE_HAVE_EXPORT_RESAMPLER
  [[nodiscard]] foundation::Result<void> convert(std::span<const float> samples, bool final_chunk) {
    const auto frames = samples.size() / 2;
    std::size_t used = 0;
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
      SRC_DATA data{};
      data.data_in = samples.data() + used * 2;
      data.input_frames = static_cast<long>(frames - used);
      data.data_out = converted.data();
      data.output_frames = static_cast<long>(converted.size() / 2);
      data.end_of_input = final_chunk ? 1 : 0;
      data.src_ratio = static_cast<double>(output_rate) / project_rate;
      if (src_process(converter.get(), &data) != 0 || data.input_frames_used < 0 ||
          data.output_frames_gen < 0 || data.input_frames_used > data.input_frames ||
          data.output_frames_gen > data.output_frames) {
        return foundation::Result<void>::failure(
            {foundation::ErrorCode::corrupt_data, "Export rate conversion failed"});
      }
      used += static_cast<std::size_t>(data.input_frames_used);
      const auto generated = static_cast<std::uint64_t>(data.output_frames_gen);
      const auto max_generated = output_frames == std::numeric_limits<std::uint64_t>::max()
                                     ? output_frames
                                     : output_frames + 1;
      if (converted_generated > max_generated || generated > max_generated - converted_generated) {
        return foundation::Result<void>::failure(
            {foundation::ErrorCode::corrupt_data, "Export rate conversion overproduced"});
      }
      converted_generated += generated;
      const auto emitted = std::min(output_frames - converted_written, generated);
      if (emitted != 0) {
        auto wrote =
            emit(std::span<const float>(converted).first(static_cast<std::size_t>(emitted) * 2));
        if (!wrote) {
          return wrote;
        }
        converted_written += emitted;
      }
      if (used == frames && (!final_chunk || data.output_frames_gen == 0)) {
        return foundation::Result<void>::success();
      }
      if (data.input_frames_used == 0 && data.output_frames_gen == 0) {
        return foundation::Result<void>::failure(
            {foundation::ErrorCode::corrupt_data, "Export rate conversion stopped early"});
      }
    }
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::resource_exhausted, "Export rate conversion exceeded bound"});
  }

  // Runs once after the final converted chunk, so observers see every
  // published frame before the caller finishes its measurements.
  [[nodiscard]] foundation::Result<void> complete_conversion() {
    // SRC's finite sinc response can end one frame before the rounded
    // duration for some rational ratios. Outside the rendered interval the
    // mix is silent, so extend that single terminal frame with silence.
    // Larger gaps indicate a converter or accounting failure.
    if (converted_written < output_frames && output_frames - converted_written == 1) {
      const std::array<float, 2> silence{};
      auto wrote = emit(silence);
      if (!wrote) {
        return wrote;
      }
      ++converted_written;
    }
    if (converted_written != output_frames) {
      return foundation::Result<void>::failure(
          {foundation::ErrorCode::corrupt_data, "Export rate conversion produced wrong duration"});
    }
    return foundation::Result<void>::success();
  }
#endif
};

foundation::Result<std::uint64_t>
converted_frame_count(std::uint64_t frames, std::uint32_t input_rate, std::uint32_t output_rate) {
  const auto rounding = input_rate / 2;
  if (frames > (std::numeric_limits<std::uint64_t>::max() - rounding) / output_rate) {
    return foundation::Result<std::uint64_t>::failure(
        {foundation::ErrorCode::resource_exhausted, "Export output duration overflows"});
  }
  return foundation::Result<std::uint64_t>::success((frames * output_rate + rounding) / input_rate);
}

MasterOutput::MasterOutput(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}
MasterOutput::MasterOutput(MasterOutput&& other) noexcept = default;
MasterOutput::~MasterOutput() = default;

foundation::Result<MasterOutput> MasterOutput::begin(const MasterOutputOptions& options,
                                                     std::uint32_t project_rate,
                                                     std::uint64_t project_frames,
                                                     Observer observer) {
  const auto chosen = std::visit([](const auto& value) { return value.sample_rate; }, options);
  const auto output_rate = chosen == 0 ? project_rate : chosen;
  if (!timeline::supported_sample_rate(project_rate) ||
      !timeline::supported_sample_rate(output_rate)) {
    return foundation::Result<MasterOutput>::failure(
        {foundation::ErrorCode::invalid_argument, "Export output sample rate is unsupported"});
  }
  auto output_frames = converted_frame_count(project_frames, project_rate, output_rate);
  if (!output_frames) {
    return foundation::Result<MasterOutput>::failure(output_frames.error());
  }
  const bool convert_rate = output_rate != project_rate;
#if OSCILLATE_HAVE_EXPORT_RESAMPLER
  std::unique_ptr<SRC_STATE, SrcDeleter> converter;
  if (convert_rate) {
    int error = 0;
    converter.reset(src_new(SRC_SINC_BEST_QUALITY, 2, &error));
    if (!converter) {
      return foundation::Result<MasterOutput>::failure(
          {foundation::ErrorCode::resource_exhausted, "Cannot prepare export rate converter"});
    }
  }
#else
  if (convert_rate) {
    return foundation::Result<MasterOutput>::failure(
        {foundation::ErrorCode::unsupported,
         "Output-rate conversion requires the libsamplerate export build"});
  }
#endif
  auto writer = begin_writer(options, output_rate, output_frames.value());
  if (!writer) {
    return foundation::Result<MasterOutput>::failure(writer.error());
  }
  auto state = std::make_unique<State>(std::move(writer).value(), std::move(observer), project_rate,
                                       output_rate, project_frames, output_frames.value());
#if OSCILLATE_HAVE_EXPORT_RESAMPLER
  state->converter = std::move(converter);
#endif
  return foundation::Result<MasterOutput>::success(MasterOutput(std::move(state)));
}

std::uint32_t MasterOutput::output_rate() const noexcept { return state_->output_rate; }

std::uint64_t MasterOutput::output_frames() const noexcept { return state_->output_frames; }

foundation::Result<void> MasterOutput::write_interleaved_stereo(std::span<const float> samples) {
  auto& state = *state_;
  if (samples.size() % 2 != 0 || samples.size() / 2 > state.project_frames - state.consumed) {
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::invalid_argument, "Export master received too many frames"});
  }
  if (state.output_rate == state.project_rate) {
    state.consumed += samples.size() / 2;
    return state.emit(samples);
  }
#if OSCILLATE_HAVE_EXPORT_RESAMPLER
  while (!samples.empty()) {
    const auto frames = std::min(chunk_frames, samples.size() / 2);
    state.consumed += frames;
    auto converted =
        state.convert(samples.first(frames * 2), state.consumed == state.project_frames);
    if (!converted) {
      return converted;
    }
    samples = samples.subspan(frames * 2);
  }
  return state.consumed == state.project_frames ? state.complete_conversion()
                                                : foundation::Result<void>::success();
#else
  return foundation::Result<void>::failure(
      {foundation::ErrorCode::unsupported,
       "Output-rate conversion requires the libsamplerate export build"});
#endif
}

foundation::Result<void> MasterOutput::shorten(std::uint64_t project_frames) {
  auto& state = *state_;
  // At least one frame must still come, so the last write ends the
  // conversion as it always does.
  if (project_frames <= state.consumed || project_frames > state.project_frames) {
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::invalid_argument, "Export master cannot shorten to that length"});
  }
  if (project_frames == state.project_frames)
    return foundation::Result<void>::success();
  auto output_frames = converted_frame_count(project_frames, state.project_rate, state.output_rate);
  if (!output_frames)
    return foundation::Result<void>::failure(output_frames.error());
  auto shortened =
      std::visit([&](auto& active) { return active.shorten(output_frames.value()); }, state.writer);
  if (!shortened)
    return shortened;
  state.project_frames = project_frames;
  state.output_frames = output_frames.value();
  return foundation::Result<void>::success();
}

foundation::Result<PublishOutcome> MasterOutput::finish() {
  if (state_->consumed != state_->project_frames) {
    return foundation::Result<PublishOutcome>::failure(
        {foundation::ErrorCode::internal, "Export master ended before its expected duration"});
  }
  return std::visit([](auto& active) { return active.finish(); }, state_->writer);
}

} // namespace oscillate::exporting::detail
