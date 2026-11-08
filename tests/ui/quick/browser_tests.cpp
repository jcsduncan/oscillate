#include "project_bridge.hpp"

#include "oscillate/application/project_session.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/test/check.hpp"

#include <QCoreApplication>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

// The browser (UI redesign): a card per preset of every built-in instrument
// and effect plus the starter sounds; categories, search and favourites;
// double-click creates a track with an instrument preset or appends an
// effect to the selected chain; every card carries the device drag payload;
// the docked column offers the same catalogue. Cards dropped on a track row
// or lane swap its instrument or extend its chain, and the docked column's
// right-click menu runs either action.
namespace {
namespace app = oscillate::application;
namespace effects = oscillate::native_dsp::effects;
namespace quick = oscillate::quick;
namespace synth = oscillate::native_dsp::synth;

using oscillate::test::require;

QQuickItem* find_item(QQuickItem* root, const QString& name) {
  if (root->objectName() == name)
    return root;
  for (auto* child : root->childItems()) {
    if (auto* found = find_item(child, name))
      return found;
  }
  return nullptr;
}

// Every item named `name` under `root`, in tree order.
void collect_items(QQuickItem* root, const QString& name, std::vector<QQuickItem*>& found) {
  if (root->objectName() == name)
    found.push_back(root);
  for (auto* child : root->childItems())
    collect_items(child, name, found);
}

// The drop target of `name` covering track row `row`.
QQuickItem* track_drop(QQuickItem* root, const QString& name, int row) {
  std::vector<QQuickItem*> found;
  collect_items(root, name, found);
  for (auto* item : found) {
    if (item->property("trackIndex").toInt() == row)
      return item;
  }
  return nullptr;
}

void put16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value));
  bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void put32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
  put16(bytes, static_cast<std::uint16_t>(value));
  put16(bytes, static_cast<std::uint16_t>(value >> 16U));
}

