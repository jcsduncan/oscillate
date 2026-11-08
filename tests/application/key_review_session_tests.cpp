#include "oscillate/application/key_cache.hpp"
#include "oscillate/application/project_session.hpp"
#include "oscillate/media/cache_store.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
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
  Directory() : TempDir("oscillate-key-session") {}
};

analysis::KeyEstimate estimate_for(const project::SourceRecord& source,
                                   analysis::MusicalKey detected, std::uint64_t generation = 1) {
  return {.identity = {take(application::key_cache_key(source)).hex(), generation},
          .sample_rate = source.sample_rate,
          .frames = source.frames,
          .key = detected,
          .confidence = 0.72,
          .compatible_keys =
              detected.mode == analysis::KeyMode::major
                  ? std::vector<analysis::CompatibleKey>{{{static_cast<std::uint8_t>(
                                                               (detected.tonic + 9) % 12),
                                                           analysis::KeyMode::minor},
                                                          analysis::KeyRelation::relative},
                                                         {{static_cast<std::uint8_t>(
                                                               (detected.tonic + 7) % 12),
                                                           analysis::KeyMode::major},
                                                          analysis::KeyRelation::dominant}}
                  : std::vector<analysis::CompatibleKey>{
                        {{static_cast<std::uint8_t>((detected.tonic + 3) % 12),
                          analysis::KeyMode::major},
                         analysis::KeyRelation::relative},
                        {{static_cast<std::uint8_t>((detected.tonic + 7) % 12),
                          analysis::KeyMode::minor},
                         analysis::KeyRelation::dominant}}};
}

void review_contract() {
  Directory directory;
  const auto root = directory.path() / "project";
  auto session = take(application::ProjectSession::create(root));
  const int descriptor = ::open(OSCILLATE_STARTER_PLUCK, O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(descriptor, 0, "starter source could not be opened");
  auto imported = session->imports().import_wav_audio(descriptor);
  ::close(descriptor);
  REQUIRE_OK(imported, "managed source import failed");
  REQUIRE_EQ(session->snapshot().sources.size(), 1, "managed source import failed");
  const auto source = session->snapshot().sources.front();
  const auto first_revision = session->snapshot().revision;
  const auto first = estimate_for(source, {0, analysis::KeyMode::major});
  accepted(session->source_analysis().accept_source_key_analysis(source.id, first_revision, first));
  accepted(session->source_analysis().set_source_key_correction(
      source.id, project::MusicalKey{2, project::KeyMode::minor}));
  auto original_review = session->snapshot().source_key_reviews.front();
  REQUIRE_EQ(original_review.detected, (project::MusicalKey{0, project::KeyMode::major}),
             "worker key or manual correction was mapped incorrectly");
  REQUIRE_EQ(original_review.correction, (project::MusicalKey{2, project::KeyMode::minor}),
             "worker key or manual correction was mapped incorrectly");
  REQUIRE_EQ(original_review.compatible.front().relation, project::KeyRelation::relative,
             "worker key or manual correction was mapped incorrectly");

  const auto stable_revision = session->snapshot().revision;
  auto wrong = first;
  wrong.identity.source_key = "unrelated";
  require(!session->source_analysis().accept_source_key_analysis(source.id, stable_revision, wrong),
          "unrelated cache identity accepted");
  wrong = first;
  wrong.identity.generation = 0;
  require(!session->source_analysis().accept_source_key_analysis(source.id, stable_revision, wrong),
          "zero worker generation accepted");
  wrong = first;
  wrong.sample_rate = oscillate::test::sample_rate_44k;
  require(!session->source_analysis().accept_source_key_analysis(source.id, stable_revision, wrong),
          "wrong source rate accepted");
  wrong = first;
  wrong.frames += 1;
  require(!session->source_analysis().accept_source_key_analysis(source.id, stable_revision, wrong),
          "wrong source frames accepted");
  wrong = first;
  wrong.compatible_keys.front().relation = analysis::KeyRelation::dominant;
  require(!session->source_analysis().accept_source_key_analysis(source.id, stable_revision, wrong),
          "wrong compatible-key relationship accepted");
  require(!session->source_analysis().accept_source_key_analysis(source.id, first_revision, first),
          "stale project revision accepted");
  require(!session->source_analysis().set_source_key_correction(
              source.id, project::MusicalKey{12, project::KeyMode::minor}),
          "invalid manual tonic accepted");
  REQUIRE_EQ(session->snapshot().revision, stable_revision,
             "rejected worker or manual edit mutated review state");
  REQUIRE_EQ(session->snapshot().source_key_reviews.front(), original_review,
             "rejected worker or manual edit mutated review state");

  accepted(session->source_analysis().accept_source_key_analysis(
      source.id, stable_revision, estimate_for(source, {9, analysis::KeyMode::minor}, 2)));
  const auto regenerated = session->snapshot().source_key_reviews.front();
  REQUIRE_EQ(regenerated.detected, (project::MusicalKey{9, project::KeyMode::minor}),
             "reanalysis overwrote manual correction");
  REQUIRE_EQ(regenerated.correction, original_review.correction,
             "reanalysis overwrote manual correction");
  const auto regenerated_revision = session->snapshot().revision;
  accepted(session->source_analysis().accept_source_key_analysis(
      source.id, regenerated_revision, estimate_for(source, {9, analysis::KeyMode::minor}, 2)));
  REQUIRE_EQ(session->snapshot().revision, regenerated_revision,
             "the same key result again made a new revision");
  accepted(session->history().undo());
  REQUIRE_EQ(session->snapshot().source_key_reviews.front(), original_review,
             "session undo lost prior generated key");
  accepted(session->history().redo());
  REQUIRE_EQ(session->snapshot().source_key_reviews.front(), regenerated,
             "session redo lost regenerated suggestion");
  accepted(session->saving().save());
  session.reset();
  auto reopened = take(application::ProjectSession::open(root));
  REQUIRE_EQ(reopened->snapshot().source_key_reviews, std::vector{regenerated},
             "key review or correction lost after save/reopen");

  auto reader = take(persistence::ProjectStore::open_reader(root));
  auto cache = take(media::CacheStore::adopt_directory_fd(take(reader.open_cache_directory())));
  auto analyzed = take(application::load_key_suggestion(reader, &cache, source, 3));
  require(!analyzed.estimate.key, "sparse starter fixture unexpectedly detected a key");
  accepted(cache.clear());
  REQUIRE_EQ(reopened->snapshot().source_key_reviews.front(), regenerated,
             "disposable cache clear changed durable correction");
  accepted(reopened->source_analysis().accept_source_key_analysis(
      source.id, reopened->snapshot().revision, analyzed.estimate));
  REQUIRE_TRUE(!reopened->snapshot().source_key_reviews.front().detected,
               "explicit unknown analysis erased manual correction");
  REQUIRE_EQ(reopened->snapshot().source_key_reviews.front().correction, regenerated.correction,
             "explicit unknown analysis erased manual correction");
}
} // namespace

int main() {
  try {
    review_contract();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
