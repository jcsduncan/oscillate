// Crate's panel model (spec 09 §5.1) over the in-memory adapter: a load
// that chops by transients, the chop list and peaks, re-chops as one edit
// each with slots kept, manual edits, a marker drag's phases, eras, reverse,
// Chop to MIDI and a null model for every other engine.

#include "crate_controller.hpp"
#include "in_memory_native_synth_adapter.hpp"
#include "native_synth_device_controller.hpp"

#include "oscillate/application/crate_chops.hpp"
#include "oscillate/native_dsp/synth/crate.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/native_dsp/synth/pulse.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/timeline/timing.hpp"

#include <QGuiApplication>
#include <QVariantMap>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace synth = oscillate::native_dsp::synth;
namespace crate = oscillate::native_dsp::synth::crate;
namespace chops = oscillate::application::crate;
namespace project = oscillate::project;
using oscillate::quick::CrateController;
using oscillate::quick::InMemoryNativeSynthAdapter;
using oscillate::quick::NativeSynthDeviceController;
using oscillate::quick::NativeSynthGesture;

constexpr std::uint32_t rate = 48'000;
constexpr double sample_seconds = 2.0;
constexpr double tempo = 120.0;
// Clicks well apart, as the chopping contract's oracle places them.
constexpr std::array<double, 4> click_seconds{0.25, 0.75, 1.25, 1.6};
constexpr std::string_view identity =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

using oscillate::test::require;

const synth::EngineDescriptor& table() { return *crate::engine().descriptor; }

std::size_t index_of(std::string_view id) {
  const auto index = synth::find_parameter(table(), id);
  require(index.has_value(), "Crate parameter exists");
  return *index;
}

double value_of(const InMemoryNativeSynthAdapter& adapter, std::string_view id) {
  return adapter.values()[index_of(id)];
}

std::uint64_t frame_of(double seconds) { return static_cast<std::uint64_t>(seconds * rate); }

// Silence with a 5 ms decaying noise click at each of click_seconds.
std::shared_ptr<const synth::SynthMediaAsset> clicks() {
  auto audio = std::make_shared<synth::SynthMediaAsset>();
  audio->source_rate = rate;
  audio->frames.assign(frame_of(sample_seconds), {0.0F, 0.0F});
  std::uint32_t state = 0x9E3779B9U;
  for (const double at : click_seconds) {
    for (std::uint64_t i = 0; i < frame_of(0.005); ++i) {
      state ^= state << 13U;
      state ^= state >> 17U;
      state ^= state << 5U;
      const double noise = static_cast<double>(state) / static_cast<double>(UINT32_MAX) * 2.0 - 1.0;
      const auto sample =
          static_cast<float>(0.8 * noise * std::exp(-static_cast<double>(i) / (rate * 0.001)));
      audio->frames[frame_of(at) + i] = {sample, sample};
    }
  }
  return audio;
}

struct Fixture {
  InMemoryNativeSynthAdapter adapter{synth::default_values(table())};
  std::unique_ptr<NativeSynthDeviceController> device;
  CrateController* crate = nullptr;

  Fixture() {
    device = std::make_unique<NativeSynthDeviceController>(table(), adapter);
    crate = qobject_cast<CrateController*>(device->crate());
    require(crate != nullptr, "a Crate device has a Crate model");
  }
  // Loads the clicks as a file would land: with a tempo, or none.
  void load(std::optional<double> bpm = tempo) {
    require(crate->load_sample(QStringLiteral("file:///samples/clicks.wav")), "load begins");
    require(crate->busy() && adapter.media_import_busy(), "a load in flight is busy");
    require(adapter.finish_media_import(clicks(), std::string(identity), bpm), "load lands");
  }
  [[nodiscard]] QVariantMap chop(int index) const { return crate->chops().at(index).toMap(); }
};

void other_engines_have_no_crate() {
  InMemoryNativeSynthAdapter adapter(synth::default_values(*synth::pulse::engine().descriptor));
  NativeSynthDeviceController device(*synth::pulse::engine().descriptor, adapter);
  require(device.crate() == nullptr, "Pulse has no Crate model");
}

