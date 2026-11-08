#include "oscillate/foundation/trace.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

// Nova's cost and output on the scenes that matter: the Analog Pad's
// overlapping chords (a demo project's heaviest track), the Pluck line, and a
// chord on every preset. Run in an uninstrumented Release build; timing is
// not a CTest gate.
//
//   oscillate_nova_bench                 timing at 128 and 512 frames
//   oscillate_nova_bench --timing PREFIX the scenes named PREFIX... only
//   oscillate_nova_bench --render DIR    each scene's output, as raw float
//   oscillate_nova_bench --compare DIR   each scene against those renders
//   oscillate_nova_bench --scene NAME    one scene, rendered ten times (for
//                                        a profiler); NOVA_TRACE=FILE records
//                                        one render's trace, NOVA_TIMELINE=1
//                                        prints its cost block by block
namespace {
using namespace oscillate;
using namespace oscillate::native_dsp;
using namespace oscillate::native_dsp::synth;

constexpr double rate = 48'000.0;

struct Note {
  double on_seconds;
  double off_seconds;
  std::uint8_t key;
};

struct Scene {
  std::string name;
  std::size_t preset;
  std::vector<Note> notes;
  double seconds;
};

std::size_t preset_index(std::string_view name) {
  const auto& table = *find_engine("nova")->descriptor;
  for (std::size_t i = 0; i < table.presets.size(); ++i) {
    if (table.presets[i].name == name)
      return i;
  }
  std::fprintf(stderr, "no Nova preset %.*s\n", static_cast<int>(name.size()), name.data());
  std::exit(2);
}

std::vector<Scene> scenes() {
  std::vector<Scene> list;
  // Four-note chords every two seconds, each held 1.9 s, so the 2.5 s
  // releases overlap the next chord: up to eight voices at once.
  Scene pad{"Analog Pad chords", preset_index("Analog Pad"), {}, 12.0};
  const std::uint8_t chords[4][4] = {
      {57, 60, 64, 67}, {53, 57, 60, 64}, {55, 59, 62, 65}, {52, 55, 59, 64}};
  for (int c = 0; c < 5; ++c) {
    for (const auto key : chords[c % 4])
      pad.notes.push_back({2.0 * c, 2.0 * c + 1.9, key});
  }
  list.push_back(pad);
  // A sixteenth-note line at 124 BPM.
  Scene pluck{"Pluck sixteenths", preset_index("Pluck"), {}, 8.0};
  const double sixteenth = 60.0 / 124.0 / 4.0;
  for (int i = 0; i < 120; ++i)
    pluck.notes.push_back({i * sixteenth, i * sixteenth + sixteenth * 0.8,
                           static_cast<std::uint8_t>(45 + (i * 7) % 12)});
  list.push_back(pluck);
  const auto& table = *find_engine("nova")->descriptor;
  for (std::size_t p = 0; p < table.presets.size(); ++p) {
    Scene chord{"preset " + std::string(table.presets[p].name), p, {}, 5.0};
    for (const std::uint8_t key :
         {std::uint8_t{48}, std::uint8_t{55}, std::uint8_t{60}, std::uint8_t{64}})
      chord.notes.push_back({0.0, 2.5, key});
    list.push_back(chord);
  }
  // Five-note pad chords, as the Chord track now holds them: every Pad
  // preset, against a lead and a pluck. Held 4 s, then 4 s of release.
  for (std::size_t p = 0; p < table.presets.size(); ++p) {
    const auto& preset = table.presets[p];
    if (preset.category != "Pad" && preset.name != "Supersaw Lead" && preset.name != "Pluck")
      continue;
    Scene chord{"chord5 " + std::string(preset.name), p, {}, 8.0};
    for (const std::uint8_t key :
         {std::uint8_t{48}, std::uint8_t{55}, std::uint8_t{60}, std::uint8_t{64}, std::uint8_t{67}})
      chord.notes.push_back({0.0, 4.0, key});
    list.push_back(chord);
  }
  return list;
}

struct Result {
  std::vector<float> audio; // Interleaved stereo.
  std::vector<double> block_ns;
};

Result render(const Scene& scene, std::size_t block) {
  const auto& engine = *find_engine("nova");
  const auto& table = *engine.descriptor;
  const auto values = preset_values(table, table.presets[scene.preset]).value();
  auto runtime = std::move(
      NativeSynthRuntime::create(engine, values, engine.preset_document(scene.preset), {}, rate)
          .value());
  const auto total = static_cast<std::size_t>(scene.seconds * rate);
  Result result;
  result.audio.reserve(total * 2);
  std::vector<engine::StereoFrame> buffer(block);
  std::vector<engine::BlockMidiEvent> events;
  for (std::size_t start = 0; start < total; start += block) {
    const auto frames = std::min(block, total - start);
    events.clear();
    for (std::size_t n = 0; n < scene.notes.size(); ++n) {
      const auto& note = scene.notes[n];
      for (const bool on : {false, true}) {
        const auto at = static_cast<std::size_t>((on ? note.on_seconds : note.off_seconds) * rate);
        if (at < start || at >= start + frames)
          continue;
        engine::BlockMidiEvent event;
        event.offset = static_cast<std::uint32_t>(at - start);
        event.clip_id_bits = {1, 1};
        event.note_id_bits = {0, n + 1};
        event.kind = on ? engine::MidiEventKind::note_on : engine::MidiEventKind::note_off;
        event.key = note.key;
        event.velocity = on ? 100 : 0;
        events.push_back(event);
      }
    }
    std::ranges::stable_sort(events, {}, &engine::BlockMidiEvent::offset);
    std::ranges::fill(buffer, engine::StereoFrame{0.0F, 0.0F});
    const double beat = static_cast<double>(start) / (rate * 60.0 / 124.0);
    const Transport transport{beat, 124.0 / 60.0 / rate, beat / 4.0, 4.0};
    const auto began = std::chrono::steady_clock::now();
    (void)runtime->process(std::span(buffer).first(frames), events, start, 1, transport, {});
    result.block_ns.push_back(
        std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - began).count());
    for (std::size_t i = 0; i < frames; ++i) {
      result.audio.push_back(buffer[i].left);
      result.audio.push_back(buffer[i].right);
    }
  }
  return result;
}

