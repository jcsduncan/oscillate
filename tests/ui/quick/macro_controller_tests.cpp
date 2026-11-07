// The macro strip and racks (ADR-0059): knobs map to macros, a turning macro
// is heard and shown without saving until it is let go, ranges and names
// edit in one step, and racks save, load, export and refuse bad files.

#include "../../../apps/daw_quick/src/macro_controller.hpp"
#include "../../../apps/daw_quick/src/native_synth_device_controller.hpp"
#include "../../../apps/daw_quick/src/native_synth_session_adapter.hpp"
#include "../../../apps/daw_quick/src/rack_library_controller.hpp"

#include "oscillate/application/project_session.hpp"
#include "oscillate/automation_project/preparation.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/test/check.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QString>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
namespace application = oscillate::application;
namespace project = oscillate::project;
namespace quick = oscillate::quick;
namespace synth = oscillate::native_dsp::synth;
namespace effects = oscillate::native_dsp::effects;
namespace fs = std::filesystem;

using oscillate::test::require;

template <typename T> T take(oscillate::foundation::Result<T> result) {
  if (!result)
    throw std::runtime_error(result.error().message);
  return std::move(result).value();
}

class Directory {
public:
  Directory() {
    std::string pattern = (fs::temp_directory_path() / "oscillate-macros-ui-XXXXXX").string();
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

// A Prism track with Contour, their panels, and a macro strip whose live
// and show hooks are recorded.
struct Fixture {
  Directory directory;
  std::unique_ptr<application::ProjectSession> session =
      take(application::ProjectSession::create(directory.path() / "song"));
  project::TrackId track =
      take(session->instruments().add_native_instrument_track("prism", std::nullopt));
  project::InsertSlotId contour = take(session->effects().add_effect_engine(track, 0, "contour"));
  std::unique_ptr<quick::SessionNativeSynthAdapter> synth_adapter;
  std::unique_ptr<quick::SessionNativeSynthAdapter> effect_adapter;
  std::unique_ptr<quick::NativeSynthDeviceController> synth_panel;
  std::unique_ptr<quick::NativeSynthDeviceController> effect_panel;
  std::unique_ptr<quick::MacroController> macros;
  QString status;
  int edits = 0;
  int saves = 0;
  std::vector<std::vector<double>> live_synth;
  std::vector<std::vector<double>> live_effect;
  int shown = 0;
  int handed_back = 0;

  Fixture() {
    const auto* prism = synth::find_engine("prism");
    const auto* filter = effects::find_engine("contour");
    synth_adapter = std::make_unique<quick::SessionNativeSynthAdapter>(*session, track, *prism);
    effect_adapter = std::make_unique<quick::SessionNativeSynthAdapter>(
        *session, project::SignalPathId{track}, contour, *filter);
    synth_panel =
        std::make_unique<quick::NativeSynthDeviceController>(*prism->descriptor, *synth_adapter);
    effect_panel =
        std::make_unique<quick::NativeSynthDeviceController>(*filter->descriptor, *effect_adapter);
    macros = std::make_unique<quick::MacroController>(quick::MacroController::Hooks{
        [this] { return session.get(); }, [] { return true; },
        [this](QString message) { status = std::move(message); },
        [this] {
          ++edits;
          synth_panel->refresh();
          effect_panel->refresh();
          macros->refresh();
        },
        [this] { ++saves; },
        [this](project::TrackId, const synth::EngineDescriptor&, std::span<const double> values) {
          live_synth.emplace_back(values.begin(), values.end());
          return true;
        },
        [this](project::TrackId, project::InsertSlotId, const synth::EngineDescriptor&,
               std::span<const double> values) {
          live_effect.emplace_back(values.begin(), values.end());
          return true;
        },
        // As the bridge does: the matching panel follows.
        [this](project::TrackId, std::optional<project::InsertSlotId> slot,
               const std::vector<double>* values) {
          values != nullptr ? ++shown : ++handed_back;
          auto& adapter = slot ? *effect_adapter : *synth_adapter;
          auto& panel = slot ? *effect_panel : *synth_panel;
          adapter.show_macro_values(values);
          panel.refresh();
        }});
    synth_panel->set_macro_map_state(&macros->map_state());
    effect_panel->set_macro_map_state(&macros->map_state());
    macros->set_track_id(QString::fromStdString(track.to_string()));
  }

  [[nodiscard]] const project::Track& saved() const { return session->snapshot().tracks.front(); }
};

void knobs_map_to_macros() {
  Fixture f;
  require(f.macros->available() && f.macros->macros().size() == 8, "eight macros for the track");
  f.macros->set_armed(2);
  require(f.synth_panel->macro_map_index() == 2 && f.effect_panel->macro_map_index() == 2,
          "MAP mode reaches every panel");
  const auto revision_before = f.synth_panel->macro_revision();
  require(f.synth_panel->map_to_macro(QStringLiteral("prism.sub.level"), 2),
          "a synth knob maps: " + f.synth_panel->status_text().toStdString());
  require(f.synth_panel->macro_of(QStringLiteral("prism.sub.level")) == 2 &&
              f.synth_panel->macro_revision() > revision_before,
          "the knob shows its macro");
  const auto& mapping = f.saved().macros.at(2).mappings.at(0);
  require(std::abs(mapping.minimum - 0.25) < 1e-9 && mapping.maximum == 1.0,
          "the range runs from the knob's value to its top");
  require(std::abs(f.synth_panel->value(QStringLiteral("prism.sub.level")) - 0.25) < 1e-9,
          "a macro at zero leaves the knob where it was");
  require(f.effect_panel->map_to_macro(QStringLiteral("contour.cutoff"), 2),
          "an effect knob maps to the same macro");
  require(f.saved().macros.at(2).mappings.size() == 2, "the macro drives both");

  // Mapping to another macro moves the knob; unmapping frees it.
  require(f.synth_panel->map_to_macro(QStringLiteral("prism.sub.level"), 3) &&
              f.synth_panel->macro_of(QStringLiteral("prism.sub.level")) == 3 &&
              f.saved().macros.at(2).mappings.size() == 1,
          "a knob follows one macro");
  require(f.synth_panel->unmap_macro(QStringLiteral("prism.sub.level")) &&
              f.synth_panel->macro_of(QStringLiteral("prism.sub.level")) == -1,
          "a knob is unmapped");
  f.synth_panel->end_macro_map();
  require(f.macros->armed() == -1 && f.synth_panel->macro_map_index() == -1, "MAP mode ends");
}

void turning_is_heard_then_saved_once() {
  Fixture f;
  require(f.synth_panel->map_to_macro(QStringLiteral("prism.sub.level"), 0) &&
              f.effect_panel->map_to_macro(QStringLiteral("contour.cutoff"), 0),
          "mappings failed");
  const auto& sub = f.saved().macros.at(0).mappings.at(0);
  const auto top = oscillate::automation_project::macro_parameter_value(sub, 1.0);
  const auto revision = f.session->snapshot().revision;
  const auto edits = f.edits;
  f.macros->begin_turn(0);
  require(f.macros->turning() == 0, "the macro turns");
  f.macros->turn(0, 0.5);
  f.macros->turn(0, 1.0);
  require(f.session->snapshot().revision == revision && f.edits == edits,
          "a turn saves nothing while it moves");
  require(f.live_synth.size() == 2 && f.live_effect.size() == 2, "each move is heard live");
  require(std::abs(f.synth_panel->value(QStringLiteral("prism.sub.level")) - top) < 1e-9 &&
              f.shown == 4,
          "the panels follow the turn");
  require(f.macros->macros().at(0).toMap().value("value").toDouble() == 1.0,
          "the strip shows the turn");
  f.macros->end_turn(0);
  require(f.session->snapshot().revision == revision + 1 && f.edits == edits + 1 && f.saves == 1,
          "the release saves one edit");
  require(f.handed_back == 2 && f.saved().macros.at(0).value == 1.0 &&
              std::abs(f.synth_panel->value(QStringLiteral("prism.sub.level")) - top) < 1e-9,
          "the saved macro and parameters match what was heard");
  require(f.session->history().undo().has_value() && f.saved().macros.at(0).value == 0.0,
          "one undo takes the turn back");

  // A cancelled turn plays the saved values again and saves nothing.
  f.synth_panel->refresh();
  const auto saved_sub = f.synth_panel->value(QStringLiteral("prism.sub.level"));
  const auto before_cancel = f.session->snapshot().revision;
  f.macros->begin_turn(0);
  f.macros->turn(0, 0.7);
  f.macros->cancel_turn(0);
  require(f.session->snapshot().revision == before_cancel && f.macros->turning() == -1,
          "a cancelled turn saves nothing");
  const auto sub_index =
      synth::find_parameter(*synth::find_engine("prism")->descriptor, "prism.sub.level");
  require(sub_index && std::abs(f.live_synth.back()[*sub_index] - saved_sub) < 1e-9,
          "the saved value plays again");
  require(std::abs(f.synth_panel->value(QStringLiteral("prism.sub.level")) - saved_sub) < 1e-9,
          "the panel shows the saved value again");
}

// A turn that outlives its session, here replaced by a copy of the same
// project with the same track and revision, commits and offers nothing.
void a_turn_outliving_its_session_touches_nothing() {
  constexpr double halfway = 0.5;
  constexpr double top = 1.0;
  Fixture f;
  require(f.synth_panel->map_to_macro(QStringLiteral("prism.sub.level"), 0), "mapping failed");
  require(f.session->saving().save().has_value(), "save failed");
  const auto copy_root = f.directory.path() / "copy";
  fs::copy(f.directory.path() / "song", copy_root, fs::copy_options::recursive);
  auto copy = take(application::ProjectSession::open(copy_root));
  require(copy->snapshot().revision == f.session->snapshot().revision,
          "the copy does not share the original's revision");

  // Released after the session changed: nothing is saved in the copy.
  f.macros->begin_turn(0);
  f.macros->turn(0, halfway);
  require(f.live_synth.size() == 1, "the turn was not heard");
  auto other = std::exchange(f.session, std::move(copy));
  const auto revision = f.session->snapshot().revision;
  const auto edits = f.edits;
  f.macros->end_turn(0);
  require(f.macros->turning() == -1 && f.session->snapshot().revision == revision &&
              f.edits == edits && f.saved().macros.at(0).value == 0.0,
          "a turn outliving its session committed to the session that replaced it");

  // Moved after the session changed: nothing more is offered.
  f.macros->begin_turn(0);
  f.macros->turn(0, halfway);
  const auto offered = f.live_synth.size();
  f.session.swap(other);
  f.macros->turn(0, top);
  require(f.live_synth.size() == offered && f.macros->turning() == -1,
          "a turn outliving its session was offered to the devices");
}

void strip_lists_ranges_and_names() {
  Fixture f;
  require(f.effect_panel->map_to_macro(QStringLiteral("contour.cutoff"), 1), "mapping failed");
  f.macros->refresh(); // The app refreshes on every saved revision.
  auto entry = f.macros->macros().at(1).toMap();
  const auto mappings = entry.value("mappings").toList();
  require(entry.value("label").toString() == QStringLiteral("Macro 2") && mappings.size() == 1,
          "the strip lists the macro's mapping");
  const auto row = mappings.front().toMap();
  require(row.value("label").toString().contains(QStringLiteral("Contour")) &&
              row.value("maximum").toDouble() == 1.0 &&
              !row.value("maximum_text").toString().isEmpty(),
          "a mapping shows its device, parameter and range");
  require(!f.macros->position_text(1, 0, 0.5).isEmpty(), "a range handle previews its value");
  const auto revision = f.session->snapshot().revision;
  f.macros->set_mapping_range(1, 0, 0.0, 0.5);
  const auto& saved = f.saved().macros.at(1).mappings.at(0);
  require(f.session->snapshot().revision == revision + 1 && saved.minimum < saved.maximum &&
              std::abs(f.macros->macros()
                           .at(1)
                           .toMap()
                           .value("mappings")
                           .toList()
                           .front()
                           .toMap()
                           .value("maximum")
                           .toDouble() -
                       0.5) < 1e-6,
          "a range edit is one step at the knob positions given");
  f.macros->rename(1, QStringLiteral("  Bright  "));
  require(f.saved().macros.at(1).name == "Bright" &&
              f.macros->macros().at(1).toMap().value("label").toString() ==
                  QStringLiteral("Bright"),
          "a macro is renamed");
  f.macros->remove_mapping(1, 0);
  require(f.saved().macros.at(1).mappings.empty() &&
              f.effect_panel->macro_of(QStringLiteral("contour.cutoff")) == -1,
          "a mapping is removed from the strip");
  f.macros->set_track_id(QString());
  require(!f.macros->available(), "no track, no macros");
}

void racks_save_load_and_refuse() {
  Fixture f;
  ::setenv("XDG_DATA_HOME", (f.directory.path() / "data").c_str(), 1);
  require(f.effect_panel->map_to_macro(QStringLiteral("contour.cutoff"), 0), "mapping failed");
  QString status;
  int edits = 0;
  quick::RackLibraryController racks(quick::RackLibraryController::Hooks{
      [&] { return f.session.get(); }, [] { return true; },
      [&](QString message) { status = std::move(message); }, [&] { ++edits; }, [] {}});
  const auto track_id = QString::fromStdString(f.track.to_string());
  require(!racks.save_rack(track_id, QStringLiteral("a/b")), "a name with a slash is refused");
  require(racks.save_rack(track_id, QStringLiteral("Wobble")) && racks.racks().size() == 1 &&
              QFile::exists(racks.directory() + QStringLiteral("/Wobble.json")),
          "a rack is saved: " + status.toStdString());

  const auto target =
      take(f.session->instruments().add_native_instrument_track("prism", std::nullopt));
  const auto target_id = QString::fromStdString(target.to_string());
  const auto revision = f.session->snapshot().revision;
  require(racks.load_rack(target_id, QStringLiteral("Wobble")) &&
              f.session->snapshot().revision == revision + 1 && edits == 1,
          "a rack loads as one edit: " + status.toStdString());
  const auto& loaded = f.session->snapshot().tracks.back();
  require(loaded.effects.size() == 1 && loaded.macros.size() == 1 &&
              std::get<project::NativeEffectParameterTarget>(
                  loaded.macros.front().mappings.front().destination)
                      .slot == loaded.effects.front().id,
          "the loaded macro drives the loaded effect");

  const auto exported = f.directory.path() / "out.json";
  require(racks.export_rack(QStringLiteral("Wobble"), QString::fromStdString(exported.string())) &&
              fs::exists(exported),
          "a rack exports");
  require(racks.delete_rack(QStringLiteral("Wobble")) && racks.racks().isEmpty(),
          "a rack is deleted");
  require(racks.import_rack(QString::fromStdString(exported.string())) && racks.racks().size() == 1,
          "an exported rack imports under its file's name");
  const auto bad = f.directory.path() / "bad.json";
  {
    QFile file(QString::fromStdString(bad.string()));
    require(file.open(QIODevice::WriteOnly) && file.write("{\"kind\": \"oscillate.rack\"}") > 0,
            "fixture write failed");
  }
  require(!racks.import_rack(QString::fromStdString(bad.string())) && racks.racks().size() == 1,
          "a malformed rack is refused and not kept");
  require(!racks.load_rack(target_id, QStringLiteral("Missing")), "a missing rack is refused");
}

} // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  try {
    knobs_map_to_macros();
    turning_is_heard_then_saved_once();
    a_turn_outliving_its_session_touches_nothing();
    strip_lists_ranges_and_names();
    racks_save_load_and_refuse();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
  return 0;
}
