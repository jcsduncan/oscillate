#include "oscillate/plugins/vst3_bundle_loader.hpp"
#include "oscillate/plugins/vst3_scan.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {
namespace fs = std::filesystem;
namespace vst = oscillate::plugins;
using Json = nlohmann::json;

constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
constexpr std::uint32_t short_note_frames = 257;
constexpr std::uint32_t long_note_frames = 4'096;
constexpr std::uint32_t guard_frames = 2'048;
constexpr std::uint32_t max_tail_frames = rate * 2;
constexpr double threshold = 1e-6;

struct Options {
  fs::path bundle;
  std::string class_id;
  fs::path output;
  std::optional<fs::path> state_file;
  std::optional<fs::path> controller_state_file;
  std::optional<std::uint32_t> parameter_id;
};

struct Selection {
  vst::ScanResult scan;
  vst::VerifiedVst3Effect verified;
};

[[nodiscard]] std::string hex(std::span<const std::uint8_t> bytes) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string result(bytes.size() * 2, '0');
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    result[i * 2] = digits[bytes[i] >> 4];
    result[i * 2 + 1] = digits[bytes[i] & 15];
  }
  return result;
}

[[nodiscard]] std::uint64_t fingerprint(std::span<const float> samples) noexcept {
  std::uint64_t hash = 14695981039346656037ULL;
  for (float sample : samples) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &sample, sizeof(bits));
    for (unsigned byte = 0; byte < 4; ++byte) {
      hash ^= bits & 0xffU;
      hash *= 1099511628211ULL;
      bits >>= 8U;
    }
  }
  return hash;
}

[[nodiscard]] Selection select(const Options& options) {
  vst::Vst3Scanner scanner(VST3_HELPER_PATH);
  auto scan = scanner.scan(fs::canonical(options.bundle));
  if (!scan.ok())
    throw std::runtime_error("VST3 scan failed: " + scan.diagnostic);
  const auto selected =
      std::ranges::find(scan.metadata.classes, options.class_id, &vst::Vst3Class::id);
  if (selected == scan.metadata.classes.end() || selected->category != "Audio Module Class")
    throw std::runtime_error("selected class is not a scanned VST3 audio module");
  return {scan, {*selected, scan.binary}};
}

[[nodiscard]] std::unique_ptr<vst::LoadedVst3Effect> load(const Selection& selected) {
  std::string error;
  // Some instruments, including the synthetic fixture, require a 128-frame
  // setup even when callbacks themselves use 64 frames.
  auto host = vst::load_verified_vst3_instrument(selected.verified, rate, 128, error);
  if (!host)
    throw std::runtime_error("VST3 instrument load failed: " + error);
  return host;
}

[[nodiscard]] std::vector<std::uint8_t> read_state_file(const fs::path& path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input || input.tellg() < 0 || input.tellg() > 4 * 1024 * 1024)
    throw std::runtime_error("state file is unavailable or exceeds 4 MiB");
  const auto size = static_cast<std::size_t>(input.tellg());
  input.seekg(0);
  std::vector<std::uint8_t> state(size);
  if (size != 0 &&
      !input.read(reinterpret_cast<char*>(state.data()), static_cast<std::streamsize>(size)))
    throw std::runtime_error("state file read failed");
  return state;
}

[[nodiscard]] vst::Vst3StatePair source_state(const Selection& selected, const Options& options) {
  if (options.state_file) {
    return {.component_state = read_state_file(*options.state_file),
            .controller_state = options.controller_state_file
                                    ? std::optional(read_state_file(*options.controller_state_file))
                                    : std::nullopt};
  }
  auto loaded = load(selected);
  std::string error;
  auto state = loaded->host().save_state_pair(error);
  if (!state)
    throw std::runtime_error("default state capture failed: " + error);
  return std::move(*state);
}

enum class NoteLength { short_note, long_note };

struct Run {
  std::uint32_t frames = 0;
  std::uint32_t latency = 0;
  std::uint32_t tail = 0;
  std::uint64_t callback_news = 0;
  std::uint32_t restart_flags = 0;
  double output_peak = 0.0;
  double past_tail_peak = 0.0;
  bool finite = true;
  bool process_ok = true;
  std::vector<float> left;
  std::vector<float> right;

