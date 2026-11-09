#include "note_tools_controller.hpp"
#include "project_bridge.hpp"
#include "smoke_audio_backend_scenario.hpp"
#include "smoke_automation_scenario.hpp"
#include "smoke_export_scenario.hpp"
#include "smoke_import_scenario.hpp"
#include "smoke_routing_scenario.hpp"
#include "smoke_tempo_map_scenario.hpp"
#include "smoke_tracks.hpp"
#include "smoke_waveform_scenario.hpp"

#include "oscillate/application/midi_note_tools.hpp"
#include "oscillate/audio_io/stereo_block.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QImage>
#include <QMimeData>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QTimer>
#include <QWheelEvent>
#include <fcntl.h>
#include <unistd.h>
#ifdef OSCILLATE_QUICK_TEST_INPUT
#include <QTest>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using quick_smoke::SmokeDeviceState;

// The callback load a streaming fixture device reports, as real outputs do
// from their DeviceTelemetry; the CPU meter shows it.
constexpr double smoke_callback_load = 0.25;

class SmokePlaybackOutput final : public oscillate::application::PlaybackOutput {
public:
  SmokePlaybackOutput(oscillate::audio_io::StereoRenderSource source,
                      oscillate::audio_io::TransportCursor& transport, bool fail,
                      std::shared_ptr<SmokeDeviceState> device = {}, int backend_id = 0)
      : source_(source), transport_(transport), fail_(fail), device_(std::move(device)),
        backend_id_(backend_id) {}

  // PlaybackOutput fixes the adjacent rate and requested-frame argument order.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  oscillate::foundation::Result<void> start(std::uint32_t rate, std::uint32_t requested_frames,
                                            std::optional<std::uint64_t> target_serial) override {
    if (device_)
      device_->requested_buffer_frames.store(requested_frames, std::memory_order_release);
    if (device_)
      device_->backend_id.store(backend_id_, std::memory_order_release);
    if (device_ && device_->delay_start.load(std::memory_order_acquire))
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (fail_) {
      return oscillate::foundation::Result<void>::failure(
          {oscillate::foundation::ErrorCode::unavailable, "Smoke output refused playback"});
    }
    status_.state = oscillate::audio_io::OutputState::streaming;
    if (device_) {
      device_->selected_serial.store(target_serial.value_or(0), std::memory_order_release);
    }
    status_.requested_client_rate = rate;
    std::array<oscillate::engine::StereoFrame, 64> frames{};
    std::array<float, 128> output{};
    (void)oscillate::audio_io::render_transport_block(source_, transport_, true, frames, output);
    if (device_) {
      device_->first_sample.store(output[0], std::memory_order_release);
      device_->first_completed_sample.store(transport_.completed_snapshot().next_sample,
                                            std::memory_order_release);
      if (device_->meter_full_window) {
        for (int block = 1; block < 16; ++block)
          (void)oscillate::audio_io::render_transport_block(source_, transport_, true, frames,
                                                            output);
      }
    }
    return oscillate::foundation::Result<void>::success();
  }
  void stop() noexcept override {
    if (device_)
      device_->stopped_off_ui.store(std::this_thread::get_id() != device_->ui_thread,
                                    std::memory_order_release);
    status_.state = oscillate::audio_io::OutputState::stopped;
  }
  [[nodiscard]] oscillate::audio_io::OutputStatus status() const noexcept override {
    auto status = status_;
    if (device_ && device_->disconnected.load(std::memory_order_acquire) &&
        status.state == oscillate::audio_io::OutputState::streaming) {
      status.state = oscillate::audio_io::OutputState::disconnected;
    }
    if (status.state == oscillate::audio_io::OutputState::streaming)
      status.callback_load =
          oscillate::audio_io::CallbackLoad{smoke_callback_load, smoke_callback_load};
    return status;
  }

private:
  oscillate::audio_io::StereoRenderSource source_;
  oscillate::audio_io::TransportCursor& transport_;
  bool fail_;
  std::shared_ptr<SmokeDeviceState> device_;
  int backend_id_ = 0;
  oscillate::audio_io::OutputStatus status_;
};

qsizetype count_clip_items(const QQuickItem* item) {
  qsizetype count = item->objectName() == QStringLiteral("arrangementClip") ? 1 : 0;
  for (const auto* child : item->childItems()) {
    count += count_clip_items(child);
  }
  return count;
}

using quick_smoke::count_visible_items;

QQuickItem* nth_visible_item(QQuickItem* item, const QString& name, qsizetype& remaining);

bool arrangement_grid_visible(const QImage& image, QQuickWindow* window) {
  if (image.isNull() || window->width() <= 0 || window->height() <= 0 ||
      count_visible_items(window->contentItem(), QStringLiteral("arrangementRowGrid")) != 2) {
    return false;
  }
  const auto scale = static_cast<double>(image.width()) / window->width();
  const auto channel_distance = [](const QColor& left, const QColor& right) {
    return std::abs(left.red() - right.red()) + std::abs(left.green() - right.green()) +
           std::abs(left.blue() - right.blue());
  };
  for (qsizetype index = 0; index < 2; ++index) {
    auto remaining = index;
    auto* row =
        nth_visible_item(window->contentItem(), QStringLiteral("arrangementRowGrid"), remaining);
    if (!row || row->width() < 200 || row->height() < 40)
      return false;
    const auto bar = static_cast<int>(std::floor(4 * row->width() / 16));
    const auto beat = static_cast<int>(std::floor(17 * row->width() / 64));
    const auto interior = beat + 7;
    const auto pixel = [&](int local_x) {
      const auto scene = row->mapToScene({local_x + 0.5, row->height() / 2});
      const auto x = static_cast<int>(std::floor(scene.x() * scale));
      const auto y = static_cast<int>(std::floor(scene.y() * scale));
      return image.valid(x, y) ? image.pixelColor(x, y) : QColor{};
    };
    const auto bar_color = pixel(bar);
    const auto beat_color = pixel(beat);
    const auto interior_color = pixel(interior);
    if (channel_distance(bar_color, interior_color) < 75 ||
        channel_distance(beat_color, interior_color) < 20 ||
        channel_distance(bar_color, beat_color) < 45)
      return false;
  }
  return true;
}

QQuickItem* first_visible_item(QQuickItem* item, const QString& name) {
  if (item->isVisible() && item->objectName() == name) {
    return item;
  }
  for (auto* child : item->childItems()) {
    if (auto* match = first_visible_item(child, name)) {
      return match;
    }
  }
  return nullptr;
}

#ifdef OSCILLATE_QUICK_TEST_INPUT
// The top bar's More menu holds the inspector, master effects, plug-ins,
// media, versions, cache and engine details: open it, then find the item.
QQuickItem* more_menu_item(QQuickWindow* window, const QString& name) {
  if (!first_visible_item(window->contentItem(), QStringLiteral("toolbarMoreMenu"))) {
    auto* more = first_visible_item(window->contentItem(), QStringLiteral("toolbarMoreButton"));
    if (!more)
      return nullptr;
    more->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Return);
    QCoreApplication::processEvents();
  }
  return first_visible_item(window->contentItem(), name);
}

// A piano-roll grid step: each key row is one hit area ("pianoRollRow-<key>")
// that works the step out from x, so a cell is a point along the row.
struct PianoRollCell {
  QPointF center;
  qreal width = 0;
};

std::optional<PianoRollCell> piano_roll_cell(QQuickWindow* window, int step, int key) {
  auto* row = first_visible_item(window->contentItem(), QStringLiteral("pianoRollRow-%1").arg(key));
  if (!row)
    return std::nullopt;
  const auto columns = row->property("columns").toInt();
  if (step < 0 || step >= columns)
    return std::nullopt;
  const auto width = row->width() / columns;
  return PianoRollCell{row->mapToScene({width * (step + 0.5), row->height() / 2}), width};
}
#endif

QQuickItem* nth_visible_item(QQuickItem* item, const QString& name, qsizetype& remaining) {
  if (item->isVisible() && item->objectName() == name && remaining-- == 0) {
    return item;
  }
  for (auto* child : item->childItems()) {
    if (auto* match = nth_visible_item(child, name, remaining)) {
      return match;
    }
  }
  return nullptr;
}

QQuickItem* visible_item_with_label(QQuickItem* item, const QString& name, const QString& label) {
  if (item->isVisible() && item->objectName() == name &&
      item->property("label").toString().contains(label)) {
    return item;
  }
  for (auto* child : item->childItems()) {
    if (auto* match = visible_item_with_label(child, name, label)) {
      return match;
    }
  }
  return nullptr;
}

bool drag_item(QQuickWindow* window, const QString& name, double delta_x,
               bool check_constant_width = false,
               Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
#ifdef OSCILLATE_QUICK_TEST_INPUT
  auto* item = first_visible_item(window->contentItem(), name);
  if (!item || !item->isEnabled()) {
    return false;
  }
  const QPointF first = item->mapToScene({item->width() / 2.0, item->height() / 2.0});
  const QPointF last = first + QPointF(delta_x, 0);
  const double initial_width = item->parentItem()->width();
  bool constant_width = true;
  QTest::mousePress(window, Qt::LeftButton, modifiers, first.toPoint());
  for (int step = 1; step <= 4; ++step) {
    QTest::mouseMove(window, (first + QPointF(delta_x * step / 4.0, 0)).toPoint());
    QCoreApplication::processEvents();
    if (check_constant_width && std::abs(item->parentItem()->width() - initial_width) > 0.01) {
      constant_width = false;
    }
  }
  QTest::mouseRelease(window, Qt::LeftButton, modifiers, last.toPoint());
  QCoreApplication::processEvents();
  return constant_width;
#else
  (void)window;
  (void)name;
  (void)delta_x;
  (void)check_constant_width;
  (void)modifiers;
  return false;
#endif
}

} // namespace

int run(int argc, char** argv) {
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("Oscillate"));
  QCoreApplication::setApplicationName(QStringLiteral("Oscillate"));
  std::filesystem::path root;
  std::filesystem::path welcome_create_parent;
  std::filesystem::path welcome_open_folder;
  std::filesystem::path smoke_home;
  bool create = false;
  bool smoke = false;
  bool smoke_midi_workflow = false;
  bool smoke_verify_midi = false;
  bool smoke_transport = false;
  bool smoke_transport_fail = false;
  bool smoke_audio_device = false;
  bool smoke_export = false;
  bool smoke_export_fail = false;
  bool smoke_export_close = false;
  bool smoke_stems = false;
  bool smoke_import = false;
  bool smoke_import_fail = false;
  bool smoke_import_stale = false;
  bool smoke_import_close = false;
  bool smoke_import_undo = false;
  bool smoke_verify_import = false;
  bool smoke_audio_gestures = false;
  // Removes the welcome screen's default returns, so playback takes the
  // direct (unrouted) path the live-edit scenarios exercise.
  bool smoke_without_returns = false;
  bool smoke_verify_audio_gestures = false;
  bool smoke_waveform = false;
  std::string smoke_tempo_mode;
  std::filesystem::path smoke_tempo_drop;
  bool smoke_live_edit = false;
  bool smoke_live_stale = false;
  bool smoke_live_fail = false;
  bool smoke_live_close = false;
  bool smoke_live_stop = false;
  bool smoke_verify_live_edit = false;
  bool smoke_automation = false;
  bool smoke_send_automation = false;
  bool smoke_verify_send_automation = false;
  bool smoke_tempo_map = false;
  bool smoke_verify_tempo_map = false;
  bool smoke_routing = false;
  bool smoke_rate_audio = false;
  bool smoke_grid = false;
  std::filesystem::path smoke_grid_source;
  bool smoke_chain_ui = false;
  std::string qualify_pipewire_serial;
  int qualify_buffer_frames = 128;
  bool qualify_master_limiter = false;
  std::filesystem::path smoke_routing_source;
  bool smoke_verify_routing = false;
  std::filesystem::path smoke_vst3_browser_bundle;
  bool smoke_vst3_two_effects = false;
  std::filesystem::path smoke_vst3_instrument_bundle;
  bool smoke_vst3_browser_disabled = false;
  bool smoke_verify_vst3_browser = false;
  bool smoke_verify_vst3_instrument = false;
  std::filesystem::path smoke_import_source;
  int smoke_export_format = 0;
  int smoke_output_rate = 0;
  std::filesystem::path smoke_export_target;
  std::string screenshot;
  for (int index = 1; index < argc; ++index) {
    const std::string arg = argv[index];
    if (arg == "--create" && index + 1 < argc) {
      root = argv[++index];
      create = true;
    } else if (arg == "--open" && index + 1 < argc) {
      root = argv[++index];
    } else if (arg == "--smoke") {
      smoke = true;
    } else if (arg == "--smoke-midi-workflow") {
      smoke_midi_workflow = true;
    } else if (arg == "--smoke-verify-midi") {
      smoke_verify_midi = true;
    } else if (arg == "--smoke-transport") {
      smoke_transport = true;
    } else if (arg == "--smoke-transport-fail") {
      smoke_transport_fail = true;
    } else if (arg == "--smoke-audio-device") {
      smoke_audio_device = true;
    } else if (arg == "--smoke-export" && index + 2 < argc) {
      smoke_export = true;
      smoke_export_target = argv[++index];
      smoke_export_format = std::stoi(argv[++index]);
    } else if (arg == "--smoke-export-fail" && index + 2 < argc) {
      smoke_export_fail = true;
      smoke_export_target = argv[++index];
      smoke_export_format = std::stoi(argv[++index]);
    } else if (arg == "--smoke-export-close" && index + 2 < argc) {
      smoke_export_close = true;
      smoke_export_target = argv[++index];
      smoke_export_format = std::stoi(argv[++index]);
    } else if (arg == "--smoke-stems") {
      smoke_stems = true;
    } else if (arg == "--smoke-output-rate" && index + 1 < argc) {
      smoke_output_rate = std::stoi(argv[++index]);
    } else if ((arg == "--smoke-import" || arg == "--smoke-import-fail" ||
                arg == "--smoke-import-stale" || arg == "--smoke-import-close" ||
                arg == "--smoke-import-undo") &&
               index + 1 < argc) {
      smoke_import = arg == "--smoke-import";
      smoke_import_fail = arg == "--smoke-import-fail";
      smoke_import_stale = arg == "--smoke-import-stale";
      smoke_import_close = arg == "--smoke-import-close";
      smoke_import_undo = arg == "--smoke-import-undo";
      smoke_import_source = argv[++index];
    } else if (arg == "--smoke-verify-import") {
      smoke_verify_import = true;
    } else if (arg == "--smoke-without-returns") {
      smoke_without_returns = true;
    } else if (arg == "--smoke-audio-gestures") {
      smoke_audio_gestures = true;
    } else if (arg == "--smoke-verify-audio-gestures") {
      smoke_verify_audio_gestures = true;
    } else if (arg == "--smoke-waveform") {
      smoke_waveform = true;
    } else if (arg == "--smoke-tempo" && index + 1 < argc) {
      smoke_tempo_mode = argv[++index];
    } else if (arg == "--smoke-tempo-drop" && index + 1 < argc) {
      // A WAV dropped in this session: analysed, then set to Follow.
      smoke_tempo_mode = "dropped";
      smoke_tempo_drop = argv[++index];
    } else if (arg == "--smoke-live-edit") {
      smoke_live_edit = true;
    } else if (arg == "--smoke-live-stale") {
      smoke_live_stale = true;
    } else if (arg == "--smoke-live-fail") {
      smoke_live_fail = true;
    } else if (arg == "--smoke-live-close") {
      smoke_live_close = true;
    } else if (arg == "--smoke-live-stop") {
      smoke_live_stop = true;
    } else if (arg == "--smoke-verify-live-edit") {
      smoke_verify_live_edit = true;
    } else if (arg == "--smoke-automation") {
      smoke_automation = true;
    } else if (arg == "--smoke-send-automation") {
      smoke_send_automation = true;
    } else if (arg == "--smoke-verify-send-automation") {
      smoke_verify_send_automation = true;
    } else if (arg == "--smoke-tempo-map") {
      smoke_tempo_map = true;
    } else if (arg == "--smoke-verify-tempo-map") {
      smoke_verify_tempo_map = true;
    } else if (arg == "--smoke-routing") {
      smoke_routing = true;
    } else if (arg == "--smoke-rate-audio") {
      smoke_rate_audio = true;
    } else if (arg == "--smoke-grid-source" && index + 1 < argc) {
      smoke_grid = true;
      smoke_grid_source = argv[++index];
    } else if (arg == "--smoke-chain-ui") {
      smoke_chain_ui = true;
    } else if (arg == "--qualify-pipewire" && index + 1 < argc) {
      qualify_pipewire_serial = argv[++index];
    } else if (arg == "--qualify-buffer-frames" && index + 1 < argc) {
      qualify_buffer_frames = std::stoi(argv[++index]);
    } else if (arg == "--qualify-master-limiter") {
      qualify_master_limiter = true;
    } else if (arg == "--smoke-routing-source" && index + 1 < argc) {
      smoke_routing_source = argv[++index];
    } else if (arg == "--smoke-verify-routing") {
      smoke_verify_routing = true;
    } else if (arg == "--smoke-vst3-browser" && index + 1 < argc) {
      smoke_vst3_browser_bundle = argv[++index];
    } else if (arg == "--smoke-vst3-two-effects") {
      smoke_vst3_two_effects = true;
    } else if (arg == "--smoke-vst3-instrument" && index + 1 < argc) {
      smoke_vst3_instrument_bundle = argv[++index];
    } else if (arg == "--smoke-vst3-browser-disabled") {
      smoke_vst3_browser_disabled = true;
    } else if (arg == "--smoke-verify-vst3-browser") {
      smoke_verify_vst3_browser = true;
    } else if (arg == "--smoke-verify-vst3-instrument") {
      smoke_verify_vst3_instrument = true;
    } else if (arg == "--smoke-create-from-welcome" && index + 1 < argc) {
      welcome_create_parent = argv[++index];
    } else if (arg == "--smoke-home" && index + 1 < argc) {
      smoke_home = argv[++index];
    } else if (arg == "--smoke-open-from-welcome" && index + 1 < argc) {
      welcome_open_folder = argv[++index];
    } else if (arg == "--screenshot" && index + 1 < argc) {
      screenshot = argv[++index];
    } else {
      std::cerr << "Usage: oscillate [[--create|--open] FOLDER] [--smoke]"
                   " [--screenshot PNG]\n";
      return 2;
    }
  }
  if ((!screenshot.empty() && !smoke) ||
      (smoke_grid && (!smoke || !create || root.empty() || screenshot.empty())) ||
      (smoke_chain_ui && (!smoke_grid || !smoke || !create || root.empty())) ||
      (!qualify_pipewire_serial.empty() && (smoke || create || root.empty())) ||
      (qualify_master_limiter && qualify_pipewire_serial.empty()) ||
      (qualify_buffer_frames != 64 && qualify_buffer_frames != 128) ||
      (smoke_output_rate != 0 &&
       (smoke_output_rate < 1 || smoke_output_rate > 4 || !(smoke_export || smoke_stems))) ||
      ((smoke_midi_workflow || smoke_verify_midi || smoke_transport || smoke_transport_fail ||
        smoke_audio_device || smoke_export || smoke_export_fail || smoke_export_close ||
        smoke_stems || smoke_import || smoke_import_fail || smoke_import_stale ||
        smoke_import_close || smoke_import_undo || smoke_verify_import || smoke_audio_gestures ||
        smoke_waveform || !smoke_tempo_mode.empty() || smoke_verify_audio_gestures ||
        smoke_live_edit || smoke_live_stale || smoke_live_fail || smoke_live_close ||
        smoke_live_stop || smoke_verify_live_edit || smoke_automation || smoke_send_automation ||
        smoke_verify_send_automation || smoke_tempo_map || smoke_verify_tempo_map ||
        smoke_routing || smoke_rate_audio || smoke_verify_routing ||
        !smoke_vst3_browser_bundle.empty() || !smoke_vst3_instrument_bundle.empty() ||
        smoke_vst3_browser_disabled || smoke_verify_vst3_browser || smoke_verify_vst3_instrument) &&
       !smoke) ||
      ((smoke_export || smoke_export_fail || smoke_export_close) &&
       (root.empty() || create || smoke_export_format < 0 ||
        smoke_export_format > (OSCILLATE_HAVE_CODEC ? 5 : 2))) ||
      (smoke_stems &&
       (root.empty() || create || smoke_export || smoke_export_fail || smoke_export_close ||
        smoke_import || smoke_import_fail || smoke_import_stale || smoke_import_close ||
        smoke_import_undo || smoke_waveform || !smoke_tempo_mode.empty() || smoke_midi_workflow ||
        smoke_transport || smoke_transport_fail || smoke_audio_device)) ||
      (static_cast<int>(smoke_export) + static_cast<int>(smoke_export_fail) +
           static_cast<int>(smoke_export_close) >
       1) ||
      (static_cast<int>(smoke_import) + static_cast<int>(smoke_import_fail) +
           static_cast<int>(smoke_import_stale) + static_cast<int>(smoke_import_close) +
           static_cast<int>(smoke_import_undo) + static_cast<int>(smoke_verify_import) +
           static_cast<int>(smoke_audio_gestures) + static_cast<int>(smoke_verify_audio_gestures) +
           static_cast<int>(smoke_waveform) + static_cast<int>(!smoke_tempo_mode.empty()) +
           static_cast<int>(smoke_live_edit) + static_cast<int>(smoke_live_stale) +
           static_cast<int>(smoke_live_fail) + static_cast<int>(smoke_live_close) +
           static_cast<int>(smoke_live_stop) + static_cast<int>(smoke_verify_live_edit) +
           static_cast<int>(smoke_automation) + static_cast<int>(smoke_send_automation) +
           static_cast<int>(smoke_verify_send_automation) + static_cast<int>(smoke_tempo_map) +
           static_cast<int>(smoke_verify_tempo_map) + static_cast<int>(smoke_routing) +
           static_cast<int>(smoke_rate_audio) + static_cast<int>(smoke_verify_routing) >
       1) ||
      ((smoke_import || smoke_import_fail || smoke_import_stale || smoke_import_close ||
        smoke_import_undo || smoke_verify_import || smoke_audio_gestures || smoke_waveform ||
        !smoke_tempo_mode.empty() || smoke_verify_audio_gestures || smoke_live_edit ||
        smoke_live_stale || smoke_live_fail || smoke_live_close || smoke_live_stop ||
        smoke_verify_live_edit) &&
       (smoke_export || smoke_export_fail || smoke_export_close || smoke_transport ||
        smoke_transport_fail || smoke_midi_workflow || smoke_verify_midi)) ||
      ((smoke_transport || smoke_transport_fail || smoke_audio_device) &&
       (!create || root.empty())) ||
      (static_cast<int>(smoke_transport) + static_cast<int>(smoke_transport_fail) +
           static_cast<int>(smoke_audio_device) >
       1) ||
      (smoke_midi_workflow && (smoke_verify_midi || !create || root.empty())) ||
      (smoke_verify_midi && (create || root.empty())) ||
      (smoke_automation && (!create || root.empty())) ||
      (smoke_send_automation && (!create || root.empty())) ||
      (smoke_verify_send_automation && (create || root.empty())) ||
      (smoke_tempo_map && (!create || root.empty())) ||
      (smoke_verify_tempo_map && (create || root.empty())) ||
      (smoke_routing && (!create || root.empty() || smoke_routing_source.empty())) ||
      (smoke_rate_audio && (!create || root.empty() || smoke_routing_source.empty())) ||
      (smoke_verify_routing && (create || root.empty())) ||
      (smoke_vst3_two_effects && smoke_vst3_browser_bundle.empty()) ||
      (!smoke_vst3_browser_bundle.empty() &&
       (!create || root.empty() || smoke_routing_source.empty())) ||
      (!smoke_vst3_instrument_bundle.empty() && (!create || root.empty())) ||
      (smoke_vst3_browser_disabled && (!create || root.empty())) ||
      (smoke_verify_vst3_browser && (create || root.empty())) ||
      (smoke_verify_vst3_instrument && (create || root.empty())) ||
      ((smoke_import || smoke_import_fail || smoke_import_stale || smoke_import_close ||
        smoke_import_undo || smoke_verify_import || smoke_audio_gestures || smoke_waveform ||
        !smoke_tempo_mode.empty() || smoke_verify_audio_gestures || smoke_live_edit ||
        smoke_live_stale || smoke_live_fail || smoke_live_close || smoke_live_stop ||
        smoke_verify_live_edit) &&
       (root.empty() || create)) ||
      (!smoke_tempo_mode.empty() &&
       ((smoke_tempo_mode != "generated" && smoke_tempo_mode != "cached" &&
         smoke_tempo_mode != "rebuilt" && smoke_tempo_mode != "dropped") ||
        root.empty() || create)) ||
      ((!welcome_create_parent.empty() || !welcome_open_folder.empty()) && !smoke) ||
      (!smoke_home.empty() && !smoke) || (!smoke_home.empty() && !welcome_create_parent.empty()) ||
      (!welcome_create_parent.empty() && !welcome_open_folder.empty()) ||
      (!root.empty() && (!welcome_create_parent.empty() || !welcome_open_folder.empty()))) {
    std::cerr << "Invalid argument combination\n";
    return 2;
  }

  oscillate::application::PlaybackOutputFactory output_factory;
  auto smoke_device_state = smoke_transport || smoke_audio_device || smoke_routing ||
                                    smoke_rate_audio || smoke_verify_vst3_instrument
                                ? std::make_shared<SmokeDeviceState>()
                                : nullptr;
  if (smoke_device_state) {
    smoke_device_state->ui_thread = std::this_thread::get_id();
    smoke_device_state->meter_full_window =
        smoke_transport || smoke_audio_device || smoke_routing || smoke_rate_audio;
  }
  if (smoke_transport || smoke_transport_fail || smoke_live_edit || smoke_live_stale ||
      smoke_live_fail || smoke_live_close || smoke_live_stop || smoke_audio_device ||
      smoke_routing || smoke_rate_audio || smoke_verify_vst3_instrument) {
    output_factory = [fail = smoke_transport_fail,
                      smoke_device_state](oscillate::audio_io::StereoRenderSource source,
                                          oscillate::audio_io::TransportCursor& cursor) {
      return std::make_unique<SmokePlaybackOutput>(source, cursor, fail, smoke_device_state);
    };
  }
  oscillate::quick::ProjectBridge::AudioDeviceDiscovery device_discovery;
  oscillate::application::PlaybackOutputFactory jack_output_factory;
  if (smoke_audio_device) {
    jack_output_factory = [smoke_device_state](oscillate::audio_io::StereoRenderSource source,
                                               oscillate::audio_io::TransportCursor& cursor) {
      return std::make_unique<SmokePlaybackOutput>(source, cursor, false, smoke_device_state, 1);
    };
    device_discovery = [smoke_device_state] {
      if (smoke_device_state->discovery_failure.load(std::memory_order_acquire)) {
        return oscillate::foundation::Result<std::vector<oscillate::audio_io::PipeWireSink>>::
            failure(
                {oscillate::foundation::ErrorCode::unavailable, "Fixture output discovery failed"});
      }
      if (smoke_device_state->hide_selected.load(std::memory_order_acquire)) {
        return oscillate::foundation::Result<
            std::vector<oscillate::audio_io::PipeWireSink>>::success({{202, "Headphones"}});
      }
      return oscillate::foundation::Result<std::vector<oscillate::audio_io::PipeWireSink>>::success(
          {{101, "Studio monitors"}, {202, "Headphones"}});
    };
  }
  if (smoke_audio_device)
    QSettings{}.setValue(QStringLiteral("audioBackend"), QStringLiteral("pipewire"));
  oscillate::quick::ProjectBridge::Dependencies dependencies;