void a_load_chops_by_transients() {
  Fixture fixture;
  auto& crate = *fixture.crate;
  require(!crate.has_sample() && crate.chops().isEmpty() && crate.peaks(16).isEmpty() &&
              !crate.chop(),
          "an empty Crate shows nothing and cannot chop");
  require(!crate.load_sample(QString()), "an empty path is refused at once");
  fixture.load();
  require(!crate.busy() && crate.has_sample(), "the loaded sample shows");
  require(fixture.adapter.imports().size() == 1 &&
              fixture.adapter.imports().front().path == "/samples/clicks.wav" &&
              fixture.adapter.imports().front().slot == crate::sample_slot,
          "the file URL imports into the sample slot");
  require(crate.sample_name() == QStringLiteral("clicks.wav") && crate.bpm() == tempo &&
              crate.sample_rate() == static_cast<int>(rate) &&
              std::abs(crate.sample_seconds() - sample_seconds) < 1e-9,
          "name, tempo, rate and length come from the load");
  const auto audio = clicks();
  require(value_of(fixture.adapter, "crate.root") == chops::detect_root(*audio) &&
              !crate.key_name().isEmpty(),
          "the root is detected and named");
  const auto chop_list = crate.chops();
  require(chop_list.size() == static_cast<qsizetype>(click_seconds.size() + 1),
          "one chop per click plus the sample's start");
  for (int index = 0; index < chop_list.size(); ++index) {
    const auto chop = chop_list.at(index).toMap();
    require(chop.value("index").toInt() == index && chop.value("slot").toInt() == index &&
                chop.value("key").toInt() == 36 + index && chop.value("bank").toInt() == 0,
            "a first chop takes slots and pads in order");
  }
  const auto second = fixture.chop(1);
  require(std::abs(second.value("start_seconds").toDouble() - click_seconds[0]) < 0.002 &&
              std::abs(second.value("start").toDouble() - click_seconds[0] / sample_seconds) <
                  0.001 &&
              std::abs(second.value("end").toDouble() - fixture.chop(2).value("start").toDouble()) <
                  1e-12,
          "chops run from their start to the next");
  require(fixture.chop(0).value("key_name").toString() == QStringLiteral("C1"), "pad 1 is C1");

  const auto peaks = crate.peaks(100);
  require(peaks.size() == 100, "one peak per column");
  double loudest = 0.0;
  for (const auto& peak : peaks)
    loudest = std::max(loudest, peak.toDouble());
  require(loudest == 1.0 && peaks.at(0).toDouble() == 0.0 &&
              peaks.at(static_cast<qsizetype>(click_seconds[0] / sample_seconds * 100)).toDouble() >
                  0.0,
          "peaks are normalised: silence 0, a click above it");
}

void rechops_are_one_edit_each_and_keep_slots() {
  Fixture fixture;
  auto& crate = *fixture.crate;
  fixture.load();
  require(crate.chop_parameter(1, QStringLiteral("gain")) == QStringLiteral("crate.chop.01.gain") &&
              crate.chop_parameter(1, QStringLiteral("wobble")).isEmpty() &&
              crate.chop_parameter(9, QStringLiteral("gain")).isEmpty(),
          "chop parameters are keyed by slot");
  // A non-default control on a chop the next chop drops.
  auto values = fixture.adapter.values();
  values[index_of("crate.chop.02.gain")] = -6.0;
  values[index_of("crate.chop.00.gain")] = -3.0;
  fixture.adapter.submit(values, NativeSynthGesture::single);
  fixture.device->refresh();

  auto edits = fixture.adapter.state_submissions().size();
  require(crate.chop_equal(4), "equal chop succeeds");
  require(fixture.adapter.state_submissions().size() == edits + 1 &&
              fixture.adapter.state_submissions().back().gesture == NativeSynthGesture::single,
          "an equal chop is one edit");
  require(crate.chop_mode() == QStringLiteral("equal") && crate.equal_count() == 4 &&
              crate.chops().size() == 4,
          "equal chop sets its mode and count");
  require(fixture.chop(0).value("slot").toInt() == 0 &&
              value_of(fixture.adapter, "crate.chop.00.gain") == -3.0,
          "the chop at the start keeps its slot and controls");
  require(value_of(fixture.adapter, "crate.chop.02.gain") == 0.0,
          "a dropped or reused slot starts plain");

  require(crate.chop_grid(4), "grid chop succeeds at the sample's tempo");
  require(crate.chops().size() == 4 && crate.grid_division() == 4 &&
              std::abs(fixture.chop(1).value("start_seconds").toDouble() - 0.5) < 1e-9,
          "quarters at 120 BPM fall every half second");

  edits = fixture.adapter.state_submissions().size();
  require(crate.chop_transients(0.5), "transient chop succeeds");
  require(fixture.adapter.state_submissions().size() == edits + 1, "a transient chop is one edit");
  require(crate.chops().size() == static_cast<qsizetype>(click_seconds.size() + 1),
          "transients find the clicks again");
  require(crate.selected() >= 0 && crate.selected() < crate.chops().size(), "the selection clamps");

  // The selection follows its start through a re-chop that keeps it.
  crate.set_selected(3);
  const auto selected_slot = fixture.chop(3).value("slot").toInt();
  edits = fixture.adapter.state_submissions().size();
  crate.set_chop_mode(QStringLiteral("manual"));
  require(crate.chop() &&
              crate.chops().size() == static_cast<qsizetype>(click_seconds.size() + 1) &&
              fixture.adapter.state_submissions().size() == edits + 1,
          "a manual chop keeps the starts in one edit");
  require(crate.selected() == 3 && fixture.chop(3).value("slot").toInt() == selected_slot,
          "the selection and slots survive a re-chop that keeps their starts");
  crate.set_chop_mode(QStringLiteral("nonsense"));
  require(crate.chop_mode() == QStringLiteral("manual"), "unknown modes are ignored");

  // Without a tempo there is no grid.
  Fixture untimed;
  untimed.load(std::nullopt);
  require(untimed.crate->bpm() == 0.0 && !untimed.crate->chop_grid(16),
          "a sample without tempo has no grid");
}

