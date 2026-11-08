// One row of the VST3 compatibility matrix: scan, load, render, state,
// editor, project save/reopen and offline export for one plugin class.
// The Python runner (qualify_vst3_matrix.py) runs every class in its own
// process, so a crash costs only that class's remaining steps; the evidence
// file is rewritten after every step for the same reason.

#include "allocation_probe.hpp"

#include "oscillate/application/project_session.hpp"
#include "oscillate/application/vst3/offline_export.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/plugins/vst3_bundle_loader.hpp"
#include "oscillate/plugins/vst3_scan.hpp"

#if defined(OSCILLATE_MATRIX_EDITOR)
#include "vst3_editor_attachment.hpp"

#include <QApplication>
#include <QElapsedTimer>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace {
namespace app = oscillate::application;
namespace exporting = oscillate::exporting;
namespace persistence = oscillate::persistence;
namespace plugins = oscillate::plugins;
namespace project = oscillate::project;
namespace timeline = oscillate::timeline;
namespace fs = std::filesystem;

constexpr double sample_rate = 48'000.0;
constexpr std::uint32_t integer_sample_rate = 48'000;
constexpr std::uint32_t max_block_frames = 128;
constexpr std::array<std::uint32_t, 2> block_sizes{64, 128};
constexpr std::uint32_t render_seconds = 2;
constexpr std::uint32_t restored_render_blocks = 128;
constexpr std::uint32_t chord_period_frames = integer_sample_rate / 2;
constexpr std::array<std::uint8_t, 3> chord_pitches{60, 64, 67};
constexpr float note_velocity = 0.8F;
constexpr double tone_hz = 440.0;
constexpr float tone_amplitude = 0.25F;
constexpr float audible_peak = 0.00001F;
constexpr std::int64_t project_notes = 8;
constexpr std::uint8_t project_pitch = 60;
constexpr std::uint16_t pcm16_full_scale = 32'767;
constexpr std::uint32_t wav_channels = 2;
constexpr std::uint32_t wav_bytes_per_sample = 2;
constexpr std::uint32_t wav_header_bytes = 44;
constexpr std::uint32_t riff_chunk_header_bytes = 8;
constexpr std::uint32_t wav_format_chunk_bytes = 16;
constexpr std::uint16_t wav_pcm_format = 1;
constexpr std::uint32_t wav_bits_per_sample = 16;
constexpr std::size_t float_bytes = 4;
constexpr std::size_t fourcc_bytes = 4;
constexpr std::size_t riff_header_bytes = 12;
constexpr double quarters_per_second = 2.0; // The host transport's default 120 BPM.
constexpr double microseconds_per_second = 1'000'000.0;
constexpr auto editor_settle = std::chrono::milliseconds(750);
constexpr std::uint32_t editor_event_slice_ms = 10;

std::string json_string(std::string_view text) {
  std::string out = "\"";
  for (const char c : text) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        constexpr std::string_view hex = "0123456789abcdef";
        out += "\\u00";
        out += hex[static_cast<unsigned char>(c) >> 4U];
        out += hex[static_cast<unsigned char>(c) & 0x0fU];
      } else {
        out += c;
      }
    }
  }
  return out + "\"";
}

std::string json_number(double value) {
  if (!std::isfinite(value))
    return "null";
  std::ostringstream out;
  out.precision(6);
  out << value;
  return out.str();
}

// Ordered JSON object of already-encoded values.
class Fields {
public:
  Fields& add(std::string_view key, std::string value) {
    entries_.emplace_back(key, std::move(value));
    return *this;
  }
  Fields& text(std::string_view key, std::string_view value) {
    return add(key, json_string(value));
  }
  Fields& number(std::string_view key, double value) { return add(key, json_number(value)); }
  Fields& integer(std::string_view key, std::uint64_t value) {
    return add(key, std::to_string(value));
  }
  Fields& flag(std::string_view key, bool value) { return add(key, value ? "true" : "false"); }
  [[nodiscard]] std::string encode() const {
    std::string out = "{";
    for (std::size_t index = 0; index < entries_.size(); ++index) {
      if (index != 0)
        out += ", ";
      out += json_string(entries_[index].first) + ": " + entries_[index].second;
    }
    return out + "}";
  }

private:
  std::vector<std::pair<std::string, std::string>> entries_;
};

