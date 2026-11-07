#include "../../../apps/daw_quick/src/loop_transport.hpp"
#include "../../../apps/daw_quick/src/project_bridge.hpp"
#include "oscillate/audio_io/stereo_block.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/plugins/vst3_scan.hpp"
#include "oscillate/test/check.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QScreen>
#include <QTest>

#include <X11/Xlib.h>
#include <dlfcn.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
namespace app = oscillate::application;
namespace audio_io = oscillate::audio_io;
namespace engine = oscillate::engine;
namespace foundation = oscillate::foundation;
namespace plugins = oscillate::plugins;
namespace persistence = oscillate::persistence;
namespace project = oscillate::project;
namespace quick = oscillate::quick;
namespace timeline = oscillate::timeline;
namespace fs = std::filesystem;

constexpr std::string_view surge_class = "abcdef019182faeb566d624153675854";
constexpr std::string_view surge_hash =
    "b8584398f314819241ca7287117d5559ed838d37ef08faab9847f27ba2993cec";
constexpr std::uint32_t cutoff_id = 1945359057;
constexpr std::uint32_t filter_type_id = 1537728286;

using oscillate::test::require;

template <typename T> T take(foundation::Result<T> result) {
  if (!result)
    throw std::runtime_error(result.error().message);
  return std::move(result).value();
}
void take(foundation::Result<void> result) {
  if (!result)
    throw std::runtime_error(result.error().message);
}

class Scratch final {
public:
  explicit Scratch(fs::path root) : root_(std::move(root)) {
    require(fs::is_directory(root_), "scratch directory does not exist");
    root_ /= "surge-native-live-" + std::to_string(::getpid());
    require(fs::create_directory(root_), "per-run scratch directory could not be created");
  }
  ~Scratch() {
    std::error_code ignored;
    fs::remove_all(root_, ignored);
  }
  [[nodiscard]] const fs::path& root() const noexcept { return root_; }

private:
  fs::path root_;
};

// The fake device clocks a real routed callback without touching the physical
// audio sink. The two full-loop baseline windows bound natural variation before
// the held-drag window is compared with them.
struct AudioCapture {
  static_assert(std::atomic<double>::is_always_lock_free);
  struct Reading {
    double absolute_sum;
    std::uint64_t samples;
  };
  std::atomic<double> absolute_sum{0.0};
  std::atomic<std::uint64_t> samples{0};
  std::atomic<bool> pause_requested{false};
  std::atomic<bool> parked{false};
  [[nodiscard]] Reading read() const noexcept {
    return {absolute_sum.load(std::memory_order_acquire), samples.load(std::memory_order_acquire)};
  }
};

[[nodiscard]] double average_level(AudioCapture::Reading start, AudioCapture::Reading end) {
  require(end.samples > start.samples, "audio capture did not advance");
  return (end.absolute_sum - start.absolute_sum) / static_cast<double>(end.samples - start.samples);
}

class PacingOutput final : public app::PlaybackOutput {
public:
  PacingOutput(audio_io::StereoRenderSource source, audio_io::TransportCursor& cursor,
               std::shared_ptr<AudioCapture> capture)
      : source_(source), cursor_(cursor), capture_(std::move(capture)) {}
  ~PacingOutput() override { stop(); }