  [[nodiscard]] bool passes() const noexcept {
    return finite && process_ok && callback_news == 0 && restart_flags == 0 &&
           past_tail_peak <= threshold;
  }
};

[[nodiscard]] Run render(const Selection& selected, const vst::Vst3StatePair& state,
                         std::uint32_t frames, NoteLength length,
                         std::optional<vst::Vst3ParameterEvent> parameter) {
  auto loaded = load(selected);
  auto& host = loaded->host();
  std::string error;
  if (!host.restore_state_pair(state, error))
    throw std::runtime_error("state-pair restore failed: " + error);
  const auto declared_tail = host.finite_tail_samples();
  if (!declared_tail || *declared_tail > max_tail_frames)
    throw std::runtime_error("tail is infinite, unavailable, or exceeds the two-second probe cap");
  const auto latency = host.latency_samples();
  if (latency > max_tail_frames)
    throw std::runtime_error("latency exceeds the two-second probe cap");
  if (!host.activate(error))
    throw std::runtime_error("instrument activation failed: " + error);
  Run result;
  result.frames = frames;
  result.latency = latency;
  result.tail = *declared_tail;
  const auto note_off_frame =
      length == NoteLength::short_note ? short_note_frames : long_note_frames;
  const std::uint64_t needed =
      static_cast<std::uint64_t>(note_off_frame) + latency + *declared_tail + guard_frames;
  const auto total = static_cast<std::size_t>((needed + frames - 1) / frames * frames);
  result.left.resize(total);
  result.right.resize(total);
  std::array<float, 128> output_left{}, output_right{};
  const auto before_news = counted_allocations;
  for (std::size_t offset = 0; offset < total; offset += frames) {
    std::array<vst::Vst3NoteEvent, 1> note{};
    std::span<const vst::Vst3NoteEvent> notes;
    if (offset == 0) {
      note[0] = {.kind = vst::Vst3NoteKind::on,
                 .sample_offset = 0,
                 .channel = 0,
                 .pitch = 60,
                 .velocity = 0.8F,
                 .note_id = 1};
      notes = note;
    } else if (offset <= note_off_frame && note_off_frame < offset + frames) {
      note[0] = {.kind = vst::Vst3NoteKind::off,
                 .sample_offset = static_cast<std::uint32_t>(note_off_frame - offset),
                 .channel = 0,
                 .pitch = 60,
                 .velocity = 0.0F,
                 .note_id = 1};
      notes = note;
    }
    const std::array parameter_event{parameter.value_or(vst::Vst3ParameterEvent{})};
    const auto parameters = parameter && offset == 0
                                ? std::span<const vst::Vst3ParameterEvent>(parameter_event)
                                : std::span<const vst::Vst3ParameterEvent>{};
    const vst::Vst3InstrumentTransport transport{
        .project_sample = static_cast<std::int64_t>(offset),
        .quarter_position = static_cast<double>(offset) / 24'000.0,
        .tempo_bpm = 120.0,
        .generation = 1,
        .playing = true};
    counting_allocations = true;
    const auto status = host.process_instrument(
        {std::span(output_left).first(frames), std::span(output_right).first(frames)}, notes,
        transport, parameters);
    counting_allocations = false;
    if (status != vst::Vst3ProcessStatus::ok) {
      result.process_ok = false;
      break;
    }
    for (std::uint32_t i = 0; i < frames; ++i) {
      const auto index = offset + i;
      const float left = output_left[i];
      const float right = output_right[i];
      result.left[index] = left;
      result.right[index] = right;
      if (!std::isfinite(left) || !std::isfinite(right)) {
        result.finite = false;
        continue;
      }
      const auto magnitude =
          std::max(std::abs(static_cast<double>(left)), std::abs(static_cast<double>(right)));
      result.output_peak = std::max(result.output_peak, magnitude);
      if (index >= note_off_frame + latency + *declared_tail)
        result.past_tail_peak = std::max(result.past_tail_peak, magnitude);
    }
  }
  result.callback_news = counted_allocations - before_news;
  result.restart_flags = host.take_restart_requests();
  if (host.finite_tail_samples() != declared_tail || host.latency_samples() != latency)
    result.process_ok = false;
  host.suspend();
  return result;
}

