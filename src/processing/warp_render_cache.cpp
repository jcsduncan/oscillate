#include "oscillate/processing/warp_render_cache.hpp"

#include "oscillate/processing/clay_source.hpp"

#include "oscillate/persistence/store.hpp"
#include "oscillate/processing/verified_source_view.hpp"
#include "oscillate/processing/warp_stream_map.hpp"
#include "oscillate/warp_dsp/analysis.hpp"
#include "oscillate/warp_dsp/rate_view.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace oscillate::processing {
namespace {
using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;

// LDW2 payload: a 96-byte little-endian header, then interleaved f32 stereo.
//   0 magic "LDW2"      4 format version     8 frame count (u64)
//  16 mode (u8)        17 tier (u8)        18 channels (u16, 2)
//  20 reserved zero    24 key digest, 64 lowercase hex bytes
//  88 reserved zero
constexpr std::array<std::byte, 4> magic{std::byte{'L'}, std::byte{'D'}, std::byte{'W'},
                                         std::byte{'2'}};
constexpr std::uint32_t format_version = 2;
constexpr std::size_t header_bytes = 96;
constexpr std::size_t digest_offset = 24;
constexpr std::size_t digest_bytes = 64;
constexpr std::uint16_t channels = 2;
constexpr std::size_t chunk_frames = WarpRenderCacheReader::max_read_frames;
// The longest render kept as a cache file (ten minutes at 96 kHz, about
// 460 MB). A longer clip plays, live and exported, from its stream.
constexpr std::uint64_t max_cached_frames = 96'000ULL * 600ULL;
// Transient and pitch-analysis settings shared by every analysed mode.
// Changing them changes rendered output, so bump the affected mode versions.
constexpr float transient_threshold_scale = 1.0F;
constexpr double transient_minimum_gap_seconds = 0.030;

void store_u16(std::span<std::byte> out, std::uint16_t value) noexcept {
  out[0] = static_cast<std::byte>(value & 0xFFU);
  out[1] = static_cast<std::byte>(value >> 8U);
}
void store_u32(std::span<std::byte> out, std::uint32_t value) noexcept {
  for (unsigned index = 0; index < 4; ++index)
    out[index] = static_cast<std::byte>(value >> (8U * index));
}
void store_u64(std::span<std::byte> out, std::uint64_t value) noexcept {
  for (unsigned index = 0; index < 8; ++index)
    out[index] = static_cast<std::byte>(value >> (8U * index));
}
[[nodiscard]] std::uint16_t load_u16(std::span<const std::byte> in) noexcept {
  return static_cast<std::uint16_t>(std::to_integer<unsigned>(in[0]) |
                                    (std::to_integer<unsigned>(in[1]) << 8U));
}
[[nodiscard]] std::uint32_t load_u32(std::span<const std::byte> in) noexcept {
  std::uint32_t value = 0;
  for (unsigned index = 0; index < 4; ++index)
    value |= std::uint32_t(std::to_integer<unsigned>(in[index])) << (8U * index);
  return value;
}
[[nodiscard]] std::uint64_t load_u64(std::span<const std::byte> in) noexcept {
  std::uint64_t value = 0;
  for (unsigned index = 0; index < 8; ++index)
    value |= std::uint64_t(std::to_integer<unsigned>(in[index])) << (8U * index);
  return value;
}
void append_u64(std::vector<std::byte>& out, std::uint64_t value) {
  const auto begin = out.size();
  out.resize(begin + 8);
  store_u64(std::span(out).subspan(begin, 8), value);
}
void append_u32(std::vector<std::byte>& out, std::uint32_t value) {
  const auto begin = out.size();
  out.resize(begin + 4);
  store_u32(std::span(out).subspan(begin, 4), value);
}
void append_f32(std::vector<std::byte>& out, float value) {
  append_u32(out, std::bit_cast<std::uint32_t>(value));
}
void append_text(std::vector<std::byte>& out, std::string_view text) {
  append_u64(out, text.size());
  for (const char character : text)
    out.push_back(static_cast<std::byte>(character));
}
void store_f32(std::span<std::byte> out, float value) noexcept {
  store_u32(out, std::bit_cast<std::uint32_t>(value));
}
[[nodiscard]] float load_f32(std::span<const std::byte> in) noexcept {
  return std::bit_cast<float>(load_u32(in));
}

[[nodiscard]] std::uint8_t mode_byte(project::WarpMode mode) noexcept {
  return static_cast<std::uint8_t>(mode);
}

[[nodiscard]] warp_dsp::BeatsLoop beats_loop(project::WarpBeatsLoop loop) noexcept {
  switch (loop) {
  case project::WarpBeatsLoop::off:
    return warp_dsp::BeatsLoop::off;
  case project::WarpBeatsLoop::back_and_forth:
    return warp_dsp::BeatsLoop::back_and_forth;
  case project::WarpBeatsLoop::forward:
    break;
  }
  return warp_dsp::BeatsLoop::forward;
}

[[nodiscard]] bool known_mode(project::WarpMode mode) noexcept {
  switch (mode) {
  case project::WarpMode::repitch:
  case project::WarpMode::beats:
  case project::WarpMode::tones:
  case project::WarpMode::texture:
  case project::WarpMode::complex:
  case project::WarpMode::complex_pro:
    return true;
  }
  return false;
}

[[nodiscard]] Result<void> validate_request(const WarpRenderRequest& request) {
  const auto& plan = request.plan;
  if (!known_mode(request.settings.mode))
    return Result<void>::failure({ErrorCode::unsupported, "Unknown warp render mode"});
  if (!warp_dsp::valid_quality_tier(request.quality) || request.settings_generation == 0 ||
      plan.warp.output_frames() == 0 || plan.source_start_frame > request.source.frames ||
      plan.warp.source_frames() > request.source.frames - plan.source_start_frame ||
      std::abs(request.settings.transpose_cents) > warp_dsp::max_transpose_cents ||
      (request.settings.mode == project::WarpMode::repitch &&
       request.settings.transpose_cents != 0))
    return Result<void>::failure({ErrorCode::invalid_argument, "Invalid warp render request"});
  if (request.source.external_link)
    return Result<void>::failure(
        {ErrorCode::unsupported,
         "Warp processing needs a managed source; collect the linked file into the project"});
  if (auto record = project::validate_source_record(request.source); !record)
    return Result<void>::failure(record.error());
  if (auto rates = warp_dsp::RateConvertedSourceView::supports(request.source.sample_rate,
                                                               request.project_rate);
      !rates)
    return Result<void>::failure(rates.error());
  return Result<void>::success();
}

[[nodiscard]] Result<void> cancelled(const std::stop_token& stop, const media::JobTicket* ticket) {
  if (stop.stop_requested() || (ticket && ticket->cancelled()))
    return Result<void>::failure({ErrorCode::cancelled, "Warp cache render cancelled"});
  return Result<void>::success();
}

[[nodiscard]] std::vector<std::uint64_t>
transient_frames(const std::vector<warp_dsp::analysis::Transient>& found) {
  std::vector<std::uint64_t> frames;
  frames.reserve(found.size());
  for (const auto& transient : found)
    frames.push_back(transient.frame);
  return frames;
}

// Sets the analysis cancellation flag from a stop token for the render's life.
class StopFlag {
public:
  explicit StopFlag(const std::stop_token& stop)
      : callback_(stop, [this]() noexcept { flag_.store(true, std::memory_order_relaxed); }) {}
  [[nodiscard]] const std::atomic<bool>& flag() const noexcept { return flag_; }

private:
  std::atomic<bool> flag_{false};
  std::stop_callback<std::function<void()>> callback_;
};

} // namespace

