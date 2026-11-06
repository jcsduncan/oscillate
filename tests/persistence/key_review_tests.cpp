#include "oscillate/persistence/store.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"
#include "private.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
using namespace oscillate;
using namespace oscillate::project;
using namespace oscillate::persistence;
namespace fs = std::filesystem;
using Json = nlohmann::json;

using oscillate::test::require;
using oscillate::test::take;
template <typename T> T id(std::uint64_t low) { return take(T::from_bits({0, low})); }

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-key-review") {}
};

struct Gate final : TransportGate {
  struct Lease final : StoppedTransportLease {};
  foundation::Result<std::unique_ptr<StoppedTransportLease>> acquire_stopped() override {
    return foundation::Result<std::unique_ptr<StoppedTransportLease>>::success(
        std::make_unique<Lease>());
  }
};

Snapshot empty_project() {
  auto tempo = take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1));
  auto meter = take(timeline::MeterMap::create({{{0}, 4, 4}}, 1));
  return {id<ProjectId>(1), 1, 48'000, std::move(tempo), std::move(meter), {}};
}

void write_wav(const fs::path& path) {
  // Valid 4-frame mono PCM16 source, including the required RIFF/fmt/data lengths.
  const std::array<unsigned char, 52> bytes{
      'R', 'I', 'F', 'F', 44, 0, 0,    0,    'W', 'A', 'V',  'E',  'f',  'm', 't', ' ', 16, 0,
      0,   0,   1,   0,   1,  0, 0x80, 0xbb, 0,   0,   0x00, 0x77, 0x01, 0,   2,   0,   16, 0,
      'd', 'a', 't', 'a', 8,  0, 0,    0,    0,   0,   0,    0,    0,    0,   0,   0};
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  require(output.good(), "WAV fixture write failed");
}

ApplySourceKeyAnalysis analysis(const Snapshot& state, const SourceRecord& source,
                                std::optional<MusicalKey> detected = MusicalKey{0,
                                                                                KeyMode::major}) {
  return {state.revision,
          source.id,
          source.sha256,
          source.sample_rate,
          source.frames,
          detected,
          detected ? 0.72 : 0.0,
          detected ? std::vector<CompatibleKey>{{{9, KeyMode::minor}, KeyRelation::relative},
                                                {{7, KeyMode::major}, KeyRelation::dominant}}
                   : std::vector<CompatibleKey>{}};
}