#ifdef OSCILLATE_QUICK_TEST_INPUT
  if (smoke_live_stale) {
    dependencies.before_live_edit_handoff =
        [](oscillate::application::ProjectSession& session,
           oscillate::application::PreparedInstrumentPlayback&) {
          if (!session.tracks().add_instrument_track() || !session.history().undo()) {
            throw std::runtime_error("Could not invalidate prepared live edit for fixture");
          }
        };
  } else if (smoke_live_fail) {
    dependencies.before_live_edit_handoff =
        [](oscillate::application::ProjectSession&,
           oscillate::application::PreparedInstrumentPlayback& prepared) {
          if (!prepared.audio_clip) {
            throw std::runtime_error("Failed handoff fixture omitted prepared audio");
          }
          prepared.audio_clip->project_end_frame = prepared.audio_clip->project_start_frame;
        };
  }
#endif
  // Close and Stop are checked against a handoff that has finished but not
  // yet been installed: the edit runs inline, so it is complete when the edit
  // call returns and the next playback poll has not happened.
  if (smoke_live_close || smoke_live_stop)
    dependencies.audio_edit_runner = [](const std::function<void()>& job) {
      job();
      return std::jthread{};
    };
  oscillate::quick::ProjectBridge bridge(
      welcome_create_parent.empty() ? smoke_home : welcome_create_parent, nullptr,
      std::move(output_factory), std::move(device_discovery), jack_output_factory,
      smoke_audio_device ? std::function<bool()>([] { return true; }) : std::function<bool()>{},
      std::move(dependencies));
  if (!root.empty()) {
    auto session = create ? oscillate::application::ProjectSession::create(root)
                          : oscillate::application::ProjectSession::open(root);
    if (!session) {
      std::cerr << "Project: " << session.error().message << '\n';
      return 1;
    }
    if (smoke_grid) {
      const int fd = ::open(smoke_grid_source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
      if (fd < 0) {
        std::cerr << "Grid smoke: cannot open source WAV\n";
        return 1;
      }
      const auto imported = session.value()->imports().import_wav_audio(fd);
      (void)::close(fd);
      if (!imported || !session.value()->tracks().add_instrument_track() ||
          !session.value()->saving().save()) {
        std::cerr << "Grid smoke: could not create audio and empty instrument rows\n";
        return 1;
      }
      if (smoke_chain_ui) {
        if (!session.value()->tracks().add_bus_track(oscillate::project::TrackKind::group_bus) ||
            !session.value()->tracks().add_bus_track(oscillate::project::TrackKind::return_bus) ||
            !session.value()->effects().add_effect_engine(oscillate::project::MasterSignalPath{}, 0,
                                                          "shape") ||
            !session.value()->effects().add_effect_engine(oscillate::project::MasterSignalPath{}, 1,
                                                          "press")) {
          std::cerr << "Chain UI smoke: could not prepare bus and native effect fixture\n";
          return 1;
        }
        for (int index = 0; index < 7; ++index) {
          if (!session.value()->tracks().add_instrument_track()) {
            std::cerr << "Chain UI smoke: could not prepare scrolling tracks\n";
            return 1;
          }
        }
        if (!session.value()->saving().save()) {
          std::cerr << "Chain UI smoke: could not save fixture\n";
          return 1;
        }
      }
    } else if (smoke && create &&
               (smoke_routing || smoke_rate_audio || !smoke_vst3_browser_bundle.empty())) {
      const int fd = ::open(smoke_routing_source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
      if (fd < 0) {
        std::cerr << "Routing smoke: cannot open source WAV\n";
        return 1;
      }
      auto imported = session.value()->imports().import_wav_audio(fd);
      (void)::close(fd);
      if (!imported) {
        std::cerr << "Routing smoke import: " << imported.error().message << '\n';
        return 1;
      }
    } else if (smoke && create && !smoke_midi_workflow) {
      const auto created = session.value()->tracks().add_instrument_track();
      if (!created) {
        std::cerr << "Smoke fixture: " << created.error().message << '\n';
        return 1;
      }
      const auto track = created.value();
      auto added = session.value()->midi_clips().add_midi_clip(track, {0});
      if (!added) {
        std::cerr << "Smoke fixture: " << added.error().message << '\n';
        return 1;
      }
      const auto& fixture_track =
          session.value()->snapshot().tracks[session.value()->tracks().track_index(track).value()];
      const auto clip = fixture_track.midi_clips.back().id;
      added = session.value()->notes().add_note(clip, {0}, 60);
      if (!added) {
        std::cerr << "Smoke fixture: " << added.error().message << '\n';
        return 1;
      }
      added = session.value()->saving().save();
      if (!added) {
        std::cerr << "Smoke fixture save: " << added.error().message << '\n';
        return 1;
      }
    }
    if (smoke_without_returns) {
      std::vector<oscillate::project::TrackId> returns;
      for (const auto& track : session.value()->snapshot().tracks) {
        if (track.kind == oscillate::project::TrackKind::return_bus)
          returns.push_back(track.id);
      }
      for (const auto track : returns) {
        if (!session.value()->tracks().remove_track(track)) {
          std::cerr << "Smoke fixture: could not remove a return track\n";
          return 1;
        }
      }
      if (!session.value()->saving().save()) {
        std::cerr << "Smoke fixture: could not save without returns\n";
        return 1;
      }
    }
    if (smoke_audio_gestures) {
      const auto& snapshot = session.value()->snapshot();
      // The welcome fixture also holds its two returns, below the import.
      using oscillate::project::Track;
      using oscillate::project::TrackKind;
      if (std::ranges::count(snapshot.tracks, TrackKind::audio, &Track::kind) != 1 ||
          snapshot.tracks.front().kind != TrackKind::audio ||
          snapshot.tracks.front().clips.size() != 1) {
        std::cerr << "Audio gesture smoke: expected one imported clip\n";
        return 1;
      }
      const auto second = snapshot.meter.tick_at({3, 1, 0});
      if (!second) {
        std::cerr << "Audio gesture smoke: bar 3 unavailable\n";
        return 1;
      }
      const auto duplicated = session.value()->audio_clips().duplicate_audio_clip(
          snapshot.tracks.front().clips.front().id, second.value());
      if (!duplicated || !session.value()->saving().save()) {
        std::cerr << "Audio gesture smoke: could not place collision fixture\n";
        return 1;
      }
    }
    bridge.set_project(std::move(session.value()), root);
  }
  QQmlApplicationEngine engine;
  engine.rootContext()->setContextProperty("projectBridge", &bridge);
  engine.load(QUrl(QStringLiteral("qrc:/DawShell.qml")));
  if (engine.rootObjects().isEmpty()) {
    std::cerr << "Qt Quick shell failed to load\n";
    return 1;
  }
  if (smoke_grid) {
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
    if (!window)
      return 1;
    window->setWidth(1920);
    window->setHeight(1080);
    auto* cache_button = more_menu_item(window, QStringLiteral("openCachePanel"));
    if (!cache_button) {
      std::cerr << "Cache panel smoke: More menu control missing\n";
      return 1;
    }
    cache_button->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Return);
    QCoreApplication::processEvents();
    if (!first_visible_item(window->contentItem(), QStringLiteral("cachePanel")) ||
        !first_visible_item(window->contentItem(), QStringLiteral("cacheBudgetInput"))) {
      std::cerr << "Cache panel smoke: controls did not open\n";
      return 1;
    }
    engine.rootObjects().front()->setProperty("cachePanelOpen", false);
  }
  if (!smoke_vst3_browser_bundle.empty()) {
#if defined(OSCILLATE_QUICK_TEST_INPUT) && OSCILLATE_HAVE_VST3
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
    auto* open = window ? more_menu_item(window, QStringLiteral("openVst3Browser")) : nullptr;
    if (!window || !open || bridge.tracks().size() != 1 ||
        bridge.tracks().front().toMap().value(QStringLiteral("kind")).toString() !=
            QStringLiteral("audio")) {
      std::cerr << "VST3 browser smoke: audio target or drawer control missing\n";
      return 1;
    }
    open->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Return);
    QCoreApplication::processEvents();
    auto* panel = first_visible_item(window->contentItem(), QStringLiteral("vst3BrowserPanel"));
    auto* path = first_visible_item(window->contentItem(), QStringLiteral("vst3AdditionalPath"));
    if (!panel || !path) {
      std::cerr << "VST3 browser smoke: searchable drawer did not open\n";
      return 1;
    }
    if (!QTest::qWaitFor([&bridge] { return !bridge.vst3_browser()->busy(); }, 15'000)) {
      std::cerr << "VST3 browser smoke: initial scan did not finish\n";
      return 1;
    }
    path->setProperty("text", QString::fromStdString(smoke_vst3_browser_bundle.string()));
    path->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Return);
    const auto selected_entry = [&bridge, &smoke_vst3_browser_bundle] {
      const auto entries = bridge.vst3_browser()->entries();
      const auto found =
          std::ranges::find_if(entries, [&smoke_vst3_browser_bundle](const auto& item) {
            return item.toMap().value(QStringLiteral("bundle")).toString() ==
                   QString::fromStdString(smoke_vst3_browser_bundle.string());
          });
      return found == entries.end() ? QVariantMap{} : found->toMap();
    };
    if (!QTest::qWaitFor(
            [&bridge, &selected_entry] {
              return !bridge.vst3_browser()->busy() && !selected_entry().isEmpty();
            },
            15'000)) {
      std::cerr << "VST3 browser smoke: worker scan omitted selected bundle: "
                << bridge.vst3_browser()->status().toStdString() << '\n';
      return 1;
    }
    if (selected_entry().value(QStringLiteral("state")).toString() != QStringLiteral("ready")) {
      std::cerr << "VST3 browser smoke: worker scan failed: "
                << selected_entry().value(QStringLiteral("diagnostic")).toString().toStdString()
                << '\n';
      return 1;
    }
    auto* search = first_visible_item(window->contentItem(), QStringLiteral("vst3BrowserSearch"));
    auto* results = first_visible_item(window->contentItem(), QStringLiteral("vst3BrowserResults"));
    if (!search || !results) {
      std::cerr << "VST3 browser smoke: search controls missing\n";
      return 1;
    }
    search->setProperty("text", QStringLiteral("no matching effect"));
    QCoreApplication::processEvents();
    if (results->property("count").toInt() != 0) {
      std::cerr << "VST3 browser smoke: search did not filter results\n";
      return 1;
    }
    search->setProperty("text", QStringLiteral("Loaded Gain"));
    QCoreApplication::processEvents();
    auto* insert = first_visible_item(window->contentItem(), QStringLiteral("vst3EntryAction"));
    if (!insert || !insert->isEnabled()) {
      std::cerr << "VST3 browser smoke: verified insert action missing\n";
      return 1;
    }
    const auto before = bridge.revision();
    insert->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Return);
    bridge.add_instrument_track();
    if (!QTest::qWaitFor([&bridge] { return !bridge.vst3_browser()->busy(); }, 10'000) ||
        !bridge.vst3_browser()->status().contains(QStringLiteral("Project changed")) ||
        !bridge.tracks().front().toMap().value(QStringLiteral("vst3Plugins")).toList().empty()) {
      std::cerr << "VST3 browser smoke: stale verification changed project model\n";
      return 1;
    }
    bridge.undo();
    if (bridge.revision() != before + 2 || bridge.tracks().size() != 1) {
      std::cerr << "VST3 browser smoke: stale fixture could not restore audio target\n";
      return 1;
    }
    insert = first_visible_item(window->contentItem(), QStringLiteral("vst3EntryAction"));
    if (!insert || !insert->isEnabled()) {
      std::cerr << "VST3 browser smoke: effect action did not recover after stale revision\n";
      return 1;
    }
    insert->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Return);
    if (!QTest::qWaitFor(
            [&bridge, before] {
              return !bridge.vst3_browser()->busy() && bridge.revision() == before + 3;
            },
            10'000)) {
      std::cerr << "VST3 browser smoke: insert did not commit: "
                << bridge.vst3_browser()->status().toStdString() << '\n';
      return 1;
    }
    const auto inserted =
        bridge.tracks().front().toMap().value(QStringLiteral("vst3Plugins")).toList();
    if (inserted.size() != 1 || inserted.front().toMap().value(QStringLiteral("name")).toString() !=
                                    QStringLiteral("Loaded Gain")) {
      std::cerr << "VST3 browser smoke: saved track insert projection missing\n";
      return 1;
    }
    bridge.undo();
    if (!bridge.tracks().front().toMap().value(QStringLiteral("vst3Plugins")).toList().empty()) {
      std::cerr << "VST3 browser smoke: undo retained insert\n";
      return 1;
    }
    bridge.redo();
    bridge.save();
    if (bridge.tracks().front().toMap().value(QStringLiteral("vst3Plugins")).toList().size() != 1 ||
        !bridge.status().startsWith(QStringLiteral("Saved"))) {
      std::cerr << "VST3 browser smoke: redo or save failed\n";
      return 1;
    }
    if (smoke_vst3_two_effects) {
      const auto first_track = bridge.tracks().front().toMap();
      const auto first_plugin_id = first_track.value(QStringLiteral("vst3Plugins"))
                                       .toList()
                                       .front()
                                       .toMap()
                                       .value(QStringLiteral("id"))
                                       .toString();
      const auto first_revision = bridge.revision();
      insert = first_visible_item(window->contentItem(), QStringLiteral("vst3EntryAction"));
      if (!insert || !insert->isEnabled()) {
        std::cerr << "VST3 browser smoke: serial second effect action disabled\n";
        return 1;
      }
      insert->forceActiveFocus();
      QTest::keyClick(window, Qt::Key_Return);
      if (!QTest::qWaitFor(
              [&bridge, first_revision] {
                return !bridge.vst3_browser()->busy() && bridge.revision() == first_revision + 1;
              },
              10'000)) {
        std::cerr << "VST3 browser smoke: serial second effect did not commit: "
                  << bridge.vst3_browser()->status().toStdString() << '\n';
        return 1;
      }
      const auto serial_plugins =
          bridge.tracks().front().toMap().value(QStringLiteral("vst3Plugins")).toList();
      const auto serial_chain =
          bridge.insert_chains().front().toMap().value(QStringLiteral("effects")).toList();
      if (serial_plugins.size() != 2 || serial_chain.size() != 2 ||
          serial_plugins[0].toMap().value(QStringLiteral("id")).toString() != first_plugin_id ||
          serial_plugins[1].toMap().value(QStringLiteral("id")).toString() == first_plugin_id ||
          serial_chain[0].toMap().value(QStringLiteral("pluginId")).toString() != first_plugin_id ||
          serial_chain[1].toMap().value(QStringLiteral("pluginId")).toString() !=
              serial_plugins[1].toMap().value(QStringLiteral("id")).toString()) {
        std::cerr << "VST3 browser smoke: serial effects changed saved order or identity\n";
        return 1;
      }
      const auto import_source =
          QUrl::fromLocalFile(QString::fromStdString(smoke_routing_source.string()));
      bridge.imports()->import_audio(import_source);
      if (!QTest::qWaitFor([&bridge] { return !bridge.imports()->import_busy(); }, 10'000) ||
          bridge.tracks().size() != 2) {
        std::cerr << "VST3 browser smoke: extra audio track import failed\n";
        return 1;
      }
      engine.rootObjects().front()->setProperty("selectedTrack", 1);
      QCoreApplication::processEvents();
      insert = first_visible_item(window->contentItem(), QStringLiteral("vst3EntryAction"));
      if (!insert || !insert->isEnabled()) {
        std::cerr << "VST3 browser smoke: a third effect on another track was not offered\n";
        return 1;
      }
      const auto before_third = bridge.revision();
      bridge.vst3_browser()->insert_vst3_effect(
          selected_entry().value(QStringLiteral("index")).toInt(),
          selected_entry().value(QStringLiteral("classId")).toString(),
          bridge.tracks().back().toMap().value(QStringLiteral("id")).toString());
      // Any track takes effects, however many the project has (ADR-0050).
      if (!QTest::qWaitFor(
              [&bridge, before_third] {
                return !bridge.vst3_browser()->busy() && bridge.revision() == before_third + 1;
              },
              10'000)) {
        std::cerr << "VST3 browser smoke: a third effect on another track did not commit: "
                  << bridge.vst3_browser()->status().toStdString() << '\n';
        return 1;
      }
      bridge.save();
      if (!bridge.status().startsWith(QStringLiteral("Saved"))) {
        std::cerr << "VST3 browser smoke: serial effect project save failed\n";
        return 1;
      }
      std::cout << "Qt Quick VST3 browser serial effects and a third on another track\n";
      return 0;
    }
    if (!screenshot.empty() &&
        !window->grabWindow().save(QString::fromStdString(screenshot), "PNG")) {
      std::cerr << "VST3 browser smoke: screenshot could not be saved\n";
      return 1;
    }
    std::cout << "Qt Quick VST3 browser verified insert, undo, redo and save\n";
    return 0;
#else
    std::cerr << "VST3 browser smoke requires enabled VST3 and Qt Test input\n";
    return 1;
#endif
  }
  if (!smoke_vst3_instrument_bundle.empty()) {
#if defined(OSCILLATE_QUICK_TEST_INPUT) && OSCILLATE_HAVE_VST3
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
    auto* open = window ? more_menu_item(window, QStringLiteral("openVst3Browser")) : nullptr;
    if (!window || !open || bridge.tracks().size() != 1 ||
        bridge.tracks().front().toMap().value(QStringLiteral("kind")).toString() !=
            QStringLiteral("instrument")) {
      std::cerr << "VST3 instrument smoke: MIDI target or drawer control missing\n";
      return 1;
    }
    open->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Return);
    QCoreApplication::processEvents();
    auto* panel = first_visible_item(window->contentItem(), QStringLiteral("vst3BrowserPanel"));
    auto* path = first_visible_item(window->contentItem(), QStringLiteral("vst3AdditionalPath"));
    if (!panel || !path ||
        !QTest::qWaitFor([&bridge] { return !bridge.vst3_browser()->busy(); }, 15'000)) {
      std::cerr << "VST3 instrument smoke: drawer or initial scan unavailable\n";
      return 1;
    }
    path->setProperty("text", QString::fromStdString(smoke_vst3_instrument_bundle.string()));
    path->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Return);
    const auto selected_entry = [&bridge, &smoke_vst3_instrument_bundle] {
      const auto entries = bridge.vst3_browser()->entries();
      const auto found =
          std::ranges::find_if(entries, [&smoke_vst3_instrument_bundle](const auto& item) {
            const auto row = item.toMap();
            return row.value(QStringLiteral("bundle")).toString() ==
                       QString::fromStdString(smoke_vst3_instrument_bundle.string()) &&
                   row.value(QStringLiteral("kind")).toString() == QStringLiteral("instrument");
          });
      return found == entries.end() ? QVariantMap{} : found->toMap();
    };
    if (!QTest::qWaitFor(
            [&bridge, &selected_entry] {
              return !bridge.vst3_browser()->busy() && !selected_entry().isEmpty();
            },
            15'000) ||
        selected_entry().value(QStringLiteral("state")).toString() != QStringLiteral("ready")) {
      std::cerr << "VST3 instrument smoke: fixture scan failed: "
                << bridge.vst3_browser()->status().toStdString() << '\n';
      return 1;
    }
    auto* search = first_visible_item(window->contentItem(), QStringLiteral("vst3BrowserSearch"));
    auto* results = first_visible_item(window->contentItem(), QStringLiteral("vst3BrowserResults"));
    if (!search || !results) {
      std::cerr << "VST3 instrument smoke: browser search unavailable\n";
      return 1;
    }
    search->setProperty("text", QStringLiteral("no such instrument"));
    QCoreApplication::processEvents();
    if (results->property("count").toInt() != 0) {
      std::cerr << "VST3 instrument smoke: search filter failed\n";
      return 1;
    }
    search->setProperty("text", QStringLiteral("Timed Instrument"));
    QCoreApplication::processEvents();
    auto* insert = first_visible_item(window->contentItem(), QStringLiteral("vst3EntryAction"));
    if (!insert || !insert->isEnabled()) {
      std::cerr << "VST3 instrument smoke: insert action unavailable\n";
      return 1;
    }
    const auto before = bridge.revision();
    const auto track = bridge.tracks().front().toMap().value(QStringLiteral("id")).toString();
    bridge.vst3_browser()->insert_vst3_effect(
        selected_entry().value(QStringLiteral("index")).toInt(),
        selected_entry().value(QStringLiteral("classId")).toString(), track);
    if (bridge.revision() != before) {
      std::cerr << "VST3 instrument smoke: effect action accepted an instrument target\n";
      return 1;
    }
    insert->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Return);
    if (!QTest::qWaitFor(
            [&bridge, before] {
              return !bridge.vst3_browser()->busy() && bridge.revision() == before + 1;
            },
            10'000)) {
      std::cerr << "VST3 instrument smoke: verified insert failed: "
                << bridge.vst3_browser()->status().toStdString() << '\n';
      return 1;
    }
    const auto inserted =
        bridge.tracks().front().toMap().value(QStringLiteral("vst3Plugins")).toList();
    if (inserted.size() != 1 || inserted.front().toMap().value(QStringLiteral("name")).toString() !=
                                    QStringLiteral("Timed Instrument")) {
      std::cerr << "VST3 instrument smoke: saved insert projection missing\n";
      return 1;
    }
    bridge.undo();
    if (!bridge.tracks().front().toMap().value(QStringLiteral("vst3Plugins")).toList().empty()) {
      std::cerr << "VST3 instrument smoke: undo retained insert\n";
      return 1;
    }
    bridge.redo();
    bridge.save();
    std::cout << "Qt Quick VST3 instrument verified insert, undo, redo and save\n";
    return 0;
#else
    std::cerr << "VST3 instrument smoke requires enabled VST3 and Qt Test input\n";
    return 1;
#endif
  }
  if (smoke_verify_vst3_instrument) {
#if defined(OSCILLATE_QUICK_TEST_INPUT) && OSCILLATE_HAVE_VST3
    const auto tracks = bridge.tracks();
    const auto saved = tracks.empty()
                           ? QVariantList{}
                           : tracks.front().toMap().value(QStringLiteral("vst3Plugins")).toList();
    if (saved.size() != 1 || saved.front().toMap().value(QStringLiteral("name")).toString() !=
                                 QStringLiteral("Timed Instrument")) {
      std::cerr << "VST3 instrument smoke: saved insert did not reopen\n";
      return 1;
    }
    bridge.toggle_playback();
    if (!QTest::qWaitFor(
            [&bridge] {
              return bridge.playback_status().contains(QStringLiteral("Playing routed revision"));
            },
            10'000) ||
        !bridge.playback_active() ||
        std::fabs(smoke_device_state->first_sample.load(std::memory_order_acquire) -
                  0.5F * 100.0F / 127.0F) > 0.00001F ||
        smoke_device_state->first_completed_sample.load(std::memory_order_acquire) != 64) {
      std::cerr << "VST3 instrument smoke: saved MIDI was not audible after reopen: "
                << bridge.playback_status().toStdString() << '\n';
      return 1;
    }
    bridge.toggle_playback();
    if (!QTest::qWaitFor([&bridge] { return !bridge.playback_active(); }, 10'000) ||
        !smoke_device_state->stopped_off_ui.load(std::memory_order_acquire)) {
      std::cerr << "VST3 instrument smoke: playback did not stop safely\n";
      return 1;
    }
    std::cout << "Qt Quick VST3 instrument reopened and played project MIDI\n";
    return 0;
#else
    std::cerr << "VST3 instrument reopen smoke requires enabled VST3 and Qt Test input\n";
    return 1;
#endif
  }
  if (smoke_verify_vst3_browser) {
    const auto plugins =
        bridge.tracks().empty()
            ? QVariantList{}
            : bridge.tracks().front().toMap().value(QStringLiteral("vst3Plugins")).toList();
    if (plugins.size() != 1 || plugins.front().toMap().value(QStringLiteral("name")).toString() !=
                                   QStringLiteral("Loaded Gain")) {
      std::cerr << "VST3 browser smoke: saved insert did not reopen\n";
      return 1;
    }
    std::cout << "Qt Quick VST3 browser insert reopened\n";
    return 0;
  }
  if (smoke_vst3_browser_disabled) {
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
    if (!window || bridge.vst3_browser()->vst3_browser_available()) {
      std::cerr << "VST3 browser smoke: scanner-off build exposed browser\n";
      return 1;
    }
#ifdef OSCILLATE_QUICK_TEST_INPUT
    auto* open = more_menu_item(window, QStringLiteral("openVst3Browser"));
    if (!open) {
      std::cerr << "VST3 browser smoke: scanner-off drawer control missing\n";
      return 1;
    }
    open->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Return);
    QCoreApplication::processEvents();
    if (!first_visible_item(window->contentItem(), QStringLiteral("vst3BrowserPanel")) ||
        !first_visible_item(window->contentItem(), QStringLiteral("vst3BrowserStatus"))) {
      std::cerr << "VST3 browser smoke: scanner-off explanation missing\n";
      return 1;
    }
#endif
    const auto before = bridge.revision();
    bridge.vst3_browser()->refresh_vst3_catalogue();
    bridge.vst3_browser()->insert_vst3_effect(0, QStringLiteral("unavailable"),
                                              QStringLiteral("unavailable"));
    if (bridge.revision() != before ||
        !bridge.vst3_browser()->status().contains(QStringLiteral("unavailable"))) {
      std::cerr << "VST3 browser smoke: scanner-off insertion was not rejected\n";
      return 1;
    }
    std::cout << "Qt Quick VST3 browser unavailable build refused insertion\n";
    return 0;
  }
  if (!qualify_pipewire_serial.empty()) {
    if (qualify_master_limiter) {
      // Ceiling on the master at -12 dB, set through its panel's controller.
      const auto master_index =
          bridge.insert_chains().back().toMap().value(QStringLiteral("effects")).toList().size();
      bridge.add_effect_engine(QStringLiteral("master"), static_cast<int>(master_index),
                               QStringLiteral("ceiling"));
      const auto master_chain =
          bridge.insert_chains().back().toMap().value(QStringLiteral("effects")).toList();
      const auto ceiling_slot =
          master_chain.isEmpty()
              ? QString{}
              : master_chain.back().toMap().value(QStringLiteral("slotId")).toString();
      bridge.open_native_effect_editor(QStringLiteral("master"), ceiling_slot);
      double ceiling = 0.0;
      if (auto* controller = bridge.native_effect_controller()) {
        QMetaObject::invokeMethod(controller, "set_value",
                                  Q_ARG(QString, QStringLiteral("ceiling.ceiling")),
                                  Q_ARG(double, -12.0));
        QMetaObject::invokeMethod(controller, "value", Q_RETURN_ARG(double, ceiling),
                                  Q_ARG(QString, QStringLiteral("ceiling.ceiling")));
      }
      bridge.close_native_effect_editor();
      bridge.save();
      if (!bridge.status().startsWith(QStringLiteral("Saved")) || ceiling != -12.0) {
        std::cerr << "PipeWire qualification could not save limiter settings: "
                  << bridge.status().toStdString() << '\n';
        return 1;
      }
      std::cout << "Quick PipeWire qualified revision: " << bridge.revision() << '\n';
    }
    const auto serial = QString::fromStdString(qualify_pipewire_serial);
    bool valid_serial = false;
    if (serial.toULongLong(&valid_serial) == 0 || !valid_serial) {
      std::cerr << "PipeWire qualification requires a positive sink serial\n";
      return 2;
    }
    auto stage = std::make_shared<int>(0);
    auto elapsed = std::make_shared<QElapsedTimer>();
    elapsed->start();
    auto active_since = std::make_shared<qint64>(0);
    auto* poll = new QTimer(&app);
    poll->setInterval(20);
    QObject::connect(
        poll, &QTimer::timeout, &app,
        [&app, &bridge, serial, qualify_buffer_frames, stage, elapsed, active_since, poll] {
          const auto status = bridge.playback_status();
          if (elapsed->elapsed() > 12'000 ||
              status.contains(QStringLiteral("Routed output errors")) ||
              status.contains(QStringLiteral("Routed source failed")) ||
              status.contains(QStringLiteral("not active")) ||
              status.contains(QStringLiteral("Audio device lost")) ||
              status.contains(QStringLiteral("output format differs"))) {
            std::cerr << "PipeWire qualification failed: " << status.toStdString()
                      << "; device: " << bridge.audio_output()->audio_device_status().toStdString()
                      << '\n';
            app.exit(1);
            return;
          }
          if (*stage == 0) {
            if (bridge.audio_output()->audio_device_busy())
              return;
            bridge.audio_output()->select_audio_device(serial);
            bridge.audio_output()->select_buffer_frames(qualify_buffer_frames);
            if (bridge.audio_output()->selected_audio_device() != serial) {
              std::cerr << "PipeWire qualification could not select target: "
                        << bridge.audio_output()->audio_device_status().toStdString() << '\n';
              app.exit(1);
              return;
            }
            bridge.toggle_playback();
            *stage = 1;
          } else if (*stage == 1) {
            if (status.contains(QStringLiteral("Playing routed revision"))) {
              *active_since = elapsed->elapsed();
              *stage = 2;
            }
          } else if (*stage == 2 && elapsed->elapsed() - *active_since >= 2'000) {
            bool valid_sample = false;
            const auto sample = status.section(QStringLiteral(", sample "), 1)
                                    .section(QLatin1Char(' '), 0, 0)
                                    .toULongLong(&valid_sample);
            if (!status.contains(QStringLiteral("Playing routed revision")) || !valid_sample ||
                sample < 48'000) {
              std::cerr << "PipeWire qualification did not advance playback: "
                        << status.toStdString() << '\n';
              app.exit(1);
              return;
            }
            std::cout << "Quick PipeWire active: " << status.toStdString() << '\n';
            std::cout << "Quick PipeWire buffer: "
                      << bridge.audio_output()->buffer_size_status().toStdString() << '\n';
            bridge.toggle_playback();
            *stage = 3;
          } else if (*stage == 3 && !bridge.playback_active()) {
            if (!bridge.exports()->export_enabled() ||
                !status.contains(QStringLiteral("Playback stopped"))) {
              std::cerr << "PipeWire qualification Stop failed: " << status.toStdString() << '\n';
              app.exit(1);
              return;
            }
            std::cout << "Quick PipeWire stopped: " << status.toStdString() << '\n';
            poll->stop();
            app.quit();
          }
        });
    poll->start();
  }
  if (smoke_routing) {
    const quick_smoke::RoutingPlaybackProbe probe{
        smoke_device_state->first_sample, smoke_device_state->first_completed_sample,
        smoke_device_state->stopped_off_ui, smoke_device_state->delay_start};
    return quick_smoke::run_routing(app, bridge,
                                    qobject_cast<QQuickWindow*>(engine.rootObjects().front()),
                                    probe, first_visible_item, visible_item_with_label);
  }
  if (smoke_verify_routing) {
    return quick_smoke::verify_routing(bridge,
                                       qobject_cast<QQuickWindow*>(engine.rootObjects().front()),
                                       quick_smoke::count_visible_items);
  }
  if (smoke_rate_audio) {
    const auto tracks = bridge.tracks();
    if (tracks.size() != 1 ||
        tracks.front().toMap().value(QStringLiteral("clips")).toList().size() != 1 ||
        bridge.routing_active()) {
      std::cerr << "Source-rate smoke: fixture requires one audio clip without buses\n";
      return 1;
    }
    bridge.save();
    if (!bridge.status().startsWith(QStringLiteral("Saved"))) {
      std::cerr << "Source-rate smoke: project save failed\n";
      return 1;
    }
    bridge.toggle_playback();
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 5'000 &&
           !bridge.playback_status().contains(QStringLiteral("Playing routed revision"))) {
      app.processEvents();
      std::this_thread::yield();
    }
    if (!bridge.playback_active() ||
        !bridge.playback_status().contains(QStringLiteral("Playing routed revision")) ||
        smoke_device_state->first_completed_sample.load(std::memory_order_acquire) != 64 ||
        smoke_device_state->first_sample.load(std::memory_order_acquire) < 0.1F ||
        bridge.master_meter().value(QStringLiteral("leftPeak")).toDouble() < 0.1) {
      std::cerr << "Source-rate smoke: converted audio did not use routed playback: "
                << bridge.playback_status().toStdString() << '\n';
      return 1;
    }
    bridge.toggle_playback();
    timer.restart();
    while (timer.elapsed() < 5'000 && bridge.playback_active()) {
      app.processEvents();
      std::this_thread::yield();
    }
    if (bridge.playback_active() || !bridge.exports()->export_enabled()) {
      std::cerr << "Source-rate smoke: routed playback did not stop\n";
      return 1;
    }
    std::cout << "Qt Quick source-rate audio played without bus routing\n";
    return 0;
  }
  if (smoke_automation) {
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
    return quick_smoke::edit_arrangement_automation(bridge, window, first_visible_item);
  }
  if (smoke_send_automation) {
#ifdef OSCILLATE_QUICK_TEST_INPUT
    const auto source = bridge.tracks().front().toMap().value(QStringLiteral("id")).toString();
    bridge.add_bus_track(QStringLiteral("return"));
    if (bridge.tracks().size() != 2) {
      std::cerr << "Send automation smoke: bus creation failed\n";
      return 1;
    }
    const auto destination = bridge.tracks().back().toMap().value(QStringLiteral("id")).toString();
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
    if (window) {
      window->setProperty("contextPanelOpen", true);
      window->setProperty("selectedTrack", 0);
    }
    QCoreApplication::processEvents();
    auto* routing =
        window ? first_visible_item(window->contentItem(), QStringLiteral("routingInspector"))
               : nullptr;
    if (routing)
      routing->setProperty("chosenSendId", destination);
    QCoreApplication::processEvents();
    auto* add_route =
        window ? first_visible_item(window->contentItem(), QStringLiteral("addSendButton"))
               : nullptr;
    if (!add_route || !add_route->isEnabled()) {
      std::cerr << "Send automation smoke: routing panel add action is unavailable\n";
      return 1;
    }
    add_route->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Return);
    QCoreApplication::processEvents();
    bridge.add_bus_track(QStringLiteral("group"));
    const auto second_destination =
        bridge.tracks().back().toMap().value(QStringLiteral("id")).toString();
    bridge.add_send_route(source, second_destination, 0.6);
    auto sends = bridge.tracks().front().toMap().value(QStringLiteral("sends")).toList();
    if (sends.size() != 2) {
      std::cerr << "Send automation smoke: send creation failed\n";
      return 1;
    }
    const auto send_id = sends.front().toMap().value(QStringLiteral("id")).toString();
    const auto second_send_id = sends.back().toMap().value(QStringLiteral("id")).toString();
    if (window)
      window->setProperty("selectedTrack", 0);
    QCoreApplication::processEvents();
    auto* bar =
        window ? first_visible_item(window->contentItem(), QStringLiteral("sendAutomationBarInput"))
               : nullptr;
    auto* gain = window ? first_visible_item(window->contentItem(),
                                             QStringLiteral("sendAutomationGainInput"))
                        : nullptr;
    auto* add =
        window ? first_visible_item(window->contentItem(), QStringLiteral("sendAutomationAddPoint"))
               : nullptr;
    if (!bar || !gain || !add || !add->isEnabled() ||
        count_visible_items(window->contentItem(), QStringLiteral("sendAutomationChoice")) != 2) {
      std::cerr << "Send automation smoke: selected send editor controls missing\n";
      return 1;
    }
    qsizetype first_index = 0;
    qsizetype second_index = 1;
    auto* first_choice = nth_visible_item(window->contentItem(),
                                          QStringLiteral("sendAutomationChoice"), first_index);
    auto* second_choice = nth_visible_item(window->contentItem(),
                                           QStringLiteral("sendAutomationChoice"), second_index);
    if (!first_choice || !second_choice) {
      std::cerr << "Send automation smoke: send selection controls missing\n";
      return 1;
    }
    second_choice->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Return);
    QCoreApplication::processEvents();
    if (engine.rootObjects().front()->property("selectedSendId").toString() != second_send_id) {
      std::cerr << "Send automation smoke: keyboard send selection failed\n";
      return 1;
    }
    first_choice->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Return);
    QCoreApplication::processEvents();
    if (engine.rootObjects().front()->property("selectedSendId").toString() != send_id) {
      std::cerr << "Send automation smoke: returning to first send failed\n";
      return 1;
    }
    const auto initial = bridge.revision();
    bar->setProperty("text", QStringLiteral("2"));
    gain->setProperty("text", QStringLiteral("0.25"));
    QCoreApplication::processEvents();
    add->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Return);
    QCoreApplication::processEvents();
    sends = bridge.tracks().front().toMap().value(QStringLiteral("sends")).toList();
    auto lane = sends.front().toMap().value(QStringLiteral("automation")).toMap();
    auto points = lane.value(QStringLiteral("points")).toList();
    if (bridge.revision() != initial + 1 || !lane.value(QStringLiteral("present")).toBool() ||
        lane.value(QStringLiteral("timebase")).toString() != QStringLiteral("Musical") ||
        points.size() != 1 ||
        points.front().toMap().value(QStringLiteral("value")).toDouble() != 0.25 ||
        count_visible_items(window->contentItem(), QStringLiteral("sendAutomationPointRow")) != 1) {
      std::cerr << "Send automation smoke: keyboard add failed: " << bridge.status().toStdString()
                << '\n';
      return 1;
    }
    const auto point_id = points.front().toMap().value(QStringLiteral("id")).toString();
    const auto first_send_points = [&bridge] {
      const auto sends = bridge.tracks().front().toMap().value(QStringLiteral("sends")).toList();
      return sends.front()
          .toMap()
          .value(QStringLiteral("automation"))
          .toMap()
          .value(QStringLiteral("points"))
          .toList();
    };
    for (const int extra_bar : std::array{0, 2, 3}) {
      bridge.set_send_automation_point(send_id, extra_bar, 0.5);
    }
    points = first_send_points();
    gain->setProperty("text", QStringLiteral("0.60"));
    QCoreApplication::processEvents();
    if (points.size() != 4 || !add->isEnabled()) {
      std::cerr << "Send automation smoke: four-point replacement was disabled\n";
      return 1;
    }
    add->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Return);
    QCoreApplication::processEvents();
    points = first_send_points();
    const auto replaced = std::ranges::find_if(points, [&point_id](const auto& item) {
      return item.toMap().value(QStringLiteral("id")).toString() == point_id;
    });
    if (replaced == points.end() ||
        replaced->toMap().value(QStringLiteral("value")).toDouble() != 0.6) {
      std::cerr << "Send automation smoke: four-point replacement changed identity or value\n";
      return 1;
    }
    bar->setProperty("text", QStringLiteral("5"));
    QCoreApplication::processEvents();
    if (add->isEnabled()) {
      std::cerr << "Send automation smoke: fifth point was enabled in bounded editor\n";
      return 1;
    }
    bridge.undo();
    for (const auto& item : points) {
      const auto id = item.toMap().value(QStringLiteral("id")).toString();
      if (id != point_id) {
        bridge.remove_send_automation_point(send_id, id);
      }
    }
    const auto before_value = bridge.revision();
    bridge.set_send_automation_value(send_id, point_id, 0.75);
    if (bridge.revision() != before_value + 1 ||
        first_send_points().front().toMap().value(QStringLiteral("value")).toDouble() != 0.75) {
      std::cerr << "Send automation smoke: point value edit failed\n";
      return 1;
    }
    bridge.undo();
    points = first_send_points();
    if (points.size() != 1 ||
        points.front().toMap().value(QStringLiteral("id")).toString() != point_id ||
        points.front().toMap().value(QStringLiteral("value")).toDouble() != 0.25) {
      std::cerr << "Send automation smoke: undo changed point identity\n";
      return 1;
    }
    bridge.redo();
    auto* remove =
        first_visible_item(window->contentItem(), QStringLiteral("sendAutomationRemovePoint"));
    if (!remove) {
      std::cerr << "Send automation smoke: remove control missing\n";
      return 1;
    }
    remove->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Return);
    QCoreApplication::processEvents();
    points = first_send_points();
    if (!points.empty()) {
      std::cerr << "Send automation smoke: remove point failed\n";
      return 1;
    }
    bridge.undo();
    auto* clear =
        first_visible_item(window->contentItem(), QStringLiteral("sendAutomationClearLane"));
    if (!clear) {
      std::cerr << "Send automation smoke: clear lane control missing\n";
      return 1;
    }
    clear->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Return);
    QCoreApplication::processEvents();
    if (first_send_points().size() != 0 || bridge.tracks()
                                               .front()
                                               .toMap()
                                               .value(QStringLiteral("sends"))
                                               .toList()
                                               .front()
                                               .toMap()
                                               .value(QStringLiteral("automation"))
                                               .toMap()
                                               .value(QStringLiteral("present"))
                                               .toBool()) {
      std::cerr << "Send automation smoke: clear lane did not remove lane\n";
      return 1;
    }
    bridge.undo();
    auto* timebase =
        first_visible_item(window->contentItem(), QStringLiteral("sendAutomationTimebase"));
    if (!timebase || !timebase->isEnabled()) {
      std::cerr << "Send automation smoke: timebase action missing\n";
      return 1;
    }
    const auto before_timebase = bridge.revision();
    timebase->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Return);
    QCoreApplication::processEvents();
    lane = bridge.tracks()
               .front()
               .toMap()
               .value(QStringLiteral("sends"))
               .toList()
               .front()
               .toMap()
               .value(QStringLiteral("automation"))
               .toMap();
    points = lane.value(QStringLiteral("points")).toList();
    if (bridge.revision() != before_timebase + 1 ||
        lane.value(QStringLiteral("timebase")).toString() != QStringLiteral("Elapsed") ||
        points.size() != 1 ||
        points.front().toMap().value(QStringLiteral("id")).toString() != point_id) {
      std::cerr << "Send automation smoke: timebase action lost point identity: "
                << bridge.status().toStdString() << '\n';
      return 1;
    }
    bridge.save();
    if (!bridge.status().startsWith(QStringLiteral("Saved"))) {
      std::cerr << "Send automation smoke: save failed\n";
      return 1;
    }
    std::cout << "Qt Quick send gain automation edited, undone, redone and saved\n";
    return 0;