std::int32_t clip_transpose_cents(double semitones) noexcept {
  // p is 100 x semitones rounded; error is the exact residual, so p + error
  // is the exact product. Only an exact tie at p decides by error's sign.
  const double product = 100.0 * semitones;
  const double error = std::fma(100.0, semitones, -product);
  double whole = std::floor(product);
  double fraction = product - whole; // exact: |product| <= 4800
  if (fraction == 0.0 && error < 0.0) {
    whole -= 1.0;
    fraction = 1.0;
  }
  // An exact tie (zero residual) rounds away from zero: up when the floor is
  // non-negative, otherwise down to the floor itself.
  double rounded = whole;
  if (fraction > 0.5 || (fraction == 0.5 && (error > 0.0 || (error == 0.0 && whole >= 0.0))))
    rounded = whole + 1.0;
  return static_cast<std::int32_t>(rounded);
}

AppliedWarpSettings applied_warp_settings(const project::ClipProcessing& processing) {
  const auto& warp = processing.warp;
  AppliedWarpSettings applied;
  applied.mode = warp.mode;
  if (warp.mode != project::WarpMode::repitch)
    applied.transpose_cents = clip_transpose_cents(processing.pitch_semitones);
  switch (warp.mode) {
  case project::WarpMode::beats:
    applied.beats_loop = beats_loop(warp.beats.loop);
    applied.beats_envelope = static_cast<float>(warp.beats.envelope);
    break;
  case project::WarpMode::tones:
    applied.tones_grain_ms = static_cast<float>(warp.tones.grain_ms);
    break;
  case project::WarpMode::texture:
    applied.texture_grain_ms = static_cast<float>(warp.texture.grain_ms);
    applied.texture_flux = static_cast<float>(warp.texture.flux);
    applied.texture_seed = texture_render_seed;
    break;
  case project::WarpMode::complex_pro:
    applied.formants =
        processing.preserve_formants ? static_cast<float>(warp.complex_pro.formants) : 0.0F;
    applied.complex_pro_envelope = static_cast<float>(warp.complex_pro.envelope);
    break;
  case project::WarpMode::repitch:
  case project::WarpMode::complex:
    break;
  }
  return applied;
}