void manual_edits_and_drags() {
  Fixture fixture;
  auto& crate = *fixture.crate;
  fixture.load();
  const auto count = crate.chops().size();
  auto edits = fixture.adapter.state_submissions().size();
  require(crate.add_chop(0.95), "a chop is added in empty space");
  require(crate.chops().size() == count + 1 &&
              fixture.adapter.state_submissions().size() == edits + 1,
          "adding is one edit");
  require(crate.selected() == static_cast<int>(count), "the new chop is selected");
  require(!crate.add_chop(0.95), "a chop on top of another is refused");
  require(!crate.remove_chop(0), "the first chop cannot go");
  const auto slot_of_last = fixture.chop(static_cast<int>(count)).value("slot").toInt();
  require(crate.remove_chop(static_cast<int>(count)) && crate.chops().size() == count,
          "a chop is removed in one edit");
  require(std::ranges::none_of(crate.chops(),
                               [&](const QVariant& chop) {
                                 return chop.toMap().value("slot").toInt() == slot_of_last &&
                                        chop.toMap().value("start").toDouble() > 0.9;
                               }),
          "the removed chop is gone");

  require(crate.rename_chop(1, QStringLiteral("Snap")) &&
              fixture.chop(1).value("name").toString() == QStringLiteral("Snap") &&
              fixture.adapter.document_submissions().back().gesture == NativeSynthGesture::single,
          "a rename is one document edit");

  // A drag: begin, update, commit under one gesture; the slot stays.
  const auto slot = fixture.chop(2).value("slot").toInt();
  const auto documents = fixture.adapter.document_submissions().size();
  // Chop 2 starts at 0.75 s of 2 s, between chops at 0.25 s and 1.25 s.
  require(crate.move_chop(2, 0.40, 0) && crate.move_chop(2, 0.42, 1) && crate.move_chop(2, 0.45, 2),
          "a drag runs its phases");
  const auto& drag = fixture.adapter.document_submissions();
  require(drag.size() == documents + 3 && drag[documents].gesture == NativeSynthGesture::begin &&
              drag[documents + 1].gesture == NativeSynthGesture::update &&
              drag[documents + 2].gesture == NativeSynthGesture::commit,
          "drag phases map to begin, update and commit");
  require(fixture.chop(2).value("slot").toInt() == slot &&
              std::abs(fixture.chop(2).value("start").toDouble() - 0.45) < 0.001,
          "the dragged chop keeps its slot at its new start");
  const auto before = fixture.chop(2).value("start").toDouble();
  require(crate.move_chop(2, 0.40, 0) && crate.move_chop(2, 0.42, 1) && crate.move_chop(2, 0, 3),
          "a cancelled drag runs");
  require(fixture.adapter.document_submissions().back().gesture == NativeSynthGesture::cancel &&
              fixture.chop(2).value("start").toDouble() == before,
          "a cancelled drag restores the chop");
  require(!crate.move_chop(0, 0.5, 0), "the first chop does not move");
}

