#include "../../../apps/daw_quick/src/clay_controller.hpp"
#include "oscillate/test/check.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QString>
#include <QTest>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <vector>

// ADR-0081 phase 2: Clay's Notes tab (docs/ui/clay-mockup.html) on the real
// controller. A sung take is detected from the empty state; a Pitch drag on
// the canvas is one undo step; the Correct pitch slider previews and commits
// once on release; To MIDI adds a Nova track; the history lists the edits
// newest first. CLAY_SCREENSHOT_DIR writes the melodic and polyphonic views.
namespace {
namespace application = oscillate::application;
namespace quick = oscillate::quick;
namespace fs = std::filesystem;

constexpr std::uint32_t rate = 48'000;
constexpr double two_pi = 2.0 * std::numbers::pi;
constexpr std::array<double, 4> sung_hz{220.0, 261.63, 329.63, 293.66};
constexpr std::array<double, 4> chord_hz{220.0, 261.63, 329.63, 392.0};
constexpr double note_seconds = 0.45;
constexpr double gap_seconds = 0.12;
constexpr double chord_seconds = 1.6;
constexpr int harmonics = 8;
constexpr int wait_ms = 60'000;
constexpr std::int16_t full_scale = 32'767;
constexpr int window_width = 1440;
constexpr int window_height = 560;
constexpr int drag_steps = 8;
constexpr double rows_dragged = 2.3;
// A drag lands within this many pixels of where it was aimed.
constexpr double pixel_cents = 1.5;
double row_cents_per_pixel(double row_pixels) { return 100.0 / row_pixels; }

using oscillate::test::require;

template <typename T> T take(oscillate::foundation::Result<T> result) {
  if (!result)
    throw std::runtime_error(result.error().message);
  return std::move(result).value();
}

class Directory {
public:
  Directory() {
    std::string pattern = (fs::temp_directory_path() / "oscillate-clay-editor-XXXXXX").string();
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

void put16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value));
  bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void put32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
  put16(bytes, static_cast<std::uint16_t>(value));
  put16(bytes, static_cast<std::uint16_t>(value >> 16U));
}

// A stereo 16-bit WAV of `seconds` whose samples come from `sample(t)`.
fs::path write_wav(const fs::path& path, double seconds,
                   const std::function<double(double)>& sample) {
  const auto frames = static_cast<std::uint32_t>(seconds * rate);
  std::vector<std::uint8_t> bytes{'R', 'I', 'F', 'F'};
  put32(bytes, 36 + frames * 4);
  bytes.insert(bytes.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  put32(bytes, 16);
  put16(bytes, 1);
  put16(bytes, 2);
  put32(bytes, rate);
  put32(bytes, rate * 4);
  put16(bytes, 4);
  put16(bytes, 16);
  bytes.insert(bytes.end(), {'d', 'a', 't', 'a'});
  put32(bytes, frames * 4);
  for (std::uint32_t i = 0; i < frames; ++i) {
    const double x = sample(static_cast<double>(i) / rate);
    for (int channel = 0; channel < 2; ++channel)
      put16(bytes, static_cast<std::uint16_t>(static_cast<std::int16_t>(
                       std::lround(std::clamp(x, -1.0, 1.0) * full_scale))));
  }
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  require(output.good(), "WAV fixture write failed");
  return path;
}

double harmonic_tone(double hz, double t) {
  double x = 0.0;
  for (int h = 1; h <= harmonics; ++h)
    x += std::sin(two_pi * hz * h * t) / h;
  return x;
}

// Four sung notes with a gentle vibrato, gaps between them.
fs::path write_sung(const fs::path& path) {
  const double slot = note_seconds + gap_seconds;
  const double length = static_cast<double>(sung_hz.size()) * slot + gap_seconds;
  return write_wav(path, length, [slot](double t) {
    const double from = t - gap_seconds;
    if (from < 0.0)
      return 0.0;
    const auto index = static_cast<std::size_t>(from / slot);
    const double within = from - static_cast<double>(index) * slot;
    if (index >= sung_hz.size() || within >= note_seconds)
      return 0.0;
    const double edge = std::min({1.0, within / 0.02, (note_seconds - within) / 0.02});
    // A 5.5 Hz vibrato of ±10 cents, integrated so its depth stays put.
    constexpr double depth = 0.006;
    constexpr double vibrato_hz = 5.5;
    const double cycles =
        sung_hz[index] *
        (within + depth * (1.0 - std::cos(two_pi * vibrato_hz * within)) / (two_pi * vibrato_hz));
    double x = 0.0;
    for (int h = 1; h <= harmonics; ++h)
      x += std::sin(two_pi * cycles * h) / h;
    return 0.25 * edge * x;
  });
}

// An Am7 stab, the four notes struck together.
fs::path write_chord(const fs::path& path) {
  return write_wav(path, chord_seconds + gap_seconds, [](double t) {
    if (t >= chord_seconds)
      return 0.0;
    const double edge = std::min({1.0, t / 0.01, (chord_seconds - t) / 0.05});
    double x = 0.0;
    for (const double hz : chord_hz)
      x += harmonic_tone(hz, t);
    return 0.09 * edge * std::exp(-0.8 * t) * x;
  });
}

QQuickItem* find_named(QQuickItem* parent, const QString& name) {
  if (parent->objectName() == name)
    return parent;
  for (auto* child : parent->childItems()) {
    if (auto* found = find_named(child, name))
      return found;
  }
  return nullptr;
}

QQuickItem* named(QQuickItem* view, const QString& name) {
  auto* item = find_named(view, name);
  require(item != nullptr, ("missing " + name).toStdString());
  return item;
}

void settle() {
  for (int i = 0; i < 5; ++i) {
    QCoreApplication::processEvents();
    QTest::qWait(10);
  }
}

void click(QQuickWindow& window, QQuickItem* item) {
  require(item->isVisible(), ("not visible: " + item->objectName()).toStdString());
  const auto centre = item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
  QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, centre);
  settle();
}

