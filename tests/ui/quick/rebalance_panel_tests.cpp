#include "meter_model.hpp"
#include "panel_fit_checks.hpp"
#include "project_bridge.hpp"
#include "rebalance_controller.hpp"

#include "oscillate/application/project_session.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

// The Rebalance panel (ADR-0123) on a real bridge: it loads with every
// role and track and its text fits; a fader drag saves the role's gain as
// one undo step; choosing a track's role saves it and moves the track; a
// role's meter combines the tracks its stage acts on.
namespace {
namespace application = oscillate::application;
namespace project = oscillate::project;
namespace quick = oscillate::quick;
namespace panel_fit = oscillate::test::panel_fit;

using oscillate::test::require;

constexpr int window_width = 1600;
constexpr int window_height = 860;
// One meter display step, and enough of them for the ballistics to settle.
constexpr double meter_step_seconds = 1.0 / 60.0;
constexpr int meter_steps = 4;
// Readings compare within float rounding.
constexpr double level_tolerance = 1e-5;
constexpr double gain_tolerance = 1e-9;
// The override menu's rows: Auto, then the roles in this order.
constexpr int auto_choice = 0;
constexpr int vocals_choice = 1;

// A realistic small song: kick and snare (built-in drum instruments) in a
// drum group, a bass, two vocal audio tracks, a pad, a track whose role is
// chosen, and a reverb return.
// A placeholder for the fixture's ids before the song is written.
constexpr std::uint64_t unset_serial = 1;
project::TrackId unset_track() { return project::TrackId::from_bits({0, unset_serial}).value(); }

struct Song {
  QTemporaryDir directory;
  std::filesystem::path root;
  std::unique_ptr<quick::ProjectBridge> bridge;
  quick::RebalanceController* controller = nullptr;
  // Set once the song is written; until then no track's.
  project::TrackId kick{unset_track()};
  project::TrackId snare{unset_track()};
  project::TrackId drums{unset_track()};
  project::TrackId bass{unset_track()};
  project::TrackId lead{unset_track()};
  project::TrackId double_vox{unset_track()};
  project::TrackId pad{unset_track()};
  project::TrackId chops{unset_track()};
};

template <typename T> T take(oscillate::foundation::Result<T> result, const char* what) {
  require(result.has_value(), what);
  return std::move(result).value();
}

void name(application::ProjectSession& session, project::TrackId track, std::string text) {
  require(session.tracks().set_track_name(track, std::move(text)).has_value(), "name a track");
}

std::unique_ptr<Song> make_song() {
  auto song = std::make_unique<Song>();
  require(song->directory.isValid(), "fixture directory");
  song->root = std::filesystem::path(song->directory.path().toStdString()) / "song";
  auto session = take(application::ProjectSession::create(song->root), "project");
  auto& s = *session;
  song->kick = take(s.instruments().add_native_instrument_track("anvil", std::nullopt), "kick");
  name(s, song->kick, "Kick");
  song->snare = take(s.instruments().add_native_instrument_track("knock", std::nullopt), "snare");
  name(s, song->snare, "Snare");
  song->drums = take(s.tracks().add_bus_track(project::TrackKind::group_bus), "drum group");
  name(s, song->drums, "Drums");
  require(s.routing().set_main_output(song->kick, song->drums).has_value() &&
              s.routing().set_main_output(song->snare, song->drums).has_value(),
          "drums into their group");
  song->bass = take(s.instruments().add_synth_track(), "bass");
  name(s, song->bass, "Sub Bass");
  song->lead = take(s.tracks().add_audio_track(), "lead vocal");
  name(s, song->lead, "Lead Vox");
  song->double_vox = take(s.tracks().add_audio_track(), "double");
  name(s, song->double_vox, "Vox Double");
  song->pad = take(s.instruments().add_synth_track(), "pad");
  name(s, song->pad, "Pad");
  song->chops = take(s.tracks().add_audio_track(), "chops");
  name(s, song->chops, "Chops");
  require(s.tracks().set_track_mix_role(song->chops, project::MixRole::vocals).has_value(),
          "a chosen role");
  const auto reverb = take(s.tracks().add_bus_track(project::TrackKind::return_bus), "reverb");
  name(s, reverb, "Reverb");
  song->bridge = std::make_unique<quick::ProjectBridge>(song->directory.path().toStdString());
  song->bridge->set_project(std::move(session), song->root);
  song->controller = qobject_cast<quick::RebalanceController*>(song->bridge->rebalance());
  require(song->controller != nullptr, "the bridge has the rebalance controller");
  return song;
}

QString text(project::TrackId id) { return QString::fromStdString(id.to_string()); }

QQuickItem* find_named(QQuickItem* parent, const QString& name) {
  if (parent->objectName() == name)
    return parent;
  for (auto* child : parent->childItems()) {
    if (auto* found = find_named(child, name))
      return found;
  }
  return nullptr;
}

QQuickItem* item(QQuickItem* root, const QString& name) {
  auto* found = find_named(root, name);
  require(found != nullptr, "panel has " + name.toStdString());
  return found;
}

quick::RebalanceRole* role(quick::RebalanceController& controller, const QString& id) {
  for (auto* object : controller.roles()) {
    auto* candidate = qobject_cast<quick::RebalanceRole*>(object);
    if (candidate != nullptr && candidate->id() == id)
      return candidate;
  }
  throw std::runtime_error("no role " + id.toStdString());
}

QVariantMap row(const quick::RebalanceController& controller, project::TrackId id) {
  for (const auto& entry : controller.tracks()) {
    if (entry.toMap().value(QStringLiteral("id")).toString() == text(id))
      return entry.toMap();
  }
  throw std::runtime_error("track missing from the role list");
}

// Fake engine readings by track, as the routed graph's post-fader meters.
using Readings = std::map<project::TrackId, quick::MeterReading>;

struct Panel {
  QQmlEngine engine;
  std::unique_ptr<quick::MeterModel> meters;
  std::unique_ptr<QObject> object;
  QQuickWindow window;
  QQuickItem* root = nullptr;
};

std::unique_ptr<Panel> load(Song& song, const Readings& readings) {
  auto panel = std::make_unique<Panel>();
  auto* controller = song.controller;
  panel->meters = std::make_unique<quick::MeterModel>(
      [controller, readings](const QString& id) -> std::optional<quick::MeterReading> {
        return controller->role_meter(id, [&readings](project::TrackId track) {
          const auto found = readings.find(track);
          return found == readings.end() ? std::nullopt : std::optional(found->second);
        });
      });
  QQmlComponent component(&panel->engine, QUrl(QStringLiteral("qrc:/RebalanceView.qml")));
  panel->object.reset(component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue(static_cast<QObject*>(controller))},
       {QStringLiteral("meters"),
        QVariant::fromValue(static_cast<QObject*>(panel->meters.get()))}}));
  require(panel->object != nullptr,
          ("rebalance view did not load: " + component.errorString()).toStdString());
  panel->root = qobject_cast<QQuickItem*>(panel->object.get());
  require(panel->root != nullptr, "the view is an item");
  panel->window.resize(window_width, window_height);
  panel->root->setParentItem(panel->window.contentItem());
  panel->root->setSize(QSizeF(window_width, window_height));
  panel->window.show();
  require(QTest::qWaitForWindowExposed(&panel->window), "window exposed");
  for (int step = 0; step < meter_steps; ++step)
    panel->meters->tick(meter_step_seconds);
  QCoreApplication::processEvents();
  return panel;
}

