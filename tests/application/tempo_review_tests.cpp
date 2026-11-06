#include "oscillate/application/project_session.hpp"
#include "oscillate/application/tempo_cache.hpp"
#include "oscillate/media/cache_store.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"
#include "oscillate/test/wav.hpp"

#include "../../src/persistence/private.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
using namespace oscillate;
namespace fs = std::filesystem;

using oscillate::test::require;

using oscillate::test::take;

void accepted(foundation::Result<void> result) {
  if (!result) {
    throw std::runtime_error(result.error().message);
  }
}

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-tempo-review") {}
};

fs::path make_wav(const fs::path& directory) {
  constexpr std::size_t frames = 48;
  constexpr std::size_t click_spacing = 8;
  constexpr std::int16_t click = 12'000;
  const auto path = directory / "source.wav";
  std::vector<std::int16_t> samples(frames);
  for (std::size_t index = 0; index < frames; ++index)
    samples[index] = index % click_spacing == 0 ? click : std::int16_t{0};
  oscillate::test::write_wav_pcm16(path, oscillate::test::sample_rate_48k,
                                   oscillate::test::mono_channels, samples);
  return path;
}

std::string replace_once(std::string text, std::string_view from, std::string_view to) {
  const auto position = text.find(from);
  REQUIRE_NE(position, std::string::npos, "manifest fixture token missing");
  text.replace(position, from.size(), to);
  return text;
}

analysis::TempoEstimate estimate_for(const project::SourceRecord& source,
                                     std::vector<std::uint64_t> beats, double bpm = 120.0) {
  const auto key = take(application::tempo_cache_key(source));
  return {.identity = {key.hex(), 7},
          .sample_rate = source.sample_rate,
          .frames = source.frames,
          .bpm = bpm,
          .confidence = 0.8,
          .suggested_beat_frames = std::move(beats),
          .suggestions_truncated = false,
          .suggested_onset_frames = {},
          .onsets_truncated = false};
}

void review_survives_reanalysis_reopen_and_cache_clear() {
  Directory directory;
  auto session = take(application::ProjectSession::create(directory.path() / "project"));
  const auto wav = make_wav(directory.path());
  const int descriptor = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(descriptor, 0, "WAV descriptor failed");
  auto imported = session->imports().import_wav_audio(descriptor);
  (void)::close(descriptor);
  REQUIRE_OK(imported, "source import failed");
  REQUIRE_EQ(session->snapshot().sources.size(), 1, "source import failed");
  const auto source = session->snapshot().sources.front();

  const auto first_revision = session->snapshot().revision;
  accepted(session->source_analysis().accept_source_tempo_analysis(
      source.id, first_revision, estimate_for(source, {0, 8, 16})));
  accepted(session->source_analysis().set_source_tempo_correction(source.id, 123.5));
  const auto stale_revision = session->snapshot().revision;
  accepted(session->source_analysis().set_source_user_beat_frames(source.id, {8, 20}));
  const auto before_rejected = session->snapshot().source_tempo_reviews.front();
  REQUIRE_TRUE(!session->source_analysis().accept_source_tempo_analysis(
                   source.id, stale_revision, estimate_for(source, {4, 12}, 99.0)),
               "stale reanalysis overwrote manual correction");
  REQUIRE_EQ(session->snapshot().source_tempo_reviews.front(), before_rejected,
             "stale reanalysis overwrote manual correction");
  accepted(session->history().undo());
  REQUIRE_TRUE(session->snapshot().source_tempo_reviews.front().user_beat_frames.empty(),
               "undo did not restore marker provenance");
  REQUIRE_EQ(session->snapshot().source_tempo_reviews.front().generated_beat_frames,
             std::vector<std::uint64_t>({0, 8, 16}), "undo did not restore marker provenance");
  accepted(session->history().redo());
  const auto& restored = session->snapshot().source_tempo_reviews.front();
  REQUIRE_EQ(restored.user_beat_frames, std::vector<std::uint64_t>({8, 20}),
             "redo lost reviewed markers or corrected BPM");
  REQUIRE_EQ(restored.generated_beat_frames, std::vector<std::uint64_t>({0, 16}),
             "redo lost reviewed markers or corrected BPM");
  REQUIRE_EQ(restored.corrected_bpm, 123.5, "redo lost reviewed markers or corrected BPM");

  accepted(session->source_analysis().accept_source_tempo_analysis(
      source.id, session->snapshot().revision, estimate_for(source, {8, 12, 24}, 98.0)));
  const auto expected = session->snapshot().source_tempo_reviews.front();
  REQUIRE_EQ(expected.detected_bpm, 98.0,
             "reanalysis replaced user review or duplicated a user marker");
  REQUIRE_EQ(expected.corrected_bpm, 123.5,
             "reanalysis replaced user review or duplicated a user marker");
  REQUIRE_EQ(expected.generated_beat_frames, std::vector<std::uint64_t>({12, 24}),
             "reanalysis replaced user review or duplicated a user marker");
  REQUIRE_EQ(expected.user_beat_frames, std::vector<std::uint64_t>({8, 20}),
             "reanalysis replaced user review or duplicated a user marker");
  accepted(session->saving().save());
  session.reset();

  auto reopened = take(application::ProjectSession::open(directory.path() / "project"));
  REQUIRE_EQ(reopened->snapshot().source_tempo_reviews, std::vector{expected},
             "review provenance or correction lost after save/reopen");
  auto reader = take(persistence::ProjectStore::open_reader(directory.path() / "project"));
  auto cache = take(media::CacheStore::adopt_directory_fd(take(reader.open_cache_directory())));
  auto first = take(application::load_tempo_suggestions(reader, &cache, source, 30));
  auto hit = take(application::load_tempo_suggestions(reader, &cache, source, 31));
  REQUIRE_TRUE(!first.cache_hit, "analysis cache did not populate and reuse");
  REQUIRE_TRUE(hit.cache_hit, "analysis cache did not populate and reuse");
  accepted(cache.clear());
  auto rebuilt = take(application::load_tempo_suggestions(reader, &cache, source, 32));
  REQUIRE_TRUE(!rebuilt.cache_hit, "cache clear/rebuild changed essential corrections");
  REQUIRE_EQ(reopened->snapshot().source_tempo_reviews.front(), expected,
             "cache clear/rebuild changed essential corrections");
  accepted(reopened->source_analysis().set_source_user_beat_frames(source.id, {20}));
  REQUIRE_EQ(reopened->snapshot().source_tempo_reviews.front().generated_beat_frames,
             std::vector<std::uint64_t>({12, 24}),
             "deleting a user marker silently restored a suppressed suggestion");
  accepted(reopened->source_analysis().accept_source_tempo_analysis(
      source.id, reopened->snapshot().revision, estimate_for(source, {8, 12, 24}, 98.0)));
  REQUIRE_EQ(reopened->snapshot().source_tempo_reviews.front().generated_beat_frames,
             std::vector<std::uint64_t>({8, 12, 24}),
             "explicit reanalysis did not recover a formerly suppressed suggestion");
}

