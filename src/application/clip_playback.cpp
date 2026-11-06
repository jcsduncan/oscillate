#include <oscillate/application/clip_playback.hpp>
#include <oscillate/processing/clip_frame_plan.hpp>
#include <oscillate/project/clip_fade.hpp>

#include <algorithm>
#include <bit>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace oscillate::application {
namespace {

using foundation::ErrorCode;
using foundation::Result;

[[nodiscard]] bool playback_encoding_supported(project::SourceEncoding encoding) noexcept {
  if (encoding == project::SourceEncoding::wav) {
    return true;
  }
#if OSCILLATE_HAVE_CODEC
  return encoding == project::SourceEncoding::aiff || encoding == project::SourceEncoding::flac ||
         encoding == project::SourceEncoding::mp3;
#else
  return false;
#endif
}

struct ClipGeometry {
  std::uint64_t project_start;
  std::uint64_t project_end;
  std::uint64_t source_start;
};

class ClipRuntime final : public engine::ProcessorRuntime {
public:
  ClipRuntime(ClipGeometry geometry, std::shared_ptr<media::PrefetchRing> ring,
              std::shared_ptr<PlaybackReadiness> readiness, project::PreparedClipFades fades,
              std::size_t max_frames)
      : geometry_(geometry), ring_(std::move(ring)), readiness_(std::move(readiness)),
        fades_(fades), scratch_(max_frames) {}

  void process(std::span<engine::StereoFrame> output,
               engine::BlockContext context) noexcept override {
    if (output.empty() || output.size() > scratch_.size() || context.transport_generation == 0 ||
        output.size() >
            std::numeric_limits<std::uint64_t>::max() - context.project_sample_position) {
      return;
    }
    const auto block_end = context.project_sample_position + output.size();
    const auto first = std::max(context.project_sample_position, geometry_.project_start);
    const auto last = std::min(block_end, geometry_.project_end);
    if (first >= last) {
      return;
    }
    const auto output_offset = static_cast<std::size_t>(first - context.project_sample_position);
    const auto count = static_cast<std::size_t>(last - first);
    const auto source_position = geometry_.source_start + first - geometry_.project_start;
    const auto consumed =
        ring_->consume(context.transport_generation, source_position,
                       std::span<media::StereoFrame>(scratch_).first(count), readiness_->steady());
    // The ring fills missing frames with silence. Keep channels separate and
    // add to the graph's existing mix without aliasing the media frame type.
    for (std::size_t index = 0; index < count; ++index) {
      const auto gain = project::clip_fade_gain(fades_, first - geometry_.project_start + index);
      output[output_offset + index].left += static_cast<float>(scratch_[index].left * gain);
      output[output_offset + index].right += static_cast<float>(scratch_[index].right * gain);
    }
    (void)consumed;
  }

private:
  ClipGeometry geometry_;
  std::shared_ptr<media::PrefetchRing> ring_;
  std::shared_ptr<PlaybackReadiness> readiness_;
  project::PreparedClipFades fades_;
  std::vector<media::StereoFrame> scratch_;
};

[[nodiscard]] Result<engine::NodeRequest> fail(ErrorCode code, std::string message) {
  return Result<engine::NodeRequest>::failure({code, std::move(message)});
}

[[nodiscard]] std::uint64_t mix(std::uint64_t hash, std::uint64_t value) noexcept {
  constexpr std::uint64_t prime = 1'099'511'628'211ULL;
  for (unsigned shift = 0; shift < 64; shift += 8) {
    hash ^= (value >> shift) & 0xffU;
    hash *= prime;
  }
  return hash;
}

[[nodiscard]] std::uint64_t compatibility_key(const project::Clip& clip,
                                              const project::SourceRecord& source,
                                              ClipGeometry geometry,
                                              const media::PrefetchRing* ring,
                                              const PlaybackReadiness* readiness) noexcept {
  constexpr std::uint64_t offset = 14'695'981'039'346'656'037ULL;
  auto hash = offset;
  hash = mix(hash, clip.id.bits().high);
  hash = mix(hash, clip.id.bits().low);
  hash = mix(hash, clip.source.bits().high);
  hash = mix(hash, clip.source.bits().low);
  hash = mix(hash, geometry.project_start);
  hash = mix(hash, geometry.project_end);
  hash = mix(hash, geometry.source_start);
  hash = mix(hash, clip.settings_generation);
  hash = mix(hash, std::bit_cast<std::uint64_t>(clip.fades.in_fraction));
  hash = mix(hash, std::bit_cast<std::uint64_t>(clip.fades.out_fraction));
  hash = mix(hash, static_cast<std::uint64_t>(clip.fades.in_curve));
  hash = mix(hash, static_cast<std::uint64_t>(clip.fades.out_curve));
  hash = mix(hash, reinterpret_cast<std::uintptr_t>(ring));
  hash = mix(hash, reinterpret_cast<std::uintptr_t>(readiness));
  for (const char digit : source.sha256) {
    hash = mix(hash, static_cast<unsigned char>(digit));
  }
  return hash == 0 ? 1 : hash;
}

} // namespace