[[nodiscard]] Json run_json(const Run& run) {
  return {{"block_frames", run.frames},
          {"latency_frames", run.latency},
          {"declared_tail_frames", run.tail},
          {"process_ok", run.process_ok},
          {"finite_output", run.finite},
          {"callback_cpp_new_calls", run.callback_news},
          {"restart_flags", run.restart_flags},
          {"output_peak", run.output_peak},
          {"past_declared_tail_peak", run.past_tail_peak},
          {"left_fingerprint_fnv64", std::to_string(fingerprint(run.left))},
          {"right_fingerprint_fnv64", std::to_string(fingerprint(run.right))}};
}

[[nodiscard]] Json probe(const Selection& selected, const vst::Vst3StatePair& state,
                         std::optional<std::uint32_t> parameter_id) {
  if (parameter_id) {
    auto loaded = load(selected);
    const auto found =
        std::ranges::find(loaded->host().parameters(), *parameter_id, &vst::Vst3ParameterInfo::id);
    if (found == loaded->host().parameters().end() || found->read_only || !found->automatable)
      throw std::runtime_error("selected parameter is not writable and automatable");
  }
  Json report{{"schema", "oscillate-vst3-instrument-tail-probe-2"},
              {"binary_sha256", selected.scan.binary.sha256},
              {"binary_byte_size", selected.scan.binary.byte_size},
              {"class_id", selected.verified.identity.id},
              {"vendor", selected.verified.identity.vendor},
              {"name", selected.verified.identity.name},
              {"version", selected.verified.identity.version},
              {"sample_rate", rate},
              {"saved_component_state_hex", hex(state.component_state)},
              {"saved_controller_state_hex",
               state.controller_state ? Json(hex(*state.controller_state)) : Json(nullptr)},
              {"saved_parameters", Json::array()},
              {"midi_pitch", 60},
              {"midi_velocity", 0.8},
              {"short_note_frames", short_note_frames},
              {"long_note_frames", long_note_frames},
              {"guard_frames", guard_frames},
              {"max_tail_frames", max_tail_frames},
              {"silence_threshold_linear", threshold},
              {"production_admitted", false},
              {"runs", Json::array()}};
  bool passes = true;
  std::optional<std::uint32_t> common_tail;
  std::optional<std::uint32_t> common_latency;
  for (const auto frames : {64U, 128U}) {
    for (const auto length : {NoteLength::short_note, NoteLength::long_note}) {
      const auto name = length == NoteLength::short_note ? "short_note" : "long_note";
      auto first = render(selected, state, frames, length, std::nullopt);
      auto repeat = render(selected, state, frames, length, std::nullopt);
      const bool identical = first.left == repeat.left && first.right == repeat.right;
      const bool stable = first.latency == repeat.latency && first.tail == repeat.tail &&
                          (!common_tail || *common_tail == first.tail) &&
                          (!common_latency || *common_latency == first.latency);
      common_tail = first.tail;
      common_latency = first.latency;
      passes = passes && first.passes() && repeat.passes() && first.output_peak > threshold &&
               repeat.output_peak > threshold && identical && stable;
      auto result = run_json(first);
      result["note_length"] = name;
      result["repeat_bit_identical"] = identical;
      result["latency_tail_stable"] = stable;
      result["repeat"] = run_json(repeat);
      report["runs"].push_back(std::move(result));
    }
    if (parameter_id) {
      for (const auto value : {0.0, 0.5, 1.0}) {
        auto changed = render(selected, state, frames, NoteLength::long_note,
                              vst::Vst3ParameterEvent{*parameter_id, 0, value});
        passes = passes && changed.passes() && changed.tail == common_tail &&
                 changed.latency == common_latency;
        auto result = run_json(changed);
        result["note_length"] = "long_note_parameter";
        result["parameter_id"] = *parameter_id;
        result["normalized_value"] = value;
        report["runs"].push_back(std::move(result));
      }
    }
  }
  report["declared_tail_frames"] = common_tail.value_or(0);
  report["latency_frames"] = common_latency.value_or(0);
  report["note_off_release_tested_64_frames"] = true;
  report["note_off_release_tested_128_frames"] = true;
  report["screen_passed"] = passes;
  report["manual_admission_required"] = true;
  return report;
}

