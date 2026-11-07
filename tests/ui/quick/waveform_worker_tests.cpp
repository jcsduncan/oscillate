#include "../../../apps/daw_quick/src/clip_waveform_projection.hpp"
#include "../../../apps/daw_quick/src/waveform_controller.hpp"
#include "../../../apps/daw_quick/src/waveform_detail.hpp"
#include "../../../apps/daw_quick/src/waveform_view.hpp"
#include "../../../apps/daw_quick/src/waveform_worker.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/wait.hpp"

#include <QCoreApplication>
#include <QVariantMap>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace oscillate;

using oscillate::test::require;

project::SourceRecord source() {
  auto id = project::SourceId::from_bits({1, 1});
  require(id.has_value(), "fixture source ID invalid");
  return {.id = id.value(),
          .encoding = project::SourceEncoding::wav,
          .sha256 = std::string(64, 'a'),
          .byte_length = 8,
          .sample_rate = 48'000,
          .channels = 2,
          .frames = 8};
}

analysis::WaveformSummary summary(std::uint64_t generation) {
  analysis::WaveformSummary value;
  value.identity = {.source_key = std::string(64, 'a'), .generation = generation};
  value.sample_rate = 48'000;
  value.channels = 2;
  value.frames = 8;
  analysis::WaveformLevel level;
  level.frames_per_bucket = 2;
  for (std::uint64_t index = 0; index < 4; ++index) {
    const auto amplitude = static_cast<float>(index + 1) / 4.0F;
    analysis::WaveformBucket bucket;
    bucket.begin_frame = index * 2;
    bucket.end_frame = bucket.begin_frame + 2;
    bucket.channels[0] = {.minimum = -amplitude, .maximum = amplitude, .rms = amplitude};
    bucket.channels[1] = bucket.channels[0];
    level.buckets.push_back(bucket);
  }
  value.levels.push_back(std::move(level));
  return value;
}

void source_range_projection() {
  auto analyzed = summary(1);
  analysis::WaveformLevel coarse;
  coarse.frames_per_bucket = 8;
  analysis::WaveformBucket whole;
  whole.begin_frame = 0;
  whole.end_frame = 8;
  whole.channels[0] = {.minimum = -1.0F, .maximum = 1.0F, .rms = 0.75F};
  whole.channels[1] = whole.channels[0];
  coarse.buckets.push_back(whole);
  analyzed.levels.push_back(std::move(coarse));
  const auto full = quick::waveform_view(analyzed, {{0}, {8}}, 512);
  const auto trimmed = quick::waveform_view(analyzed, {{4}, {8}}, 512);
  require(full.size() == 512 && trimmed.size() == 512, "column budget changed");
  require(full[64].toMap().value("max").toDouble() == 0.25 &&
              full[448].toMap().value("max").toDouble() == 1.0,
          "full source envelope incorrect");
  require(trimmed[64].toMap().value("max").toDouble() == 0.75 &&
              trimmed[448].toMap().value("max").toDouble() == 1.0,
          "trim must project source frames, not original clip columns");
  const auto zoomed = quick::waveform_view(analyzed, {{2}, {4}}, 512);
  require(zoomed[64].toMap().value("max").toDouble() == 0.5 &&
              zoomed[448].toMap().value("max").toDouble() == 0.5,
          "zoomed source interval did not retain detail");
  const auto overview = quick::waveform_view(analyzed, {{0}, {8}}, 1);
  const auto close_up = quick::waveform_view(analyzed, {{4}, {8}}, 4);
  require(overview.size() == 1 && overview.front().toMap().value("max").toDouble() == 1.0 &&
              close_up.size() == 4 && close_up.front().toMap().value("max").toDouble() == 0.75 &&
              close_up.back().toMap().value("max").toDouble() == 1.0,
          "zoom did not select the finer source envelope");
  require(quick::waveform_view(analyzed, {{-1}, {8}}, 512).isEmpty() &&
              quick::waveform_view(analyzed, {{0}, {9}}, 512).isEmpty() &&
              quick::waveform_view(analyzed, {{0}, {8}}, 0).isEmpty() &&
              quick::waveform_view(analyzed, {{0}, {8}}, 4097).isEmpty(),
          "invalid range must not paint stale data");
}

void closing_during_blocked_analysis() {
  std::binary_semaphore entered{0};
  std::binary_semaphore release{0};
  std::binary_semaphore exited{0};
  std::atomic<bool> cancelled{false};
  auto worker = std::make_unique<quick::WaveformWorker>(
      [&](const auto&, const auto&, std::uint64_t generation, const auto& stop) {
        entered.release();
        release.acquire();
        cancelled.store(stop(), std::memory_order_release);
        exited.release();
        return foundation::Result<analysis::WaveformSummary>::success(summary(generation));
      });
  worker->submit({}, {source()}, 1);
  entered.acquire();
  worker.reset(); // Must return while the simulated reader remains blocked.
  release.release();
  exited.acquire();
  require(cancelled.load(std::memory_order_acquire), "late result was not cancelled");
}

