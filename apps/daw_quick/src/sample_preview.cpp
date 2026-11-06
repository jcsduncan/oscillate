#include "sample_preview.hpp"

#include "preview_pipewire_output.hpp"

#include "oscillate/application/browser_preview.hpp"
#include "oscillate/media/prefetch.hpp"
#include "oscillate/warp_dsp/source_view.hpp"
#include "oscillate/warp_dsp/stream.hpp"
#include "oscillate/warp_dsp/time_map.hpp"
#if OSCILLATE_HAVE_CODEC
#include "oscillate/codec/encoded_reader.hpp"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <exception>
#include <span>
#include <utility>

namespace oscillate::quick {
namespace {

using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;

constexpr double max_preview_seconds = 30.0;
constexpr const char* cancelled_message = "Preview cancelled";
// Previews sit under the song: samples are often mastered hot.
constexpr float preview_gain = 0.7F;

struct Decoded {
  std::vector<float> left;
  std::vector<float> right;
  std::uint32_t rate = 0;
  bool truncated = false;
};

// Reads up to max_preview_seconds through `read(first, frames)`.
template <typename Frame, typename Reader>
Result<Decoded> read_all(Reader& reader, std::uint64_t frames, std::uint32_t rate,
                         const std::stop_token& stop) {
  Decoded out;
  out.rate = rate;
  const auto cap = static_cast<std::uint64_t>(max_preview_seconds * rate);
  const auto wanted = std::min(frames, cap);
  out.truncated = frames > cap;
  out.left.reserve(static_cast<std::size_t>(wanted));
  out.right.reserve(static_cast<std::size_t>(wanted));
  std::array<Frame, 4096> chunk{};
  std::uint64_t at = 0;
  while (at < wanted) {
    if (stop.stop_requested())
      return Result<Decoded>::failure({ErrorCode::cancelled, cancelled_message});
    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), wanted - at));
    auto read = reader.read_at(at, std::span(chunk).first(count));
    if (!read)
      return Result<Decoded>::failure(read.error());
    if (read.value() == 0)
      break;
    for (std::size_t i = 0; i < read.value(); ++i) {
      out.left.push_back(chunk[i].left);
      out.right.push_back(chunk[i].right);
    }
    at += read.value();
  }
  return Result<Decoded>::success(std::move(out));
}

Result<Decoded> decode(const std::filesystem::path& file, const std::stop_token& stop) {
#if OSCILLATE_HAVE_CODEC
  auto reader = codec::EncodedReader::open(file);
  if (!reader)
    return Result<Decoded>::failure(reader.error());
  const auto info = reader.value().info();
  return read_all<codec::StereoFrame>(reader.value(), info.frames, info.sample_rate, stop);
#else
  auto reader = media::WavReader::open(file);
  if (!reader)
    return Result<Decoded>::failure(reader.error());
  return read_all<media::StereoFrame>(reader.value(), reader.value().frame_count(),
                                      reader.value().sample_rate(), stop);
#endif
}

// Renders `stream` over `map` into whole planar buffers.
Result<void> render_all(warp_dsp::WarpStream& stream, const warp_dsp::WarpTimeMap& map,
                        warp_dsp::SourceView& source, PreparedSample& out,
                        const std::stop_token& stop) {
  if (stream.start(map, source, 0) != warp_dsp::StreamStatus::ok)
    return Result<void>::failure({ErrorCode::unsupported, "Cannot stretch this sample"});
  const auto total = static_cast<std::size_t>(map.output_frames());
  out.left.assign(total, 0.0F);
  out.right.assign(total, 0.0F);
  std::size_t at = 0;
  while (at < total) {
    if (stop.stop_requested())
      return Result<void>::failure({ErrorCode::cancelled, cancelled_message});
    const auto count = std::min<std::size_t>(4096, total - at);
    const auto written = stream.render(std::span(out.left).subspan(at, count),
                                       std::span(out.right).subspan(at, count));
    if (written == 0)
      break;
    at += written;
  }
  if (stream.status() != warp_dsp::StreamStatus::ok &&
      stream.status() != warp_dsp::StreamStatus::out_of_range)
    return Result<void>::failure({ErrorCode::unsupported, "Cannot stretch this sample"});
  return Result<void>::success();
}

} // namespace

