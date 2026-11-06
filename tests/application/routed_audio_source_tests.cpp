#include "oscillate/application/project_session.hpp"
#include "oscillate/application/routed_audio_source.hpp"
#include "oscillate/audio_io/stereo_block.hpp"
#include "oscillate/media/source_rate_reader.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_inserts/effect_binding.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {
namespace app = oscillate::application;
namespace audio = oscillate::audio_io;
namespace engine = oscillate::engine;
namespace foundation = oscillate::foundation;
namespace persistence = oscillate::persistence;
namespace project = oscillate::project;
namespace timeline = oscillate::timeline;
namespace fs = std::filesystem;

using oscillate::test::require;

using oscillate::test::take;

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-routed-device") {}
};

void put16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value));
  bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}
void put32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
  put16(bytes, static_cast<std::uint16_t>(value));
  put16(bytes, static_cast<std::uint16_t>(value >> 16U));
}

struct WavFixtureOptions {
  std::uint32_t sample_rate = oscillate::test::sample_rate_48k;
  std::optional<std::uint32_t> impulse_at = std::nullopt;
};

fs::path make_wav(const fs::path& root, std::uint32_t frames, WavFixtureOptions options = {}) {
  auto path = root / "source.wav";
  std::vector<std::uint8_t> bytes{'R', 'I', 'F', 'F'};
  put32(bytes, 36 + frames * 2);
  bytes.insert(bytes.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  put32(bytes, 16);
  put16(bytes, 1);
  put16(bytes, 1);
  put32(bytes, options.sample_rate);
  put32(bytes, options.sample_rate * 2);
  put16(bytes, 2);
  put16(bytes, 16);
  bytes.insert(bytes.end(), {'d', 'a', 't', 'a'});
  put32(bytes, frames * 2);
  for (std::uint32_t frame = 0; frame < frames; ++frame)
    put16(bytes, options.impulse_at ? (frame == *options.impulse_at ? 24'576 : 0) : 8'192);
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  require(output.good(), "WAV fixture write failed");
  return path;
}

class Gate final : public project::TransportGate {
public:
  class Lease final : public project::StoppedTransportLease {};
  foundation::Result<std::unique_ptr<project::StoppedTransportLease>> acquire_stopped() override {
    return foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::success(
        std::make_unique<Lease>());
  }
};

engine::RoutedMixerLimits limits(std::size_t max_frames = 64) {
  return {{1, 64},
          max_frames,
          6ULL * max_frames * sizeof(engine::StereoFrame),
          5ULL * 65 * sizeof(engine::StereoFrame)};
}

void near(float actual, float expected) {
  REQUIRE_LT(std::fabs(actual - expected), 0.00001F, "routed fake-device sample was incorrect");
}

void managed_wav_routes_through_one_transport_cursor() {
  Directory directory;
  const auto root = directory.path() / "song";
  const auto wav = make_wav(directory.path(), 32);
  auto session = take(app::ProjectSession::create(root));
  int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "WAV fixture open failed");
  auto first = session->imports().import_wav_audio(fd);
  (void)::close(fd);
  REQUIRE_OK(first, "first managed WAV import failed");
  fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "second WAV fixture open failed");
  auto second = session->imports().import_wav_audio(fd);
  (void)::close(fd);
  REQUIRE_OK(second, "second managed WAV import failed");
  auto group = session->tracks().add_bus_track(project::TrackKind::group_bus);
  auto returns = session->tracks().add_bus_track(project::TrackKind::return_bus);
  REQUIRE_TRUE(group, "managed project bus route setup failed");
  REQUIRE_TRUE(returns, "managed project bus route setup failed");
  REQUIRE_TRUE(session->routing().set_main_output(first.value(), group.value()),
               "managed project bus route setup failed");
  auto send = session->routing().add_send(first.value(), returns.value(), 0.5);
  REQUIRE_TRUE(send, "managed project send save failed");
  REQUIRE_TRUE(session->saving().save(), "managed project send save failed");
  const auto snapshot = session->snapshot();
  session.reset();
  auto writer = take(persistence::ProjectStore::open_writer(root, false));
  // The test opens the saved project in a separate model so edits are exact
  // applied revisions while the managed asset reader remains independent.
  Gate gate;
  auto model = take(project::ProjectModel::open(snapshot, gate));
  // History for the five saved edges plus the one the held edit adds.
  auto held_limits = limits();
  held_limits.max_history_bytes += 65 * sizeof(engine::StereoFrame);
  app::RoutedPlaybackControl control(model, held_limits);
  REQUIRE_OK(control.submit_current(), "saved routing graph did not prepare");
  auto source = take(app::RoutedAudioSource::prepare(root, *model.snapshot(), control, 128));
  audio::TransportCursor transport;
  std::array<engine::StereoFrame, 4> stereo{};
  std::array<float, 8> samples{};
  const auto fake_device = source->render_source();
  counting_allocations = true;
  const auto first_block =
      audio::render_transport_block(fake_device, transport, true, stereo, samples);
  counting_allocations = false;
  REQUIRE_EQ(first_block, audio::BlockResult::rendered,
             "first managed routed callback failed or allocated");
  REQUIRE_EQ(counted_allocations, 0, "first managed routed callback failed or allocated");
  near(samples[0], 0.625F);
  near(samples[1], 0.625F);
  REQUIRE_EQ(transport.completed_snapshot().next_sample, 4,
             "initial routed block advanced wrong transport boundary");
  REQUIRE_EQ(transport.completed_snapshot().generation, 1,
             "initial routed block advanced wrong transport boundary");
  control.observe();
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::active,
             "first routed block did not activate saved revision");

  // A send gain alone glides live through the mixer overlay (ADR-0029). A new,
  // silent send adds a graph edge, so this edit takes the held handoff while
  // the audible result stays the gain change alone.
  const auto silent_send = project::SendId::from_bits({0, 0x5e7d}).value();
  auto changed = take(
      model.apply({project::SetSendGain{send.value(), 1.0},
                   project::AddSendRoute{second.value(),
                                         project::SendRoute{silent_send, returns.value(), 0.0}}}));
  const auto saved_gain =
      take(writer.save(*changed.snapshot, persistence::SaveKind::explicit_save));
  REQUIRE_EQ(saved_gain.status, persistence::SaveStatus::committed,
             "accepted send gain was not durably reopened before graph handoff");
  REQUIRE_EQ(take(writer.load_current()).snapshot.revision, changed.snapshot->revision,
             "accepted send gain was not durably reopened before graph handoff");
  // Held switching, which this test covers, is otherwise reserved for source
  // and VST3 changes; a route edit alone switches live.
  control.set_live_switching(false);
  REQUIRE_OK(control.begin_edit(changed, "send gain and new send"),
             "send gain and new send edit did not begin routed graph handoff");
  counting_allocations = true;
  const auto held = audio::render_transport_block(fake_device, transport, true, stereo, samples);
  counting_allocations = false;
  REQUIRE_EQ(held, audio::BlockResult::rendered,
             "held routed block did not freeze and silence fake device");
  REQUIRE_EQ(counted_allocations, 0, "held routed block did not freeze and silence fake device");
  REQUIRE_EQ(samples[0], 0.0F, "held routed block did not freeze and silence fake device");
  REQUIRE_EQ(transport.completed_snapshot().next_sample, 4,
             "held routed block did not freeze and silence fake device");
  REQUIRE_EQ(control.held_boundary().value_or(engine::BlockContext{}).project_sample_position, 4,
             "held routed block did not freeze and silence fake device");
  require(transport.request_seek(0), "fake-device seek could not queue during hold");
  REQUIRE_EQ(audio::render_transport_block(fake_device, transport, true, stereo, samples),
             audio::BlockResult::rendered, "queued seek escaped while routed graph was held");
  REQUIRE_EQ(transport.completed_snapshot().next_sample, 4,
             "queued seek escaped while routed graph was held");
  REQUIRE_EQ(transport.completed_snapshot().generation, 1,
             "queued seek escaped while routed graph was held");
  REQUIRE_OK(control.publish_held(), "held send-gain graph failed to prepare");
  counting_allocations = true;
  const auto switched =
      audio::render_transport_block(fake_device, transport, true, stereo, samples);
  counting_allocations = false;
  REQUIRE_EQ(switched, audio::BlockResult::rendered, "routed graph switch failed or allocated");
  REQUIRE_EQ(counted_allocations, 0, "routed graph switch failed or allocated");
  near(samples[0], 0.75F);
  REQUIRE_EQ(transport.completed_snapshot().next_sample, 8,
             "routed handoff advanced cursor more than one audible block");
  REQUIRE_EQ(transport.completed_snapshot().generation, 1,
             "routed handoff advanced cursor more than one audible block");
  control.observe();
  REQUIRE_EQ(control.status().active_graph_revision, changed.snapshot->revision,
             "routed gain edit not marked active after callback switch");
  bool seek_resumed = false;
  for (std::size_t attempt = 0; attempt < 100'000 && !seek_resumed; ++attempt) {
    REQUIRE_EQ(audio::render_transport_block(fake_device, transport, true, stereo, samples),
               audio::BlockResult::rendered, "deferred seek rendered a device failure");
    const auto cursor = transport.completed_snapshot();
    seek_resumed = cursor.next_sample == 4 && cursor.generation == 2;
    if (!seek_resumed) {
      REQUIRE_EQ(cursor.next_sample, 0,
                 "deferred seek did not freeze on the new generation while worker primed");
      REQUIRE_EQ(cursor.generation, 2,
                 "deferred seek did not freeze on the new generation while worker primed");
      REQUIRE_EQ(samples[0], 0.0F,
                 "deferred seek did not freeze on the new generation while worker primed");
      std::this_thread::yield();
    }
  }
  REQUIRE_TRUE(seek_resumed, "deferred seek did not resume at the requested generation and sample");
  REQUIRE_EQ(source->status().ready_generation, 2,
             "deferred seek did not resume at the requested generation and sample");
  near(samples[0], 0.75F);

  // A lookahead limiter added to the master changes the output latency.
  auto unsupported = take(model.apply({project::AddNativeEffect{
      project::MasterSignalPath{}, 0,
      take(oscillate::native_inserts::native_effect_patch_for_preset(
          *oscillate::native_dsp::effects::find_engine("ceiling"), std::nullopt))}}));
  const auto saved_failure =
      take(writer.save(*unsupported.snapshot, persistence::SaveKind::explicit_save));
  REQUIRE_EQ(saved_failure.status, persistence::SaveStatus::committed,
             "unsupported accepted edit was not preserved before failed graph build");
  REQUIRE_EQ(take(writer.load_current()).snapshot.revision, unsupported.snapshot->revision,
             "unsupported accepted edit was not preserved before failed graph build");
  REQUIRE_OK(control.begin_edit(unsupported, "master limiter"),
             "unsupported graph edit did not enter a frozen hold");
  REQUIRE_EQ(audio::render_transport_block(fake_device, transport, true, stereo, samples),
             audio::BlockResult::rendered, "unsupported graph edit did not enter a frozen hold");
  REQUIRE_EQ(transport.completed_snapshot().next_sample, 4,
             "unsupported graph edit did not enter a frozen hold");
  auto failed = control.publish_held();
  REQUIRE_TRUE(!failed, "failed routed edit was silently substituted for active graph");
  REQUIRE_EQ(failed.error().code, foundation::ErrorCode::unsupported,
             "failed routed edit was silently substituted for active graph");
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::failed,
             "failed routed edit was silently substituted for active graph");
  REQUIRE_EQ(control.status().active_graph_revision, changed.snapshot->revision,
             "failed routed edit was silently substituted for active graph");
  REQUIRE_EQ(control.status().model_revision, unsupported.snapshot->revision,
             "failed routed edit was silently substituted for active graph");
  REQUIRE_EQ(audio::render_transport_block(fake_device, transport, true, stereo, samples),
             audio::BlockResult::rendered,
             "failed graph did not resume old route at same source sample");
  REQUIRE_EQ(transport.completed_snapshot().next_sample, 8,
             "failed graph did not resume old route at same source sample");
  near(samples[0], 0.75F);
  REQUIRE_OK(control.retry(), "failed graph retry did not remain visibly failed and frozen");
  REQUIRE_EQ(audio::render_transport_block(fake_device, transport, true, stereo, samples),
             audio::BlockResult::rendered,
             "failed graph retry did not remain visibly failed and frozen");
  REQUIRE_EQ(transport.completed_snapshot().next_sample, 8,
             "failed graph retry did not remain visibly failed and frozen");
  REQUIRE_TRUE(!control.publish_held(),
               "failed graph retry did not remain visibly failed and frozen");
  REQUIRE_EQ(audio::render_transport_block(fake_device, transport, true, stereo, samples),
             audio::BlockResult::rendered, "retry failure did not preserve previous audible graph");
  REQUIRE_EQ(transport.completed_snapshot().next_sample, 12,
             "retry failure did not preserve previous audible graph");
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::failed,
             "retry failure did not preserve previous audible graph");
  near(samples[0], 0.75F);

  auto recovered = take(model.undo());
  const auto saved_recovery =
      take(writer.save(*recovered.snapshot, persistence::SaveKind::explicit_save));
  REQUIRE_EQ(saved_recovery.status, persistence::SaveStatus::committed,
             "undo after failed graph build did not durably recover the project");
  REQUIRE_EQ(take(writer.load_current()).snapshot.revision, recovered.snapshot->revision,
             "undo after failed graph build did not durably recover the project");
  REQUIRE_TRUE(take(writer.load_current()).snapshot.master_effects.empty(),
               "undo after failed graph build did not durably recover the project");
  REQUIRE_TRUE(control.begin_edit(recovered, "undo"),
               "saved undo did not publish from the held source sample");
  REQUIRE_EQ(audio::render_transport_block(fake_device, transport, true, stereo, samples),
             audio::BlockResult::rendered,
             "saved undo did not publish from the held source sample");
  REQUIRE_EQ(transport.completed_snapshot().next_sample, 12,
             "saved undo did not publish from the held source sample");
  REQUIRE_TRUE(control.publish_held(), "saved undo did not publish from the held source sample");
  counting_allocations = true;
  const auto resumed = audio::render_transport_block(fake_device, transport, true, stereo, samples);
  counting_allocations = false;
  REQUIRE_EQ(resumed, audio::BlockResult::rendered,
             "saved undo did not resume from the held source sample without allocating");
  REQUIRE_EQ(counted_allocations, 0,
             "saved undo did not resume from the held source sample without allocating");
  REQUIRE_EQ(transport.completed_snapshot().next_sample, 16,
             "saved undo did not resume from the held source sample without allocating");
  REQUIRE_EQ(transport.completed_snapshot().generation, 2,
             "saved undo did not resume from the held source sample without allocating");
  near(samples[0], 0.75F);
  control.observe();
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::active,
             "recovered routed project was not marked active after fake callback");
  REQUIRE_EQ(control.status().active_graph_revision, recovered.snapshot->revision,
             "recovered routed project was not marked active after fake callback");
}