#else
    std::cerr << "Send automation smoke requires Qt Test input support\n";
    return 1;
#endif
  }
  if (smoke_verify_send_automation) {
    const auto sends = bridge.tracks().front().toMap().value(QStringLiteral("sends")).toList();
    const auto lane = sends.size() == 2
                          ? sends.front().toMap().value(QStringLiteral("automation")).toMap()
                          : QVariantMap{};
    const auto points = lane.value(QStringLiteral("points")).toList();
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
    if (!lane.value(QStringLiteral("present")).toBool() ||
        lane.value(QStringLiteral("timebase")).toString() != QStringLiteral("Elapsed") ||
        points.size() != 1 ||
        points.front().toMap().value(QStringLiteral("value")).toDouble() != 0.75 || !window ||
        count_visible_items(window->contentItem(), QStringLiteral("sendAutomationPointRow")) != 1) {
      std::cerr << "Send automation smoke: saved send lane did not reopen\n";
      return 1;
    }
    std::cout << "Qt Quick send gain automation reopened\n";
    return 0;
  }
  if (smoke_tempo_map) {
    return quick_smoke::edit_tempo_map(bridge,
                                       qobject_cast<QQuickWindow*>(engine.rootObjects().front()),
                                       first_visible_item)
               ? 0
               : 1;
  }
  if (smoke_verify_tempo_map) {
    return quick_smoke::verify_tempo_map(bridge,
                                         qobject_cast<QQuickWindow*>(engine.rootObjects().front()))
               ? 0
               : 1;
  }
  if (smoke_waveform) {
    quick_smoke::start_waveform(app, engine, bridge, first_visible_item);
  }
  if (smoke_tempo_mode == "dropped") {
    // A dropped track is analysed and set up as the clip strip has it:
    // Follow on, the clip musical, its downbeat on a bar.
    const auto before = bridge.tracks().size();
    bridge.imports()->import_audio(
        QUrl::fromLocalFile(QString::fromStdString(smoke_tempo_drop.string())));
    auto* timer = new QTimer(&app);
    timer->setInterval(20);
    QElapsedTimer elapsed;
    elapsed.start();
    QObject::connect(timer, &QTimer::timeout, &app, [&app, &bridge, timer, elapsed, before] {
      if (elapsed.elapsed() > 20'000) {
        const auto tracks = bridge.tracks();
        QString id;
        for (const auto& track : tracks)
          for (const auto& item : track.toMap().value(QStringLiteral("clips")).toList())
            if (item.toMap().value(QStringLiteral("name")).toString() == QStringLiteral("tempo"))
              id = item.toMap().value(QStringLiteral("id")).toString();
        const auto entry = bridge.clip_tempo()->property("clips").toMap().value(id).toMap();
        std::cerr << "Tempo drop smoke: the dropped clip was not set up: tracks " << tracks.size()
                  << " (before " << before << ") clip " << id.toStdString() << " analysing "
                  << entry.value(QStringLiteral("analysing")).toBool() << " BPM "
                  << entry.value(QStringLiteral("bpm")).toDouble() << " follow "
                  << entry.value(QStringLiteral("follow")).toBool() << " status "
                  << bridge.status().toStdString() << '\n';
        timer->stop();
        app.exit(1);
        return;
      }
      const auto tracks = bridge.tracks();
      if (tracks.size() <= before)
        return;
      // The dropped clip: named after its file.
      QVariantMap clip;
      for (const auto& track : tracks)
        for (const auto& item : track.toMap().value(QStringLiteral("clips")).toList())
          if (item.toMap().value(QStringLiteral("name")).toString() == QStringLiteral("tempo"))
            clip = item.toMap();
      if (clip.isEmpty())
        return;
      const auto entry = bridge.clip_tempo()
                             ->property("clips")
                             .toMap()
                             .value(clip.value(QStringLiteral("id")).toString())
                             .toMap();
      if (entry.isEmpty() || !entry.value(QStringLiteral("follow")).toBool())
        return;
      const auto bars = bridge.timeline_bars();
      const auto downbeat_bar = (clip.value(QStringLiteral("start")).toDouble() +
                                 entry.value(QStringLiteral("downbeat")).toDouble() *
                                     clip.value(QStringLiteral("length")).toDouble()) *
                                bars;
      // Not the project's first audio, so the project keeps its tempo.
      if (clip.value(QStringLiteral("timebase")).toString() != QStringLiteral("Musical") ||
          entry.value(QStringLiteral("bpm")).toDouble() != 120.0 ||
          std::abs(downbeat_bar - std::round(downbeat_bar)) > 0.01) {
        std::cerr << "Tempo drop smoke: dropped clip set up wrongly: timebase "
                  << clip.value(QStringLiteral("timebase")).toString().toStdString() << " BPM "
                  << entry.value(QStringLiteral("bpm")).toDouble() << " downbeat bar "
                  << downbeat_bar << " name "
                  << clip.value(QStringLiteral("name")).toString().toStdString() << '\n';
        timer->stop();
        app.exit(1);
        return;
      }
      std::cout << "Qt Quick dropped track follows with its downbeat on bar "
                << std::llround(downbeat_bar) + 1 << '\n';
      timer->stop();
      app.quit();
    });
    timer->start();
  } else if (!smoke_tempo_mode.empty()) {
    // Background analysis (ADR-0053): opening a project analyses its audio
    // without changing it; the clip strip shows the result.
    const auto track = quick_smoke::audio_track(bridge);
    if (!track.isValid() || track.toMap().value(QStringLiteral("clips")).toList().size() != 1) {
      std::cerr << "Tempo smoke: audio fixture missing\n";
      return 1;
    }
    const auto clip_id = track.toMap()
                             .value(QStringLiteral("clips"))
                             .toList()
                             .front()
                             .toMap()
                             .value(QStringLiteral("id"))
                             .toString();
    // What the user placed: unchanged by analysis (the waveform may load).
    const auto placed = [&bridge] {
      const auto clip = quick_smoke::audio_track(bridge)
                            .toMap()
                            .value(QStringLiteral("clips"))
                            .toList()
                            .front()
                            .toMap();
      QVariantMap fields;
      for (const auto* name : {"id", "timebase", "start", "length", "sourceStartFrame",
                               "sourceFrameCount", "processed", "warpBadge"})
        fields.insert(QString::fromLatin1(name), clip.value(QString::fromLatin1(name)));
      return fields;
    };
    const auto initial_tracks = placed();
    const auto initial_tempo_map = bridge.tempo_lane()->property("points").toList();
    const auto before_revision = bridge.revision();
    auto* timer = new QTimer(&app);
    timer->setInterval(20);
    QElapsedTimer elapsed;
    elapsed.start();
    QObject::connect(
        timer, &QTimer::timeout, &app,
        [&app, &bridge, timer, elapsed, before_revision, clip_id, smoke_tempo_mode, initial_tracks,
         initial_tempo_map, placed] {
          if (elapsed.elapsed() > 15'000) {
            std::cerr << "Tempo smoke: analysis timed out\n";
            timer->stop();
            app.exit(1);
            return;
          }
          const auto clips = bridge.clip_tempo()->property("clips").toMap();
          const auto entry = clips.value(clip_id).toMap();
          if (entry.isEmpty() || entry.value(QStringLiteral("analysing")).toBool())
            return;
          const auto source_key = entry.value(QStringLiteral("sourceKey")).toString();
          const bool hit = entry.value(QStringLiteral("cacheHit")).toBool();
          if (entry.value(QStringLiteral("bpm")).toDouble() != 120.0 ||
              entry.value(QStringLiteral("confidence")).toDouble() < 0.55 ||
              source_key.size() != 64 || bridge.revision() != before_revision ||
              bridge.tempo_lane()->property("points").toList() != initial_tempo_map ||
              placed() != initial_tracks || (smoke_tempo_mode == "cached") != hit) {
            std::cerr << "Tempo smoke: clip strip analysis invalid: BPM "
                      << entry.value(QStringLiteral("bpm")).toDouble() << " confidence "
                      << entry.value(QStringLiteral("confidence")).toDouble()
                      << " source-key length " << source_key.size() << " revision "
                      << bridge.revision() << " expected " << before_revision
                      << " tempo map unchanged "
                      << (bridge.tempo_lane()->property("points").toList() == initial_tempo_map)
                      << " clip unchanged " << (placed() == initial_tracks) << " cache hit " << hit
                      << '\n';
            timer->stop();
            app.exit(1);
            return;
          }
          std::cout << "Qt Quick tempo analysis " << smoke_tempo_mode << " "
                    << source_key.toStdString() << '\n';
          timer->stop();
          app.quit();
        });
    timer->start();
  }
  if (smoke_audio_device)
    quick_smoke::run_audio_backend(app, engine, bridge, smoke_device_state, jack_output_factory,
                                   count_visible_items);
  if (!welcome_create_parent.empty()) {
    bridge.create_default_project(QStringLiteral("Quick Start"));
  } else if (!welcome_open_folder.empty()) {
    bridge.open_listed(
        QString::fromStdString(std::filesystem::absolute(welcome_open_folder).string()));
  }
  const bool import_workflow = smoke_import || smoke_import_fail || smoke_import_stale ||
                               smoke_import_close || smoke_import_undo;
  const bool live_workflow =
      smoke_live_edit || smoke_live_stale || smoke_live_fail || smoke_live_close || smoke_live_stop;
  if (smoke_verify_live_edit) {
    const auto clips = bridge.tracks().front().toMap().value(QStringLiteral("clips")).toList();
    if (clips.size() != 1 ||
        std::abs(clips.front().toMap().value(QStringLiteral("start")).toDouble() - 1.0 / 16.0) >
            0.00001) {
      std::cerr << "Live audio smoke: saved move did not reopen\n";
      return 1;
    }
    std::cout << "Qt Quick live audio edit reopened\n";
    return 0;
  }