Result<engine::NodeRequest> make_clip_request(const project::Snapshot& snapshot,
                                              project::ClipId clip_id,
                                              std::shared_ptr<media::PrefetchRing> ring,
                                              std::shared_ptr<PlaybackReadiness> readiness,
                                              std::uint64_t instance_id, std::size_t max_frames) {
  if (!ring || !readiness || ring->clip() != clip_id || instance_id == 0 || max_frames == 0 ||
      max_frames > 8192) {
    return fail(ErrorCode::invalid_argument, "invalid clip feed or graph capacity");
  }
  const project::Clip* clip = nullptr;
  for (const auto& track : snapshot.tracks) {
    const auto found = std::ranges::find_if(
        track.clips, [clip_id](const auto& item) { return item.id == clip_id; });
    if (found != track.clips.end()) {
      clip = &*found;
      break;
    }
  }
  if (clip == nullptr) {
    return fail(ErrorCode::not_found, "clip is not in the project snapshot");
  }
  if (clip->processing.requires_rendering()) {
    return fail(ErrorCode::unsupported,
                "clip pitch or reviewed warp markers need a processed source");
  }
  const auto source = std::ranges::find_if(
      snapshot.sources, [clip](const auto& item) { return item.id == clip->source; });
  if (source == snapshot.sources.end()) {
    return fail(ErrorCode::unavailable, "clip source has no managed record");
  }
  if (!playback_encoding_supported(source->encoding) ||
      source->sample_rate != snapshot.sample_rate ||
      (source->channels != 1 && source->channels != 2)) {
    return fail(ErrorCode::unsupported,
                "clip source requires unsupported decoding or rate conversion");
  }
  auto planned =
      processing::prepare_clip_frame_plan(*clip, *source, snapshot.tempo, snapshot.sample_rate);
  if (!planned) {
    return fail(planned.error().code, planned.error().message);
  }
  const auto& frames = planned.value();
  const auto duration = frames.warp.output_frames();
  if (duration != frames.warp.source_frames()) {
    return fail(ErrorCode::unsupported,
                "M2 clip playback requires one source frame per project frame");
  }
  const ClipGeometry geometry{frames.project_start_frame, frames.project_end_frame(),
                              frames.source_start_frame};
  auto fades = project::prepare_clip_fades(clip->fades, duration);
  if (!fades)
    return fail(fades.error().code, fades.error().message);
  const auto key = compatibility_key(*clip, *source, geometry, ring.get(), readiness.get());
  return Result<engine::NodeRequest>::success(
      {instance_id, key,
       [geometry, ring = std::move(ring), readiness = std::move(readiness), fades = fades.value(),
        max_frames] {
         return std::make_unique<ClipRuntime>(geometry, ring, readiness, fades, max_frames);
       }});
}

} // namespace oscillate::application