std::string_view warp_algorithm_id(project::WarpMode mode) noexcept {
  switch (mode) {
  case project::WarpMode::repitch:
    return "oscillate.warp_dsp.repitch";
  case project::WarpMode::beats:
    return "oscillate.warp_dsp.beats";
  case project::WarpMode::tones:
    return "oscillate.warp_dsp.tones";
  case project::WarpMode::texture:
    return "oscillate.warp_dsp.texture";
  case project::WarpMode::complex:
    return "oscillate.warp_dsp.complex";
  case project::WarpMode::complex_pro:
    return "oscillate.warp_dsp.complex_pro";
  }
  return "oscillate.warp_dsp.unknown";
}

std::uint32_t warp_algorithm_version(project::WarpMode mode) noexcept {
  switch (mode) {
  case project::WarpMode::repitch:
    return 2U; // LDW1 used version 1 for a same-rate-only key
  case project::WarpMode::complex_pro:
    return 2U; // formant shaping keeps each frame's energy
  default:
    return 1U;
  }
}

Result<WarpSourceAnalysis> analyse_warp_source(const WarpRenderRequest& request,
                                               warp_dsp::SourceView& view,
                                               const std::atomic<bool>* cancelled) {
  WarpSourceAnalysis analysis;
  switch (request.settings.mode) {
  case project::WarpMode::beats:
  case project::WarpMode::complex:
  case project::WarpMode::complex_pro: {
    auto found = warp_dsp::analysis::detect_transients(
        view, {request.project_rate, transient_threshold_scale, transient_minimum_gap_seconds},
        cancelled);
    if (!found)
      return Result<WarpSourceAnalysis>::failure(found.error());
    analysis.transients = transient_frames(found.value());
    break;
  }
  case project::WarpMode::tones: {
    auto track =
        warp_dsp::analysis::track_pitch(view, {.sample_rate = request.project_rate}, cancelled);
    if (!track)
      return Result<WarpSourceAnalysis>::failure(track.error());
    auto marks = warp_dsp::analysis::place_pitch_marks(view, track.value(), request.project_rate);
    if (!marks)
      return Result<WarpSourceAnalysis>::failure(marks.error());
    analysis.pitch_marks = std::move(marks).value();
    break;
  }
  case project::WarpMode::repitch:
  case project::WarpMode::texture:
    break;
  }
  return Result<WarpSourceAnalysis>::success(std::move(analysis));
}

namespace {

constexpr std::array<std::byte, 4> analysis_magic{std::byte{'L'}, std::byte{'D'}, std::byte{'W'},
                                                  std::byte{'A'}};
constexpr std::uint32_t analysis_format_version = 1;
enum class AnalysisKind : std::uint8_t { none = 0, transients = 1, pitch_marks = 2 };

[[nodiscard]] AnalysisKind analysis_kind(project::WarpMode mode) noexcept {
  switch (mode) {
  case project::WarpMode::beats:
  case project::WarpMode::complex:
  case project::WarpMode::complex_pro:
    return AnalysisKind::transients;
  case project::WarpMode::tones:
    return AnalysisKind::pitch_marks;
  case project::WarpMode::repitch:
  case project::WarpMode::texture:
    break;
  }
  return AnalysisKind::none;
}

[[nodiscard]] std::vector<std::byte> encode_analysis(AnalysisKind kind,
                                                     const WarpSourceAnalysis& analysis) {
  std::vector<std::byte> out(analysis_magic.begin(), analysis_magic.end());
  append_u32(out, analysis_format_version);
  out.push_back(static_cast<std::byte>(kind));
  if (kind == AnalysisKind::transients) {
    append_u64(out, analysis.transients.size());
    for (const auto frame : analysis.transients)
      append_u64(out, frame);
  } else {
    append_u64(out, analysis.pitch_marks.size());
    for (const auto& mark : analysis.pitch_marks) {
      append_u64(out, mark.frame);
      append_f32(out, mark.period);
    }
  }
  return out;
}

[[nodiscard]] std::optional<WarpSourceAnalysis> decode_analysis(AnalysisKind kind,
                                                                std::span<const std::byte> in) {
  constexpr std::size_t header = 4 + 4 + 1 + 8;
  if (in.size() < header || !std::equal(analysis_magic.begin(), analysis_magic.end(), in.begin()) ||
      load_u32(in.subspan(4, 4)) != analysis_format_version ||
      in[8] != static_cast<std::byte>(kind))
    return std::nullopt;
  const auto count = load_u64(in.subspan(9, 8));
  const std::size_t entry = kind == AnalysisKind::transients ? 8 : 12;
  if (count > (in.size() - header) / entry || in.size() != header + count * entry)
    return std::nullopt;
  WarpSourceAnalysis analysis;
  for (std::uint64_t index = 0; index < count; ++index) {
    const auto at = in.subspan(header + index * entry, entry);
    if (kind == AnalysisKind::transients)
      analysis.transients.push_back(load_u64(at.first(8)));
    else
      analysis.pitch_marks.push_back({load_u64(at.first(8)), load_f32(at.subspan(8, 4))});
  }
  return analysis;
}

// Reports how far an analysis has read through its source.
class ProgressView final : public warp_dsp::SourceView {
public:
  ProgressView(warp_dsp::SourceView& inner, std::atomic<double>& progress)
      : inner_(inner), progress_(progress) {}
  [[nodiscard]] std::uint64_t frames() const noexcept override { return inner_.frames(); }
  [[nodiscard]] bool read(std::int64_t first, std::span<float> left,
                          std::span<float> right) noexcept override {
    const auto ok = inner_.read(first, left, right);
    const auto total = inner_.frames();
    if (ok && total > 0 && first >= 0) {
      const auto reached =
          std::min(1.0, static_cast<double>(first + static_cast<std::int64_t>(left.size())) /
                            static_cast<double>(total));
      if (reached > progress_.load(std::memory_order_relaxed))
        progress_.store(reached, std::memory_order_relaxed);
    }
    return ok;
  }

private:
  warp_dsp::SourceView& inner_;
  std::atomic<double>& progress_;
};

} // namespace

