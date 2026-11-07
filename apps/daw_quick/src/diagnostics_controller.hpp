#pragma once

#include "background_job.hpp"

#include "oscillate/foundation/crash_report.hpp"
#include "oscillate/foundation/diagnostics_bundle.hpp"
#include "oscillate/foundation/error.hpp"
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
#include "oscillate/plugins/vst3_catalogue.hpp"
#endif

#include <QObject>
#include <QString>
#include <QTimer>

#include <filesystem>
#include <functional>
#include <iosfwd>
#include <string>
#include <vector>

namespace oscillate::quick {

// What plugins.txt says: the scan's rows, and a note when there are none to
// give (no scan yet, or no plug-in host in this build).
struct DiagnosticsPlugins {
  std::vector<foundation::diagnostics_bundle::Plugin> plugins;
  std::string note;
};

// This build's optional features, for build.txt.
[[nodiscard]] std::vector<foundation::crash_report::Fact> build_features();

#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
// One row per audio processor a scan found, and one per bundle that failed
// or has none (its folder name and status). Paths and diagnostics stay out.
[[nodiscard]] DiagnosticsPlugins
diagnostics_plugins(const plugins::Vst3CatalogueSnapshot& snapshot);
#endif

// A bundle's options from the environment (XDG_STATE_HOME, XDG_DATA_HOME,
// HOME and the log settings): its directories, this application's facts and
// build features, and the newest performance trace when `include_trace`.
[[nodiscard]] foundation::diagnostics_bundle::Options
diagnostics_options(DiagnosticsPlugins plugins, bool include_trace);

// `oscillate --save-diagnostics [--with-trace]`: scans the standard VST3
// folders (when this build hosts VST3), writes a bundle and prints its path
// to `out`. Returns the process exit code; problems go to `err`.
[[nodiscard]] int save_diagnostics_from_command_line(bool include_trace, std::ostream& out,
                                                     std::ostream& err);

// "Save diagnostics" in the command palette: writes a bundle on a worker,
// then opens its folder. Nothing is sent anywhere.
class DiagnosticsController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool saving READ saving NOTIFY changed)
  Q_PROPERTY(QString bundlePath READ bundle_path NOTIFY changed)

public:
  struct Host {
    // The plug-in scan as it stands in this session.
    std::function<DiagnosticsPlugins()> plugins;
    // Reports a failure to the user.
    std::function<void(QString)> status;
    // Shows the saved bundle (opens its folder). Test seam.
    std::function<void(const std::filesystem::path&)> reveal;
    // Writes the bundle. Test seam: empty uses foundation::diagnostics_bundle::write.
    std::function<foundation::Result<std::filesystem::path>(
        const foundation::diagnostics_bundle::Options&)>
        write;
  };

  explicit DiagnosticsController(Host host, QObject* parent = nullptr);
  ~DiagnosticsController() override;
  DiagnosticsController(const DiagnosticsController&) = delete;
  DiagnosticsController& operator=(const DiagnosticsController&) = delete;

  [[nodiscard]] bool saving() const noexcept { return job_.running(); }
  [[nodiscard]] QString bundle_path() const { return bundle_path_; }

  // Saves a bundle, with the newest performance trace when `include_trace`.
  Q_INVOKABLE void save(bool include_trace);
  // Whether a saved performance trace exists to include.
  [[nodiscard]] Q_INVOKABLE bool trace_available() const;
  // Blocks until a save in progress finishes and reports it (tests, quit).
  void wait();

signals:
  void changed();

private:
  void poll();

  Host host_;
  BackgroundJob<foundation::Result<std::filesystem::path>> job_;
  QTimer timer_;
  QString bundle_path_;
};

} // namespace oscillate::quick
