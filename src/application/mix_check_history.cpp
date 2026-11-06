// Mix Check's history (ADR-0074): each check's summary in the project's
// disposable cache directory, and what changed since the previous one.
#include "oscillate/application/mix_check.hpp"

#include "oscillate/foundation/file_io.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace oscillate::application::mix_check {
namespace {
using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;
using Json = nlohmann::json;

// cache/mix-check/history.json, replaced through a temporary file.
constexpr const char* history_directory = "mix-check";
constexpr const char* history_file = "history.json";
constexpr const char* history_temporary = "history.json.tmp";
constexpr int history_version = 1;
// History is small; anything larger is not ours.
constexpr std::size_t max_history_bytes = 1U << 20U;

using Fd = foundation::UniqueFd;

[[nodiscard]] Result<int> open_history_directory(const persistence::ProjectStore& store) {
  auto cache = store.open_cache_directory();
  if (!cache)
    return Result<int>::failure(cache.error());
  Fd parent(cache.value());
  if (::mkdirat(parent.get(), history_directory, 0700) != 0 && errno != EEXIST)
    return Result<int>::failure({ErrorCode::io_failure, "Cannot create the Mix Check history"});
  const int directory =
      ::openat(parent.get(), history_directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (directory < 0)
    return Result<int>::failure({ErrorCode::io_failure, "Cannot open the Mix Check history"});
  return Result<int>::success(directory);
}

[[nodiscard]] std::string fixed(double value, int decimals) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(decimals) << value;
  return out.str();
}

// Every metric reads better lower, except the integrated loudness, which
// reads better nearer the target range.
[[nodiscard]] double badness(const HistoryFinding& finding, double value, const Target& goal) {
  if (finding.id == "loudness:integrated")
    return value < goal.lufs_low    ? goal.lufs_low - value
           : value > goal.lufs_high ? value - goal.lufs_high
                                    : 0.0;
  return value;
}

} // namespace

History summarise(const Report& report, std::int64_t checked_at) {
  History history;
  history.checked_at = checked_at;
  history.loudness = report.loudness;
  for (const auto& finding : report.findings)
    history.findings.push_back({finding.id, finding.title, finding.metric, finding.metric_label});
  return history;
}

std::vector<Change> since_last(const History& previous, const History& current,
                               std::string_view target_id) {
  const auto& goal = target(target_id);
  std::vector<Change> changes;
  std::size_t same = 0;
  for (const auto& before : previous.findings) {
    const auto now = std::ranges::find(current.findings, before.id, &HistoryFinding::id);
    if (now == current.findings.end()) {
      changes.push_back({"fixed", before.title + ": fixed"});
      continue;
    }
    const double was = badness(before, before.metric, goal);
    const double is = badness(before, now->metric, goal);
    if (std::fabs(now->metric - before.metric) <= unchanged_db) {
      ++same;
      continue;
    }
    changes.push_back({is < was ? "better" : "worse", before.title + ": " + before.label + " " +
                                                          fixed(before.metric, 1) + " → " +
                                                          fixed(now->metric, 1)});
  }
  for (const auto& now : current.findings)
    if (std::ranges::find(previous.findings, now.id, &HistoryFinding::id) ==
        previous.findings.end())
      changes.push_back({"new", "New: " + now.title});
  // The loudness numbers outside any finding.
  const auto compare = [&](const char* what, double was, double is, bool lower_better,
                           const char* unit) {
    if (std::fabs(is - was) <= unchanged_db) {
      ++same;
      return;
    }
    const bool rose = is > was;
    changes.push_back({rose != lower_better ? "better" : "worse",
                       std::string(what) + (rose ? " rose " : " fell ") +
                           fixed(std::fabs(is - was), 1) + " " + unit + " (" + fixed(was, 1) +
                           " → " + fixed(is, 1) + ")"});
  };
  const bool tracked_peak =
      std::ranges::any_of(current.findings,
                          [](const HistoryFinding& f) { return f.id == "loudness:true_peak"; }) ||
      std::ranges::any_of(previous.findings,
                          [](const HistoryFinding& f) { return f.id == "loudness:true_peak"; });
  if (!tracked_peak)
    compare("True peak", previous.loudness.true_peak, current.loudness.true_peak, true, "dB");
  compare("Loudness range", previous.loudness.range, current.loudness.range, false, "LU");
  compare("Drum crest", previous.loudness.crest, current.loudness.crest, false, "dB");
  if (changes.empty() || same > 0)
    changes.push_back(
        {"same", changes.empty() ? "Nothing changed by more than " + fixed(unchanged_db, 1) + " dB"
                                 : "Everything else within " + fixed(unchanged_db, 1) + " dB"});
  return changes;
}

std::string to_json(const History& history) {
  Json findings = Json::array();
  for (const auto& finding : history.findings)
    findings.push_back({{"id", finding.id},
                        {"title", finding.title},
                        {"metric", finding.metric},
                        {"label", finding.label}});
  const Json document{{"version", history_version},
                      {"checked_at", history.checked_at},
                      {"integrated", history.loudness.integrated},
                      {"true_peak", history.loudness.true_peak},
                      {"range", history.loudness.range},
                      {"crest", history.loudness.crest},
                      {"findings", findings}};
  return document.dump();
}

std::optional<History> from_json(std::string_view text) {
  const auto document = Json::parse(text, nullptr, false);
  if (document.is_discarded() || !document.is_object() ||
      document.value("version", 0) != history_version)
    return std::nullopt;
  try {
    History history;
    history.checked_at = document.at("checked_at").get<std::int64_t>();
    history.loudness.integrated = document.at("integrated").get<double>();
    history.loudness.true_peak = document.at("true_peak").get<double>();
    history.loudness.range = document.at("range").get<double>();
    history.loudness.crest = document.at("crest").get<double>();
    for (const auto& finding : document.at("findings"))
      history.findings.push_back(
          {finding.at("id").get<std::string>(), finding.at("title").get<std::string>(),
           finding.at("metric").get<double>(), finding.at("label").get<std::string>()});
    return history;
  } catch (const Json::exception&) {
    return std::nullopt;
  }
}

std::optional<History> load_history(const persistence::ProjectStore& store) {
  auto directory = open_history_directory(store);
  if (!directory)
    return std::nullopt;
  Fd parent(directory.value());
  Fd file(::openat(parent.get(), history_file, O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
  if (file.get() < 0)
    return std::nullopt;
  std::string text;
  std::array<char, 16'384> buffer{};
  for (;;) {
    const auto count = ::read(file.get(), buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      break;
    text.append(buffer.data(), static_cast<std::size_t>(count));
    if (text.size() > max_history_bytes)
      return std::nullopt;
  }
  return from_json(text);
}

Result<void> save_history(const persistence::ProjectStore& store, const History& history) {
  auto directory = open_history_directory(store);
  if (!directory)
    return Result<void>::failure(directory.error());
  Fd parent(directory.value());
  const auto text = to_json(history);
  auto replaced = foundation::replace_file(parent.get(), history_temporary, history_file,
                                           std::as_bytes(std::span(text.data(), text.size())),
                                           "Mix Check history");
  if (!replaced)
    return Result<void>::failure(std::move(replaced.error()));
  return Result<void>::success();
}

} // namespace oscillate::application::mix_check