Result<std::optional<media::CacheKey>> warp_analysis_cache_key(const WarpRenderRequest& request) {
  const auto kind = analysis_kind(request.settings.mode);
  if (kind == AnalysisKind::none)
    return Result<std::optional<media::CacheKey>>::success(std::nullopt);
  std::vector<std::byte> settings;
  settings.push_back(static_cast<std::byte>(kind));
  append_f32(settings, transient_threshold_scale);
  append_u64(settings, std::bit_cast<std::uint64_t>(transient_minimum_gap_seconds));
  auto key = media::make_cache_key({.artifact = media::CacheArtifact::source_analysis,
                                    .source = request.source,
                                    .clip = std::nullopt,
                                    .project_rate = request.project_rate,
                                    .algorithm_id = "warp-analysis",
                                    .algorithm_version = 1,
                                    .quality = 1,
                                    .canonical_settings = std::move(settings),
                                    .tempo_sha256 = std::nullopt});
  if (!key)
    return Result<std::optional<media::CacheKey>>::failure(key.error());
  return Result<std::optional<media::CacheKey>>::success(key.value());
}

Result<WarpSourceAnalysis> cached_warp_analysis(media::CacheStore* cache,
                                                const WarpRenderRequest& request,
                                                warp_dsp::SourceView& view,
                                                const std::atomic<bool>* cancelled,
                                                std::atomic<double>* progress) {
  const auto kind = analysis_kind(request.settings.mode);
  auto key = warp_analysis_cache_key(request);
  if (!key)
    return Result<WarpSourceAnalysis>::failure(key.error());
  if (cache != nullptr && key.value()) {
    // A cache filesystem error only means analysing again.
    if (auto stored = cache->get(*key.value()); stored && stored.value())
      if (auto decoded = decode_analysis(kind, *stored.value())) {
        if (progress)
          progress->store(1.0, std::memory_order_relaxed);
        return Result<WarpSourceAnalysis>::success(std::move(*decoded));
      }
  }
  std::atomic<double> unused{0.0};
  ProgressView watched(view, progress ? *progress : unused);
  auto analysis = analyse_warp_source(request, watched, cancelled);
  if (!analysis)
    return analysis;
  if (cache != nullptr && key.value())
    (void)cache->put(*key.value(), encode_analysis(kind, analysis.value()));
  if (progress)
    progress->store(1.0, std::memory_order_relaxed);
  return analysis;
}

Result<void> prepare_warp_analysis(const std::filesystem::path& project_root,
                                   const WarpRenderRequest& request, std::atomic<double>* progress,
                                   std::stop_token stop) {
  auto store = persistence::ProjectStore::open_reader(project_root);
  if (!store)
    return Result<void>::failure(store.error());
  auto directory = store.value().open_cache_directory();
  if (!directory)
    return Result<void>::failure(directory.error());
  auto cache = media::CacheStore::adopt_directory_fd(directory.value());
  if (!cache)
    return Result<void>::failure(cache.error());
  if (warp_analysis_ready(cache.value(), request)) {
    if (progress)
      progress->store(1.0, std::memory_order_relaxed);
    return Result<void>::success();
  }
  auto view = open_request_view(project_root, request, stop);
  if (!view)
    return Result<void>::failure(view.error());
  const StopFlag stop_flag(stop);
  auto analysis =
      cached_warp_analysis(&cache.value(), request, *view.value(), &stop_flag.flag(), progress);
  if (!analysis)
    return Result<void>::failure(analysis.error());
  return Result<void>::success();
}

bool warp_analysis_ready(const media::CacheStore& cache, const WarpRenderRequest& request) {
  auto key = warp_analysis_cache_key(request);
  if (!key)
    return false;
  if (!key.value())
    return true;
  auto stored = cache.get(*key.value());
  return stored && stored.value() &&
         decode_analysis(analysis_kind(request.settings.mode), *stored.value()).has_value();
}