double call(QQuickItem* item, const char* method, double value) {
  QVariant result;
  require(QMetaObject::invokeMethod(item, method, Q_RETURN_ARG(QVariant, result),
                                    Q_ARG(QVariant, value)),
          std::string("could not call ") + method);
  return result.toDouble();
}

void screenshot(QQuickWindow& window, const char* name) {
  const QByteArray directory = qgetenv("CLAY_SCREENSHOT_DIR");
  if (directory.isEmpty())
    return;
  settle();
  const QString path = QDir(QString::fromUtf8(directory)).filePath(QString::fromLatin1(name));
  require(window.grabWindow().save(path), "screenshot write failed");
  std::cout << "wrote " << path.toStdString() << '\n';
}

QString clip_of(const application::ProjectSession& session, std::size_t track) {
  return QString::fromStdString(session.snapshot().tracks.at(track).clips.front().id.to_string());
}

void the_notes_tab_edits_a_take() {
  Directory directory;
  auto session = take(application::ProjectSession::create(directory.path() / "song"));
  for (const auto& wav : {write_sung(directory.path() / "ohyeah_vox.wav"),
                          write_chord(directory.path() / "dub_stab_Am7.wav")}) {
    const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
    require(fd >= 0, "fixture open failed");
    (void)take(session->imports().import_wav_audio(fd));
    (void)::close(fd);
  }
  require(static_cast<bool>(session->saving().save()), "the project saves");
  quick::ClayController controller(
      [&] { return session.get(); }, [] { return true; }, [](const QString&) {}, [&] {},
      [](const QString&) { return std::optional<oscillate::persistence::GrooveTemplate>{}; },
      [&] { (void)session->history().undo(); }, [&] { (void)session->history().redo(); });
  // In the app the bridge reports every new revision.
  const auto changed = [&] {
    controller.project_changed();
    settle();
  };

  QQmlEngine engine;
  QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/ClayEditor.qml")));
  std::unique_ptr<QObject> object(component.createWithInitialProperties(
      {{QStringLiteral("clay"), QVariant::fromValue(static_cast<QObject*>(&controller))}}));
  require(object != nullptr,
          ("the Notes tab did not load: " + component.errorString()).toStdString());
  auto* view = qobject_cast<QQuickItem*>(object.get());
  QQuickWindow window;
  window.resize(window_width, window_height);
  view->setParentItem(window.contentItem());
  view->setSize(QSizeF(window_width, window_height));
  window.show();
  require(QTest::qWaitForWindowExposed(&window), "window exposed");

  // The empty state, then detection.
  view->setProperty("clipId", clip_of(*session, 0));
  settle();
  require(controller.is_open(), "the tab opens the clip");
  require(named(view, QStringLiteral("clayEmpty"))->isVisible(),
          "a clip without a take offers detection");
  const auto steps = session->history().undo_steps().size();
  click(window, named(view, QStringLiteral("clayDetect:melodic")));
  require(controller.wait_for_detect(wait_ms), "detection finishes");
  changed();
  require(controller.has_take() && controller.notes().size() == static_cast<int>(sung_hz.size()),
          "the four sung notes are found: " + std::to_string(controller.notes().size()));
  require(!named(view, QStringLiteral("clayEmpty"))->isVisible(), "the empty state goes");
  require(session->history().undo_steps().size() == steps + 1, "detecting is one undo step");

  // A Pitch drag on the second note, unsnapped: up a little over two
  // semitones, one step.
  click(window, named(view, QStringLiteral("clayTool:pitch")));
  require(view->property("tool").toString() == QStringLiteral("pitch"), "the Pitch tool is chosen");
  click(window, named(view, QStringLiteral("claySnap:off")));
  require(controller.property("snap").toString() == QStringLiteral("off"), "snap is off");
  auto* canvas = named(view, QStringLiteral("clayCanvas"));
  const auto second = controller.notes().at(1).toMap();
  const double before = second.value(QStringLiteral("centre")).toDouble();
  const double t = 0.5 * (second.value(QStringLiteral("start")).toDouble() +
                          second.value(QStringLiteral("end")).toDouble());
  const QPointF at(canvas->property("plotX").toDouble() + call(canvas, "xOf", t),
                   canvas->property("plotY").toDouble() + call(canvas, "yOf", before));
  const double row = canvas->property("rowHeight").toDouble();
  const QPoint from = canvas->mapToScene(at).toPoint();
  const QPoint to = from - QPoint(0, static_cast<int>(std::lround(rows_dragged * row)));
  QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, from);
  for (int step = 1; step <= drag_steps; ++step)
    QTest::mouseMove(&window, from + (to - from) * step / drag_steps);
  require(controller.previewing(), "the drag previews");
  QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, to);
  changed();
  require(session->history().undo_steps().size() == steps + 2, "the drag is one undo step");
  const double after = controller.notes().at(1).toMap().value(QStringLiteral("centre")).toDouble();
  require(std::abs(after - before - rows_dragged * 100.0) < pixel_cents * row_cents_per_pixel(row),
          "the note moved up by the drag: " + std::to_string(before) + " -> " +
              std::to_string(after));
  screenshot(window, "clay_melodic_selected.png");

  // Correct pitch: dragging the slider previews; letting go is one step.
  click(window, named(view, QStringLiteral("clayTool:main")));
  auto* correct = named(view, QStringLiteral("clayCorrectCentre"));
  const QPoint left =
      correct->mapToScene(QPointF(correct->width() * 0.35, correct->height() / 2)).toPoint();
  const QPoint right =
      correct->mapToScene(QPointF(correct->width() * 0.8, correct->height() / 2)).toPoint();
  QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, left);
  QTest::mouseMove(&window, (left + right) / 2);
  QTest::mouseMove(&window, right);
  require(controller.previewing(), "the macro previews while dragging");
  QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, right);
  changed();
  require(session->history().undo_steps().size() == steps + 3,
          "the macro is one undo step on release");

  // To MIDI on a new Nova track.
  const auto tracks = session->snapshot().tracks.size();
  click(window, named(view, QStringLiteral("clayToMidi")));
  changed();
  require(session->snapshot().tracks.size() == tracks + 1, "To MIDI adds a Nova track");

  // The history, newest first.
  const auto label = named(view, QStringLiteral("clayHistory:0"))
                         ->childItems()
                         .front()
                         ->property("text")
                         .toString();
  require(label == QStringLiteral("To MIDI"),
          "the newest edit heads the history: " + label.toStdString());
  require(find_named(view, QStringLiteral("clayHistory:2")) != nullptr, "earlier edits follow");
  controller.clear_selection();
  view->setProperty("showOriginal", true);
  screenshot(window, "clay_melodic.png");

  // The chord stab, detected as polyphonic.
  view->setProperty("showOriginal", false);
  view->setProperty("clipId", clip_of(*session, 1));
  settle();
  require(named(view, QStringLiteral("clayEmpty"))->isVisible(), "the stab has no take yet");
  click(window, named(view, QStringLiteral("clayDetect:polyphonic")));
  require(controller.wait_for_detect(wait_ms), "polyphonic detection finishes");
  changed();
  require(controller.mode() == QStringLiteral("polyphonic") && controller.notes().size() >= 3,
          "the stab's notes stack: " + std::to_string(controller.notes().size()));
  // With no song key the take starts chromatic; in A minor it can flip.
  controller.set_key(9, true);
  changed();
  require(named(view, QStringLiteral("clayFlipQuality"))->isVisible(),
          "a polyphonic take offers Minor ↔ Major");
  screenshot(window, "clay_polyphonic.png");
}

} // namespace

int main(int argc, char** argv) {
  QStandardPaths::setTestModeEnabled(true);
  QGuiApplication app(argc, argv);
  try {
    the_notes_tab_edits_a_take();
  } catch (const std::exception& error) {
    std::cerr << "clay editor: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "clay editor contract passed\n";
  return EXIT_SUCCESS;
}