void latest_generation_wins() {
  std::binary_semaphore first_entered{0};
  std::binary_semaphore release_first{0};
  quick::WaveformWorker worker(
      [&](const auto&, const auto&, std::uint64_t generation, const auto&) {
        if (generation == 1) {
          first_entered.release();
          release_first.acquire();
        }
        return foundation::Result<analysis::WaveformSummary>::success(summary(generation));
      });
  worker.submit({}, {source()}, 1);
  first_entered.acquire();
  worker.submit({}, {source()}, 2);
  release_first.release();
  std::vector<quick::WaveformWorker::Outcome> completed;
  oscillate::test::wait_until(
      [&] {
        completed = worker.collect();
        return !completed.empty();
      },
      "the waveform worker delivered an outcome");
  require(completed.size() == 1 && completed[0].generation == 2 && completed[0].result.has_value(),
          "superseded analysis installed or latest analysis was lost");
}

} // namespace

// --- Arrangement waveform of processed clips --------------------------------

void columns_follow_their_source_intervals() {
  const auto analyzed = summary(1);
  const std::array<std::uint64_t, 5> edges{0, 2, 4, 6, 8};
  const auto columns = quick::waveform_columns(analyzed, edges);
  require(columns.size() == 4, "one envelope per column");
  for (qsizetype index = 0; index < 4; ++index)
    require(std::abs(columns[index].toMap().value("max").toDouble() -
                     static_cast<double>(index + 1) / 4.0) < 1e-6,
            "a column did not show its own source interval");
  // A column narrower than a source frame (an extreme stretch) shows the
  // frame it plays; a wide one spans every bucket it covers.
  const std::array<std::uint64_t, 3> stretched{5, 5, 8};
  const auto narrow = quick::waveform_columns(analyzed, stretched);
  require(narrow.size() == 2 && std::abs(narrow[0].toMap().value("max").toDouble() - 0.75) < 1e-6 &&
              std::abs(narrow[1].toMap().value("max").toDouble() - 1.0) < 1e-6,
          "a zero-width column did not show the frame it plays");
  const std::array<std::uint64_t, 2> backwards{6, 2};
  const std::array<std::uint64_t, 2> outside{0, 9};
  require(quick::waveform_columns(analyzed, backwards).isEmpty() &&
              quick::waveform_columns(analyzed, outside).isEmpty(),
          "invalid column edges were drawn");
}

// One second at 48 kHz in 4/4 at 120 BPM: half a bar. A marker pins source
// 12,000 to output 24,000, so the first half plays at half speed.
project::Snapshot warped_snapshot() {
  auto tempo = timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1);
  auto meter = timeline::MeterMap::create({{{0}, 4, 4}}, 1);
  require(tempo && meter, "fixture timing maps invalid");
  const auto source = project::SourceRecord{.id = project::SourceId::from_bits({1, 1}).value(),
                                            .encoding = project::SourceEncoding::wav,
                                            .sha256 = std::string(64, 'a'),
                                            .byte_length = 96'044,
                                            .sample_rate = 48'000,
                                            .channels = 1,
                                            .frames = 48'000};
  project::Clip clip{project::ClipId::from_bits({1, 2}).value(), source.id,
                     project::SourceRange{{0}, {48'000}}, project::ElapsedInterval{{0}, {48'000}}};
  clip.processing.reviewed_warp_markers.push_back({{12'000}, timeline::SamplePosition{24'000}});
  project::Snapshot snapshot{project::ProjectId::from_bits({1, 3}).value(),
                             1,
                             48'000,
                             std::move(tempo).value(),
                             std::move(meter).value(),
                             {{project::TrackId::from_bits({1, 4}).value(), {clip}}}};
  snapshot.sources.push_back(source);
  return snapshot;
}