Result<std::unique_ptr<warp_dsp::WarpStream>> create_warp_stream(const WarpRenderRequest& request,
                                                                 const WarpSourceAnalysis& analysis,
                                                                 const warp_dsp::WarpTimeMap& map,
                                                                 warp_dsp::QualityTier tier,
                                                                 std::uint32_t max_render_frames) {
  using Created = Result<std::unique_ptr<warp_dsp::WarpStream>>;
  const auto& applied = request.settings;
  const warp_dsp::StreamLimits limits{request.project_rate, max_render_frames};
  switch (applied.mode) {
  case project::WarpMode::repitch:
    return warp_dsp::create_repitch_stream(limits, tier);
  case project::WarpMode::beats:
    return warp_dsp::create_beats_stream(
        limits, tier,
        {analysis.transients, applied.beats_loop, applied.beats_envelope, applied.transpose_cents});
  case project::WarpMode::complex:
    return warp_dsp::create_complex_stream(limits, tier,
                                           {analysis.transients, applied.transpose_cents});
  case project::WarpMode::complex_pro:
    return warp_dsp::create_complex_pro_stream(limits, tier,
                                               {analysis.transients, applied.transpose_cents,
                                                applied.formants, applied.complex_pro_envelope});
  case project::WarpMode::tones: {
    // The map-specific seek index makes every seek exact and bounded; the
    // stream only starts on the map it was prepared for.
    warp_dsp::TonesSettings settings{analysis.pitch_marks, applied.tones_grain_ms,
                                     applied.transpose_cents};
    auto index = warp_dsp::prepare_tones_seek_index(map, limits, tier, settings);
    if (!index)
      return Created::failure(index.error());
    return warp_dsp::create_tones_stream(limits, tier, std::move(settings),
                                         std::move(index).value());
  }
  case project::WarpMode::texture:
    return warp_dsp::create_texture_stream(limits, tier,
                                           {applied.texture_grain_ms, applied.texture_flux,
                                            applied.transpose_cents, applied.texture_seed});
  }
  return Created::failure({ErrorCode::unsupported, "Unknown warp render mode"});
}

Result<std::unique_ptr<ProjectRateSourceView>>
open_request_view(const std::filesystem::path& project_root, const WarpRenderRequest& request,
                  std::stop_token stop) {
  using Opened = Result<std::unique_ptr<ProjectRateSourceView>>;
  if (!request.clay)
    return ProjectRateSourceView::open(project_root, request.source, request.project_rate,
                                       std::move(stop));
  auto store = persistence::ProjectStore::open_reader(project_root);
  if (!store)
    return Opened::failure(store.error());
  auto rendered =
      clay::open_rendered(store.value(), request.clay->original, request.clay->take, stop);
  if (!rendered)
    return Opened::failure(rendered.error());
  if (rendered.value().record() != request.source)
    return Opened::failure(
        {ErrorCode::conflict, "The Clay render does not match the warp request"});
  return ProjectRateSourceView::from_descriptor(rendered.value().fd(), rendered.value().record(),
                                                request.project_rate);
}

Result<std::optional<WarpRenderRequest>> processed_clip_request(const project::Snapshot& snapshot,
                                                                project::ClipId clip_id,
                                                                warp_dsp::QualityTier quality) {
  using Resolved = Result<std::optional<WarpRenderRequest>>;
  for (const auto& track : snapshot.tracks) {
    const auto clip = std::ranges::find_if(
        track.clips, [&](const project::Clip& item) { return item.id == clip_id; });
    if (clip == track.clips.end())
      continue;
    const auto source = std::ranges::find_if(
        snapshot.sources, [&](const auto& item) { return item.id == clip->source; });
    if (source == snapshot.sources.end())
      return Resolved::failure({ErrorCode::not_found, "Warp clip source record is missing"});
    auto plan = prepare_clip_frame_plan(*clip, *source, snapshot.tempo, snapshot.sample_rate);
    if (!plan)
      return Resolved::failure(plan.error());
    auto first = warp_dsp::project_rate_frames(plan.value().source_start_frame, source->sample_rate,
                                               snapshot.sample_rate);
    auto end = warp_dsp::project_rate_frames(plan.value().source_start_frame +
                                                 plan.value().warp.source_frames(),
                                             source->sample_rate, snapshot.sample_rate);
    if (!first || !end)
      return Resolved::failure(!first ? first.error() : end.error());
    if (!clip->processing.requires_rendering() &&
        end.value() - first.value() == plan.value().warp.output_frames())
      return Resolved::success(std::nullopt);
    if (source->external_link)
      return Resolved::failure(
          {ErrorCode::unsupported,
           "Warp processing needs a managed source; collect the linked file into the project"});
    WarpRenderRequest request{*source,
                              clip_id,
                              clip->settings_generation,
                              std::move(plan).value(),
                              snapshot.sample_rate,
                              applied_warp_settings(clip->processing),
                              quality};
    if (clip->clay) {
      auto rendered = clay::rendered_record(*source, *clip->clay);
      if (!rendered)
        return Resolved::failure(rendered.error());
      request.source = std::move(rendered).value();
      request.clay = ClayInput{*source, *clip->clay};
    }
    if (auto valid = validate_request(request); !valid)
      return Resolved::failure(valid.error());
    return Resolved::success(std::move(request));
  }
  return Resolved::failure({ErrorCode::not_found, "Warp clip is no longer in the project"});
}

