// Renders Anvil presets (ADR-0060) as four-bar, four-to-the-floor loops at
// 124 BPM through the engine's own runtime, as 24-bit stereo WAVs. Each
// argument names a preset and may override values and curves:
//
//   "Tech House Thump|anvil.punch=0|pitch=0:200:0,1:50:0|file=thump_dry"
//
// Pitch points take Hz; amp and drive points take 0..1; each point is
// x:y:tension. `--list` prints each preset's output level and `--dump NAME`
// every value of one preset.
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/anvil.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace {
using namespace oscillate;
namespace synth = native_dsp::synth;

constexpr int rate = 48'000;
constexpr double bpm = 124.0;
constexpr int hits = 16;

void write_wav(const std::string& path, const std::vector<float>& interleaved) {
  FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr)
    throw std::runtime_error("cannot write " + path);
  const auto frames = static_cast<std::uint32_t>(interleaved.size() / 2);
  const std::uint32_t data = frames * 2 * 3;
  const auto u32 = [&](std::uint32_t v) { std::fwrite(&v, 4, 1, file); };
  const auto u16 = [&](std::uint16_t v) { std::fwrite(&v, 2, 1, file); };
  std::fwrite("RIFF", 1, 4, file);
  u32(36 + data);
  std::fwrite("WAVEfmt ", 1, 8, file);
  u32(16);
  u16(1);
  u16(2);
  u32(rate);
  u32(rate * 6);
  u16(6);
  u16(24);
  std::fwrite("data", 1, 4, file);
  u32(data);
  for (const float sample : interleaved) {
    const auto v = static_cast<std::int32_t>(
        std::lround(std::clamp(static_cast<double>(sample), -1.0, 1.0) * 8'388'607.0));
    const unsigned char bytes[3] = {static_cast<unsigned char>(v),
                                    static_cast<unsigned char>(v >> 8),
                                    static_cast<unsigned char>(v >> 16)};
    std::fwrite(bytes, 1, 3, file);
  }
  std::fclose(file);
}

std::optional<std::size_t> preset_index(const native_dsp::EngineDescriptor& table,
                                        std::string_view name) {
  for (std::size_t i = 0; i < table.presets.size(); ++i) {
    if (table.presets[i].name == name)
      return i;
  }
  return std::nullopt;
}

std::vector<std::string> split(const std::string& text, char separator) {
  std::vector<std::string> parts;
  std::stringstream stream(text);
  std::string part;
  while (std::getline(stream, part, separator))
    parts.push_back(part);
  return parts;
}