std::filesystem::path file_of(const std::filesystem::path& directory, const Scene& scene) {
  std::string name = scene.name;
  std::ranges::replace(name, ' ', '_');
  return directory / (name + ".f32");
}

int timing(std::string_view prefix) {
  for (const std::size_t block : {std::size_t{128}, std::size_t{512}}) {
    for (const auto& scene : scenes()) {
      if (!scene.name.starts_with(prefix))
        continue;
      const auto result = render(scene, block);
      auto sorted = result.block_ns;
      std::ranges::sort(sorted);
      double total = 0.0;
      for (const double ns : result.block_ns)
        total += ns;
      const double budget = static_cast<double>(block) / rate * 1e9;
      std::printf("%-34s block %3zu: mean %6.1f%%  p99 %6.1f%%  max %6.1f%% of one core\n",
                  scene.name.c_str(), block,
                  100.0 * total / static_cast<double>(result.block_ns.size()) / budget,
                  100.0 * sorted[sorted.size() * 99 / 100] / budget,
                  100.0 * sorted.back() / budget);
    }
  }
  return 0;
}

int render_all(const std::filesystem::path& directory) {
  std::filesystem::create_directories(directory);
  for (const auto& scene : scenes()) {
    const auto result = render(scene, 128);
    std::ofstream file(file_of(directory, scene), std::ios::binary);
    file.write(reinterpret_cast<const char*>(result.audio.data()),
               static_cast<std::streamsize>(result.audio.size() * sizeof(float)));
  }
  return 0;
}

int compare_all(const std::filesystem::path& directory) {
  bool identical = true;
  for (const auto& scene : scenes()) {
    const auto result = render(scene, 128);
    std::ifstream file(file_of(directory, scene), std::ios::binary);
    std::vector<float> reference(result.audio.size());
    file.read(reinterpret_cast<char*>(reference.data()),
              static_cast<std::streamsize>(reference.size() * sizeof(float)));
    if (!file) {
      std::printf("%-34s no reference\n", scene.name.c_str());
      identical = false;
      continue;
    }
    double worst = 0.0;
    double peak = 0.0;
    for (std::size_t i = 0; i < reference.size(); ++i) {
      worst = std::max(worst, static_cast<double>(std::fabs(result.audio[i] - reference[i])));
      peak = std::max(peak, static_cast<double>(std::fabs(reference[i])));
    }
    identical = identical && worst == 0.0;
    std::printf("%-34s max difference %s (peak %.2f dBFS)\n", scene.name.c_str(),
                worst == 0.0 ? "none (bit-identical)"
                             : (std::to_string(20.0 * std::log10(worst)) + " dBFS").c_str(),
                20.0 * std::log10(std::max(peak, 1e-12)));
  }
  return identical ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
  const std::string_view mode = argc > 1 ? argv[1] : "";
  if (mode == "--render" && argc > 2)
    return render_all(argv[2]);
  if (mode == "--compare" && argc > 2)
    return compare_all(argv[2]);
  if (mode == "--scene" && argc > 2) {
    for (const auto& scene : scenes()) {
      if (scene.name == argv[2]) {
        const char* trace = std::getenv("NOVA_TRACE");
        if (trace != nullptr)
          (void)foundation::trace::start(std::size_t{1} << 22);
        if (std::getenv("NOVA_TIMELINE") != nullptr) {
          // Each 1024-frame block's cost, against its budget, over time.
          constexpr std::size_t block = 1024;
          const auto result = render(scene, block);
          const double budget = static_cast<double>(block) / rate * 1e9;
          for (std::size_t b = 0; b < result.block_ns.size(); ++b)
            std::printf("%5.2f s %6.1f%%\n", static_cast<double>(b * block) / rate,
                        100.0 * result.block_ns[b] / budget);
          return 0;
        }
        for (int i = 0; i < (trace != nullptr ? 1 : 10); ++i)
          (void)render(scene, 128);
        if (trace != nullptr)
          (void)foundation::trace::stop_and_write(trace);
        return 0;
      }
    }
    std::fprintf(stderr, "no scene %s\n", argv[2]);
    return 2;
  }
  // --timing PREFIX times only the scenes whose names start with PREFIX
  // (for example "chord5").
  if (mode == "--timing" && argc > 2)
    return timing(argv[2]);
  return timing("");
}