Result<WarpRenderRequest> warp_render_request(const project::Snapshot& snapshot,
                                              project::ClipId clip_id,
                                              warp_dsp::QualityTier quality) {
  auto resolved = processed_clip_request(snapshot, clip_id, quality);
  if (!resolved)
    return Result<WarpRenderRequest>::failure(resolved.error());
  if (!resolved.value())
    return Result<WarpRenderRequest>::failure(
        {ErrorCode::unsupported, "Clip plays without warp processing"});
  return Result<WarpRenderRequest>::success(std::move(*resolved.value()));
}

Result<media::CacheKey> warp_render_cache_key(const WarpRenderRequest& request) {
  if (auto valid = validate_request(request); !valid)
    return Result<media::CacheKey>::failure(valid.error());
  auto converted =
      make_warp_stream_map(request.plan, request.source.sample_rate, request.project_rate);
  if (!converted)
    return Result<media::CacheKey>::failure(converted.error());
  const auto segments = converted.value().segments();
  const auto& applied = request.settings;
  std::vector<std::byte> canonical;
  canonical.reserve(160 + segments.size() * 40);
  // What is heard, not the clip's edit count: a clip moved along the
  // timeline (or a copy of it) plays the same render.
  append_text(canonical, warp_dsp::rate_view_algorithm);
  append_u32(canonical, warp_dsp::rate_view_algorithm_version);
  append_u64(canonical, request.plan.source_start_frame);
  append_u64(canonical, request.plan.warp.source_frames());
  append_u64(canonical, request.plan.warp.output_frames());
  append_u64(canonical, segments.size());
  for (const auto& segment : segments) {
    append_u64(canonical, segment.output_begin);
    append_u64(canonical, segment.output_end);
    append_u64(canonical, segment.source_begin);
    append_u64(canonical, segment.source_end);
    append_u64(canonical, segment.splice ? 1U : 0U);
  }
  // Applied parameters of the active mode only, after intent rules.
  canonical.push_back(static_cast<std::byte>(mode_byte(applied.mode)));
  append_u32(canonical, static_cast<std::uint32_t>(applied.transpose_cents));
  switch (applied.mode) {
  case project::WarpMode::beats:
    canonical.push_back(static_cast<std::byte>(applied.beats_loop));
    append_f32(canonical, applied.beats_envelope);
    break;
  case project::WarpMode::tones:
    append_f32(canonical, applied.tones_grain_ms);
    break;
  case project::WarpMode::texture:
    append_f32(canonical, applied.texture_grain_ms);
    append_f32(canonical, applied.texture_flux);
    append_u64(canonical, applied.texture_seed);
    break;
  case project::WarpMode::complex_pro:
    append_f32(canonical, applied.formants);
    append_f32(canonical, applied.complex_pro_envelope);
    break;
  case project::WarpMode::repitch:
  case project::WarpMode::complex:
    break;
  }
  // Absolute project placement is absent. Reuse after a move is possible only
  // when the caller retains the same settings generation; a new generation
  // deliberately creates a new key even if rendered samples are identical.
  return media::make_cache_key(
      {media::CacheArtifact::clip_processed, request.source, request.clip, request.project_rate,
       std::string(warp_algorithm_id(applied.mode)), warp_algorithm_version(applied.mode),
       static_cast<std::uint32_t>(request.quality) + 1U, std::move(canonical), std::nullopt});
}