std::vector<float> render(const synth::SynthEngine& engine, std::span<const double> values,
                          const project::NativeSynthDocument& document) {
  auto created = synth::NativeSynthRuntime::create(engine, values, document, {}, rate);
  if (!created)
    throw std::runtime_error(created.error().message);
  auto runtime = std::move(created.value());
  const double beat = 60.0 / bpm * rate;
  const auto total = static_cast<std::size_t>(std::lround(beat * hits));
  std::vector<float> out;
  out.reserve(total * 2);
  std::vector<engine::StereoFrame> block(128);
  for (std::size_t start = 0; start < total; start += block.size()) {
    const std::size_t frames = std::min(block.size(), total - start);
    std::vector<engine::BlockMidiEvent> events;
    const auto add = [&](std::size_t at, engine::MidiEventKind kind, std::uint64_t id) {
      if (at < start || at >= start + frames)
        return;
      engine::BlockMidiEvent event;
      event.offset = static_cast<std::uint32_t>(at - start);
      event.clip_id_bits = {1, 1};
      event.note_id_bits = {0, id};
      event.kind = kind;
      event.key = 36;
      event.velocity = kind == engine::MidiEventKind::note_on ? 120 : 0;
      events.push_back(event);
    };
    for (int k = 0; k < hits; ++k) {
      const auto on = static_cast<std::size_t>(std::lround(k * beat));
      add(on + static_cast<std::size_t>(beat / 2), engine::MidiEventKind::note_off,
          static_cast<std::uint64_t>(k + 1));
      add(on, engine::MidiEventKind::note_on, static_cast<std::uint64_t>(k + 1));
    }
    std::ranges::stable_sort(events, {}, &engine::BlockMidiEvent::offset);
    std::ranges::fill(block, engine::StereoFrame{0.0F, 0.0F});
    const double first_beat = static_cast<double>(start) / beat;
    const native_dsp::Transport transport{first_beat, 1.0 / beat, first_beat / 4.0, 4.0};
    (void)runtime->process(std::span(block).first(frames), events, start, 1, transport, {});
    for (std::size_t i = 0; i < frames; ++i) {
      out.push_back(block[i].left);
      out.push_back(block[i].right);
    }
  }
  return out;
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: oscillate_anvil_loops OUTPUT_FOLDER SPEC... | --list | --dump NAME\n";
    return 2;
  }
  const auto& engine = *synth::find_engine("anvil");
  const auto& table = *engine.descriptor;
  const auto value_index = [&](std::string_view id) -> std::optional<std::size_t> {
    for (std::size_t i = 0; i < table.parameters.size(); ++i) {
      if (table.parameters[i].id == id)
        return i;
    }
    return std::nullopt;
  };
  const std::string first = argv[1];
  if (first == "--list" || first == "--dump") {
    for (std::size_t i = 0; i < table.presets.size(); ++i) {
      const auto values = native_dsp::preset_values(table, table.presets[i]).value();
      if (first == "--list") {
        std::cout << table.presets[i].name << '\t' << values[*value_index("anvil.output")] << '\n';
      } else if (argc > 2 && table.presets[i].name == argv[2]) {
        for (std::size_t p = 0; p < table.parameters.size(); ++p)
          std::cout << table.parameters[p].id << ' ' << values[p] << '\n';
      }
    }
    return 0;
  }
  try {
    for (int a = 2; a < argc; ++a) {
      const auto parts = split(argv[a], '|');
      const auto index = preset_index(table, parts.front());
      if (!index)
        throw std::runtime_error("no preset " + parts.front());
      auto values = native_dsp::preset_values(table, table.presets[*index]).value();
      auto document = engine.preset_document(*index);
      std::string file = parts.front();
      for (std::size_t k = 1; k < parts.size(); ++k) {
        const auto equals = parts[k].find('=');
        const std::string key = parts[k].substr(0, equals);
        const std::string value = parts[k].substr(equals + 1);
        if (key == "file") {
          file = value;
        } else if (key == "pitch" || key == "amp" || key == "drive") {
          project::NativeCurve curve{key, {}};
          for (const auto& point : split(value, ',')) {
            const auto fields = split(point, ':');
            double y = std::stod(fields.at(1));
            if (key == "pitch")
              y = synth::anvil::y_of_hz(y);
            curve.points.push_back({std::stod(fields.at(0)), y, std::stod(fields.at(2)),
                                    project::NativeCurveSegment::curve});
          }
          std::erase_if(document.curves, [&](const auto& c) { return c.id == key; });
          document.curves.push_back(std::move(curve));
        } else if (const auto at = value_index(key)) {
          values[*at] = std::stod(value);
        } else {
          throw std::runtime_error("no parameter " + key);
        }
      }
      std::ranges::replace(file, ' ', '_');
      const auto audio = render(engine, values, document);
      write_wav(std::string(argv[1]) + "/Anvil_" + file + ".wav", audio);
      float peak = 0.0F;
      for (const float sample : audio)
        peak = std::max(peak, std::fabs(sample));
      std::cout << file << " peak " << 20.0 * std::log10(std::max(peak, 1e-9F)) << " dBFS\n";
    }
  } catch (const std::exception& error) {
    std::cerr << "anvil loops: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
