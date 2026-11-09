#include "oscillate/export/offline_master.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_inserts/instrument_binding.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/project/model.hpp"
#include "oscillate/timeline/timing.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace foundation = oscillate::foundation;
namespace project = oscillate::project;
namespace timeline = oscillate::timeline;
namespace synth = oscillate::native_dsp::synth;
namespace effects = oscillate::native_dsp::effects;
namespace persistence = oscillate::persistence;
namespace exporting = oscillate::exporting;
namespace fs = std::filesystem;

constexpr std::int64_t beat = timeline::ticks_per_quarter;
constexpr std::int64_t bar = beat * 4;
constexpr int song_bars = 96;

template <typename T> T take(foundation::Result<T> result) {
  if (!result) {
    throw std::runtime_error(result.error().message);
  }
  return std::move(result).value();
}

template <typename T> T id(std::uint64_t serial) { return take(T::from_bits({0, serial})); }

class Gate final : public project::TransportGate {
public:
  class Lease final : public project::StoppedTransportLease {};
  foundation::Result<std::unique_ptr<project::StoppedTransportLease>> acquire_stopped() override {
    return foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::success(
        std::make_unique<Lease>());
  }
};

struct Composer {
  std::vector<project::Operation> operations;
  std::uint64_t next_track = 1;
  std::uint64_t next_clip = 1;
  std::uint64_t next_note = 1;
  std::uint64_t next_marker = 1;
  std::uint64_t next_lane = 1;
  std::uint64_t next_point = 1;

  project::TrackId instrument(std::string_view engine_id, std::size_t preset, std::string name,
                              std::uint32_t color, double level, double pan = 0.0) {
    const auto* engine = synth::find_engine(engine_id);
    if (engine == nullptr) {
      throw std::runtime_error("missing native synth: " + std::string(engine_id));
    }
    const auto track = id<project::TrackId>(next_track++);
    operations.emplace_back(project::AddTrack{track, project::TrackKind::instrument});
    operations.emplace_back(project::SetNativeInstrumentPatch{
        track, take(oscillate::native_inserts::native_patch_for_preset(*engine, preset))});
    operations.emplace_back(project::SetTrackName{track, std::move(name)});
    operations.emplace_back(project::SetTrackColor{track, color});
    operations.emplace_back(project::SetTrackLevelPan{track, level, pan});
    return track;
  }

  void effect(project::SignalPathId path, std::size_t index, std::string_view engine,
              std::vector<project::NativeInstrumentParameter> parameters = {}) {
    if (effects::find_engine(engine) == nullptr) {
      throw std::runtime_error("missing native effect: " + std::string(engine));
    }
    operations.emplace_back(project::AddNativeEffect{
        std::move(path), index,
        project::NativeEffectPatch{std::string(engine), 1, std::move(parameters), 1}});
  }

  void marker(int at_bar, std::string name) {
    operations.emplace_back(project::AddMarker{
        {id<project::MarkerId>(next_marker++), {at_bar * bar}, std::move(name)}});
  }

  void instrument_sweep(project::TrackId track, std::string_view engine_id,
                        std::string_view parameter,
                        std::initializer_list<std::pair<int, double>> moves) {
    const auto* engine = synth::find_engine(engine_id);
    if (engine == nullptr) {
      throw std::runtime_error("missing native synth for automation");
    }
    project::AutomationLane lane{
        id<project::AutomationLaneId>(next_lane++),
        project::NativeParameterTarget{track, std::string(parameter)},
        take(oscillate::native_inserts::native_automation_domain(*engine, parameter)),
        project::AutomationTimebase::musical,
        {},
        1};
    for (const auto& [at_bar, value] : moves) {
      lane.points.push_back({id<project::AutomationPointId>(next_point++), at_bar * bar, value,
                             project::AutomationShape::linear});
    }
    operations.emplace_back(project::AddAutomationLane{track, std::move(lane)});
  }

  struct Phrase {
    Composer& song;
    project::TrackId track;
    int start_bar;
    int bars;
    std::vector<project::MidiNote> notes;

    void note(int local_bar, double beat_offset, double length_beats, int key, int velocity) {
      const auto start = static_cast<std::int64_t>(local_bar) * bar +
                         static_cast<std::int64_t>(beat_offset * beat);
      const auto length = static_cast<std::int64_t>(length_beats * beat);
      notes.push_back({id<project::NoteId>(song.next_note++),
                       {start},
                       {start + length},
                       static_cast<std::uint8_t>(key),
                       static_cast<std::uint8_t>(velocity)});
    }

    void finish() {
      std::ranges::sort(notes, {}, [](const project::MidiNote& note) { return note.start.value; });
      song.operations.emplace_back(project::AddMidiClip{
          track,
          {id<project::ClipId>(song.next_clip++),
           project::MusicalInterval{{start_bar * bar}, {(start_bar + bars) * bar}},
           {bars * bar},
           false,
           std::move(notes)}});
    }
  };

  Phrase phrase(project::TrackId track, int start_bar, int bars) {
    return {*this, track, start_bar, bars, {}};
  }
};

