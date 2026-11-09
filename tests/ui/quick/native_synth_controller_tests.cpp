#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"
#include "native_synth_document_model.hpp"
#include "native_synth_session_adapter.hpp"

#include "oscillate/application/project_session.hpp"
#include "oscillate/native_dsp/effects/pump.hpp"

#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/concert.hpp"
#include "oscillate/native_dsp/synth/drift_engine.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_dsp/synth/pulse.hpp"
#include "oscillate/native_dsp/synth/torque.hpp"
#include "oscillate/test/check.hpp"

#include <QFile>
#include <QGuiApplication>
#include <QStringList>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariantMap>

#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace synth = oscillate::native_dsp::synth;
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
using oscillate::quick::NativeSynthGesture;
using namespace std::string_view_literals;

using oscillate::test::require;

// A tiny engine no panel has ever seen: the controller must need nothing else.
constexpr std::array waves{"Soft"sv, "Hard"sv, "Noise"sv};
constexpr std::array<oscillate::native_dsp::ParameterDescriptor, 3> parameters{{
    {"fake.level",
     "Level",
     "Out",
     "%",
     oscillate::native_dsp::ParameterScale::linear,
     0,
     1,
     0.5,
     {},
     oscillate::native_dsp::ParameterTiming::live,
     0.02},
    {"fake.cutoff",
     "Cutoff",
     "Tone",
     "Hz",
     oscillate::native_dsp::ParameterScale::logarithmic,
     20,
     20'000,
     1'000,
     {},
     oscillate::native_dsp::ParameterTiming::live,
     0.02},
    {"fake.wave", "Wave", "Tone", "", oscillate::native_dsp::ParameterScale::choice, 0, 2, 0, waves,
     oscillate::native_dsp::ParameterTiming::live, 0, false},
}};
constexpr std::array bright_values{std::pair{"fake.cutoff"sv, 8'000.0}};
constexpr std::array<oscillate::native_dsp::Preset, 1> presets{{{"Bright", "Lead", bright_values}}};
const oscillate::native_dsp::EngineDescriptor fake{"fake", "Fake", 1, parameters, presets};

void any_engine_drives_the_panel_model() {
  require(oscillate::native_dsp::validate_engine(fake).has_value(), "fixture engine is valid");
  InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(fake));
  NativeSynthDeviceController controller(fake, adapter);

  require(controller.engine_id() == QStringLiteral("fake") &&
              controller.engine_name() == QStringLiteral("Fake"),
          "the controller names its engine");
  require(!controller.has_filter_response() && controller.filter_response(64).isEmpty(),
          "an engine without visuals draws no curve");
  require(controller.parameter_ids(QStringLiteral("Tone")) ==
              QStringList({QStringLiteral("fake.cutoff"), QStringLiteral("fake.wave")}),
          "groups come from the engine table");
  const auto wave = controller.parameter(QStringLiteral("fake.wave"));
  require(wave.value(QStringLiteral("choices")).toStringList() ==
                  QStringList(
                      {QStringLiteral("Soft"), QStringLiteral("Hard"), QStringLiteral("Noise")}) &&
              wave.value(QStringLiteral("discrete")).toBool(),
          "choice parameters expose their labels");
  require(controller.parameter(QStringLiteral("nope")).isEmpty(), "unknown ids are empty");

  controller.set_normalised(QStringLiteral("fake.cutoff"), 0.5);
  require(std::fabs(controller.value(QStringLiteral("fake.cutoff")) - std::sqrt(20.0 * 20'000.0)) <
                  1e-6 &&
              std::fabs(controller.normalised(QStringLiteral("fake.cutoff")) - 0.5) < 1e-12,
          "log knob travel round-trips through the engine's taper");
  require(controller.display(QStringLiteral("fake.cutoff")) == QStringLiteral("632 Hz"),
          "values are formatted by the engine table");
  controller.set_choice(QStringLiteral("fake.wave"), 2);
  require(controller.choice(QStringLiteral("fake.wave")) == 2, "choices apply");
  controller.step(QStringLiteral("fake.wave"), 1, false);
  require(controller.choice(QStringLiteral("fake.wave")) == 2, "steps clamp at the last choice");
  controller.reset_to_default(QStringLiteral("fake.wave"));
  require(controller.is_default(QStringLiteral("fake.wave")), "reset restores the default");
  controller.set_value(QStringLiteral("nope"), 1.0);

  const auto before = adapter.submissions().size();
  controller.begin_gesture(QStringLiteral("fake.level"));
  controller.set_normalised(QStringLiteral("fake.level"), 0.25);
  controller.set_normalised(QStringLiteral("fake.level"), 0.75);
  controller.end_gesture(QStringLiteral("fake.level"));
  const auto& s = adapter.submissions();
  require(s.size() == before + 4 && s[before].gesture == NativeSynthGesture::begin &&
              s[before + 1].gesture == NativeSynthGesture::update &&
              s[before + 3].gesture == NativeSynthGesture::commit && s.back().values[0] == 0.75,
          "drags reach the adapter as one gesture");

  controller.apply_preset(0);
  require(controller.preset_name() == QStringLiteral("Bright") && !controller.preset_modified() &&
              controller.value(QStringLiteral("fake.cutoff")) == 8'000.0,
          "engine presets apply by index");
  controller.set_normalised(QStringLiteral("fake.level"), 0.1);
  require(controller.preset_modified(), "an edit marks the preset modified");
}

// Telemetry frames decode by each engine's documented layout (ADR-0037).
void telemetry_decodes_each_engine() {
  {
    InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(fake));
    NativeSynthDeviceController controller(fake, adapter);
    require(!controller.telemetry().value(QStringLiteral("live")).toBool(), "no frame: not live");
    adapter.set_telemetry({1.0F, 2.0F});
    const auto unknown = controller.telemetry();
    require(!unknown.value(QStringLiteral("live")).toBool(),
            "an engine with no telemetry layout is not decoded");
    const auto raw = unknown.value(QStringLiteral("frame")).toList();
    require(raw.size() == 2 && raw[1].toDouble() == 2.0, "but its raw frame passes through");
  }
  {
    const auto& table = *synth::torque::engine().descriptor;
    InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table));
    NativeSynthDeviceController controller(table, adapter);
    std::vector<float> frame(synth::torque::telemetry::size);
    frame[synth::torque::telemetry::running] = 1.0F;
    frame[synth::torque::telemetry::step] = 7.0F;
    frame[synth::torque::telemetry::key] = 48.5F;
    frame[synth::torque::telemetry::cutoff] = 900.0F;
    frame[synth::torque::telemetry::gate] = 1.0F;
    adapter.set_telemetry(frame);
    const auto t = controller.telemetry();
    require(t.value(QStringLiteral("live")).toBool() &&
                t.value(QStringLiteral("running")).toBool() &&
                t.value(QStringLiteral("step")).toInt() == 7 &&
                t.value(QStringLiteral("key")).toDouble() == 48.5 &&
                t.value(QStringLiteral("cutoff")).toDouble() == 900.0 &&
                t.value(QStringLiteral("gate")).toBool(),
            "Torque's step, key, cutoff and gate decode");
  }
  {
    const auto& table = *synth::pulse::engine().descriptor;
    InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table));
    NativeSynthDeviceController controller(table, adapter);
    std::vector<float> frame(synth::pulse::telemetry::size);
    frame[synth::pulse::telemetry::gain_reduction] = 3.5F;
    frame[synth::pulse::telemetry::first_pad + 2] = 0.75F;
    adapter.set_telemetry(frame);
    const auto t = controller.telemetry();
    const auto pads = t.value(QStringLiteral("pads")).toList();
    require(t.value(QStringLiteral("gain_reduction")).toDouble() == 3.5 && pads.size() == 12 &&
                pads[2].toDouble() == 0.75 && pads[0].toDouble() == 0.0,
            "Pulse's gain reduction and pad levels decode");
  }
  {
    const auto& table = *synth::drift::engine().descriptor;
    InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table));
    NativeSynthDeviceController controller(table, adapter);
    namespace d = synth::drift::telemetry;
    std::vector<float> frame(d::first_grain + 2 * d::stride);
    frame[d::count] = 2.0F;
    frame[d::first_grain + d::stride + d::key] = 64.0F;
    frame[d::first_grain + d::stride + d::behind] = 0.5F;
    frame[d::first_grain + d::stride + d::semitones] = 12.0F;
    frame[d::first_grain + d::stride + d::reverse] = 1.0F;
    frame[d::first_grain + d::stride + d::frozen] = 1.0F;
    adapter.set_telemetry(frame);
    const auto grains = controller.telemetry().value(QStringLiteral("grains")).toList();
    require(grains.size() == 2, "both grains decode");
    const auto second = grains[1].toMap();
    require(second.value(QStringLiteral("key")).toInt() == 64 &&
                second.value(QStringLiteral("behind")).toDouble() == 0.5 &&
                second.value(QStringLiteral("semitones")).toDouble() == 12.0 &&
                second.value(QStringLiteral("reverse")).toBool() &&
                second.value(QStringLiteral("frozen")).toBool(),
            "a grain's fields decode");
  }
  {
    const auto& table = *synth::concert::engine().descriptor;
    InMemoryNativeSynthAdapter adapter(oscillate::native_dsp::default_values(table));
    NativeSynthDeviceController controller(table, adapter);
    namespace c = synth::concert::telemetry;
    std::vector<float> frame(c::size, 0.0F);
    frame[c::sustain] = 0.5F;
    frame[c::soft] = 1.0F;
    frame[c::first_energy + 39] = -0.25F;
    frame[c::first_damper + 87] = 1.0F;
    adapter.set_telemetry(frame);
    const auto t = controller.telemetry();
    const auto energies = t.value(QStringLiteral("energies")).toList();
    const auto dampers = t.value(QStringLiteral("dampers")).toList();
    require(t.value(QStringLiteral("live")).toBool() &&
                t.value(QStringLiteral("sustain")).toDouble() == 0.5 &&
                t.value(QStringLiteral("sostenuto")).toDouble() == 0.0 &&
                t.value(QStringLiteral("soft")).toDouble() == 1.0,
            "Concert's pedals decode");
    require(energies.size() == 88 && dampers.size() == 88 && energies[39].toDouble() == -0.25 &&
                dampers[87].toDouble() == 1.0 && dampers[0].toDouble() == 0.0,
            "Concert's key energies and dampers decode");
    adapter.set_telemetry(std::vector<float>(c::size - 1, 0.0F));
    require(!controller.telemetry().value(QStringLiteral("live")).toBool(),
            "a short Concert frame is not decoded");
  }
}