void click(Panel& panel, QQuickItem* target) {
  const auto centre = target->mapToScene(QPointF(target->width() / 2.0, target->height() / 2.0));
  QTest::mouseClick(&panel.window, Qt::LeftButton, {}, centre.toPoint());
  QCoreApplication::processEvents();
}

// ---- Cases ----------------------------------------------------------------

void the_panel_shows_every_role_and_track_and_fits() {
  auto song = make_song();
  auto panel = load(*song, {});
  for (const auto* id : {"vocals", "drums", "bass", "music"})
    item(panel->root, QStringLiteral("rebalanceRole:") + QString::fromLatin1(id));
  for (const auto& entry : song->controller->tracks())
    item(panel->root,
         QStringLiteral("rebalanceTrack:") + entry.toMap().value(QStringLiteral("id")).toString());
  // Why each track plays its role.
  require(row(*song->controller, song->kick).value(QStringLiteral("reason")) ==
              QStringLiteral("Anvil"),
          "a drum instrument names itself as the reason");
  require(row(*song->controller, song->lead).value(QStringLiteral("role")) ==
                  QStringLiteral("vocals") &&
              row(*song->controller, song->lead).value(QStringLiteral("reason")) ==
                  QStringLiteral("name"),
          "a vocal track's name says vocals");
  require(row(*song->controller, song->chops).value(QStringLiteral("chosen")).toBool(),
          "a chosen role shows as chosen");
  panel_fit::require_text_fits(panel->root, "rebalance");
}

