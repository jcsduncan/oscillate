#include "../../../apps/daw_quick/src/key_match_controller.hpp"
#include "../../../apps/daw_quick/src/key_review_controller.hpp"
#include "oscillate/test/check.hpp"

#include <QColor>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QSignalSpy>
#include <QTest>

#include <atomic>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
using namespace oscillate;
namespace fs = std::filesystem;

using oscillate::test::require;
// How long a key-analysis worker may take to report back.
constexpr int worker_wait_ms = 3000;
// The step a test waits in while a worker runs.
constexpr int wait_step_ms = 1;
// The song key smart drops are offered: D major.
constexpr project::MusicalKey song_key{2, project::KeyMode::major};
template <typename T> T take(foundation::Result<T> result) {
  if (!result) {
    throw std::runtime_error(result.error().message);
  }
  return std::move(result).value();
}

class Directory {
public:
  Directory() {
    auto pattern = (fs::temp_directory_path() / "oscillate-key-quick-XXXXXX").string();
    require(::mkdtemp(pattern.data()) != nullptr, "temporary project directory failed");
    path_ = pattern;
  }
  ~Directory() {
    std::error_code ignored;
    fs::remove_all(path_, ignored);
  }
  const fs::path& path() const { return path_; }

private:
  fs::path path_;
};

struct Fixture {
  Directory directory;
  fs::path root = directory.path() / "project";
  std::unique_ptr<application::ProjectSession> session;
  project::SourceRecord source;
  QString clip_id;

  static project::SourceRecord import_source(application::ProjectSession& session) {
    const int fd = ::open(OSCILLATE_STARTER_PLUCK, O_RDONLY | O_CLOEXEC);
    require(fd >= 0, "starter source could not be opened");
    const auto imported = session.imports().import_wav_audio(fd);
    ::close(fd);
    require(imported.has_value(), "managed starter import failed");
    return session.snapshot().sources.front();
  }
  Fixture()
      : session(take(application::ProjectSession::create(root))), source(import_source(*session)) {
    clip_id =
        QString::fromStdString(session->snapshot().tracks.front().clips.front().id.to_string());
  }
};

application::KeyCacheResult suggestion(const project::SourceRecord& source,
                                       std::uint64_t generation, bool second) {
  const auto key = take(application::key_cache_key(source));
  application::KeyCacheResult result;
  result.estimate.identity = {key.hex(), generation};
  result.estimate.sample_rate = source.sample_rate;
  result.estimate.frames = source.frames;
  result.estimate.key = second ? analysis::MusicalKey{9, analysis::KeyMode::minor}
                               : analysis::MusicalKey{0, analysis::KeyMode::major};
  result.estimate.confidence = 0.72;
  result.estimate.compatible_keys =
      second ? std::vector<analysis::CompatibleKey>{{{0, analysis::KeyMode::major},
                                                     analysis::KeyRelation::relative}}
             : std::vector<analysis::CompatibleKey>{
                   {{9, analysis::KeyMode::minor}, analysis::KeyRelation::relative},
                   {{7, analysis::KeyMode::major}, analysis::KeyRelation::dominant}};
  return result;
}

void wait_for_commit(QSignalSpy& spy) {
  if (spy.isEmpty()) {
    require(spy.wait(worker_wait_ms), "key-analysis worker did not commit within 3 seconds");
  }
}

