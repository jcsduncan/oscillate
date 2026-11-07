#include "../../apps/daw_quick/src/realtime_export_run.hpp"

#include "oscillate/application/project_session.hpp"
#include "oscillate/project/model.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"

#include <jack/jack.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
namespace app = oscillate::application;
namespace audio = oscillate::audio_io;
namespace exporting = oscillate::exporting;
namespace foundation = oscillate::foundation;
namespace fs = std::filesystem;
namespace project = oscillate::project;
namespace quick = oscillate::quick;

using oscillate::test::take;

std::uint32_t number(std::string_view text, std::uint32_t maximum) {
  if (text.empty())
    throw std::runtime_error("empty numeric argument");
  std::uint32_t result = 0;
  for (const char digit : text) {
    if (digit < '0' || digit > '9' || static_cast<std::uint32_t>(digit - '0') > maximum ||
        result > (maximum - static_cast<std::uint32_t>(digit - '0')) / 10)
      throw std::runtime_error("invalid or oversized numeric argument");
    result = result * 10 + static_cast<std::uint32_t>(digit - '0');
  }
  if (result == 0)
    throw std::runtime_error("numeric argument must be positive");
  return result;
}

void put16(std::ofstream& out, std::uint16_t value) {
  const char bytes[]{static_cast<char>(value), static_cast<char>(value >> 8U)};
  out.write(bytes, sizeof(bytes));
}
void put32(std::ofstream& out, std::uint32_t value) {
  put16(out, static_cast<std::uint16_t>(value));
  put16(out, static_cast<std::uint16_t>(value >> 16U));
}

