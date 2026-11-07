#include "diagnostics_controller.hpp"

#include "performance_trace.hpp"
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
#include "vst3_browser_controller.hpp"
#include "vst3_helper_path.hpp"

#include "oscillate/plugins/vst3_scan.hpp"
#endif

#include "oscillate/foundation/log.hpp"

#include <QDesktopServices>
#include <QUrl>
#include <QtGlobal>

#include <cstdlib>
#include <format>
#include <ostream>
#include <string_view>
#include <utility>

#include <pwd.h>
#include <unistd.h>

// OSCILLATE_VERSION is defined for this file only (apps/daw_quick/CMakeLists.txt),
// so a version bump recompiles nothing else.
#ifndef OSCILLATE_VERSION
#error "OSCILLATE_VERSION must be defined for diagnostics_controller.cpp"
#endif

namespace oscillate::quick {
namespace {

namespace bundle = foundation::diagnostics_bundle;
namespace crash = foundation::crash_report;

constexpr int poll_interval_ms = 50;
// getpwuid_r's buffer when the system gives no size.
constexpr std::size_t passwd_buffer_bytes = 16384;
constexpr std::string_view no_plugins_note =
    "No VST3 plug-ins are listed: none were found, or the VST3 browser has not scanned in "
    "this session.";
#if !(defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3)
constexpr std::string_view no_vst3_note = "This build has no VST3 host.";
#endif
// A bundle's processors; its edit controllers and other classes are not listed.
constexpr std::string_view audio_module_class = "Audio Module Class";
constexpr std::string_view truncated_note =
    "The scan stopped at its limits; plug-ins beyond them are not listed.";

std::string yes_no(bool value) { return value ? "yes" : "no"; }

std::string environment(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr ? std::string(value) : std::string{};
}

// HOME, else the account's home directory, so the bundle is redacted even
// when HOME is unset.
std::string home_directory() {
  if (auto home = environment("HOME"); !home.empty())
    return home;
  const auto size = ::sysconf(_SC_GETPW_R_SIZE_MAX);
  std::vector<char> buffer(size > 0 ? static_cast<std::size_t>(size) : passwd_buffer_bytes);
  passwd entry{};
  passwd* found = nullptr;
  if (::getpwuid_r(::getuid(), &entry, buffer.data(), buffer.size(), &found) != 0 ||
      found == nullptr || found->pw_dir == nullptr)
    return {};
  return found->pw_dir;
}

} // namespace

std::vector<crash::Fact> build_features() {
#if defined(NDEBUG)
  constexpr bool release = true;
#else
  constexpr bool release = false;
#endif
  return {
      {"build", release ? "release" : "debug"},
      {"vst3 host", yes_no(OSCILLATE_HAVE_VST3 != 0)},
      {"jack output and real-time export", yes_no(OSCILLATE_HAVE_JACK_EXPORT != 0)},
      {"aiff, flac and mp3 import and export", yes_no(OSCILLATE_HAVE_CODEC != 0)},
      {"export sample-rate conversion", yes_no(OSCILLATE_HAVE_EXPORT_RESAMPLER != 0)},
      {"source sample-rate conversion", yes_no(OSCILLATE_HAVE_SOURCE_RESAMPLER != 0)},
      {"convolution reverb", yes_no(OSCILLATE_HAVE_CONVOLUTION != 0)},
  };
}

#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
DiagnosticsPlugins diagnostics_plugins(const plugins::Vst3CatalogueSnapshot& snapshot) {
  DiagnosticsPlugins result;
  for (const auto& entry : snapshot.entries) {
    const auto status = vst3_scan_state(entry.scan.status).toStdString();
    const auto listed = result.plugins.size();
    if (entry.scan.ok())
      for (const auto& klass : entry.scan.metadata.classes)
        if (klass.category == audio_module_class)
          result.plugins.push_back(
              {.name = klass.name, .version = klass.version, .status = status});
    if (result.plugins.size() == listed)
      result.plugins.push_back(
          {.name = entry.canonical_bundle.filename().string(), .version = {}, .status = status});
  }
  if (snapshot.truncated)
    result.note = truncated_note;
  else if (result.plugins.empty())
    result.note = no_plugins_note;
  return result;
}
#endif

bundle::Options diagnostics_options(DiagnosticsPlugins plugins, bool include_trace) {
  const auto home = home_directory();
  const auto state_home = environment("XDG_STATE_HOME");
  const char* home_text = home.empty() ? nullptr : home.c_str();
  const char* state_text = state_home.empty() ? nullptr : state_home.c_str();
  bundle::Options options;
  options.directory = bundle::default_directory(state_text, home_text);
  options.crash_directory = crash::default_directory(state_text, home_text);
  options.log_directory = foundation::log::options_from_environment().directory;
  // Same account-home fallback as the bundle and crash-report directories.
  if (options.log_directory.empty())
    options.log_directory = foundation::log::default_directory(state_text, home_text);
  options.home = home;
  options.application = {{"application", std::format("Oscillate {}", OSCILLATE_VERSION)},
                         {"qt", qVersion()}};
  options.build = build_features();
  options.plugins = std::move(plugins.plugins);
  options.plugin_note = std::move(plugins.note);
#if !(defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3)
  options.plugin_note = no_vst3_note;
#endif
  if (include_trace)
    options.trace = latest_trace().value_or(std::filesystem::path{});
  return options;
}

int save_diagnostics_from_command_line(
    bool include_trace, std::ostream& out, // NOLINT(bugprone-easily-swappable-parameters)
    std::ostream& err) {
  DiagnosticsPlugins plugins;
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  err << "Scanning VST3 plug-ins in their own process...\n";
  plugins::Vst3Scanner scanner(detail::vst3_helper_path());
  plugins::Vst3Catalogue catalogue(scanner);
  plugins = diagnostics_plugins(catalogue.refresh());
#endif
  auto options = diagnostics_options(std::move(plugins), include_trace);
  if (include_trace && options.trace.empty())
    err << "No saved performance trace to include.\n";
  auto written = bundle::write(options);
  if (!written) {
    err << "Could not save diagnostics: " << written.error().message << '\n';
    return 1;
  }
  out << written.value().string() << '\n';
  return 0;
}

DiagnosticsController::DiagnosticsController(Host host, QObject* parent)
    : QObject(parent), host_(std::move(host)), timer_(this) {
  if (!host_.write)
    host_.write = [](const bundle::Options& options) { return bundle::write(options); };
  if (!host_.reveal)
    host_.reveal = [this](const std::filesystem::path& path) {
      const auto folder = QString::fromStdString(path.parent_path().string());
      if (!QDesktopServices::openUrl(QUrl::fromLocalFile(folder)) && host_.status)
        host_.status(QStringLiteral("Could not open the folder; diagnostics saved to %1")
                         .arg(QString::fromStdString(path.string())));
    };
  timer_.setInterval(poll_interval_ms);
  connect(&timer_, &QTimer::timeout, this, &DiagnosticsController::poll);
}

DiagnosticsController::~DiagnosticsController() { job_.wait(); }

bool DiagnosticsController::trace_available() const { return latest_trace().has_value(); }

void DiagnosticsController::save(bool include_trace) {
  if (saving())
    return;
  auto options =
      diagnostics_options(host_.plugins ? host_.plugins() : DiagnosticsPlugins{}, include_trace);
  const auto failed = job_.start(
      0, [write = host_.write, options = std::move(options)] { return write(options); },
      [](const std::string& what) {
        return foundation::Result<std::filesystem::path>::failure(
            {foundation::ErrorCode::internal, "Diagnostics not saved: " + what});
      });
  if (failed) {
    if (host_.status)
      host_.status(
          QStringLiteral("Could not save diagnostics: %1").arg(QString::fromStdString(*failed)));
    return;
  }
  timer_.start();
  emit changed();
}

void DiagnosticsController::wait() {
  if (!job_.running())
    return;
  job_.wait();
  poll();
}

void DiagnosticsController::poll() {
  if (!job_.finished())
    return;
  timer_.stop();
  auto finished = job_.take();
  if (finished && finished->result) {
    const auto& path = finished->result.value();
    bundle_path_ = QString::fromStdString(path.string());
    foundation::log::info(foundation::log::Category::application, "Diagnostics saved to {}",
                          path.string());
    host_.reveal(path);
  } else {
    const auto message = finished ? finished->result.error().message : std::string("unknown");
    foundation::log::error(foundation::log::Category::application, "Could not save diagnostics: {}",
                           message);
    if (host_.status)
      host_.status(
          QStringLiteral("Could not save diagnostics: %1").arg(QString::fromStdString(message)));
  }
  emit changed();
}

} // namespace oscillate::quick
