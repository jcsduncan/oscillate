#include "oscillate/application/project_collection.hpp"

#include "oscillate/foundation/file_io.hpp"
#include "oscillate/persistence/store.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <map>
#include <ranges>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace oscillate::application {
namespace {

using foundation::ErrorCode;
using foundation::Result;

struct TemporaryProject {
  std::filesystem::path path;
  bool published = false;
  ~TemporaryProject() {
    if (!published && !path.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
    }
  }
};

Result<void> cancelled() {
  return Result<void>::failure({ErrorCode::cancelled, "Project collection cancelled"});
}

} // namespace

Result<void> collect_and_save_copy(const std::filesystem::path& source_root,
                                   const std::filesystem::path& destination_root,
                                   const std::stop_token& stop,
                                   std::optional<std::uint64_t> expected_revision,
                                   const std::function<void(CollectionProgress)>& progress) {
  if (!source_root.is_absolute() || !destination_root.is_absolute() ||
      destination_root.filename().empty() || destination_root.filename() == "." ||
      destination_root.filename() == ".." || source_root == destination_root) {
    return Result<void>::failure(
        {ErrorCode::invalid_argument, "Collection needs a distinct absolute destination"});
  }
  {
    // Not inside the source either: staging there would sit in the project's
    // own state and fail its final consistency check.
    std::error_code error;
    const auto source = std::filesystem::weakly_canonical(source_root, error);
    const auto destination = error ? std::filesystem::path{}
                                   : std::filesystem::weakly_canonical(destination_root, error);
    if (error)
      return Result<void>::failure({ErrorCode::io_failure, "Cannot resolve the collection paths"});
    const auto [inside, rest] = std::ranges::mismatch(source, destination);
    if (inside == source.end())
      return Result<void>::failure(
          {ErrorCode::invalid_argument, "Collection destination is inside the source project"});
  }
  if (stop.stop_requested())
    return cancelled();

  auto reader = persistence::ProjectStore::open_reader(source_root);
  if (!reader)
    return Result<void>::failure(reader.error());
  auto loaded = reader.value().load_current();
  if (!loaded)
    return Result<void>::failure(loaded.error());
  const auto current_generation = loaded.value().generation;
  auto snapshot = std::move(loaded).value().snapshot;
  if (expected_revision && snapshot.revision != *expected_revision) {
    return Result<void>::failure(
        {ErrorCode::conflict, "Saved project changed before collection began"});
  }
  if (snapshot.revision == std::numeric_limits<std::uint64_t>::max()) {
    return Result<void>::failure(
        {ErrorCode::resource_exhausted, "Project revision space exhausted"});
  }
  auto revision_listing = reader.value().list_named_revisions();
  if (!revision_listing)
    return Result<void>::failure(revision_listing.error());
  if (!revision_listing.value().issues.empty())
    return Result<void>::failure(
        {ErrorCode::corrupt_data, "Named revision history needs repair before collection"});
  const auto source_revisions = revision_listing.value().revisions;
  struct HistoryEntry {
    std::string name;
    project::Snapshot snapshot;
  };
  std::vector<HistoryEntry> history;
  history.reserve(source_revisions.size());
  struct SourceCandidate {
    project::SourceRecord record;
    std::string generation;
  };
  std::map<project::SourceId, SourceCandidate> sources;
  auto collect_sources = [&](const project::Snapshot& state,
                             const std::string& generation) -> Result<void> {
    for (const auto& source : state.sources) {
      auto [item, inserted] = sources.try_emplace(source.id, SourceCandidate{source, generation});
      if (!inserted) {
        const auto& previous = item->second.record;
        if (previous.id != source.id || previous.encoding != source.encoding ||
            previous.sha256 != source.sha256 || previous.byte_length != source.byte_length ||
            previous.sample_rate != source.sample_rate || previous.channels != source.channels ||
            previous.frames != source.frames)
          return Result<void>::failure(
              {ErrorCode::corrupt_data, "Source identity differs across saved revisions"});
        if (!source.external_link || item->second.record.external_link)
          item->second = SourceCandidate{source, generation};
      }
    }
    return Result<void>::success();
  };
  for (const auto& named : source_revisions) {
    if (stop.stop_requested())
      return cancelled();
    auto revision = reader.value().load_named_revision(named.name);
    if (!revision)
      return Result<void>::failure(revision.error());
    if (revision.value().generation != named.generation ||
        revision.value().snapshot.id != snapshot.id ||
        revision.value().snapshot.revision > snapshot.revision)
      return Result<void>::failure(
          {ErrorCode::conflict, "Named revision changed or exceeds the current project"});
    auto added = collect_sources(revision.value().snapshot, revision.value().generation);
    if (!added)
      return added;
    history.push_back({named.name, std::move(revision).value().snapshot});
  }
  auto current_sources = collect_sources(snapshot, current_generation);
  if (!current_sources)
    return current_sources;
  std::ranges::sort(history, [](const auto& left, const auto& right) {
    if (left.snapshot.revision != right.snapshot.revision)
      return left.snapshot.revision < right.snapshot.revision;
    return left.name < right.name;
  });

  const auto parent = destination_root.parent_path();
  foundation::UniqueFd parent_fd(
      ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (parent_fd.get() < 0)
    return Result<void>::failure({ErrorCode::io_failure, "Collection parent is unavailable"});
  const auto name = destination_root.filename().string();
  struct stat existing{};
  if (::fstatat(parent_fd.get(), name.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0)
    return Result<void>::failure({ErrorCode::conflict, "Collection destination already exists"});
  if (errno != ENOENT)
    return Result<void>::failure({ErrorCode::io_failure, "Cannot inspect collection destination"});

  auto template_path = (parent / ("." + name + ".collect-XXXXXX")).string();
  std::vector<char> template_bytes(template_path.begin(), template_path.end());
  template_bytes.push_back('\0');
  if (::mkdtemp(template_bytes.data()) == nullptr)
    return Result<void>::failure({ErrorCode::io_failure, "Cannot stage collected project"});
  TemporaryProject temporary{std::filesystem::path(template_bytes.data())};
  auto writer = persistence::ProjectStore::open_writer(temporary.path, true);
  if (!writer)
    return Result<void>::failure(writer.error());

  std::vector<persistence::SourceReceipt> receipts;
  receipts.reserve(sources.size());
  if (progress)
    progress({0, sources.size()});
  for (const auto& item : sources) {
    if (stop.stop_requested())
      return cancelled();
    const auto& candidate = item.second;
    const auto& source = candidate.record;
    auto opened =
        reader.value().open_verified_generation_source(candidate.generation, source, stop);
    if (!opened)
      return Result<void>::failure(opened.error());
    const foundation::UniqueFd input(opened.value());
    auto stage = source.encoding == project::SourceEncoding::wav
                     ? writer.value().begin_wav_stage(source.id, input.get())
                     : writer.value().begin_encoded_stage(source.id, input.get());
    if (!stage)
      return Result<void>::failure(stage.error());
    auto prepared = std::move(stage).value().run(stop);
    if (!prepared)
      return Result<void>::failure(prepared.error());
    auto expected = source;
    expected.external_link.reset();
    if (prepared.value().record() != expected) {
      return Result<void>::failure(
          {ErrorCode::corrupt_data, "Collected media differs from saved source identity"});
    }
    auto published = source.encoding == project::SourceEncoding::wav
                         ? writer.value().publish_wav_stage(std::move(prepared).value())
                         : writer.value().publish_encoded_stage(std::move(prepared).value());
    if (!published)
      return Result<void>::failure(published.error());
    receipts.push_back(std::move(published).value());
    if (progress)
      progress({receipts.size(), sources.size()});
  }
  auto make_managed = [](project::Snapshot& state) {
    for (auto& source : state.sources)
      source.external_link.reset();
  };
  for (auto& named : history) {
    if (stop.stop_requested())
      return cancelled();
    make_managed(named.snapshot);
    auto saved_history =
        writer.value().save(named.snapshot, persistence::SaveKind::explicit_save, receipts);
    if (!saved_history)
      return Result<void>::failure(saved_history.error());
    if (saved_history.value().status != persistence::SaveStatus::committed)
      return Result<void>::failure(
          {ErrorCode::io_failure, "Collected revision save durability is uncertain"});
    auto named_result = writer.value().name_current_revision(named.name);
    if (!named_result)
      return Result<void>::failure(named_result.error());
    if (named_result.value().status != persistence::SaveStatus::committed)
      return Result<void>::failure(
          {ErrorCode::io_failure, "Collected revision pointer durability is uncertain"});
  }
  make_managed(snapshot);
  ++snapshot.revision;
  auto saved = writer.value().save(snapshot, persistence::SaveKind::explicit_save, receipts);
  if (!saved)
    return Result<void>::failure(saved.error());
  if (saved.value().status != persistence::SaveStatus::committed) {
    return Result<void>::failure(
        {ErrorCode::io_failure, "Collected project save durability is uncertain"});
  }
  if (stop.stop_requested())
    return cancelled();

  auto still_current = reader.value().load_current();
  auto still_named = reader.value().list_named_revisions();
  if (!still_current || !still_named || still_current.value().generation != current_generation ||
      !still_named.value().issues.empty() ||
      still_named.value().revisions.size() != source_revisions.size() ||
      !std::ranges::equal(still_named.value().revisions, source_revisions,
                          [](const auto& left, const auto& right) {
                            return left.name == right.name && left.generation == right.generation &&
                                   left.entry == right.entry;
                          }))
    return Result<void>::failure({ErrorCode::conflict, "Saved project changed during collection"});

  const auto staged_name = temporary.path.filename().string();
  const auto published =
      foundation::rename_in(parent_fd.get(), staged_name, name, foundation::PublishMode::no_replace,
                            "Cannot publish collected project at destination");
  if (!published) {
    const int failure = errno; // Read while the failed result still holds it.
    return Result<void>::failure({failure == EEXIST ? ErrorCode::conflict : ErrorCode::io_failure,
                                  "Cannot publish collected project at destination"});
  }
  temporary.published = true;
  if (::fsync(parent_fd.get()) != 0) {
    return Result<void>::failure(
        {ErrorCode::io_failure,
         "Collected project is valid but directory durability is uncertain"});
  }
  return Result<void>::success();
}

} // namespace oscillate::application
