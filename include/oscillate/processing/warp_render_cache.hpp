#pragma once

#include "oscillate/media/cache_store.hpp"
#include "oscillate/media/prefetch.hpp"
#include "oscillate/media/processing_job.hpp"
#include "oscillate/processing/clip_frame_plan.hpp"
#include "oscillate/warp_dsp/analysis.hpp"
#include "oscillate/warp_dsp/stream.hpp"
#include "oscillate/warp_dsp/time_map.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>
#include <vector>

namespace oscillate::processing {

// Production processed-clip render (ADR-0026 points 4-7). All of this is
// control- or worker-side; nothing here may run on the audio callback.

// round(100 x semitones) cents with exact half-cent ties away from zero,
// evaluated on the exact value of the stored double (ADR-0026 point 4).
// Callers pass a validated, finite pitch; the result is within +-4800.
[[nodiscard]] std::int32_t clip_transpose_cents(double semitones) noexcept;

// The settings a stream actually applies. Only the active mode's fields are
// meaningful; the cache key encodes exactly those, after intent rules:
// Re-Pitch ignores transpose and Complex Pro applies formants only when the
// clip's formant intent is set.
struct AppliedWarpSettings {
  project::WarpMode mode = project::WarpMode::complex;
  std::int32_t transpose_cents = 0;
  warp_dsp::BeatsLoop beats_loop = warp_dsp::BeatsLoop::forward;
  float beats_envelope = 100.0F;
  float tones_grain_ms = 40.0F;
  float texture_grain_ms = 100.0F;
  float texture_flux = 0.0F;
  std::uint64_t texture_seed = 0;
  float formants = 0.0F;
  float complex_pro_envelope = 128.0F;
  bool operator==(const AppliedWarpSettings&) const = default;
};
// Texture's grain-jitter seed is fixed so a render is a pure function of its key.
inline constexpr std::uint64_t texture_render_seed = 1;
[[nodiscard]] AppliedWarpSettings applied_warp_settings(const project::ClipProcessing& processing);

// Engine identity per mode, part of the key. Bump a mode's version whenever
// its rendered samples or its analysis inputs change.
[[nodiscard]] std::string_view warp_algorithm_id(project::WarpMode mode) noexcept;
[[nodiscard]] std::uint32_t warp_algorithm_version(project::WarpMode mode) noexcept;

// Everything one processed render depends on, resolved from a project
// snapshot through the authoritative clip frame plan.
// A Clay take under the clip (ADR-0081): the request's `source` is then the
// take's render, opened through open_request_view rather than the store.
struct ClayInput {
  project::SourceRecord original;
  project::ClayTake take;
  bool operator==(const ClayInput&) const = default;
};
struct WarpRenderRequest {
  project::SourceRecord source;
  project::ClipId clip;
  std::uint64_t settings_generation = 0;
  ClipFramePlan plan;
  std::uint32_t project_rate = 0;
  AppliedWarpSettings settings;
  warp_dsp::QualityTier quality = warp_dsp::QualityTier::q0;
  std::optional<ClayInput> clay = std::nullopt;
};

class ProjectRateSourceView;
// The request's source at the project rate: the managed source, or the Clay
// render it names (rendered first when the cache lacks it). Worker-only.
[[nodiscard]] foundation::Result<std::unique_ptr<ProjectRateSourceView>>
open_request_view(const std::filesystem::path& project_root, const WarpRenderRequest& request,
                  std::stop_token stop = {});

// Fails with not_found for a missing clip or source, and with unsupported
// when the clip plays without processing (no active pitch, no markers and a
// placement equal to its rate-converted source duration) or uses an external
// link. Export and cache renders use Q0; live step-down may request Q1/Q2.
// The one decision every playback and export path shares: nullopt when the
// clip plays its source unprocessed (no active pitch, no reviewed markers and
// a placement equal to its rate-converted source duration), otherwise its
// render request through the authoritative clip frame plan. Fails for a
// missing clip or source, and for a processed clip on an external link.
[[nodiscard]] foundation::Result<std::optional<WarpRenderRequest>>
processed_clip_request(const project::Snapshot& snapshot, project::ClipId clip,
                       warp_dsp::QualityTier quality = warp_dsp::QualityTier::q0);

[[nodiscard]] foundation::Result<WarpRenderRequest>
warp_render_request(const project::Snapshot& snapshot, project::ClipId clip,
                    warp_dsp::QualityTier quality = warp_dsp::QualityTier::q0);

// What a mode's stream needs from its source: transients (Beats, Complex,
// Complex Pro) or pitch marks (Tones), in project-rate view frames. Analysis
// covers the whole view, so it depends on the source alone. Worker-only;
// `cancelled` is polled between analysis frames.
struct WarpSourceAnalysis {
  std::vector<std::uint64_t> transients;
  std::vector<warp_dsp::analysis::PitchMark> pitch_marks;
};
[[nodiscard]] foundation::Result<WarpSourceAnalysis>
analyse_warp_source(const WarpRenderRequest& request, warp_dsp::SourceView& view,
                    const std::atomic<bool>* cancelled = nullptr);

// Analysis is nearly all the cost of opening a stretched clip, and it depends
// on the source alone (at the project rate, and the mode's kind of analysis),
// never on tempo, markers or where the clip sits. It is kept in the project's
// cache, once per source and kind, as Ableton keeps .asd files: a tempo change
// then only restarts the real-time stretch. Absent for modes that analyse
// nothing (Re-Pitch, Texture).
[[nodiscard]] foundation::Result<std::optional<media::CacheKey>>
warp_analysis_cache_key(const WarpRenderRequest& request);
// The source's analysis from `cache` when it holds it, else analysed (and
// kept there when `cache` is given). `progress` rises 0..1 as an analysis
// reads the source.
[[nodiscard]] foundation::Result<WarpSourceAnalysis>
cached_warp_analysis(media::CacheStore* cache, const WarpRenderRequest& request,
                     warp_dsp::SourceView& view, const std::atomic<bool>* cancelled = nullptr,
                     std::atomic<double>* progress = nullptr);
// Worker-only: makes sure the project's cache holds the request's analysis.
[[nodiscard]] foundation::Result<void>
prepare_warp_analysis(const std::filesystem::path& project_root, const WarpRenderRequest& request,
                      std::atomic<double>* progress = nullptr, std::stop_token stop = {});
// Whether `cache` holds the request's analysis (or it needs none).
[[nodiscard]] bool warp_analysis_ready(const media::CacheStore& cache,
                                       const WarpRenderRequest& request);

// The mode's stream at `tier`, built from the request's applied settings.
// Every allocation happens here. `map` must outlive the stream: Tones keeps a
// map-specific index that makes every seek exact. Cache and live renders use
// this one factory, so equal inputs give equal samples.
[[nodiscard]] foundation::Result<std::unique_ptr<warp_dsp::WarpStream>>
create_warp_stream(const WarpRenderRequest& request, const WarpSourceAnalysis& analysis,
                   const warp_dsp::WarpTimeMap& map, warp_dsp::QualityTier tier,
                   std::uint32_t max_render_frames);

// Sensitive to source identity, project rate, clip ID and settings
// generation, the canonical map, the applied mode parameters, quality tier,
// per-mode algorithm version and the rate-adapter version.
[[nodiscard]] foundation::Result<media::CacheKey>
warp_render_cache_key(const WarpRenderRequest& request);

// Opens and verifies exact saved source bytes at the project rate, analyses
// what the mode needs and streams an LDW2 payload in bounded chunks.
// Cancellation and ticket checks run before publication and between chunks;
// CacheStore owns atomic publication and interruption cleanup, so a failed or
// cancelled render leaves any previous entry in place.
// `progress`, when given, rises from 0 to 1 as the render goes (analysis is
// its first 5%), for showing the clip filling in.
[[nodiscard]] foundation::Result<media::CacheKey>
render_warp_to_cache(media::CacheStore& cache, const std::filesystem::path& project_root,
                     const WarpRenderRequest& request, std::stop_token stop = {},
                     const media::JobTicket* ticket = nullptr,
                     std::atomic<double>* progress = nullptr);

// Shape a reader must find in the LDW2 header before any seek read.
struct WarpPayloadShape {
  std::uint64_t frames = 0;
  project::WarpMode mode = project::WarpMode::complex;
  warp_dsp::QualityTier quality = warp_dsp::QualityTier::q0;
};

class WarpRenderCacheReader {
public:
  static constexpr std::size_t max_read_frames = 8'192;
  WarpRenderCacheReader(const WarpRenderCacheReader&) = delete;
  WarpRenderCacheReader& operator=(const WarpRenderCacheReader&) = delete;
  WarpRenderCacheReader(WarpRenderCacheReader&&) noexcept = default;
  WarpRenderCacheReader& operator=(WarpRenderCacheReader&&) noexcept = default;

