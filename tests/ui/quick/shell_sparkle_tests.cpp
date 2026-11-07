#include "../../../apps/daw_quick/src/command_palette.hpp"
#include "../../../apps/daw_quick/src/history_controller.hpp"
#include "../../../apps/daw_quick/src/master_section.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/wait.hpp"

#include <QCoreApplication>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>

// The shell's sparkle: the command palette's fuzzy ranking and recents, the
// history panel's jumps, and the master section's readings and mono switch.
namespace {
namespace application = oscillate::application;
namespace engine = oscillate::engine;
namespace project = oscillate::project;
namespace quick = oscillate::quick;
namespace fs = std::filesystem;

using oscillate::test::require;

class Directory {
public:
  Directory() {
    std::string pattern = (fs::temp_directory_path() / "oscillate-sparkle-XXXXXX").string();
    require(::mkdtemp(pattern.data()) != nullptr, "fixture directory creation failed");
    path_ = pattern;
  }
  ~Directory() {
    std::error_code ignored;
    fs::remove_all(path_, ignored);
  }
  [[nodiscard]] const fs::path& path() const { return path_; }

private:
  fs::path path_;
};

void fuzzy_matching_prefers_word_starts_and_runs() {
  require(quick::fuzzy_score("", "Anything") == 0, "an empty query matches with 0");
  require(!quick::fuzzy_score("xyz", "Add Pump"), "letters not in the text matched");
  require(!quick::fuzzy_score("pa", "Ap"), "letters out of order matched");
  require(quick::fuzzy_score("adpu", "Add Pump").has_value(), "a subsequence did not match");
  require(quick::fuzzy_score("ADD", "add pump").has_value(), "matching is not case-insensitive");
  require(quick::fuzzy_score("add pump", "Add Pump").has_value(), "spaces in a query count");
  // Word starts beat letters buried in a word.
  require(*quick::fuzzy_score("nit", "New instrument track") > *quick::fuzzy_score("nit", "Unit"),
          "initials did not beat a buried run");
  // A run beats scattered letters; an exact title beats a longer one.
  require(*quick::fuzzy_score("undo", "Undo") > *quick::fuzzy_score("undo", "Unlock door"),
          "an exact title did not win");
  require(*quick::fuzzy_score("pump", "Pump") > *quick::fuzzy_score("pump", "Pump Gate"),
          "the shorter exact title did not win");
  // An earlier match reads better than a later one.
  require(*quick::fuzzy_score("loop", "Loop on / off") > *quick::fuzzy_score("loop", "A loop"),
          "a later match beat an earlier one");
}

QVariantMap entry(const QString& key, const QString& title, bool enabled = true,
                  const QString& keywords = {}) {
  return {{"key", key}, {"title", title}, {"enabled", enabled}, {"keywords", keywords}};
}

QStringList keys(const QVariantList& results) {
  QStringList list;
  for (const auto& result : results)
    list.push_back(result.toMap().value("key").toString());
  return list;
}

void palette_ranks_recents_and_disabled_entries() {
  quick::CommandPalette palette;
  const QVariantList entries{entry("action:save", "Save"), entry("action:stop", "Stop"),
                             entry("action:split", "Split the clip", false),
                             entry("track:a", "Strings"),
                             entry("device:pump", "Pump", true, "sidechain duck kick")};
  // No query: the given order, limited.
  require(keys(palette.search(entries, "", 3)) ==
              QStringList({"action:save", "action:stop", "action:split"}),
          "an empty query did not keep the order");
  // A disabled entry ranks below every enabled match.
  auto results = palette.search(entries, "s", 10);
  require(keys(results).last() == "action:split", "a disabled entry outranked an enabled one");
  // Keywords match too.
  require(keys(palette.search(entries, "duck", 10)) == QStringList({"device:pump"}),
          "keywords did not match");
  // Recents come first with no query and win ties with one.
  palette.note_used("track:a");
  palette.note_used("action:stop");
  require(palette.recents().first() == "action:stop" && palette.recents().size() == 2,
          "recents are not newest first");
  require(keys(palette.search(entries, "", 2)) == QStringList({"action:stop", "track:a"}),
          "recents did not lead an empty query");
  results = palette.search(entries, "st", 10);
  require(keys(results).first() == "action:stop", "a recent did not win its match");
  // Recents survive a new palette (QSettings, here in a temporary home).
  quick::CommandPalette reopened;
  require(reopened.recents() == palette.recents(), "recents were not kept");
  for (int i = 0; i < quick::CommandPalette::max_recents + 5; ++i)
    palette.note_used(QStringLiteral("action:%1").arg(i));
  require(palette.recents().size() == quick::CommandPalette::max_recents,
          "recents grew without bound");
}

void history_jumps_in_one_commit() {
  Directory directory;
  auto opened = application::ProjectSession::create(directory.path() / "song");
  require(opened.has_value(), "project creation failed");
  auto session = std::move(opened.value());
  int commits = 0;
  QString status;
  quick::HistoryController history([&] { return session.get(); }, [] { return true; },
                                   [&](QString message) { status = std::move(message); },
                                   [&] { ++commits; });
  require(history.steps().isEmpty(), "an inactive panel described the history");
  const auto base = static_cast<int>(session->history().undo_steps().size());
  const auto track = session->instruments().add_synth_track();
  require(track.has_value(), "track failed");
  require(session->tracks().set_track_name(track.value(), "Lead").has_value() &&
              session->tracks().set_track_muted(track.value(), true).has_value(),
          "edits failed");
  history.set_active(true);
  require(history.steps().size() == base + 3 && history.position() == base + 3,
          "the panel did not list every step");
  require(history.steps().last().toMap().value("label").toString() == "Mute Lead",
          "the newest step is not last");

  history.jump_to(base);
  require(commits == 1 && session->history().redo_steps().size() == 3 &&
              history.position() == base && status == "Undid 3 steps",
          "a jump back was not one commit of three undos: " + status.toStdString());
  require(history.steps().size() == base + 3 &&
              history.steps().last().toMap().value("undone").toBool(),
          "undone steps left the list");
  const auto& tracks = session->snapshot().tracks;
  require(std::ranges::none_of(
              tracks, [&](const project::Track& item) { return item.id == track.value(); }),
          "the track survived the jump back");

  history.jump_to(base + 2);
  require(commits == 2 && history.position() == base + 2 && status == "Redid 2 steps",
          "a jump forward did not redo two steps");
  history.jump_to(base + 2);
  require(commits == 2, "a jump to where it is committed");
  // A new edit drops the redo steps from the list.
  require(session->tracks().set_track_soloed(track.value(), true).has_value(), "solo failed");
  history.refresh();
  require(history.steps().size() == base + 3 &&
              history.steps().last().toMap().value("label").toString() == "Solo Lead" &&
              !history.steps().last().toMap().value("undone").toBool(),
          "a new edit kept the old redo steps");

  quick::HistoryController refused([&] { return session.get(); }, [] { return false; },
                                   [](QString) {}, [&] { ++commits; });
  refused.jump_to(0);
  require(commits == 2 && session->history().redo_steps().empty(), "a refused jump still undid");
}

void master_section_reads_the_analysis() {
  auto analysis = std::make_shared<quick::LiveTruePeakMonitor>();
  auto mono = std::make_shared<std::atomic<bool>>(false);
  quick::MasterSection section(analysis, mono);
  require(!section.reading().value("valid").toBool() &&
              std::isnan(section.reading().value("integrated").toDouble()),
          "silence read as a level");
  section.set_mono_listen(true);
  require(mono->load() && section.mono_listen(), "the mono switch did not reach the output");
  section.set_mono_listen(false);
  require(!mono->load(), "the mono switch did not release");

  analysis->set_sample_rate_after_quiesce(48'000);
  std::array<engine::StereoFrame, 128> frames{};
  for (std::size_t n = 0; n < frames.size(); ++n)
    frames[n] = {0.5F, -0.5F};
  analysis->observe(frames,
                    engine::BlockContext{.project_sample_position = 0, .transport_generation = 1});
  oscillate::test::wait_until(
      [&] {
        section.poll();
        return section.reading().value("valid").toBool();
      },
      "the master section read the analysis");
  const auto reading = section.reading();
  require(reading.value("valid").toBool(), "the section never read the analysis");
  require(std::abs(reading.value("correlation").toDouble() + 1.0) < 1e-6,
          "an inverted pair did not read -1");
  require(std::abs(reading.value("truePeakDb").toDouble() - 20.0 * std::log10(0.5)) < 1.5,
          "true peak is not the block's level");
  // Loudness needs 400 ms; one block has none yet.
  require(std::isnan(reading.value("integrated").toDouble()), "loudness from one block");
}

} // namespace

int main(int argc, char** argv) {
  Directory home;
  // QSettings writes the palette's recents here, never in the user's home.
  ::setenv("XDG_CONFIG_HOME", home.path().c_str(), 1);
  QCoreApplication app(argc, argv);
  QCoreApplication::setOrganizationName("oscillate-tests");
  QCoreApplication::setApplicationName("shell-sparkle");
  fuzzy_matching_prefers_word_starts_and_runs();
  palette_ranks_recents_and_disabled_entries();
  history_jumps_in_one_commit();
  master_section_reads_the_analysis();
}