#ifdef OSCILLATE_QUICK_TEST_INPUT
  if (live_workflow) {
    const auto clips = bridge.tracks().front().toMap().value(QStringLiteral("clips")).toList();
    if (clips.size() != 1) {
      std::cerr << "Live audio smoke: expected one imported clip\n";
      return 1;
    }
    const QString clip_id = clips.front().toMap().value(QStringLiteral("id")).toString();
    auto stage = std::make_shared<int>(0);
    auto timeout = std::make_shared<QElapsedTimer>();
    timeout->start();
    auto* poll = new QTimer(&app);
    poll->setInterval(10);
    QObject::connect(
        poll, &QTimer::timeout, &app,
        [&app, &bridge, root, clip_id, stage, timeout, smoke_live_edit, smoke_live_stale,
         smoke_live_fail, smoke_live_close, smoke_live_stop, poll] {
          if (timeout->elapsed() > 8000) {
            std::cerr << "Live audio smoke timed out: " << bridge.playback_status().toStdString()
                      << '\n';
            app.exit(1);
            return;
          }
          if (*stage == 0) {
            if (!bridge.playback_status().contains(QStringLiteral("Playing revision"))) {
              return;
            }
            const auto before = bridge.revision();
            if (!bridge.audio_edit_enabled()) {
              std::cerr << "Live audio smoke: edit was disabled during streaming\n";
              app.exit(1);
              return;
            }
            bridge.move_audio_clip(clip_id, 1.0 / 16.0, true);
            if (bridge.revision() != before + 1 ||
                !bridge.playback_status().contains(QStringLiteral("pending")) ||
                bridge.audio_edit_enabled()) {
              std::cerr << "Live audio smoke: edit was not accepted as pending: "
                        << bridge.playback_status().toStdString() << '\n';
              app.exit(1);
              return;
            }
            bridge.save();
            if (!bridge.status().contains(QStringLiteral("Wait for the audio edit"))) {
              std::cerr << "Live audio smoke: concurrent save was not guarded\n";
              app.exit(1);
              return;
            }
            bridge.open_project(QUrl::fromLocalFile(QString::fromStdString(root.string())));
            if (!bridge.status().contains(QStringLiteral("Stop playback"))) {
              std::cerr << "Live audio smoke: project switch was not guarded\n";
              app.exit(1);
              return;
            }
            if (smoke_live_close || smoke_live_stop) {
              // Still within the edit's event-loop turn: the inline handoff is
              // done and no playback poll has installed it.
              if (smoke_live_close && bridge.request_close()) {
                std::cerr << "Live audio smoke: close did not await completed handoff\n";
                app.exit(1);
                return;
              }
              if (smoke_live_stop) {
                bridge.toggle_playback();
              }
              *stage = 2;
              return;
            }
            *stage = 1;
            return;
          }
          if (*stage == 1) {
            const auto status = bridge.playback_status();
            const bool settled = smoke_live_edit
                                     ? status.contains(QStringLiteral("Playing revision")) &&
                                           status.contains(QString::number(bridge.revision()))
                                     : status.contains(QStringLiteral("not active"));
            if (!settled) {
              return;
            }
            if (smoke_live_stale && !status.contains(QStringLiteral("stale"))) {
              std::cerr << "Live audio smoke: stale preparation was not identified: "
                        << status.toStdString() << '\n';
              app.exit(1);
              return;
            }
            if (smoke_live_fail && !status.contains(QStringLiteral("invalid geometry"))) {
              std::cerr << "Live audio smoke: output failure was not identified: "
                        << status.toStdString() << '\n';
              app.exit(1);
              return;
            }
            if (bridge.playback_active()) {
              bridge.toggle_playback();
            }
            bridge.save();
            if (!bridge.status().startsWith(QStringLiteral("Saved"))) {
              std::cerr << "Live audio smoke: accepted edit did not save\n";
              app.exit(1);
              return;
            }
            bridge.undo();
            const auto restored = bridge.tracks()
                                      .front()
                                      .toMap()
                                      .value(QStringLiteral("clips"))
                                      .toList()
                                      .front()
                                      .toMap()
                                      .value(QStringLiteral("start"))
                                      .toDouble();
            if (std::abs(restored) > 0.00001) {
              std::cerr << "Live audio smoke: one undo did not restore placement\n";
              app.exit(1);
              return;
            }
            // Closing saves pending edits, so redo to leave the saved move as
            // the state the next session reopens.
            bridge.redo();
            const auto redone = bridge.tracks()
                                    .front()
                                    .toMap()
                                    .value(QStringLiteral("clips"))
                                    .toList()
                                    .front()
                                    .toMap()
                                    .value(QStringLiteral("start"))
                                    .toDouble();
            if (std::abs(redone - 1.0 / 16.0) > 0.00001) {
              std::cerr << "Live audio smoke: redo did not reapply the move\n";
              app.exit(1);
              return;
            }
            std::cout << "Qt Quick live audio "
                      << (smoke_live_edit    ? "edit applied"
                          : smoke_live_stale ? "stale preparation rejected"
                                             : "failed re-prime preserved edit")
                      << '\n';
            poll->stop();
            app.quit();
          } else if (*stage == 2 && smoke_live_stop && !bridge.playback_active()) {
            if (!bridge.playback_status().contains(QStringLiteral("Playback stopped"))) {
              std::cerr << "Live audio smoke: Stop did not report stopped playback\n";
              app.exit(1);
              return;
            }
            bridge.save();
            bridge.undo();
            const auto restored = bridge.tracks()
                                      .front()
                                      .toMap()
                                      .value(QStringLiteral("clips"))
                                      .toList()
                                      .front()
                                      .toMap()
                                      .value(QStringLiteral("start"))
                                      .toDouble();
            if (std::abs(restored) > 0.00001) {
              std::cerr << "Live audio smoke: Stop lost one-step undo\n";
              app.exit(1);
              return;
            }
            // Closing saves pending edits, so redo to leave the saved move as
            // the state the next session reopens.
            bridge.redo();
            const auto redone = bridge.tracks()
                                    .front()
                                    .toMap()
                                    .value(QStringLiteral("clips"))
                                    .toList()
                                    .front()
                                    .toMap()
                                    .value(QStringLiteral("start"))
                                    .toDouble();
            if (std::abs(redone - 1.0 / 16.0) > 0.00001) {
              std::cerr << "Live audio smoke: redo did not reapply the move\n";
              app.exit(1);
              return;
            }
            std::cout << "Qt Quick live audio Stop awaited handoff\n";
            poll->stop();
            app.quit();
          }
        });
    if (smoke_live_close) {
      QObject::connect(&bridge, &oscillate::quick::ProjectBridge::readyToClose, &app,
                       [&app, &bridge, poll] {
                         if (bridge.playback_active()) {
                           std::cerr << "Live audio smoke: playback remained active at close\n";
                           app.exit(1);
                           return;
                         }
                         std::cout << "Qt Quick live audio close awaited handoff\n";
                         poll->stop();
                         app.quit();
                       });
    }
    poll->start();
    bridge.toggle_playback();
  }