  foundation::Result<void> start(std::uint32_t, std::uint32_t frames,
                                 std::optional<std::uint64_t>) override {
    if (frames != 64 && frames != 128)
      return foundation::Result<void>::failure(
          {foundation::ErrorCode::invalid_argument, "smoke requires 64 or 128 frames"});
    running_.store(true, std::memory_order_release);
    worker_ = std::thread([this, frames] {
      std::array<engine::StereoFrame, 128> render{};
      std::array<float, 256> output{};
      while (running_.load(std::memory_order_acquire)) {
        if (capture_->pause_requested.load(std::memory_order_acquire)) {
          capture_->parked.store(true, std::memory_order_release);
          while (capture_->pause_requested.load(std::memory_order_acquire) &&
                 running_.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          capture_->parked.store(false, std::memory_order_release);
          continue;
        }
        (void)audio_io::render_transport_block(
            source_, cursor_, true, std::span(render).first(frames),
            std::span(output).first(static_cast<std::size_t>(frames) * 2));
        double absolute_sum = 0.0;
        for (const auto sample : std::span(output).first(static_cast<std::size_t>(frames) * 2))
          absolute_sum += std::abs(static_cast<double>(sample));
        capture_->absolute_sum.fetch_add(absolute_sum, std::memory_order_release);
        capture_->samples.fetch_add(static_cast<std::size_t>(frames) * 2,
                                    std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
    return foundation::Result<void>::success();
  }
  void stop() noexcept override {
    running_.store(false, std::memory_order_release);
    if (worker_.joinable())
      worker_.join();
  }
  [[nodiscard]] audio_io::OutputStatus status() const noexcept override {
    audio_io::OutputStatus result;
    result.state = running_.load(std::memory_order_acquire) ? audio_io::OutputState::streaming
                                                            : audio_io::OutputState::stopped;
    return result;
  }

private:
  audio_io::StereoRenderSource source_;
  audio_io::TransportCursor& cursor_;
  std::shared_ptr<AudioCapture> capture_;
  std::atomic<bool> running_{false};
  std::thread worker_;
};

class XTestDrag final {
public:
  XTestDrag() {
    library_ = ::dlopen("libXtst.so.6", RTLD_NOW | RTLD_LOCAL);
    require(library_ != nullptr, "libXtst.so.6 unavailable");
    display_ = XOpenDisplay(nullptr);
    require(display_ != nullptr, "isolated X11 display unavailable");
    motion_ = reinterpret_cast<Motion>(::dlsym(library_, "XTestFakeMotionEvent"));
    button_ = reinterpret_cast<Button>(::dlsym(library_, "XTestFakeButtonEvent"));
    require(motion_ && button_, "XTest pointer entry points unavailable");
  }
  ~XTestDrag() {
    if (held_ && display_ && button_) {
      (void)button_(display_, 1, False, 0);
      XSync(display_, False);
    }
    if (display_)
      XCloseDisplay(display_);
    if (library_)
      ::dlclose(library_);
  }
  void press_and_move(QWindow& window, QPoint start, QPoint finish) {
    const auto from = window.mapToGlobal(start);
    const auto to = window.mapToGlobal(finish);
    require(motion_(display_, -1, from.x(), from.y(), 0) != 0, "XTest move to knob failed");
    require(button_(display_, 1, True, 0) != 0, "XTest press failed");
    held_ = true;
    XSync(display_, False);
    QTest::qWait(60);
    require(motion_(display_, -1, to.x(), to.y(), 0) != 0, "XTest drag failed");
    XSync(display_, False);
    std::cout << "drag_global=" << from.x() << ',' << from.y() << " -> " << to.x() << ',' << to.y()
              << '\n';
  }
  void release() {
    require(button_(display_, 1, False, 0) != 0, "XTest release failed");
    held_ = false;
    XSync(display_, False);
  }

private:
  using Motion = int (*)(Display*, int, int, int, unsigned long);
  using Button = int (*)(Display*, unsigned int, Bool, unsigned long);
  void* library_ = nullptr;
  Display* display_ = nullptr;
  Motion motion_ = nullptr;
  Button button_ = nullptr;
  bool held_ = false;
};

void run(const fs::path& bundle, const fs::path& scratch_root, int requested_frames) {
  require(QApplication::platformName() == QStringLiteral("xcb"),
          "use QT_QPA_PLATFORM=xcb on an isolated X11 display");
  Scratch scratch(scratch_root);
  qputenv("XDG_CONFIG_HOME", QByteArray::fromStdString((scratch.root() / "config").string()));
  plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto receipt = scanner.scan(bundle);
  require(receipt.ok(), "out-of-process Surge scan failed");
  require(receipt.binary.sha256 == surge_hash, "Surge binary changed; smoke refuses it");
  const auto klass =
      std::ranges::find(receipt.metadata.classes, surge_class, &plugins::Vst3Class::id);
  require(klass != receipt.metadata.classes.end() && klass->category == "Audio Module Class" &&
              klass->name == "Surge XT",
          "expected Surge XT instrument class unavailable");
  const auto root = scratch.root() / "song";
  auto session = take(app::ProjectSession::create(root));
  const app::Vst3InsertIdentity identity{klass->id,
                                         klass->vendor,
                                         klass->name,
                                         klass->version,
                                         receipt.binary.canonical_bundle.string(),
                                         receipt.binary.sha256};
  const auto track = take(session->instruments().create_vst3_instrument_track(identity));
  take(session->midi_clips().add_midi_clip(track, {0}));
  const auto clip = session->snapshot().tracks.front().midi_clips.front().id;
  for (int step = 0; step < 16; ++step)
    take(session->notes().add_note(clip, {step * timeline::ticks_per_quarter / 4}, 60));
  const auto default_plugin =
      std::get<project::Vst3PluginInstance>(session->snapshot().tracks.front().instrument_source);
  // This pinned patch turns Filter 1 on. The default Surge patch leaves it Off,
  // so a cutoff gesture on that patch cannot establish an audible response.
  take(session->instruments().set_vst3_instrument_state(
      session->snapshot().revision, track, default_plugin, default_plugin.component_state,
      default_plugin.controller_state, {{filter_type_id, 0.1}, {cutoff_id, 0.95}}));
  take(session->saving().save());
  const auto initial_revision = session->snapshot().revision;
  const auto initial_plugin =
      std::get<project::Vst3PluginInstance>(session->snapshot().tracks.front().instrument_source);
  require(initial_plugin.parameters.size() == 2,
          "enabled-filter fixture did not retain both parameter references");

  auto capture = std::make_shared<AudioCapture>();
  std::optional<project::Vst3PluginInstance> committed_pair;
  {
    quick::ProjectBridge bridge(
        scratch.root(), nullptr,
        [capture](audio_io::StereoRenderSource source, audio_io::TransportCursor& cursor) {
          return std::make_unique<PacingOutput>(source, cursor, capture);
        },
        [] { return foundation::Result<std::vector<audio_io::PipeWireSink>>::success({}); });
    bridge.set_project(std::move(session), root);
    bridge.audio_output()->select_buffer_frames(requested_frames);
    require(bridge.audio_output()->requested_buffer_frames() == requested_frames,
            "requested test buffer size was not selected");
    auto* loop = qobject_cast<quick::LoopTransport*>(bridge.loop_transport());
    require(loop != nullptr, "loop transport unavailable");
    loop->set_range(1, 2);
    loop->set_enabled(true);
    QQmlApplicationEngine shell;
    shell.rootContext()->setContextProperty("projectBridge", &bridge);
    shell.load(QUrl(QStringLiteral("qrc:/DawShell.qml")));
    require(!shell.rootObjects().isEmpty(), "Qt Quick shell did not load");
    auto* window = qobject_cast<QQuickWindow*>(shell.rootObjects().front());
    require(window != nullptr, "Qt Quick root is not a window");
    window->show();
    bridge.vst3_browser()->add_vst3_search_path(QString::fromStdString(bundle.string()));
    require(QTest::qWaitFor([&] { return !bridge.vst3_browser()->busy(); }, 15'000),
            "Surge catalogue scan did not settle");
    bridge.toggle_playback();
    require(QTest::qWaitFor(
                [&] {
                  return bridge.playback_active() &&
                         bridge.playback_status().startsWith(
                             QStringLiteral("Playing routed revision %1").arg(initial_revision));
                },
                15'000),
            "routed playback did not activate the initial Surge graph");
    bridge.vst3_editor()->open_vst3_instrument_editor(QString::fromStdString(track.to_string()));
    require(QTest::qWaitFor(
                [&] {
                  return bridge.vst3_editor()->editor().value(QStringLiteral("native")).toBool() &&
                         bridge.vst3_editor()->native_window() != nullptr &&
                         bridge.vst3_editor()->native_window()->isVisible();
                },
                5'000),
            "real Surge editor did not embed in the Quick shell");
    QTest::qWait(150);
    const auto before_start = capture->read();
    QTest::qWait(1'600);
    const auto baseline_mid = capture->read();
    QTest::qWait(1'600);
    const auto before_end = capture->read();
    const auto first_baseline = average_level(before_start, baseline_mid);
    const auto before_level = average_level(baseline_mid, before_end);
    require(before_level > 0.001, "enabled-filter fixture produced no baseline sound");
    require(std::abs(first_baseline - before_level) < before_level * 0.2,
            "unchanged Surge loop was not a stable audio comparison baseline");
    const auto before_image = bridge.vst3_editor()
                                  ->native_window()
                                  ->screen()
                                  ->grabWindow(bridge.vst3_editor()->native_window()->winId())
                                  .toImage();
    require(!before_image.isNull(), "native editor before image could not be captured");
    if (qEnvironmentVariableIsSet("SURGE_CAPTURE_IMAGES"))
      (void)before_image.save(QString::fromStdString((scratch_root / "surge-before.png").string()));
    XTestDrag input;
    // The exact pinned 1.3.4 editor exposes Filter 1 Cutoff at these
    // shell-local coordinates (913 x 569 content). This is a test fixture,
    // not a production mapping of Surge controls.
    input.press_and_move(*bridge.vst3_editor()->native_window(), {375, 225}, {175, 225});
    const auto held_start = capture->read();
    QTest::qWait(1'600); // The parameter is previewed while the mouse remains held.
    const auto held_end = capture->read();
    const auto held_level = average_level(held_start, held_end);
    const auto held_image = bridge.vst3_editor()
                                ->native_window()
                                ->screen()
                                ->grabWindow(bridge.vst3_editor()->native_window()->winId())
                                .toImage();
    require(!held_image.isNull(), "native editor held image could not be captured");
    if (qEnvironmentVariableIsSet("SURGE_CAPTURE_IMAGES"))
      (void)held_image.save(QString::fromStdString((scratch_root / "surge-held.png").string()));
    require(before_image.size() == held_image.size() && held_image.width() > 455 &&
                held_image.height() > 255,
            "native editor image changed size during the gesture");
    std::size_t changed_cutoff_pixels = 0;
    // Pinned 1.3.4 skin fixture: this small region contains only Filter 1's
    // Cutoff slider and value text, not the animated oscillator/LFO panels.
    for (int y = 210; y < 255; ++y)
      for (int x = 330; x < 455; ++x)
        changed_cutoff_pixels += before_image.pixel(x, y) != held_image.pixel(x, y);
    require(changed_cutoff_pixels > 200,
            "native editor cutoff control did not visibly move during the drag");
    std::cout << "audio_before=" << before_level << " audio_while_held=" << held_level << '\n';
    std::cout << "changed_cutoff_pixels=" << changed_cutoff_pixels << '\n';
    require(held_level < before_level * 0.7,
            "native knob preview did not audibly change the routed Surge output");
    require(bridge.revision() == initial_revision,
            "performEdit during drag committed before endEdit");
    require(bridge.playback_active(), "holding a native knob stopped playback");
    // Hold the fake device between callbacks, not inside the callback. This
    // makes the receipt order deterministic while the real editor keeps
    // sending native gesture callbacks on the UI thread.
    capture->pause_requested.store(true, std::memory_order_release);
    require(QTest::qWaitFor([&] { return capture->parked.load(std::memory_order_acquire); }, 5'000),
            "fake output did not park before the rapid gesture sequence");
    input.release();
    require(QTest::qWaitFor([&] { return bridge.revision() == initial_revision + 1; }, 8'000),
            "endEdit did not commit one saved project revision");
    require(bridge.playback_active(), "first native gesture stopped playback");
    // Complete an opposite gesture while the first save still lacks an audio
    // receipt. The bridge must retain this completed gesture for later.
    input.press_and_move(*bridge.vst3_editor()->native_window(), {375, 225}, {575, 225});
    input.release();
    require(bridge.revision() == initial_revision + 1,
            "second gesture committed over an unacknowledged first revision");
    // A third drag supersedes the transient preview while gesture two waits
    // to commit. Its held value must remain audible after gesture two's
    // receipt; otherwise draining the queue erased a newer preview.
    input.press_and_move(*bridge.vst3_editor()->native_window(), {375, 225}, {175, 225});
    require(bridge.revision() == initial_revision + 1,
            "held third gesture created a saved revision before endEdit");
    capture->pause_requested.store(false, std::memory_order_release);
    const bool queued_second_live = QTest::qWaitFor(
        [&] {
          return bridge.revision() == initial_revision + 2 &&
                 bridge.vst3_editor()
                     ->editor()
                     .value(QStringLiteral("liveStatus"))
                     .toString()
                     .startsWith(
                         QStringLiteral("Revision %1 live from sample ").arg(initial_revision + 2));
        },
        8'000);
    require(queued_second_live,
            "queued second native gesture did not commit and reach audio: revision " +
                std::to_string(bridge.revision()) + ", status " +
                bridge.vst3_editor()
                    ->editor()
                    .value(QStringLiteral("liveStatus"))
                    .toString()
                    .toStdString() +
                ", playback " + bridge.playback_status().toStdString() + ", samples " +
                std::to_string(capture->read().samples) + ", parked " +
                std::to_string(capture->parked.load(std::memory_order_acquire)) +
                ", pause requested " +
                std::to_string(capture->pause_requested.load(std::memory_order_acquire)));
    auto second_reader = take(persistence::ProjectStore::open_reader(root));
    const auto second_generation = take(second_reader.load_current());
    const auto& second_plugin = std::get<project::Vst3PluginInstance>(
        second_generation.snapshot.tracks.front().instrument_source);
    const auto second_cutoff = std::ranges::find(second_plugin.parameters, cutoff_id,
                                                 &project::Vst3ParameterReference::id);
    require(second_plugin.parameters.size() == 2 && second_cutoff != second_plugin.parameters.end(),
            "second gesture edited another parameter instead of Filter 1 Cutoff");
    require(second_cutoff->normalized_value > 0.8,
            "second native cutoff drag did not raise the same parameter");
    std::cout << "second_cutoff=" << second_cutoff->normalized_value << '\n';
    const auto third_held_start = capture->read();
    QTest::qWait(1'600);
    const auto third_held_level = average_level(third_held_start, capture->read());
    const auto third_held_image = bridge.vst3_editor()
                                      ->native_window()
                                      ->screen()
                                      ->grabWindow(bridge.vst3_editor()->native_window()->winId())
                                      .toImage();
    require(!third_held_image.isNull() && third_held_image.size() == before_image.size(),
            "third held editor image could not be captured");
    std::size_t third_changed_cutoff_pixels = 0;
    for (int y = 210; y < 255; ++y)
      for (int x = 330; x < 455; ++x)
        third_changed_cutoff_pixels += before_image.pixel(x, y) != third_held_image.pixel(x, y);
    std::cout << "audio_third_while_held=" << third_held_level << '\n';
    std::cout << "third_changed_cutoff_pixels=" << third_changed_cutoff_pixels << '\n';
    require(third_changed_cutoff_pixels > 200,
            "third native cutoff control did not visibly move during held drag");
    require(third_held_level < before_level * 0.7,
            "queued commit cleared the newer held native preview");
    require(bridge.revision() == initial_revision + 2,
            "third performEdit committed before its endEdit");
    input.release();
    require(QTest::qWaitFor([&] { return bridge.revision() == initial_revision + 3; }, 8'000),
            "third endEdit did not commit exactly one saved revision");
    require(
        QTest::qWaitFor(
            [&] {
              return bridge.vst3_editor()
                  ->editor()
                  .value(QStringLiteral("liveStatus"))
                  .toString()
                  .startsWith(
                      QStringLiteral("Revision %1 live from sample ").arg(initial_revision + 3));
            },
            8'000),
        "audio callback did not acknowledge the last editor gesture");
    require(bridge.playback_active(), "completed native gesture stopped playback");
    auto reader = take(persistence::ProjectStore::open_reader(root));
    auto committed = take(reader.load_current());
    const auto& committed_plugin =
        std::get<project::Vst3PluginInstance>(committed.snapshot.tracks.front().instrument_source);
    require(committed.snapshot.revision == initial_revision + 3 &&
                !committed_plugin.component_state.empty(),
            "callback-applied revision did not persist the editor component state");
    committed_pair = committed_plugin;
    bridge.vst3_editor()->apply_vst3_insert_editor();
    require(bridge.revision() == initial_revision + 3,
            "closing the editor created a duplicate parameter revision");
    bridge.toggle_playback();
    require(QTest::qWaitFor([&] { return !bridge.playback_active(); }, 10'000),
            "fake routed playback did not stop");
  }
  auto reopened = take(app::ProjectSession::open(root));
  const auto& saved =
      std::get<project::Vst3PluginInstance>(reopened->snapshot().tracks.front().instrument_source);
  const auto cutoff =
      std::ranges::find(saved.parameters, cutoff_id, &project::Vst3ParameterReference::id);
  require(committed_pair && saved == *committed_pair && saved.class_id == initial_plugin.class_id &&
              saved.binary_sha256 == initial_plugin.binary_sha256 && saved.parameters.size() == 2 &&
              cutoff != saved.parameters.end() && cutoff->normalized_value != 0.95 &&
              reopened->snapshot().revision == initial_revision + 3,
          "saved native cutoff gesture or exact plugin identity failed to reopen");
  std::cout << "PASS native_editor_drag_commit_receipt_reopen revision="
            << reopened->snapshot().revision << " cutoff=" << cutoff->normalized_value << '\n';
}
} // namespace

int main(int argc, char** argv) {
  QApplication application(argc, argv);
  if (argc != 4 || (std::string_view(argv[3]) != "64" && std::string_view(argv[3]) != "128")) {
    std::cerr << "usage: oscillate_quick_surge_native_live_smoke BUNDLE BUILD_SCRATCH 64|128\n";
    return 64;
  }
  try {
    run(argv[1], argv[2], std::string_view(argv[3]) == "64" ? 64 : 128);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