// A tenth of a second of 48 kHz mono 16-bit PCM.
void write_wav(const std::filesystem::path& path) {
  constexpr std::uint32_t frames = 4'800;
  std::vector<std::uint8_t> wav{'R', 'I', 'F', 'F'};
  put32(wav, 36 + frames * 2);
  wav.insert(wav.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  put32(wav, 16);
  put16(wav, 1);
  put16(wav, 1);
  put32(wav, 48'000);
  put32(wav, 96'000);
  put16(wav, 2);
  put16(wav, 16);
  wav.insert(wav.end(), {'d', 'a', 't', 'a'});
  put32(wav, frames * 2);
  for (std::uint32_t frame = 0; frame < frames; ++frame)
    put16(wav, 8192);
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
  require(output.good(), "WAV fixture write failed");
}

struct DropResult {
  bool entered = false;
  bool dropped = false;
};

// A browser drag of `payload` released over the centre of `target`.
DropResult drop_payload(QQuickWindow& window, QQuickItem& target, const QString& payload) {
  QMimeData mime;
  mime.setData(QStringLiteral("application/x-oscillate-device"), payload.toUtf8());
  const auto position = target.mapToScene({target.width() / 2, target.height() / 2}).toPoint();
  QDragEnterEvent enter(position, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
  QCoreApplication::sendEvent(&window, &enter);
  if (!enter.isAccepted()) {
    QDragLeaveEvent leave;
    QCoreApplication::sendEvent(&window, &leave);
    QCoreApplication::processEvents();
    return {};
  }
  QDropEvent drop(position, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
  QCoreApplication::sendEvent(&window, &drop);
  QCoreApplication::processEvents();
  return {true, drop.isAccepted()};
}

QVariantMap card_with(const QVariantList& cards, const QString& field, const QString& value) {
  for (const auto& card : cards) {
    if (card.toMap().value(field).toString() == value)
      return card.toMap();
  }
  return {};
}

void catalogue_has_a_card_per_engine() {
  QTemporaryDir directory;
  quick::ProjectBridge bridge(directory.path().toStdString());
  const auto cards = bridge.browser_catalogue();
  // One card per engine, not per preset; the four starter sounds have one each.
  const auto expected =
      static_cast<qsizetype>(4 + synth::engines().size() + effects::engines().size());
  require(cards.size() == expected, "one card per engine and starter sound");
  QSet<QString> keys;
  for (const auto& value : cards) {
    const auto card = value.toMap();
    require(!keys.contains(card.value("key").toString()), "card keys are unique");
    keys.insert(card.value("key").toString());
    const auto category = card.value("category").toString();
    require(
        QStringList{"Instruments", "Drums", "Effects", "Utilities", "Samples"}.contains(category),
        "every card has a browser category");
    require(!card.value("presetName").toString().isEmpty() &&
                !card.value("blurb").toString().isEmpty() &&
                card.value("color").toString().startsWith('#'),
            "every card has a name, a one-line description and an accent");
  }
  for (const auto* engine : synth::engines()) {
    const auto card =
        card_with(cards, "engineId", QString::fromStdString(std::string(engine->descriptor->id)));
    require(card.value("presetIndex").toInt() == -1 &&
                card.value("presets").toList().size() ==
                    static_cast<qsizetype>(engine->descriptor->presets.size()),
            "an engine card makes the default sound and carries its presets");
  }
  require(card_with(cards, "engineId", "pulse").value("category") == QStringLiteral("Drums"),
          "Pulse is a drum instrument");
  require(card_with(cards, "engineId", "lens").value("category") == QStringLiteral("Utilities"),
          "Lens is a utility");
  require(card_with(cards, "source", "starter").value("instrument").toBool(),
          "starter sounds create tracks");
  // Every built-in device is listed by what it is, under its job.
  for (const auto& value : cards) {
    const auto card = value.toMap();
    if (card.value("source").toString() != QStringLiteral("native"))
      continue;
    require(!card.value("section").toString().isEmpty() && card.value("sectionRank").toInt() >= 0 &&
                !card.value("abbreviation").toString().isEmpty() &&
                !card.value("keywords").toString().isEmpty(),
            "every built-in device has a section, a tile and search keywords: " +
                card.value("engineId").toString().toStdString());
  }
  const auto press = card_with(cards, "engineId", "press");
  require(press.value("kind") == QStringLiteral("Compressor") &&
              press.value("section") == QStringLiteral("Dynamics"),
          "Press is listed as a compressor under Dynamics");
}

// An empty project opens the docked browser on Instruments, the first step:
// with no track, an effect could only reach the master.
void empty_project_opens_on_instruments() {
  QTemporaryDir directory;
  require(directory.isValid(), "scratch directory unavailable");
  const auto root = std::filesystem::path(directory.path().toStdString()) / "empty";
  auto session = app::ProjectSession::create(root);
  require(session.has_value(), "project creation failed");
  require(session.value()->snapshot().tracks.empty(), "the new project has no tracks");
  quick::ProjectBridge bridge(directory.path().toStdString());
  bridge.set_project(std::move(session).value(), root);

  QQmlApplicationEngine qml;
  qml.rootContext()->setContextProperty("projectBridge", &bridge);
  qml.load(QUrl(QStringLiteral("qrc:/DawShell.qml")));
  require(!qml.rootObjects().isEmpty(), "DAW shell did not load");
  auto* window = qobject_cast<QQuickWindow*>(qml.rootObjects().front());
  require(window != nullptr, "DAW shell has no window");
  window->resize(1600, 1000);
  QCoreApplication::processEvents();
  auto* docked = find_item(window->contentItem(), QStringLiteral("dockedBrowser"));
  require(docked != nullptr && docked->isVisible(), "the shell shows its docked browser");
  auto* catalogue = docked->findChild<QObject*>(QStringLiteral("browserCatalogue"));
  require(catalogue != nullptr, "the docked browser has its catalogue");
  require(catalogue->property("category").toString() == QStringLiteral("Instruments"),
          "an empty project's browser opens on Instruments, not " +
              catalogue->property("category").toString().toStdString());
  // The full browser, opened on the built-ins, agrees.
  window->setProperty("pluginBrowserOpen", true);
  QCoreApplication::processEvents();
  auto* full = find_item(window->contentItem(), QStringLiteral("vst3BrowserPanel"));
  require(full != nullptr && full->isVisible(), "the full browser opens");
  full->setProperty("showBuiltIns", true);
  QCoreApplication::processEvents();
  auto* full_catalogue = full->findChild<QObject*>(QStringLiteral("browserCatalogue"));
  require(full_catalogue != nullptr &&
              full_catalogue->property("category").toString() == QStringLiteral("Instruments"),
          "an empty project's full browser opens on Instruments too");
}

void shell_browser_creates_and_adds() {
  QTemporaryDir directory;
  require(directory.isValid(), "scratch directory unavailable");
  const auto root = std::filesystem::path(directory.path().toStdString()) / "browser";
  auto session = app::ProjectSession::create(root);
  require(session.has_value(), "project creation failed");
  require(session.value()->instruments().add_synth_track().has_value(), "first track");
  require(session.value()->saving().save().has_value(), "project saved");
  quick::ProjectBridge bridge(directory.path().toStdString());
  bridge.set_project(std::move(session).value(), root);

  QQmlApplicationEngine qml;
  qml.rootContext()->setContextProperty("projectBridge", &bridge);
  qml.load(QUrl(QStringLiteral("qrc:/DawShell.qml")));
  require(!qml.rootObjects().isEmpty(), "DAW shell did not load");
  auto* window = qobject_cast<QQuickWindow*>(qml.rootObjects().front());
  require(window != nullptr, "DAW shell has no window");
  window->resize(1600, 1000);
  window->setProperty("selectedTrack", 0);
  window->setProperty("pluginBrowserOpen", true);
  QCoreApplication::processEvents();
  auto* browser = find_item(window->contentItem(), QStringLiteral("vst3BrowserPanel"));
  require(browser != nullptr && browser->isVisible(), "browser did not open");
  browser->setProperty("showBuiltIns", true);
  auto* catalogue = browser->findChild<QObject*>(QStringLiteral("browserCatalogue"));
  require(catalogue != nullptr, "the browser has a catalogue");
  auto* results = find_item(browser, QStringLiteral("nativeBrowserResults"));
  require(results != nullptr, "built-in results");

  const auto show = [&](const QString& category) {
    QMetaObject::invokeMethod(catalogue, "selectCategory", Q_ARG(QVariant, category));
    QCoreApplication::processEvents();
  };
  const auto double_click = [&](const QString& engine) {
    const auto entries = browser->property("filteredNativeEntries").toList();
    int index = -1;
    for (int i = 0; i < entries.size(); ++i) {
      if (entries[i].toMap().value("id").toString() == engine) {
        index = i;
        break;
      }
    }
    require(index >= 0, "the browser lists " + engine.toStdString());
    QMetaObject::invokeMethod(results, "positionViewAtIndex", Q_ARG(int, index), Q_ARG(int, 0));
    QCoreApplication::processEvents();
    auto* row = find_item(results, QStringLiteral("nativeBrowserEntry_") + engine);
    require(row != nullptr, "the card for " + engine.toStdString() + " exists");
    const auto point = row->mapToScene({row->width() / 3, row->height() / 3}).toPoint();
    QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier, point);
    QCoreApplication::processEvents();
    return entries[index].toMap();
  };

  // Instruments: every listed card is an instrument, drums included under
  // their own heading; double-click makes a new track playing that preset.
  show(QStringLiteral("Instruments"));
  const auto instruments = browser->property("filteredNativeEntries").toList();
  require(!instruments.isEmpty(), "instrument cards are listed");
  for (const auto& value : instruments)
    require(value.toMap().value("instrument").toBool(),
            "the Instruments category lists instruments only");
  // Listed by job: the synths before the drums.
  const auto first_drum = std::ranges::find_if(instruments, [](const QVariant& value) {
    return value.toMap().value("category").toString() == QStringLiteral("Drums");
  });
  require(first_drum != instruments.end() &&
              std::all_of(first_drum, instruments.end(),
                          [](const QVariant& value) {
                            return value.toMap().value("section").toString() !=
                                   QStringLiteral("Synths");
                          }),
          "drums are listed together, after the synths");
  const auto ember = double_click(QStringLiteral("ember"));
  const auto tracks = bridge.tracks();
  require(tracks.size() == 2 &&
              tracks.back().toMap().value("nativeEngineId").toString() == QStringLiteral("ember"),
          "double-click created an Ember track: " + bridge.status().toStdString());
  require(bridge.status().contains(ember.value("presetName").toString()),
          "the new track plays the engine's default sound");
  require(bridge.browser_recents().contains(ember.value("key").toString()),
          "the sound is remembered as recent");

  // Effects: double-click appends to the selected track's chain.
  window->setProperty("selectedTrack", 0);
  show(QStringLiteral("Effects"));
  QCoreApplication::processEvents();
  double_click(QStringLiteral("expanse"));
  const auto chain = bridge.insert_chains().at(0).toMap().value("effects").toList();
  require(chain.size() == 1 &&
              chain.front().toMap().value("engine").toString() == QStringLiteral("expanse"),
          "double-click added Expanse to the selected track: " + bridge.status().toStdString());

  // A search spans categories.
  auto* search = find_item(browser, QStringLiteral("vst3BrowserSearch"));
  require(search != nullptr, "search field");
  search->setProperty("text", QStringLiteral("hall"));
  QCoreApplication::processEvents();
  const auto found = browser->property("filteredNativeEntries").toList();
  require(!found.isEmpty(), "a search finds cards");
  for (const auto& value : found) {
    const auto card = value.toMap();
    // A search matches an engine by its name, kind, section, description,
    // keywords or any of its presets (Expanse's "Concert Hall").
    QString presets;
    for (const auto& preset : card.value("presets").toList())
      presets += ' ' + preset.toMap().value("name").toString();
    const auto haystack =
        (card.value("presetName").toString() + ' ' + card.value("engineName").toString() + ' ' +
         card.value("kind").toString() + ' ' + card.value("tag").toString() + ' ' +
         card.value("category").toString() + ' ' + card.value("blurb").toString() + ' ' +
         card.value("section").toString() + ' ' + card.value("keywords").toString() + presets)
            .toLower();
    require(haystack.contains(QStringLiteral("hall")), "every result matches the search");
  }
  // A producer's word finds the device, best match first.
  search->setProperty("text", QStringLiteral("comp"));
  QCoreApplication::processEvents();
  const auto compressors = browser->property("filteredNativeEntries").toList();
  require(!compressors.isEmpty() &&
              compressors.front().toMap().value("engineId") == QStringLiteral("press"),
          "a search for comp lists the compressor first");
  search->setProperty("text", QStringLiteral("verb"));
  QCoreApplication::processEvents();
  const auto reverbs = browser->property("filteredNativeEntries").toList();
  require(!reverbs.isEmpty() &&
              reverbs.front().toMap().value("engineId") == QStringLiteral("expanse"),
          "a search for verb lists the reverb first");
  search->setProperty("text", QStringLiteral("crackle"));
  QCoreApplication::processEvents();
  const auto textures = browser->property("filteredNativeEntries").toList();
  require(!textures.isEmpty() &&
              textures.front().toMap().value("engineId") == QStringLiteral("dust"),
          "a search for crackle lists Dust first");
  search->setProperty("text", QStringLiteral("stutter"));
  QCoreApplication::processEvents();
  const auto gates = browser->property("filteredNativeEntries").toList();
  require(!gates.isEmpty() && gates.front().toMap().value("engineId") == QStringLiteral("chop"),
          "a search for stutter lists Chop first");
  search->setProperty("text", QString());

  // Favourites persist and list.
  bridge.toggle_browser_favourite(ember.value("key").toString());
  show(QStringLiteral("Favourites"));
  require(!card_with(browser->property("filteredNativeEntries").toList(), "key",
                     ember.value("key").toString())
               .isEmpty(),
          "a starred sound is listed under Favourites");
  bridge.toggle_browser_favourite(ember.value("key").toString());
  require(!bridge.browser_favourites().contains(ember.value("key").toString()),
          "un-starring removes it");

  // Every card carries the device drag payload.
  QVariant payload;
  QMetaObject::invokeMethod(catalogue, "dragPayload", Q_RETURN_ARG(QVariant, payload),
                            Q_ARG(QVariant, QVariant(ember)));
  const auto json = QJsonDocument::fromJson(payload.toString().toUtf8()).object();
  require(json.value("source").toString() == QStringLiteral("native") &&
              json.value("kind").toString() == QStringLiteral("instrument") &&
              json.value("engine").toString() == QStringLiteral("ember") &&
              json.value("preset").toInt() == ember.value("presetIndex").toInt() &&
              json.value("key").toString() == ember.value("key").toString(),
          "the drag payload names the sound");
  window->setProperty("pluginBrowserOpen", false);

  // The docked column shares the catalogue and its double-click rules.
  QQmlComponent component(&qml, QUrl(QStringLiteral("qrc:/DockedBrowser.qml")));
  std::unique_ptr<QObject> created(component.createWithInitialProperties(
      {{QStringLiteral("bridge"), QVariant::fromValue(static_cast<QObject*>(&bridge))}}));
  auto* docked = qobject_cast<QQuickItem*>(created.get());
  require(docked != nullptr, "the docked browser loads: " + component.errorString().toStdString());
  docked->setParentItem(window->contentItem());
  docked->setSize({248, 900});
  QCoreApplication::processEvents();
  auto* docked_results = find_item(docked, QStringLiteral("dockedBrowserResults"));
  auto* docked_catalogue = docked->findChild<QObject*>(QStringLiteral("browserCatalogue"));
  require(docked_results && docked_catalogue &&
              docked_results->property("count").toInt() ==
                  docked->property("cards").toList().size() &&
              docked_results->property("count").toInt() > 0,
          "the docked browser lists its category's cards");
  require(docked_catalogue->property("showBuiltIns").toBool(),
          "the docked browser starts on built-in sounds");
}

void drops_and_docked_menu_target_tracks() {
  QTemporaryDir directory;
  require(directory.isValid(), "scratch directory unavailable");
  const auto root = std::filesystem::path(directory.path().toStdString()) / "drops";
  auto session = app::ProjectSession::create(root);
  require(session.has_value(), "project creation failed");
  require(session.value()->instruments().add_synth_track().has_value(), "instrument track");
  const auto source = std::filesystem::path(directory.path().toStdString()) / "source.wav";
  write_wav(source);
  const int fd = ::open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  require(fd >= 0, "WAV fixture open failed");
  const auto imported = session.value()->imports().import_wav_audio(fd);
  (void)::close(fd);
  require(imported.has_value(), "audio track import failed");
  require(session.value()->saving().save().has_value(), "project saved");
  quick::ProjectBridge bridge(directory.path().toStdString());
  bridge.set_project(std::move(session).value(), root);

  QQmlApplicationEngine qml;
  qml.rootContext()->setContextProperty("projectBridge", &bridge);
  qml.load(QUrl(QStringLiteral("qrc:/DawShell.qml")));
  require(!qml.rootObjects().isEmpty(), "DAW shell did not load");
  auto* window = qobject_cast<QQuickWindow*>(qml.rootObjects().front());
  require(window != nullptr, "DAW shell has no window");
  window->resize(1600, 1000);
  QCoreApplication::processEvents();

  const auto row_of = [&](const QString& kind) {
    const auto tracks = bridge.tracks();
    for (int index = 0; index < tracks.size(); ++index) {
      if (tracks[index].toMap().value("kind").toString() == kind)
        return index;
    }
    return -1;
  };
  const int instrument_row = row_of(QStringLiteral("instrument"));
  const int audio_row = row_of(QStringLiteral("audio"));
  require(bridge.tracks().size() == 2 && instrument_row >= 0 && audio_row >= 0,
          "an instrument and an audio track");
  const auto instrument_id = bridge.tracks().at(instrument_row).toMap().value("id").toString();
  const auto engine_of = [&](const QString& id) {
    for (const auto& track : bridge.tracks()) {
      if (track.toMap().value("id").toString() == id)
        return track.toMap().value("nativeEngineId").toString();
    }
    return QString();
  };
  const auto cards = bridge.browser_catalogue();
  const auto native_instrument = [&](const QString& except) {
    for (const auto& value : cards) {
      const auto card = value.toMap();
      if (card.value("source").toString() == QStringLiteral("native") &&
          card.value("instrument").toBool() && card.value("engineId").toString() != except)
        return card;
    }
    return QVariantMap{};
  };
  auto* docked = find_item(window->contentItem(), QStringLiteral("dockedBrowser"));
  require(docked != nullptr && docked->isVisible(), "the docked browser is shown");
  auto* catalogue = docked->findChild<QObject*>(QStringLiteral("browserCatalogue"));
  require(catalogue != nullptr, "the docked browser has a catalogue");
  const auto payload_of = [&](const QVariantMap& card) {
    QVariant payload;
    QMetaObject::invokeMethod(catalogue, "dragPayload", Q_RETURN_ARG(QVariant, payload),
                              Q_ARG(QVariant, QVariant(card)));
    return payload.toString();
  };

  // An instrument card on the instrument track's header replaces its
  // instrument as one revision.
  const auto first = native_instrument(engine_of(instrument_id));
  require(!first.isEmpty(), "a native instrument card to drop");
  auto* header =
      track_drop(window->contentItem(), QStringLiteral("trackRowDeviceDrop"), instrument_row);
  require(header != nullptr && header->isVisible(), "the instrument track header takes drops");
  auto before = bridge.revision();
  const auto replaced = drop_payload(*window, *header, payload_of(first));
  require(replaced.entered && replaced.dropped, "the header accepted the instrument drop");
  require(engine_of(instrument_id) == first.value("engineId").toString() &&
              bridge.revision() == before + 1 && bridge.tracks().size() == 2,
          "the dropped instrument replaced the track's engine in one revision: " +
              bridge.status().toStdString());

  // An instrument is refused on an audio track.
  auto* audio_header =
      track_drop(window->contentItem(), QStringLiteral("trackRowDeviceDrop"), audio_row);
  require(audio_header != nullptr, "the audio track header takes drops");
  before = bridge.revision();
  require(!drop_payload(*window, *audio_header, payload_of(first)).entered &&
              bridge.revision() == before,
          "an audio track refuses an instrument drop");

  // An effect card on the audio track's lane goes at the end of its chain.
  const auto effect = card_with(cards, "engineId", "expanse");
  require(!effect.isEmpty() && !effect.value("instrument").toBool(), "an effect card to drop");
  auto* audio_lane =
      track_drop(window->contentItem(), QStringLiteral("arrangementRowDeviceDrop"), audio_row);
  require(audio_lane != nullptr && audio_lane->isVisible(), "the audio lane takes drops");
  before = bridge.revision();
  const auto inserted = drop_payload(*window, *audio_lane, payload_of(effect));
  const auto chain = bridge.insert_chains().at(audio_row).toMap().value("effects").toList();
  require(inserted.entered && inserted.dropped && chain.size() == 1 &&
              chain.front().toMap().value("engine").toString() == QStringLiteral("expanse") &&
              bridge.revision() == before + 1,
          "the dropped effect was appended to the audio track's chain: " +
              bridge.status().toStdString());

  // An instrument on the empty space under the last lane makes a track.
  auto* empty = find_item(window->contentItem(), QStringLiteral("arrangementEmptyDrop"));
  require(empty != nullptr && empty->height() > 0, "empty arrangement space takes drops");
  const auto created = drop_payload(*window, *empty, payload_of(first));
  const auto playing_first = std::ranges::count_if(bridge.tracks(), [&](const QVariant& track) {
    return track.toMap().value("nativeEngineId").toString() == first.value("engineId").toString();
  });
  require(created.entered && created.dropped && bridge.tracks().size() == 3 && playing_first == 2,
          "an instrument dropped on empty space made a track: " + bridge.status().toStdString());

  // The docked browser's right-click menu swaps the selected track's
  // instrument.
  int selected_row = -1;
  for (int index = 0; index < bridge.tracks().size(); ++index) {
    if (bridge.tracks().at(index).toMap().value("id").toString() == instrument_id)
      selected_row = index;
  }
  window->setProperty("selectedTrack", selected_row);
  QCoreApplication::processEvents();
  const auto docked_cards = docked->property("cards").toList();
  int pick = -1;
  for (int index = 0; index < docked_cards.size(); ++index) {
    const auto card = docked_cards[index].toMap();
    if (card.value("source").toString() == QStringLiteral("native") &&
        card.value("instrument").toBool() &&
        card.value("engineId").toString() != engine_of(instrument_id)) {
      pick = index;
      break;
    }
  }
  require(pick >= 0, "the docked browser lists another instrument");
  const auto swap = docked_cards[pick].toMap();
  auto* docked_results = find_item(docked, QStringLiteral("dockedBrowserResults"));
  require(docked_results != nullptr, "docked results");
  QMetaObject::invokeMethod(docked_results, "positionViewAtIndex", Q_ARG(int, pick), Q_ARG(int, 0));
  QCoreApplication::processEvents();
  auto* entry = find_item(docked_results,
                          QStringLiteral("dockedBrowserEntry_") + swap.value("key").toString());
  require(entry != nullptr, "the docked card exists");
  QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier,
                    entry->mapToScene({entry->width() / 3, entry->height() / 2}).toPoint());
  QCoreApplication::processEvents();
  auto* menu = find_item(docked, QStringLiteral("dockedBrowserMenu"));
  auto* primary = find_item(docked, QStringLiteral("dockedBrowserMenuPrimary"));
  auto* secondary = find_item(docked, QStringLiteral("dockedBrowserMenuSecondary"));
  require(menu && menu->isVisible() && primary && secondary && secondary->isEnabled(),
          "right-click opens the card menu with an enabled replace action");
  before = bridge.revision();
  QTest::mouseClick(
      window, Qt::LeftButton, Qt::NoModifier,
      secondary->mapToScene({secondary->width() / 2, secondary->height() / 2}).toPoint());
  QCoreApplication::processEvents();
  require(!menu->isVisible() && engine_of(instrument_id) == swap.value("engineId").toString() &&
              bridge.revision() == before + 1 && bridge.tracks().size() == 3,
          "the menu's replace action swapped the selected track's instrument: " +
              bridge.status().toStdString());
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication application(argc, argv);
  try {
    catalogue_has_a_card_per_engine();
    empty_project_opens_on_instruments();
    shell_browser_creates_and_adds();
    drops_and_docked_menu_target_tracks();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "browser contract passed\n";
  return 0;
}