void warped_clip_columns_follow_the_warp_map_at_any_zoom() {
  const auto snapshot = warped_snapshot();
  const auto& clip = snapshot.tracks.front().clips.front();
  const auto edges = quick::clip_waveform_edges(snapshot, clip, 0.0, 0.5, 4);
  // Bars 0, 1/8, ..., 1/2 are project samples 0, 12,000, ... 48,000.
  const std::vector<std::uint64_t> expected{0, 6'000, 12'000, 30'000, 48'000};
  require(edges && *edges == expected, "column edges do not follow the warp map");
  // Zoomed in across the marker, every edge is still the map at its sample.
  const auto zoomed = quick::clip_waveform_edges(snapshot, clip, 0.1875, 0.3125, 4);
  const std::vector<std::uint64_t> close{9'000, 10'500, 12'000, 16'500, 21'000};
  require(zoomed && *zoomed == close, "zoomed column edges do not follow the warp map");
  // Columns are bounded: 4,096 at most, and none for an empty span.
  require(quick::clip_waveform_edges(snapshot, clip, 0.0, 0.5, 4'096) &&
              !quick::clip_waveform_edges(snapshot, clip, 0.0, 0.5, 4'097) &&
              !quick::clip_waveform_edges(snapshot, clip, 0.25, 0.25, 4) &&
              !quick::clip_waveform_edges(snapshot, clip, 0.0, 0.5, 0),
          "column count bounds are not enforced");
  // The unwarped clip maps linearly, like the raw source-range view.
  auto plain = snapshot;
  plain.tracks.front().clips.front().processing = {};
  const auto linear =
      quick::clip_waveform_edges(plain, plain.tracks.front().clips.front(), 0.0, 0.5, 4);
  const std::vector<std::uint64_t> straight{0, 12'000, 24'000, 36'000, 48'000};
  require(linear && *linear == straight, "an unwarped clip did not map linearly");
}

void clip_preview_status_projection() {
  auto snapshot = warped_snapshot();
  const auto& clip = snapshot.tracks.front().clips.front();
  quick::WaveformController controller({});
  const quick::WaveformController::ClipView visible{
      .start_bar = 0.0, .end_bar = 4.0, .processed = false, .audio_clip_ordinal = 1};
  const auto source_limited = controller.project_clip_waveform(snapshot, clip, visible);
  require(source_limited.value("sourceTotalFrames").toDouble() == 48'000.0 &&
              source_limited.value("waveformState").toString() == "unavailable" &&
              source_limited.value("waveformMessage").toString() ==
                  "Waveform preview limited to 64 sources",
          "a valid source outside the preview budget must retain its frame count and status");
  snapshot.sources.clear();
  const auto missing = controller.project_clip_waveform(snapshot, clip, visible);
  require(missing.value("sourceTotalFrames").toDouble() == 0.0 &&
              missing.value("waveformMessage").toString() == "Missing source record",
          "missing source status must remain distinguishable from preview limits");
  const auto clip_limited = controller.project_clip_waveform(
      snapshot, clip,
      {.start_bar = 0.0,
       .end_bar = 4.0,
       .processed = false,
       .audio_clip_ordinal = quick::WaveformController::max_clips + 1});
  require(clip_limited.value("waveformMessage").toString() ==
              "Waveform preview limited to 256 clips",
          "clip budget must take precedence over source status");
}

void close_up_columns_show_every_frame() {
  // A kick-like click: silence, one frame at 0.9, a frame at -0.5, silence.
  quick::WaveformDetail detail{.first_frame = 1'000, .frames = std::vector<media::StereoFrame>(64)};
  detail.frames[10] = {0.9F, 0.8F};
  detail.frames[11] = {-0.2F, -0.5F};
  // One column per frame: each shows its own frame, both channels.
  std::vector<std::uint64_t> edges;
  for (std::uint64_t frame = 1'008; frame <= 1'014; ++frame)
    edges.push_back(frame);
  const auto columns = quick::detail_columns(detail, edges);
  require(columns.size() == 6 && columns[2].toMap().value("max").toFloat() == 0.9F &&
              columns[2].toMap().value("min").toFloat() == 0.8F &&
              columns[3].toMap().value("min").toFloat() == -0.5F &&
              columns[0].toMap().value("max").toFloat() == 0.0F,
          "close-up columns do not show their own frames");
  // Wider columns take the extremes of every frame they cover.
  const std::vector<std::uint64_t> wide{1'000, 1'032, 1'064};
  const auto summary = quick::detail_columns(detail, wide);
  require(summary.size() == 2 && summary[0].toMap().value("max").toFloat() == 0.9F &&
              summary[0].toMap().value("min").toFloat() == -0.5F &&
              summary[1].toMap().value("max").toFloat() == 0.0F,
          "close-up columns lost a peak");
  // Columns narrower than a frame show the frame they play.
  const std::vector<std::uint64_t> narrow{1'010, 1'010, 1'011};
  const auto stretched = quick::detail_columns(detail, narrow);
  require(stretched.size() == 2 && stretched[0].toMap().value("max").toFloat() == 0.9F &&
              stretched[1].toMap().value("max").toFloat() == 0.9F,
          "a sub-frame column did not show its frame");
  // Edges outside the frames read none.
  const std::vector<std::uint64_t> outside{990, 1'010};
  require(quick::detail_columns(detail, outside).isEmpty() &&
              quick::detail_columns(detail, std::vector<std::uint64_t>{1'060, 1'070}).isEmpty(),
          "close-up columns read outside their frames");
}

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  try {
    source_range_projection();
    closing_during_blocked_analysis();
    latest_generation_wins();
    columns_follow_their_source_intervals();
    warped_clip_columns_follow_the_warp_map_at_any_zoom();
    clip_preview_status_projection();
    close_up_columns_show_every_frame();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  } catch (...) {
    std::cerr << "Unexpected waveform contract failure\n";
    return 1;
  }
}