void a_fader_drag_saves_the_role_gain_as_one_step() {
  auto song = make_song();
  auto panel = load(*song, {});
  auto* vocals = role(*song->controller, QStringLiteral("vocals"));
  require(vocals->value(QStringLiteral("gain")) == 0.0 && !song->controller->enabled(),
          "the rebalance starts flat and off");
  auto* fader = item(panel->root, QStringLiteral("rebalanceFader:vocals"));
  const double x = fader->width() / 2.0;
  // Press high on the travel (off the cap, so the cap jumps there), then drag up.
  constexpr double press_share = 0.3;
  constexpr double drag_share = 0.2;
  const auto press = fader->mapToScene(QPointF(x, fader->height() * press_share)).toPoint();
  const auto drag = fader->mapToScene(QPointF(x, fader->height() * drag_share)).toPoint();
  QTest::mousePress(&panel->window, Qt::LeftButton, {}, press);
  QTest::mouseMove(&panel->window, drag);
  QTest::mouseRelease(&panel->window, Qt::LeftButton, {}, drag);
  QCoreApplication::processEvents();
  const double moved = vocals->value(QStringLiteral("gain"));
  require(moved > 0.0, "dragging the vocal fader up raises the vocals' gain");
  require(song->controller->enabled(), "moving a role turns the rebalance on");
  require(item(panel->root, QStringLiteral("rebalanceGain:vocals"))->property("text").toString() ==
              vocals->display(QStringLiteral("gain")),
          "the readout shows the saved gain");
  song->bridge->undo();
  QCoreApplication::processEvents();
  require(std::abs(vocals->value(QStringLiteral("gain"))) < gain_tolerance &&
              !song->controller->enabled(),
          "one undo takes the whole drag back");
}

void choosing_a_role_saves_it_and_moves_the_track() {
  auto song = make_song();
  auto panel = load(*song, {});
  require(row(*song->controller, song->pad).value(QStringLiteral("role")) ==
              QStringLiteral("music"),
          "the pad plays music");
  click(*panel, item(panel->root, QStringLiteral("rebalanceRolePill:") + text(song->pad)));
  auto* menu = item(panel->root, QStringLiteral("shellMenu"));
  require(menu->isVisible(), "the role pill opens the menu");
  QMetaObject::invokeMethod(menu, "choose", Q_ARG(QVariant, vocals_choice));
  QCoreApplication::processEvents();
  const auto chosen = row(*song->controller, song->pad);
  require(chosen.value(QStringLiteral("role")) == QStringLiteral("vocals") &&
              chosen.value(QStringLiteral("chosen")).toBool() &&
              chosen.value(QStringLiteral("reason")) == QStringLiteral("chosen"),
          "the chosen role is saved and shown as chosen");
  require(role(*song->controller, QStringLiteral("vocals"))
                  ->track_names()
                  .contains(QStringLiteral("Pad")) &&
              !role(*song->controller, QStringLiteral("music"))
                   ->track_names()
                   .contains(QStringLiteral("Pad")),
          "the pad moves from the music card to the vocals card");
  // Auto lets the role be inferred again.
  click(*panel, item(panel->root, QStringLiteral("rebalanceRolePill:") + text(song->pad)));
  QMetaObject::invokeMethod(menu, "choose", Q_ARG(QVariant, auto_choice));
  QCoreApplication::processEvents();
  require(row(*song->controller, song->pad).value(QStringLiteral("role")) ==
                  QStringLiteral("music") &&
              !row(*song->controller, song->pad).value(QStringLiteral("chosen")).toBool(),
          "Auto returns the track to its inferred role");
}

