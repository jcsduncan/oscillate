#include "oscillate/plugins/vst3_bundle_loader.hpp"
#include "oscillate/plugins/vst3_scan.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/check.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace oscillate::plugins;

using oscillate::test::counting_process_allocations;
using oscillate::test::process_allocations;

namespace {
using oscillate::test::require;

void check_transport(const VerifiedVst3Effect& verified, Vst3InstrumentTransport expected) {
  void* module = dlopen(verified.binary.canonical_binary.c_str(), RTLD_NOW | RTLD_NOLOAD);
  REQUIRE_NE(module, nullptr, "instrument module is not resident");
  using Reader = bool (*)(std::int64_t*, double*, double*, bool*);
  auto reader = reinterpret_cast<Reader>(dlsym(module, "OscillateFixtureTransport"));
  REQUIRE_NE(reader, nullptr, "instrument transport probe is unavailable");
  std::int64_t sample = -1;
  double quarter = -1.0;
  double tempo = -1.0;
  bool playing = !expected.playing;
  require(reader(&sample, &quarter, &tempo, &playing), "instrument transport probe failed");
  REQUIRE_EQ(sample, expected.project_sample, "instrument received the wrong block transport");
  REQUIRE_EQ(quarter, expected.quarter_position, "instrument received the wrong block transport");
  REQUIRE_EQ(tempo, expected.tempo_bpm, "instrument received the wrong block transport");
  REQUIRE_EQ(playing, expected.playing, "instrument received the wrong block transport");
  REQUIRE_EQ(dlclose(module), 0, "instrument transport probe could not close");
}

// The independent oracle compares the two stereo channels separately.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void check_audio(const std::vector<float>& left, const std::vector<float>& right,
                 const std::vector<Vst3NoteEvent>& events, bool& active, float& velocity,
                 float level) {
  std::size_t next = 0;
  for (std::size_t frame = 0; frame < left.size(); ++frame) {
    while (next < events.size() && events[next].sample_offset == frame) {
      active = events[next].kind == Vst3NoteKind::on;
      if (active)
        velocity = events[next].velocity;
      ++next;
    }
    const auto expected = active ? level * velocity : 0.0F;
    REQUIRE_LT(std::abs(left[frame] - expected), 0.00001F, "instrument left timing differs");
    REQUIRE_LT(std::abs(right[frame] + expected), 0.00001F, "instrument right timing differs");
  }
}

