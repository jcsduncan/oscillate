#include "oscillate/test/check.hpp"

#include <QByteArray>
#include <QGuiApplication>
#include <QMetaObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QString>
#include <QStringList>
#include <QTest>
#include <QUrl>
#include <QVariant>

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

// The Mix Check view (docs/ui/mix-check-mockup.html) against a fake
// controller shaped like projectBridge.mixCheck and filled with the four
// findings measured on Funky: the empty state and its Run, the finding
// cards, Apply, Listen and Stop, a band marker that highlights its card, the
// loudness target, and a controller with nothing in it rendering cleanly.
namespace {

using oscillate::test::require;

QStringList qml_warnings;
void collect_warnings(QtMsgType type, const QMessageLogContext&, const QString& message) {
  if (type != QtDebugMsg && type != QtInfoMsg && message.contains(QStringLiteral("qrc:/")))
    qml_warnings.push_back(message);
  std::cerr << message.toStdString() << '\n';
}

// The fake controller: every property of the contract, and a log of calls.
constexpr const char* fake_controller = R"QML(
import QtQuick
QtObject {
    id: fake
    property bool running: false
    property real progress: 0
    property string stage: ""
    property bool stale: false
    property string status: "Funky · 14 tracks · 3:12 · checked 41 s ago"
    property bool hasResult: false
    property string checkedAt: "41 s ago"
    property var summary: ({ projectName: "Funky", trackCount: 14, durationSeconds: 192 })
    property var spectrum: ({})
    property var meters: [
        { id: "sub", label: "Sub 28–35 Hz", value: "+5.9 dB", note: "above the bass's lowest note", state: "bad" },
        { id: "mono", label: "Mono loss, mids", value: "3.6 dB", note: "lost when folded to mono", state: "bad" },
        { id: "ring", label: "Ringing peaks", value: "3", note: "narrow, ≥ 9 dB over neighbours", state: "bad" },
        { id: "dip", label: "Kick/bass dip", value: "11.3 dB", note: "deepest cancellation", state: "warn" },
        { id: "level", label: "Level", value: "−17.5", note: "dB RMS · no clipping", state: "ok" }
    ]
    property string target: "club"
    property var targets: [
        { id: "club", name: "Club master", note: "Loud and dense.", lufsLow: -9, lufsHigh: -7, truePeak: -0.3,
          lraLow: 4, lraHigh: 8, crestLow: 8, crestHigh: 12 },
        { id: "stream", name: "Streaming", note: "Services turn loud masters down.", lufsLow: -15, lufsHigh: -13,
          truePeak: -1.0, lraLow: 5, lraHigh: 10, crestLow: 9, crestHigh: 14 },
        { id: "dj", name: "Into a DJ mix", note: "Leaves headroom for the mixer.", lufsLow: -12, lufsHigh: -9,
          truePeak: -2.0, lraLow: 4, lraHigh: 9, crestLow: 9, crestHigh: 14 }
    ]
    property var loudness: ({ integrated: -8.6, truePeak: 0.4, range: 5.8, crest: 9.4 })
    property var energy: []
    property var sections: [
        { name: "Intro", firstBar: 0, lastBar: 16, lufs: -13 },
        { name: "Build", firstBar: 16, lastBar: 32, lufs: -11 },
        { name: "Drop 1", firstBar: 32, lastBar: 64, lufs: -8.2 },
        { name: "Break", firstBar: 64, lastBar: 80, lufs: -12.5 },
        { name: "Drop 2", firstBar: 80, lastBar: 112, lufs: -9.0 },
        { name: "Outro", firstBar: 112, lastBar: 128, lufs: -12.8 }
    ]
    property string energyNote: "The second drop is 0.8 LU quieter than the first."
    property var findings: []
    property var fine: ["No track or the master clips.", "Left/right balance sits within 0.3 dB."]
    property var sinceLast: [
        { direction: "fixed", text: "Hat ringing fixed: peaks down 11 dB" },
        { direction: "worse", text: "True peak rose 0.6 dB" },
        { direction: "same", text: "Everything else within 0.1 dB" }
    ]
    property string lastCheckedAt: "yesterday 22:41"
    property var reference: [{ band: "Sub", diffDb: -1.8 }, { band: "Low", diffDb: 0.6 }, { band: "Low-mid", diffDb: 2.4 }]
    property string referenceName: "Reference.flac"
    property string log: ""

    function run() { fake.log += "run;" }
    function cancel() { fake.log += "cancel;" }
    function apply(id) { fake.log += "apply:" + id + ";" }
    function listen(id) { fake.log += "listen:" + id + ";" }
    function stopListening() { fake.log += "stop;" }
    function setReference(path) { fake.log += "reference:" + path + ";" }
    function clearReference() { fake.log += "clearReference;" }
    function chooseReference() { fake.log += "chooseReference;" }
    function markApplied(id) {
        fake.findings = fake.findings.map(f => f.id === id ? Object.assign({}, f, { applied: true, result: "28–35 Hz down 5.9 dB" }) : f)
    }

    function series(from, to, count, logScale, shape) {
        const x = []
        const y = []
        for (let i = 0; i < count; ++i) {
            const t = i / (count - 1)
            const at = logScale ? from * Math.pow(to / from, t) : from + (to - from) * t
            x.push(at)
            y.push(shape(at, t))
        }
        return { x: x, y: y }
    }
    Component.onCompleted: {
        const mix = series(20, 20000, 200, true, f => -18 - 4.2 * (Math.log10(f) - 2)
                           + 7 * Math.exp(-Math.pow((Math.log10(f) - 1.5) / 0.06, 2)))
        spectrum = { freqs: mix.x, before: mix.y, after: [], mono: mix.y.map(v => v - 1.5) }
        const bars = []
        for (let bar = 0; bar <= 128; ++bar)
            bars.push({ bar: bar, lufs: bar >= 32 && bar < 64 ? -8.2 : bar >= 80 && bar < 112 ? -9 : -12.5 })
        energy = bars
        const sub = series(20, 80, 60, true, f => Math.exp(-Math.pow((Math.log10(f) - 1.5) / 0.06, 2)))
        const ring = series(8000, 12500, 90, false, f => [9680, 10320, 11010].reduce(
            (s, c) => s + Math.exp(-Math.pow((f - c) / 70, 2)), 0.25))
        const mono = series(100, 5000, 60, true, (f, t) => 3.6 * Math.exp(-Math.pow((t - 0.5) / 0.22, 2)))
        const dip = series(0, 1, 120, false, t => Math.sin(t * 64) * (1 - 0.55 * Math.exp(-Math.pow(((t * 4) % 1 - 0.18) / 0.06, 2))))
        findings = [
            { id: "ring", kind: "resonance", severity: "bad", title: "The hat rings at three pitches",
              text: "Three narrow peaks at <b>9.68, 10.32 and 11.01 kHz</b> stand about <b>9 dB</b> over the hat.",
              tracks: [{ id: "t-hat", name: "Hat (Knock)", color: "#c9a6ff" }],
              band: { low: 9500, high: 11200, label: "9.6–11.1 kHz" },
              evidence: { kind: "spectrum", x: ring.x, before: ring.y, after: ring.y.map(v => v * 0.2),
                          markers: [9680, 10320, 11010] },
              fix: { device: "Temper", where: "last on Hat",
                     params: [["Bands", "3 dynamic cuts"], ["Freqs", "9680 · 10320 · 11010"], ["Q", "8"], ["Range", "−9 dB"]] },
              expected: "Peaks down 10–12 dB · rest of hat ±0.1 dB", applied: false, result: "" },
            { id: "sub", kind: "rumble", severity: "bad", title: "Rumble under the bass",
              text: "There is energy at <b>28–35 Hz</b>, below the bass's lowest real note.",
              tracks: [{ id: "t-bass", name: "Bass (Nova)", color: "#6aa7ff" }],
              band: { low: 28, high: 35, label: "28–35 Hz" },
              evidence: { kind: "spectrum", x: sub.x, before: sub.y, after: sub.y.map(v => v * 0.25),
                          markers: [{ x: 32.7, label: "C1" }] },
              fix: { device: "Contour", where: "first on Bass",
                     params: [["Mode", "High-pass 24 dB/oct"], ["Cutoff", "38 Hz"], ["Mix", "100 %"]] },
              expected: "28–35 Hz down 5.9 dB · fundamental kept", applied: false, result: "" },
            { id: "mono", kind: "mono", severity: "bad", title: "The low mids disappear in mono",
              text: "Folded to mono, <b>137 Hz – 2.5 kHz</b> loses <b>3.6 dB</b>.",
              tracks: [{ id: "t-bass", name: "Bass (Nova)", color: "#6aa7ff" }],
              band: { low: 137, high: 2500, label: "137 Hz–2.5 kHz" },
              evidence: { kind: "mono", x: mono.x, before: mono.y, after: [], markers: [] },
              fix: { device: "Vista", where: "your existing one on Bass",
                     params: [["Band 2 width", "1.80 → 0.55"], ["Net width", "≈ 1.0"]] },
              expected: "Mono loss 3.6 → 2.1 dB", applied: false, result: "" },
            { id: "dip", kind: "cancellation", severity: "warn", title: "Kick and bass cancel under the hits",
              text: "Where the kick's tail and the bass overlap they cancel by up to <b>11.3 dB</b>.",
              tracks: [{ id: "t-kick", name: "Kick (Anvil)", color: "#f08a5d" },
                       { id: "t-bass", name: "Bass (Nova)", color: "#6aa7ff" }],
              band: { low: 40, high: 120, label: "40–120 Hz" },
              evidence: { kind: "time", x: dip.x, before: dip.y, after: [], markers: [0.25, 0.5, 0.75] },
              fix: { device: "Lock", where: "Bass + kicks",
                     params: [["Bass duck", "under each kick"], ["Mono lows", "below 120 Hz"]] },
              expected: "Deepest dip 11.3 → 4.3 dB", applied: false, result: "" }
        ]
    }
}
)QML";

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