// Rewritten after every step so a crash leaves the steps that finished.
class Evidence {
public:
  explicit Evidence(fs::path path) : path_(std::move(path)) {}
  Fields& header() { return header_; }
  void step(std::string_view name, std::string_view status, std::string_view detail,
            Fields metrics = {}) {
    steps_.add(name, Fields{}
                         .text("status", status)
                         .text("detail", detail)
                         .add("metrics", metrics.encode())
                         .encode());
    write();
  }
  void write() const {
    const auto pending = fs::path(path_.string() + ".pending");
    {
      std::ofstream out(pending, std::ios::trunc);
      out << Fields(header_).add("steps", steps_.encode()).encode() << '\n';
      if (!out)
        throw std::runtime_error("cannot write the evidence file");
    }
    fs::rename(pending, path_);
  }

private:
  fs::path path_;
  Fields header_;
  Fields steps_;
};

plugins::Vst3Scanner make_scanner() { return plugins::Vst3Scanner(VST3_QUALIFICATION_HELPER); }

bool is_instrument(const plugins::Vst3Class& klass) {
  return klass.subcategories.find("Instrument") != std::string::npos;
}

int list_classes(const fs::path& bundle, const fs::path& output) {
  auto scanner = make_scanner();
  const auto started = std::chrono::steady_clock::now();
  const auto scan = scanner.scan(bundle);
  const auto elapsed =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  std::string classes = "[";
  for (const auto& klass : scan.metadata.classes) {
    if (classes.size() > 1)
      classes += ", ";
    classes += Fields{}
                   .text("id", klass.id)
                   .text("name", klass.name)
                   .text("vendor", klass.vendor)
                   .text("version", klass.version)
                   .text("category", klass.category)
                   .text("subcategories", klass.subcategories)
                   .text("kind", is_instrument(klass) ? "instrument" : "effect")
                   .encode();
  }
  classes += "]";
  std::ofstream out(output, std::ios::trunc);
  out << Fields{}
             .flag("ok", scan.ok())
             .text("diagnostic", scan.diagnostic)
             .text("vendor", scan.metadata.vendor)
             .text("binary_sha256", scan.binary.sha256)
             .integer("binary_bytes", scan.binary.byte_size)
             .number("scan_ms", elapsed)
             .add("classes", classes)
             .encode()
      << '\n';
  return out && scan.ok() ? 0 : 1;
}

struct RenderStats {
  std::uint64_t blocks = 0;
  std::uint64_t failed_blocks = 0;
  std::uint64_t nonfinite = 0;
  std::uint64_t allocations = 0;
  float peak = 0.0F;
  double max_block_us = 0.0;
  std::uint32_t restart_flags = 0;
};

