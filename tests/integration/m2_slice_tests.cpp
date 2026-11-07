#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"
#include "oscillate/test/wav.hpp"

#include <oscillate/application/clip_playback.hpp>
#include <oscillate/application/revision_status.hpp>
#include <oscillate/audio_io/graph_source.hpp>
#include <oscillate/audio_io/stereo_block.hpp>
#include <oscillate/engine/graph.hpp>
#include <oscillate/media/prefetch.hpp>
#include <oscillate/persistence/store.hpp>
#include <oscillate/project/model.hpp>
#include <oscillate/timeline/timing.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {
namespace app = oscillate::application;
namespace audio_io = oscillate::audio_io;
namespace engine = oscillate::engine;
namespace foundation = oscillate::foundation;
namespace media = oscillate::media;
namespace persistence = oscillate::persistence;
namespace project = oscillate::project;
namespace timeline = oscillate::timeline;
namespace fs = std::filesystem;

using oscillate::test::require;

using oscillate::test::take;

template <typename T> T id(std::uint64_t low) { return take(T::from_bits({0, low})); }

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-m2") {}
};

void write_fixture_wav(const fs::path& path) {
  constexpr std::int16_t frames = 8;
  constexpr std::int16_t step = 1'000;
  std::vector<std::int16_t> interleaved;
  for (std::int16_t index = 1; index <= frames; ++index) {
    interleaved.push_back(static_cast<std::int16_t>(index * step));
    interleaved.push_back(static_cast<std::int16_t>(-index * step));
  }
  oscillate::test::write_wav_pcm16(path, oscillate::test::sample_rate_48k,
                                   oscillate::test::stereo_channels, interleaved);
}

class Lease final : public project::StoppedTransportLease {};
class Gate final : public project::TransportGate {
public:
  [[nodiscard]] foundation::Result<std::unique_ptr<project::StoppedTransportLease>>
  acquire_stopped() override {
    return foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::success(
        std::make_unique<Lease>());
  }
};

void near(float actual, float expected, std::string_view message) {
  REQUIRE_LT(std::fabs(actual - expected), 0.0001F, message);
}

struct ExpectedFrame {
  std::size_t output;
  std::int16_t source;
};

void check_frame(std::span<const float> interleaved, ExpectedFrame frame) {
  const float expected = static_cast<float>(frame.source * 1000) / 32768.0F;
  near(interleaved[frame.output * 2], expected, "managed WAV left sample mismatch");
  near(interleaved[frame.output * 2 + 1], -expected, "managed WAV right sample mismatch");
}

std::shared_ptr<media::PrefetchRing> primed_ring(media::WavReader& wav, project::ClipId clip,
                                                 std::uint64_t generation) {
  auto ring = std::make_shared<media::PrefetchRing>(clip, 16);
  std::array<media::StereoFrame, 8> scratch{};
  const auto count = take(media::fill_from_wav(wav, 48000, *ring, {generation, 0}, scratch));
  REQUIRE_EQ(count, 8, "managed WAV did not prefetch all fixture frames");
  return ring;
}