void external_wav_requires_saved_identity_before_routed_reader() {
  Directory directory;
  const auto root = directory.path() / "linked-song";
  const auto wav = make_wav(directory.path(), 32);
  auto session = take(app::ProjectSession::create(root));
  const int input = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(input, 0, "external routed WAV fixture did not open");
  REQUIRE_OK(session->imports().import_wav_audio(input), "external routed fixture import failed");
  ::close(input);
  auto saved = session->snapshot();
  session.reset();
  saved.revision++;
  saved.sources.front().external_link = project::ExternalSourceLink{wav.string()};
  {
    auto writer = take(persistence::ProjectStore::open_writer(root, false));
    REQUIRE_OK(writer.save(saved, persistence::SaveKind::explicit_save),
               "external routed fixture save failed");
  }
  Gate gate;
  auto model = take(project::ProjectModel::open(saved, gate));
  app::RoutedPlaybackControl control(model, limits());
  REQUIRE_OK(control.submit_current(), "external routed graph preparation failed");
  std::stop_source cancelled;
  cancelled.request_stop();
  require(!app::RoutedAudioSource::prepare(root, *model.snapshot(), control, 64, nullptr,
                                           cancelled.get_token()),
          "cancelled external routed preparation opened a reader");
  auto prepared = app::RoutedAudioSource::prepare(root, *model.snapshot(), control, 64);
  REQUIRE_OK(prepared, "verified external WAV could not prepare routed playback");
  audio::TransportCursor cursor;
  std::array<engine::StereoFrame, 4> scratch{};
  std::array<float, 8> output{};
  REQUIRE_EQ(audio::render_transport_block(prepared.value()->render_source(), cursor, true, scratch,
                                           output),
             audio::BlockResult::rendered, "external WAV could not render from pinned source");
  near(output[0], 0.25F);
  prepared.value().reset();
  {
    std::fstream altered(wav, std::ios::in | std::ios::out | std::ios::binary);
    altered.seekp(44);
    altered.put('\x7f');
  }
  auto damaged = app::RoutedAudioSource::prepare(root, *model.snapshot(), control, 64);
  REQUIRE_TRUE(!damaged, "same-size damaged external WAV reached routed decoder");
  REQUIRE_EQ(damaged.error().code, foundation::ErrorCode::corrupt_data,
             "same-size damaged external WAV reached routed decoder");
  fs::remove(wav);
  auto missing = app::RoutedAudioSource::prepare(root, *model.snapshot(), control, 64);
  REQUIRE_TRUE(!missing, "missing external WAV silently prepared routed playback");
  REQUIRE_EQ(missing.error().code, foundation::ErrorCode::not_found,
             "missing external WAV silently prepared routed playback");
}