constexpr std::array<int, 4> bass_roots{40, 36, 31, 38}; // E2 C2 G1 D2.
constexpr std::array<std::array<int, 4>, 4> chords{{
    {52, 55, 59, 66}, // Em9
    {48, 52, 55, 59}, // Cmaj7
    {50, 55, 59, 66}, // Gmaj7/D
    {50, 54, 57, 64}, // Dadd9
}};

void drums(Composer& song, project::TrackId track) {
  for (int section = 0; section < song_bars; section += 16) {
    auto p = song.phrase(track, section, 16);
    for (int b = 0; b < 16; ++b) {
      const int absolute = section + b;
      if (absolute < 8) {
        continue;
      }
      const bool break_section = absolute >= 48 && absolute < 64;
      const bool outro = absolute >= 80;
      const bool full = absolute >= 16 && !break_section && (!outro || absolute < 88);
      if (!break_section && absolute < 88) {
        for (int pulse = 0; pulse < 4; ++pulse) {
          p.note(b, pulse, 0.12, 36, pulse == 0 ? 110 : 99);
        }
      }
      if (full) {
        p.note(b, 1, 0.12, 40, 82);
        p.note(b, 3, 0.12, 40, 91);
      }
      if (absolute >= 12 && absolute < 92) {
        const int divisions =
            ((absolute >= 32 && absolute < 48) || (absolute >= 72 && absolute < 88)) ? 8 : 4;
        for (int h = 0; h < divisions; ++h) {
          p.note(b, static_cast<double>(h) * 4 / divisions, 0.09, 42,
                 43 + (h % 2 ? 19 : 0) + (absolute % 4 == 3 ? 5 : 0));
        }
      }
      if (absolute >= 24 && absolute < 88 && !break_section) {
        p.note(b, 1.5, 0.16, 46, 48);
        p.note(b, 3.5, 0.16, 46, 53);
      }
      if (absolute == 47 || absolute == 79) {
        for (int r = 0; r < 8; ++r) {
          p.note(b, 2 + r * 0.25, 0.09, 38, 44 + r * 8);
        }
      }
      if (absolute == 31 || absolute == 79 || absolute == 87) {
        p.note(b, 3, 0.35, 47, 68);
      }
    }
    if (!p.notes.empty()) {
      p.finish();
    }
  }
}

void bass(Composer& song, project::TrackId track) {
  for (int section : {16, 32, 64, 80}) {
    auto p = song.phrase(track, section, 16);
    for (int b = 0; b < 16; ++b) {
      const int root = bass_roots[static_cast<std::size_t>(((section + b) / 4) % 4)];
      p.note(b, 0.5, 0.43, root, 90);
      p.note(b, 1.5, 0.40, root, 77);
      p.note(b, 2.5, 0.45, root, 94);
      if ((section + b) % 4 != 3) {
        p.note(b, 3.5, 0.36, root + 7, 63);
      } else {
        p.note(b, 3.5, 0.36, root + 12, 76);
      }
    }
    p.finish();
  }
}

void pads(Composer& song, project::TrackId track) {
  for (int section : {0, 16, 32, 48, 64, 80}) {
    auto p = song.phrase(track, section, 16);
    for (int b = 0; b < 16; b += 4) {
      const auto& chord = chords[static_cast<std::size_t>(((section + b) / 4) % 4)];
      const int velocity = section == 48 ? 76 : section == 80 ? 68 : 58;
      for (int key : chord) {
        p.note(b, 0, 15.0, key, velocity);
      }
    }
    p.finish();
  }
}

void arp(Composer& song, project::TrackId track) {
  for (int section : {16, 32, 64, 80}) {
    auto p = song.phrase(track, section, 16);
    for (int b = 0; b < 16; ++b) {
      const auto& chord = chords[static_cast<std::size_t>(((section + b) / 4) % 4)];
      for (int n = 0; n < 8; ++n) {
        const auto index =
            std::array<std::size_t, 8>{0, 1, 2, 3, 2, 1, 3, 1}[static_cast<std::size_t>(n)];
        const int velocity = 48 + (n % 4 == 0 ? 19 : 0) + (b % 4 == 3 ? 6 : 0);
        p.note(b, n * 0.5, 0.28, chord[index] + 12, velocity);
      }
    }
    p.finish();
  }
}

void lead(Composer& song, project::TrackId track) {
  constexpr std::array<int, 16> melody{76, 74, 71, 74, 79, 78, 74, 71,
                                       72, 71, 67, 71, 69, 71, 74, 78};
  for (int section : {32, 80}) {
    auto p = song.phrase(track, section, 16);
    for (int b = 0; b < 16; ++b) {
      if (section == 32 && b < 4) {
        continue;
      }
      const int key = melody[static_cast<std::size_t>(b)];
      p.note(b, 0.0, 0.72, key, 73 + b % 3 * 5);
      if (b % 4 != 3) {
        p.note(b, 2.5, 0.36, melody[static_cast<std::size_t>((b + 1) % 16)], 58);
      }
      if (section == 80 && b % 4 == 3) {
        p.note(b, 3.0, 0.7, key - 12, 55);
      }
    }
    p.finish();
  }
}