void managed_audio_live_edit_and_recovery() {
  Directory temp;
  const auto external = temp.path() / "external.wav";
  const auto root = temp.path() / "project";
  write_fixture_wav(external);
  auto writer = take(persistence::ProjectStore::open_writer(root, true));
  const int input = ::open(external.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  REQUIRE_GE(input, 0, "cannot open external WAV fixture");
  const auto source_id = id<project::SourceId>(4);
  auto staged = writer.stage_wav(source_id, input);
  ::close(input);
  auto receipt = take(std::move(staged));
  const auto source = receipt.record();
  REQUIRE_EQ(source.frames, 8, "staged metadata mismatch");
  REQUIRE_EQ(source.channels, 2, "staged metadata mismatch");
  REQUIRE_EQ(source.sample_rate, 48000, "staged metadata mismatch");

  Gate gate;
  auto model = take(project::ProjectModel::create(
      id<project::ProjectId>(1), 48000,
      take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1)),
      take(timeline::MeterMap::create({{{0}, 4, 4}}, 1)), gate));
  const auto track_id = id<project::TrackId>(2);
  const auto clip_id = id<project::ClipId>(3);
  project::Clip clip{clip_id, source_id, {{0}, {8}}, project::ElapsedInterval{{4}, {12}}, 1};
  std::vector<project::Operation> initial{project::AddSource{source}, project::AddTrack{track_id},
                                          project::AddClip{track_id, clip}};
  auto initial_edit = take(model.apply(initial));
  REQUIRE_EQ(initial_edit.snapshot->revision, 2, "initial model edit was not atomic");
  std::array<persistence::SourceReceipt, 1> receipts{std::move(receipt)};
  const auto saved = take(writer.save(*model.snapshot(), persistence::SaveKind::explicit_save,
                                      std::span<const persistence::SourceReceipt>(receipts)));
  REQUIRE_EQ(saved.status, persistence::SaveStatus::committed, "initial project save failed");
  fs::remove(external);

  auto reader = take(persistence::ProjectStore::open_reader(root));
  const auto loaded = take(reader.load_current());
  REQUIRE_EQ(loaded.snapshot.revision, 2, "reopened project lost its managed source record");
  REQUIRE_EQ(loaded.snapshot.sources, std::vector<project::SourceRecord>{source},
             "reopened project lost its managed source record");
  REQUIRE_TRUE(loaded.source_diagnostics.empty(),
               "reopened project lost its managed source record");
  const int managed_fd = take(reader.open_managed_source(source));
  auto wav_result = media::WavReader::open_fd(managed_fd);
  ::close(managed_fd);
  auto wav = take(std::move(wav_result));

  auto readiness = std::make_shared<app::PlaybackReadiness>();
  readiness->set_steady(true);
  auto ring = primed_ring(wav, clip_id, 1);
  auto request = take(app::make_clip_request(loaded.snapshot, clip_id, ring, readiness, 3, 4));
  engine::GraphPublisher graph;
  auto first_plan = take(engine::prepare_graph(2, 4, std::span(&request, 1), nullptr));
  REQUIRE_EQ(graph.publish(std::move(first_plan)), engine::PublishStatus::accepted,
             "initial managed-media graph was not published");
  audio_io::TransportCursor transport;
  const auto render_source = audio_io::graph_render_source(graph);
  std::array<engine::StereoFrame, 4> render_scratch{};
  std::array<float, 8> interleaved{};
  REQUIRE_EQ(
      audio_io::render_transport_block(render_source, transport, true, render_scratch, interleaved),
      audio_io::BlockResult::rendered, "pre-clip device block failed");
  REQUIRE_EQ(interleaved, (std::array<float, 8>{}), "pre-clip block was not silent");
  REQUIRE_EQ(
      audio_io::render_transport_block(render_source, transport, true, render_scratch, interleaved),
      audio_io::BlockResult::rendered, "managed source did not reach stereo device seam");
  for (std::size_t frame = 0; frame < 4; ++frame) {
    check_frame(interleaved, {frame, static_cast<std::int16_t>(frame + 1)});
  }

  app::RevisionTracker revisions(2, graph.active_revision());
  std::vector<project::Operation> move_clip{
      project::SetClipPlacement{clip_id, project::ElapsedInterval{{12}, {20}}}};
  const auto moved = take(model.apply(move_clip));
  require(revisions.accept_model_revision(moved.snapshot->revision),
          "accepted project edit did not become pending");
  auto next_ring = primed_ring(wav, clip_id, 2);
  auto next_request =
      take(app::make_clip_request(*moved.snapshot, clip_id, next_ring, readiness, 3, 4));
  auto next_plan =
      take(engine::prepare_graph(3, 4, std::span(&next_request, 1), graph.active_plan()));
  REQUIRE_EQ(graph.publish(std::move(next_plan)), engine::PublishStatus::accepted,
             "live graph edit was not queued");
  require(transport.request_seek(12), "cannot seek to edited clip");
  REQUIRE_EQ(
      audio_io::render_transport_block(render_source, transport, true, render_scratch, interleaved),
      audio_io::BlockResult::rendered, "live graph switch failed");
  REQUIRE_TRUE(revisions.observe_active_graph(graph.active_revision()),
               "new model revision was not marked audible after activation");
  REQUIRE_EQ(revisions.snapshot().application, app::PlaybackApplication::active,
             "new model revision was not marked audible after activation");
  (void)graph.collect_retired();
  REQUIRE_EQ(
      audio_io::render_transport_block(render_source, transport, true, render_scratch, interleaved),
      audio_io::BlockResult::rendered, "edited clip continuation failed");
  for (std::size_t frame = 0; frame < 4; ++frame) {
    check_frame(interleaved, {frame, static_cast<std::int16_t>(frame + 5)});
  }

  const auto failed_edit = take(model.apply({project::AddTrack{id<project::TrackId>(5)}}));
  require(revisions.accept_model_revision(failed_edit.snapshot->revision),
          "failed graph edit did not commit to project");
  auto bad_plan = engine::prepare_graph(4, 0, std::span(&next_request, 1), graph.active_plan());
  require(!bad_plan, "invalid graph unexpectedly prepared");
  require(revisions.fail_revision(4, bad_plan.error()), "graph failure was not recorded");
  REQUIRE_EQ(revisions.snapshot().model_revision, 4,
             "saved and audible revisions became indistinguishable");
  REQUIRE_EQ(revisions.snapshot().active_graph_revision, 3,
             "saved and audible revisions became indistinguishable");
  REQUIRE_EQ(revisions.snapshot().application, app::PlaybackApplication::failed,
             "saved and audible revisions became indistinguishable");
  const auto saved_failed_edit =
      take(writer.save(*model.snapshot(), persistence::SaveKind::explicit_save));
  REQUIRE_EQ(saved_failed_edit.status, persistence::SaveStatus::committed,
             "failed graph edit was not durably saved");
  REQUIRE_EQ(take(reader.load_current()).snapshot.revision, 4,
             "failed graph edit was not durably saved");
  REQUIRE_EQ(graph.active_revision(), 3, "failed graph edit displaced last valid graph");

  writer = take(persistence::ProjectStore::open_reader(root)); // Release writer lock.
  auto interrupted =
      take(persistence::ProjectStore::open_writer(root, false, [](persistence::SaveStage stage) {
        return stage == persistence::SaveStage::manifest_written;
      }));
  const auto later = take(model.apply({project::AddTrack{id<project::TrackId>(6)}}));
  REQUIRE_EQ(later.snapshot->revision, 5, "recovery fixture edit failed");
  require(!interrupted.save(*later.snapshot, persistence::SaveKind::explicit_save),
          "interrupted save unexpectedly committed");
  REQUIRE_EQ(take(reader.load_current()).snapshot.revision, 4,
             "interrupted save damaged previous complete project generation");
}
} // namespace

int main() {
  try {
    managed_audio_live_edit_and_recovery();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