// Renders `blocks` blocks of `frames` frames: a repeating chord for an
// instrument, a sine for an effect.
RenderStats render(plugins::Vst3EffectHost& host, bool instrument, std::uint32_t frames,
                   std::uint64_t blocks, std::uint64_t generation) {
  RenderStats stats;
  std::array<float, max_block_frames> in_left{}, in_right{}, out_left{}, out_right{};
  std::array<plugins::Vst3NoteEvent, chord_pitches.size() * 2> notes{};
  std::int32_t next_note_id = 1;
  std::array<std::int32_t, chord_pitches.size()> sounding{};
  const auto before = allocation_probe_count();
  for (std::uint64_t block = 0; block < blocks; ++block) {
    const auto first = block * frames;
    std::size_t note_count = 0;
    if (instrument) {
      const auto onset = ((first + chord_period_frames - 1) / chord_period_frames) *
                         static_cast<std::uint64_t>(chord_period_frames);
      if (onset < first + frames) {
        const auto offset = static_cast<std::uint32_t>(onset - first);
        for (std::size_t voice = 0; voice < chord_pitches.size(); ++voice) {
          if (sounding[voice] != 0)
            notes[note_count++] = {.kind = plugins::Vst3NoteKind::off,
                                   .sample_offset = offset,
                                   .pitch = chord_pitches[voice],
                                   .note_id = sounding[voice]};
        }
        for (std::size_t voice = 0; voice < chord_pitches.size(); ++voice) {
          sounding[voice] = next_note_id++;
          notes[note_count++] = {.kind = plugins::Vst3NoteKind::on,
                                 .sample_offset = offset,
                                 .pitch = chord_pitches[voice],
                                 .velocity = note_velocity,
                                 .note_id = sounding[voice]};
        }
      }
    } else {
      for (std::uint32_t frame = 0; frame < frames; ++frame) {
        const auto phase =
            2.0 * std::numbers::pi * tone_hz * static_cast<double>(first + frame) / sample_rate;
        in_left[frame] = in_right[frame] = tone_amplitude * static_cast<float>(std::sin(phase));
      }
    }
    const auto started = std::chrono::steady_clock::now();
    begin_allocation_probe();
    const auto status =
        instrument ? host.process_instrument(
                         {std::span(out_left).first(frames), std::span(out_right).first(frames)},
                         std::span<const plugins::Vst3NoteEvent>(notes.data(), note_count),
                         {.project_sample = static_cast<std::int64_t>(first),
                          .quarter_position =
                              static_cast<double>(first) / sample_rate * quarters_per_second,
                          .generation = generation})
                   : host.process({.input_left = std::span(in_left).first(frames),
                                   .input_right = std::span(in_right).first(frames),
                                   .output_left = std::span(out_left).first(frames),
                                   .output_right = std::span(out_right).first(frames),
                                   .project_sample = static_cast<std::int64_t>(first)});
    end_allocation_probe();
    const auto elapsed =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started)
            .count();
    stats.max_block_us = std::max(stats.max_block_us, elapsed);
    ++stats.blocks;
    stats.failed_blocks += status == plugins::Vst3ProcessStatus::ok ? 0U : 1U;
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
      for (const float sample : {out_left[frame], out_right[frame]}) {
        if (!std::isfinite(sample))
          ++stats.nonfinite;
        else
          stats.peak = std::max(stats.peak, std::abs(sample));
      }
    }
  }
  stats.allocations = allocation_probe_count() - before;
  stats.restart_flags = host.take_restart_requests();
  return stats;
}

Fields render_metrics(const RenderStats& stats, std::uint32_t frames) {
  const double period_us = static_cast<double>(frames) * microseconds_per_second / sample_rate;
  return Fields{}
      .integer("frames", frames)
      .integer("blocks", stats.blocks)
      .integer("failed_blocks", stats.failed_blocks)
      .integer("nonfinite_samples", stats.nonfinite)
      .integer("callback_allocations", stats.allocations)
      .number("peak", stats.peak)
      .number("max_block_us", stats.max_block_us)
      .number("max_block_period_percent", 100.0 * stats.max_block_us / period_us)
      .integer("restart_flags", stats.restart_flags);
}

// Pass needs clean blocks and audible output; allocation on the callback
// thread is reported as a warning because it is the plug-in's own behaviour.
std::pair<std::string_view, std::string> judge(const RenderStats& stats) {
  if (stats.failed_blocks != 0)
    return {"fail", std::to_string(stats.failed_blocks) + " block(s) failed to process"};
  if (stats.nonfinite != 0)
    return {"fail", std::to_string(stats.nonfinite) + " non-finite sample(s)"};
  if (stats.peak <= audible_peak)
    return {"warn", "output stayed silent"};
  if (stats.allocations != 0)
    return {"warn", std::to_string(stats.allocations) + " allocation(s) on the process thread"};
  return {"pass", "audible, finite output"};
}

std::unique_ptr<plugins::LoadedVst3Effect> load(const plugins::VerifiedVst3Effect& verified,
                                                bool instrument, std::string& error) {
  return instrument
             ? plugins::load_verified_vst3_instrument(verified, sample_rate, max_block_frames,
                                                      error)
             : plugins::load_verified_vst3_effect(verified, sample_rate, max_block_frames, error);
}

