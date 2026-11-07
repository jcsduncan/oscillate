#include "../../../apps/daw_quick/src/diagnostics_controller.hpp"
#include "oscillate/test/check.hpp"

#include <QCoreApplication>

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

namespace {

namespace quick = oscillate::quick;
namespace bundle = oscillate::foundation::diagnostics_bundle;
using oscillate::foundation::ErrorCode;
using oscillate::foundation::Result;

constexpr std::string_view plugin_name = "Glass Synth";
constexpr std::string_view plugin_version = "1.2.3";
constexpr std::string_view plugin_status = "ready";
constexpr std::string_view written_bundle = "/state/oscillate/diagnostics/bundle.zip";
constexpr std::string_view write_failure = "disk full";
constexpr std::string_view older_trace = "trace-20261101-090000.json";
constexpr std::string_view newer_trace = "trace-20261107-120000.json";
constexpr std::string_view not_a_trace = "notes.json";
constexpr std::string_view application_prefix = "Oscillate ";
constexpr int overwrite = 1;

struct Recorded {
  std::optional<bundle::Options> options;
  std::optional<std::filesystem::path> revealed;
  std::vector<QString> statuses;
};

quick::DiagnosticsController::Host host(Recorded& recorded,
                                        const Result<std::filesystem::path>& outcome) {
  return {
      .plugins =
          [] {
            return quick::DiagnosticsPlugins{
                .plugins = {{std::string(plugin_name), std::string(plugin_version),
                             std::string(plugin_status)}},
                .note = {}};
          },
      .status = [&recorded](QString message) { recorded.statuses.push_back(std::move(message)); },
      .reveal = [&recorded](const std::filesystem::path& path) { recorded.revealed = path; },
      .write =
          [&recorded, outcome](const bundle::Options& options) {
            recorded.options = options;
            return outcome;
          },
  };
}

void write_text(const std::filesystem::path& path, std::string_view text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path, std::ios::binary);
  file << text;
}

void save_writes_on_a_worker_then_reveals_the_bundle() {
  Recorded recorded;
  quick::DiagnosticsController controller(host(
      recorded, Result<std::filesystem::path>::success(std::filesystem::path(written_bundle))));
  controller.save(false);
  REQUIRE_TRUE(controller.saving(), "the bundle is written on a worker");
  controller.wait();
  REQUIRE_FALSE(controller.saving());
  REQUIRE_EQ(recorded.revealed, std::optional(std::filesystem::path(written_bundle)),
             "the saved bundle's folder is opened for review");
  REQUIRE_EQ(controller.bundle_path().toStdString(), std::string(written_bundle));
  REQUIRE_TRUE(recorded.statuses.empty(), "success is not reported as a problem");

  if (!recorded.options)
    throw std::runtime_error("the bundle was not written");
  const auto& options = *recorded.options;
  REQUIRE_EQ(options.plugins.size(), std::size_t{1});
  REQUIRE_EQ(options.plugins.front().name, std::string(plugin_name));
  REQUIRE_TRUE(options.trace.empty(), "no trace unless asked for");
  REQUIRE_FALSE(options.application.empty());
  REQUIRE_TRUE(options.application.front().value.starts_with(application_prefix),
               "the bundle names the application and its version");
  REQUIRE_EQ(options.build.size(), quick::build_features().size());
  REQUIRE_FALSE(options.directory.empty());
}

void failure_is_reported_and_nothing_is_revealed() {
  Recorded recorded;
  quick::DiagnosticsController controller(host(
      recorded,
      Result<std::filesystem::path>::failure({ErrorCode::io_failure, std::string(write_failure)})));
  controller.save(false);
  controller.wait();
  REQUIRE_FALSE(recorded.revealed.has_value());
  REQUIRE_EQ(recorded.statuses.size(), std::size_t{1});
  REQUIRE_TRUE(recorded.statuses.front().startsWith(QStringLiteral("Could not save diagnostics")),
               "the shell shows failures that start this way");
  REQUIRE_TRUE(recorded.statuses.front().contains(QString::fromUtf8(write_failure.data())));
}