Result<media::CacheKey> render_warp_to_cache(media::CacheStore& cache,
                                             const std::filesystem::path& project_root,
                                             const WarpRenderRequest& request, std::stop_token stop,
                                             const media::JobTicket* ticket,
                                             std::atomic<double>* progress) {
  auto key = warp_render_cache_key(request);
  if (!key)
    return key;
  if (request.plan.warp.output_frames() > max_cached_frames)
    return Result<media::CacheKey>::failure(
        {ErrorCode::unsupported, "A clip this long plays from its warp stream, uncached"});
  if (ticket && ticket->key() != key.value())
    return Result<media::CacheKey>::failure({ErrorCode::conflict, "Warp cache ticket key changed"});
  if (auto live = cancelled(stop, ticket); !live)
    return Result<media::CacheKey>::failure(live.error());
  auto view = open_request_view(project_root, request, stop);
  if (!view)
    return Result<media::CacheKey>::failure(view.error());
  auto map = make_warp_stream_map(request.plan, request.source.sample_rate, request.project_rate);
  if (!map)
    return Result<media::CacheKey>::failure(map.error());
  const StopFlag stop_flag(stop);
  auto analysis = cached_warp_analysis(&cache, request, *view.value(), &stop_flag.flag());
  if (!analysis) {
    if (view.value()->status() != SourceReadStatus::ok)
      return Result<media::CacheKey>::failure(
          {view.value()->error_code(), "Warp source read failed during analysis"});
    return Result<media::CacheKey>::failure(analysis.error());
  }
  if (progress)
    progress->store(0.05, std::memory_order_relaxed);
  auto stream = create_warp_stream(request, analysis.value(), map.value(), request.quality,
                                   static_cast<std::uint32_t>(chunk_frames));
  if (!stream)
    return Result<media::CacheKey>::failure(stream.error());
  if (auto live = cancelled(stop, ticket); !live)
    return Result<media::CacheKey>::failure(live.error());
  const auto started = stream.value()->start(map.value(), *view.value(), 0);
  if (started == warp_dsp::StreamStatus::unsupported_map)
    return Result<media::CacheKey>::failure(
        {ErrorCode::unsupported, std::string("Warp markers exceed the speed range of ") +
                                     std::string(warp_algorithm_id(request.settings.mode))});
  if (started != warp_dsp::StreamStatus::ok) {
    if (view.value()->status() != SourceReadStatus::ok)
      return Result<media::CacheKey>::failure(
          {view.value()->error_code(), "Warp source read failed at render start"});
    return Result<media::CacheKey>::failure({ErrorCode::internal, "Warp stream did not start"});
  }

  const auto frames = request.plan.warp.output_frames();
  const auto digest = key.value().hex();
  const auto bytes = header_bytes + frames * 8U;
  auto stored = cache.put_stream(
      key.value(), bytes, [&](const media::CacheChunkAppend& append) -> Result<void> {
        std::array<std::byte, header_bytes> header{};
        std::ranges::copy(magic, header.begin());
        store_u32(std::span(header).subspan(4, 4), format_version);
        store_u64(std::span(header).subspan(8, 8), frames);
        header[16] = static_cast<std::byte>(mode_byte(request.settings.mode));
        header[17] = static_cast<std::byte>(request.quality);
        store_u16(std::span(header).subspan(18, 2), channels);
        for (std::size_t index = 0; index < digest_bytes; ++index)
          header[digest_offset + index] = static_cast<std::byte>(digest[index]);
        if (auto result = append(header); !result)
          return result;
        std::vector<float> left(chunk_frames);
        std::vector<float> right(chunk_frames);
        std::vector<std::byte> encoded(chunk_frames * 8);
        std::uint64_t produced = 0;
        while (produced < frames) {
          if (auto live = cancelled(stop, ticket); !live)
            return live;
          const auto count =
              static_cast<std::size_t>(std::min<std::uint64_t>(chunk_frames, frames - produced));
          const auto actual =
              stream.value()->render(std::span(left).first(count), std::span(right).first(count));
          if (actual != count || stream.value()->status() != warp_dsp::StreamStatus::ok) {
            if (view.value()->status() != SourceReadStatus::ok)
              return Result<void>::failure(
                  {view.value()->error_code(), "Warp source read failed during render"});
            return Result<void>::failure({ErrorCode::io_failure, "Warp stream stopped early"});
          }
          for (std::size_t index = 0; index < count; ++index) {
            if (!std::isfinite(left[index]) || !std::isfinite(right[index]))
              return Result<void>::failure({ErrorCode::corrupt_data, "Nonfinite warped sample"});
            store_f32(std::span(encoded).subspan(index * 8, 4), left[index]);
            store_f32(std::span(encoded).subspan(index * 8 + 4, 4), right[index]);
          }
          if (auto live = cancelled(stop, ticket); !live)
            return live;
          if (auto result = append(std::span(encoded).first(count * 8)); !result)
            return result;
          produced += count;
          if (progress)
            progress->store(0.05 +
                                0.95 * static_cast<double>(produced) / static_cast<double>(frames),
                            std::memory_order_relaxed);
        }
        return cancelled(stop, ticket);
      });
  if (!stored)
    return Result<media::CacheKey>::failure(stored.error());
  if (progress)
    progress->store(1.0, std::memory_order_relaxed);
  return key;
}