void bounded_preparation_rejects_large_managed_clip() {
  Directory directory;
  const auto root = directory.path() / "song";
  const auto wav = make_wav(directory.path(), 8);
  auto session = take(app::ProjectSession::create(root));
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "bounded fixture open failed");
  auto imported = session->imports().import_wav_audio(fd);
  (void)::close(fd);
  REQUIRE_OK(imported, "bounded fixture import failed");
  Gate gate;
  auto model = take(project::ProjectModel::open(session->snapshot(), gate));
  app::RoutedPlaybackControl control(model, limits());
  REQUIRE_OK(control.submit_current(), "bounded fixture graph failed");
  auto rejected = app::RoutedAudioSource::prepare(root, *model.snapshot(), control, 4);
  REQUIRE_TRUE(!rejected, "managed PCM budget did not reject oversized source");
  REQUIRE_EQ(rejected.error().code, foundation::ErrorCode::resource_exhausted,
             "managed PCM budget did not reject oversized source");
  REQUIRE_TRUE(!app::RoutedAudioSource::prepare(root, *model.snapshot(), control, 0),
               "invalid preparation inputs or missing project reader were accepted");
  REQUIRE_TRUE(
      !app::RoutedAudioSource::prepare(directory.path() / "absent", *model.snapshot(), control, 8),
      "invalid preparation inputs or missing project reader were accepted");
  auto source = take(app::RoutedAudioSource::prepare(root, *model.snapshot(), control, 64));
  audio::TransportCursor transport;
  std::array<engine::StereoFrame, 4> stereo{};
  std::array<float, 8> samples{};
  const auto fake_device = source->render_source();
  REQUIRE_EQ(audio::render_transport_block(fake_device, transport, true, stereo, samples),
             audio::BlockResult::rendered, "bounded source fixture did not activate");
  control.observe();
  const auto clip_id = model.snapshot()->tracks.front().clips.front().id;
  auto moved =
      take(model.apply({project::SetClipPlacement{clip_id, project::ElapsedInterval{{8}, {16}}}}));
  REQUIRE_OK(control.begin_edit(moved, "move clip"), "clip edit did not enter a safe graph hold");
  REQUIRE_EQ(audio::render_transport_block(fake_device, transport, true, stereo, samples),
             audio::BlockResult::rendered, "clip edit did not enter a safe graph hold");
  REQUIRE_EQ(transport.completed_snapshot().next_sample, 4,
             "clip edit did not enter a safe graph hold");
  auto failed = control.publish_held();
  REQUIRE_TRUE(!failed, "changed clip silently reused stale prepared managed PCM");
  REQUIRE_EQ(failed.error().code, foundation::ErrorCode::unsupported,
             "changed clip silently reused stale prepared managed PCM");
  REQUIRE_EQ(control.status().application, app::PlaybackApplication::failed,
             "changed clip silently reused stale prepared managed PCM");
  REQUIRE_EQ(audio::render_transport_block(fake_device, transport, true, stereo, samples),
             audio::BlockResult::rendered,
             "changed clip silently reused stale prepared managed PCM");
  near(samples[0], 0.25F);
}