#else
  if (live_workflow) {
    std::cerr << "Live audio fixtures require a test build\n";
    return 2;
  }
#endif
  if (smoke_verify_import &&
      !quick_smoke::verify_reopened_import(
          bridge, qobject_cast<QQuickWindow*>(engine.rootObjects().front()))) {
    return 1;
  }
  const auto import_scenario = smoke_import         ? quick_smoke::ImportScenario::success
                               : smoke_import_fail  ? quick_smoke::ImportScenario::failure
                               : smoke_import_stale ? quick_smoke::ImportScenario::stale
                               : smoke_import_close ? quick_smoke::ImportScenario::close
                               : smoke_import_undo  ? quick_smoke::ImportScenario::undo
                                                    : quick_smoke::ImportScenario::none;
  if (!quick_smoke::start_import(import_scenario, app, bridge,
                                 qobject_cast<QQuickWindow*>(engine.rootObjects().front()), root,
                                 smoke_import_source)) {
    return 1;
  }
  if (smoke_midi_workflow) {
    bridge.add_instrument_track();
    if (bridge.tracks().size() != 1) {
      std::cerr << "MIDI smoke: instrument track was not created\n";
      return 1;
    }
    const auto track_id = bridge.tracks().front().toMap().value(QStringLiteral("id")).toString();
    const auto before_failure = bridge.revision();
    bridge.add_midi_clip(QStringLiteral("invalid-id"), 0);
    if (bridge.revision() != before_failure ||
        !bridge.status().contains(QStringLiteral("Select a track"))) {
      std::cerr << "MIDI smoke: invalid track changed the project\n";
      return 1;
    }
    bridge.bind_starter_sound(track_id, -1);
    if (bridge.revision() != before_failure ||
        !bridge.status().contains(QStringLiteral("bundled sound"))) {
      std::cerr << "MIDI smoke: invalid starter sound changed the project\n";
      return 1;
    }
    bridge.bind_starter_sound(track_id, 3);
    if (!bridge.tracks().front().toMap().value(QStringLiteral("nativeSourceReady")).toBool() ||
        bridge.tracks().front().toMap().value(QStringLiteral("soundName")).toString() !=
            QStringLiteral("Pluck")) {
      std::cerr << "MIDI smoke: bundled sound was not bound: " << bridge.status().toStdString()
                << '\n';
      return 1;
    }
    bridge.add_midi_clip(track_id, 0);
    const auto clips = bridge.tracks().front().toMap().value(QStringLiteral("clips")).toList();
    if (clips.size() != 1) {
      std::cerr << "MIDI smoke: clip was not created: " << bridge.status().toStdString() << '\n';
      return 1;
    }
    const auto clip_id = clips.front().toMap().value(QStringLiteral("id")).toString();
    engine.rootObjects().front()->setProperty("selectedClipId", clip_id);
    const auto clip_revision = bridge.revision();
    bridge.add_midi_clip(track_id, 0);
    if (bridge.revision() != clip_revision ||
        !bridge.status().contains(QStringLiteral("MIDI clip:"))) {
      std::cerr << "MIDI smoke: overlap was not rejected\n";
      return 1;
    }
    bridge.add_note(clip_id, 4, 60);
    const auto notes = bridge.tracks()
                           .front()
                           .toMap()
                           .value(QStringLiteral("clips"))
                           .toList()
                           .front()
                           .toMap()
                           .value(QStringLiteral("notes"))
                           .toList();
    if (notes.size() != 1 || notes.front().toMap().value(QStringLiteral("step")).toInt() != 4 ||
        notes.front().toMap().value(QStringLiteral("key")).toInt() != 60) {
      std::cerr << "MIDI smoke: note was not projected: " << bridge.status().toStdString() << '\n';
      return 1;
    }
    const auto note_revision = bridge.revision();
    bridge.add_note(clip_id, 16, 60);
    if (bridge.revision() != note_revision ||
        !bridge.status().contains(QStringLiteral("Select a MIDI clip"))) {
      std::cerr << "MIDI smoke: invalid note changed the project\n";
      return 1;
    }
#ifdef OSCILLATE_QUICK_TEST_INPUT
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
    const auto note_cell = [&]() { return piano_roll_cell(window, 4, 60); };
    const auto current_notes = [&]() {
      return bridge.tracks()
          .front()
          .toMap()
          .value(QStringLiteral("clips"))
          .toList()
          .front()
          .toMap()
          .value(QStringLiteral("notes"))
          .toList();
    };
    const auto cell = note_cell();
    if (!cell) {
      auto* dock = window->findChild<QObject*>(QStringLiteral("deviceChainDock"));
      std::cerr << "MIDI smoke: selected piano-roll cell missing (dock tab "
                << (dock ? dock->property("activeTab").toInt() : -1) << ")\n";
      return 1;
    }
    const auto from = cell->center;
    const auto to = from + QPointF(cell->width, -18);
    const auto before_note_move = bridge.revision();
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, from.toPoint());
    for (int step = 1; step <= 4; ++step) {
      QTest::mouseMove(window, (from + (to - from) * (step / 4.0)).toPoint());
      QCoreApplication::processEvents();
    }
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, to.toPoint());
    QCoreApplication::processEvents();
    if (bridge.revision() != before_note_move + 1 || current_notes().size() != 1 ||
        current_notes().front().toMap().value(QStringLiteral("step")).toInt() != 5 ||
        current_notes().front().toMap().value(QStringLiteral("key")).toInt() != 61) {
      std::cerr << "MIDI smoke: piano-roll note drag did not change step and pitch: "
                << bridge.status().toStdString() << '\n';
      return 1;
    }
    bridge.undo();
    QCoreApplication::processEvents();
    if (current_notes().front().toMap().value(QStringLiteral("step")).toInt() != 4 ||
        current_notes().front().toMap().value(QStringLiteral("key")).toInt() != 60) {
      std::cerr << "MIDI smoke: piano-roll note drag did not undo\n";
      return 1;
    }
    auto* resize_note_handle = first_visible_item(
        window->contentItem(), QStringLiteral("pianoNoteResizeHandle-") +
                                   current_notes().front().toMap().value("id").toString());
    if (!resize_note_handle) {
      std::cerr << "MIDI smoke: note duration handle is missing\n";
      return 1;
    }
    const QPointF note_resize_from = resize_note_handle->mapToScene(
        {resize_note_handle->width() / 2, resize_note_handle->height() / 2});
    const auto before_note_resize = bridge.revision();
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, note_resize_from.toPoint());
    for (int step = 1; step <= 4; ++step) {
      QTest::mouseMove(window, (note_resize_from + QPointF(cell->width * step / 4, 0)).toPoint());
    }
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier,
                        (note_resize_from + QPointF(cell->width, 0)).toPoint());
    QCoreApplication::processEvents();
    if (bridge.revision() != before_note_resize + 1 ||
        current_notes().front().toMap().value(QStringLiteral("durationTicks")).toLongLong() !=
            480'000'000) {
      std::cerr << "MIDI smoke: note edge drag did not resize by one grid step\n";
      return 1;
    }
    bridge.undo();
    QCoreApplication::processEvents();
    if (current_notes().front().toMap().value(QStringLiteral("durationTicks")).toLongLong() !=
        240'000'000) {
      std::cerr << "MIDI smoke: one Undo did not restore note duration\n";
      return 1;
    }
    auto* roll = first_visible_item(window->contentItem(), QStringLiteral("pianoRoll"));
    auto* eighth_grid = first_visible_item(window->contentItem(), QStringLiteral("pianoGrid8"));
    auto* sixteenth_grid = first_visible_item(window->contentItem(), QStringLiteral("pianoGrid16"));
    if (!roll || !eighth_grid || !sixteenth_grid) {
      std::cerr << "MIDI smoke: piano roll grid controls missing\n";
      return 1;
    }
    const auto eighth_center =
        eighth_grid->mapToScene({eighth_grid->width() / 2, eighth_grid->height() / 2});
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, eighth_center.toPoint());
    QCoreApplication::processEvents();
    if (roll->property("gridSteps").toInt() != 8 || current_notes().size() != 1) {
      std::cerr << "MIDI smoke: changing grid lost an existing note\n";
      return 1;
    }
    const auto sixteenth_center =
        sixteenth_grid->mapToScene({sixteenth_grid->width() / 2, sixteenth_grid->height() / 2});
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, sixteenth_center.toPoint());
    const QPointF wheel_position = roll->mapToScene({roll->width() - 30, roll->height() / 2});
    QWheelEvent pitch_wheel(wheel_position, window->mapToGlobal(wheel_position.toPoint()), QPoint{},
                            QPoint{0, -120}, Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
    QCoreApplication::sendEvent(window, &pitch_wheel);
    QCoreApplication::processEvents();
    if (roll->property("lowestKey").toInt() != 57) {
      std::cerr << "MIDI smoke: mouse wheel did not scroll to lower pitches\n";
      return 1;
    }
    auto* find_notes = first_visible_item(window->contentItem(), QStringLiteral("pianoFocusNotes"));
    if (!find_notes) {
      std::cerr << "MIDI smoke: find-notes action is missing\n";
      return 1;
    }
    QTest::mouseClick(
        window, Qt::LeftButton, Qt::NoModifier,
        find_notes->mapToScene({find_notes->width() / 2, find_notes->height() / 2}).toPoint());
    QCoreApplication::processEvents();
    const auto focused_lowest = roll->property("lowestKey").toInt();
    if (focused_lowest > 60 || focused_lowest + 11 < 60) {
      std::cerr << "MIDI smoke: find-notes did not reveal the saved note\n";
      return 1;
    }
    roll->setProperty("lowestKey", 60);
    QCoreApplication::processEvents();
    auto* thirty_second_grid =
        first_visible_item(window->contentItem(), QStringLiteral("pianoGrid32"));
    if (!thirty_second_grid) {
      std::cerr << "MIDI smoke: thirty-second grid is missing\n";
      return 1;
    }
    QTest::mouseClick(
        window, Qt::LeftButton, Qt::NoModifier,
        thirty_second_grid
            ->mapToScene({thirty_second_grid->width() / 2, thirty_second_grid->height() / 2})
            .toPoint());
    QCoreApplication::processEvents();
    const auto off_grid_cell = piano_roll_cell(window, 3, 65);
    if (!off_grid_cell) {
      std::cerr << "MIDI smoke: off-grid fixture cell is missing\n";
      return 1;
    }
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, off_grid_cell->center.toPoint());
    QCoreApplication::processEvents();
    if (current_notes().size() != 2 ||
        current_notes().back().toMap().value(QStringLiteral("startTicks")).toLongLong() !=
            360'000'000) {
      std::cerr << "MIDI smoke: 1/32 grid did not create an exact note\n";
      return 1;
    }
    const auto temporary_note_id =
        current_notes().back().toMap().value(QStringLiteral("id")).toString();
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, eighth_center.toPoint());
    auto* quantise = first_visible_item(window->contentItem(), QStringLiteral("pianoQuantise"));
    if (!quantise) {
      std::cerr << "MIDI smoke: Quantise control is missing\n";
      return 1;
    }
    const auto before_quantise = bridge.revision();
    QTest::mouseClick(
        window, Qt::LeftButton, Qt::NoModifier,
        quantise->mapToScene({quantise->width() / 2, quantise->height() / 2}).toPoint());
    QCoreApplication::processEvents();
    if (bridge.revision() != before_quantise + 1 ||
        current_notes().back().toMap().value(QStringLiteral("startTicks")).toLongLong() !=
            480'000'000) {
      std::cerr << "MIDI smoke: Quantise button did not move note to 1/8 grid ("
                << bridge.property("status").toString().toStdString() << ")\n";
      return 1;
    }
    bridge.undo();
    if (current_notes().back().toMap().value(QStringLiteral("startTicks")).toLongLong() !=
        360'000'000) {
      std::cerr << "MIDI smoke: one Undo did not restore the off-grid note\n";
      return 1;
    }
    bridge.remove_note_id(clip_id, temporary_note_id);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, sixteenth_center.toPoint());
    QCoreApplication::processEvents();
    if (current_notes().size() != 1) {
      std::cerr << "MIDI smoke: temporary quantise fixture note was not removed\n";
      return 1;
    }
    // The finest grids: every division in the shared list has a button, and
    // a note drawn on the finest is one of its steps long, at its step.
    for (const auto steps : oscillate::application::midi_grid_steps_per_bar) {
      if (!first_visible_item(window->contentItem(), QStringLiteral("pianoGrid%1").arg(steps))) {
        std::cerr << "MIDI smoke: the 1/" << steps << " grid button is missing\n";
        return 1;
      }
    }
    const auto finest = oscillate::application::midi_grid_steps_per_bar.back();
    auto* finest_grid =
        first_visible_item(window->contentItem(), QStringLiteral("pianoGrid%1").arg(finest));
    QTest::mouseClick(
        window, Qt::LeftButton, Qt::NoModifier,
        finest_grid->mapToScene({finest_grid->width() / 2, finest_grid->height() / 2}).toPoint());
    QCoreApplication::processEvents();
    const auto fine_cell = piano_roll_cell(window, 3, 65);
    if (roll->property("gridSteps").toInt() != finest || !fine_cell) {
      std::cerr << "MIDI smoke: the 1/" << finest << " grid was not selected\n";
      return 1;
    }
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, fine_cell->center.toPoint());
    QCoreApplication::processEvents();
    const auto fine_ticks = oscillate::application::midi_grid_ticks(finest);
    QString fine_note_id;
    for (const auto& entry : current_notes()) {
      const auto drawn = entry.toMap();
      if (drawn.value(QStringLiteral("key")).toInt() == 65 &&
          drawn.value(QStringLiteral("startTicks")).toLongLong() == 3 * fine_ticks &&
          drawn.value(QStringLiteral("durationTicks")).toLongLong() == fine_ticks)
        fine_note_id = drawn.value(QStringLiteral("id")).toString();
    }
    if (current_notes().size() != 2 || fine_note_id.isEmpty()) {
      std::cerr << "MIDI smoke: the 1/" << finest << " grid did not draw a one-step note\n";
      return 1;
    }
    bridge.remove_note_id(clip_id, fine_note_id);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, sixteenth_center.toPoint());
    QCoreApplication::processEvents();
    if (current_notes().size() != 1 || roll->property("gridSteps").toInt() != 16) {
      std::cerr << "MIDI smoke: the 1/" << finest << " fixture note was not removed\n";
      return 1;
    }
    auto click_cell = [&]() {
      const auto target = note_cell();
      if (!target)
        return false;
      QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, target->center.toPoint());
      QCoreApplication::processEvents();
      return true;
    };
    auto* note_item = first_visible_item(
        window->contentItem(),
        QStringLiteral("pianoRollNote-") + current_notes().front().toMap().value("id").toString());
    if (!note_item) {
      std::cerr << "MIDI smoke: note body missing\n";
      return 1;
    }
    note_item->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Delete);
    QCoreApplication::processEvents();
    if (!current_notes().empty()) {
      std::cerr << "MIDI smoke: piano-roll Delete did not remove the note\n";
      return 1;
    }
    bridge.undo();
    QCoreApplication::processEvents();
    if (current_notes().size() != 1) {
      std::cerr << "MIDI smoke: piano-roll Delete did not undo\n";
      return 1;
    }
    // The pedal strip (ADR-0035): drag across it to press the sustain pedal
    // over the whole bar; the note then rings to the clip end, drawn as a
    // tail; Pedal per chord and Delete each make one undo step.
    const auto sustain_spans = [&]() {
      return bridge.tracks()
          .front()
          .toMap()
          .value(QStringLiteral("clips"))
          .toList()
          .front()
          .toMap()
          .value(QStringLiteral("pedals"))
          .toMap()
          .value(QStringLiteral("sustain"))
          .toList();
    };
    auto* sustain_lane =
        first_visible_item(window->contentItem(), QStringLiteral("pedalLane-sustain"));
    if (!sustain_lane || !sustain_spans().isEmpty()) {
      std::cerr << "MIDI smoke: the pedal strip is missing or not empty\n";
      return 1;
    }
    const auto lane_start = sustain_lane->mapToScene({2.0, sustain_lane->height() / 2});
    const auto lane_end =
        sustain_lane->mapToScene({sustain_lane->width() - 1.0, sustain_lane->height() / 2});
    const auto before_pedal = bridge.revision();
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, lane_start.toPoint());
    QTest::mouseMove(window, ((lane_start + lane_end) / 2).toPoint());
    QTest::mouseMove(window, lane_end.toPoint());
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, lane_end.toPoint());
    QCoreApplication::processEvents();
    if (bridge.revision() != before_pedal + 1 || sustain_spans().size() != 1 ||
        sustain_spans().front().toMap().value(QStringLiteral("startTicks")).toLongLong() != 0) {
      std::cerr << "MIDI smoke: dragging across the pedal strip did not press the pedal\n";
      return 1;
    }
    const auto note_id = current_notes().front().toMap().value(QStringLiteral("id")).toString();
    const auto note = current_notes().front().toMap();
    if (note.value(QStringLiteral("soundingEndTicks")).toLongLong() <=
            note.value(QStringLiteral("startTicks")).toLongLong() +
                note.value(QStringLiteral("durationTicks")).toLongLong() ||
        !first_visible_item(window->contentItem(), QStringLiteral("pianoRingingTail-") + note_id)) {
      std::cerr << "MIDI smoke: a note under the pedal does not show its ringing tail\n";
      return 1;
    }
    auto* per_chord =
        first_visible_item(window->contentItem(), QStringLiteral("pianoPedalPerChord"));
    if (!per_chord) {
      std::cerr << "MIDI smoke: Pedal per chord is missing\n";
      return 1;
    }
    const auto before_chords = bridge.revision();
    QTest::mouseClick(
        window, Qt::LeftButton, Qt::NoModifier,
        per_chord->mapToScene({per_chord->width() / 2, per_chord->height() / 2}).toPoint());
    QCoreApplication::processEvents();
    if (bridge.revision() != before_chords + 1 || sustain_spans().size() != 1 ||
        sustain_spans().front().toMap().value(QStringLiteral("startTicks")).toLongLong() <=
            note.value(QStringLiteral("startTicks")).toLongLong()) {
      std::cerr << "MIDI smoke: Pedal per chord did not re-pedal after the chord\n";
      return 1;
    }
    bridge.undo();
    QCoreApplication::processEvents();
    if (sustain_spans().size() != 1 ||
        sustain_spans().front().toMap().value(QStringLiteral("startTicks")).toLongLong() != 0) {
      std::cerr << "MIDI smoke: one Undo did not restore the drawn pedal\n";
      return 1;
    }
    auto* span_item =
        first_visible_item(window->contentItem(), QStringLiteral("pedalSpan-sustain-0"));
    if (!span_item) {
      std::cerr << "MIDI smoke: the drawn pedal span is not shown\n";
      return 1;
    }
    span_item->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Delete);
    QCoreApplication::processEvents();
    if (!sustain_spans().isEmpty()) {
      std::cerr << "MIDI smoke: Delete did not lift the pedal span\n";
      return 1;
    }
    // The velocity lane: drag a stem up to set its note, which new notes
    // then use; Humanise over the whole clip is one undo step.
    auto* stem =
        first_visible_item(window->contentItem(), QStringLiteral("velocityStem-") + note_id);
    if (!stem) {
      std::cerr << "MIDI smoke: the note's velocity stem is missing\n";
      return 1;
    }
    const auto stem_bottom = stem->mapToScene({stem->width() / 2, stem->height() - 6});
    const auto stem_top = stem->mapToScene({stem->width() / 2, 2.0});
    const auto before_velocity = bridge.revision();
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, stem_bottom.toPoint());
    QTest::mouseMove(window, ((stem_bottom + stem_top) / 2).toPoint());
    QTest::mouseMove(window, stem_top.toPoint());
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, stem_top.toPoint());
    QCoreApplication::processEvents();
    const auto dragged = current_notes().front().toMap().value(QStringLiteral("velocity")).toInt();
    if (bridge.revision() != before_velocity + 1 || dragged < 120 ||
        qobject_cast<oscillate::quick::NoteToolsController*>(bridge.note_tools())
                ->new_note_velocity() != dragged) {
      std::cerr << "MIDI smoke: dragging a velocity stem did not set the note (" << dragged
                << ")\n";
      return 1;
    }
    auto* whole_clip =
        first_visible_item(window->contentItem(), QStringLiteral("velocityWholeClip"));
    auto* humanise = first_visible_item(window->contentItem(), QStringLiteral("velocityHumanize"));
    if (!whole_clip || !humanise) {
      std::cerr << "MIDI smoke: velocity tools are missing\n";
      return 1;
    }
    QTest::mouseClick(
        window, Qt::LeftButton, Qt::NoModifier,
        whole_clip->mapToScene({whole_clip->width() / 2, whole_clip->height() / 2}).toPoint());
    const auto before_humanise = bridge.revision();
    for (int attempt = 0; attempt < 8 && bridge.revision() == before_humanise; ++attempt) {
      // A variation can, rarely, land on the same value: reroll.
      QTest::mouseClick(
          window, Qt::LeftButton, Qt::NoModifier,
          humanise->mapToScene({humanise->width() / 2, humanise->height() / 2}).toPoint());
      QCoreApplication::processEvents();
    }
    if (bridge.revision() != before_humanise + 1) {
      std::cerr << "MIDI smoke: Humanise did not vary the clip in one step\n";
      return 1;
    }
    bridge.undo();
    QCoreApplication::processEvents();
    if (current_notes().front().toMap().value(QStringLiteral("velocity")).toInt() != dragged) {
      std::cerr << "MIDI smoke: one Undo did not restore the humanised velocity\n";
      return 1;
    }
    if (const char* shots = std::getenv("QUICK_SMOKE_SCREENSHOT_DIR")) {
      // A half pedal across most of the bar, so the strip and tails show.
      qobject_cast<oscillate::quick::NoteToolsController*>(bridge.note_tools())
          ->paint_pedal_grid(clip_id, QStringLiteral("sustain"), 0, 12, 0.6, 16);
      QCoreApplication::processEvents();
      window->grabWindow().save(QString::fromUtf8(shots) + QStringLiteral("/piano_roll.png"));
      bridge.undo();
      QCoreApplication::processEvents();
    }
    if (!click_cell() || current_notes().size() != 1) {
      std::cerr << "MIDI smoke: clicking an occupied note created a duplicate\n";
      return 1;
    }
    auto* clip_item =
        window ? first_visible_item(window->contentItem(), QStringLiteral("arrangementClip"))
               : nullptr;
    if (!clip_item) {
      std::cerr << "MIDI smoke: arrangement clip missing\n";
      return 1;
    }
    const auto position =
        clip_item->mapToScene({clip_item->width() / 2.0, clip_item->height() / 2.0});
    QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, position.toPoint());
    QCoreApplication::processEvents();
    auto* duplicate =
        first_visible_item(window->contentItem(), QStringLiteral("duplicateMidiClipAction"));
    if (!duplicate) {
      std::cerr << "MIDI smoke: duplicate context action missing\n";
      return 1;
    }
    const auto before_duplicate = bridge.revision();
    duplicate->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Return);
    QCoreApplication::processEvents();
    auto copied_clips = bridge.tracks().front().toMap().value(QStringLiteral("clips")).toList();
    if (bridge.revision() != before_duplicate + 1 || copied_clips.size() != 2) {
      std::cerr << "MIDI smoke: context duplicate did not create one clip\n";
      return 1;
    }
    const auto original_note =
        copied_clips.front().toMap().value(QStringLiteral("notes")).toList().front().toMap();
    const auto copied_note =
        copied_clips.back().toMap().value(QStringLiteral("notes")).toList().front().toMap();
    if (copied_clips.back().toMap().value(QStringLiteral("id")).toString() == clip_id ||
        copied_note.value(QStringLiteral("id")) == original_note.value(QStringLiteral("id")) ||
        copied_note.value(QStringLiteral("startTicks")) !=
            original_note.value(QStringLiteral("startTicks")) ||
        copied_note.value(QStringLiteral("durationTicks")) !=
            original_note.value(QStringLiteral("durationTicks")) ||
        copied_note.value(QStringLiteral("key")) != original_note.value(QStringLiteral("key")) ||
        copied_note.value(QStringLiteral("velocity")) !=
            original_note.value(QStringLiteral("velocity"))) {
      std::cerr << "MIDI smoke: context duplicate lost clip notes: "
                << bridge.status().toStdString() << '\n';
      return 1;
    }
    bridge.undo();
    if (bridge.tracks().front().toMap().value(QStringLiteral("clips")).toList().size() != 1) {
      std::cerr << "MIDI smoke: duplicate did not undo\n";
      return 1;
    }
    bridge.redo();
    auto* arrangement_item =
        first_visible_item(window->contentItem(), QStringLiteral("arrangementView"));
    auto second_handle = qsizetype{1};
    auto* resize_handle = nth_visible_item(
        window->contentItem(), QStringLiteral("midiClipRightResizeHandle"), second_handle);
    if (!arrangement_item || !resize_handle) {
      std::cerr << "MIDI smoke: second clip resize handle missing\n";
      return 1;
    }
    const auto resize_from =
        resize_handle->mapToScene({resize_handle->width() / 2.0, resize_handle->height() / 2.0});
    const double bar_width = arrangement_item->width() /
                             engine.rootObjects().front()->property("visibleBars").toDouble();
    const auto resize_to = resize_from + QPointF(bar_width, 0);
    const auto before_resize = bridge.revision();
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, resize_from.toPoint());
    for (int step = 1; step <= 4; ++step) {
      QTest::mouseMove(window, (resize_from + (resize_to - resize_from) * (step / 4.0)).toPoint());
      QCoreApplication::processEvents();
    }
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, resize_to.toPoint());
    QCoreApplication::processEvents();
    if (bridge.revision() != before_resize + 1 ||
        std::abs(bridge.tracks()
                     .front()
                     .toMap()
                     .value(QStringLiteral("clips"))
                     .toList()
                     .back()
                     .toMap()
                     .value(QStringLiteral("length"))
                     .toDouble() -
                 2.0 / bridge.timeline_bars()) > 0.00001) {
      std::cerr << "MIDI smoke: pointer resize did not extend the clip: "
                << bridge.status().toStdString() << '\n';
      return 1;
    }
    // Loop is off for a new clip, so the edge sets its length (PI-06c).
    const auto resized_clip =
        bridge.tracks().front().toMap().value(QStringLiteral("clips")).toList().back().toMap();
    if (resized_clip.value(QStringLiteral("loop")).toBool() ||
        resized_clip.value(QStringLiteral("patternTicks")).toLongLong() != 2LL * 3'840'000'000LL) {
      std::cerr << "MIDI smoke: pointer resize did not lengthen the clip's pattern\n";
      return 1;
    }
    auto* loop_toggle =
        first_visible_item(window->contentItem(), QStringLiteral("pianoLoopToggle"));
    if (!loop_toggle) {
      std::cerr << "MIDI smoke: piano-roll loop switch missing\n";
      return 1;
    }
    const auto before_loop = bridge.revision();
    QTest::mouseClick(
        window, Qt::LeftButton, Qt::NoModifier,
        loop_toggle->mapToScene({loop_toggle->width() / 2, loop_toggle->height() / 2}).toPoint());
    QCoreApplication::processEvents();
    const auto looped_clip =
        bridge.tracks().front().toMap().value(QStringLiteral("clips")).toList().back().toMap();
    if (bridge.revision() != before_loop + 1 ||
        !looped_clip.value(QStringLiteral("loop")).toBool() ||
        looped_clip.value(QStringLiteral("patternTicks")).toLongLong() != 2LL * 3'840'000'000LL) {
      std::cerr << "MIDI smoke: loop switch did not loop the whole clip in one step: "
                << bridge.status().toStdString() << '\n';
      return 1;
    }
    bridge.undo();
    QCoreApplication::processEvents();
    bridge.undo();
    QCoreApplication::processEvents();
    for (const auto& expected_timebase : {QStringLiteral("Elapsed"), QStringLiteral("Musical")}) {
      QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, position.toPoint());
      QCoreApplication::processEvents();
      auto* timebase =
          first_visible_item(window->contentItem(), QStringLiteral("toggleMidiClipTimebaseAction"));
      if (!timebase) {
        std::cerr << "MIDI smoke: timebase context action missing\n";
        return 1;
      }
      const auto before_timebase = bridge.revision();
      timebase->forceActiveFocus();
      QTest::keyClick(window, Qt::Key_Return);
      QCoreApplication::processEvents();
      const auto clips = bridge.tracks().front().toMap().value(QStringLiteral("clips")).toList();
      if (bridge.revision() != before_timebase + 1 ||
          clips.front().toMap().value(QStringLiteral("timebase")).toString() != expected_timebase ||
          clips.back().toMap().value(QStringLiteral("timebase")).toString() !=
              QStringLiteral("Musical")) {
        std::cerr << "MIDI smoke: timebase action changed the wrong clip: "
                  << bridge.status().toStdString() << '\n';
        return 1;
      }
      if (expected_timebase == QStringLiteral("Elapsed")) {
        auto* arrangement_item =
            first_visible_item(window->contentItem(), QStringLiteral("arrangementView"));
        if (!arrangement_item) {
          std::cerr << "MIDI smoke: arrangement drag target missing\n";
          return 1;
        }
        const double bar_width = arrangement_item->width() /
                                 engine.rootObjects().front()->property("visibleBars").toDouble();
        const auto before_move = bridge.revision();
        if (!drag_item(window, QStringLiteral("midiClipMoveHandle"), 2.0 * bar_width, true) ||
            bridge.revision() != before_move + 1) {
          std::cerr << "MIDI smoke: elapsed clip drag failed: " << bridge.status().toStdString()
                    << '\n';
          return 1;
        }
        const auto moved =
            bridge.tracks().front().toMap().value(QStringLiteral("clips")).toList().front().toMap();
        if (moved.value(QStringLiteral("timebase")).toString() != QStringLiteral("Elapsed") ||
            std::abs(moved.value(QStringLiteral("start")).toDouble() -
                     2.0 / bridge.timeline_bars()) > 0.00001) {
          std::cerr << "MIDI smoke: elapsed drag changed the wrong clip or bar\n";
          return 1;
        }
        bridge.undo();
        QCoreApplication::processEvents();
        if (bridge.tracks()
                .front()
                .toMap()
                .value(QStringLiteral("clips"))
                .toList()
                .front()
                .toMap()
                .value(QStringLiteral("start"))
                .toDouble() != 0.0) {
          std::cerr << "MIDI smoke: elapsed drag did not undo\n";
          return 1;
        }
      }
    }
    QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, position.toPoint());
    QCoreApplication::processEvents();
    auto* remove =
        first_visible_item(window->contentItem(), QStringLiteral("deleteMidiClipAction"));
    if (!remove) {
      std::cerr << "MIDI smoke: delete context action missing\n";
      return 1;
    }
    remove->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Return);
    QCoreApplication::processEvents();
    if (bridge.tracks().front().toMap().value(QStringLiteral("clips")).toList().size() != 1) {
      std::cerr << "MIDI smoke: context delete did not remove clip\n";
      return 1;
    }
    bridge.undo();
    if (bridge.tracks().front().toMap().value(QStringLiteral("clips")).toList().size() != 2) {
      std::cerr << "MIDI smoke: delete did not undo\n";
      return 1;
    }
    engine.rootObjects().front()->setProperty("selectedClipId", clip_id);
