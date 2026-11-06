#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"

#include <oscillate/media/cache_identity.hpp>
#include <oscillate/media/processing_job.hpp>
#include <oscillate/timeline/timing.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <latch>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace foundation = oscillate::foundation;
namespace media = oscillate::media;
namespace project = oscillate::project;
namespace timeline = oscillate::timeline;

using oscillate::test::require;

using oscillate::test::take;

template <typename T> T id(std::uint64_t low) { return take(T::from_bits({0, low})); }

media::CacheDescriptor source_analysis() {
  return {media::CacheArtifact::source_analysis,
          {id<project::SourceId>(1), project::SourceEncoding::wav, std::string(64, 'a'), 60, 48000,
           2, 8},
          {},
          {},
          "waveform.v1",
          1,
          1,
          {std::byte{0x01}, std::byte{0x02}},
          {}};
}

media::CacheKey key(const media::CacheDescriptor& descriptor) {
  return take(media::make_cache_key(descriptor));
}

void semantic_keys_are_stable_and_dependency_scoped() {
  auto input = source_analysis();
  const auto baseline = key(input);
  // Golden digest of the documented version-1 binary encoding, calculated
  // independently from its fields. It prevents an accidental format change.
  REQUIRE_EQ(baseline.hex(), "dcc09941a74cc442a125b6f53b90d1de63f4d7cb2faf6d28c9c513db0f2f1541",
             "cache key format changed");
  input.source.id = id<project::SourceId>(99);
  REQUIRE_EQ(key(input), baseline, "source ID prevented identical-byte reuse");
  input.source.sha256[0] = 'b';
  REQUIRE_NE(key(input), baseline, "changed source bytes retained cache identity");
  input.source.sha256[0] = '0';
  REQUIRE_NE(key(input), baseline, "numeric source digest byte was not decoded");
  input = source_analysis();
  ++input.source.byte_length;
  REQUIRE_NE(key(input), baseline, "source byte length omitted from key");
  for (const auto encoding : {project::SourceEncoding::aiff, project::SourceEncoding::flac,
                              project::SourceEncoding::mp3}) {
    input = source_analysis();
    input.source.encoding = encoding;
    input.source.byte_length = 1;
    REQUIRE_NE(key(input), baseline, "encoded source did not get a distinct cache identity");
  }
  input = source_analysis();
  input.algorithm_id = "waveform.v2";
  REQUIRE_NE(key(input), baseline, "algorithm identity omitted from key");
  input = source_analysis();
  input.algorithm_version = 2;
  REQUIRE_NE(key(input), baseline, "algorithm version omitted from key");
  input = source_analysis();
  input.quality = 2;
  REQUIRE_NE(key(input), baseline, "quality omitted from key");
  input = source_analysis();
  input.canonical_settings[1] = std::byte{0x03};
  REQUIRE_NE(key(input), baseline, "semantic settings omitted from key");
  input = source_analysis();
  input.project_rate = oscillate::test::sample_rate_48k;
  REQUIRE_NE(key(input), baseline, "conditional project rate omitted from key");
  input.project_rate = oscillate::test::sample_rate_44k;
  REQUIRE_NE(key(input), baseline, "changed project rate retained key");
  input = source_analysis();
  input.tempo_sha256 = std::string(64, 'b');
  const auto tempo_dependent = key(input);
  REQUIRE_NE(tempo_dependent, baseline, "tempo dependence omitted from key");
  input.tempo_sha256 = std::string(64, 'c');
  REQUIRE_NE(key(input), tempo_dependent, "tempo edit retained dependent key");
  input.tempo_sha256.reset();
  REQUIRE_EQ(key(input), baseline, "tempo-independent key changed with tempo state");

  input = source_analysis();
  input.artifact = media::CacheArtifact::clip_processed;
  input.clip = id<project::ClipId>(7);
  input.project_rate = oscillate::test::sample_rate_48k;
  const auto clip_key = key(input);
  REQUIRE_NE(clip_key, baseline, "clip artifact collided with source artifact");
  input.clip = id<project::ClipId>(8);
  REQUIRE_NE(key(input), clip_key, "instance identity omitted from processed clip key");
}

class Gate final : public project::TransportGate {
public:
  class Lease final : public project::StoppedTransportLease {};
  foundation::Result<std::unique_ptr<project::StoppedTransportLease>> acquire_stopped() override {
    return foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::success(
        std::make_unique<Lease>());
  }
};

void unrelated_project_edit_does_not_churn_cache_key() {
  Gate gate;
  auto model = take(project::ProjectModel::create(
      id<project::ProjectId>(10), 48000,
      take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1)),
      take(timeline::MeterMap::create({{{0}, 4, 4}}, 1)), gate));
  REQUIRE_OK(model.apply({project::AddSource{source_analysis().source}}));
  auto before = source_analysis();
  before.source = model.snapshot()->sources.front();
  const auto before_key = key(before);
  const auto revision = model.snapshot()->revision;
  REQUIRE_OK(model.apply({project::AddTrack{id<project::TrackId>(20)}}));
  auto after = source_analysis();
  after.source = model.snapshot()->sources.front();
  REQUIRE_GT(model.snapshot()->revision, revision,
             "unrelated model revision invalidated source analysis cache");
  REQUIRE_EQ(key(after), before_key, "unrelated model revision invalidated source analysis cache");
}