void malformed_review_is_rejected_before_publication() {
  Directory directory;
  auto session = take(application::ProjectSession::create(directory.path() / "project"));
  const auto wav = make_wav(directory.path());
  const int descriptor = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(descriptor, 0, "WAV descriptor failed");
  REQUIRE_OK(session->imports().import_wav_audio(descriptor), "source import failed");
  (void)::close(descriptor);
  const auto source = session->snapshot().sources.front();
  accepted(session->source_analysis().accept_source_tempo_analysis(
      source.id, session->snapshot().revision, estimate_for(source, {0, 8, 16})));
  accepted(session->source_analysis().set_source_tempo_correction(source.id, 121.0));
  accepted(session->source_analysis().set_source_user_beat_frames(source.id, {20, 28}));
  const auto state = session->snapshot();
  const auto stable_revision = state.revision;
  REQUIRE_TRUE(!session->source_analysis().set_source_tempo_correction(source.id, -1.0),
               "invalid review command changed project revision");
  REQUIRE_TRUE(!session->source_analysis().set_source_user_beat_frames(source.id, {28, 20}),
               "invalid review command changed project revision");
  REQUIRE_EQ(session->snapshot().revision, stable_revision,
             "invalid review command changed project revision");
  auto wrong_key = estimate_for(source, {0, 8});
  wrong_key.identity.source_key = "wrong";
  require(!session->source_analysis().accept_source_tempo_analysis(source.id, stable_revision,
                                                                   wrong_key),
          "analysis with unrelated semantic cache key accepted");
  const auto manifest = persistence::detail::encode_manifest(
      state, std::string(32, 'b'), std::nullopt, persistence::SaveKind::explicit_save);
  for (const auto& bad :
       {replace_once(manifest, "\"corrected_bpm\":121.0", "\"corrected_bpm\":-1"),
        replace_once(manifest, "\"confidence\":0.8", "\"confidence\":2"),
        replace_once(manifest, "\"generated_beat_frames\":[0,8,16]",
                     "\"generated_beat_frames\":[8,0,16]"),
        replace_once(manifest, "\"user_beat_frames\":[20,28]", "\"user_beat_frames\":[20,48]"),
        replace_once(manifest, "\"user_beat_frames\":[20,28]", "\"user_beat_frames\":[8,20]"),
        replace_once(manifest, "\"suggestions_truncated\":false", "\"suggestions_truncated\":1"),
        replace_once(manifest, "\"source_tempo_reviews\":[",
                     "\"source_tempo_reviews\":[{\"unknown\":1},")}) {
    require(!persistence::detail::decode_manifest(bad), "malformed review manifest accepted");
  }
  REQUIRE_EQ(session->snapshot().revision, stable_revision,
             "malformed input changed live review state");
  REQUIRE_EQ(session->snapshot().source_tempo_reviews, state.source_tempo_reviews,
             "malformed input changed live review state");
  application::ControlTransportGate gate;
  auto model = take(project::ProjectModel::open(state, gate));
  auto removed = take(model.apply({project::RemoveSource{source.id}}));
  REQUIRE_TRUE(removed.snapshot->sources.empty(),
               "explicit source removal retained orphaned review state");
  REQUIRE_TRUE(removed.snapshot->source_tempo_reviews.empty(),
               "explicit source removal retained orphaned review state");
  auto restored = take(model.undo());
  REQUIRE_EQ(restored.snapshot->source_tempo_reviews, state.source_tempo_reviews,
             "undo of source removal lost essential review state");
}

} // namespace

int main() {
  try {
    review_survives_reanalysis_reopen_and_cache_clear();
    malformed_review_is_rejected_before_publication();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