#else
    std::cerr << "MIDI duplicate smoke requires Qt Test input support\n";
    return 1;
#endif
    bridge.save();
    if (!bridge.status().startsWith(QStringLiteral("Saved"))) {
      std::cerr << "MIDI smoke: save failed: " << bridge.status().toStdString() << '\n';
      return 1;
    }
    std::cout << "Qt Quick MIDI workflow saved\n";
  }
  if (smoke_verify_midi) {
    const auto tracks = bridge.tracks();
    if (tracks.size() != 1) {
      std::cerr << "MIDI smoke: reopen lost track\n";
      return 1;
    }
    const auto track = tracks.front().toMap();
    const auto clips = track.value(QStringLiteral("clips")).toList();
    if (!track.value(QStringLiteral("nativeSourceReady")).toBool() ||
        track.value(QStringLiteral("soundName")).toString() != QStringLiteral("Pluck") ||
        clips.size() != 2) {
      std::cerr << "MIDI smoke: reopen lost source or clip\n";
      return 1;
    }
    const auto notes = clips.front().toMap().value(QStringLiteral("notes")).toList();
    const auto copied_notes = clips.back().toMap().value(QStringLiteral("notes")).toList();
    if (notes.size() != 1 || notes.front().toMap().value(QStringLiteral("step")).toInt() != 4 ||
        notes.front().toMap().value(QStringLiteral("key")).toInt() != 60 ||
        copied_notes.size() != 1 ||
        copied_notes.front().toMap().value(QStringLiteral("startTicks")) !=
            notes.front().toMap().value(QStringLiteral("startTicks")) ||
        copied_notes.front().toMap().value(QStringLiteral("durationTicks")) !=
            notes.front().toMap().value(QStringLiteral("durationTicks")) ||
        copied_notes.front().toMap().value(QStringLiteral("key")) !=
            notes.front().toMap().value(QStringLiteral("key"))) {
      std::cerr << "MIDI smoke: reopen lost note\n";
      return 1;
    }
    engine.rootObjects().front()->setProperty(
        "selectedClipId", clips.front().toMap().value(QStringLiteral("id")).toString());
    std::cout << "Qt Quick MIDI workflow reopened\n";
  }
  if (smoke_transport || smoke_transport_fail) {
    const auto track_id = bridge.tracks().front().toMap().value(QStringLiteral("id")).toString();
    bridge.bind_starter_sound(track_id, 3);
    if (smoke_transport) {
      bridge.add_midi_clip(track_id, 0);
      const auto clips = bridge.tracks().front().toMap().value(QStringLiteral("clips")).toList();
      if (clips.empty()) {
        std::cerr << "Transport smoke: MIDI fixture was not created\n";
        return 1;
      }
      bridge.add_note(clips.front().toMap().value(QStringLiteral("id")).toString(), 0, 60);
    }
    if (!bridge.tracks().front().toMap().value(QStringLiteral("nativeSourceReady")).toBool()) {
      std::cerr << "Transport smoke: source was not bound\n";
      return 1;
    }
    bridge.toggle_playback();
    if (!bridge.playback_status().contains("Preparing")) {
      std::cerr << "Transport smoke: preparation did not start\n";
      return 1;
    }
  }
  const quick_smoke::ExportOptions export_options{
      smoke_export         ? quick_smoke::MasterExportScenario::success
      : smoke_export_fail  ? quick_smoke::MasterExportScenario::failure
      : smoke_export_close ? quick_smoke::MasterExportScenario::close
                           : quick_smoke::MasterExportScenario::none,
      smoke_stems,
      root,
      smoke_export_target,
      smoke_export_format,
      smoke_output_rate};
  if (!quick_smoke::start_master_export(export_options, app, engine, bridge, count_visible_items)) {
    return 1;
  }
  if (smoke && !smoke_audio_device && !smoke_export && !smoke_export_fail && !smoke_export_close &&
      !smoke_stems && !import_workflow && !live_workflow && !smoke_verify_live_edit &&
      !smoke_automation && !smoke_send_automation && !smoke_verify_send_automation &&
      !smoke_routing && !smoke_rate_audio && !smoke_verify_routing && !smoke_waveform &&
      smoke_tempo_mode.empty()) {
    const bool expect_project =
        !root.empty() || !welcome_create_parent.empty() || !welcome_open_folder.empty();
    const bool expect_piano = smoke_midi_workflow || smoke_verify_midi;
    QTimer::singleShot(
        800, &app,
        [&app, &engine, &bridge, root, screenshot, expect_project, expect_piano, smoke_grid,
         smoke_grid_source, smoke_chain_ui, smoke_transport, smoke_transport_fail,
         smoke_audio_gestures, smoke_verify_audio_gestures]() {
          auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
          if (window == nullptr || !window->isVisible() ||
              bridge.project_open() != expect_project) {
            std::cerr << "Smoke check failed: launch state mismatch: "
                      << bridge.status().toStdString() << '\n';
            app.exit(1);
            return;
          }
          const auto visible_welcome =
              count_visible_items(window->contentItem(), QStringLiteral("welcomeScreen"));
          const auto visible_arrangement =
              count_visible_items(window->contentItem(), QStringLiteral("arrangementView"));
          if (visible_welcome != (expect_project ? 0 : 1) ||
              visible_arrangement != (expect_project ? 1 : 0)) {
            std::cerr << "Smoke check failed: wrong visible screen\n";
            app.exit(1);
            return;
          }
          if (count_visible_items(window->contentItem(), QStringLiteral("togglePlayback")) !=
              (expect_project ? 1 : 0)) {
            std::cerr << "Smoke check failed: transport control visibility mismatch\n";
            app.exit(1);
            return;
          }
          if (!expect_project) {
            if (!screenshot.empty() &&
                !window->grabWindow().save(QString::fromStdString(screenshot), "PNG")) {
              std::cerr << "Smoke check failed: welcome screenshot could not be saved\n";
              app.exit(1);
              return;
            }
            std::cout << "Qt Quick welcome ready; projects: " << bridge.available_projects().size()
                      << '\n';
            app.quit();
            return;
          }
          if (bridge.revision() == 0) {
            std::cerr << "Smoke check failed: project revision missing\n";
            app.exit(1);
            return;
          }
          qsizetype projected_clips = 0;
          for (const auto& track : bridge.tracks()) {
            projected_clips += track.toMap().value(QStringLiteral("clips")).toList().size();
          }
          const auto rendered_clips = count_clip_items(window->contentItem());
          if (rendered_clips != projected_clips) {
            std::cerr << "Smoke check failed: clip projection " << projected_clips
                      << " and rendered delegates " << rendered_clips << " differ\n";
            app.exit(1);
            return;
          }
          if (expect_piano &&
              count_visible_items(window->contentItem(), QStringLiteral("pianoRoll")) != 1) {
            std::cerr << "Smoke check failed: piano roll did not show selected MIDI clip\n";
            app.exit(1);
            return;
          }
          if (smoke_audio_gestures) {
            auto* arrangement_item =
                first_visible_item(window->contentItem(), QStringLiteral("arrangementView"));
            const auto first_clip = [&bridge]() {
              return bridge.tracks()
                  .front()
                  .toMap()
                  .value(QStringLiteral("clips"))
                  .toList()
                  .front()
                  .toMap();
            };
            if (!arrangement_item || projected_clips != 2 || !bridge.audio_edit_enabled() ||
                count_visible_items(window->contentItem(), QStringLiteral("audioSnapToggle")) !=
                    1 ||
                count_visible_items(window->contentItem(), QStringLiteral("audioClipMoveHandle")) !=
                    2) {
              std::cerr << "Audio gesture smoke: handles or clip fixture absent\n";
              app.exit(1);
              return;
            }
            engine.rootObjects().front()->setProperty("visibleBars", 8);
            QCoreApplication::processEvents();
            const auto open_clip_action = [&](QQuickItem* clip, const QString& action_name) {
              if (!clip)
                return false;
              const auto clip_position =
                  clip->mapToScene({clip->width() / 2.0, clip->height() / 2.0});
              QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, clip_position.toPoint());
              QCoreApplication::processEvents();
              auto* action = first_visible_item(window->contentItem(), action_name);
              if (!action)
                return false;
              const auto action_position =
                  action->mapToScene({action->width() / 2.0, action->height() / 2.0});
              QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, action_position.toPoint());
              QCoreApplication::processEvents();
              return true;
            };
            const auto before_context = bridge.revision();
            if (!open_clip_action(
                    first_visible_item(window->contentItem(), QStringLiteral("arrangementClip")),
                    QStringLiteral("duplicateAudioClipAction")) ||
                bridge.revision() != before_context + 1) {
              std::cerr << "Audio gesture smoke: context duplicate failed: "
                        << bridge.status().toStdString() << '\n';
              app.exit(1);
              return;
            }
            const auto clips_after_copy =
                bridge.tracks().front().toMap().value(QStringLiteral("clips")).toList();
            QString copied_id;
            for (const auto& clip : clips_after_copy) {
              const auto item = clip.toMap();
              if (std::abs(item.value(QStringLiteral("start")).toDouble() - 1.0 / 16.0) < 0.00001)
                copied_id = item.value(QStringLiteral("id")).toString();
            }
            qsizetype copied_index = 2;
            auto* copied_item = nth_visible_item(window->contentItem(),
                                                 QStringLiteral("arrangementClip"), copied_index);
            if (copied_id.isEmpty() ||
                !open_clip_action(copied_item, QStringLiteral("deleteAudioClipAction")) ||
                bridge.revision() != before_context + 2 ||
                bridge.tracks().front().toMap().value(QStringLiteral("clips")).toList().size() !=
                    2) {
              std::cerr << "Audio gesture smoke: context delete failed: "
                        << bridge.status().toStdString() << '\n';
              app.exit(1);
              return;
            }
            bridge.seek_to_sample(48'000);
            const auto before_split = bridge.revision();
            const auto before_split_count =
                bridge.tracks().front().toMap().value(QStringLiteral("clips")).toList().size();
            if (!open_clip_action(
                    first_visible_item(window->contentItem(), QStringLiteral("arrangementClip")),
                    QStringLiteral("splitAudioClipAction")) ||
                bridge.revision() != before_split + 1 ||
                bridge.tracks().front().toMap().value(QStringLiteral("clips")).toList().size() !=
                    before_split_count + 1) {
              std::cerr << "Audio gesture smoke: context split failed: "
                        << bridge.status().toStdString() << '\n';
              app.exit(1);
              return;
            }
            bridge.undo();
            QCoreApplication::processEvents();
            if (bridge.tracks().front().toMap().value(QStringLiteral("clips")).toList().size() !=
                before_split_count) {
              std::cerr << "Audio gesture smoke: split undo failed\n";
              app.exit(1);
              return;
            }
            const auto before_timebase = bridge.revision();
            if (first_clip().value(QStringLiteral("timebase")).toString() !=
                    QStringLiteral("Elapsed") ||
                !open_clip_action(
                    first_visible_item(window->contentItem(), QStringLiteral("arrangementClip")),
                    QStringLiteral("toggleAudioClipTimebaseAction")) ||
                bridge.revision() != before_timebase + 1 ||
                first_clip().value(QStringLiteral("timebase")).toString() !=
                    QStringLiteral("Musical")) {
              std::cerr << "Audio gesture smoke: context timebase toggle failed: "
                        << bridge.status().toStdString() << '\n';
              app.exit(1);
              return;
            }
            bridge.undo();
            QCoreApplication::processEvents();
            if (first_clip().value(QStringLiteral("timebase")).toString() !=
                QStringLiteral("Elapsed")) {
              std::cerr << "Audio gesture smoke: timebase undo failed\n";
              app.exit(1);
              return;
            }
            const double bar_width = arrangement_item->width() / 8.0;
            const auto base_revision = bridge.revision();
            if (!drag_item(window, QStringLiteral("audioClipMoveHandle"), bar_width, true) ||
                bridge.revision() != base_revision + 1 ||
                std::abs(first_clip().value(QStringLiteral("start")).toDouble() - 1.0 / 16.0) >
                    0.00001) {
              std::cerr << "Audio gesture smoke: move failed: " << bridge.status().toStdString()
                        << '\n';
              app.exit(1);
              return;
            }
#ifdef OSCILLATE_QUICK_TEST_INPUT
            const auto move_revision = bridge.revision();
            auto* focused_clip =
                first_visible_item(window->contentItem(), QStringLiteral("arrangementClip"));
            if (!focused_clip) {
              std::cerr << "Audio gesture smoke: focusable clip absent\n";
              app.exit(1);
              return;
            }
            focused_clip->forceActiveFocus();
            QTest::keyClick(window, Qt::Key_Left);
            if (bridge.revision() != move_revision + 1 ||
                std::abs(first_clip().value(QStringLiteral("start")).toDouble() - 3.0 / 64.0) >
                    0.00001) {
              std::cerr << "Audio gesture smoke: keyboard beat nudge failed: "
                        << bridge.status().toStdString() << '\n';
              app.exit(1);
              return;
            }
            bridge.undo();
            QCoreApplication::processEvents();
            if (std::abs(first_clip().value(QStringLiteral("start")).toDouble() - 4.0 / 64.0) >
                0.00001) {
              std::cerr << "Audio gesture smoke: one undo did not restore keyboard move\n";
              app.exit(1);
              return;
            }
#endif
            const auto restored_revision = bridge.revision();
            bool fade_drag =
                drag_item(window, QStringLiteral("audioClipFadeInHandle"), bar_width / 4.0);
#ifdef OSCILLATE_QUICK_TEST_INPUT
            if (bridge.revision() == restored_revision) {
              // Undo may replace the delegate while the offscreen input event is in flight.
              QTest::qWait(20);
              if (bridge.revision() == restored_revision)
                fade_drag =
                    drag_item(window, QStringLiteral("audioClipFadeInHandle"), bar_width / 4.0);
            }
#endif
            if (count_visible_items(window->contentItem(),
                                    QStringLiteral("audioClipFadeInHandle")) == 0 ||
                !fade_drag || bridge.revision() != restored_revision + 1 ||
                first_clip().value(QStringLiteral("fadeInFraction")).toDouble() <= 0.0) {
              std::cerr << "Audio gesture smoke: fade handle did not edit clip: "
                        << bridge.status().toStdString() << '\n';
              app.exit(1);
              return;
            }
            bridge.set_audio_clip_fade(first_clip().value(QStringLiteral("id")).toString(), false,
                                       0.1, 1);
            if (bridge.revision() != restored_revision + 2 ||
                first_clip().value(QStringLiteral("fadeOutCurve")).toInt() != 1) {
              std::cerr << "Audio gesture smoke: fade curve setting failed\n";
              app.exit(1);
              return;
            }
            const auto fade_revision = bridge.revision();
            bool overlap_drag = drag_item(window, QStringLiteral("audioClipMoveHandle"), bar_width);
#ifdef OSCILLATE_QUICK_TEST_INPUT
            if (bridge.revision() == fade_revision &&
                !bridge.status().contains(QStringLiteral("rejected"))) {
              // Undo can recreate the QML delegate while an offscreen test is still dispatching
              // events. Retry only if the gesture caused no project edit or rejection at all.
              QTest::qWait(20);
              overlap_drag = drag_item(window, QStringLiteral("audioClipMoveHandle"), bar_width);
            }
#endif
            if (!overlap_drag || bridge.revision() != fade_revision ||
                !bridge.status().contains(QStringLiteral("rejected"))) {
              std::cerr << "Audio gesture smoke: overlap was accepted or gesture was not "
                           "delivered: revision "
                        << bridge.revision() << " expected " << fade_revision << "; "
                        << bridge.status().toStdString() << '\n';
              app.exit(1);
              return;
            }
            const double beat_width = bar_width / 4.0;
            if (!drag_item(window, QStringLiteral("audioClipRightTrimHandle"), -beat_width) ||
                bridge.revision() != fade_revision + 1) {
              std::cerr << "Audio gesture smoke: right trim failed: "
                        << bridge.status().toStdString() << '\n';
              app.exit(1);
              return;
            }
            const auto trim_revision = bridge.revision();
            bridge.undo();
            const auto restored_trim_ready = [&] {
              const auto clip = first_clip();
              const double start = clip.value(QStringLiteral("start")).toDouble();
              const double length = clip.value(QStringLiteral("length")).toDouble();
              auto* rendered =
                  first_visible_item(window->contentItem(), QStringLiteral("arrangementClip"));
              auto* handle =
                  rendered
                      ? first_visible_item(rendered, QStringLiteral("audioClipRightTrimHandle"))
                      : nullptr;
              const double expected_width =
                  std::max(7.0, arrangement_item->width() * (4.0 / 64.0) * 16.0 / 8.0 - 4.0);
              return bridge.revision() == trim_revision + 1 &&
                     std::abs(length - 4.0 / 64.0) < 0.00001 && rendered && handle &&
                     handle->isEnabled() && !rendered->property("gestureActive").toBool() &&
                     std::abs(rendered->property("previewStart").toDouble() - start) < 0.00001 &&
                     std::abs(rendered->property("previewEnd").toDouble() - (start + length)) <
                         0.00001 &&
                     std::abs(rendered->width() - expected_width) < 0.5;
            };
#ifdef OSCILLATE_QUICK_TEST_INPUT
            if (!QTest::qWaitFor(restored_trim_ready, 500)) {
#else
            QCoreApplication::processEvents();
            if (!restored_trim_ready()) {
#endif
              std::cerr << "Audio gesture smoke: undo model or right-trim delegate did not settle: "
                        << bridge.status().toStdString() << '\n';
              app.exit(1);
              return;
            }
            const auto post_undo_revision = bridge.revision();
            bool right_trim_after_undo =
                drag_item(window, QStringLiteral("audioClipRightTrimHandle"), -beat_width);
#ifdef OSCILLATE_QUICK_TEST_INPUT
            if (bridge.revision() == post_undo_revision &&
                std::abs(first_clip().value(QStringLiteral("length")).toDouble() - 4.0 / 64.0) <
                    0.00001 &&
                !bridge.status().contains(QStringLiteral("rejected")) &&
                QTest::qWaitFor(restored_trim_ready, 500)) {
              // A delegate replacement can consume the input without a model edit or rejection.
              right_trim_after_undo =
                  drag_item(window, QStringLiteral("audioClipRightTrimHandle"), -beat_width);
            }
#endif
            if (!right_trim_after_undo || bridge.revision() != post_undo_revision + 1 ||
                std::abs(first_clip().value(QStringLiteral("length")).toDouble() - 3.0 / 64.0) >
                    0.00001) {
              std::cerr << "Audio gesture smoke: right trim after undo did not apply: "
                        << bridge.status().toStdString() << '\n';
              app.exit(1);
              return;
            }
#ifdef OSCILLATE_QUICK_TEST_INPUT
            const double expected_width =
                std::max(7.0, arrangement_item->width() * (3.0 / 64.0) * 16.0 / 8.0 - 4.0);
            if (!QTest::qWaitFor(
                    [&] {
                      auto* rendered = first_visible_item(window->contentItem(),
                                                          QStringLiteral("arrangementClip"));
                      return rendered && std::abs(rendered->width() - expected_width) < 0.5;
                    },
                    500)) {
              std::cerr << "Audio gesture smoke: trimmed QML delegate did not settle\n";
              app.exit(1);
              return;
            }
#endif
            const bool left_trim_drag =
                drag_item(window, QStringLiteral("audioClipLeftTrimHandle"), beat_width);
            if (!left_trim_drag ||
                std::abs(first_clip().value(QStringLiteral("start")).toDouble() - 5.0 / 64.0) >
                    0.00001 ||
                std::abs(first_clip().value(QStringLiteral("length")).toDouble() - 2.0 / 64.0) >
                    0.00001) {
              std::cerr << "Audio gesture smoke: left trim failed: "
                        << bridge.status().toStdString() << "; delivered=" << left_trim_drag
                        << "; start=" << first_clip().value(QStringLiteral("start")).toDouble()
                        << "; length=" << first_clip().value(QStringLiteral("length")).toDouble()
                        << '\n';
              app.exit(1);
              return;
            }
            const auto before_slip = first_clip();
            const auto slip_revision = bridge.revision();
            if (!drag_item(window, QStringLiteral("audioClipMoveHandle"), beat_width / 2.0, true,
                           Qt::ShiftModifier | Qt::AltModifier) ||
                bridge.revision() != slip_revision + 1 ||
                first_clip().value(QStringLiteral("sourceStartFrame")).toLongLong() <=
                    before_slip.value(QStringLiteral("sourceStartFrame")).toLongLong() ||
                first_clip().value(QStringLiteral("start")) !=
                    before_slip.value(QStringLiteral("start")) ||
                first_clip().value(QStringLiteral("length")) !=
                    before_slip.value(QStringLiteral("length"))) {
              std::cerr << "Audio gesture smoke: Shift-drag slip changed placement or failed: "
                        << bridge.status().toStdString() << '\n';
              app.exit(1);
              return;
            }
            bridge.save();
            if (!bridge.status().startsWith(QStringLiteral("Saved"))) {
              std::cerr << "Audio gesture smoke: save failed\n";
              app.exit(1);
              return;
            }
            std::cout << "Qt Quick audio gestures saved\n";
          }
          if (smoke_verify_audio_gestures) {
            const auto clips =
                bridge.tracks().front().toMap().value(QStringLiteral("clips")).toList();
            if (clips.size() != 2 ||
                std::abs(clips.front().toMap().value(QStringLiteral("start")).toDouble() -
                         5.0 / 64.0) > 0.00001 ||
                std::abs(clips.front().toMap().value(QStringLiteral("length")).toDouble() -
                         2.0 / 64.0) > 0.00001 ||
                clips.front().toMap().value(QStringLiteral("fadeInFraction")).toDouble() <= 0.0 ||
                clips.front().toMap().value(QStringLiteral("sourceStartFrame")).toLongLong() <=
                    24'000 ||
                std::abs(clips.front().toMap().value(QStringLiteral("fadeOutFraction")).toDouble() -
                         0.1) > 0.00001 ||
                clips.front().toMap().value(QStringLiteral("fadeOutCurve")).toInt() != 1) {
              std::cerr << "Audio gesture smoke: saved placement did not reopen\n";
              app.exit(1);
              return;
            }
            std::cout << "Qt Quick audio gestures reopened\n";
          }
          if (smoke_transport || smoke_transport_fail) {
            if (smoke_transport &&
                (!bridge.playback_active() ||
                 !bridge.playback_status().contains(QStringLiteral("Playing routed revision")))) {
              std::cerr << "Transport smoke: playback did not activate: "
                        << bridge.playback_status().toStdString() << '\n';
              app.exit(1);
              return;
            }
            if (smoke_transport) {
              const auto meter = bridge.master_meter();
              auto* display =
                  first_visible_item(window->contentItem(), QStringLiteral("masterMeter"));
              if (!display || !meter.value(QStringLiteral("valid")).toBool() ||
                  meter.value(QStringLiteral("leftPeak")).toDouble() <= 0.0 ||
                  meter.value(QStringLiteral("leftRms")).toDouble() <= 0.0 ||
                  !QTest::qWaitFor(
                      [display] {
                        const auto* reading = display->property("reading").value<QObject*>();
                        return reading && reading->property("valid").toBool();
                      },
                      2'000)) {
                std::cerr << "Transport smoke: direct output meter was not visible or live\n";
                app.exit(1);
                return;
              }
            }
            if (smoke_transport_fail &&
                (bridge.playback_active() || !bridge.playback_enabled() ||
                 !bridge.playback_status().contains(QStringLiteral("Smoke output refused")))) {
              std::cerr << "Transport smoke: output failure not reported: "
                        << bridge.playback_status().toStdString() << '\n';
              app.exit(1);
              return;
            }
            if (smoke_transport) {
              bridge.exports()->set_export_target(
                  QString::fromStdString((root / "unexpected-playback-master.wav").string()));
              bridge.exports()->export_master();
              if (bridge.exports()->export_busy() ||
                  !bridge.exports()->export_status().contains(QStringLiteral("Stop playback"))) {
                std::cerr << "Transport smoke: WAV export ignored playback guard\n";
                app.exit(1);
                return;
              }
              const auto active_revision = bridge.revision();
              bridge.open_project(QUrl::fromLocalFile(QString::fromStdString(root.string())));
              if (!bridge.playback_active() || bridge.revision() != active_revision ||
                  !bridge.status().contains(QStringLiteral("Stop playback"))) {
                std::cerr << "Transport smoke: project switch displaced active playback\n";
                app.exit(1);
                return;
              }
              bridge.toggle_playback();
              if (!QTest::qWaitFor(
                      [&bridge] {
                        return !bridge.playback_active() && bridge.playback_enabled() &&
                               !bridge.master_meter().value(QStringLiteral("valid")).toBool();
                      },
                      10'000)) {
                std::cerr << "Transport smoke: Stop did not release playback\n";
                app.exit(1);
                return;
              }
            }
            std::cout << "Qt Quick transport "
                      << (smoke_transport_fail ? "output failure reported" : "played and stopped")
                      << '\n';
          }
          if (smoke_chain_ui) {
            auto* shell = engine.rootObjects().front();
            auto* chain_panel =
                first_visible_item(window->contentItem(), QStringLiteral("insertChainPanel"));
            auto* chain_resize = first_visible_item(window->contentItem(),
                                                    QStringLiteral("insertChainResizeHandle"));
            auto* path_name =
                first_visible_item(window->contentItem(), QStringLiteral("insertChainPathName"));
            auto* master_row =
                first_visible_item(window->contentItem(), QStringLiteral("masterTrackRow"));
            auto* master_arrangement =
                first_visible_item(window->contentItem(), QStringLiteral("masterArrangementRow"));
            auto* track_list =
                first_visible_item(window->contentItem(), QStringLiteral("trackList"));
            auto* track_viewport = first_visible_item(window->contentItem(),
                                                      QStringLiteral("arrangementTrackViewport"));
            auto* creation_row =
                first_visible_item(window->contentItem(), QStringLiteral("trackCreationRow"));
            auto* add_track_button =
                first_visible_item(window->contentItem(), QStringLiteral("addTrackButton"));
            qsizetype first_index = 0;
            qsizetype second_index = 1;
            auto* first_row =
                nth_visible_item(window->contentItem(), QStringLiteral("trackRow"), first_index);
            auto* second_row =
                nth_visible_item(window->contentItem(), QStringLiteral("trackRow"), second_index);
            // The header row (TRACKS, + Track) heads the list; the master is
            // pinned below it, level with its arrangement lane.
            if (!chain_panel || !chain_resize || !path_name || !master_row || !master_arrangement ||
                !track_list || !track_viewport || !creation_row || !add_track_button ||
                !first_row || !second_row || !track_list->clip() || !track_viewport->clip() ||
                bridge.tracks().size() != 11 ||
                bridge.insert_chains().size() != bridge.tracks().size() + 1 ||
                std::abs(second_row->mapToScene({0, 0}).y() -
                         (first_row->mapToScene({0, 0}).y() + first_row->height())) > 0.5 ||
                std::abs(track_list->mapToScene({0, 0}).y() -
                         (creation_row->mapToScene({0, 0}).y() + creation_row->height())) > 0.5 ||
                std::abs(master_row->mapToScene({0, 0}).y() -
                         (track_list->mapToScene({0, 0}).y() + track_list->height())) > 0.5 ||
                std::abs(master_row->mapToScene({0, 0}).y() -
                         master_arrangement->mapToScene({0, 0}).y()) > 0.5) {
              std::cerr << "Chain UI smoke: pinned master, saved paths or clipped rows missing"
                        << " tracks=" << bridge.tracks().size()
                        << " chains=" << bridge.insert_chains().size()
                        << " first=" << (first_row ? first_row->mapToScene({0, 0}).y() : -1)
                        << " second=" << (second_row ? second_row->mapToScene({0, 0}).y() : -1)
                        << " add=" << (creation_row ? creation_row->mapToScene({0, 0}).y() : -1)
                        << " list-bottom="
                        << (track_list ? track_list->mapToScene({0, 0}).y() + track_list->height()
                                       : -1)
                        << " master=" << (master_row ? master_row->mapToScene({0, 0}).y() : -1)
                        << " arrangement-master="
                        << (master_arrangement ? master_arrangement->mapToScene({0, 0}).y() : -1)
                        << '\n';
              app.exit(1);
              return;
            }
#ifdef OSCILLATE_QUICK_TEST_INPUT
            QTest::mouseClick(
                window, Qt::LeftButton, Qt::NoModifier,
                add_track_button
                    ->mapToScene({add_track_button->width() / 2, add_track_button->height() / 2})
                    .toPoint());
            QCoreApplication::processEvents();
            if (!creation_row->property("menuOpen").toBool() ||
                !first_visible_item(window->contentItem(), QStringLiteral("addTrackMenu"))) {
              std::cerr << "Chain UI smoke: pinned add-track chooser did not open\n";
              app.exit(1);
              return;
            }
            QTest::mouseClick(
                window, Qt::LeftButton, Qt::NoModifier,
                add_track_button
                    ->mapToScene({add_track_button->width() / 2, add_track_button->height() / 2})
                    .toPoint());
            QCoreApplication::processEvents();
            const double initial_chain_height = chain_panel->height();
            chain_resize->forceActiveFocus();
            QTest::keyClick(window, Qt::Key_Up);
            QCoreApplication::processEvents();
            if (chain_panel->height() != initial_chain_height + 24) {
              std::cerr << "Chain UI smoke: keyboard resize did not expand selected chain\n";
              app.exit(1);
              return;
            }
#endif
            const auto master_chain = bridge.insert_chains().back().toMap();
            const auto master_effects = master_chain.value(QStringLiteral("effects")).toList();
            if (master_effects.size() != 2 ||
                master_effects[0].toMap().value(QStringLiteral("name")) !=
                    QStringLiteral("Shape") ||
                master_effects[1].toMap().value(QStringLiteral("name")) !=
                    QStringLiteral("Press") ||
                master_effects[0].toMap().value(QStringLiteral("type")) !=
                    QStringLiteral("BUILT-IN")) {
              std::cerr << "Chain UI smoke: saved native master order is wrong\n";
              app.exit(1);
              return;
            }
            if (bridge.insert_chains()[1].toMap().value(QStringLiteral("sourceName")) !=
                    QStringLiteral("No instrument source") ||
                !bridge.insert_chains()[0]
                     .toMap()
                     .value(QStringLiteral("sourceName"))
                     .toString()
                     .isEmpty()) {
              std::cerr << "Chain UI smoke: instrument source leaked into audio inserts\n";
              app.exit(1);
              return;
            }
            for (int index : {0, 1, 2, 3}) {
              shell->setProperty("selectedTrack", index);
              QCoreApplication::processEvents();
              if (path_name->property("text").toString() !=
                      bridge.tracks()[index].toMap().value(QStringLiteral("name")).toString() ||
                  chain_panel->property("chain")
                          .toMap()
                          .value(QStringLiteral("effects"))
                          .toList()
                          .size() != 0) {
                std::cerr << "Chain UI smoke: selected track or bus did not own its chain\n";
                app.exit(1);
                return;
              }
            }
            shell->setProperty("selectedTrack", 0);
            QCoreApplication::processEvents();
            auto* add_eq =
                first_visible_item(window->contentItem(), QStringLiteral("deviceChainDropZone"));
            const auto audio_id = bridge.tracks()[0].toMap().value(QStringLiteral("id")).toString();
            const auto original_revision = bridge.revision();
            if (!add_eq || !add_eq->isEnabled() || audio_id.isEmpty()) {
              std::cerr << "Chain UI smoke: selected audio track has no native effect chooser\n";
              app.exit(1);
              return;
            }
#ifdef OSCILLATE_QUICK_TEST_INPUT
            QTest::mouseClick(
                window, Qt::LeftButton, Qt::NoModifier,
                add_eq->mapToScene({add_eq->width() / 2, add_eq->height() / 2}).toPoint());
            QCoreApplication::processEvents();
            auto* choose_eq =
                first_visible_item(window->contentItem(), QStringLiteral("insertAddNative_eq"));
            if (!choose_eq) {
              std::cerr << "Chain UI smoke: native effect chooser did not show Shape\n";
              app.exit(1);
              return;
            }
            QTest::mouseClick(
                window, Qt::LeftButton, Qt::NoModifier,
                choose_eq->mapToScene({choose_eq->width() / 2, choose_eq->height() / 2}).toPoint());
#else
            bridge.add_effect_engine(audio_id, 0, QStringLiteral("shape"));
#endif
            QCoreApplication::processEvents();
            auto first_chain =
                bridge.insert_chains()[0].toMap().value(QStringLiteral("effects")).toList();
            if (bridge.revision() != original_revision + 1 || first_chain.size() != 1 ||
                count_visible_items(window->contentItem(), QStringLiteral("insertEffectCard")) !=
                    1 ||
                !bridge.playback_status().contains(QStringLiteral("not active"))) {
              std::cerr
                  << "Chain UI smoke: Add Shape did not update saved chain and active status\n";
              app.exit(1);
              return;
            }
            const auto first_slot =
                first_chain[0].toMap().value(QStringLiteral("slotId")).toString();
#ifdef OSCILLATE_QUICK_TEST_INPUT
            // A built-in effect's panel opens floating from its strip.
            auto* open_shape =
                first_visible_item(window->contentItem(), QStringLiteral("insertOpenEditor"));
            if (!open_shape || !open_shape->isEnabled()) {
              std::cerr << "Chain UI smoke: saved Shape has no panel action\n";
              app.exit(1);
              return;
            }
            QTest::mouseClick(
                window, Qt::LeftButton, Qt::NoModifier,
                open_shape->mapToScene({open_shape->width() / 2, open_shape->height() / 2})
                    .toPoint());
            QCoreApplication::processEvents();
            auto* shape_panel = first_visible_item(window->contentItem(),
                                                   QStringLiteral("nativeEffectEditorOverlay"));
            if (!shape_panel || shape_panel->property("docked").toBool()) {
              std::cerr << "Chain UI smoke: Shape's panel did not open floating\n";
              app.exit(1);
              return;
            }
            if (!screenshot.empty()) {
              QTest::qWait(100);
              auto detailed_path = std::filesystem::path(screenshot);
              detailed_path.replace_extension(".detailed.png");
              if (!window->grabWindow().save(QString::fromStdString(detailed_path.string()),
                                             "PNG")) {
                std::cerr << "Chain UI smoke: detailed device screenshot failed\n";
                app.exit(1);
                return;
              }
            }
            bridge.close_native_effect_editor();
            QCoreApplication::processEvents();
#endif
            bridge.add_effect_engine(audio_id, 1, QStringLiteral("press"));
            const auto second_chain =
                bridge.insert_chains()[0].toMap().value(QStringLiteral("effects")).toList();
            const auto second_slot =
                second_chain[1].toMap().value(QStringLiteral("slotId")).toString();
            if (second_chain.size() != 2 ||
                second_chain[1].toMap().value(QStringLiteral("engine")) !=
                    QStringLiteral("press") ||
                !first_visible_item(window->contentItem(), QStringLiteral("deviceChainDropZone"))
                     ->isEnabled()) {
              std::cerr << "Chain UI smoke: Press chain or saved edit action missing\n";
              app.exit(1);
              return;
            }
            const auto before_third = bridge.revision();
            bridge.add_effect_engine(audio_id, 2, QStringLiteral("expanse"));
            const auto three_effects =
                bridge.insert_chains()[0].toMap().value(QStringLiteral("effects")).toList();
            QCoreApplication::processEvents();
            // Stopped, the next Play validates the saved chain; the engine
            // reports it (Engine details) without alarming the musician.
            if (bridge.revision() != before_third + 1 || three_effects.size() != 3 ||
                !bridge.playback_status().contains(QStringLiteral("not active"))) {
              std::cerr << "Chain UI smoke: saved third stage lost pending graph status: "
                        << bridge.playback_status().toStdString() << '\n';
              app.exit(1);
              return;
            }
            bridge.remove_effect_insert(
                audio_id, three_effects[2].toMap().value(QStringLiteral("slotId")).toString());
#ifdef OSCILLATE_QUICK_TEST_INPUT
            shell->setProperty("insertChainHeight", 312);
            QCoreApplication::processEvents();
            auto* chain_list =
                first_visible_item(window->contentItem(), QStringLiteral("insertChainList"));
            auto handle_index = [&](qsizetype index) {
              return nth_visible_item(window->contentItem(), QStringLiteral("insertDragHandle"),
                                      index);
            };
            if (!chain_list || chain_list->height() < 2 * 48 || !handle_index(1)) {
              std::cerr << "Chain UI smoke: drag handles are not visible in expanded panel\n";
              app.exit(1);
              return;
            }
            const auto revision_before_drag = bridge.revision();
            auto* second_handle = handle_index(1);
            const QPointF from = second_handle->mapToScene(
                {second_handle->width() / 2, second_handle->height() / 2});
            const QPointF to = from - QPointF(second_handle->parentItem()->width() + 18, 0);
            QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, from.toPoint());
            QTest::mouseMove(window, (from - QPointF(24, 0)).toPoint());
            QTest::mouseMove(window, to.toPoint());
            QCoreApplication::processEvents();
            if (!first_visible_item(window->contentItem(), QStringLiteral("insertDropMarker"))) {
              std::cerr << "Chain UI smoke: pointer reorder has no insertion marker\n";
              app.exit(1);
              return;
            }
            QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, to.toPoint());
            QCoreApplication::processEvents();
            const auto dragged_chain =
                bridge.insert_chains()[0].toMap().value(QStringLiteral("effects")).toList();
            if (bridge.revision() != revision_before_drag + 1 ||
                dragged_chain[0].toMap().value(QStringLiteral("slotId")) != second_slot ||
                dragged_chain[1].toMap().value(QStringLiteral("slotId")) != first_slot) {
              std::cerr << "Chain UI smoke: drag did not commit one stable-slot move\n";
              app.exit(1);
              return;
            }
            auto* first_handle = handle_index(0);
            const QPointF same_from =
                first_handle->mapToScene({first_handle->width() / 2, first_handle->height() / 2});
            QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, same_from.toPoint());
            QTest::mouseMove(window, (same_from + QPointF(12, 0)).toPoint());
            QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier,
                                (same_from + QPointF(12, 0)).toPoint());
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, same_from.toPoint());
            QCoreApplication::processEvents();
            if (bridge.revision() != revision_before_drag + 1) {
              std::cerr << "Chain UI smoke: same-row drag or click committed a move\n";
              app.exit(1);
              return;
            }
            QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, same_from.toPoint());
            QTest::mouseMove(
                window,
                (same_from + QPointF(first_handle->parentItem()->width() + 18, 0)).toPoint());
            QCoreApplication::processEvents();
            if (!first_visible_item(window->contentItem(), QStringLiteral("insertDropMarker"))) {
              std::cerr << "Chain UI smoke: cancellation fixture did not start a drag\n";
              app.exit(1);
              return;
            }
            first_handle->ungrabMouse();
            QTest::mouseRelease(
                window, Qt::LeftButton, Qt::NoModifier,
                (same_from + QPointF(first_handle->parentItem()->width() + 18, 0)).toPoint());
            QCoreApplication::processEvents();
            if (bridge.revision() != revision_before_drag + 1 ||
                first_visible_item(window->contentItem(), QStringLiteral("insertDropMarker"))) {
              std::cerr << "Chain UI smoke: canceled drag committed or retained its marker\n";
              app.exit(1);
              return;
            }