[[nodiscard]] Options parse(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (index + 1 >= argc)
      throw std::runtime_error("missing value for " + std::string(argument));
    const std::string_view value(argv[++index]);
    if (argument == "--bundle")
      options.bundle = value;
    else if (argument == "--class")
      options.class_id = value;
    else if (argument == "--output")
      options.output = value;
    else if (argument == "--state-file")
      options.state_file = fs::path(value);
    else if (argument == "--controller-state-file")
      options.controller_state_file = fs::path(value);
    else if (argument == "--parameter") {
      std::uint32_t parsed = 0;
      const auto number = std::from_chars(value.data(), value.data() + value.size(), parsed);
      if (number.ec != std::errc{} || number.ptr != value.data() + value.size())
        throw std::runtime_error("invalid parameter ID");
      options.parameter_id = parsed;
    } else
      throw std::runtime_error("unknown option: " + std::string(argument));
  }
  if (options.bundle.empty() || options.class_id.empty())
    throw std::runtime_error("usage: --bundle PATH --class CLASS_ID [--state-file RAW_BYTES] "
                             "[--controller-state-file RAW_BYTES] [--parameter ID] "
                             "[--output JSON_PATH]");
  if (options.controller_state_file && !options.state_file)
    throw std::runtime_error("--controller-state-file requires --state-file");
  return options;
}
} // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--self-test") {
      Options options;
      options.bundle = VST3_INSTRUMENT_BUNDLE;
      vst::Vst3Scanner scanner(VST3_HELPER_PATH);
      const auto scanned = scanner.scan(fs::canonical(options.bundle));
      if (!scanned.ok() || scanned.metadata.classes.size() != 1)
        throw std::runtime_error("synthetic instrument scan failed");
      options.class_id = scanned.metadata.classes.front().id;
      const auto selected = select(options);
      float gain = 0.5F;
      std::array<std::uint8_t, sizeof(gain)> state{};
      std::memcpy(state.data(), &gain, sizeof(gain));
      if (setenv("OSCILLATE_VST3_INSTRUMENT_TAIL", "8", 1) != 0)
        throw std::runtime_error("cannot select honest synthetic instrument tail");
      const vst::Vst3StatePair pair{.component_state = {state.begin(), state.end()},
                                    .controller_state = std::nullopt};
      auto good = probe(selected, pair, std::nullopt);
      if (!good["screen_passed"].get<bool>() || good["latency_frames"] != 0 ||
          good["declared_tail_frames"] != 8 || good["runs"].size() != 4 ||
          good["runs"][0]["past_declared_tail_peak"].get<double>() > threshold)
        throw std::runtime_error("honest synthetic note release did not pass");
      const auto default_state = source_state(selected, options);
      auto with_controller = probe(selected, default_state, std::nullopt);
      if (!default_state.controller_state ||
          with_controller["saved_controller_state_hex"].is_null() ||
          !with_controller["screen_passed"].get<bool>())
        throw std::runtime_error("controller-state tail probe did not pass");
      if (setenv("OSCILLATE_VST3_INSTRUMENT_TAIL", "dishonest", 1) != 0)
        throw std::runtime_error("cannot select dishonest synthetic instrument tail");
      auto dishonest = probe(selected, pair, std::nullopt);
      if (dishonest["screen_passed"].get<bool>() ||
          dishonest["runs"][0]["past_declared_tail_peak"].get<double>() <= threshold ||
          dishonest["runs"][1]["past_declared_tail_peak"].get<double>() <= threshold)
        throw std::runtime_error("late note release was not rejected");
      if (unsetenv("OSCILLATE_VST3_INSTRUMENT_TAIL") != 0)
        throw std::runtime_error("cannot clear synthetic instrument tail");
      return 0;
    }
    const auto options = parse(argc, argv);
    const auto selected = select(options);
    const auto state = source_state(selected, options);
    const auto report = probe(selected, state, options.parameter_id);
    if (!options.output.empty()) {
      std::ofstream output(options.output, std::ios::binary | std::ios::trunc);
      if (!output || !(output << report.dump(2) << '\n'))
        throw std::runtime_error("evidence output write failed");
    } else {
      std::cout << report.dump(2) << '\n';
    }
    return report["screen_passed"].get<bool>() ? 0 : 2;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "VST3 instrument-tail probe: %s\n", error.what());
    return 1;
  }
}