// Saved processing is never played as raw source audio: the clip is
// prepared through the processed-clip reader. Detailed processed playback
// gates live in routed_warp_playback_tests.cpp.
void saved_processing_never_falls_back_to_raw_routed_audio() {
  Directory directory;
  const auto root = directory.path() / "song";
  const auto wav = make_wav(directory.path(), 64);
  auto session = take(app::ProjectSession::create(root));
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "processed routed fixture import failed");
  REQUIRE_OK(session->imports().import_wav_audio(fd), "processed routed fixture import failed");
  (void)::close(fd);
  auto state = session->snapshot();
  state.tracks.front().clips.front().processing.reviewed_warp_markers.push_back(
      {{32}, timeline::SamplePosition{16}});
  Gate gate;
  auto model = take(project::ProjectModel::open(state, gate));
  app::RoutedPlaybackControl control(model, limits());
  REQUIRE_OK(control.submit_current(), "processed routed graph did not prepare");
  auto warped = take(app::RoutedAudioSource::prepare(root, *model.snapshot(), control, 128));
  REQUIRE_EQ(warped->status().processed_clips, 1,
             "same-duration reviewed warp was not prepared as a processed clip");
  REQUIRE_EQ(warped->processed_clip_status().size(), 1,
             "same-duration reviewed warp was not prepared as a processed clip");
}

void musical_placement_and_missing_asset() {
  Directory directory;
  const auto root = directory.path() / "song";
  const auto wav = make_wav(directory.path(), 25);
  auto session = take(app::ProjectSession::create(root));
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "musical fixture open failed");
  auto imported = session->imports().import_wav_audio(fd);
  (void)::close(fd);
  REQUIRE_OK(imported, "musical fixture import failed");
  Gate gate;
  auto model = take(project::ProjectModel::open(session->snapshot(), gate));
  const auto clip = model.snapshot()->tracks.front().clips.front().id;
  (void)take(
      model.apply({project::SetClipPlacement{clip, project::MusicalInterval{{0}, {1'000'000}}}}));
  app::RoutedPlaybackControl control(model, limits());
  REQUIRE_OK(control.submit_current(), "musical graph preparation failed");
  auto prepared = app::RoutedAudioSource::prepare(root, *model.snapshot(), control, 64);
  REQUIRE_OK(prepared, "musical placement did not prepare from shared tempo map");
  audio::TransportCursor cursor;
  std::array<engine::StereoFrame, 4> scratch{};
  std::array<float, 8> output{};
  REQUIRE_EQ(audio::render_transport_block(prepared.value()->render_source(), cursor, true, scratch,
                                           output),
             audio::BlockResult::rendered, "musical source did not render");
  near(output[0], 0.25F);
  prepared.value().reset();
  // The managed source must remain present at preparation. Loss is reported,
  // never substituted with an empty audio span.
  const auto assets = root / "assets";
  std::error_code error;
  for (const auto& entry : fs::directory_iterator(assets)) {
    if (entry.is_regular_file())
      fs::remove(entry.path(), error);
  }
  auto missing = app::RoutedAudioSource::prepare(root, *model.snapshot(), control, 64);
  REQUIRE_TRUE(!missing, "missing managed source was silently treated as playable");
  REQUIRE_NE(missing.error().code, foundation::ErrorCode::unsupported,
             "missing managed source was silently treated as playable");
}

void delayed_worker_exposes_underrun_and_preserves_seek_generation() {
  Directory directory;
  const auto root = directory.path() / "song";
  const auto wav = make_wav(directory.path(), 256);
  auto session = take(app::ProjectSession::create(root));
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "delayed-worker WAV open failed");
  REQUIRE_OK(session->imports().import_wav_audio(fd), "delayed-worker import failed");
  (void)::close(fd);
  Gate gate;
  auto model = take(project::ProjectModel::open(session->snapshot(), gate));
  app::RoutedPlaybackControl control(model, limits());
  REQUIRE_OK(control.submit_current(), "delayed-worker graph failed");
  std::atomic<bool> worker_permit{false};
  auto source =
      take(app::RoutedAudioSource::prepare(root, *model.snapshot(), control, 64, &worker_permit));
  audio::TransportCursor transport;
  std::array<engine::StereoFrame, 64> scratch{};
  std::array<float, 128> output{};
  const auto device = source->render_source();
  counting_allocations = true;
  const auto first = audio::render_transport_block(device, transport, true, scratch, output);
  const auto missing = audio::render_transport_block(device, transport, true, scratch, output);
  counting_allocations = false;
  REQUIRE_EQ(first, audio::BlockResult::rendered,
             "paused worker did not report an audible source underrun without allocating");
  REQUIRE_EQ(missing, audio::BlockResult::rendered,
             "paused worker did not report an audible source underrun without allocating");
  REQUIRE_EQ(counted_allocations, 0,
             "paused worker did not report an audible source underrun without allocating");
  REQUIRE_EQ(output[0], 0.0F,
             "paused worker did not report an audible source underrun without allocating");
  REQUIRE_EQ(source->status().missing_frames, 64,
             "paused worker did not report an audible source underrun without allocating");
  REQUIRE_EQ(transport.completed_snapshot().next_sample, 128,
             "paused worker did not report an audible source underrun without allocating");
  const auto reported = source->pop_source_underrun();
  REQUIRE_TRUE(reported,
               "routed source did not report the affected clip and missing source interval");
  REQUIRE_EQ(reported->clip, model.snapshot()->tracks.front().clips.front().id,
             "routed source did not report the affected clip and missing source interval");
  REQUIRE_EQ(reported->generation, 1,
             "routed source did not report the affected clip and missing source interval");
  REQUIRE_EQ(reported->first_source_frame, 64,
             "routed source did not report the affected clip and missing source interval");
  REQUIRE_EQ(reported->missing_frames, 64,
             "routed source did not report the affected clip and missing source interval");
  REQUIRE_TRUE(!source->pop_source_underrun(),
               "routed source did not report the affected clip and missing source interval");
  require(transport.request_seek(0), "delayed-worker seek was rejected");
  for (int attempt = 0; attempt < 3; ++attempt) {
    counting_allocations = true;
    const auto waiting = audio::render_transport_block(device, transport, true, scratch, output);
    counting_allocations = false;
    REQUIRE_EQ(waiting, audio::BlockResult::rendered,
               "seek reused stale-generation PCM or advanced while worker was delayed");
    REQUIRE_EQ(counted_allocations, 0,
               "seek reused stale-generation PCM or advanced while worker was delayed");
    REQUIRE_EQ(output[0], 0.0F,
               "seek reused stale-generation PCM or advanced while worker was delayed");
    REQUIRE_EQ(transport.completed_snapshot().next_sample, 0,
               "seek reused stale-generation PCM or advanced while worker was delayed");
    REQUIRE_EQ(transport.completed_snapshot().generation, 2,
               "seek reused stale-generation PCM or advanced while worker was delayed");
    REQUIRE_EQ(source->status().ready_generation, 1,
               "seek reused stale-generation PCM or advanced while worker was delayed");
  }
  worker_permit.store(true, std::memory_order_release);
  bool resumed = false;
  for (std::size_t attempt = 0; attempt < 100'000 && !resumed; ++attempt) {
    counting_allocations = true;
    const auto block = audio::render_transport_block(device, transport, true, scratch, output);
    counting_allocations = false;
    REQUIRE_EQ(block, audio::BlockResult::rendered, "delayed-worker resume failed or allocated");
    REQUIRE_EQ(counted_allocations, 0, "delayed-worker resume failed or allocated");
    resumed = transport.completed_snapshot().next_sample == 64 &&
              transport.completed_snapshot().generation == 2;
    if (!resumed) {
      REQUIRE_EQ(output[0], 0.0F, "unprimed seek emitted stale-generation audio");
      std::this_thread::yield();
    }
  }
  REQUIRE_TRUE(resumed,
               "seek failed to resume from its exact sample/generation after worker release");
  REQUIRE_EQ(source->status().ready_generation, 2,
             "seek failed to resume from its exact sample/generation after worker release");
  REQUIRE_EQ(source->status().missing_frames, 64,
             "seek failed to resume from its exact sample/generation after worker release");
  near(output[0], 0.25F);
}