void activate(QQuickItem* view, const QString& name) {
  require(QMetaObject::invokeMethod(named(view, name), "activate"),
          ("could not activate " + name).toStdString());
  QCoreApplication::processEvents();
}

QString text_of(QQuickItem* view, const QString& name, const char* property = "text") {
  return named(view, name)->property(property).toString();
}

std::unique_ptr<QObject> make_controller(QQmlEngine& engine, const QByteArray& source) {
  QQmlComponent component(&engine);
  component.setData(source, QUrl::fromLocalFile(QStringLiteral("FakeMixCheck.qml")));
  std::unique_ptr<QObject> controller(component.create());
  require(controller != nullptr,
          ("fake controller did not load: " + component.errorString()).toStdString());
  return controller;
}

struct ViewHost {
  QQuickWindow window;
  std::unique_ptr<QObject> object;
  QQuickItem* view = nullptr;

  ViewHost(QQmlEngine& engine, QObject* controller) {
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/MixCheckView.qml")));
    object.reset(component.createWithInitialProperties(
        {{QStringLiteral("controller"), QVariant::fromValue(controller)}}));
    require(object != nullptr,
            ("mix check view did not load: " + component.errorString()).toStdString());
    view = qobject_cast<QQuickItem*>(object.get());
    require(view != nullptr, "mix check view is not an item");
    window.resize(1400, 900);
    view->setParentItem(window.contentItem());
    view->setSize(QSizeF(1400, 900));
    window.show();
    require(QTest::qWaitForWindowExposed(&window), "mix check window exposed");
    QCoreApplication::processEvents();
  }
};