std::string_view latency_status(plugins::Vst3LatencyCheckStatus status) {
  switch (status) {
  case plugins::Vst3LatencyCheckStatus::stable:
    return "stable";
  case plugins::Vst3LatencyCheckStatus::changed:
    return "changed";
  case plugins::Vst3LatencyCheckStatus::restart_requested:
    return "restart_requested";
  case plugins::Vst3LatencyCheckStatus::unavailable:
    break;
  }
  return "unavailable";
}

std::string_view editor_probe_name(plugins::Vst3EditorProbe probe) {
  switch (probe) {
  case plugins::Vst3EditorProbe::x11_candidate:
    return "x11_candidate";
  case plugins::Vst3EditorProbe::no_controller:
    return "no_controller";
  case plugins::Vst3EditorProbe::no_view:
    return "no_view";
  case plugins::Vst3EditorProbe::unsupported_platform:
    return "unsupported_platform";
  case plugins::Vst3EditorProbe::wayland_bridge_unavailable:
    return "wayland_bridge_unavailable";
  case plugins::Vst3EditorProbe::host_unavailable:
    return "host_unavailable";
  case plugins::Vst3EditorProbe::plugin_exception:
    break;
  }
  return "plugin_exception";
}

#if defined(OSCILLATE_MATRIX_EDITOR)
std::string_view editor_open_name(oscillate::plugin_ui::EditorOpenStatus status) {
  using oscillate::plugin_ui::EditorOpenStatus;
  switch (status) {
  case EditorOpenStatus::attached:
    return "attached";
  case EditorOpenStatus::native_wayland_unavailable:
    return "native_wayland_unavailable";
  case EditorOpenStatus::no_native_window:
    return "no_native_window";
  case EditorOpenStatus::no_view:
    return "no_view";
  case EditorOpenStatus::no_controller:
    return "no_controller";
  case EditorOpenStatus::unsupported_platform:
    return "unsupported_platform";
  case EditorOpenStatus::host_unavailable:
    return "host_unavailable";
  case EditorOpenStatus::frame_rejected:
    return "frame_rejected";
  case EditorOpenStatus::attach_failed:
    return "attach_failed";
  case EditorOpenStatus::plugin_exception:
    break;
  }
  return "plugin_exception";
}
#endif

void editor_step(Evidence& evidence, plugins::Vst3EffectHost& host, bool open_editor) {
  const auto probe = host.probe_editor(plugins::Vst3EditorPlatform::x11_xembed);
  Fields metrics = Fields{}.text("probe", editor_probe_name(probe));
  if (probe != plugins::Vst3EditorProbe::x11_candidate) {
    evidence.step("editor", "warn", "no X11 editor; the generic parameter editor is used",
                  std::move(metrics));
    return;
  }
#if defined(OSCILLATE_MATRIX_EDITOR)
  if (open_editor) {
    oscillate::plugin_ui::Vst3EditorAttachment attachment;
    const auto opened = attachment.open(host);
    metrics.text("open", editor_open_name(opened));
    if (opened != oscillate::plugin_ui::EditorOpenStatus::attached) {
      evidence.step("editor", "fail", "the X11 editor did not attach", std::move(metrics));
      return;
    }
    attachment.show();
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < editor_settle.count())
      QApplication::processEvents(QEventLoop::AllEvents, editor_event_slice_ms);
    const auto* window = attachment.widget();
    metrics.integer("width", window != nullptr ? static_cast<std::uint64_t>(window->width()) : 0)
        .integer("height", window != nullptr ? static_cast<std::uint64_t>(window->height()) : 0);
    attachment.close();
    QApplication::processEvents();
    evidence.step("editor", "pass", "X11 editor attached, shown and closed", std::move(metrics));
    return;
  }
#endif
  (void)open_editor;
  evidence.step("editor", "skip", "X11 candidate; no display to attach it", std::move(metrics));
}

