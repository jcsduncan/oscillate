#include "../../../apps/daw_quick/src/project_bridge.hpp"
#include "oscillate/test/check.hpp"

#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
#include "oscillate/application/vst3/offline_export.hpp"
#include "oscillate/plugins/vst3_scan.hpp"
#endif

#include <QColor>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QTest>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace app = oscillate::application;
namespace exporting = oscillate::exporting;
namespace foundation = oscillate::foundation;
namespace project = oscillate::project;
namespace quick = oscillate::quick;

using oscillate::test::require;

template <typename T> T take(foundation::Result<T> result) {
  if (!result)
    throw std::runtime_error(result.error().message);
  return std::move(result).value();
}

class Directory {
public:
  Directory() {
    std::string pattern =
        (fs::temp_directory_path() / "oscillate-quick-vst3-export-XXXXXX").string();
    require(::mkdtemp(pattern.data()) != nullptr, "temporary directory failed");
    root_ = pattern;
  }
  ~Directory() {
    std::error_code ignored;
    fs::remove_all(root_, ignored);
  }
  [[nodiscard]] const fs::path& root() const { return root_; }

private:
  fs::path root_;
};

#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
void put16(std::vector<std::uint8_t>& output, std::uint16_t value) {
  output.push_back(static_cast<std::uint8_t>(value));
  output.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void put32(std::vector<std::uint8_t>& output, std::uint32_t value) {
  put16(output, static_cast<std::uint16_t>(value));
  put16(output, static_cast<std::uint16_t>(value >> 16U));
}

fs::path make_source_wav(const fs::path& root) {
  constexpr std::uint32_t frames = 6000;
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
  const auto path = root / "source.wav";
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
  require(output.good(), "source WAV write failed");
  return path;
}

std::vector<std::uint8_t> bytes(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  require(input.good(), "export did not open");
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

float sample(const std::vector<std::uint8_t>& wav, std::size_t frame) {
  require(wav.size() >= 56 + (frame + 1) * 8, "float32 WAV frame is missing");
  float value = 0.0F;
  std::memcpy(&value, wav.data() + 56 + frame * 8, sizeof(value));
  return value;
}

void near(float actual, float expected) {
  require(std::fabs(actual - expected) < 0.00001F, "independent WAV sample oracle failed");
}

std::uint32_t read_le(const std::vector<std::uint8_t>& data, std::size_t offset,
                      std::size_t width) {
  require(data.size() >= offset + width, "WAV header is truncated");
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < width; ++index)
    value |= static_cast<std::uint32_t>(data[offset + index]) << (8U * index);
  return value;
}

// Left channel of a canonical 44-byte PCM24 WAV, scaled to [-1, 1).
float pcm24_left(const std::vector<std::uint8_t>& wav, std::size_t frame) {
  const auto raw = read_le(wav, 44 + frame * 6, 3);
  return static_cast<float>(static_cast<std::int32_t>(raw << 8U) >> 8U) / 8'388'608.0F;
}

void wait_export(quick::ProjectBridge& bridge) {
  require(bridge.exports()->export_busy(), "export was not dispatched to a worker");
  bool ui_tick = false;
  QTimer::singleShot(0, [&ui_tick] { ui_tick = true; });
  require(QTest::qWaitFor([&bridge] { return !bridge.exports()->export_busy(); }, 15'000),
          "export worker did not complete");
  require(ui_tick, "UI event loop did not run during export");
}
#endif

void export_checks() {
  Directory directory;
  const auto project_root = directory.root() / "project";
  const auto bundle = directory.root() / "TimedInstrument.vst3";
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  fs::copy(VST3_INSTRUMENT_BUNDLE, bundle, fs::copy_options::recursive);
  oscillate::plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto scanned = scanner.scan(bundle);
  require(scanned.ok() && scanned.metadata.classes.size() == 1, "fixture scan failed");
  const auto& klass = scanned.metadata.classes.front();
  app::Vst3InsertIdentity identity{klass.id,
                                   klass.vendor,
                                   klass.name,
                                   klass.version,
                                   scanned.binary.canonical_bundle.string(),
                                   scanned.binary.sha256};
#else
  app::Vst3InsertIdentity identity{std::string(32, 'a'), "fixture-vendor",
                                   "Timed Instrument",   "1.0",
                                   bundle.string(),      std::string(64, 'a')};
#endif
  auto session = take(app::ProjectSession::create(project_root));
  require(session->tracks().add_instrument_track().has_value(), "instrument track failed");
  const auto track = session->snapshot().tracks.front().id;
  require(session->midi_clips().add_midi_clip(track, {0}).has_value(), "MIDI clip failed");
  const auto clip = session->snapshot().tracks.front().midi_clips.front().id;
  require(session->notes().add_note(clip, {0}, 60).has_value(), "MIDI note failed");
  auto inserted = session->effects().insert_vst3_plugin(track, 0, std::move(identity));
  require(inserted.has_value(), inserted ? "saved VST3 insert failed"
                                         : "saved VST3 insert failed: " + inserted.error().message);
  require(session->saving().save().has_value(), "project save failed");
  session.reset();
  session = take(app::ProjectSession::open(project_root));
  require(std::holds_alternative<project::Vst3PluginInstance>(
              session->snapshot().tracks.front().instrument_source),
          "saved VST3 instrument did not reopen");
  {
    auto task = take(session->exports().prepare_stopped_snapshot_export());
    require(!session->reserve_playback_start(), "stopped export lease did not block playback");
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
    auto altered = task.snapshot();
    ++altered.revision;
    const auto stale_target = directory.root() / "stale.wav";
    const auto& reader = task.reader();
    auto stale =
        exporting::render_vst3_master(altered, reader, std::move(task).take_authority(), scanner,
                                      exporting::MasterRenderOptions{stale_target});
    require(!stale && stale.error().code == foundation::ErrorCode::conflict &&
                !fs::exists(stale_target),
            "stale authority published output");
#endif
  }
  auto token = take(session->reserve_playback_start());
  token.finish_after_join();

  quick::ProjectBridge bridge(directory.root(), nullptr, {}, [] {
    return foundation::Result<std::vector<oscillate::audio_io::PipeWireSink>>::success({});
  });
  bridge.set_project(std::move(session), project_root);
  const auto master = directory.root() / "master.wav";
  bridge.exports()->set_export_target(QString::fromStdString(master.string()));
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  require(::setenv("OSCILLATE_VST3_INSTRUMENT_TAIL", "8", 1) == 0, "tail mode failed");
  // PCM16 at 44.1 kHz from a 48 kHz project reaches the shared output stage.
  const auto converted = directory.root() / "master-pcm16.wav";
  bridge.exports()->set_export_target(QString::fromStdString(converted.string()));
  bridge.exports()->set_export_format_index(2);
  bridge.exports()->set_export_rate_index(1);
  bridge.exports()->export_master();
  if (bridge.exports()->output_rate_conversion_available()) {
    wait_export(bridge);
    const auto pcm = bytes(converted);
    constexpr std::uint32_t expected_frames = (6'009U * 44'100U + 24'000U) / 48'000U;
    require(bridge.exports()->export_status().contains(QStringLiteral("VST3 instrument WAV")) &&
                read_le(pcm, 20, 2) == 1 && read_le(pcm, 34, 2) == 16 &&
                read_le(pcm, 24, 4) == 44'100 && read_le(pcm, 40, 4) == expected_frames * 4 &&
                pcm.size() == 44 + expected_frames * 4,
            "Quick VST3 PCM16 44.1 kHz export format or tail duration differs: " +
                bridge.exports()->export_status().toStdString());
  } else {
    require(!bridge.exports()->export_busy() && !fs::exists(converted),
            "VST3 conversion without the resampler build published output");
  }
  if (bridge.exports()->encoded_export_available()) {
    const auto mp3 = directory.root() / "master.mp3";
    bridge.exports()->set_export_target(QString::fromStdString(mp3.string()));
    bridge.exports()->set_export_format_index(5);
    bridge.exports()->set_export_rate_index(4);
    bridge.exports()->export_master();
    require(!bridge.exports()->export_busy() &&
                bridge.exports()->export_status().contains(QStringLiteral("48 kHz")) &&
                !fs::exists(mp3),
            "96 kHz VST3 MP3 was not rejected before publication");
    const auto flac = directory.root() / "master.flac";
    bridge.exports()->set_export_target(QString::fromStdString(flac.string()));
    bridge.exports()->set_export_format_index(3);
    bridge.exports()->set_export_rate_index(0);
    bridge.exports()->export_master();
    wait_export(bridge);
    const auto encoded = bytes(flac);
    require(bridge.exports()->export_status().contains(QStringLiteral("VST3 instrument FLAC")) &&
                encoded.size() > 4 && std::memcmp(encoded.data(), "fLaC", 4) == 0,
            "Quick VST3 FLAC export did not publish: " +
                bridge.exports()->export_status().toStdString());
  }
  bridge.exports()->set_export_target(QString::fromStdString(master.string()));
  bridge.exports()->set_export_format_index(0);
  bridge.exports()->set_export_rate_index(0);
  bridge.exports()->export_master();
  wait_export(bridge);
  require(bridge.exports()->export_status().contains(QStringLiteral("exported")) &&
              fs::exists(master),
          "Quick instrument export did not publish: " +
              bridge.exports()->export_status().toStdString());
  const auto first = bytes(master);
  require(first.size() == 56 + 6009 * 8, "WAV duration omitted eight-frame tail");
  constexpr float level = 0.5F * 100.0F / 127.0F;
  near(sample(first, 0), level);
  near(sample(first, 5999), level);
  near(sample(first, 6000), level);
  near(sample(first, 6007), level / 8.0F);
  near(sample(first, 6008), 0.0F);

  require(::setenv("OSCILLATE_VST3_INSTRUMENT_TAIL", "infinite", 1) == 0,
          "unknown tail mode failed");
  bridge.exports()->export_master();
  wait_export(bridge);
  require(bridge.exports()->export_status().contains(QStringLiteral("tail")) &&
              bytes(master) == first,
          "unknown tail replaced valid master");
  require(::setenv("OSCILLATE_VST3_INSTRUMENT_TAIL", "8", 1) == 0, "tail reset failed");
  require(::setenv("OSCILLATE_VST3_INSTRUMENT_FAIL", "process", 1) == 0,
          "host failure mode failed");
  bridge.exports()->export_master();
  wait_export(bridge);
  require(bridge.exports()->export_status().contains(QStringLiteral("failed")) &&
              bytes(master) == first,
          "host failure replaced valid master");
  require(::unsetenv("OSCILLATE_VST3_INSTRUMENT_FAIL") == 0, "host mode reset failed");

  const auto binary = bundle / "Contents" / "x86_64-linux" / "TimedInstrument.so";
  std::ofstream changed(binary, std::ios::binary | std::ios::app);
  changed.put('\0');
  changed.close();
  bridge.exports()->export_master();
  wait_export(bridge);
  require(bridge.exports()->export_status().contains(QStringLiteral("binary differs")) &&
              bytes(master) == first,
          "changed plugin replaced valid master");
  fs::remove_all(bundle);
  bridge.exports()->export_master();
  wait_export(bridge);
  require(bridge.exports()->export_status().contains(QStringLiteral("scan failed")) &&
              bytes(master) == first,
          "missing plugin replaced valid master");
#else
  bridge.exports()->export_master();
  require(!bridge.exports()->export_busy() &&
              bridge.exports()->export_status().contains(QStringLiteral("scanner-enabled")) &&
              !fs::exists(master),
          "scanner-disabled export did not fail closed");
#endif
}

#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
void two_instrument_source_export_checks() {
  Directory directory;
  const auto project_root = directory.root() / "two-sources";
  const auto first_bundle = directory.root() / "first" / "TimedInstrument.vst3";
  const auto second_bundle = directory.root() / "second" / "TimedInstrument.vst3";
  fs::create_directories(first_bundle.parent_path());
  fs::create_directories(second_bundle.parent_path());
  fs::copy(VST3_INSTRUMENT_BUNDLE, first_bundle, fs::copy_options::recursive);
  fs::copy(VST3_INSTRUMENT_BUNDLE, second_bundle, fs::copy_options::recursive);
  oscillate::plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto first_scan = scanner.scan(first_bundle);
  const auto second_scan = scanner.scan(second_bundle);
  require(first_scan.ok() && second_scan.ok() && first_scan.metadata.classes.size() == 1 &&
              second_scan.metadata.classes.size() == 1,
          "two-source fixture scan failed");
  const auto identity = [](const auto& scanned) {
    const auto& klass = scanned.metadata.classes.front();
    return app::Vst3InsertIdentity{klass.id,
                                   klass.vendor,
                                   klass.name,
                                   klass.version,
                                   scanned.binary.canonical_bundle.string(),
                                   scanned.binary.sha256};
  };
  auto session = take(app::ProjectSession::create(project_root));
  const auto first_track =
      take(session->instruments().create_vst3_instrument_track(identity(first_scan)));
  const auto second_track =
      take(session->instruments().create_vst3_instrument_track(identity(second_scan)));
  require(first_track != second_track, "two source tracks share an identity");
  for (const auto track : {first_track, second_track}) {
    require(session->midi_clips().add_midi_clip(track, {0}).has_value(), "source MIDI clip failed");
    const auto& tracks = session->snapshot().tracks;
    const auto found = std::ranges::find(tracks, track, &project::Track::id);
    require(found != tracks.end() && !found->midi_clips.empty(), "source clip missing");
    require(session->notes().add_note(found->midi_clips.front().id, {0}, 60).has_value(),
            "source MIDI note failed");
  }
  require(session->tracks().set_track_level_pan(second_track, 0.25, 0.0).has_value() &&
              session->saving().save().has_value(),
          "two-source state did not save");
  session.reset();
  session = take(app::ProjectSession::open(project_root));
  require(session->snapshot().tracks.size() == 2 &&
              std::holds_alternative<project::Vst3PluginInstance>(
                  session->snapshot().tracks.front().instrument_source) &&
              std::holds_alternative<project::Vst3PluginInstance>(
                  session->snapshot().tracks.back().instrument_source),
          "two independently saved sources did not reopen");
  quick::ProjectBridge bridge(directory.root(), nullptr, {}, [] {
    return foundation::Result<std::vector<oscillate::audio_io::PipeWireSink>>::success({});
  });
  bridge.set_project(std::move(session), project_root);
  QQmlEngine qml_engine;
  QQmlComponent export_component(&qml_engine, QUrl(QStringLiteral("qrc:/ExportPanel.qml")));
  require(export_component.isReady(), export_component.errorString().toStdString());
  std::unique_ptr<QObject> export_panel(export_component.createWithInitialProperties(
      {{"bridge", QVariant::fromValue<QObject*>(&bridge)},
       {"canvas", QColor("#101820")},
       {"panelRaised", QColor("#26313b")},
       {"line", QColor("#66727f")},
       {"textMain", QColor("#eeeeee")},
       {"textDim", QColor("#a0a0a0")},
       {"accent", QColor("#50d0a0")},
       {"amber", QColor("#e0a040")}}));
  require(export_panel != nullptr, export_component.errorString().toStdString());
  require(export_panel->property("twoInstrumentSources").toBool() &&
              bridge.exports()->two_instrument_source_stem_available() &&
              bridge.exports()->two_instrument_source_stem_reason().isEmpty() &&
              export_panel->findChild<QObject*>(QStringLiteral("vst3StemSourceSelector")) &&
              export_panel->findChild<QObject*>(QStringLiteral("vst3StemExportButton")),
          "two-source QML stem selector or action is missing");
  require(::setenv("OSCILLATE_VST3_INSTRUMENT_TAIL", "8", 1) == 0, "two-source tail mode failed");
  const auto master_path = directory.root() / "two-master.wav";
  bridge.exports()->set_export_target(QString::fromStdString(master_path.string()));
  bridge.exports()->export_master();
  wait_export(bridge);
  require(bridge.exports()->export_status().contains(QStringLiteral("exported")) &&
              fs::exists(master_path),
          "Quick two-source master failed: " + bridge.exports()->export_status().toStdString());
  const auto master = bytes(master_path);
  require(master.size() == 56 + 6009 * 8, "two-source master tail was truncated");
  constexpr float one_source = 0.5F * 100.0F / 127.0F;
  near(sample(master, 0), one_source * 1.25F);
  near(sample(master, 6007), one_source * 1.25F / 8.0F);

  bridge.exports()->export_stems();
  require(!bridge.exports()->export_busy() &&
              bridge.exports()->export_status().contains(
                  QStringLiteral("Choose an instrument source track")) &&
              !fs::exists(project_root / ("stems-rev-" + std::to_string(bridge.revision()))),
          "ambiguous folder stem command silently accepted two VST3 sources");
  const auto first_stem_path = directory.root() / "first-stem.wav";
  bridge.exports()->set_export_target(QString::fromStdString(first_stem_path.string()));
  require(QMetaObject::invokeMethod(export_panel.get(), "exportSelectedStem"),
          "QML source-stem action is unavailable");
  wait_export(bridge);
  require(bridge.exports()->export_status().contains(QStringLiteral("exported")) &&
              fs::exists(first_stem_path),
          "Quick first source stem failed: " + bridge.exports()->export_status().toStdString());
  const auto first_stem = bytes(first_stem_path);
  const auto second_stem_path = directory.root() / "second-stem.wav";
  bridge.exports()->set_export_target(QString::fromStdString(second_stem_path.string()));
  require(export_panel->setProperty("selectedStemIndex", 1) &&
              QMetaObject::invokeMethod(export_panel.get(), "exportSelectedStem"),
          "QML second source selection or export action is unavailable");
  wait_export(bridge);
  require(bridge.exports()->export_status().contains(QStringLiteral("exported")) &&
              fs::exists(second_stem_path),
          "Quick second source stem failed: " + bridge.exports()->export_status().toStdString());
  const auto second_stem = bytes(second_stem_path);
  require(first_stem.size() == master.size() && second_stem.size() == master.size(),
          "source stems are not aligned to their master");
  near(sample(first_stem, 0), one_source);
  near(sample(second_stem, 0), one_source * 0.25F);
  near(sample(master, 6007), sample(first_stem, 6007) + sample(second_stem, 6007));

  bridge.exports()->set_export_target(
      QString::fromStdString((directory.root() / "stem.flac").string()));
  bridge.exports()->export_instrument_source_stem(QString::fromStdString(second_track.to_string()));
  require(!bridge.exports()->export_busy() &&
              bridge.exports()->export_status().contains(QStringLiteral(".wav")) &&
              !fs::exists(directory.root() / "stem.flac") && bytes(second_stem_path) == second_stem,
          "selected VST3 source stem accepted a non-WAV target");
  bridge.exports()->export_instrument_source_stem(QStringLiteral("absent"));
  require(!bridge.exports()->export_busy() &&
              bridge.exports()->export_status().contains(QStringLiteral("Choose a saved")) &&
              bytes(second_stem_path) == second_stem,
          "unknown source selection replaced a valid stem");
  bridge.exports()->set_export_target(QString::fromStdString(master_path.string()));
  const auto second_binary = second_bundle / "Contents" / "x86_64-linux" / "TimedInstrument.so";
  std::ofstream changed(second_binary, std::ios::binary | std::ios::app);
  changed.put('\0');
  changed.close();
  bridge.exports()->export_master();
  wait_export(bridge);
  require(bridge.exports()->export_status().contains(QStringLiteral("binary differs")) &&
              bytes(master_path) == master && bytes(second_stem_path) == second_stem,
          "changed second source binary replaced valid outputs");
  bridge.exports()->set_export_target(QString::fromStdString(second_stem_path.string()));
  bridge.exports()->export_instrument_source_stem(QString::fromStdString(second_track.to_string()));
  wait_export(bridge);
  require(bridge.exports()->export_status().contains(QStringLiteral("binary differs")) &&
              bytes(second_stem_path) == second_stem && bytes(master_path) == master,
          "changed second source binary replaced a valid selected stem");
  bridge.add_effect_engine(QString::fromStdString(first_track.to_string()), 0,
                           QStringLiteral("shape"));
  require(
      !bridge.exports()->two_instrument_source_stem_available() &&
          bridge.exports()->two_instrument_source_stem_reason().contains(QStringLiteral("bare")) &&
          export_panel->property("twoInstrumentSources").toBool() &&
          !export_panel->findChild<QObject*>(QStringLiteral("vst3StemExportButton"))
               ->property("activeFocusOnTab")
               .toBool(),
      "non-bare source was shown as ready for two-source stem export");
  bridge.exports()->export_instrument_source_stem(QString::fromStdString(first_track.to_string()));
  require(!bridge.exports()->export_busy() &&
              bridge.exports()->export_status().contains(QStringLiteral("bare")) &&
              bytes(second_stem_path) == second_stem && bytes(master_path) == master,
          "non-bare source stem reached the worker or changed prior output");
  require(::unsetenv("OSCILLATE_VST3_INSTRUMENT_TAIL") == 0, "two-source tail mode reset failed");
}

void paired_instrument_effect_export_checks() {
  Directory directory;
  const auto root = directory.root() / "paired-instrument-effects";
  oscillate::plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto instrument_scan = scanner.scan(VST3_INSTRUMENT_BUNDLE);
  const auto effect_scan = scanner.scan(VST3_GAIN_BUNDLE);
  require(instrument_scan.ok() && instrument_scan.metadata.classes.size() == 1 &&
              effect_scan.ok() && effect_scan.metadata.classes.size() == 1,
          "paired Quick export fixtures did not scan");
  const auto identity = [](const auto& scanned) {
    const auto& klass = scanned.metadata.classes.front();
    return app::Vst3InsertIdentity{klass.id,
                                   klass.vendor,
                                   klass.name,
                                   klass.version,
                                   scanned.binary.canonical_bundle.string(),
                                   scanned.binary.sha256};
  };
  auto session = take(app::ProjectSession::create(root));
  for (int index = 0; index < 2; ++index) {
    const auto track =
        take(session->instruments().create_vst3_instrument_track(identity(instrument_scan)));
    require(session->midi_clips().add_midi_clip(track, {0}).has_value(),
            "paired Quick export MIDI clip failed");
    const auto clip = session->snapshot().tracks.back().midi_clips.front().id;
    require(session->notes().add_note(clip, {0}, 60).has_value() &&
                session->effects().insert_vst3_plugin(track, 0, identity(effect_scan)).has_value(),
            "paired Quick export note or effect failed");
  }
  require(session->saving().save().has_value(), "paired Quick export project save failed");
  session.reset();
  session = take(app::ProjectSession::open(root));
  const auto first_track = session->snapshot().tracks.front().id;
  quick::ProjectBridge bridge(directory.root(), nullptr, {}, [] {
    return foundation::Result<std::vector<oscillate::audio_io::PipeWireSink>>::success({});
  });
  bridge.set_project(std::move(session), root);
  require(bridge.exports()->two_instrument_source_stem_available() &&
              bridge.exports()->two_instrument_source_stem_reason().isEmpty(),
          "paired source/effect layout is not offered for selected stem export");
  require(::setenv("OSCILLATE_VST3_INSTRUMENT_TAIL", "8", 1) == 0,
          "paired Quick export tail mode failed");
  const auto stem_path = directory.root() / "paired-first-stem.wav";
  bridge.exports()->set_export_target(QString::fromStdString(stem_path.string()));
  bridge.exports()->export_instrument_source_stem(QString::fromStdString(first_track.to_string()));
  wait_export(bridge);
  require(bridge.exports()->export_status().contains(QStringLiteral("exported")) &&
              fs::exists(stem_path),
          "paired Quick selected stem failed: " + bridge.exports()->export_status().toStdString());
  const auto master_path = directory.root() / "paired-master.wav";
  bridge.exports()->set_export_target(QString::fromStdString(master_path.string()));
  bridge.exports()->export_master();
  wait_export(bridge);
  require(bridge.exports()->export_status().contains(QStringLiteral("exported")) &&
              fs::exists(master_path),
          "paired Quick master failed: " + bridge.exports()->export_status().toStdString());
  const auto stem = bytes(stem_path);
  const auto master = bytes(master_path);
  constexpr float one_source = 0.5F * 100.0F / 127.0F;
  require(stem.size() == 56 + 6009 * 8 && master.size() == stem.size(),
          "paired Quick render lost its release tail");
  near(sample(stem, 0), one_source * 2.0F);
  near(sample(master, 0), one_source * 4.0F);
  near(sample(master, 6007), sample(stem, 6007) * 2.0F);
  bridge.add_effect_engine(QString::fromStdString(first_track.to_string()), 1,
                           QStringLiteral("shape"));
  require(
      !bridge.exports()->two_instrument_source_stem_available() &&
          bridge.exports()->two_instrument_source_stem_reason().contains(QStringLiteral("bare")),
      "an extra stage was shown as ready for paired selected-stem export");
  require(::unsetenv("OSCILLATE_VST3_INSTRUMENT_TAIL") == 0,
          "paired Quick export tail mode reset failed");
}

void native_and_vst3_source_export_panel_checks() {
  Directory directory;
  const auto root = directory.root() / "native-vst3";
  oscillate::plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto scanned = scanner.scan(VST3_INSTRUMENT_BUNDLE);
  require(scanned.ok() && scanned.metadata.classes.size() == 1, "mixed source fixture scan failed");
  const auto& klass = scanned.metadata.classes.front();
  const app::Vst3InsertIdentity identity{klass.id,
                                         klass.vendor,
                                         klass.name,
                                         klass.version,
                                         scanned.binary.canonical_bundle.string(),
                                         scanned.binary.sha256};
  auto session = take(app::ProjectSession::create(root));
  require(session->tracks().add_instrument_track() && session->tracks().add_instrument_track(),
          "mixed source tracks could not be created");
  const auto native_track = session->snapshot().tracks[0].id;
  const auto vst3_track = session->snapshot().tracks[1].id;
  require(session->instruments().select_native_instrument(native_track, "prism") &&
              session->effects().insert_vst3_plugin(vst3_track, 0, identity),
          "mixed source instruments could not be selected");
  for (const auto track : {native_track, vst3_track}) {
    require(session->midi_clips().add_midi_clip(track, {0}).has_value(),
            "mixed source MIDI clip failed");
    const auto found = std::ranges::find(session->snapshot().tracks, track, &project::Track::id);
    require(found != session->snapshot().tracks.end() &&
                session->notes().add_note(found->midi_clips.front().id, {0}, 60).has_value(),
            "mixed source MIDI note failed");
  }
  require(session->saving().save().has_value(), "mixed source project did not save");
  session.reset();
  session = take(app::ProjectSession::open(root));
  quick::ProjectBridge bridge(directory.root(), nullptr, {}, [] {
    return foundation::Result<std::vector<oscillate::audio_io::PipeWireSink>>::success({});
  });
  bridge.set_project(std::move(session), root);
  QQmlEngine qml_engine;
  QQmlComponent component(&qml_engine, QUrl(QStringLiteral("qrc:/ExportPanel.qml")));
  require(component.isReady(), component.errorString().toStdString());
  std::unique_ptr<QObject> panel(
      component.createWithInitialProperties({{"bridge", QVariant::fromValue<QObject*>(&bridge)},
                                             {"canvas", QColor("#101820")},
                                             {"panelRaised", QColor("#26313b")},
                                             {"line", QColor("#66727f")},
                                             {"textMain", QColor("#eeeeee")},
                                             {"textDim", QColor("#a0a0a0")},
                                             {"accent", QColor("#50d0a0")},
                                             {"amber", QColor("#e0a040")}}));
  require(panel && panel->property("twoInstrumentSources").toBool() &&
              bridge.exports()->two_instrument_source_stem_available() &&
              bridge.exports()->two_instrument_source_stem_reason().isEmpty(),
          "mixed source selector is missing or disabled");
  const auto stem_path = directory.root() / "prism-stem.wav";
  bridge.exports()->set_export_target(QString::fromStdString(stem_path.string()));
  require(QMetaObject::invokeMethod(panel.get(), "exportSelectedStem"),
          "mixed source QML stem action is unavailable");
  wait_export(bridge);
  require(bridge.exports()->export_status().contains(QStringLiteral("exported")) &&
              fs::exists(stem_path),
          "mixed native stem did not export from Quick");
  const auto stem = bytes(stem_path);
  require(stem.size() > 56 + 256 * 8 &&
              std::ranges::any_of(std::views::iota(std::size_t{32}, std::size_t{256}),
                                  [&stem](std::size_t frame) {
                                    return std::fabs(sample(stem, frame)) > 0.000001F;
                                  }),
          "mixed native stem contains no synth signal");
  const auto master_path = directory.root() / "mixed-master.wav";
  bridge.exports()->set_export_target(QString::fromStdString(master_path.string()));
  bridge.exports()->export_master();
  wait_export(bridge);
  require(bridge.exports()->export_status().contains(QStringLiteral("exported")) &&
              fs::exists(master_path) && bytes(master_path).size() == stem.size(),
          "mixed master did not export or align with selected stem");
  // A master chain is not part of the mixed source stem contract.
  bridge.add_effect_engine(QStringLiteral("master"), 0, QStringLiteral("expanse"));
  require(
      !bridge.exports()->two_instrument_source_stem_available() &&
          bridge.exports()->two_instrument_source_stem_reason().contains(QStringLiteral("master")),
      "a master effect made an unsupported mixed stem appear ready");
  bridge.undo();
  require(bridge.exports()->two_instrument_source_stem_available(),
          "removing the master effect did not restore the mixed stem");
  bridge.add_effect_engine(QString::fromStdString(native_track.to_string()), 0,
                           QStringLiteral("shape"));
  require(
      !bridge.exports()->two_instrument_source_stem_available() &&
          bridge.exports()->two_instrument_source_stem_reason().contains(QStringLiteral("bare")) &&
          panel->property("twoInstrumentSources").toBool(),
      "mixed source effect made an unsupported stem appear ready");
}

void native_vst3_audio_source_export_panel_checks() {
  Directory directory;
  const auto root = directory.root() / "native-vst3-audio";
  oscillate::plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto scanned = scanner.scan(VST3_INSTRUMENT_BUNDLE);
  require(scanned.ok() && scanned.metadata.classes.size() == 1,
          "three-source Quick instrument scan failed");
  const auto& klass = scanned.metadata.classes.front();
  const app::Vst3InsertIdentity identity{klass.id,
                                         klass.vendor,
                                         klass.name,
                                         klass.version,
                                         scanned.binary.canonical_bundle.string(),
                                         scanned.binary.sha256};
  auto session = take(app::ProjectSession::create(root));
  require(session->tracks().add_instrument_track() && session->tracks().add_instrument_track(),
          "three-source Quick instrument tracks failed");
  const auto native_track = session->snapshot().tracks[0].id;
  const auto vst3_track = session->snapshot().tracks[1].id;
  require(session->instruments().select_native_instrument(native_track, "prism") &&
              session->effects().insert_vst3_plugin(vst3_track, 0, identity),
          "three-source Quick instruments failed");
  for (const auto track : {native_track, vst3_track}) {
    require(session->midi_clips().add_midi_clip(track, {0}).has_value(),
            "three-source Quick MIDI clip failed");
    const auto found = std::ranges::find(session->snapshot().tracks, track, &project::Track::id);
    require(found != session->snapshot().tracks.end() &&
                session->notes().add_note(found->midi_clips.front().id, {0}, 60),
            "three-source Quick MIDI note failed");
  }
  const auto source = make_source_wav(directory.root());
  const auto fd = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "three-source Quick audio open failed");
  const auto audio_track = session->imports().import_wav_audio(fd);
  (void)::close(fd);
  require(audio_track && session->saving().save(), "three-source Quick audio save failed");
  session.reset();
  session = take(app::ProjectSession::open(root));

  quick::ProjectBridge bridge(directory.root(), nullptr, {}, [] {
    return foundation::Result<std::vector<oscillate::audio_io::PipeWireSink>>::success({});
  });
  bridge.set_project(std::move(session), root);
  QQmlEngine qml_engine;
  QQmlComponent component(&qml_engine, QUrl(QStringLiteral("qrc:/ExportPanel.qml")));
  require(component.isReady(), component.errorString().toStdString());
  std::unique_ptr<QObject> panel(
      component.createWithInitialProperties({{"bridge", QVariant::fromValue<QObject*>(&bridge)},
                                             {"canvas", QColor("#101820")},
                                             {"panelRaised", QColor("#26313b")},
                                             {"line", QColor("#66727f")},
                                             {"textMain", QColor("#eeeeee")},
                                             {"textDim", QColor("#a0a0a0")},
                                             {"accent", QColor("#50d0a0")},
                                             {"amber", QColor("#e0a040")}}));
  require(panel && panel->property("twoInstrumentSources").toBool() &&
              panel->property("stemSourceTracks").toList().size() == 3 &&
              bridge.exports()->two_instrument_source_stem_available(),
          "three-source Quick stem selector is unavailable");

  std::array<std::vector<std::uint8_t>, 3> stems;
  for (std::size_t index = 0; index < stems.size(); ++index) {
    const auto target = directory.root() / ("three-stem-" + std::to_string(index) + ".wav");
    panel->setProperty("selectedStemIndex", static_cast<int>(index));
    bridge.exports()->set_export_target(QString::fromStdString(target.string()));
    require(QMetaObject::invokeMethod(panel.get(), "exportSelectedStem"),
            "three-source Quick selected stem action failed");
    wait_export(bridge);
    require(bridge.exports()->export_status().contains(QStringLiteral("exported")) &&
                fs::exists(target),
            "three-source Quick selected stem failed: " +
                bridge.exports()->export_status().toStdString());
    stems[index] = bytes(target);
  }
  const auto master_path = directory.root() / "three-master.wav";
  bridge.exports()->set_export_target(QString::fromStdString(master_path.string()));
  bridge.exports()->export_master();
  wait_export(bridge);
  require(bridge.exports()->export_status().contains(QStringLiteral("exported")) &&
              fs::exists(master_path),
          "three-source Quick master export failed");
  const auto master = bytes(master_path);
  require(master.size() == stems[0].size() && master.size() == stems[1].size() &&
              master.size() == stems[2].size(),
          "three-source Quick stems lost common extent");
  bool native_audible = false;
  bool audio_audible = false;
  for (std::size_t frame = 0; frame < (master.size() - 56) / 8; ++frame) {
    native_audible |= std::fabs(sample(stems[0], frame)) > 0.0001F;
    audio_audible |= std::fabs(sample(stems[2], frame)) > 0.0001F;
    near(sample(master, frame),
         sample(stems[0], frame) + sample(stems[1], frame) + sample(stems[2], frame));
  }
  require(native_audible && audio_audible, "three-source Quick stem source is silent");
}

void mixed_export_checks() {
  Directory directory;
  const auto project_root = directory.root() / "mixed-project";
  const auto bundle = directory.root() / "TimedInstrument.vst3";
  fs::copy(VST3_INSTRUMENT_BUNDLE, bundle, fs::copy_options::recursive);
  oscillate::plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto scanned = scanner.scan(bundle);
  require(scanned.ok() && scanned.metadata.classes.size() == 1, "mixed fixture scan failed");
  const auto& klass = scanned.metadata.classes.front();
  auto session = take(app::ProjectSession::create(project_root));
  require(session->tracks().add_instrument_track().has_value(), "mixed instrument track failed");
  const auto track = session->snapshot().tracks.front().id;
  require(session->midi_clips().add_midi_clip(track, {0}).has_value(), "mixed MIDI clip failed");
  const auto clip = session->snapshot().tracks.front().midi_clips.front().id;
  require(session->notes().add_note(clip, {0}, 60).has_value(), "mixed MIDI note failed");
  app::Vst3InsertIdentity identity{klass.id,
                                   klass.vendor,
                                   klass.name,
                                   klass.version,
                                   scanned.binary.canonical_bundle.string(),
                                   scanned.binary.sha256};
  require(session->effects().insert_vst3_plugin(track, 0, std::move(identity)).has_value(),
          "mixed VST3 insert failed");
  const auto source = make_source_wav(directory.root());
  const auto fd = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "mixed source open failed");
  auto imported = session->imports().import_wav_audio(fd);
  (void)::close(fd);
  require(imported.has_value(), "mixed source import failed");
  const auto effect_bundle = directory.root() / "LoadedGain.vst3";
  fs::copy(VST3_GAIN_BUNDLE, effect_bundle, fs::copy_options::recursive);
  const auto effect_scan = scanner.scan(effect_bundle);
  require(effect_scan.ok() && effect_scan.metadata.classes.size() == 1,
          "mixed effect fixture scan failed");
  const auto& effect_class = effect_scan.metadata.classes.front();
  app::Vst3InsertIdentity effect_identity{effect_class.id,
                                          effect_class.vendor,
                                          effect_class.name,
                                          effect_class.version,
                                          effect_scan.binary.canonical_bundle.string(),
                                          effect_scan.binary.sha256};
  require(session->effects()
              .insert_vst3_plugin(imported.value(), 0, std::move(effect_identity))
              .has_value(),
          "mixed audio effect insert failed");
  require(session->saving().save().has_value(), "mixed save failed");
  session.reset();
  session = take(app::ProjectSession::open(project_root));
  require(session->snapshot().sources.size() == 1 && session->snapshot().tracks.size() == 2 &&
              session->snapshot().tracks.back().effects.size() == 1,
          "mixed saved project did not reopen");
  {
    const auto assets = project_root / "assets";
    const auto displaced = project_root / "assets-displaced";
    fs::rename(assets, displaced);
    auto unavailable = session->exports().prepare_stopped_snapshot_export();
    fs::rename(displaced, assets);
    require(!unavailable, "missing managed directory did not reject export preparation");
    auto playback = take(session->reserve_playback_start());
    playback.finish_after_join();
  }
  {
    auto stale_task = take(session->exports().prepare_stopped_snapshot_export());
    auto altered = stale_task.snapshot();
    ++altered.revision;
    const auto stale_target = directory.root() / "mixed-stale.wav";
    const auto& reader = stale_task.reader();
    auto authority = std::move(stale_task).take_authority();
    auto stale = exporting::render_vst3_master(altered, reader, std::move(authority), scanner,
                                               exporting::MasterRenderOptions{stale_target});
    require(!stale && stale.error().code == foundation::ErrorCode::conflict &&
                !fs::exists(stale_target),
            "stale mixed authority published output");
  }

  // The worker capability must keep the exact saved revision and managed reader
  // usable after the control-side session closes.
  auto detached = take(session->exports().prepare_stopped_snapshot_export());
  const auto revision = detached.snapshot().revision;
  session.reset();
  const auto detached_target = directory.root() / "detached.wav";
  require(::setenv("OSCILLATE_VST3_INSTRUMENT_TAIL", "8", 1) == 0, "mixed tail mode failed");
  const auto& detached_snapshot = detached.snapshot();
  const auto& detached_reader = detached.reader();
  auto authority = std::move(detached).take_authority();
  auto detached_result =
      exporting::render_vst3_master(detached_snapshot, detached_reader, std::move(authority),
                                    scanner, exporting::MasterRenderOptions{detached_target});
  require(detached_result.has_value() && fs::exists(detached_target),
          "detached mixed task lost its saved reader");
  session = take(app::ProjectSession::open(project_root));
  require(session->snapshot().revision == revision, "mixed revision changed on reopen");
  const auto source_hash = session->snapshot().sources.front().sha256;

  quick::ProjectBridge bridge(directory.root(), nullptr, {}, [] {
    return foundation::Result<std::vector<oscillate::audio_io::PipeWireSink>>::success({});
  });
  bridge.set_project(std::move(session), project_root);
  const auto master = directory.root() / "mixed-master.wav";
  bridge.exports()->set_export_target(QString::fromStdString(master.string()));
  bridge.exports()->export_master();
  wait_export(bridge);
  require(bridge.exports()->export_status().contains(QStringLiteral("exported")) &&
              fs::exists(master),
          "Quick mixed export failed: " + bridge.exports()->export_status().toStdString());
  const auto rendered = bytes(master);
  require(rendered == bytes(detached_target), "mixed task and Quick export differ");
  require(rendered.size() == 56 + 6009 * 8, "mixed render omitted instrument tail");
  constexpr float instrument = 0.5F * 100.0F / 127.0F;
  near(sample(rendered, 0), instrument + 0.5F);
  near(sample(rendered, 5999), instrument + 0.5F);
  near(sample(rendered, 6000), instrument);
  near(sample(rendered, 6007), instrument / 8.0F);
  near(sample(rendered, 6008), 0.0F);

  const auto saved_source = project_root / "assets" / (source_hash + ".wav");
  { // Corrupt the managed copy before the next export; verification must fail.
    std::ofstream damaged(saved_source, std::ios::binary | std::ios::app);
    damaged.put('\0');
  }
  bridge.exports()->export_master();
  wait_export(bridge);
  require(bridge.exports()->export_status().contains(QStringLiteral("failed")) &&
              bytes(master) == rendered,
          "changed managed source replaced valid mixed master");
}

void effect_export_checks() {
  Directory directory;
  const auto project_root = directory.root() / "effect-project";
  const auto bundle = directory.root() / "LoadedGain.vst3";
  fs::copy(VST3_GAIN_BUNDLE, bundle, fs::copy_options::recursive);
  oscillate::plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto scanned = scanner.scan(bundle);
  require(scanned.ok() && scanned.metadata.classes.size() == 1, "effect fixture scan failed");
  const auto& klass = scanned.metadata.classes.front();
  auto session = take(app::ProjectSession::create(project_root));
  const auto source = make_source_wav(directory.root());
  const auto fd = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "effect source open failed");
  auto imported = session->imports().import_wav_audio(fd);
  (void)::close(fd);
  require(imported.has_value() && session->snapshot().tracks.size() == 1,
          "effect source import failed");
  const auto track = session->snapshot().tracks.front().id;
  app::Vst3InsertIdentity identity{klass.id,
                                   klass.vendor,
                                   klass.name,
                                   klass.version,
                                   scanned.binary.canonical_bundle.string(),
                                   scanned.binary.sha256};
  require(session->effects().insert_vst3_plugin(track, 0, std::move(identity)).has_value(),
          "effect insert failed");
  require(session->saving().save().has_value(), "effect project save failed");
  session.reset();
  session = take(app::ProjectSession::open(project_root));
  require(session->snapshot().tracks.front().effects.size() == 1, "effect insert did not reopen");
  quick::ProjectBridge bridge(directory.root(), nullptr, {}, [] {
    return foundation::Result<std::vector<oscillate::audio_io::PipeWireSink>>::success({});
  });
  bridge.set_project(std::move(session), project_root);
  const auto master = directory.root() / "effect-master.wav";
  bridge.exports()->set_export_target(QString::fromStdString(master.string()));
  // PCM24 at 96 kHz: the effect still runs at the 48 kHz project rate.
  const auto converted = directory.root() / "effect-pcm24.wav";
  bridge.exports()->set_export_target(QString::fromStdString(converted.string()));
  bridge.exports()->set_export_format_index(1);
  bridge.exports()->set_export_rate_index(4);
  bridge.exports()->export_master();
  if (bridge.exports()->output_rate_conversion_available()) {
    wait_export(bridge);
    const auto pcm = bytes(converted);
    require(bridge.exports()->export_status().contains(QStringLiteral("VST3 effect WAV")) &&
                read_le(pcm, 20, 2) == 1 && read_le(pcm, 34, 2) == 24 &&
                read_le(pcm, 24, 4) == 96'000 && pcm.size() == 44 + 12'000 * 6,
            "Quick VST3 effect PCM24 96 kHz export differs: " +
                bridge.exports()->export_status().toStdString());
    // Converted plateau level is timing/level evidence independent of SRC.
    require(std::fabs(pcm24_left(pcm, 6'000) - 0.5F) < 0.01F,
            "converted effect plateau level differs");
  } else {
    require(!bridge.exports()->export_busy() && !fs::exists(converted),
            "VST3 conversion without the resampler build published output");
  }
  bridge.exports()->set_export_target(QString::fromStdString(master.string()));
  bridge.exports()->set_export_format_index(0);
  bridge.exports()->set_export_rate_index(0);
  bridge.exports()->export_master();
  wait_export(bridge);
  require(bridge.exports()->export_status().contains(QStringLiteral("exported")) &&
              fs::exists(master),
          "Quick effect export failed: " + bridge.exports()->export_status().toStdString());
  const auto rendered = bytes(master);
  require(rendered.size() == 56 + 6000 * 8, "effect export shifted project duration");
  near(sample(rendered, 0), 0.5F);
  near(sample(rendered, 5999), 0.5F);
}
#endif
} // namespace

int main(int argc, char** argv) {
  QGuiApplication application(argc, argv);
  QGuiApplication::setOrganizationName(QStringLiteral("OscillateTest"));
  QGuiApplication::setApplicationName(QStringLiteral("QuickVst3Export"));
  try {
    export_checks();
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
    two_instrument_source_export_checks();
    paired_instrument_effect_export_checks();
    native_and_vst3_source_export_panel_checks();
    native_vst3_audio_source_export_panel_checks();
    mixed_export_checks();
    effect_export_checks();
#endif
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Quick VST3 export: %s\n", error.what());
    return 1;
  }
}
