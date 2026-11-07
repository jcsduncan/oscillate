// A track's MIDI effects in the Quick app (ADR-0059): the chain's edits,
// each one undo step, and a device-kit controller per slot whose knobs and
// presets save through the engine.

#include "../../../apps/daw_quick/src/note_effects_controller.hpp"
#include "oscillate/test/check.hpp"

#include <QCoreApplication>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>

namespace {
namespace application = oscillate::application;
namespace project = oscillate::project;
namespace quick = oscillate::quick;
namespace fs = std::filesystem;

using oscillate::test::require;

class Directory {
public:
  Directory() {
    std::string pattern = (fs::temp_directory_path() / "oscillate-note-fx-ui-XXXXXX").string();
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

void chains_edit_in_one_step() {
  Directory directory;
  auto opened = application::ProjectSession::create(directory.path() / "song");
  require(opened.has_value(), "project creation failed");
  auto session = std::move(opened.value());
  const auto synth = session->instruments().add_synth_track();
  const auto bus = session->tracks().add_bus_track(project::TrackKind::group_bus);
  require(synth && bus, "tracks failed");
  QString status;
  int edits = 0;
  int saves = 0;
  quick::NoteEffectsController effects([&] { return session.get(); }, [] { return true; },
                                       [&](QString message) { status = std::move(message); },
                                       [&] { ++edits; }, [] {}, [&] { ++saves; });
  const auto track = QString::fromStdString(synth.value().to_string());
  const auto bus_id = QString::fromStdString(bus.value().to_string());
  require(effects.engines().size() == 6, "six engines for the browser, Comp among them");
  require(effects.accepts(track) && !effects.accepts(bus_id), "MIDI effects feed a built-in synth");
  require(effects.add(bus_id, QStringLiteral("arp")).isEmpty(), "a bus takes none");

  const auto before = session->snapshot().revision;
  // The "Chord Stabs" preset of the arp.
  int stabs = -1;
  for (const auto& value : effects.engines()) {
    const auto engine = value.toMap();
    if (engine.value("id").toString() != QStringLiteral("arp"))
      continue;
    for (const auto& preset : engine.value("presets").toList()) {
      if (preset.toMap().value("name").toString() == QStringLiteral("Chord Stabs"))
        stabs = preset.toMap().value("index").toInt();
    }
  }
  require(stabs >= 0, "the arp lists its presets");
  const auto arp = effects.add(track, QStringLiteral("arp"), -1, stabs);
  const auto scale = effects.add(track, QStringLiteral("scale"));
  require(!arp.isEmpty() && !scale.isEmpty() && session->snapshot().revision == before + 2 &&
              edits == 2 && saves == 2,
          "each add is one saved revision: " + status.toStdString());
  const auto chain = effects.chain(track);
  require(chain.size() == 2 && chain[0].toMap().value("engineId") == QStringLiteral("arp") &&
              chain[1].toMap().value("slotId") == scale &&
              chain[0].toMap().value("enabled").toBool(),
          "the chain lists them in order");
  const auto& saved = session->snapshot().tracks.front().note_effects.front();
  require(!saved.parameters.empty(), "a preset's values are saved with the add");

  effects.move(track, scale, 0);
  require(effects.chain(track)[0].toMap().value("slotId") == scale, "a MIDI effect moves");
  effects.set_enabled(track, arp, false);
  require(!session->snapshot().tracks.front().note_effects[1].enabled, "and switches off");

  // A knob drag through the device kit: one undo step for the whole drag.
  auto* controller =
      qobject_cast<quick::NativeSynthDeviceController*>(effects.controller(track, arp));
  require(controller != nullptr, "each slot has a device-kit controller");
  require(effects.controller(track, arp) == controller, "a second card shares it");
  controller->begin_gesture(QStringLiteral("arp.octaves"));
  controller->set_value(QStringLiteral("arp.octaves"), 2.0);
  controller->set_value(QStringLiteral("arp.octaves"), 3.0);
  controller->end_gesture(QStringLiteral("arp.octaves"));
  require(controller->value(QStringLiteral("arp.octaves")) == 3.0, "the knob turns");
  require(session->history().undo().has_value() &&
              std::ranges::none_of(session->snapshot().tracks.front().note_effects[1].parameters,
                                   [](const project::NativeInstrumentParameter& p) {
                                     return p.id == "arp.octaves";
                                   }),
          "one undo takes the whole drag back");
  controller->apply_preset(0);
  require(session->snapshot().tracks.front().note_effects[1].parameters.empty(),
          "the default preset saves no values");
  effects.release(track, arp);
  effects.release(track, arp);

  effects.remove(track, arp);
  require(effects.chain(track).size() == 1, "a MIDI effect is removed");
  require(session->history().undo().has_value() && effects.chain(track).size() == 2,
          "and comes back");
}

} // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  chains_edit_in_one_step();
}