void trace_is_the_newest_saved_one() {
  const auto data_home = std::filesystem::temp_directory_path() /
                         ("oscillate-diagnostics-data-" + std::to_string(::getpid()));
  std::filesystem::remove_all(data_home);
  const auto traces = data_home / "oscillate" / "traces";
  ::setenv("XDG_DATA_HOME", data_home.c_str(), overwrite);

  Recorded recorded;
  quick::DiagnosticsController controller(host(
      recorded, Result<std::filesystem::path>::success(std::filesystem::path(written_bundle))));
  REQUIRE_FALSE(controller.trace_available(), "no trace saved yet");

  write_text(traces / older_trace, "{}\n");
  write_text(traces / newer_trace, "{}\n");
  write_text(traces / not_a_trace, "{}\n");
  REQUIRE_TRUE(controller.trace_available());
  controller.save(true);
  controller.wait();
  if (!recorded.options)
    throw std::runtime_error("the bundle was not written");
  REQUIRE_EQ(recorded.options->trace, traces / newer_trace);
  std::filesystem::remove_all(data_home);
}

void build_features_answer_yes_or_no() {
  const auto features = quick::build_features();
  REQUIRE_TRUE(
      std::ranges::any_of(features, [](const auto& fact) { return fact.name == "vst3 host"; }));
  for (const auto& fact : features)
    if (fact.name != "build")
      REQUIRE_TRUE(fact.value == "yes" || fact.value == "no", fact.name);
}

#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
void scan_rows_keep_names_versions_and_status_only() {
  namespace plugins = oscillate::plugins;
  plugins::Vst3CatalogueSnapshot snapshot;
  plugins::Vst3CatalogueEntry good;
  good.canonical_bundle = "/home/listener/.vst3/Glass.vst3";
  good.scan.status = plugins::ScanStatus::ok;
  good.scan.metadata.vendor = "Glassworks";
  good.scan.metadata.classes = {{.id = "A",
                                 .category = "Audio Module Class",
                                 .name = std::string(plugin_name),
                                 .vendor = "Glassworks",
                                 .version = std::string(plugin_version),
                                 .subcategories = "Instrument"},
                                {.id = "B",
                                 .category = "Audio Module Class",
                                 .name = "Glass Delay",
                                 .vendor = "Glassworks",
                                 .version = "2.0",
                                 .subcategories = "Fx"},
                                {.id = "C",
                                 .category = "Component Controller Class",
                                 .name = std::string(plugin_name),
                                 .vendor = "Glassworks",
                                 .version = std::string(plugin_version),
                                 .subcategories = {}}};
  plugins::Vst3CatalogueEntry broken;
  broken.canonical_bundle = "/home/listener/.vst3/Broken.vst3";
  broken.scan.status = plugins::ScanStatus::timed_out;
  broken.scan.diagnostic = "stderr from /home/listener/.vst3/Broken.vst3";
  snapshot.entries = {good, broken};

  const auto rows = quick::diagnostics_plugins(snapshot);
  REQUIRE_TRUE(rows.note.empty());
  REQUIRE_EQ(rows.plugins.size(), std::size_t{3});
  REQUIRE_EQ(rows.plugins[0].name, std::string(plugin_name));
  REQUIRE_EQ(rows.plugins[0].version, std::string(plugin_version));
  REQUIRE_EQ(rows.plugins[0].status, std::string(plugin_status));
  REQUIRE_EQ(rows.plugins[2].name, std::string("Broken.vst3"), "a failed bundle by folder name");
  REQUIRE_EQ(rows.plugins[2].status, std::string("scan timed out"));
  for (const auto& row : rows.plugins)
    for (const auto* field : {&row.name, &row.version, &row.status})
      REQUIRE_EQ(field->find('/'), std::string::npos, "no paths or diagnostics");

  REQUIRE_FALSE(quick::diagnostics_plugins({}).note.empty(), "an empty scan says why");
  snapshot.truncated = true;
  REQUIRE_FALSE(quick::diagnostics_plugins(snapshot).note.empty(), "a cut-short scan says so");
}
#endif

} // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  try {
    save_writes_on_a_worker_then_reveals_the_bundle();
    failure_is_reported_and_nothing_is_revealed();
    trace_is_the_newest_saved_one();
    build_features_answer_yes_or_no();
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
    scan_rows_keep_names_versions_and_status_only();
#endif
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