void review_panel_and_reopen() {
  Fixture fixture;
  std::atomic<int> calls{0};
  quick::KeyReviewController review(
      [&](const auto&, const project::SourceRecord& source, std::uint64_t generation, const auto&) {
        const bool second = calls.fetch_add(1) != 0;
        return foundation::Result<application::KeyCacheResult>::success(
            suggestion(source, generation, second));
      });
  review.set_project(fixture.session.get(), fixture.root);
  QSignalSpy commits(&review, &quick::KeyReviewController::projectRevisionChanged);
  review.select_clip(fixture.clip_id, fixture.source);
  wait_for_commit(commits);
  require(review.preview().value("detected").toString() == QStringLiteral("C major") &&
              review.preview().value("compatible").toList().size() == 2 &&
              review.preview().value("confidence").toDouble() == 0.72,
          "generated key projection lost label, relation or confidence");

  QQmlEngine engine;
  QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/KeyReviewPanel.qml")));
  if (!component.isReady()) {
    throw std::runtime_error("key panel QML did not load: " +
                             component.errorString().toStdString());
  }
  std::unique_ptr<QObject> panel(component.createWithInitialProperties(
      {{"review", QVariant::fromValue(static_cast<QObject*>(&review))},
       {"textMain", QColor("#ffffff")},
       {"textDim", QColor("#aaaaaa")},
       {"accent", QColor("#72e3c0")},
       {"amber", QColor("#ecbc76")},
       {"lineColor", QColor("#2a313d")}}));
  require(panel != nullptr, "key panel creation failed");
  auto* generated = panel->findChild<QObject*>(QStringLiteral("keyGeneratedValue"));
  auto* correction = panel->findChild<QObject*>(QStringLiteral("keyCorrectionValue"));
  auto* compatible = panel->findChild<QObject*>(QStringLiteral("keyCompatibleValues"));
  require(generated && correction && compatible &&
              generated->property("text").toString().contains(QStringLiteral("C major")) &&
              compatible->property("text").toString().contains(QStringLiteral("Relative: A minor")),
          "offscreen key review panel did not show generated suggestion");

  review.set_correction(2, 1);
  require(review.preview().value("corrected").toString() == QStringLiteral("D minor") &&
              correction->property("text").toString().contains(QStringLiteral("D minor")),
          "manual correction did not reach QML");
  require(fixture.session->saving().save().has_value(), "key correction save failed");
  review.set_project(nullptr, {});
  fixture.session.reset();
  fixture.session = take(application::ProjectSession::open(fixture.root));
  const auto reopened_revision = fixture.session->snapshot().revision;
  review.set_project(fixture.session.get(), fixture.root);
  review.select_clip(fixture.clip_id, fixture.source);
  require(calls.load() == 1 && fixture.session->snapshot().revision == reopened_revision &&
              review.preview().value("corrected").toString() == QStringLiteral("D minor"),
          "selecting a reviewed clip reanalyzed or lost the saved correction");
  commits.clear();
  review.reanalyze();
  wait_for_commit(commits);
  require(review.preview().value("detected").toString() == QStringLiteral("A minor") &&
              review.preview().value("corrected").toString() == QStringLiteral("D minor"),
          "explicit reanalysis overwrote manual correction");
}

void deselection_cancels_late_result() {
  Fixture fixture;
  std::binary_semaphore entered{0};
  std::binary_semaphore release{0};
  std::binary_semaphore exited{0};
  std::atomic<bool> cancelled{false};
  quick::KeyReviewController review([&](const auto&, const project::SourceRecord& source,
                                        std::uint64_t generation, const auto& stop) {
    entered.release();
    release.acquire();
    cancelled.store(stop(), std::memory_order_release);
    exited.release();
    return foundation::Result<application::KeyCacheResult>::success(
        suggestion(source, generation, false));
  });
  review.set_project(fixture.session.get(), fixture.root);
  const auto revision = fixture.session->snapshot().revision;
  review.select_clip(fixture.clip_id, fixture.source);
  entered.acquire();
  review.select_clip({}, std::nullopt);
  release.release();
  exited.acquire();
  QCoreApplication::processEvents();
  require(cancelled.load(std::memory_order_acquire) &&
              fixture.session->snapshot().revision == revision &&
              fixture.session->snapshot().source_key_reviews.empty() &&
              review.preview().value("state").toString() == QStringLiteral("idle"),
          "deselected worker result changed project or review panel");
}