void role_meters_combine_the_tracks_each_stage_acts_on() {
  auto song = make_song();
  // Each source track carries its role's stage and the role's meter combines
  // them: the kick and snare make the drums' (the drum group, which carries
  // no stage, is not counted again); the two vocal tracks the vocals'.
  const quick::MeterReading kick{0.9F, 0.9F, 0.5F, 0.5F, false};
  const quick::MeterReading snare{0.7F, 0.6F, 0.3F, 0.3F, false};
  const quick::MeterReading group{0.8F, 0.75F, 0.45F, 0.4F, false};
  const quick::MeterReading lead{0.5F, 0.4F, 0.3F, 0.2F, false};
  const quick::MeterReading backing{0.3F, 0.6F, 0.4F, 0.1F, true};
  const Readings readings{{song->kick, kick},
                          {song->snare, snare},
                          {song->drums, group},
                          {song->lead, lead},
                          {song->double_vox, backing}};
  auto panel = load(*song, readings);
  auto* drums = item(panel->root, QStringLiteral("rebalanceMeterL:drums"));
  require(drums->property("valid").toBool() &&
              std::abs(drums->property("peak").toDouble() - kick.left_peak) < level_tolerance,
          "the drums' meter reads its tracks, not the drum group again");
  auto* vocals_left = item(panel->root, QStringLiteral("rebalanceMeterL:vocals"));
  auto* vocals_right = item(panel->root, QStringLiteral("rebalanceMeterR:vocals"));
  require(std::abs(vocals_left->property("peak").toDouble() - lead.left_peak) < level_tolerance &&
              std::abs(vocals_right->property("peak").toDouble() - backing.right_peak) <
                  level_tolerance,
          "the vocals' peak is the louder vocal track's");
  require(vocals_left->property("clipped").toBool(), "a clipping vocal track lights the clip");
  require(!item(panel->root, QStringLiteral("rebalanceMeterL:bass"))->property("valid").toBool(),
          "a role whose tracks have no reading shows none");
  const auto combined = song->controller->role_meter(
      QStringLiteral("role:vocals"), [&readings](project::TrackId track) {
        const auto found = readings.find(track);
        return found == readings.end() ? std::nullopt : std::optional(found->second);
      });
  if (!combined)
    throw std::runtime_error("the vocals do not combine");
  const double power = static_cast<double>(lead.left_rms) * lead.left_rms +
                       static_cast<double>(backing.left_rms) * backing.left_rms;
  require(std::abs(combined->left_rms - std::sqrt(power)) < level_tolerance,
          "RMS adds the tracks' power");
}

// ---- Screenshot -----------------------------------------------------------

void screenshot_if_requested() {
  const char* directory = std::getenv("REBALANCE_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0')
    return;
  auto song = make_song();
  struct Move {
    const char* role;
    const char* parameter;
    double value;
  };
  // A mastering pass: the vocal up and present, the drums brighter, the
  // low end tidied, the music tucked under.
  constexpr std::array moves{
      Move{"vocals", "gain", 1.5}, Move{"vocals", "mid", 2.5},   Move{"vocals", "midHz", 3200.0},
      Move{"vocals", "high", 1.5}, Move{"drums", "high", 3.0},   Move{"drums", "highHz", 6500.0},
      Move{"drums", "low", 1.5},   Move{"drums", "lowHz", 70.0}, Move{"bass", "gain", -1.0},
      Move{"bass", "mid", -3.0},   Move{"bass", "midHz", 250.0}, Move{"bass", "midQ", 1.4},
      Move{"music", "gain", -2.0}, Move{"music", "mid", -2.0},   Move{"music", "midHz", 2500.0},
  };
  for (const auto& move : moves)
    role(*song->controller, QString::fromLatin1(move.role))
        ->set_value(QString::fromLatin1(move.parameter), move.value);
  const Readings readings{
      {song->kick, {0.5F, 0.47F, 0.25F, 0.24F, false}},
      {song->snare, {0.36F, 0.4F, 0.14F, 0.16F, false}},
      {song->bass, {0.38F, 0.38F, 0.22F, 0.22F, false}},
      {song->lead, {0.33F, 0.3F, 0.15F, 0.14F, false}},
      {song->double_vox, {0.18F, 0.21F, 0.08F, 0.09F, false}},
      {song->chops, {0.14F, 0.12F, 0.05F, 0.05F, false}},
      {song->pad, {0.24F, 0.27F, 0.13F, 0.14F, false}},
  };
  auto panel = load(*song, readings);
  panel_fit::require_text_fits(panel->root, "rebalance screenshot");
  QDir().mkpath(QString::fromLocal8Bit(directory));
  const QString path = QString::fromLocal8Bit(directory) + QStringLiteral("/rebalance.png");
  require(panel->window.grabWindow().save(path, "PNG"), "screenshot saved");
  std::cout << "wrote " << path.toStdString() << '\n';
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  return oscillate::test::run_cases(argc, argv, "rebalance panel contract passed",
                                    {{"the_panel_shows_every_role_and_track_and_fits",
                                      the_panel_shows_every_role_and_track_and_fits},
                                     {"a_fader_drag_saves_the_role_gain_as_one_step",
                                      a_fader_drag_saves_the_role_gain_as_one_step},
                                     {"choosing_a_role_saves_it_and_moves_the_track",
                                      choosing_a_role_saves_it_and_moves_the_track},
                                     {"role_meters_combine_the_tracks_each_stage_acts_on",
                                      role_meters_combine_the_tracks_each_stage_acts_on},
                                     {"screenshot_if_requested", screenshot_if_requested}});
}