// A 16-bit stereo sine the effect row imports as its audio clip.
fs::path write_tone(const fs::path& directory) {
  const auto path = directory / "tone.wav";
  const std::uint32_t frames = integer_sample_rate * render_seconds;
  const std::uint32_t data_bytes = frames * wav_channels * wav_bytes_per_sample;
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  const auto u32 = [&out](std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
      out.put(static_cast<char>((value >> shift) & 0xffU));
  };
  const auto u16 = [&out](std::uint16_t value) {
    out.put(static_cast<char>(value & 0xffU));
    out.put(static_cast<char>(value >> 8U));
  };
  out.write("RIFF", static_cast<std::streamsize>(fourcc_bytes));
  u32(wav_header_bytes - riff_chunk_header_bytes + data_bytes);
  out.write("WAVEfmt ", static_cast<std::streamsize>(2 * fourcc_bytes));
  u32(wav_format_chunk_bytes);
  u16(wav_pcm_format);
  u16(wav_channels);
  u32(integer_sample_rate);
  u32(integer_sample_rate * wav_channels * wav_bytes_per_sample);
  u16(wav_channels * wav_bytes_per_sample);
  u16(wav_bits_per_sample);
  out.write("data", static_cast<std::streamsize>(fourcc_bytes));
  u32(data_bytes);
  for (std::uint32_t frame = 0; frame < frames; ++frame) {
    const auto value = static_cast<std::int16_t>(std::lround(
        tone_amplitude * pcm16_full_scale *
        std::sin(2.0 * std::numbers::pi * tone_hz * static_cast<double>(frame) / sample_rate)));
    for (std::uint32_t channel = 0; channel < wav_channels; ++channel)
      u16(static_cast<std::uint16_t>(value));
  }
  if (!out)
    throw std::runtime_error("cannot write the tone WAV");
  return path;
}

template <typename T> T take(oscillate::foundation::Result<T> result, std::string_view what) {
  if (!result)
    throw std::runtime_error(std::string(what) + ": " + result.error().message);
  return std::move(result).value();
}

void take(oscillate::foundation::Result<void> result, std::string_view what) {
  if (!result)
    throw std::runtime_error(std::string(what) + ": " + result.error().message);
}

const project::Vst3PluginInstance* saved_plugin(const project::Snapshot& snapshot) {
  for (const auto& track : snapshot.tracks) {
    if (const auto* source = std::get_if<project::Vst3PluginInstance>(&track.instrument_source))
      return source;
    for (const auto& slot : track.effects) {
      if (const auto* insert = std::get_if<project::Vst3PluginInstance>(&slot.processor))
        return insert;
    }
  }
  return nullptr;
}

class Gate final : public project::TransportGate {
public:
  class Lease final : public project::StoppedTransportLease {};
  oscillate::foundation::Result<std::unique_ptr<project::StoppedTransportLease>>
  acquire_stopped() override {
    return oscillate::foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::success(
        std::make_unique<Lease>());
  }
};

struct WavSummary {
  double seconds = 0.0;
  std::uint64_t nonfinite = 0;
  float peak = 0.0F;
};

// Reads a float32 stereo WAV written by the export.
WavSummary summarise_wav(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  const std::vector<char> bytes{std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>()};
  const auto u32 = [&bytes](std::size_t at) {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + at, sizeof value);
    return value;
  };
  std::size_t at = riff_header_bytes;
  while (at + riff_chunk_header_bytes <= bytes.size() &&
         std::string_view(bytes.data() + at, fourcc_bytes) != "data")
    at += riff_chunk_header_bytes + u32(at + fourcc_bytes);
  if (at + riff_chunk_header_bytes > bytes.size())
    throw std::runtime_error("export WAV has no data chunk");
  const auto size =
      std::min<std::size_t>(u32(at + fourcc_bytes), bytes.size() - at - riff_chunk_header_bytes);
  WavSummary summary;
  const char* data = bytes.data() + at + riff_chunk_header_bytes;
  for (std::size_t offset = 0; offset + float_bytes <= size; offset += float_bytes) {
    float sample = 0.0F;
    std::memcpy(&sample, data + offset, float_bytes);
    if (!std::isfinite(sample))
      ++summary.nonfinite;
    else
      summary.peak = std::max(summary.peak, std::abs(sample));
  }
  summary.seconds =
      static_cast<double>(size / (float_bytes * wav_channels)) / static_cast<double>(sample_rate);
  return summary;
}