void findings_apply_listen_and_highlight() {
  QQmlEngine engine;
  auto controller = make_controller(engine, QByteArray(fake_controller));
  ViewHost host(engine, controller.get());
  auto* view = host.view;
  const auto log = [&] { return controller->property("log").toString(); };

  // Before the first run: the explanation and one big Run.
  require(named(view, QStringLiteral("mixCheckEmpty"))->isVisible() &&
              !named(view, QStringLiteral("mixCheckContent"))->isVisible(),
          "the empty state shows before a result");
  activate(view, QStringLiteral("mixCheckEmptyRunButton"));
  require(log() == QStringLiteral("run;"), "the empty state's Run runs the check");
  controller->setProperty("running", true);
  QCoreApplication::processEvents();
  require(text_of(view, QStringLiteral("mixCheckEmptyRunButton"), "label") ==
              QStringLiteral("Cancel"),
          "Run turns into Cancel while the check runs");
  activate(view, QStringLiteral("mixCheckEmptyRunButton"));
  require(log().endsWith(QStringLiteral("cancel;")), "Cancel cancels the check");
  controller->setProperty("running", false);

  // A result: the cards, one per finding.
  controller->setProperty("hasResult", true);
  QCoreApplication::processEvents();
  require(!named(view, QStringLiteral("mixCheckEmpty"))->isVisible() &&
              named(view, QStringLiteral("mixCheckContent"))->isVisible(),
          "the result replaces the empty state");
  for (const auto* id : {"ring", "sub", "mono", "dip"}) {
    auto* card = named(view, QStringLiteral("mixCheckFinding:") + QString::fromLatin1(id));
    require(card->isVisible() && card->height() > 80, "a finding card renders");
  }
  require(text_of(view, QStringLiteral("mixCheckFindings"))
              .contains(QStringLiteral("4 TO LOOK AT · 0 FIXED")),
          "the heading counts the open findings");
  activate(view, QStringLiteral("mixCheckRunButton"));
  require(log().endsWith(QStringLiteral("run;")), "the bar's Run runs the check again");

  // Apply: the controller applies; once applied the card says so.
  activate(view, QStringLiteral("mixCheckApply:sub"));
  require(log().endsWith(QStringLiteral("apply:sub;")), "Apply calls apply(id)");
  require(QMetaObject::invokeMethod(controller.get(), "markApplied",
                                    Q_ARG(QVariant, QVariant(QStringLiteral("sub")))),
          "fake marks the finding applied");
  QCoreApplication::processEvents();
  require(text_of(view, QStringLiteral("mixCheckApply:sub"), "label") ==
              QStringLiteral("✓ Applied"),
          "an applied finding shows Applied");
  require(text_of(view, QStringLiteral("mixCheckOutcome:sub"))
              .startsWith(QStringLiteral("Re-checked:")),
          "an applied finding shows its re-checked result");
  require(text_of(view, QStringLiteral("mixCheckFindings"))
              .contains(QStringLiteral("3 TO LOOK AT · 1 FIXED")),
          "the heading counts the fixed finding");
  activate(view, QStringLiteral("mixCheckApply:sub"));
  require(!log().endsWith(QStringLiteral("apply:sub;apply:sub;")),
          "an applied fix is not applied twice");

  // Listen toggles to Stop; another finding's Listen moves the audition.
  activate(view, QStringLiteral("mixCheckListen:ring"));
  require(log().endsWith(QStringLiteral("listen:ring;")) &&
              text_of(view, QStringLiteral("mixCheckListen:ring"), "label") ==
                  QStringLiteral("■ Stop"),
          "Listen auditions the band and turns into Stop");
  activate(view, QStringLiteral("mixCheckListen:mono"));
  require(
      text_of(view, QStringLiteral("mixCheckListen:ring"), "label") == QStringLiteral("▶ Listen") &&
          text_of(view, QStringLiteral("mixCheckListen:mono"), "label") == QStringLiteral("■ Stop"),
      "one finding is auditioned at a time");
  activate(view, QStringLiteral("mixCheckListen:mono"));
  require(log().endsWith(QStringLiteral("stop;")) &&
              text_of(view, QStringLiteral("mixCheckListen:mono"), "label") ==
                  QStringLiteral("▶ Listen"),
          "Stop stops listening");

  // A band marker on the spectrum highlights and scrolls to its card.
  auto* spectrum = named(view, QStringLiteral("mixCheckSpectrum"));
  QVariant x;
  require(QMetaObject::invokeMethod(spectrum, "xOfFrequency", Q_RETURN_ARG(QVariant, x),
                                    Q_ARG(QVariant, QVariant(10300.0))),
          "spectrum maps a frequency");
  QVariant hit;
  require(QMetaObject::invokeMethod(spectrum, "markerAt", Q_RETURN_ARG(QVariant, hit),
                                    Q_ARG(QVariant, x)) &&
              hit.toString() == QStringLiteral("ring"),
          "the hat's band is under 10.3 kHz");
  auto* scroller = named(view, QStringLiteral("mixCheckContent"));
  const auto scrolled_before = scroller->property("contentY").toDouble();
  QTest::mouseClick(&host.window, Qt::LeftButton, Qt::NoModifier,
                    spectrum->mapToScene(QPointF(x.toDouble(), 16)).toPoint());
  QCoreApplication::processEvents();
  require(named(view, QStringLiteral("mixCheckFinding:ring"))->property("highlighted").toBool() &&
              !named(view, QStringLiteral("mixCheckFinding:sub"))->property("highlighted").toBool(),
          "clicking a band marker highlights its finding");
  require(scroller->property("contentY").toDouble() > scrolled_before,
          "clicking a band marker scrolls to its finding");

  // The loudness target writes through to the controller.
  require(text_of(view, QStringLiteral("mixCheckGaugeValue:integrated")) ==
                  QStringLiteral("−8.6 LUFS") &&
              text_of(view, QStringLiteral("mixCheckGaugeValue:truePeak")) ==
                  QStringLiteral("+0.4 dBTP"),
          "the gauges show the song's loudness");
  activate(view, QStringLiteral("mixCheckTarget:stream"));
  require(controller->property("target").toString() == QStringLiteral("stream"),
          "the target switch writes the controller's target");
  require(
      text_of(view, QStringLiteral("mixCheckLoudnessNote")).contains(QStringLiteral("1.4 dB over")),
      "the note flags a true peak over the target's ceiling");

  // The spectrum's mono fold, the energy, since last and the reference.
  activate(view, QStringLiteral("mixCheckSpectrumView:mono"));
  require(spectrum->property("monoView").toBool(), "the spectrum shows the mono fold");
  require(named(view, QStringLiteral("mixCheckEnergy"))->height() > 100,
          "the energy chart renders");
  require(named(view, QStringLiteral("mixCheckSinceLast"))->isVisible() &&
              find_named(view, QStringLiteral("mixCheckSinceLast:2")) != nullptr,
          "the since-last list renders each change");
  require(find_named(view, QStringLiteral("mixCheckReferenceBand:Low-mid")) != nullptr,
          "the reference bars render per band");
  activate(view, QStringLiteral("mixCheckReferenceClear"));
  require(log().endsWith(QStringLiteral("clearReference;")), "Clear clears the reference");
}