// A quiet managed source: 0.5 s of silence allows the selected JACK
// connections to settle; the sine is intentionally below normal programme
// level. The selected bar end cuts it while it is still playing.
fs::path fixture_source(const fs::path& project_root) {
  const auto path = project_root.parent_path() / "qualification-source.wav";
  constexpr std::uint32_t frames = 48'000 * 3;
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out)
    throw std::runtime_error("cannot create qualification WAV");
  out.write("RIFF", 4);
  put32(out, 36 + frames * 2);
  out.write("WAVEfmt ", 8);
  put32(out, 16);
  put16(out, 1);
  put16(out, 1);
  put32(out, 48'000);
  put32(out, 96'000);
  put16(out, 2);
  put16(out, 16);
  out.write("data", 4);
  put32(out, frames * 2);
  for (std::uint32_t index = 0; index < frames; ++index) {
    const double phase = 2.0 * std::numbers::pi * 220.0 * index / 48'000.0;
    const auto sample = index < 24'000
                            ? 0
                            : static_cast<std::int16_t>(std::clamp(
                                  static_cast<int>(2'048.0 * std::sin(phase)), -32'768, 32'767));
    put16(out, static_cast<std::uint16_t>(sample));
  }
  if (!out)
    throw std::runtime_error("qualification WAV write failed");
  return path;
}

int create_fixture(const fs::path& root) {
  if (fs::exists(root))
    throw std::runtime_error("fixture project path already exists");
  fs::create_directories(root.parent_path());
  const auto source = fixture_source(root);
  auto session = take(app::ProjectSession::create(root));
  const int fd = ::open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0)
    throw std::runtime_error("cannot reopen qualification WAV");
  auto imported = session->imports().import_wav_audio(fd);
  (void)::close(fd);
  (void)take(std::move(imported));
  const auto synth = take(session->instruments().add_synth_track());
  take(session->instruments().select_native_instrument(synth, "prism", 2));
  take(session->midi_clips().add_midi_clip(synth, {0}));
  const auto clip = session->snapshot().tracks.back().midi_clips.front().id;
  take(session->notes().add_note(clip, {oscillate::timeline::ticks_per_quarter}, 48));
  const project::SignalPathId master = project::MasterSignalPath{};
  (void)take(session->effects().add_effect_engine(master, 0, "expanse"));
  take(session->saving().save());
  std::cout << "fixture_ready sample_rate=oscillate::test::sample_rate_48k end_bar=1 tracks="
            << session->snapshot().tracks.size() << '\n';
  return 0;
}

// Real JACK API plus one explicit connection to each named input. This is
// qualification-only plumbing; the graph, callback, capture, tail planning
// and publication remain the production Quick path.
struct NamedJack {
  std::string left_name;
  std::string right_name;
  jack_port_t* left = nullptr;
  jack_port_t* right = nullptr;
  audio::JackApi api{};

  explicit NamedJack(std::string left_port, std::string right_port)
      : left_name(std::move(left_port)), right_name(std::move(right_port)) {
    api = {this,   open,   close,   activate,    deactivate,  rate, size,
           output, buffer, process, rate_change, size_change, xrun, shutdown};
  }
  [[nodiscard]] bool only_named_connections() const noexcept {
    return left != nullptr && right != nullptr && jack_port_connected(left) == 1 &&
           jack_port_connected(right) == 1 &&
           jack_port_connected_to(left, left_name.c_str()) != 0 &&
           jack_port_connected_to(right, right_name.c_str()) != 0;
  }
  static NamedJack& self(void* context) noexcept { return *static_cast<NamedJack*>(context); }
  static void* open(void*, const char* name) noexcept {
    jack_status_t status = JackFailure;
    return jack_client_open(name, JackNoStartServer, &status);
  }
  static int close(void* context, void* client) noexcept {
    auto& named = self(context);
    named.left = nullptr;
    named.right = nullptr;
    return jack_client_close(static_cast<jack_client_t*>(client));
  }
  static int activate(void* context, void* opaque) noexcept {
    auto& named = self(context);
    auto* client = static_cast<jack_client_t*>(opaque);
    if (jack_activate(client) != 0 || named.left == nullptr || named.right == nullptr)
      return -1;
    if (jack_connect(client, jack_port_name(named.left), named.left_name.c_str()) != 0 ||
        jack_connect(client, jack_port_name(named.right), named.right_name.c_str()) != 0) {
      (void)jack_deactivate(client);
      return -1;
    }
    return 0;
  }
  static int deactivate(void*, void* client) noexcept {
    return jack_deactivate(static_cast<jack_client_t*>(client));
  }
  static std::uint32_t rate(void*, void* client) noexcept {
    return jack_get_sample_rate(static_cast<jack_client_t*>(client));
  }
  static std::uint32_t size(void*, void* client) noexcept {
    return jack_get_buffer_size(static_cast<jack_client_t*>(client));
  }
  static void* output(void* context, void* opaque, const char* name) noexcept {
    auto& named = self(context);
    auto* port = jack_port_register(static_cast<jack_client_t*>(opaque), name,
                                    JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    if (name[0] == 'l')
      named.left = port;
    else if (name[0] == 'r')
      named.right = port;
    return port;
  }
  static float* buffer(void*, void* port, std::uint32_t frames) noexcept {
    return static_cast<float*>(jack_port_get_buffer(static_cast<jack_port_t*>(port), frames));
  }
  static int process(void*, void* opaque, audio::JackApi::Process callback, void* data) noexcept {
    return jack_set_process_callback(static_cast<jack_client_t*>(opaque), callback, data);
  }
  static int rate_change(void*, void* opaque, audio::JackApi::Change callback,
                         void* data) noexcept {
    return jack_set_sample_rate_callback(static_cast<jack_client_t*>(opaque), callback, data);
  }
  static int size_change(void*, void* opaque, audio::JackApi::Change callback,
                         void* data) noexcept {
    return jack_set_buffer_size_callback(static_cast<jack_client_t*>(opaque), callback, data);
  }
  static int xrun(void*, void* opaque, audio::JackApi::Event callback, void* data) noexcept {
    return jack_set_xrun_callback(static_cast<jack_client_t*>(opaque), callback, data);
  }
  static void shutdown(void*, void* opaque, audio::JackApi::Shutdown callback,
                       void* data) noexcept {
    jack_on_shutdown(static_cast<jack_client_t*>(opaque), callback, data);
  }
};

void inspect_server(std::uint32_t requested, const NamedJack& named) {
  jack_status_t status = JackFailure;
  auto* client = jack_client_open("LDW Export Preflight", JackNoStartServer, &status);
  if (!client)
    throw std::runtime_error("JACK server is unavailable; no server was started");
  const auto close =
      std::unique_ptr<jack_client_t, decltype(&jack_client_close)>(client, jack_client_close);
  if (jack_get_sample_rate(client) != 48'000 || jack_get_buffer_size(client) != requested)
    throw std::runtime_error("JACK must already run at 48 kHz and the requested 64/128 frames");
  for (const auto& name : {named.left_name, named.right_name}) {
    auto* port = jack_port_by_name(client, name.c_str());
    if (!port || (jack_port_flags(port) & JackPortIsInput) == 0 ||
        jack_port_type(port) == nullptr ||
        std::string_view(jack_port_type(port)) != JACK_DEFAULT_AUDIO_TYPE)
      throw std::runtime_error("named JACK destination is not an audio input port: " + name);
  }
}

int run_export(const fs::path& root, const fs::path& target, std::string left, std::string right,
               std::uint32_t buffer_frames, std::uint32_t end_bar) {
  if (left == right || left.empty() || right.empty() ||
      (buffer_frames != 64 && buffer_frames != 128) || end_bar > 8)
    throw std::runtime_error("name two distinct JACK inputs and use 64/128 frames, bars 1..8");
  NamedJack named(std::move(left), std::move(right));
  inspect_server(buffer_frames, named);
  auto session = take(app::ProjectSession::open(root));
  const auto& snapshot = session->snapshot();
  if (snapshot.sample_rate != 48'000)
    throw std::runtime_error("physical export qualification requires a 48 kHz project");
  const auto tick = take(snapshot.meter.tick_at({static_cast<std::int64_t>(end_bar) + 1, 1, 0}));
  const auto sample = take(snapshot.tempo.tick_to_sample(tick, snapshot.sample_rate)).value;
  if (sample <= 0 || sample > 15LL * 48'000)
    throw std::runtime_error("selected end exceeds the 15-second qualification bound");
  auto run = take(quick::RealtimeExportRun::begin(
      root, snapshot, take(session->reserve_playback_start()), target,
      static_cast<std::uint64_t>(sample), buffer_frames, {}, &named.api));
  if (!named.only_named_connections())
    throw std::runtime_error("JACK outputs are not connected only to the named inputs");
  std::atomic<bool> cancelled{false};
  take(run->wait_until_ready(cancelled, {}));
  if (!named.only_named_connections())
    throw std::runtime_error("named JACK output connections changed during export");
  const auto expected = run->expected_frames();
  const auto captured = run->frames_captured();
  const auto tail = run->tail_frames();
  const auto latency = run->output_latency_frames();
  const auto published =
      take(run->finish_if_current(snapshot.revision, session->snapshot().revision, false));
  const auto device = run->device_status();
  if (!device.callback_timing_distribution || !device.server_rate || !device.server_buffer_frames ||
      *device.server_rate != 48'000 || *device.server_buffer_frames != buffer_frames ||
      captured != expected || published != exporting::PublishOutcome::durable)
    throw std::runtime_error("JACK export lacked complete durable qualification evidence");
  const auto& timing = *device.callback_timing_distribution;
  std::uint64_t blocks = 0;
  std::uint64_t over_period = 0;
  for (std::size_t index = 0; index < timing.bins.size(); ++index) {
    blocks += timing.bins[index];
    if (index > 4)
      over_period += timing.bins[index];
  }
  std::cout << "jack_export_result published=durable rate=" << *device.server_rate
            << " requested_frames=" << buffer_frames
            << " server_frames=" << *device.server_buffer_frames << " expected_frames=" << expected
            << " captured_frames=" << captured << " tail_frames=" << tail
            << " latency_frames=" << latency << " callback_blocks=" << blocks
            << " over_period_blocks=" << over_period << " xruns=" << device.xrun_notifications
            << " render_failures=" << device.render_failures
            << " invalid_buffers=" << device.invalid_buffers
            << " oversized_blocks=" << device.oversized_blocks
            << " source_health=passed named_connections=passed\n";
  return 0;
}

int main_run(int argc, char** argv) {
  if (argc == 3 && std::string_view(argv[1]) == "create-fixture")
    return create_fixture(argv[2]);
  if (argc == 8 && std::string_view(argv[1]) == "run")
    return run_export(argv[2], argv[3], argv[4], argv[5], number(argv[6], 128), number(argv[7], 8));
  std::cerr << "Usage: oscillate_physical_jack_export create-fixture PROJECT\n"
               "   or: oscillate_physical_jack_export run PROJECT TARGET.wav "
               "LEFT_JACK_INPUT RIGHT_JACK_INPUT 64|128 END_BAR\n";
  return 2;
}
} // namespace

int main(int argc, char** argv) {
  try {
    return main_run(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "jack_export_error " << error.what() << '\n';
    return 1;
  }
}
