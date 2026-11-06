#include "oscillate/application/project_collection.hpp"
#include "oscillate/application/project_session.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
using namespace oscillate;

using oscillate::test::require;

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-collect") {}
};

void collected_copy_preserves_identity_and_survives_missing_external_media() {
  Directory directory;
  const auto original = directory.path() / "external.wav";
  fs::copy_file(OSCILLATE_STARTER_PLUCK, original);
  const auto root = directory.path() / "original-project";
  auto session = application::ProjectSession::create(root);
  REQUIRE_OK(session, "source project creation failed");
  const int input = ::open(original.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(input, 0, "source audio open failed");
  const auto imported = session.value()->imports().import_wav_audio(input);
  ::close(input);
  REQUIRE_OK(imported, "source audio import failed");
  const auto second = directory.path() / "managed.wav";
  fs::copy_file(fs::path(OSCILLATE_STARTER_PLUCK).parent_path() / "kick.wav", second);
  const int second_input = ::open(second.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(second_input, 0, "second source audio open failed");
  const auto second_import = session.value()->imports().import_wav_audio(second_input);
  ::close(second_input);
  REQUIRE_OK(second_import, "second source audio import failed");
  auto linked = session.value()->snapshot();
  ++linked.revision;
  linked.sources.front().external_link = project::ExternalSourceLink{original.string()};
  const auto replacement = directory.path() / "replacement.wav";
  fs::copy_file(original, replacement);
  session.value().reset();
  {
    auto writer = persistence::ProjectStore::open_writer(root, false);
    REQUIRE_TRUE(writer, "linked fixture save failed");
    REQUIRE_TRUE(writer.value().save(linked, persistence::SaveKind::explicit_save),
                 "linked fixture save failed");
    REQUIRE_OK(writer.value().name_current_revision("Before bus"),
               "historical linked revision fixture failed");
    linked.tracks.pop_back();
    linked.sources.pop_back();
    linked.sources.front().external_link = project::ExternalSourceLink{replacement.string()};
    ++linked.revision;
    REQUIRE_OK(writer.value().save(linked, persistence::SaveKind::explicit_save),
               "historical-only source fixture failed");
  }
  session = application::ProjectSession::open(root);
  REQUIRE_TRUE(session, "current revision fixture failed");
  REQUIRE_TRUE(session.value()->tracks().add_bus_track(project::TrackKind::group_bus),
               "current revision fixture failed");
  REQUIRE_TRUE(session.value()->saving().save(), "current revision fixture failed");
  linked = session.value()->snapshot();
  session.value().reset();
  {
    auto writer = persistence::ProjectStore::open_writer(root, false);
    REQUIRE_TRUE(writer, "second historical revision fixture failed");
    REQUIRE_OK(writer.value().name_current_revision("After bus"),
               "second historical revision fixture failed");
  }
  fs::remove(original);

  require(!application::collect_and_save_copy(root, "relative-copy"),
          "relative collection destination was accepted");
  require(!application::collect_and_save_copy(directory.path() / "absent-project",
                                              directory.path() / "absent-copy"),
          "missing source project produced a collected copy");
  const auto blocked_parent = directory.path() / "parent-link";
  fs::create_symlink(directory.path(), blocked_parent);
  REQUIRE_TRUE(!application::collect_and_save_copy(root, blocked_parent / "copy"),
               "symlinked destination parent was followed");
  REQUIRE_TRUE(!fs::exists(directory.path() / "copy"), "symlinked destination parent was followed");
  require(!application::collect_and_save_copy(root, directory.path() / std::string(245, 'x')),
          "unusable staging name produced a collected project");
  const auto broken_root = directory.path() / "no-current-project";
  auto broken = application::ProjectSession::create(broken_root);
  REQUIRE_OK(broken, "broken-pointer fixture creation failed");
  broken.value().reset();
  fs::remove(broken_root / "state" / "CURRENT");
  require(!application::collect_and_save_copy(broken_root, directory.path() / "no-current-copy"),
          "project with no saved CURRENT produced a collected copy");

  const auto bad_destination = directory.path() / "damaged-copy";
  {
    std::fstream media(replacement, std::ios::binary | std::ios::in | std::ios::out);
    require(static_cast<bool>(media), "source audio mutation open failed");
    media.seekp(-1, std::ios::end);
    media.put('\x7f');
  }
  REQUIRE_TRUE(!application::collect_and_save_copy(root, bad_destination),
               "changed external bytes produced a collected project");
  REQUIRE_TRUE(!fs::exists(bad_destination), "changed external bytes produced a collected project");
  fs::copy_file(OSCILLATE_STARTER_PLUCK, replacement, fs::copy_options::overwrite_existing);

  const auto destination = directory.path() / "collected-project";
  REQUIRE_TRUE(!application::collect_and_save_copy(root, directory.path() / "stale-copy", {},
                                                   linked.revision - 1),
               "stale project revision produced a collected copy");
  REQUIRE_TRUE(!fs::exists(directory.path() / "stale-copy"),
               "stale project revision produced a collected copy");
  {
    std::ofstream malformed(root / "state" / "revisions" / "invalid-entry");
    require(static_cast<bool>(malformed), "malformed revision fixture creation failed");
  }
  REQUIRE_TRUE(!application::collect_and_save_copy(root, directory.path() / "lost-history-copy"),
               "damaged named history was silently dropped");
  REQUIRE_TRUE(!fs::exists(directory.path() / "lost-history-copy"),
               "damaged named history was silently dropped");
  fs::remove(root / "state" / "revisions" / "invalid-entry");
  std::stop_source mid_copy_cancel;
  const auto partial_destination = directory.path() / "partial-copy";
  REQUIRE_TRUE(!application::collect_and_save_copy(root, partial_destination,
                                                   mid_copy_cancel.get_token(), std::nullopt,
                                                   [&](application::CollectionProgress progress) {
                                                     if (progress.completed_sources == 1)
                                                       mid_copy_cancel.request_stop();
                                                   }),
               "cancelled partial media copy published a project");
  REQUIRE_TRUE(!fs::exists(partial_destination),
               "cancelled partial media copy published a project");
  const auto raced_destination = directory.path() / "raced-copy";
  bool destination_claimed = false;
  REQUIRE_TRUE(!application::collect_and_save_copy(
                   root, raced_destination, {}, std::nullopt,
                   [&](application::CollectionProgress update) {
                     if (update.completed_sources == 1 && !destination_claimed) {
                       destination_claimed = true;
                       fs::create_directory(raced_destination);
                       std::ofstream(raced_destination / "owner-marker") << "other owner";
                     }
                   }),
               "collection replaced a destination claimed during media copying");
  REQUIRE_TRUE(destination_claimed,
               "collection replaced a destination claimed during media copying");
  REQUIRE_TRUE(fs::is_regular_file(raced_destination / "owner-marker"),
               "collection replaced a destination claimed during media copying");
  REQUIRE_TRUE(!fs::exists(raced_destination / "state" / "CURRENT"),
               "collection replaced a destination claimed during media copying");
  const auto superseded_destination = directory.path() / "superseded-copy";
  bool source_advanced = false;
  REQUIRE_TRUE(
      !application::collect_and_save_copy(
          root, superseded_destination, {}, std::nullopt,
          [&](application::CollectionProgress update) {
            if (update.completed_sources == 1 && !source_advanced) {
              source_advanced = true;
              ++linked.revision;
              auto writer = persistence::ProjectStore::open_writer(root, false);
              require(writer && writer.value().save(linked, persistence::SaveKind::explicit_save),
                      "concurrent source-save fixture failed");
            }
          }),
      "collection published a copy after CURRENT changed mid-copy");
  REQUIRE_TRUE(source_advanced, "collection published a copy after CURRENT changed mid-copy");
  REQUIRE_TRUE(!fs::exists(superseded_destination),
               "collection published a copy after CURRENT changed mid-copy");
  std::vector<application::CollectionProgress> progress;
  require(application::collect_and_save_copy(
              root, destination, {}, std::nullopt,
              [&](application::CollectionProgress update) { progress.push_back(update); })
              .has_value(),
          "collection failed");
  REQUIRE_EQ(progress.size(), 3, "collection did not report retained media-source progress");
  REQUIRE_EQ(progress.front().completed_sources, 0,
             "collection did not report retained media-source progress");
  REQUIRE_EQ(progress.front().total_sources, 2,
             "collection did not report retained media-source progress");
  REQUIRE_EQ(progress.back().completed_sources, 2,
             "collection did not report retained media-source progress");
  REQUIRE_EQ(progress.back().total_sources, 2,
             "collection did not report retained media-source progress");
  require(!application::collect_and_save_copy(root, destination),
          "collection replaced an existing destination");
  fs::remove(replacement);
  auto copy = persistence::ProjectStore::open_reader(destination);
  REQUIRE_OK(copy, "collected project did not open");
  auto loaded = copy.value().load_current();
  REQUIRE_OK(loaded, "collected project has no valid saved generation");
  const auto& saved = loaded.value().snapshot;
  REQUIRE_EQ(saved.revision, linked.revision + 1,
             "collection changed source identity or project edits");
  REQUIRE_EQ(saved.tracks, linked.tracks, "collection changed source identity or project edits");
  REQUIRE_EQ(saved.sources.size(), linked.sources.size(),
             "collection changed source identity or project edits");
  REQUIRE_EQ(saved.sources.front().id, linked.sources.front().id,
             "collection changed source identity or project edits");
  REQUIRE_EQ(saved.sources.front().sha256, linked.sources.front().sha256,
             "collection changed source identity or project edits");
  REQUIRE_TRUE(
      std::ranges::none_of(saved.sources,
                           [](const auto& source) { return source.external_link.has_value(); }),
      "collection changed source identity or project edits");
  for (const auto& source : saved.sources) {
    auto pinned = copy.value().open_verified_source(source);
    REQUIRE_OK(pinned, "collected media requires an original source file");
    ::close(pinned.value());
  }
  auto named = copy.value().list_named_revisions();
  REQUIRE_TRUE(named, "collection dropped named revision history");
  REQUIRE_TRUE(named.value().issues.empty(), "collection dropped named revision history");
  REQUIRE_EQ(named.value().revisions.size(), 2, "collection dropped named revision history");
  auto before_bus = copy.value().load_named_revision("Before bus");
  auto after_bus = copy.value().load_named_revision("After bus");
  REQUIRE_TRUE(before_bus, "collected named revision lost edits or retained external links");
  REQUIRE_TRUE(after_bus, "collected named revision lost edits or retained external links");
  REQUIRE_NE(before_bus.value().snapshot.tracks, saved.tracks,
             "collected named revision lost edits or retained external links");
  REQUIRE_EQ(before_bus.value().snapshot.sources.size(), saved.sources.size() + 1,
             "collected named revision lost edits or retained external links");
  REQUIRE_EQ(after_bus.value().snapshot.tracks, saved.tracks,
             "collected named revision lost edits or retained external links");
  REQUIRE_TRUE(
      std::ranges::none_of(before_bus.value().snapshot.sources,
                           [](const auto& source) { return source.external_link.has_value(); }),
      "collected named revision lost edits or retained external links");
  for (const auto& source : before_bus.value().snapshot.sources) {
    auto pinned =
        copy.value().open_verified_generation_source(before_bus.value().generation, source);
    REQUIRE_OK(pinned, "historical collected media needs its external source");
    ::close(pinned.value());
  }
  require(!copy.value().open_verified_generation_source(after_bus.value().generation,
                                                        before_bus.value().snapshot.sources.back()),
          "historical source opened from a generation that does not reference it");
  auto original_reader = persistence::ProjectStore::open_reader(root);
  REQUIRE_TRUE(original_reader, "collection changed the original saved links");
  REQUIRE_EQ(original_reader.value().load_current().value().snapshot.sources, linked.sources,
             "collection changed the original saved links");
  auto reopened_copy = application::ProjectSession::open(destination);
  REQUIRE_TRUE(reopened_copy, "collected historical project could not be restored and saved");
  REQUIRE_TRUE(reopened_copy.value()->saving().restore_named_revision("Before bus"),
               "collected historical project could not be restored and saved");
  REQUIRE_EQ(reopened_copy.value()->snapshot().sources.size(), 2,
             "collected historical project could not be restored and saved");
  REQUIRE_TRUE(reopened_copy.value()->saving().save(),
               "collected historical project could not be restored and saved");
  reopened_copy.value().reset();
  reopened_copy = application::ProjectSession::open(destination);
  REQUIRE_TRUE(reopened_copy, "restored collected history did not reopen");
  REQUIRE_EQ(reopened_copy.value()->snapshot().sources.size(), 2,
             "restored collected history did not reopen");
  require(!application::collect_and_save_copy(root, directory.path() / "missing-copy"),
          "missing external source produced a collected project");
  std::stop_source cancel;
  cancel.request_stop();
  require(!application::collect_and_save_copy(root, directory.path() / "cancelled-copy",
                                              cancel.get_token()),
          "cancelled collection produced a project");
}

} // namespace

int main() {
  try {
    collected_copy_preserves_identity_and_survives_missing_external_media();
    std::cout << "Project collection contract passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