#else
            bridge.move_effect_insert(audio_id, second_slot, 0);
#endif
            bridge.set_effect_insert_enabled(audio_id, first_slot, false);
            const auto edited_chain =
                bridge.insert_chains()[0].toMap().value(QStringLiteral("effects")).toList();
            if (second_slot == first_slot || edited_chain.size() != 2 ||
                edited_chain[0].toMap().value(QStringLiteral("slotId")) != second_slot ||
                edited_chain[1].toMap().value(QStringLiteral("enabled")).toBool() ||
                !first_visible_item(window->contentItem(), QStringLiteral("insertMoveDown")) ||
                !first_visible_item(window->contentItem(), QStringLiteral("insertBypass")) ||
                !first_visible_item(window->contentItem(), QStringLiteral("insertRemove"))) {
              std::cerr << "Chain UI smoke: saved reorder or bypass controls failed\n";
              app.exit(1);
              return;
            }
            if (!screenshot.empty()) {
              QTest::qWait(100);
              auto selected_path = std::filesystem::path(screenshot);
              selected_path.replace_extension(".selected.png");
              if (!window->grabWindow().save(QString::fromStdString(selected_path.string()),
                                             "PNG")) {
                std::cerr << "Chain UI smoke: selected audio screenshot failed\n";
                app.exit(1);
                return;
              }
            }
            bridge.remove_effect_insert(audio_id, first_slot);
            bridge.save();
            if (bridge.insert_chains()[0]
                        .toMap()
                        .value(QStringLiteral("effects"))
                        .toList()
                        .size() != 1 ||
                !bridge.status().startsWith(QStringLiteral("Saved"))) {
              std::cerr << "Chain UI smoke: removed Shape did not leave a savable selected chain\n";
              app.exit(1);
              return;
            }
            const auto group_id = bridge.tracks()[2].toMap().value(QStringLiteral("id")).toString();
            // Later tracks go in above the return bus, so find it by kind.
            const auto chain_tracks = bridge.tracks();
            const auto return_row = static_cast<int>(
                std::ranges::find_if(
                    chain_tracks,
                    [](const QVariant& track) {
                      return track.toMap().value(QStringLiteral("kind")).toString() ==
                             QStringLiteral("return");
                    }) -
                chain_tracks.begin());
            const auto return_id =
                chain_tracks[return_row].toMap().value(QStringLiteral("id")).toString();
            const auto before_second_path = bridge.revision();
            bridge.add_effect_engine(group_id, 0, QStringLiteral("expanse"));
            const auto parallel_group =
                bridge.insert_chains()[2].toMap().value(QStringLiteral("effects")).toList();
            if (bridge.revision() != before_second_path + 1 || parallel_group.size() != 1 ||
                !bridge.playback_status().contains(QStringLiteral("not active"))) {
              std::cerr << "Chain UI smoke: bus edit was blocked by another saved path\n";
              app.exit(1);
              return;
            }
            bridge.remove_effect_insert(
                group_id, parallel_group[0].toMap().value(QStringLiteral("slotId")).toString());
            bridge.remove_effect_insert(audio_id, second_slot);
            shell->setProperty("selectedTrack", 2);
            QCoreApplication::processEvents();
            auto* bus_add =
                first_visible_item(window->contentItem(), QStringLiteral("deviceChainDropZone"));
            if (!bus_add || !bus_add->isEnabled()) {
              std::cerr << "Chain UI smoke: selected group has no native insert action\n";
              app.exit(1);
              return;
            }
            bridge.add_effect_engine(group_id, 0, QStringLiteral("expanse"));
            bridge.add_effect_engine(group_id, 1, QStringLiteral("ceiling"));
            const auto group_effects =
                bridge.insert_chains()[2].toMap().value(QStringLiteral("effects")).toList();
            if (group_effects.size() != 2 ||
                group_effects[0].toMap().value(QStringLiteral("engine")) !=
                    QStringLiteral("expanse") ||
                group_effects[1].toMap().value(QStringLiteral("engine")) !=
                    QStringLiteral("ceiling")) {
              std::cerr << "Chain UI smoke: group reverb and limiter were not projected\n";
              app.exit(1);
              return;
            }
            const auto reverb_slot =
                group_effects[0].toMap().value(QStringLiteral("slotId")).toString();
            const auto limiter_slot =
                group_effects[1].toMap().value(QStringLiteral("slotId")).toString();
            bridge.remove_effect_insert(group_id, reverb_slot);
            bridge.remove_effect_insert(group_id, limiter_slot);
            shell->setProperty("selectedTrack", return_row);
            QCoreApplication::processEvents();
            if (!first_visible_item(window->contentItem(), QStringLiteral("deviceChainDropZone"))
                     ->isEnabled()) {
              std::cerr << "Chain UI smoke: return bus insert action is disabled\n";
              app.exit(1);
              return;
            }
            bridge.add_effect_engine(return_id, 0, QStringLiteral("press"));
            const auto return_effects = bridge.insert_chains()[return_row]
                                            .toMap()
                                            .value(QStringLiteral("effects"))
                                            .toList();
            if (return_effects.size() != 1 || return_effects[0].toMap().value(QStringLiteral(
                                                  "engine")) != QStringLiteral("press")) {
              std::cerr << "Chain UI smoke: return bus insert did not retain its engine\n";
              app.exit(1);
              return;
            }
            bridge.remove_effect_insert(
                return_id, return_effects[0].toMap().value(QStringLiteral("slotId")).toString());
            const double scroll_end =
                track_list->property("contentHeight").toDouble() - track_list->height();
            track_list->setProperty("contentY", scroll_end);
            QCoreApplication::processEvents();
            qsizetype track_row_index = bridge.tracks().size() - 1;
            auto* track_row = nth_visible_item(window->contentItem(), QStringLiteral("trackRow"),
                                               track_row_index);
            qsizetype arrangement_row_index = bridge.tracks().size() - 1;
            auto* arrangement_row = nth_visible_item(
                window->contentItem(), QStringLiteral("arrangementRow"), arrangement_row_index);
            if (scroll_end <= 0 || shell->property("trackScrollY").toDouble() <= 0 || !track_row ||
                !arrangement_row ||
                std::abs(track_row->mapToScene({0, 0}).y() -
                         arrangement_row->mapToScene({0, 0}).y()) > 0.5) {
              std::cerr << "Chain UI smoke: lower tracks are not reachable in aligned viewports"
                        << " scroll-end=" << scroll_end
                        << " track-scroll=" << shell->property("trackScrollY").toDouble()
                        << " track-row=" << (track_row ? track_row->mapToScene({0, 0}).y() : -1)
                        << " arrangement-row="
                        << (arrangement_row ? arrangement_row->mapToScene({0, 0}).y() : -1) << "\n";
              app.exit(1);
              return;
            }