void malformed_descriptors_are_rejected() {
  auto input = source_analysis();
  input.source.byte_length = 0;
  require(!media::make_cache_key(input), "invalid source length accepted");
  input.source.encoding = project::SourceEncoding::flac;
  require(!media::make_cache_key(input), "zero-length encoded source accepted");
  input = source_analysis();
  input.source.sha256[0] = 'A';
  require(!media::make_cache_key(input), "uppercase source hash accepted");
  input = source_analysis();
  input.source.sha256.pop_back();
  require(!media::make_cache_key(input), "short source hash accepted");
  input = source_analysis();
  input.tempo_sha256 = std::string(64, 'z');
  require(!media::make_cache_key(input), "malformed tempo identity accepted");
  input = source_analysis();
  input.project_rate = 0;
  require(!media::make_cache_key(input), "zero project rate accepted");
  input = source_analysis();
  input.algorithm_id = "Waveform";
  require(!media::make_cache_key(input), "noncanonical algorithm ID accepted");
  input = source_analysis();
  input.algorithm_version = 0;
  require(!media::make_cache_key(input), "zero algorithm version accepted");
  input = source_analysis();
  input.canonical_settings.resize(1024U * 1024U + 1U);
  require(!media::make_cache_key(input), "oversized settings accepted");
  input = source_analysis();
  input.clip = id<project::ClipId>(7);
  require(!media::make_cache_key(input), "source-only artifact acquired clip dependency");
  input = source_analysis();
  input.artifact = media::CacheArtifact::clip_processed;
  require(!media::make_cache_key(input), "clip artifact without instance/rate accepted");
}

void cancellation_and_final_claim_are_generation_checked() {
  auto input = source_analysis();
  const auto original_key = key(input);
  input.canonical_settings[0] = std::byte{0x04};
  const auto edited_key = key(input);
  media::CompletionGate gate;
  const auto old = take(gate.issue(original_key));
  REQUIRE_EQ(old.generation(), 1, "first worker ticket invalid");
  REQUIRE_TRUE(!old.cancelled(), "first worker ticket invalid");

  std::latch worker_started{1};
  std::latch release_worker{1};
  std::atomic<bool> saw_cancel{false};
  std::thread worker([old, &worker_started, &release_worker, &saw_cancel] {
    worker_started.count_down();
    release_worker.wait();
    saw_cancel.store(old.cancelled(), std::memory_order_release);
  });
  worker_started.wait();
  const auto current = take(gate.issue(edited_key));
  release_worker.count_down();
  worker.join();
  REQUIRE_TRUE(saw_cancel.load(std::memory_order_acquire),
               "superseded worker did not observe cancellation");
  REQUIRE_TRUE(old.cancelled(), "superseded worker did not observe cancellation");
  REQUIRE_EQ(current.generation(), 2, "superseded worker did not observe cancellation");
  REQUIRE_TRUE(!current.cancelled(), "superseded worker did not observe cancellation");
  REQUIRE_TRUE(!gate.claim_install(old, original_key),
               "late old generation installed after an edit");
  REQUIRE_TRUE(!gate.claim_install(old, edited_key), "late old generation installed after an edit");
  require(!gate.claim_install(current, original_key),
          "new worker installed against stale current inputs");
  REQUIRE_TRUE(gate.claim_install(current, edited_key),
               "completion was not one-shot and input checked");
  REQUIRE_TRUE(current.cancelled(), "completion was not one-shot and input checked");
  REQUIRE_TRUE(!gate.claim_install(current, edited_key),
               "completion was not one-shot and input checked");

  const auto cancelled = take(gate.issue(edited_key));
  gate.cancel();
  REQUIRE_TRUE(cancelled.cancelled(), "explicitly cancelled completion installed");
  REQUIRE_TRUE(!gate.claim_install(cancelled, edited_key),
               "explicitly cancelled completion installed");
  const auto after_cancel = take(gate.issue(original_key));
  REQUIRE_EQ(after_cancel.generation(), 4, "new generation failed after cancellation");
  REQUIRE_TRUE(gate.claim_install(after_cancel, original_key),
               "new generation failed after cancellation");

  media::JobTicket orphan = [&] {
    media::CompletionGate temporary;
    return take(temporary.issue(original_key));
  }();
  require(orphan.cancelled(), "destroyed gate left a worker request live");
}

} // namespace

int main() {
  try {
    semantic_keys_are_stable_and_dependency_scoped();
    unrelated_project_edit_does_not_churn_cache_key();
    malformed_descriptors_are_rejected();
    cancellation_and_final_claim_are_generation_checked();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