Result<std::optional<WarpRenderCacheReader>>
WarpRenderCacheReader::open(media::CacheStore& cache, const media::CacheKey& key,
                            WarpPayloadShape expected) {
  using Return = Result<std::optional<WarpRenderCacheReader>>;
  if (expected.frames == 0 || !known_mode(expected.mode) ||
      !warp_dsp::valid_quality_tier(expected.quality))
    return Return::failure({ErrorCode::invalid_argument, "Invalid warp cache payload shape"});
  // A render longer than any cache file is never cached: a miss.
  if (expected.frames > max_cached_frames)
    return Return::success(std::nullopt);
  auto opened = cache.open_validated(key);
  if (!opened)
    return Return::failure(opened.error());
  auto maybe_reader = std::move(opened).value();
  if (!maybe_reader)
    return Return::success(std::nullopt);
  auto reader = std::move(maybe_reader).value();
  if (reader.size_bytes() != header_bytes + expected.frames * 8U)
    return Return::success(std::nullopt);
  std::array<std::byte, header_bytes> header{};
  auto result = reader.read_at(0, header);
  if (!result || result.value() != header.size())
    return Return::success(std::nullopt);
  const auto digest = key.hex();
  const bool reserved_zero =
      load_u32(std::span(header).subspan(20, 4)) == 0 &&
      load_u64(std::span(header).subspan(digest_offset + digest_bytes, 8)) == 0;
  bool digest_matches = digest.size() == digest_bytes;
  for (std::size_t index = 0; digest_matches && index < digest_bytes; ++index)
    digest_matches = header[digest_offset + index] == static_cast<std::byte>(digest[index]);
  if (!std::ranges::equal(std::span(header).first(4), magic) ||
      load_u32(std::span(header).subspan(4, 4)) != format_version ||
      load_u64(std::span(header).subspan(8, 8)) != expected.frames ||
      header[16] != static_cast<std::byte>(mode_byte(expected.mode)) ||
      header[17] != static_cast<std::byte>(expected.quality) ||
      load_u16(std::span(header).subspan(18, 2)) != channels || !reserved_zero || !digest_matches)
    return Return::success(std::nullopt);
  std::vector<std::byte> block(chunk_frames * 8);
  for (std::uint64_t frame = 0; frame < expected.frames;) {
    const auto count =
        static_cast<std::size_t>(std::min<std::uint64_t>(chunk_frames, expected.frames - frame));
    auto loaded = reader.read_at(header_bytes + frame * 8, std::span(block).first(count * 8));
    if (!loaded || loaded.value() != count * 8)
      return Return::success(std::nullopt);
    for (std::size_t index = 0; index < count; ++index)
      if (!std::isfinite(load_f32(std::span(block).subspan(index * 8, 4))) ||
          !std::isfinite(load_f32(std::span(block).subspan(index * 8 + 4, 4))))
        return Return::success(std::nullopt);
    frame += count;
  }
  return Return::success(std::optional<WarpRenderCacheReader>(
      WarpRenderCacheReader(std::move(reader), expected.frames)));
}

Result<std::size_t> WarpRenderCacheReader::read_at(std::uint64_t first_frame,
                                                   std::span<media::StereoFrame> output) const {
  if (output.size() > chunk_frames)
    return Result<std::size_t>::failure({ErrorCode::invalid_argument, "Warp read chunk too large"});
  if (first_frame >= frames_ || output.empty())
    return Result<std::size_t>::success(0);
  const auto count =
      static_cast<std::size_t>(std::min<std::uint64_t>(output.size(), frames_ - first_frame));
  std::array<std::byte, chunk_frames * 8> encoded{};
  auto loaded =
      reader_.read_at(header_bytes + first_frame * 8, std::span(encoded).first(count * 8));
  if (!loaded || loaded.value() != count * 8)
    return Result<std::size_t>::failure(
        loaded ? foundation::Error{ErrorCode::io_failure, "Warp cache short read"}
               : loaded.error());
  for (std::size_t index = 0; index < count; ++index) {
    output[index] = {load_f32(std::span(encoded).subspan(index * 8, 4)),
                     load_f32(std::span(encoded).subspan(index * 8 + 4, 4))};
  }
  return Result<std::size_t>::success(count);
}

Result<std::size_t> fill_from_warp_render_cache(const WarpRenderCacheReader& source,
                                                media::PrefetchRing& ring,
                                                media::PrefetchCursor cursor,
                                                std::span<media::StereoFrame> scratch) {
  if (cursor.generation == 0)
    return Result<std::size_t>::failure(
        {ErrorCode::invalid_argument, "Invalid warp prefetch generation"});
  const auto budget = std::min({scratch.size(), ring.free_slots(), chunk_frames});
  if (cursor.source_frame > std::numeric_limits<std::uint64_t>::max() - budget)
    return Result<std::size_t>::failure(
        {ErrorCode::invalid_argument, "Warp prefetch frame range overflows"});
  auto decoded = source.read_at(cursor.source_frame, scratch.first(budget));
  if (!decoded)
    return decoded;
  for (std::size_t index = 0; index < decoded.value(); ++index) {
    if (!ring.try_push(
            {ring.clip(), cursor.generation, cursor.source_frame + index, scratch[index]}))
      return Result<std::size_t>::failure(
          {ErrorCode::conflict, "Warp prefetch ring rejected frame position"});
  }
  return decoded;
}

} // namespace oscillate::processing
