#include "oscillate/plugins/vst3_catalogue.hpp"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

namespace oscillate::plugins {
namespace {

[[nodiscard]] std::optional<std::filesystem::path>
canonical_path(const std::filesystem::path& path) {
  if (path.empty())
    return std::nullopt;
  std::error_code error;
  auto canonical = std::filesystem::weakly_canonical(path, error);
  if (error)
    return std::nullopt;
  return canonical;
}

void keep_smallest(std::set<std::filesystem::path>& paths, std::filesystem::path candidate,
                   std::size_t limit, bool& truncated) {
  if (limit == 0) {
    truncated = true;
    return;
  }
  if (paths.contains(candidate))
    return;
  if (paths.size() < limit) {
    paths.insert(std::move(candidate));
  } else {
    truncated = true;
    const auto last = std::prev(paths.end());
    if (candidate < *last) {
      paths.erase(last);
      paths.insert(std::move(candidate));
    }
  }
}

} // namespace

std::vector<std::filesystem::path> standard_linux_vst3_roots(std::filesystem::path home) {
  if (home.empty()) {
    if (const auto* value = std::getenv("HOME"); value != nullptr && value[0] != '\0')
      home = value;
  }
  std::vector<std::filesystem::path> roots;
  roots.reserve(5);
  if (!home.empty())
    roots.push_back(home / ".vst3");
  roots.emplace_back("/usr/lib64/vst3");
  roots.emplace_back("/usr/lib/vst3");
  roots.emplace_back("/usr/local/lib64/vst3");
  roots.emplace_back("/usr/local/lib/vst3");
  return roots;
}

Vst3Catalogue::Vst3Catalogue(Vst3Scanner& scanner, std::vector<std::filesystem::path> roots,
                             Vst3CatalogueLimits limits)
    : Vst3Catalogue([&scanner](const auto& path) { return scanner.scan(path); },
                    [&scanner](const auto& path) { scanner.retry(path); }, std::move(roots),
                    limits) {}

Vst3Catalogue::Vst3Catalogue(ScanFunction scan, RetryFunction retry,
                             std::vector<std::filesystem::path> roots, Vst3CatalogueLimits limits)
    : scan_(std::move(scan)), retry_(std::move(retry)), roots_(std::move(roots)), limits_(limits) {
  if (!scan_ || !retry_)
    throw std::invalid_argument("VST3 catalogue requires scan and retry actions");
}

const Vst3CatalogueSnapshot&
Vst3Catalogue::refresh(std::span<const std::filesystem::path> explicit_paths) {
  Vst3CatalogueSnapshot next;
  std::set<std::filesystem::path> seen_bundles;
  std::set<std::filesystem::path> seen_search_paths;
  std::size_t visited = 0;
  std::size_t searched = 0;

  // Explicit paths are the user's choice, so a missing one is reported;
  // missing standard roots are normal and stay silent.
  const auto discover = [&](const std::filesystem::path& search, bool chosen) {
    const auto root = canonical_path(search);
    if (!root) {
      next.truncated = true;
      next.issues.push_back({search, search.empty() ? "Plugin search path is empty"
                                                    : "Cannot resolve plugin search path"});
      return;
    }
    if (seen_search_paths.contains(*root))
      return;
    if (next.entries.size() >= limits_.max_bundles) {
      next.truncated = true;
      return;
    }
    if (searched >= limits_.max_search_paths) {
      next.truncated = true;
      return;
    }
    ++searched;
    seen_search_paths.insert(*root);
    std::set<std::filesystem::path> local;
    if (search.extension() == ".vst3") {
      // Explicit bundle paths are retained even when missing or malformed, so
      // the scanner can produce an actionable status for that user choice.
      keep_smallest(local, *root, limits_.max_bundles, next.truncated);
    } else {
      std::error_code error;
      if (!std::filesystem::is_directory(search, error)) {
        if (chosen)
          next.issues.push_back({search, error
                                             ? "Cannot read plugin search path: " + error.message()
                                             : "Plugin search path is not a directory"});
        return;
      }
      std::filesystem::recursive_directory_iterator it(
          search, std::filesystem::directory_options::skip_permission_denied, error);
      const std::filesystem::recursive_directory_iterator end;
      while (!error && it != end) {
        if (visited >= limits_.max_entries) {
          next.truncated = true;
          break;
        }
        ++visited;
        const auto path = it->path();
        const auto is_bundle = path.extension() == ".vst3" && it->is_directory(error);
        if (!error && is_bundle) {
          if (const auto canonical = canonical_path(path))
            keep_smallest(local, *canonical, limits_.max_bundles, next.truncated);
          else
            next.truncated = true;
          it.disable_recursion_pending();
        }
        it.increment(error);
      }
      if (error)
        next.truncated = true;
    }
    for (const auto& bundle : local) {
      if (!seen_bundles.insert(bundle).second)
        continue;
      if (next.entries.size() >= limits_.max_bundles) {
        next.truncated = true;
        break;
      }
      next.entries.push_back({bundle, {}});
    }
  };

  for (const auto& path : explicit_paths)
    discover(path, true);
  for (const auto& root : roots_)
    discover(root, false);
  for (auto& entry : next.entries)
    entry.scan = scan_(entry.canonical_bundle);
  snapshot_ = std::move(next);
  return snapshot_;
}

bool Vst3Catalogue::retry(const std::filesystem::path& bundle) {
  const auto canonical = canonical_path(bundle);
  if (!canonical)
    return false;
  const auto found =
      std::ranges::find(snapshot_.entries, *canonical, &Vst3CatalogueEntry::canonical_bundle);
  if (found == snapshot_.entries.end())
    return false;
  retry_(*canonical);
  found->scan = scan_(*canonical);
  return true;
}

} // namespace oscillate::plugins