void character_reverse_midi_and_keys() {
  Fixture fixture;
  auto& crate = *fixture.crate;
  fixture.load();
  require(crate.set_era(1), "an era is chosen");
  const auto& sp12 = crate::era_settings[1];
  require(value_of(fixture.adapter, "crate.era") == 1.0 &&
              value_of(fixture.adapter, "crate.bits") == sp12.bits &&
              value_of(fixture.adapter, "crate.rate") == sp12.rate &&
              value_of(fixture.adapter, "crate.drive") == sp12.drive &&
              fixture.adapter.submissions().back().gesture == NativeSynthGesture::single,
          "an era writes its bits, rate and drive as one edit");
  require(!crate.set_era(9), "an unknown era is refused");

  const auto edits = fixture.adapter.submissions().size();
  require(crate.reverse_all(true) && fixture.adapter.submissions().size() == edits + 1,
          "reverse all is one edit");
  for (const auto& chop : crate.chops()) {
    const auto slot = static_cast<std::size_t>(chop.toMap().value("slot").toInt());
    require(value_of(fixture.adapter, crate::chop_parameter_id(slot, "reverse")) == 1.0,
            "every chop is reversed");
  }

  require(crate.chop_to_midi(2), "Chop to MIDI writes a clip");
  require(fixture.adapter.clips().size() == 1, "one clip");
  const auto& clip = fixture.adapter.clips().front();
  require(clip.bar == 2 && clip.notes.size() == static_cast<std::size_t>(crate.chops().size()),
          "the clip is at the bar with a note per chop");
  constexpr auto quarter = oscillate::timeline::ticks_per_quarter;
  require(clip.notes.front().key == crate::first_pad_key && clip.notes.front().start.value == 0 &&
              clip.notes[1].key == crate::first_pad_key + 1 &&
              std::abs(static_cast<double>(clip.notes[1].start.value) -
                       click_seconds[0] * 2.0 * static_cast<double>(quarter)) <
                  0.01 * static_cast<double>(quarter) &&
              clip.length.value == 4 * quarter,
          "notes play the pads at the sample's tempo; two seconds at 120 is one bar");

  Fixture untimed;
  untimed.load(std::nullopt);
  require(!untimed.crate->chop_to_midi(0) && untimed.adapter.clips().empty(), "no tempo, no clip");

  require(crate.pad_key(0) == 36 && crate.pad_key(17) == 53 && crate.pad_key(-1) == -1,
          "pads play from key 36");
  auto values = fixture.adapter.values();
  values[index_of("crate.layout")] = static_cast<double>(crate::Layout::keys);
  fixture.adapter.submit(values, NativeSynthGesture::single);
  fixture.device->refresh();
  require(crate.pad_key(0) == -1, "the keys layout has no pad keys");

  crate.set_bank(1);
  crate.set_selected(99);
  require(crate.bank() == 1 && crate.selected() == static_cast<int>(crate.chops().size()) - 1,
          "bank and selection clamp");
}

void builtin_samples_count() {
  Fixture fixture;
  project::NativeSynthDocument document;
  document.media.push_back(
      {std::string(crate::sample_slot), project::NativeBuiltinMedia{"kick"}, 0});
  document.regions.push_back({0, std::string(crate::sample_slot), 0, "kick", "Kick"});
  fixture.adapter.submit_document(std::move(document), NativeSynthGesture::single);
  fixture.adapter.set_media(std::string(crate::sample_slot),
                            {nullptr, "kick", true, false, std::nullopt, {}});
  require(fixture.crate->has_sample() && fixture.crate->sample_name() == QStringLiteral("kick") &&
              fixture.crate->sample_rate() == static_cast<int>(crate::builtin_sample_rate) &&
              fixture.crate->chops().size() == 1 && !fixture.crate->peaks(8).isEmpty(),
          "a built-in sample shows and can be chopped");
}

} // namespace

int main(int argc, char** argv) {
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
    qputenv("QT_QPA_PLATFORM", "offscreen");
  QGuiApplication application(argc, argv);
  try {
    other_engines_have_no_crate();
    a_load_chops_by_transients();
    rechops_are_one_edit_each_and_keep_slots();
    manual_edits_and_drags();
    character_reverse_midi_and_keys();
    builtin_samples_count();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "crate controller contract passed\n";
  return 0;
}