void long_managed_clip_streams_past_previous_cap() {
  Directory directory;
  const auto root = directory.path() / "song";
  constexpr std::uint32_t seconds = 31;
  const auto wav = make_wav(directory.path(), 48'000 * seconds);
  auto session = take(app::ProjectSession::create(root));
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "long WAV open failed");
  REQUIRE_OK(session->imports().import_wav_audio(fd), "long WAV import failed");
  (void)::close(fd);
  Gate gate;
  auto model = take(project::ProjectModel::open(session->snapshot(), gate));
  app::RoutedPlaybackControl control(model, limits());
  REQUIRE_OK(control.submit_current(), "long clip graph failed");
  auto source = take(app::RoutedAudioSource::prepare(root, *model.snapshot(), control, 64));
  audio::TransportCursor transport;
  std::array<engine::StereoFrame, 4> scratch{};
  std::array<float, 8> output{};
  const auto device = source->render_source();
  REQUIRE_EQ(audio::render_transport_block(device, transport, true, scratch, output),
             audio::BlockResult::rendered, "long clip first block failed");
  near(output[0], 0.25F);
  constexpr auto late = static_cast<std::uint64_t>(30) * 48'000;
  require(transport.request_seek(late), "long clip late seek failed");
  bool resumed = false;
  for (std::size_t attempt = 0; attempt < 100'000 && !resumed; ++attempt) {
    counting_allocations = true;
    const auto result = audio::render_transport_block(device, transport, true, scratch, output);
    counting_allocations = false;
    REQUIRE_EQ(result, audio::BlockResult::rendered, "long clip late block failed or allocated");
    REQUIRE_EQ(counted_allocations, 0, "long clip late block failed or allocated");
    resumed = transport.completed_snapshot().next_sample == late + 4 &&
              transport.completed_snapshot().generation == 2;
    if (!resumed) {
      REQUIRE_EQ(transport.completed_snapshot().next_sample, late,
                 "long clip seek advanced before re-prime");
      REQUIRE_EQ(output[0], 0.0F, "long clip seek advanced before re-prime");
      std::this_thread::yield();
    }
  }
  REQUIRE_TRUE(resumed, "long clip did not stream at the previously rejected position");
  REQUIRE_EQ(source->status().missing_frames, 0,
             "long clip did not stream at the previously rejected position");
  near(output[0], 0.25F);
}