void model_review_contract() {
  Gate gate;
  auto state = empty_project();
  const SourceRecord source{
      id<SourceId>(2), SourceEncoding::wav, std::string(64, 'a'), 52, 48'000, 1, 4};
  auto model = take(ProjectModel::open(state, gate));
  REQUIRE_OK(model.apply({AddSource{source}}));
  auto stale = analysis(*model.snapshot(), source);
  stale.origin_revision -= 1;
  require(!model.apply({stale}), "stale revision installed");
  stale = analysis(*model.snapshot(), source);
  stale.source_sha256[0] = 'b';
  require(!model.apply({stale}), "wrong source version installed");
  stale = analysis(*model.snapshot(), source);
  stale.sample_rate = oscillate::test::sample_rate_44k;
  require(!model.apply({stale}), "wrong source rate installed");
  stale = analysis(*model.snapshot(), source);
  stale.frames = 5;
  require(!model.apply({stale}), "wrong source frame count installed");
  require(model.snapshot()->source_key_reviews.empty(), "rejected analysis mutated state");
  REQUIRE_OK(model.apply({SetSourceKeyCorrection{source.id, MusicalKey{2, KeyMode::minor}}}));
  auto generated = analysis(*model.snapshot(), source);
  REQUIRE_OK(model.apply({generated}));
  const auto accepted = model.snapshot()->source_key_reviews.front();
  REQUIRE_EQ(accepted.detected, (MusicalKey{0, KeyMode::major}),
             "generated and correction provenance lost");
  REQUIRE_EQ(accepted.correction, (MusicalKey{2, KeyMode::minor}),
             "generated and correction provenance lost");
  REQUIRE_EQ(accepted.compatible.size(), 2, "generated and correction provenance lost");
  auto invalid = analysis(*model.snapshot(), source);
  invalid.compatible.push_back({MusicalKey{0, KeyMode::major}, KeyRelation::subdominant});
  require(!model.apply({invalid}), "duplicate primary key accepted");
  invalid = analysis(*model.snapshot(), source);
  invalid.detected = MusicalKey{12, KeyMode::major};
  require(!model.apply({invalid}), "invalid tonic accepted");
  REQUIRE_EQ(model.snapshot()->source_key_reviews.front(), accepted,
             "rejected key edit mutated state");
  REQUIRE_OK(model.apply({analysis(*model.snapshot(), source, std::nullopt)}));
  REQUIRE_TRUE(!model.snapshot()->source_key_reviews.front().detected,
               "unknown reanalysis erased manual correction");
  REQUIRE_EQ(model.snapshot()->source_key_reviews.front().correction, accepted.correction,
             "unknown reanalysis erased manual correction");
  REQUIRE_OK(model.undo());
  REQUIRE_EQ(model.snapshot()->source_key_reviews.front(), accepted,
             "undo lost previous suggestion");
  REQUIRE_OK(model.redo());
  REQUIRE_OK(model.apply({RemoveSource{source.id}}));
  require(model.snapshot()->source_key_reviews.empty(), "source removal retained orphan review");
  REQUIRE_OK(model.undo());
  REQUIRE_EQ(model.snapshot()->source_key_reviews.front().correction, accepted.correction,
             "source removal undo lost correction");
}

void codec_and_store_contract() {
  Directory temp;
  const auto input = temp.path() / "source.wav";
  const auto root = temp.path() / "project";
  write_wav(input);
  const int fd = ::open(input.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "WAV fixture open failed");
  auto writer = take(ProjectStore::open_writer(root, true));
  auto receipt = take(writer.stage_wav(id<SourceId>(2), fd));
  ::close(fd);
  Gate gate;
  auto model = take(ProjectModel::open(empty_project(), gate));
  REQUIRE_OK(model.apply({AddSource{receipt.record()}}));
  REQUIRE_OK(
      model.apply({SetSourceKeyCorrection{receipt.record().id, MusicalKey{2, KeyMode::minor}}}));
  REQUIRE_OK(model.apply({analysis(*model.snapshot(), receipt.record())}));
  const auto before = *model.snapshot();
  const auto text =
      detail::encode_manifest(before, std::string(32, 'a'), std::nullopt, SaveKind::explicit_save);
  auto value = Json::parse(text);
  REQUIRE_EQ(value["schema"]["minor"], 47, "wrong key-review schema version");
  REQUIRE_EQ(take(detail::decode_manifest(text)).snapshot.source_key_reviews,
             before.source_key_reviews, "key review codec round-trip failed");
  for (const auto& altered : {"detected", "confidence", "compatible", "correction"}) {
    auto malformed = value;
    malformed["project"]["source_key_reviews"][0].erase(altered);
    require(!detail::decode_manifest(malformed.dump()), "missing key review field accepted");
  }
  auto malformed = value;
  malformed["project"]["source_key_reviews"][0]["compatible"][0]["key"]["tonic"] = 12;
  require(!detail::decode_manifest(malformed.dump()), "malformed compatible key accepted");
  malformed = value;
  malformed["project"]["source_key_reviews"][0]["compatible"][0]["relation"] = "dominant";
  require(!detail::decode_manifest(malformed.dump()), "incorrect compatible relation accepted");
  malformed = value;
  malformed["project"]["source_key_reviews"][0]["correction"]["mode"] = "dorian";
  require(!detail::decode_manifest(malformed.dump()), "unsupported correction mode accepted");
  malformed = value;
  malformed["project"]["source_key_reviews"].push_back(
      malformed["project"]["source_key_reviews"][0]);
  require(!detail::decode_manifest(malformed.dump()), "duplicate review accepted");
  for (const int minor : {46, 48}) {
    malformed = value;
    malformed["schema"]["minor"] = minor;
    require(!detail::decode_manifest(malformed.dump()), "another schema minor accepted");
  }
  std::array<SourceReceipt, 1> receipts{std::move(receipt)};
  const auto saved = take(writer.save(before, SaveKind::explicit_save, receipts));
  REQUIRE_EQ(take(writer.load_current()).snapshot.source_key_reviews, before.source_key_reviews,
             "saved correction did not reopen");
  writer = take(ProjectStore::open_reader(root));
  auto interrupted = take(ProjectStore::open_writer(
      root, false, [](SaveStage stage) { return stage == SaveStage::pointer_written; }));
  auto next = before;
  next.revision += 1;
  next.source_key_reviews.front().correction = MusicalKey{4, KeyMode::major};
  require(!interrupted.save(next, SaveKind::explicit_save), "interrupted save committed");
  REQUIRE_EQ(take(interrupted.load_current()).generation, saved.generation,
             "interrupted save lost last valid key correction");
  REQUIRE_EQ(take(interrupted.load_current()).snapshot.source_key_reviews,
             before.source_key_reviews, "interrupted save lost last valid key correction");
}
// Schema 1.29 transient review: generated onsets are replaced only by an
// accepted reanalysis; a user dismissal is a correction that reanalysis never
// replaces and that hides generated onsets near it.
void onset_review_contract() {
  Gate gate;
  auto state = empty_project();
  const SourceRecord source{
      id<SourceId>(2), SourceEncoding::wav, std::string(64, 'a'), 52, 48'000, 1, 48'000};
  auto model = take(ProjectModel::open(state, gate));
  REQUIRE_OK(model.apply({AddSource{source}}));
  const auto onsets = [&](std::vector<std::uint64_t> frames) {
    return ApplySourceOnsetAnalysis{
        model.snapshot()->revision, source.id, source.sha256, source.sample_rate, source.frames,
        std::move(frames),          false};
  };
  auto stale = onsets({100, 5'000});
  stale.origin_revision -= 1;
  require(!model.apply({stale}), "stale onset analysis installed");
  stale = onsets({100, 5'000});
  stale.source_sha256[0] = 'b';
  require(!model.apply({stale}), "onset analysis for another source version installed");
  require(!model.apply({onsets({5'000, 100})}), "unordered onset suggestions installed");
  require(!model.apply({onsets({48'000})}), "out-of-range onset suggestion installed");
  REQUIRE_OK(model.apply({onsets({100, 5'000, 9'000})}));
  // Dismissing hides the generated onset within the dismissal distance.
  REQUIRE_OK(model.apply({SetSourceDismissedOnsets{source.id, {5'030}}}));
  auto review = model.snapshot()->source_tempo_reviews.front();
  REQUIRE_EQ(review.generated_onset_frames, (std::vector<std::uint64_t>{100, 9'000}),
             "a dismissal did not hide its generated onset");
  REQUIRE_EQ(review.dismissed_onset_frames, std::vector<std::uint64_t>{5'030},
             "a dismissal did not hide its generated onset");
  REQUIRE_EQ(review.detected_bpm, 0.0, "a dismissal did not hide its generated onset");
  // Reanalysis replaces generated onsets but keeps the dismissal.
  REQUIRE_OK(model.apply({onsets({120, 5'010, 20'000})}));
  review = model.snapshot()->source_tempo_reviews.front();
  REQUIRE_EQ(review.generated_onset_frames, (std::vector<std::uint64_t>{120, 20'000}),
             "reanalysis overwrote a dismissal or kept old suggestions");
  REQUIRE_EQ(review.dismissed_onset_frames, std::vector<std::uint64_t>{5'030},
             "reanalysis overwrote a dismissal or kept old suggestions");
  // Clearing a dismissal does not silently restore the suggestion.
  REQUIRE_OK(model.apply({SetSourceDismissedOnsets{source.id, {}}}));
  REQUIRE_EQ(model.snapshot()->source_tempo_reviews.front().generated_onset_frames,
             (std::vector<std::uint64_t>{120, 20'000}),
             "clearing a dismissal restored an old suggestion");
  REQUIRE_OK(model.undo());
  REQUIRE_EQ(model.snapshot()->source_tempo_reviews.front(), review, "undo lost the dismissal");
  // Tempo reanalysis leaves onset review untouched.
  REQUIRE_OK(model.apply({ApplySourceTempoAnalysis{model.snapshot()->revision,
                                                   source.id,
                                                   source.sha256,
                                                   source.sample_rate,
                                                   source.frames,
                                                   120.0,
                                                   0.8,
                                                   {0, 24'000},
                                                   false}}));
  REQUIRE_EQ(model.snapshot()->source_tempo_reviews.front().generated_onset_frames,
             review.generated_onset_frames, "tempo analysis changed onset review");
  REQUIRE_EQ(model.snapshot()->source_tempo_reviews.front().dismissed_onset_frames,
             review.dismissed_onset_frames, "tempo analysis changed onset review");

  // The downbeat flag (1.38) lies inside the source and round-trips.
  require(!model.apply({SetSourceDownbeat{source.id, source.frames}}),
          "a downbeat past the source end was accepted");
  REQUIRE_OK(model.apply({SetSourceDownbeat{source.id, 12'000}}));
  REQUIRE_EQ(model.snapshot()->source_tempo_reviews.front().downbeat_frame, 12'000,
             "the downbeat was not stored");
  // Codec: round-trip and exact fields.
  const auto before = *model.snapshot();
  const auto text =
      detail::encode_manifest(before, std::string(32, 'a'), std::nullopt, SaveKind::explicit_save);
  REQUIRE_EQ(take(detail::decode_manifest(text)).snapshot.source_tempo_reviews,
             before.source_tempo_reviews, "onset review codec round-trip failed");
  const auto value = Json::parse(text);
  for (const auto& missing :
       {"generated_onset_frames", "dismissed_onset_frames", "onsets_truncated", "downbeat_frame"}) {
    auto malformed = value;
    malformed["project"]["source_tempo_reviews"][0].erase(missing);
    require(!detail::decode_manifest(malformed.dump()), "missing onset field accepted");
  }
  auto malformed = value;
  malformed["project"]["source_tempo_reviews"][0]["onsets_truncated"] = 1;
  require(!detail::decode_manifest(malformed.dump()), "nonboolean onset truncation accepted");
  malformed = value;
  malformed["project"]["source_tempo_reviews"][0]["downbeat_frame"] = before.sources.front().frames;
  require(!detail::decode_manifest(malformed.dump()), "a downbeat past the source end loaded");
}
} // namespace

int main() {
  try {
    model_review_contract();
    codec_and_store_contract();
    onset_review_contract();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