void empty_controllers_render_cleanly() {
  QQmlEngine engine;
  // Nothing at all, then a result with every other property missing.
  auto bare = make_controller(engine, QByteArrayLiteral("import QtQuick\nQtObject {}\n"));
  {
    ViewHost host(engine, bare.get());
    require(named(host.view, QStringLiteral("mixCheckEmpty"))->isVisible(),
            "an empty controller shows the empty state");
  }
  auto hollow = make_controller(
      engine, QByteArrayLiteral("import QtQuick\nQtObject { property bool hasResult: true }\n"));
  {
    ViewHost host(engine, hollow.get());
    require(named(host.view, QStringLiteral("mixCheckContent"))->isVisible(),
            "a result with no data still lays out");
  }
  {
    ViewHost host(engine, nullptr);
    require(named(host.view, QStringLiteral("mixCheckEmpty"))->isVisible(),
            "no controller shows the empty state");
  }
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication application(argc, argv);
  // The shell's QML lives in the core library's resources; nothing else here
  // pulls them in.
  Q_INIT_RESOURCE(qml);
  qInstallMessageHandler(collect_warnings);
  try {
    findings_apply_listen_and_highlight();
    empty_controllers_render_cleanly();
    require(qml_warnings.isEmpty(), "the Mix Check view logged QML warnings");
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    for (const auto& warning : qml_warnings)
      std::cerr << "  " << warning.toStdString() << '\n';
    return 1;
  }
}