Result<PreparedSample> prepare_sample(const std::filesystem::path& file, std::uint32_t project_rate,
                                      std::optional<double> project_bpm,
                                      const std::stop_token& stop) {
  if (project_rate == 0)
    return Result<PreparedSample>::failure({ErrorCode::invalid_argument, "No project rate"});
  auto decoded = decode(file, stop);
  if (!decoded)
    return Result<PreparedSample>::failure(decoded.error());
  auto& source = decoded.value();
  PreparedSample out;
  const auto frames = static_cast<std::uint64_t>(source.left.size());
  if (frames == 0 || source.rate == 0)
    return Result<PreparedSample>::failure({ErrorCode::corrupt_data, "The sample is empty"});
  out.bpm = application::tempo_from_file_name(file.stem().string());
  if (!out.bpm && !source.truncated)
    out.bpm = application::tempo_from_loop_length(frames, source.rate, project_bpm.value_or(120.0));
  // Output length at the project's rate: the tempo ratio for a loop that
  // differs from the song, then the rate ratio.
  double speed = 1.0; // Song tempo over sample tempo.
  if (out.bpm && project_bpm && *project_bpm > 0.0) {
    const double ratio = *project_bpm / *out.bpm;
    if (ratio >= 0.5 && ratio <= 2.0 && std::abs(ratio - 1.0) > 0.002)
      speed = ratio;
  }
  const double rate_ratio = static_cast<double>(project_rate) / source.rate;
  if (speed == 1.0 && source.rate == project_rate) {
    out.left = std::move(source.left);
    out.right = std::move(source.right);
    return Result<PreparedSample>::success(std::move(out));
  }
  const auto output_frames =
      static_cast<std::uint64_t>(std::llround(static_cast<double>(frames) * rate_ratio / speed));
  auto map = warp_dsp::WarpTimeMap::create({{0, std::max<std::uint64_t>(output_frames, 1), 0,
                                             frames << warp_dsp::source_fraction_bits, false}});
  if (!map)
    return Result<PreparedSample>::failure(map.error());
  const warp_dsp::StreamLimits limits{project_rate, 4096};
  warp_dsp::MemorySourceView view(source.left, source.right);
  // A rate change only is band-limited varispeed, an exact resample. A
  // stretch reads the source as if at the project's rate; transposing by
  // the rates' ratio restores its pitch.
  warp_dsp::ComplexSettings settings;
  settings.transpose_cents = static_cast<std::int32_t>(
      std::lround(1200.0 * std::log2(static_cast<double>(source.rate) / project_rate)));
  auto stream = speed == 1.0 ? warp_dsp::create_repitch_stream(limits, warp_dsp::QualityTier::q2)
                             : warp_dsp::create_complex_stream(limits, warp_dsp::QualityTier::q2,
                                                               std::move(settings));
  if (!stream)
    return Result<PreparedSample>::failure(stream.error());
  auto rendered = render_all(*stream.value(), map.value(), view, out, stop);
  if (!rendered)
    return Result<PreparedSample>::failure(rendered.error());
  out.warped = speed != 1.0;
  return Result<PreparedSample>::success(std::move(out));
}

struct SamplePreview::Stream {
  PreparedSample sample;
  audio_io::TransportCursor cursor;
  std::unique_ptr<application::PlaybackOutput> output;
  // Callback only: frames of silence before the sample, then its position.
  std::uint64_t wait = 0;
  std::size_t position = 0;