  // Validates the store digest, then the LDW2 header (format, key digest,
  // mode, tier, channel count, frame count) and every sample's finiteness
  // before exposing seek reads. Any mismatch, including an LDW1 entry, is a
  // miss. Source identity must be reverified separately.
  [[nodiscard]] static foundation::Result<std::optional<WarpRenderCacheReader>>
  open(media::CacheStore& cache, const media::CacheKey& key, WarpPayloadShape expected);
  [[nodiscard]] std::uint64_t frame_count() const noexcept { return frames_; }
  // Worker-side seek, at most max_read_frames per request.
  [[nodiscard]] foundation::Result<std::size_t> read_at(std::uint64_t first_frame,
                                                        std::span<media::StereoFrame> output) const;

private:
  WarpRenderCacheReader(media::ValidatedCacheReader reader, std::uint64_t frames) noexcept
      : reader_(std::move(reader)), frames_(frames) {}
  media::ValidatedCacheReader reader_;
  std::uint64_t frames_;
};

// Worker-only transfer of clip-local processed frames into a clip's preallocated
// ring. PrefetchCursor::source_frame denotes the processed output frame here;
// the source media coordinate is already resolved by the cached warp map.
// The caller must prove that this cache key still belongs to the current
// project revision before publishing the ring to playback.
[[nodiscard]] foundation::Result<std::size_t>
fill_from_warp_render_cache(const WarpRenderCacheReader& source, media::PrefetchRing& ring,
                            media::PrefetchCursor cursor, std::span<media::StereoFrame> scratch);

} // namespace oscillate::processing