// ADR-0058: a built-in effect's own presets — saved, listed, loaded whole as
// one undo step, exported and imported — plus the clipboard, and leaving a
// linked group, through a real session.
void user_presets_clipboard_and_links() {
  QTemporaryDir data;
  require(data.isValid(), "temporary data directory");
  qputenv("XDG_DATA_HOME", data.path().toUtf8());
  auto session = oscillate::application::ProjectSession::create(
      std::filesystem::path(data.path().toStdString()) / "song");
  require(session.has_value(), "project creation");
  auto& project = *session.value();
  const auto bass = project.instruments().add_synth_track().value();
  const auto pad = project.instruments().add_synth_track().value();
  const auto& pump = oscillate::native_dsp::effects::pump::engine();
  const auto slot = project.effects().add_effect_engine(bass, 0, "pump").value();
  const auto other = project.effects().add_effect_engine(pad, 0, "pump").value();
  oscillate::quick::SessionNativeSynthAdapter adapter(project, bass, slot, pump);
  NativeSynthDeviceController controller(*pump.descriptor, adapter);
  auto* document = qobject_cast<oscillate::quick::NativeSynthDocumentModel*>(controller.document());
  require(document != nullptr, "the controller has a document model");

  controller.set_value(QStringLiteral("pump.depth"), 0.4);
  const QVariantList drawn{QVariantMap{{QStringLiteral("x"), 0.0}, {QStringLiteral("y"), 0.0}},
                           QVariantMap{{QStringLiteral("x"), 0.3}, {QStringLiteral("y"), 1.0}},
                           QVariantMap{{QStringLiteral("x"), 1.0}, {QStringLiteral("y"), 1.0}}};
  require(document->set_curve(QStringLiteral("shape"), drawn), "a shape is drawn");
  const auto mine = project.effects().native_effect_document(bass, slot).value();
  require(!controller.save_user_preset(QStringLiteral("../escape")) &&
              controller.save_user_preset(QStringLiteral("Big Duck")),
          "a preset saves under a plain name only");
  require(controller.user_presets().size() == 1 &&
              controller.user_presets().front().toMap().value(QStringLiteral("name")) ==
                  QStringLiteral("Big Duck") &&
              controller.preset_name() == QStringLiteral("Big Duck") &&
              controller.preset_category() == QStringLiteral("User"),
          "the saved preset is listed and named");

  controller.apply_preset(0); // Classic Pump.
  require(controller.value(QStringLiteral("pump.depth")) == 1.0, "a factory preset loads");
  const auto before = project.snapshot().revision;
  require(controller.apply_user_preset(QStringLiteral("Big Duck")), "the user preset loads");
  require(project.snapshot().revision == before + 1 &&
              controller.value(QStringLiteral("pump.depth")) == 0.4 &&
              project.effects().native_effect_document(bass, slot).value() == mine &&
              !controller.preset_modified(),
          "values and shape load in one step");

  const auto exported = data.path() + QStringLiteral("/Shared Pump.json");
  require(controller.export_preset(QUrl::fromLocalFile(exported).toString()) &&
              controller.delete_user_preset(QStringLiteral("Big Duck")) &&
              controller.user_presets().isEmpty(),
          "a preset exports and deletes");
  controller.apply_preset(0);
  require(controller.import_preset(exported) &&
              controller.value(QStringLiteral("pump.depth")) == 0.4 &&
              controller.user_presets().size() == 1 &&
              controller.preset_name() == QStringLiteral("Shared Pump"),
          "an import loads and is kept");
  QFile junk(data.path() + QStringLiteral("/junk.json"));
  require(junk.open(QIODevice::WriteOnly) && junk.write("{}") == 2, "junk file");
  junk.close();
  require(!controller.import_preset(junk.fileName()), "a non-preset is refused");

  controller.set_clipboard_text(QStringLiteral("[[0,0],[1,1]]"));
  require(controller.clipboard_text() == QStringLiteral("[[0,0],[1,1]]"),
          "text round-trips through the clipboard");

  require(controller.linked_count() == 1, "a lone Pump reports a group of one");
  const std::vector<oscillate::project::InsertSlotId> group{slot, other};
  require(project.effects().link_native_effects(group).has_value(), "linking");
  controller.refresh();
  require(controller.linked_count() == 2, "the panel sees the link");
  require(document->remove_curve(QStringLiteral("shape")) &&
              project.effects().native_effect_document(pad, other).value().curves.empty(),
          "a removed curve leaves the whole group");
  controller.unlink();
  require(controller.linked_count() == 1 && project.effects().native_effect_link_count(other) == 1,
          "unlinking from the panel leaves the group");
  qunsetenv("XDG_DATA_HOME");
}