  static bool render(void* opaque, std::span<engine::StereoFrame> frames,
                     engine::BlockContext /*block*/) noexcept {
    auto& stream = *static_cast<Stream*>(opaque);
    std::size_t index = 0;
    for (; index < frames.size() && stream.wait > 0; ++index, --stream.wait)
      frames[index] = {0.0F, 0.0F};
    const auto total = stream.sample.left.size();
    for (; index < frames.size(); ++index) {
      if (stream.position < total) {
        frames[index] = {stream.sample.left[stream.position] * preview_gain,
                         stream.sample.right[stream.position] * preview_gain};
        ++stream.position;
      } else {
        frames[index] = {0.0F, 0.0F};
      }
    }
    return true;
  }
};

SamplePreview::SamplePreview(ErrorSink error_sink)
    : error_sink_(std::move(error_sink)), worker_([this](std::stop_token token) { run(token); }) {}

SamplePreview::~SamplePreview() {
  worker_.request_stop();
  wake_.notify_all();
}

bool SamplePreview::play(Request request) {
  if (request.file.empty() || request.project_rate == 0 || request.buffer_frames == 0)
    return false;
  try {
    std::lock_guard lock(mutex_);
    // Only the newest request matters: a quick scroll previews where it lands.
    pending_ = std::move(request);
    stop_requested_ = false;
    wake_.notify_one();
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

void SamplePreview::stop() noexcept {
  try {
    std::lock_guard lock(mutex_);
    pending_.reset();
    stop_requested_ = true;
    wake_.notify_one();
  } catch (const std::exception&) {
    worker_.request_stop();
    wake_.notify_all();
  }
}

void SamplePreview::report(std::string message) const {
  if (!error_sink_)
    return;
  try {
    error_sink_(std::move(message));
  } catch (const std::exception&) {
  }
}

void SamplePreview::run(std::stop_token stop_token) noexcept {
  std::unique_ptr<Stream> stream;
  const auto halt = [&] {
    if (stream && stream->output)
      stream->output->stop();
    stream.reset();
  };
  while (!stop_token.stop_requested()) {
    std::optional<Request> request;
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, stop_token, [this] { return pending_.has_value() || stop_requested_; });
      if (stop_token.stop_requested())
        break;
      request = std::exchange(pending_, std::nullopt);
      stop_requested_ = false;
    }
    // Any new request or stop silences what plays now.
    halt();
    if (!request)
      continue;
    try {
      auto prepared =
          prepare_sample(request->file, request->project_rate, request->project_bpm, stop_token);
      if (!prepared) {
        if (prepared.error().message != cancelled_message)
          report("Cannot preview " + request->file.filename().string() + ": " +
                 prepared.error().message);
        continue;
      }
      {
        // A newer request arrived while this one decoded: skip it.
        std::lock_guard lock(mutex_);
        if (pending_ || stop_requested_)
          continue;
      }
      stream = std::make_unique<Stream>();
      stream->sample = std::move(prepared).value();
      // A loop waits for the song's next beat; a one-shot plays at once.
      if (request->clock && stream->sample.bpm) {
        const auto& clock = *request->clock;
        const auto rate = clock.sample_rate;
        const auto elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - clock.read_at).count();
        const auto now = static_cast<std::int64_t>(clock.playhead) +
                         static_cast<std::int64_t>(std::llround(std::max(0.0, elapsed) * rate));
        const auto beat = application::next_beat_sample(clock.tempo, now, rate);
        if (beat && *beat >= now)
          stream->wait = static_cast<std::uint64_t>(*beat - now);
      }
      const audio_io::StereoRenderSource source{stream.get(), &Stream::render, nullptr};
      stream->output = request->output_factory
                           ? request->output_factory(source, stream->cursor)
                           : std::make_unique<PreviewPipeWireOutput>(source, stream->cursor);
      if (!stream->output) {
        report("Sample preview has no audio output");
        stream.reset();
        continue;
      }
      const auto started = stream->output->start(request->project_rate, request->buffer_frames,
                                                 request->target_serial);
      if (!started) {
        report("Cannot start the sample preview: " + started.error().message);
        halt();
      }
    } catch (const std::exception& error) {
      report(std::string("Sample preview failed: ") + error.what());
      halt();
    }
  }
  halt();
}

} // namespace oscillate::quick
