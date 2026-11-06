#include "oscillate/plugins/vst3_bundle_loader.hpp"
#include "oscillate/plugins/vst3_scan.hpp"
#include "oscillate/test/audio_constants.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace daw = oscillate::plugins;
using Json = nlohmann::json;

namespace {

struct Selection {
  daw::ScanResult receipt;
  daw::VerifiedVst3Effect verified;
  daw::Vst3HostKind kind;
};

[[nodiscard]] Selection select(const std::filesystem::path& bundle, std::string_view class_id,
                               std::string_view kind) {
  if (kind != "effect" && kind != "instrument")
    throw std::runtime_error("kind must be effect or instrument");
  daw::Vst3Scanner scanner(VST3_HELPER_PATH);
  auto receipt = scanner.scan(bundle);
  if (!receipt.ok())
    throw std::runtime_error("verified scan failed: " + receipt.diagnostic);
  const auto found = std::ranges::find(receipt.metadata.classes, class_id, &daw::Vst3Class::id);
  if (found == receipt.metadata.classes.end() || found->category != "Audio Module Class")
    throw std::runtime_error("selected ID is not a scanned audio module class");
  return {receipt,
          {*found, receipt.binary},
          kind == "instrument" ? daw::Vst3HostKind::instrument : daw::Vst3HostKind::effect};
}

[[nodiscard]] std::unique_ptr<daw::LoadedVst3Effect> load(const Selection& selected,
                                                          std::string& error) {
  constexpr double rate = oscillate::test::sample_rate_48k_hz;
  constexpr std::uint32_t frames = 128;
  return selected.kind == daw::Vst3HostKind::instrument
             ? daw::load_verified_vst3_instrument(selected.verified, rate, frames, error)
             : daw::load_verified_vst3_effect(selected.verified, rate, frames, error);
}

[[nodiscard]] std::string lower(std::string_view text) {
  std::string result(text);
  std::ranges::transform(result, result.begin(), [](unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  return result;
}

[[nodiscard]] Json list_parameters(const Selection& selected, std::string_view filter) {
  std::string error;
  auto loaded = load(selected, error);
  if (!loaded)
    throw std::runtime_error("host load failed: " + error);
  Json result{{"binary_sha256", selected.receipt.binary.sha256},
              {"class_id", selected.verified.identity.id},
              {"initial_latency_samples", loaded->host().latency_samples()},
              {"parameters", Json::array()}};
  const auto needle = lower(filter);
  for (const auto& parameter : loaded->host().parameters()) {
    if (!needle.empty() && lower(parameter.title).find(needle) == std::string::npos)
      continue;
    result["parameters"].push_back({{"id", parameter.id},
                                    {"title", parameter.title},
                                    {"step_count", parameter.step_count},
                                    {"default_normalized", parameter.default_normalized},
                                    {"automatable", parameter.automatable},
                                    {"read_only", parameter.read_only}});
  }
  return result;
}

[[nodiscard]] daw::Vst3ParameterEvent parse_edit(std::string_view text) {
  const auto separator = text.find(':');
  if (separator == std::string_view::npos)
    throw std::runtime_error("edit must be PARAMETER_ID:NORMALIZED_VALUE");
  std::uint32_t id = 0;
  const auto first = text.data();
  const auto split = first + separator;
  const auto parsed_id = std::from_chars(first, split, id);
  if (parsed_id.ec != std::errc{} || parsed_id.ptr != split)
    throw std::runtime_error("invalid parameter ID");
  double value = 0.0;
  const auto value_first = split + 1;
  const auto value_last = first + text.size();
  const auto parsed_value = std::from_chars(value_first, value_last, value);
  if (parsed_value.ec != std::errc{} || parsed_value.ptr != value_last || !std::isfinite(value) ||
      value < 0.0 || value > 1.0)
    throw std::runtime_error("normalized value must be finite and within 0..1");
  return {id, 64, value};
}

[[nodiscard]] daw::Vst3ProcessStatus process(daw::Vst3EffectHost& host, daw::Vst3HostKind kind,
                                             std::span<const daw::Vst3ParameterEvent> edits,
                                             float& peak) {
  std::array<float, 128> input_left{}, input_right{}, output_left{}, output_right{};
  input_left.fill(0.25F);
  input_right.fill(-0.25F);
  daw::Vst3ProcessStatus status;
  if (kind == daw::Vst3HostKind::instrument) {
    const std::array notes{daw::Vst3NoteEvent{.velocity = 0.5F, .note_id = 1}};
    status = host.process_instrument({output_left, output_right}, notes,
                                     {.project_sample = 0, .generation = 1}, edits);
  } else {
    status = host.process({input_left, input_right, output_left, output_right}, edits);
  }
  peak = 0.0F;
  for (const auto value : output_left)
    peak = std::max(peak, std::abs(value));
  for (const auto value : output_right)
    peak = std::max(peak, std::abs(value));
  return status;
}

[[nodiscard]] Json probe(const Selection& selected, std::vector<daw::Vst3ParameterEvent> edits) {
  std::string error;
  auto loaded = load(selected, error);
  if (!loaded)
    throw std::runtime_error("host load failed: " + error);
  auto& host = loaded->host();
  for (const auto& edit : edits) {
    const auto parameter =
        std::ranges::find(host.parameters(), edit.id, &daw::Vst3ParameterInfo::id);
    if (parameter == host.parameters().end() || !parameter->automatable || parameter->read_only)
      throw std::runtime_error("requested parameter is unavailable for event delivery");
  }
  std::ranges::sort(edits, {}, &daw::Vst3ParameterEvent::id);
  if (std::ranges::adjacent_find(edits, std::equal_to{}, &daw::Vst3ParameterEvent::id) !=
      edits.end())
    throw std::runtime_error("duplicate parameter ID");
  Json result{{"binary_sha256", selected.receipt.binary.sha256},
              {"class_id", selected.verified.identity.id},
              {"plugin_name", selected.verified.identity.name},
              {"plugin_version", selected.verified.identity.version},
              {"sample_rate", 48000},
              {"block_frames", 128},
              {"initial_latency_samples", host.latency_samples()},
              {"edits", Json::array()},
              {"restart_requests_before_edit", host.take_restart_requests()},
              {"factory_preset_change", "not_tested"},
              {"fixed_latency_qualified", false}};
  for (const auto& edit : edits)
    result["edits"].push_back({{"id", edit.id}, {"normalized", edit.normalized_value}});
  if (!host.activate(error))
    throw std::runtime_error("activation failed: " + error);
  float peak = 0.0F;
  const auto status = process(host, selected.kind, edits, peak);
  if (status != daw::Vst3ProcessStatus::ok || !std::isfinite(peak))
    throw std::runtime_error("128-frame parameter process failed");
  result["first_block_peak"] = peak;
  // The host stores the initial latency and does not re-query it on the
  // callback. This field is reported as cached, never as measured dynamic PDC.
  result["cached_latency_after_event_samples"] = host.latency_samples();
  // This observes requests only. The host returned kResultFalse and did not
  // perform the requested restart or rebuild delay compensation.
  result["restart_requests_after_event"] = host.take_restart_requests();
  host.suspend();
  auto state = host.save_component_state(error);
  if (!state)
    throw std::runtime_error("state capture failed: " + error);
  result["saved_component_state_bytes"] = state->size();
  loaded.reset();
  auto restored = load(selected, error);
  if (!restored || !restored->host().restore_component_state(*state, error))
    throw std::runtime_error("fresh-instance state restore failed: " + error);
  result["reloaded_state_latency_samples"] = restored->host().latency_samples();
  result["restart_requests_during_restore"] = restored->host().take_restart_requests();
  result["controller_state_sync"] =
      restored->host().controller_state_sync() == daw::Vst3ControllerStateSync::not_implemented
          ? "not_implemented"
          : "synchronized_or_unavailable";
  if (!restored->host().activate(error))
    throw std::runtime_error("restored host activation failed: " + error);
  if (process(restored->host(), selected.kind, {}, peak) != daw::Vst3ProcessStatus::ok ||
      !std::isfinite(peak))
    throw std::runtime_error("restored host process failed");
  result["reloaded_first_block_peak"] = peak;
  return result;
}

// An opt-in active-instance survey for the exact Surge XT 1.3.4 cutoff.
// A synthetic instrument can run the same sequence as a deterministic smoke
// test, but only a hash-matched Surge bundle supplies qualification evidence.
[[nodiscard]] Json cutoff_sweep(const Selection& selected, std::uint32_t parameter_id,
                                std::uint32_t frames, std::string_view required_hash = {}) {
  if (selected.kind != daw::Vst3HostKind::instrument || (frames != 64 && frames != 128) ||
      (!required_hash.empty() && selected.receipt.binary.sha256 != required_hash))
    throw std::runtime_error("cutoff sweep requires the selected instrument and binary hash");
  std::string error;
  auto loaded = load(selected, error);
  if (!loaded)
    throw std::runtime_error("instrument load failed: " + error);
  auto& host = loaded->host();
  const auto found =
      std::ranges::find(host.parameters(), parameter_id, &daw::Vst3ParameterInfo::id);
  if (found == host.parameters().end() || !found->automatable || found->read_only ||
      found->step_count != 0)
    throw std::runtime_error("cutoff parameter is not continuous and automatable");
  const auto initial_latency = host.latency_samples();
  const auto initial_restart = host.take_restart_requests();
  if (!host.activate(error))
    throw std::runtime_error("instrument activation failed: " + error);

  constexpr std::array<double, 8> sequence{0.0, 0.05, 0.5, 0.95, 1.0, 0.95, 0.5, 0.05};
  constexpr std::array<double, 5> levels{0.0, 0.05, 0.5, 0.95, 1.0};
  constexpr std::uint32_t cycles = 8;
  constexpr std::uint32_t blocks_per_value = 8;
  constexpr std::uint32_t expected_blocks = cycles * sequence.size() * blocks_per_value;
  std::array<float, 128> left{}, right{};
  Json values = Json::array();
  for (double value : levels)
    values.push_back({{"normalized", value},
                      {"blocks", 0},
                      {"peak", 0.0},
                      {"absolute_sum", 0.0},
                      {"restart_flags", 0}});
  std::uint32_t restart_flags = 0;
  std::uint32_t blocks_with_restart = 0;
  std::uint64_t nonzero_samples = 0;
  for (std::uint32_t block = 0; block < expected_blocks; ++block) {
    const auto sequence_index = (block / blocks_per_value) % sequence.size();
    const auto value = sequence[sequence_index];
    const auto value_index =
        static_cast<std::size_t>(std::ranges::find(levels, value) - levels.begin());
    const std::array edit{daw::Vst3ParameterEvent{parameter_id, frames / 2, value}};
    const std::array note{daw::Vst3NoteEvent{.velocity = 0.5F, .note_id = 1}};
    const auto status = host.process_instrument(
        {std::span(left).first(frames), std::span(right).first(frames)},
        block == 0 ? std::span<const daw::Vst3NoteEvent>(note)
                   : std::span<const daw::Vst3NoteEvent>{},
        {.project_sample = static_cast<std::int64_t>(block) * frames, .generation = 1},
        block % blocks_per_value == 0 ? std::span<const daw::Vst3ParameterEvent>(edit)
                                      : std::span<const daw::Vst3ParameterEvent>{});
    if (status != daw::Vst3ProcessStatus::ok)
      throw std::runtime_error("active cutoff sweep process failed at block " +
                               std::to_string(block));
    auto& bucket = values[value_index];
    bucket["blocks"] = bucket["blocks"].get<std::uint32_t>() + 1;
    double peak = bucket["peak"].get<double>();
    double absolute_sum = bucket["absolute_sum"].get<double>();
    for (std::uint32_t sample = 0; sample < frames; ++sample) {
      if (!std::isfinite(left[sample]) || !std::isfinite(right[sample]))
        throw std::runtime_error("non-finite output during active cutoff sweep");
      const auto l = std::abs(static_cast<double>(left[sample]));
      const auto r = std::abs(static_cast<double>(right[sample]));
      peak = std::max({peak, l, r});
      absolute_sum += l + r;
      nonzero_samples +=
          static_cast<std::uint64_t>(l != 0.0) + static_cast<std::uint64_t>(r != 0.0);
    }
    bucket["peak"] = peak;
    bucket["absolute_sum"] = absolute_sum;
    const auto flags = host.take_restart_requests();
    restart_flags |= flags;
    if (flags != 0) {
      ++blocks_with_restart;
      bucket["restart_flags"] = bucket["restart_flags"].get<std::uint32_t>() | flags;
    }
  }
  host.suspend();
  const auto flags_after_suspend = host.take_restart_requests();
  auto state = host.save_component_state(error);
  if (!state)
    throw std::runtime_error("cutoff sweep state save failed: " + error);
  const auto cached_suspended_latency = host.latency_samples();
  loaded.reset();
  auto restored = load(selected, error);
  if (!restored || !restored->host().restore_component_state(*state, error))
    throw std::runtime_error("cutoff sweep fresh-instance restore failed: " + error);
  const auto restored_latency = restored->host().latency_samples();
  const auto restore_flags = restored->host().take_restart_requests();
  if (!restored->host().activate(error))
    throw std::runtime_error("restored cutoff instrument activation failed: " + error);
  float restored_peak = 0.0F;
  if (process(restored->host(), daw::Vst3HostKind::instrument, {}, restored_peak) !=
          daw::Vst3ProcessStatus::ok ||
      !std::isfinite(restored_peak))
    throw std::runtime_error("restored cutoff instrument process failed");
  return {{"binary_sha256", selected.receipt.binary.sha256},
          {"class_id", selected.verified.identity.id},
          {"parameter_id", parameter_id},
          {"sample_rate", 48000},
          {"block_frames", frames},
          {"blocks_processed", expected_blocks},
          {"cycles", cycles},
          {"blocks_per_value", blocks_per_value},
          {"values", std::move(values)},
          {"nonzero_output_samples", nonzero_samples},
          {"restart_flags_before_activation", initial_restart},
          {"restart_flags_during_active_sweep", restart_flags},
          {"blocks_with_restart_requests", blocks_with_restart},
          {"restart_flags_after_suspend", flags_after_suspend},
          {"restart_flags_during_restore", restore_flags},
          {"initial_cached_latency_samples", initial_latency},
          {"suspended_cached_latency_samples", cached_suspended_latency},
          {"fresh_restored_latency_samples", restored_latency},
          {"saved_component_state_bytes", state->size()},
          {"restored_first_block_peak", restored_peak},
          {"fixed_latency_qualified", false}};
}

} // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--self-test") {
      const auto selected =
          select(VST3_CONNECTED_BUNDLE, "0102030405060708090a0b0c0d0e0f10", "effect");
      const auto result = probe(selected, {{101, 64, 0.75}});
      if (result["initial_latency_samples"] != 7 || result["reloaded_state_latency_samples"] != 7 ||
          result["saved_component_state_bytes"] != sizeof(float) ||
          result["fixed_latency_qualified"] != false ||
          result["restart_requests_after_event"] != 0 ||
          result["restart_requests_during_restore"] != 0) {
        std::cerr << result.dump(2) << '\n';
        return 1;
      }
      return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--self-test-sweep") {
      const auto selected =
          select(VST3_INSTRUMENT_BUNDLE, "464748494a4b4c4d4e4f505152535455", "instrument");
      for (const auto frames : {64U, 128U}) {
        const auto result = cutoff_sweep(selected, 1, frames);
        if (result["blocks_processed"] != 512 || result["nonzero_output_samples"] == 0 ||
            result["restart_flags_during_active_sweep"] != 0 ||
            result["fresh_restored_latency_samples"] != 0 ||
            result["saved_component_state_bytes"] == 0 || result["values"].size() != 5 ||
            result["values"][0]["blocks"] != 64 || result["values"][1]["blocks"] != 128 ||
            result["values"][2]["blocks"] != 128 || result["values"][3]["blocks"] != 128 ||
            result["values"][4]["blocks"] != 64 || result["fixed_latency_qualified"] != false)
          throw std::runtime_error("synthetic active sweep contract failed");
      }
      return 0;
    }
    if (argc == 4 && std::string_view(argv[1]) == "--surge-cutoff-sweep") {
      const auto frames = std::string_view(argv[3]);
      if (frames != "64" && frames != "128")
        throw std::runtime_error("cutoff sweep block size must be 64 or 128");
      const auto selected = select(argv[2], "abcdef019182faeb566d624153675854", "instrument");
      std::cout << cutoff_sweep(selected, 1945359057, frames == "64" ? 64 : 128,
                                "b8584398f314819241ca7287117d5559ed838d37ef08faab9847f27ba2993cec")
                       .dump(2)
                << '\n';
      return 0;
    }
    if (argc == 6 && std::string_view(argv[1]) == "--list") {
      const auto selected = select(argv[2], argv[3], argv[4]);
      std::cout << list_parameters(selected, argv[5]).dump(2) << '\n';
      return 0;
    }
    if (argc >= 5 && std::string_view(argv[1]) == "--probe") {
      const auto selected = select(argv[2], argv[3], argv[4]);
      std::vector<daw::Vst3ParameterEvent> edits;
      edits.reserve(static_cast<std::size_t>(argc - 5));
      for (int index = 5; index < argc; ++index)
        edits.push_back(parse_edit(argv[index]));
      std::cout << probe(selected, std::move(edits)).dump(2) << '\n';
      return 0;
    }
    std::cerr << "Usage: live_latency_probe --list BUNDLE CLASS_ID effect|instrument FILTER\n"
                 "       live_latency_probe --probe BUNDLE CLASS_ID effect|instrument ID:VALUE...\n"
                 "       live_latency_probe --self-test-sweep\n"
                 "       live_latency_probe --surge-cutoff-sweep BUNDLE 64|128\n";
    return 2;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