app::Vst3InsertIdentity identity_from(const plugins::Vst3Class& klass,
                                      const plugins::ScanResult& scan) {
  return {.class_id = klass.id,
          .vendor = klass.vendor,
          .name = klass.name,
          .version = klass.version,
          .canonical_bundle_path = scan.binary.canonical_bundle.string(),
          .binary_sha256 = scan.binary.sha256};
}

void project_and_export(Evidence& evidence, const plugins::Vst3Class& klass,
                        const plugins::ScanResult& scan, const fs::path& scratch, bool instrument) {
  const auto root = scratch / "project";
  fs::remove_all(root);
  try {
    auto session = take(app::ProjectSession::create(root), "project create");
    if (instrument) {
      const auto track =
          take(session->instruments().create_vst3_instrument_track(identity_from(klass, scan)),
               "instrument track");
      take(session->midi_clips().add_midi_clip(track, {0}), "MIDI clip");
      const auto clip = session->snapshot().tracks.back().midi_clips.back().id;
      for (std::int64_t note = 0; note < project_notes; ++note)
        take(session->notes().add_note(clip, {note * timeline::ticks_per_quarter / 2},
                                       project_pitch),
             "note");
    } else {
      const auto tone = write_tone(scratch);
      const int fd = ::open(tone.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
      if (fd < 0)
        throw std::runtime_error("cannot open the tone WAV");
      auto imported = session->imports().import_wav_audio(fd);
      (void)::close(fd);
      const auto track = take(std::move(imported), "audio import");
      (void)take(session->effects().insert_vst3_plugin(track, 0, identity_from(klass, scan)),
                 "effect insert");
    }
    take(session->saving().save(), "save");
    session.reset();
    session = take(app::ProjectSession::open(root), "reopen");
    const auto* saved = saved_plugin(session->snapshot());
    if (saved == nullptr)
      throw std::runtime_error("the reopened project lost the plug-in");
    if (saved->class_id != klass.id || saved->binary_sha256 != scan.binary.sha256 || saved->missing)
      throw std::runtime_error("the reopened plug-in identity changed");
    evidence.step("project", "pass", "saved and reopened with the same identity",
                  Fields{}.integer("component_state_bytes", saved->component_state.size()));
  } catch (const std::exception& error) {
    evidence.step("project", "fail", error.what());
    evidence.step("export", "skip", "needs the saved project");
    return;
  }
  try {
    auto store = take(persistence::ProjectStore::open_reader(root), "store");
    const auto snapshot = take(store.load_current(), "load").snapshot;
    Gate gate;
    auto authority = take(exporting::StoppedExportAuthority::acquire(snapshot, gate), "authority");
    auto scanner = make_scanner();
    const auto target = scratch / "export.wav";
    fs::remove(target);
    exporting::MasterRenderOptions options{target};
    options.trim_silent_tail = true;
    const auto started = std::chrono::steady_clock::now();
    (void)take(
        exporting::render_vst3_master(snapshot, store, std::move(authority), scanner, options),
        "export");
    const auto elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const auto summary = summarise_wav(target);
    Fields metrics = Fields{}
                         .number("seconds", summary.seconds)
                         .number("render_seconds", elapsed)
                         .number("peak", summary.peak)
                         .integer("nonfinite_samples", summary.nonfinite);
    if (summary.nonfinite != 0)
      evidence.step("export", "fail", "non-finite samples in the export", std::move(metrics));
    else if (summary.peak <= audible_peak)
      evidence.step("export", "warn", "the export is silent", std::move(metrics));
    else
      evidence.step("export", "pass", "offline render written", std::move(metrics));
  } catch (const std::exception& error) {
    evidence.step("export", "fail", error.what());
  }
}

int probe_class(const fs::path& bundle, std::string_view class_id, const fs::path& scratch,
                const fs::path& output, bool open_editor) {
  Evidence evidence(output);
  evidence.header().text("class_id", class_id);
  auto scanner = make_scanner();
  const auto scan = scanner.scan(bundle);
  const auto klass = std::ranges::find(scan.metadata.classes, class_id, &plugins::Vst3Class::id);
  if (!scan.ok() || klass == scan.metadata.classes.end()) {
    evidence.step("scan", "fail", scan.ok() ? "class not in the scan" : scan.diagnostic);
    return 1;
  }
  const bool instrument = is_instrument(*klass);
  evidence.header()
      .text("name", klass->name)
      .text("vendor", klass->vendor)
      .text("version", klass->version)
      .text("kind", instrument ? "instrument" : "effect")
      .text("binary_sha256", scan.binary.sha256);
  evidence.step("scan", "pass", "out-of-process scan");

  const plugins::VerifiedVst3Effect verified{*klass, scan.binary};
  std::string error;
  auto loaded = load(verified, instrument, error);
  if (!loaded || !loaded->host().activate(error)) {
    evidence.step("load", "fail", error.empty() ? "load failed" : error);
    return 1;
  }
  auto& host = loaded->host();
  const auto latency = host.check_prepared_latency();
  const auto tail = host.finite_tail_samples();
  evidence.step("load", "pass", "loaded and activated",
                Fields{}
                    .integer("parameters", host.parameters().size())
                    .integer("latency_samples", host.latency_samples())
                    .text("latency_check", latency_status(latency.status))
                    .add("tail_samples", tail ? std::to_string(*tail) : "null"));

  std::uint64_t generation = 1;
  for (const auto frames : block_sizes) {
    const auto stats = render(
        host, instrument, frames,
        static_cast<std::uint64_t>(render_seconds) * integer_sample_rate / frames, generation++);
    const auto [status, detail] = judge(stats);
    evidence.step("render_" + std::to_string(frames), status, detail,
                  render_metrics(stats, frames));
  }

  host.suspend();
  try {
    auto state = host.save_state_pair(error);
    if (!state)
      throw std::runtime_error(error.empty() ? "state capture failed" : error);
    auto restored = load(verified, instrument, error);
    if (!restored || !restored->host().restore_state_pair(*state, error) ||
        !restored->host().activate(error))
      throw std::runtime_error(error.empty() ? "state restore failed" : error);
    const auto stats =
        render(restored->host(), instrument, max_block_frames, restored_render_blocks, generation);
    restored->host().suspend();
    const auto [status, detail] = judge(stats);
    evidence.step("state", status,
                  status == "pass" ? "restored into a fresh instance and rendered" : detail,
                  Fields{}
                      .integer("component_bytes", state->component_state.size())
                      .add("controller_bytes", state->controller_state
                                                   ? std::to_string(state->controller_state->size())
                                                   : "null"));
  } catch (const std::exception& failure) {
    evidence.step("state", "fail", failure.what());
  }

  try {
    editor_step(evidence, host, open_editor);
  } catch (const std::exception& failure) {
    evidence.step("editor", "fail", failure.what());
  }
  loaded.reset();

  project_and_export(evidence, *klass, scan, scratch, instrument);
  return 0;
}
} // namespace

int main(int argc, char** argv) {
  try {
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    if (args.size() == 3 && args[0] == "list")
      return list_classes(args[1], args[2]);
    if ((args.size() == 5 || args.size() == 6) && args[0] == "class") {
      const bool open_editor = args.size() == 6 && args[5] == "--editor";
      if (args.size() == 6 && !open_editor)
        throw std::runtime_error("the only option is --editor");
      if (!fs::is_directory(args[3]))
        throw std::runtime_error("the scratch directory must exist");
#if defined(OSCILLATE_MATRIX_EDITOR)
      std::optional<QApplication> application;
      if (open_editor)
        application.emplace(argc, argv);
#endif
      return probe_class(args[1], args[2], args[3], args[4], open_editor);
    }
    std::cerr << "usage: oscillate_vst3_matrix_probe list BUNDLE OUTPUT.json\n"
                 "       oscillate_vst3_matrix_probe class BUNDLE CLASS_ID SCRATCH OUTPUT.json "
                 "[--editor]\n";
    return 2;
  } catch (const std::exception& error) {
    std::cerr << "VST3 matrix probe: " << error.what() << '\n';
    return 1;
  }
}