void exercise(const VerifiedVst3Effect& verified, std::size_t frames,
              Vst3ProcessingMode mode = Vst3ProcessingMode::realtime) {
  std::string error;
  auto loaded = load_verified_vst3_instrument(verified, 48000.0, 128, error, mode);
  REQUIRE_NE(loaded, nullptr, "verified instrument failed to load");
  REQUIRE_EQ(loaded->host().latency_samples(), 0, "unexpected instrument latency");
  require(loaded->host().activate(error), "instrument activation failed");
  std::vector<float> left(frames, -99.0F), right(frames, -99.0F);
  Vst3InstrumentBlock block{left, right};
  const Vst3InstrumentTransport first_transport{
      .project_sample = 0, .quarter_position = 0.0, .tempo_bpm = 125.0, .generation = 1};
  std::vector<Vst3NoteEvent> first{
      {.kind = Vst3NoteKind::on, .sample_offset = 5, .velocity = 0.25F, .note_id = 1},
      {.kind = Vst3NoteKind::off, .sample_offset = 19, .note_id = 1},
      {.kind = Vst3NoteKind::on,
       .sample_offset = static_cast<std::uint32_t>(frames - 1),
       .velocity = 0.75F,
       .note_id = 2}};
  bool active = false;
  float velocity = 0.0F;
  process_allocations = 0;
  counting_process_allocations = true;
  const auto first_status = loaded->host().process_instrument(block, first, first_transport);
  counting_process_allocations = false;
  REQUIRE_EQ(first_status, Vst3ProcessStatus::ok, "first instrument block failed or allocated");
  REQUIRE_EQ(process_allocations, 0, "first instrument block failed or allocated");
  check_audio(left, right, first, active, velocity, 0.5F);
  check_transport(verified, first_transport);

  std::vector<Vst3NoteEvent> second{
      {.kind = Vst3NoteKind::off, .sample_offset = 0, .note_id = 2},
      {.kind = Vst3NoteKind::on, .sample_offset = 0, .velocity = 0.5F, .note_id = 3},
      {.kind = Vst3NoteKind::off, .sample_offset = 8, .note_id = 3}};
  left.assign(frames, -99.0F);
  right.assign(frames, -99.0F);
  process_allocations = 0;
  counting_process_allocations = true;
  const Vst3InstrumentTransport second_transport{.project_sample =
                                                     static_cast<std::int64_t>(frames),
                                                 .quarter_position = 0.5,
                                                 .tempo_bpm = 130.0,
                                                 .generation = 1};
  const auto second_status = loaded->host().process_instrument(block, second, second_transport);
  counting_process_allocations = false;
  REQUIRE_EQ(second_status, Vst3ProcessStatus::ok, "second instrument block failed or allocated");
  REQUIRE_EQ(process_allocations, 0, "second instrument block failed or allocated");
  check_audio(left, right, second, active, velocity, 0.5F);
  check_transport(verified, second_transport);
  REQUIRE_EQ(loaded->host().process({}), Vst3ProcessStatus::invalid_block,
             "effect entry point accepted instrument");
  REQUIRE_EQ(loaded->host().process_instrument(
                 block,
                 std::array{Vst3NoteEvent{.sample_offset = static_cast<std::uint32_t>(frames)}},
                 second_transport),
             Vst3ProcessStatus::invalid_note_events, "out-of-block note was accepted");
  REQUIRE_EQ(loaded->host().process_instrument(
                 block,
                 std::array{Vst3NoteEvent{.sample_offset = 3}, Vst3NoteEvent{.sample_offset = 2}},
                 second_transport),
             Vst3ProcessStatus::invalid_note_events, "unordered notes were accepted");
  REQUIRE_EQ(loaded->host().process_instrument(block, std::array{Vst3NoteEvent{.channel = 1}},
                                               second_transport),
             Vst3ProcessStatus::invalid_note_events, "unsupported event channel was accepted");
  const std::vector<Vst3NoteEvent> excess(257);
  REQUIRE_EQ(loaded->host().process_instrument(block, excess, second_transport),
             Vst3ProcessStatus::note_capacity_exceeded, "note capacity was not enforced");
  REQUIRE_EQ(loaded->host().process_instrument(
                 block, {}, second_transport,
                 std::array{Vst3ParameterEvent{1, static_cast<std::uint32_t>(frames), 0.5}}),
             Vst3ProcessStatus::invalid_parameter_events,
             "out-of-block instrument parameter event was accepted");
  REQUIRE_EQ(loaded->host().process_instrument(
                 block, {}, second_transport,
                 std::array{Vst3ParameterEvent{1, 3, 0.5}, Vst3ParameterEvent{1, 2, 0.25}}),
             Vst3ProcessStatus::invalid_parameter_events,
             "unordered instrument parameter events were accepted");
  REQUIRE_EQ(loaded->host().process_instrument(block, {}, second_transport,
                                               std::array{Vst3ParameterEvent{2, 0, 0.5}}),
             Vst3ProcessStatus::parameter_unavailable, "unknown instrument parameter was accepted");
  loaded->host().suspend();
  auto state = loaded->host().save_component_state(error);
  if (!state || state->size() != sizeof(float))
    throw std::runtime_error("instrument state did not save");
  auto state_bytes = state.value();
  const float quarter = 0.25F;
  std::memcpy(state_bytes.data(), &quarter, sizeof(quarter));
  require(loaded->host().restore_component_state(state_bytes, error),
          "instrument state restore failed");
  require(loaded->host().activate(error), "restored instrument did not activate");
  const std::array one{Vst3NoteEvent{.kind = Vst3NoteKind::on, .velocity = 1.0F}};
  REQUIRE_EQ(loaded->host().process_instrument(block, one, second_transport), Vst3ProcessStatus::ok,
             "restored instrument first audible block failed");
  REQUIRE_LT(std::abs(left[0] - quarter), 0.00001F, "restored state not audible on first frame");
  // An explicit ID must be unique across pitch.
  const std::array unique_note{
      Vst3NoteEvent{.kind = Vst3NoteKind::on, .pitch = 60, .velocity = 1.0F, .note_id = 42}};
  REQUIRE_EQ(loaded->host().process_instrument(block, unique_note, second_transport),
             Vst3ProcessStatus::ok, "explicit-ID note did not play");
  const std::array repeated_id{
      Vst3NoteEvent{.kind = Vst3NoteKind::on, .pitch = 61, .velocity = 1.0F, .note_id = 42}};
  REQUIRE_EQ(loaded->host().process_instrument(block, repeated_id, second_transport),
             Vst3ProcessStatus::invalid_note_events, "duplicate explicit note ID was accepted");
  const Vst3InstrumentTransport seek_transport{
      .project_sample = 48000, .quarter_position = 4.25, .tempo_bpm = 96.0, .generation = 2};
  const std::array seek_note{
      Vst3NoteEvent{.kind = Vst3NoteKind::on, .sample_offset = 5, .velocity = 0.5F, .note_id = 9}};
  process_allocations = 0;
  counting_process_allocations = true;
  const auto seek_status = loaded->host().process_instrument(block, seek_note, seek_transport);
  counting_process_allocations = false;
  REQUIRE_EQ(seek_status, Vst3ProcessStatus::ok, "seek block failed or allocated");
  REQUIRE_EQ(process_allocations, 0, "seek block failed or allocated");
  REQUIRE_EQ(left[0], 0.0F, "old note survived seek or first new note was late");
  REQUIRE_EQ(left[4], 0.0F, "old note survived seek or first new note was late");
  REQUIRE_LT(std::abs(left[5] - 0.125F), 0.00001F,
             "old note survived seek or first new note was late");
  check_transport(verified, seek_transport);
  auto stopped = seek_transport;
  stopped.project_sample += static_cast<std::int64_t>(frames);
  stopped.playing = false;
  REQUIRE_EQ(loaded->host().process_instrument(block, {}, stopped), Vst3ProcessStatus::ok,
             "stop block could not flush active notes");
  REQUIRE_EQ(left[0], 0.0F, "stop left an instrument note audible");
  REQUIRE_EQ(left[frames - 1], 0.0F, "stop left an instrument note audible");
  check_transport(verified, stopped);
  REQUIRE_EQ(loaded->host().process_instrument(block, seek_note, stopped),
             Vst3ProcessStatus::invalid_transport, "stopped transport accepted new notes");
  auto invalid = seek_transport;
  invalid.tempo_bpm = std::numeric_limits<double>::quiet_NaN();
  REQUIRE_EQ(loaded->host().process_instrument(block, {}, invalid),
             Vst3ProcessStatus::invalid_transport, "nonfinite transport was accepted");
  invalid = seek_transport;
  invalid.project_sample = std::numeric_limits<std::int64_t>::max();
  REQUIRE_EQ(loaded->host().process_instrument(block, {}, invalid),
             Vst3ProcessStatus::invalid_transport, "overflowing project sample range was accepted");

  constexpr int iterations = 10000;
  process_allocations = 0;
  counting_process_allocations = true;
  const auto start = std::chrono::steady_clock::now();
  for (int index = 0; index < iterations; ++index) {
    if (loaded->host().process_instrument(block, {}, stopped) != Vst3ProcessStatus::ok) {
      counting_process_allocations = false;
      throw std::runtime_error("instrument transport benchmark block failed");
    }
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  counting_process_allocations = false;
  REQUIRE_EQ(process_allocations, 0, "steady instrument transport allocated");
  std::fprintf(
      stdout, "VST3 instrument transport %zu frames: %lld ns/block, %zu allocations\n", frames,
      static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() /
                             iterations),
      process_allocations.load());

  std::vector<Vst3NoteEvent> full_ledger;
  full_ledger.reserve(256);
  for (std::int32_t id = 0; id < 256; ++id)
    full_ledger.push_back({.kind = Vst3NoteKind::on, .note_id = id});
  auto full_transport = seek_transport;
  full_transport.generation = 3;
  REQUIRE_EQ(loaded->host().process_instrument(block, full_ledger, full_transport),
             Vst3ProcessStatus::ok, "bounded note ledger could not accept its capacity");
  ++full_transport.generation;
  REQUIRE_EQ(loaded->host().process_instrument(block, seek_note, full_transport),
             Vst3ProcessStatus::note_capacity_exceeded,
             "seek release plus new note exceeded capacity without rejection");
  REQUIRE_EQ(loaded->host().process_instrument(block, {}, full_transport), Vst3ProcessStatus::ok,
             "release-only block after capacity rejection failed");
  REQUIRE_EQ(left[0], 0.0F, "release-only block retained a voice");
  REQUIRE_EQ(left[frames - 1], 0.0F, "release-only block retained a voice");

  REQUIRE_EQ(loaded->host().process_instrument(block, seek_note, full_transport),
             Vst3ProcessStatus::ok, "pre-suspend note did not play");
  loaded->host().suspend();
  require(loaded->host().activate(error), "instrument reactivation failed");
  REQUIRE_EQ(loaded->host().process_instrument(block, seek_note, full_transport),
             Vst3ProcessStatus::ok, "first block after suspension failed");
  REQUIRE_EQ(left[0], 0.0F, "suspended voice leaked into first resumed block");
  REQUIRE_EQ(left[4], 0.0F, "suspended voice leaked into first resumed block");
  REQUIRE_LT(std::abs(left[5] - 0.125F), 0.00001F,
             "suspended voice leaked into first resumed block");
}
} // namespace