// A synth's own presets save and load whole, as an effect's do (ADR-0058);
// favourites and renames persist beside them, and every factory preset
// lists its tags.
void synth_presets_favourites_and_renames() {
  QTemporaryDir data;
  require(data.isValid(), "temporary data directory");
  qputenv("XDG_DATA_HOME", data.path().toUtf8());
  auto session = oscillate::application::ProjectSession::create(
      std::filesystem::path(data.path().toStdString()) / "song");
  require(session.has_value(), "project creation");
  auto& project = *session.value();
  const auto track = project.instruments().add_synth_track().value();
  const auto& engine = oscillate::native_dsp::synth::default_engine();
  oscillate::quick::SessionNativeSynthAdapter adapter(project, track, engine);
  NativeSynthDeviceController controller(*engine.descriptor, adapter);
  const auto cutoff = QStringLiteral("prism.filter.cutoff");

  controller.set_value(cutoff, 900.0);
  require(controller.save_user_preset(QStringLiteral("Dark Keys")) &&
              controller.preset_category() == QStringLiteral("User"),
          "a synth's preset saves");
  controller.apply_preset(0);
  require(controller.value(cutoff) != 900.0, "a factory preset replaces it");
  const auto before = project.snapshot().revision;
  require(controller.apply_user_preset(QStringLiteral("Dark Keys")) &&
              controller.value(cutoff) == 900.0 && project.snapshot().revision == before + 1 &&
              !controller.preset_modified(),
          "the synth preset loads in one step");

  const auto dark = controller.preset_key(true, QStringLiteral("Dark Keys"));
  const auto night = controller.preset_key(true, QStringLiteral("Night Keys"));
  const auto init = controller.preset_key(false, QStringLiteral("Init"));
  require(controller.set_favourite(dark, true) && controller.set_favourite(init, true) &&
              controller.favourites().size() == 2,
          "factory and user presets can be favourites");
  require(
      !controller.rename_user_preset(QStringLiteral("Dark Keys"), QStringLiteral("../x")) &&
          controller.rename_user_preset(QStringLiteral("Dark Keys"), QStringLiteral("Night Keys")),
      "a rename takes a plain name only");
  require(controller.preset_name() == QStringLiteral("Night Keys") &&
              controller.is_favourite(night) && !controller.is_favourite(dark) &&
              controller.user_presets().size() == 1,
          "a renamed preset stays loaded and a favourite");
  controller.set_value(cutoff, 500.0);
  require(controller.save_user_preset(QStringLiteral("Bright Keys")) &&
              !controller.rename_user_preset(QStringLiteral("Bright Keys"),
                                             QStringLiteral("Night Keys")),
          "a rename never replaces another preset");
  {
    NativeSynthDeviceController again(*engine.descriptor, adapter);
    require(again.favourites().size() == 2 && again.is_favourite(night) && again.is_favourite(init),
            "favourites persist");
  }
  require(controller.delete_user_preset(QStringLiteral("Night Keys")) &&
              !controller.is_favourite(night) && controller.favourites() == QStringList{init},
          "deleting a preset drops its favourite");
  require(controller.user_presets().size() == 1, "favourites never list as presets");
  for (const auto& entry : controller.presets()) {
    require(entry.toMap().contains(QStringLiteral("tags")), "every factory preset lists its tags");
  }
  qunsetenv("XDG_DATA_HOME");
}

} // namespace

int main(int argc, char** argv) {
  // The clipboard needs a GUI application; no window ever shows.
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
    qputenv("QT_QPA_PLATFORM", "offscreen");
  QGuiApplication application(argc, argv);
  try {
    any_engine_drives_the_panel_model();
    telemetry_decodes_each_engine();
    user_presets_clipboard_and_links();
    synth_presets_favourites_and_renames();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "native synth controller contract passed\n";
  return 0;
}