void atmosphere(Composer& song, project::TrackId track) {
  for (int section : {0, 48, 64, 80}) {
    auto p = song.phrase(track, section, 16);
    for (int b = 0; b < 16; b += 4) {
      const auto& chord = chords[static_cast<std::size_t>(((section + b) / 4) % 4)];
      p.note(b, 0, 14.8, chord[0] - 12, 55);
      p.note(b, 0, 14.8, chord[2], 44);
    }
    p.finish();
  }
}

void create(const fs::path& root, const fs::path& wav) {
  if (fs::exists(root) || fs::exists(wav)) {
    throw std::runtime_error("project or WAV already exists; choose new output paths");
  }
  Gate gate;
  auto model = take(project::ProjectModel::create(
      id<project::ProjectId>(1), 48'000,
      take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1)),
      take(timeline::MeterMap::create({{{0}, 4, 4}}, 1)), gate));
  Composer song;
  const auto pulse = song.instrument("pulse", 3, "01  Pulse · 909 rhythm", 0xE69561, 0.75);
  const auto torque = song.instrument("torque", 4, "02  Torque · rolling bass", 0xE4BC66, 0.50);
  const auto ember =
      song.instrument("ember", 2, "03  Ember · analog strings", 0xB47AC7, 0.34, -0.08);
  const auto nova = song.instrument("nova", 5, "04  Nova · glass arpeggio", 0x6ABDC1, 0.32, 0.15);
  const auto halo = song.instrument("halo", 2, "05  Halo · bell motif", 0x78A9DB, 0.30, -0.12);
  const auto drift = song.instrument("drift", 5, "06  Drift · atmosphere", 0x8B93C9, 0.25);

  song.effect(ember, 0, "orbit", {{"orbit.mix", 0.28}});
  song.effect(ember, 1, "expanse", {{"expanse.mix", 0.18}, {"expanse.decay", 3.2}});
  song.effect(nova, 0, "relay",
              {{"relay.routing", 1}, {"relay.mix", 0.21}, {"relay.feedback", 0.38}});
  song.effect(halo, 0, "expanse", {{"expanse.mix", 0.24}, {"expanse.decay", 2.8}});
  song.effect(project::MasterSignalPath{}, 0, "ceiling", {{"ceiling.ceiling", -1.0}});

  drums(song, pulse);
  bass(song, torque);
  pads(song, ember);
  arp(song, nova);
  lead(song, halo);
  atmosphere(song, drift);
  song.marker(0, "Intro · distant lights");
  song.marker(16, "Pulse");
  song.marker(32, "First lift");
  song.marker(48, "Breakdown");
  song.marker(64, "Rebuild");
  song.marker(80, "Afterglow");
  song.marker(88, "Outro");
  song.operations.emplace_back(
      project::SetProjectKey{project::MusicalKey{4, project::KeyMode::minor}});
  song.instrument_sweep(torque, "torque", "torque.cutoff",
                        {{0, 280},
                         {16, 330},
                         {31, 600},
                         {32, 500},
                         {47, 900},
                         {48, 280},
                         {64, 280},
                         {79, 1'200},
                         {80, 750},
                         {95, 320}});
  song.instrument_sweep(
      ember, "ember", "ember.filter.cutoff",
      {{0, 900}, {16, 1'500}, {32, 2'300}, {48, 900}, {64, 1'100}, {80, 2'800}, {95, 650}});
  song.instrument_sweep(
      nova, "nova", "nova.filter.1.cutoff",
      {{0, 1'800}, {16, 1'800}, {32, 4'300}, {48, 1'200}, {64, 1'600}, {80, 6'000}, {95, 1'600}});
  auto applied = take(model.apply(song.operations));
  auto store = take(persistence::ProjectStore::open_writer(root, true));
  const auto saved = take(store.save(*applied.snapshot, persistence::SaveKind::explicit_save, {}));
  if (saved.status != persistence::SaveStatus::committed) {
    throw std::runtime_error("project save did not commit");
  }
  const auto reopened = take(store.load_current());
  if (reopened.snapshot.tracks.size() != 6 || reopened.snapshot.markers.size() != 7 ||
      !reopened.source_diagnostics.empty()) {
    throw std::runtime_error("saved demo did not reopen intact");
  }
  fs::create_directories(wav.parent_path());
  auto authority = take(exporting::StoppedExportAuthority::acquire(reopened.snapshot, gate));
  const auto outcome = take(exporting::render_wav_master(
      reopened.snapshot, store, std::move(authority), {wav, exporting::WavFormat::float32}));
  if (outcome != exporting::PublishOutcome::durable) {
    throw std::runtime_error("WAV export was not durable");
  }
  std::cout << "project=" << root << "\nmaster=" << wav << "\n";
}
} // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: oscillate_demo_song <new-project-dir> <new-master-wav>\n";
    return 2;
  }
  try {
    create(argv[1], argv[2]);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "demo song: " << error.what() << '\n';
    return 1;
  }
}