void project_revision_cancels_late_result() {
  Fixture fixture;
  std::binary_semaphore entered{0};
  std::binary_semaphore release{0};
  std::binary_semaphore exited{0};
  quick::KeyReviewController review(
      [&](const auto&, const project::SourceRecord& source, std::uint64_t generation, const auto&) {
        entered.release();
        release.acquire();
        exited.release();
        return foundation::Result<application::KeyCacheResult>::success(
            suggestion(source, generation, false));
      });
  review.set_project(fixture.session.get(), fixture.root);
  review.select_clip(fixture.clip_id, fixture.source);
  entered.acquire();
  require(fixture.session->source_analysis()
              .set_source_key_correction(fixture.source.id,
                                         project::MusicalKey{2, project::KeyMode::minor})
              .has_value(),
          "manual correction during key analysis failed");
  const auto revised = fixture.session->snapshot().revision;
  review.refresh_snapshot();
  release.release();
  exited.acquire();
  QCoreApplication::processEvents();
  const auto& reviews = fixture.session->snapshot().source_key_reviews;
  require(fixture.session->snapshot().revision == revised && reviews.size() == 1 &&
              !reviews.front().detected &&
              reviews.front().correction == project::MusicalKey{2, project::KeyMode::minor} &&
              review.preview().value("state").toString() == QStringLiteral("stale"),
          "stale worker result replaced a newer manual correction");
}
// A clip's key analysis that finishes after its session was replaced, here
// by an on-disk copy of the same project with the same source and clip
// IDs, is dropped: neither stored in the copy nor offered there. A clip still
// waiting for analysis is not analysed in the copy either.
void key_match_drops_a_replaced_sessions_result() {
  Fixture fixture;
  (void)Fixture::import_source(*fixture.session);
  std::vector<project::ClipId> clips;
  for (const auto& track : fixture.session->snapshot().tracks)
    for (const auto& clip : track.clips)
      clips.push_back(clip.id);
  require(clips.size() == 2, "the second clip was not imported");
  require(fixture.session->song().set_project_key(song_key).has_value(), "song key");
  require(fixture.session->saving().save().has_value(), "save");
  const auto copy_root = fixture.directory.path() / "copy";
  fs::copy(fixture.root, copy_root, fs::copy_options::recursive);
  auto copy = take(application::ProjectSession::open(copy_root));
  const auto copy_revision = copy->snapshot().revision;
  application::ProjectSession* current = fixture.session.get();
  fs::path current_root = fixture.root;
  std::binary_semaphore entered{0};
  std::binary_semaphore release{0};
  std::atomic<int> analyses{0};
  int commits = 0;
  quick::KeyMatchController match(
      [&] { return current; }, [&] { return current_root; }, [] { return true; }, [](QString) {},
      [&] { ++commits; },
      [&](const auto&, const project::SourceRecord& source, std::uint64_t generation, const auto&) {
        if (analyses.fetch_add(1) == 0) {
          entered.release();
          release.acquire();
        }
        return foundation::Result<application::KeyCacheResult>::success(
            suggestion(source, generation, false));
      });
  match.dropped(clips.front());
  require(match.busy(), "the drop did not start key analysis");
  entered.acquire();
  match.dropped(clips.back());
  current = copy.get();
  current_root = copy_root;
  release.release();
  QElapsedTimer waited;
  waited.start();
  while (match.busy() && waited.elapsed() < worker_wait_ms)
    QTest::qWait(wait_step_ms);
  require(!match.busy(), "key analysis did not finish");
  require(commits == 0 && copy->snapshot().revision == copy_revision &&
              copy->snapshot().source_key_reviews.empty() && match.hint().isEmpty(),
          "a replaced session's key analysis landed on the copy that replaced it");
  require(analyses.load() == 1, "a clip queued in the replaced session was analysed in the copy");
}
// A dropped clip whose analysis finds no key is offered nothing and is not
// analysed again: re-queueing it stored the same result in a new revision
// every poll, for as long as the clip existed.
void key_match_stops_after_a_keyless_result() {
  Fixture fixture;
  require(fixture.session->song().set_project_key(song_key).has_value(), "song key");
  const auto clip = fixture.session->snapshot().tracks.front().clips.front().id;
  const auto before = fixture.session->snapshot().revision;
  std::atomic<int> analyses{0};
  int commits = 0;
  quick::KeyMatchController match(
      [&] { return fixture.session.get(); }, [&] { return fixture.root; }, [] { return true; },
      [](const QString&) {}, [&] { ++commits; },
      [&](const auto&, const project::SourceRecord& source, std::uint64_t generation, const auto&) {
        analyses.fetch_add(1);
        auto keyless = suggestion(source, generation, false);
        keyless.estimate.key.reset();
        keyless.estimate.confidence = 0.0;
        keyless.estimate.compatible_keys.clear();
        return foundation::Result<application::KeyCacheResult>::success(std::move(keyless));
      });
  match.dropped(clip);
  require(match.busy(), "the drop did not start key analysis");
  QElapsedTimer waited;
  waited.start();
  while (match.busy() && waited.elapsed() < worker_wait_ms)
    QTest::qWait(wait_step_ms);
  require(!match.busy(), "a keyless clip was analysed again and again");
  require(analyses.load() == 1 && commits == 1 &&
              fixture.session->snapshot().revision == before + 1 && match.hint().isEmpty(),
          "a keyless clip's analysis was not stored exactly once");
}
} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    review_panel_and_reopen();
    deselection_cancels_late_result();
    project_revision_cancels_late_result();
    key_match_drops_a_replaced_sessions_result();
    key_match_stops_after_a_keyless_result();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