#ifdef OSCILLATE_QUICK_TEST_INPUT
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                              track_row->mapToScene({30, 40}).toPoint());
            QCoreApplication::processEvents();
            if (shell->property("selectedTrack").toInt() !=
                static_cast<int>(bridge.tracks().size() - 1)) {
              std::cerr << "Chain UI smoke: last scrolled track cannot be selected\n";
              app.exit(1);
              return;
            }
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                              master_row->mapToScene({30, 35}).toPoint());
#else
            shell->setProperty("selectedTrack", -2);
#endif
            QCoreApplication::processEvents();
            if (shell->property("selectedTrack").toInt() != -2 ||
                path_name->property("text") != QStringLiteral("Master") ||
                count_visible_items(window->contentItem(), QStringLiteral("insertEffectCard")) !=
                    2) {
              std::cerr << "Chain UI smoke: selected master did not show its chain\n";
              app.exit(1);
              return;
            }
            auto* master_add =
                first_visible_item(window->contentItem(), QStringLiteral("deviceChainDropZone"));
            if (!master_add || !master_add->isEnabled() ||
                chain_panel->property("pathId").toString() != QStringLiteral("master")) {
              std::cerr << "Chain UI smoke: pinned master has no common insert action\n";
              app.exit(1);
              return;
            }
            const auto master_before = bridge.revision();
            bridge.add_effect_engine(QStringLiteral("master"), 2, QStringLiteral("expanse"));
            auto master_saved =
                bridge.insert_chains().back().toMap().value(QStringLiteral("effects")).toList();
            const auto added_master =
                master_saved.size() > 2 ? master_saved[2].toMap() : QVariantMap{};
            if (bridge.revision() != master_before + 1 || master_saved.size() != 3 ||
                added_master.value(QStringLiteral("engine")) != QStringLiteral("expanse")) {
              std::cerr << "Chain UI smoke: master insert was not saved in order: revision="
                        << bridge.revision() << " expected=" << master_before + 1
                        << " size=" << master_saved.size() << " engine="
                        << added_master.value(QStringLiteral("engine")).toString().toStdString()
                        << " status=" << bridge.status().toStdString() << '\n';
              app.exit(1);
              return;
            }
            const auto master_slot =
                master_saved[2].toMap().value(QStringLiteral("slotId")).toString();
            bridge.move_effect_insert(QStringLiteral("master"), master_slot, 0);
            bridge.set_effect_insert_enabled(QStringLiteral("master"), master_slot, false);
            master_saved =
                bridge.insert_chains().back().toMap().value(QStringLiteral("effects")).toList();
            if (master_saved[0].toMap().value(QStringLiteral("slotId")) != master_slot ||
                master_saved[0].toMap().value(QStringLiteral("enabled")).toBool()) {
              std::cerr << "Chain UI smoke: master insert move or bypass failed\n";
              app.exit(1);
              return;
            }
            bridge.remove_effect_insert(QStringLiteral("master"), master_slot);
            QCoreApplication::processEvents();
            if (bridge.insert_chains()
                    .back()
                    .toMap()
                    .value(QStringLiteral("effects"))
                    .toList()
                    .size() != 2) {
              std::cerr << "Chain UI smoke: master insert removal failed\n";
              app.exit(1);
              return;
            }
#ifdef OSCILLATE_QUICK_TEST_INPUT
            const auto count_audio = [&bridge] {
              const auto tracks = bridge.tracks();
              return std::ranges::count_if(tracks, [](const QVariant& track) {
                return track.toMap().value(QStringLiteral("kind")).toString() ==
                       QStringLiteral("audio");
              });
            };
            const auto audio_before = count_audio();
            QMimeData audio_drop;
            audio_drop.setUrls(
                {QUrl::fromLocalFile(QString::fromStdString(smoke_grid_source.string()))});
            const QPoint drop_at =
                track_viewport->mapToScene({track_viewport->width() / 2, 30}).toPoint();
            QDragEnterEvent enter(drop_at, Qt::CopyAction, &audio_drop, Qt::LeftButton,
                                  Qt::NoModifier);
            QCoreApplication::sendEvent(window, &enter);
            QDropEvent drop(drop_at, Qt::CopyAction, &audio_drop, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(window, &drop);
            QElapsedTimer import_wait;
            import_wait.start();
            while (bridge.tracks().size() == 11 && import_wait.elapsed() < 5'000)
              QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            // The new audio track goes in above the return bus.
            if (!enter.isAccepted() || !drop.isAccepted() || bridge.tracks().size() != 12 ||
                count_audio() != audio_before + 1) {
              std::cerr << "Chain UI smoke: file drop did not create a managed audio track\n";
              app.exit(1);
              return;
            }
#endif
            std::cout << "Qt Quick selected chain and pinned master ready\n";
          }
          const auto rendered_image =
              screenshot.empty() && !smoke_grid ? QImage{} : window->grabWindow();
          if (smoke_grid && !smoke_chain_ui && !arrangement_grid_visible(rendered_image, window)) {
            std::cerr << "Grid smoke: bar and beat lines do not cross both empty rows\n";
            app.exit(1);
            return;
          }
          if (!screenshot.empty() &&
              !rendered_image.save(QString::fromStdString(screenshot), "PNG")) {
            std::cerr << "Smoke check failed: screenshot could not be saved\n";
            app.exit(1);
            return;
          }
          std::cout << "Qt Quick project view: revision " << bridge.revision() << ", "
                    << bridge.tracks().size() << " tracks; clip delegates: " << rendered_clips
                    << '\n';
          if (smoke_grid && !smoke_chain_ui) {
            // Ctrl+wheel over the arrangement zooms in around the pointer;
            // a sideways swipe scrolls through time.
            auto* view =
                first_visible_item(window->contentItem(), QStringLiteral("arrangementView"));
            const auto bars = window->property("visibleBars").toDouble();
            const auto at =
                view ? view->mapToScene(QPointF(view->width() / 2, view->height() / 3)) : QPointF{};
            const auto wheel = [&](QPoint pixels, QPoint angle, Qt::KeyboardModifiers modifiers) {
              QWheelEvent event(at, window->mapToGlobal(at), pixels, angle, Qt::NoButton, modifiers,
                                Qt::NoScrollPhase, false);
              QCoreApplication::sendEvent(window, &event);
              QCoreApplication::processEvents();
            };
            wheel({}, {0, 120}, Qt::ControlModifier);
            const auto zoomed = window->property("visibleBars").toDouble();
            wheel({0, 60}, {}, Qt::ControlModifier);
            const auto smooth_zoomed = window->property("visibleBars").toDouble();
            wheel({-60, 0}, {-120, 0}, Qt::NoModifier);
            if (!view || !(smooth_zoomed < zoomed && zoomed < bars) ||
                window->property("viewStartBar").toDouble() <= 0.0) {
              std::cerr << "Grid smoke: Ctrl+wheel or smooth scroll did not zoom (" << bars
                        << " -> " << zoomed << " -> " << smooth_zoomed
                        << " bars), or a swipe did not scroll\n";
              app.exit(1);
              return;
            }
            std::cout << "Qt Quick arrangement grid visible at " << rendered_image.width() << 'x'
                      << rendered_image.height() << '\n';
          }
          app.quit();
        });
  }
  quick_smoke::observe_master_export(export_options, app, bridge);
  if (!quick_smoke::start_stem_export(export_options, app, engine, bridge, count_visible_items)) {
    return 1;
  }
  quick_smoke::observe_import(import_scenario, app, bridge);
  return app.exec();
}

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