void encoded_managed_clips_stream_seek_reopen_and_reject_corruption() {
  const auto fixtures = fs::path(__FILE__).parent_path().parent_path() / "codec/assets";
  for (const auto [name, encoding] : {std::pair{"sine.aiff", project::SourceEncoding::aiff},
                                      std::pair{"sine.flac", project::SourceEncoding::flac},
                                      std::pair{"sine.mp3", project::SourceEncoding::mp3}}) {
    Directory directory;
    const auto root = directory.path() / "song";
    auto session = take(app::ProjectSession::create(root));
    const int fd = ::open((fixtures / name).c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE_GE(fd, 0, "encoded fixture open failed");
    auto started = session->imports().begin_encoded_audio_import(fd);
    (void)::close(fd);
    if (!started) {
      REQUIRE_EQ(started.error().code, foundation::ErrorCode::unsupported,
                 "encoded import failed without an explicit codec-unavailable result");
      // Codec-off builds must also reject a saved encoded source at the
      // routed source boundary, before touching its managed bytes.
      const auto wav = make_wav(directory.path(), 64);
      const int wav_fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
      REQUIRE_GE(wav_fd, 0, "codec-off fallback project setup failed");
      REQUIRE_OK(session->imports().import_wav_audio(wav_fd),
                 "codec-off fallback project setup failed");
      (void)::close(wav_fd);
      auto snapshot = session->snapshot();
      snapshot.sources.front().encoding = encoding;
      Gate gate;
      auto model = take(project::ProjectModel::open(snapshot, gate));
      app::RoutedPlaybackControl control(model, limits());
      REQUIRE_OK(control.submit_current(), "codec-off graph setup failed");
      auto rejected = app::RoutedAudioSource::prepare(root, *model.snapshot(), control, 64);
      REQUIRE_TRUE(!rejected, "codec-off routed source silently accepted encoded media");
      REQUIRE_EQ(rejected.error().code, foundation::ErrorCode::unsupported,
                 "codec-off routed source silently accepted encoded media");
      continue;
    }
    auto staged = take(std::move(started).value().run());
    REQUIRE_EQ(staged.source().encoding, encoding, "encoded stage changed source format");
    REQUIRE_OK(session->imports().finish_encoded_audio_import(std::move(staged)),
               "encoded managed import/save failed");
    REQUIRE_OK(session->saving().save(), "encoded managed import/save failed");
    const auto source_record = session->snapshot().sources.front();
    session.reset();
    auto reopened = take(app::ProjectSession::open(root));
    REQUIRE_EQ(reopened->snapshot().sources.front(), source_record,
               "encoded source identity or metadata changed on reopen");
    auto store_reader = take(persistence::ProjectStore::open_reader(root));
    const int managed_fd = take(store_reader.open_managed_source(source_record));
    auto oracle = oscillate::media::AudioReader::open_fd(managed_fd, source_record);
    (void)::close(managed_fd);
    auto decoded = take(std::move(oracle));
    std::array<oscillate::media::StereoFrame, 1> expected_start{};
    std::array<oscillate::media::StereoFrame, 1> expected_seek{};
    REQUIRE_EQ(take(decoded.read_at(0, expected_start)), 1,
               "encoded source oracle could not read both requested frames");
    REQUIRE_EQ(take(decoded.read_at(400, expected_seek)), 1,
               "encoded source oracle could not read both requested frames");
    Gate gate;
    auto model = take(project::ProjectModel::open(reopened->snapshot(), gate));
    app::RoutedPlaybackControl control(model, limits());
    REQUIRE_OK(control.submit_current(), "encoded routed graph failed");
    auto source = take(app::RoutedAudioSource::prepare(root, *model.snapshot(), control, 128));
    audio::TransportCursor transport;
    std::array<engine::StereoFrame, 64> scratch{};
    std::array<float, 128> output{};
    const auto device = source->render_source();
    counted_allocations = 0;
    counting_allocations = true;
    const auto first = audio::render_transport_block(device, transport, true, scratch, output);
    counting_allocations = false;
    REQUIRE_EQ(first, audio::BlockResult::rendered,
               "encoded first block failed, allocated, or underrun");
    REQUIRE_EQ(counted_allocations, 0, "encoded first block failed, allocated, or underrun");
    REQUIRE_EQ(transport.completed_snapshot().next_sample, 64,
               "encoded first block failed, allocated, or underrun");
    REQUIRE_EQ(source->status().missing_frames, 0,
               "encoded first block failed, allocated, or underrun");
    float energy = 0.0F;
    for (float value : output)
      energy += std::fabs(value);
    REQUIRE_GT(energy, 0.1F, "encoded first block was silent");
    near(output[0], expected_start[0].left);
    near(output[1], expected_start[0].right);
    require(transport.request_seek(400), "encoded seek request failed");
    bool resumed = false;
    for (std::size_t attempt = 0; attempt < 100'000 && !resumed; ++attempt) {
      counting_allocations = true;
      const auto block = audio::render_transport_block(device, transport, true, scratch, output);
      counting_allocations = false;
      REQUIRE_EQ(block, audio::BlockResult::rendered, "encoded seek callback failed or allocated");
      REQUIRE_EQ(counted_allocations, 0, "encoded seek callback failed or allocated");
      resumed = transport.completed_snapshot().next_sample == 464 &&
                transport.completed_snapshot().generation == 2;
      if (!resumed) {
        REQUIRE_EQ(transport.completed_snapshot().next_sample, 400,
                   "encoded seek advanced or played stale-generation samples before prime");
        REQUIRE_EQ(output[0], 0.0F,
                   "encoded seek advanced or played stale-generation samples before prime");
        std::this_thread::yield();
      }
    }
    REQUIRE_TRUE(resumed, "encoded worker failed to resume exact seek generation");
    REQUIRE_EQ(source->status().ready_generation, 2,
               "encoded worker failed to resume exact seek generation");
    REQUIRE_EQ(source->status().missing_frames, 0,
               "encoded worker failed to resume exact seek generation");
    REQUIRE_TRUE(!source->status().worker_failed,
                 "encoded worker failed to resume exact seek generation");
    energy = 0.0F;
    for (float value : output)
      energy += std::fabs(value);
    REQUIRE_GT(energy, 0.1F, "encoded seek resumed with silence");
    near(output[0], expected_seek[0].left);
    near(output[1], expected_seek[0].right);
    source.reset();
    const auto assets = root / "assets";
    for (const auto& entry : fs::directory_iterator(assets)) {
      if (!entry.is_regular_file())
        continue;
      std::fstream tamper(entry.path(), std::ios::binary | std::ios::in | std::ios::out);
      require(tamper.good(), "encoded managed asset could not be opened for corruption fixture");
      tamper.seekp(10);
      tamper.put(static_cast<char>(0x7f));
      require(tamper.good(), "encoded managed asset corruption fixture failed");
      break;
    }
    auto corrupt = app::RoutedAudioSource::prepare(root, *model.snapshot(), control, 128);
    REQUIRE_TRUE(!corrupt, "corrupted managed encoded bytes were accepted after reopen");
    REQUIRE_EQ(corrupt.error().code, foundation::ErrorCode::corrupt_data,
               "corrupted managed encoded bytes were accepted after reopen");
  }
}

void benchmark_routed_managed_callback() {
  Directory directory;
  const auto root = directory.path() / "song";
  const auto wav = make_wav(directory.path(), 200'000);
  auto session = take(app::ProjectSession::create(root));
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "benchmark WAV open failed");
  auto track = session->imports().import_wav_audio(fd);
  (void)::close(fd);
  REQUIRE_OK(track, "benchmark WAV import failed");
  auto group = session->tracks().add_bus_track(project::TrackKind::group_bus);
  auto returns = session->tracks().add_bus_track(project::TrackKind::return_bus);
  REQUIRE_TRUE(group, "benchmark routing setup failed");
  REQUIRE_TRUE(returns, "benchmark routing setup failed");
  REQUIRE_TRUE(session->routing().set_main_output(track.value(), group.value()),
               "benchmark routing setup failed");
  REQUIRE_TRUE(session->routing().add_send(track.value(), returns.value(), 0.5),
               "benchmark routing setup failed");
  Gate gate;
  std::array<engine::StereoFrame, 128> stereo{};
  std::array<float, 256> output{};
  constexpr std::size_t blocks = 1'000;
  for (const bool faded : {false, true}) {
    auto model = take(project::ProjectModel::open(session->snapshot(), gate));
    if (faded)
      require(model
                  .apply({project::SetClipFades{model.snapshot()->tracks.front().clips.front().id,
                                                {.in_fraction = 0.25, .out_fraction = 0.25}}})
                  .has_value(),
              "benchmark clip fade setup failed");
    for (const auto frames : {64U, 128U}) {
      app::RoutedPlaybackControl control(model, limits(128));
      REQUIRE_OK(control.submit_current(), "benchmark graph preparation failed");
      auto source = take(app::RoutedAudioSource::prepare(root, *model.snapshot(), control, 8192));
      auto render_source = source->render_source();
      audio::TransportCursor cursor;
      std::size_t failed = 0;
      double checksum = 0.0;
      counted_allocations = 0;
      std::chrono::duration<double> callback_time{};
      auto deadline = std::chrono::steady_clock::now();
      const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<double>(static_cast<double>(frames) / 48'000.0));
      for (std::size_t block = 0; block < blocks; ++block) {
        const auto started = std::chrono::steady_clock::now();
        counting_allocations = true;
        const auto result = audio::render_transport_block(
            render_source, cursor, true, std::span(stereo).first(frames),
            std::span(output).first(static_cast<std::size_t>(frames) * 2));
        counting_allocations = false;
        callback_time += std::chrono::steady_clock::now() - started;
        if (result != audio::BlockResult::rendered)
          ++failed;
        checksum += output[0];
        deadline += period;
        std::this_thread::sleep_until(deadline);
      }
      std::fprintf(stdout, "benchmark faded=%d frames=%u failed=%zu missing=%llu checksum=%.1f\n",
                   faded, frames, failed,
                   static_cast<unsigned long long>(source->status().missing_frames), checksum);
      REQUIRE_EQ(failed, 0,
                 "routed managed callback benchmark failed audio or allocation contract");
      REQUIRE_EQ(counted_allocations, 0,
                 "routed managed callback benchmark failed audio or allocation contract");
      REQUIRE_GT(checksum, 0.0,
                 "routed managed callback benchmark failed audio or allocation contract");
      REQUIRE_EQ(source->status().missing_frames, 0,
                 "routed managed callback benchmark failed audio or allocation contract");
      const auto microseconds = std::chrono::duration<double, std::micro>(callback_time).count() /
                                static_cast<double>(blocks);
      std::fprintf(stdout,
                   "faded=%d %u frames: %.3f us/block, %zu callback allocations, checksum %.1f\n",
                   faded, frames, microseconds, counted_allocations, checksum);
    }
  }
}

void converted_managed_impulse_seeks_on_project_grid() {
  Directory directory;
  const auto root = directory.path() / "song";
  const auto wav = make_wav(directory.path(), 4'410,
                            {.sample_rate = oscillate::test::sample_rate_44k, .impulse_at = 2'205});
  auto session = take(app::ProjectSession::create(root));
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "converted impulse fixture open failed");
  const auto supported = oscillate::media::source_rate_supported(44'100, 48'000);
  auto imported = session->imports().import_wav_audio(fd);
  (void)::close(fd);
  if (!supported) {
    REQUIRE_TRUE(!imported, "converter-OFF routed fixture was imported");
    REQUIRE_EQ(imported.error().code, foundation::ErrorCode::unsupported,
               "converter-OFF routed fixture was imported");
    return;
  }
  REQUIRE_TRUE(imported, "converted impulse import/save failed");
  REQUIRE_TRUE(session->saving().save(), "converted impulse import/save failed");
  session.reset();
  fs::remove(wav);
  session = take(app::ProjectSession::open(root));
  const auto& placement =
      std::get<project::ElapsedInterval>(session->snapshot().tracks[0].clips[0].placement);
  REQUIRE_EQ(placement.end.value, 4'800, "converted impulse reopened on wrong frame grid");
  REQUIRE_EQ(session->snapshot().tracks[0].clips[0].source_range.end.value, 4'410,
             "converted impulse reopened on wrong frame grid");
  Gate gate;
  auto model = take(project::ProjectModel::open(session->snapshot(), gate));
  app::RoutedPlaybackControl control(model, limits());
  REQUIRE_OK(control.submit_current(), "converted impulse graph failed");
  auto source = take(app::RoutedAudioSource::prepare(root, *model.snapshot(), control, 128));
  auto render_source = source->render_source();
  audio::TransportCursor cursor;
  std::array<engine::StereoFrame, 8> stereo{};
  std::array<float, 16> output{};
  REQUIRE_EQ(audio::render_transport_block(render_source, cursor, true, stereo, output),
             audio::BlockResult::rendered, "converted impulse initial render failed");
  require(cursor.request_seek(2'396), "converted impulse seek rejected");
  bool resumed = false;
  counted_allocations = 0;
  for (std::size_t attempt = 0; attempt < 100'000 && !resumed; ++attempt) {
    counting_allocations = true;
    const auto result = audio::render_transport_block(render_source, cursor, true, stereo, output);
    counting_allocations = false;
    REQUIRE_EQ(result, audio::BlockResult::rendered,
               "converted impulse callback failed or allocated");
    REQUIRE_EQ(counted_allocations, 0, "converted impulse callback failed or allocated");
    resumed = cursor.completed_snapshot().next_sample == 2'404;
    if (!resumed)
      std::this_thread::yield();
  }
  REQUIRE_TRUE(resumed, "converted impulse seek did not re-prime");
  REQUIRE_EQ(source->status().ready_generation, 2, "converted impulse seek did not re-prime");
  REQUIRE_GT(output[8], 0.5F, "converted impulse did not land near project frame 2400");
  REQUIRE_LT(std::abs(output[0]), 0.2F, "converted impulse did not land near project frame 2400");
  REQUIRE_EQ(source->status().missing_frames, 0,
             "converted impulse did not land near project frame 2400");
  require(cursor.request_seek(4'796), "converted impulse end seek rejected");
  resumed = false;
  for (std::size_t attempt = 0; attempt < 100'000 && !resumed; ++attempt) {
    REQUIRE_EQ(audio::render_transport_block(render_source, cursor, true, stereo, output),
               audio::BlockResult::rendered, "converted impulse end render failed");
    resumed = cursor.completed_snapshot().next_sample == 4'804;
    if (!resumed)
      std::this_thread::yield();
  }
  REQUIRE_TRUE(resumed, "converted impulse clip end was not exact or underrun-free");
  REQUIRE_EQ(output[8], 0.0F, "converted impulse clip end was not exact or underrun-free");
  REQUIRE_EQ(output[14], 0.0F, "converted impulse clip end was not exact or underrun-free");
  REQUIRE_EQ(source->status().missing_frames, 0,
             "converted impulse clip end was not exact or underrun-free");
}

void routed_clip_fades_match_exact_sample_boundaries() {
  Directory directory;
  const auto root = directory.path() / "song";
  const auto wav = make_wav(directory.path(), 8);
  auto session = take(app::ProjectSession::create(root));
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "fade fixture open failed");
  const auto track = session->imports().import_wav_audio(fd);
  (void)::close(fd);
  REQUIRE_TRUE(track, "routed fade fixture could not save");
  REQUIRE_TRUE(session->audio_clips().set_audio_clip_fades(
                   session->snapshot().tracks.front().clips.front().id,
                   {0.5, 0.25, project::FadeCurve::linear, project::FadeCurve::smoothstep}),
               "routed fade fixture could not save");
  REQUIRE_TRUE(session->saving().save(), "routed fade fixture could not save");
  Gate gate;
  auto model = take(project::ProjectModel::open(session->snapshot(), gate));
  app::RoutedPlaybackControl control(model, limits());
  REQUIRE_OK(control.submit_current(), "routed fade graph did not prepare");
  auto source = take(app::RoutedAudioSource::prepare(root, *model.snapshot(), control, 64));
  audio::TransportCursor cursor;
  std::array<engine::StereoFrame, 8> stereo{};
  std::array<float, 16> output{};
  counted_allocations = 0;
  counting_allocations = true;
  const auto rendered =
      audio::render_transport_block(source->render_source(), cursor, true, stereo, output);
  counting_allocations = false;
  REQUIRE_EQ(rendered, audio::BlockResult::rendered, "routed fade callback failed or allocated");
  REQUIRE_EQ(counted_allocations, 0, "routed fade callback failed or allocated");
  const float expected[]{0.0F, 0.0625F, 0.125F, 0.1875F, 0.25F, 0.25F, 0.125F, 0.0F};
  for (std::size_t frame = 0; frame < 8; ++frame) {
    near(output[frame * 2], expected[frame]);
    near(output[frame * 2 + 1], expected[frame]);
  }
  REQUIRE_EQ(source->status().missing_frames, 0, "routed fade lost source frames");
}

void routed_start_at_later_sample_primes_matching_source_frames() {
  Directory directory;
  const auto root = directory.path() / "song";
  const auto wav = make_wav(directory.path(), 32, {.impulse_at = 12});
  auto session = take(app::ProjectSession::create(root));
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "later-start fixture open failed");
  const auto track = session->imports().import_wav_audio(fd);
  (void)::close(fd);
  REQUIRE_TRUE(track, "later-start fixture could not save");
  REQUIRE_TRUE(session->saving().save(), "later-start fixture could not save");
  Gate gate;
  auto model = take(project::ProjectModel::open(session->snapshot(), gate));
  app::RoutedPlaybackControl control(model, limits());
  REQUIRE_OK(control.submit_current(), "later-start graph did not prepare");
  auto source =
      take(app::RoutedAudioSource::prepare(root, *model.snapshot(), control, 64, nullptr, {}, 8));
  audio::TransportCursor cursor(8);
  std::array<engine::StereoFrame, 8> stereo{};
  std::array<float, 16> output{};
  counted_allocations = 0;
  counting_allocations = true;
  const auto rendered =
      audio::render_transport_block(source->render_source(), cursor, true, stereo, output);
  counting_allocations = false;
  REQUIRE_EQ(rendered, audio::BlockResult::rendered,
             "later routed start lost the requested source position or allocated");
  REQUIRE_EQ(counted_allocations, 0,
             "later routed start lost the requested source position or allocated");
  REQUIRE_EQ(cursor.completed_snapshot().next_sample, 16,
             "later routed start lost the requested source position or allocated");
  REQUIRE_EQ(cursor.completed_snapshot().generation, 1,
             "later routed start lost the requested source position or allocated");
  REQUIRE_EQ(source->status().missing_frames, 0,
             "later routed start lost the requested source position or allocated");
  for (std::size_t frame = 0; frame < stereo.size(); ++frame) {
    const auto expected = frame == 4 ? 0.75F : 0.0F;
    near(output[2 * frame], expected);
    near(output[2 * frame + 1], expected);
  }
}

void captured_audio_edit_prepares_without_reading_mutable_control_state() {
  Directory directory;
  const auto root = directory.path() / "captured-source-song";
  const auto wav = make_wav(directory.path(), 512);
  auto session = take(app::ProjectSession::create(root));
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "captured source WAV open failed");
  const auto imported = session->imports().import_wav_audio(fd);
  (void)::close(fd);
  REQUIRE_TRUE(imported, "captured source import/save failed");
  REQUIRE_TRUE(session->saving().save(), "captured source import/save failed");
  const auto clip = session->snapshot().tracks.front().clips.front().id;
  Gate gate;
  auto model = take(project::ProjectModel::open(session->snapshot(), gate));
  app::RoutedPlaybackControl control(model, limits());
  REQUIRE_OK(control.submit_current(), "captured source graph did not prepare");
  std::array<engine::StereoFrame, 4> input{}, output{};
  std::array<std::span<const engine::StereoFrame>, 2> sources{};
  sources[0] = input;
  REQUIRE_EQ(control.render_block(sources, output, {0, 1}), engine::RoutedRenderStatus::rendered,
             "captured source graph did not activate");
  control.observe();
  const auto moved =
      take(model.apply({project::SetClipPlacement{clip, project::ElapsedInterval{{8}, {520}}}}));
  const auto capture = take(control.capture_source_edit(moved, "move clip"));
  // A later UI edit changes the model while the worker still owns the exact
  // earlier capture. Preparation must use only the captured snapshot/layout.
  (void)take(model.apply({project::SetClipPlacement{clip, project::ElapsedInterval{{16}, {528}}}}));
  auto prepared = app::RoutedAudioSource::prepare(root, capture, control, 64, nullptr, {}, 8, 7);
  REQUIRE_OK(prepared, "captured source preparation read superseding mutable control state");
  REQUIRE_EQ(prepared.value()->prime_handoff_window(7, 8, 64),
             oscillate::media::HandoffWindowStatus::ready,
             "captured source did not prove the exact first block and generation");
  REQUIRE_EQ(prepared.value()->prime_handoff_window(7, 0, 8),
             oscillate::media::HandoffWindowStatus::ready,
             "captured source did not prove the exact first block and generation");
  REQUIRE_EQ(prepared.value()->prime_handoff_window(8, 8, 64),
             oscillate::media::HandoffWindowStatus::invalid,
             "captured source did not prove the exact first block and generation");
  audio::TransportCursor live_cursor;
  REQUIRE_OK(live_cursor.seek_after_quiesce(32),
             "late-source cursor did not advance its transport generation");
  auto late =
      app::RoutedAudioSource::prepare(root, capture, control, 64, nullptr, {}, 8, 7, &live_cursor);
  REQUIRE_TRUE(late, "late source prime ignored the current sample or transport generation");
  REQUIRE_EQ(late.value()->prime_handoff_window(2, 32, 64),
             oscillate::media::HandoffWindowStatus::ready,
             "late source prime ignored the current sample or transport generation");
  REQUIRE_EQ(late.value()->prime_handoff_window(7, 32, 64),
             oscillate::media::HandoffWindowStatus::invalid,
             "late source prime ignored the current sample or transport generation");
  auto invalid = capture;
  invalid.max_frames = 0;
  require(!app::RoutedAudioSource::prepare(root, invalid, control, 64),
          "invalid captured frame limit was accepted");
}
} // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--bench") {
      benchmark_routed_managed_callback();
      return 0;
    }
    managed_wav_routes_through_one_transport_cursor();
    external_wav_requires_saved_identity_before_routed_reader();
    bounded_preparation_rejects_large_managed_clip();
    saved_processing_never_falls_back_to_raw_routed_audio();
    musical_placement_and_missing_asset();
    delayed_worker_exposes_underrun_and_preserves_seek_generation();
    long_managed_clip_streams_past_previous_cap();
    encoded_managed_clips_stream_seek_reopen_and_reject_corruption();
    converted_managed_impulse_seeks_on_project_grid();
    routed_clip_fades_match_exact_sample_boundaries();
    routed_start_at_later_sample_primes_matching_source_frames();
    captured_audio_edit_prepares_without_reading_mutable_control_state();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
  return 0;
}