int main() {
  try {
    Vst3Scanner scanner(VST3_HELPER_PATH);
    auto receipt = scanner.scan(fs::path(VST3_INSTRUMENT_BUNDLE));
    REQUIRE_TRUE(receipt.ok(), "instrument fixture scanner receipt failed");
    REQUIRE_EQ(receipt.metadata.classes.size(), 1, "instrument fixture scanner receipt failed");
    VerifiedVst3Effect verified{receipt.metadata.classes.front(), receipt.binary};
    std::string error;
    require(!load_verified_vst3_effect(verified, 48000.0, 128, error),
            "effect host accepted instrument topology");
    exercise(verified, 64);
    exercise(verified, 128);
    REQUIRE_EQ(setenv("OSCILLATE_VST3_INSTRUMENT_FAIL", "require-offline", 1), 0,
               "cannot set offline instrument fixture mode");
    exercise(verified, 64, Vst3ProcessingMode::offline);
    REQUIRE_EQ(unsetenv("OSCILLATE_VST3_INSTRUMENT_FAIL"), 0,
               "cannot clear offline instrument fixture mode");
    REQUIRE_EQ(setenv("OSCILLATE_VST3_INSTRUMENT_FAIL", "two-event-buses", 1), 0,
               "cannot set topology failure mode");
    require(!load_verified_vst3_instrument(verified, 48000.0, 128, error),
            "two event buses were accepted");
    REQUIRE_EQ(unsetenv("OSCILLATE_VST3_INSTRUMENT_FAIL"), 0, "cannot clear topology failure mode");
    auto failing = load_verified_vst3_instrument(verified, 48000.0, 128, error);
    REQUIRE_TRUE(failing, "failure fixture activation failed");
    REQUIRE_TRUE(failing->host().activate(error), "failure fixture activation failed");
    std::array<float, 64> left{}, right{};
    left.fill(7.0F);
    right.fill(7.0F);
    REQUIRE_EQ(setenv("OSCILLATE_VST3_INSTRUMENT_FAIL", "process", 1), 0,
               "cannot set process failure mode");
    REQUIRE_EQ(failing->host().process_instrument({left, right}, {}, {}),
               Vst3ProcessStatus::plugin_error, "failed process was not quarantined");
    REQUIRE_EQ(unsetenv("OSCILLATE_VST3_INSTRUMENT_FAIL"), 0, "cannot clear process failure mode");
    REQUIRE_EQ(left[0], 0.0F, "failed process was not silenced");
    REQUIRE_EQ(right[0], 0.0F, "failed process was not silenced");
    REQUIRE_EQ(failing->host().process_instrument({left, right}, {}, {}),
               Vst3ProcessStatus::plugin_error, "faulted processor ran again");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
