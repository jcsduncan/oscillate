#include "oscillate/test/check.hpp"

#include <oscillate/application/routed_audio_source.hpp>
#include <oscillate/audio_io/pipewire_output.hpp>
#include <oscillate/native_dsp/effects/effect.hpp>
#include <oscillate/native_inserts/effect_binding.hpp>
#include <oscillate/persistence/store.hpp>
#include <oscillate/project/model.hpp>

#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <poll.h>
#include <unistd.h>

namespace {
namespace app = oscillate::application;
namespace audio = oscillate::audio_io;
namespace engine = oscillate::engine;
namespace foundation = oscillate::foundation;
namespace persistence = oscillate::persistence;
namespace native_dsp = oscillate::native_dsp;
namespace native_inserts = oscillate::native_inserts;
namespace project = oscillate::project;
using namespace std::chrono_literals;

class StoppedLease final : public project::StoppedTransportLease {};
class StoppedGate final : public project::TransportGate {
public:
  [[nodiscard]] foundation::Result<std::unique_ptr<project::StoppedTransportLease>>
  acquire_stopped() override {
    return foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::success(
        std::make_unique<StoppedLease>());
  }
};

using oscillate::test::take;

std::uint64_t positive_number(std::string_view text, std::uint64_t limit) {
  std::uint64_t value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value == 0 ||
      value > limit)
    throw std::runtime_error("invalid numeric playback option");
  return value;
}

const char* application_name(app::PlaybackApplication state) noexcept {
  switch (state) {
  case app::PlaybackApplication::pending:
    return "pending";
  case app::PlaybackApplication::active:
    return "active";
  case app::PlaybackApplication::failed:
    return "failed";
  }
  return "unknown";
}

void report(const app::RoutedPlaybackControl& control, const audio::TransportCursor& transport,
            audio::OutputStatus device, const app::RoutedAudioSource* source = nullptr) {
  const auto revision = control.status();
  const auto cursor = transport.completed_snapshot();
  std::cout << "model=" << revision.model_revision << " active=" << revision.active_graph_revision
            << " application=" << application_name(revision.application)
            << " sample=" << cursor.next_sample << " generation=" << cursor.generation
            << " device=" << static_cast<unsigned>(device.state)
            << " render_failures=" << device.render_failures;
  if (source) {
    const auto stream = source->status();
    std::cout << " stream_ready_generation=" << stream.ready_generation
              << " stream_missing_frames=" << stream.missing_frames
              << " stream_worker_failed=" << stream.worker_failed
              << " stream_worker_paused=" << stream.worker_paused;
  }
  std::cout << '\n';
  if (revision.failure)
    std::cout << "Graph application failed: " << revision.failure->message << '\n';
  if (source) {
    if (const auto failure = source->worker_failure())
      std::cout << "Source streaming failed: " << failure->message << '\n';
  }
  std::cout.flush();
}

class CommandInput {
public:
  template <typename Handler> void poll_lines(Handler&& handle) {
    if (closed_)
      return;
    pollfd input{STDIN_FILENO, static_cast<short>(POLLIN | POLLHUP), 0};
    const auto ready = ::poll(&input, 1, 0);
    if (ready < 0) {
      if (errno != EINTR)
        throw std::runtime_error("cannot poll routed control input");
      return;
    }
    if (ready == 0)
      return;
    std::array<char, 256> bytes{};
    const auto count = ::read(STDIN_FILENO, bytes.data(), bytes.size());
    if (count < 0) {
      if (errno != EINTR && errno != EAGAIN)
        throw std::runtime_error("cannot read routed control input");
      return;
    }
    if (count == 0) {
      closed_ = true;
      return;
    }
    for (std::size_t index = 0; index < static_cast<std::size_t>(count); ++index) {
      if (bytes[index] == '\n') {
        if (!dropping_)
          handle(std::string_view(line_));
        line_.clear();
        dropping_ = false;
      } else if (!dropping_) {
        if (line_.size() == 128) {
          std::cerr << "Routed command exceeds 128 bytes\n";
          line_.clear();
          dropping_ = true;
        } else {
          line_.push_back(bytes[index]);
        }
      }
    }
  }

private:
  std::string line_;
  bool closed_ = false;
  bool dropping_ = false;
};

enum class EditPhase : std::uint8_t { idle, awaiting_activation };

double parse_gain(std::string_view text) {
  double value = 0.0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      !std::isfinite(value) || value < 0.0 || value > 4.0)
    throw std::runtime_error("send gain must be finite and between 0 and 4");
  return value;
}

int run(int argc, char** argv) {
  if (argc < 4 || argc > 5 || std::string_view(argv[1]) != "--pipewire") {
    std::cerr << "Usage: oscillate_routed_player --pipewire PROJECT SECONDS [TARGET_SERIAL]\n"
                 "Audio-only, same-rate managed WAV clips (AIFF/FLAC/MP3 with codec adapter); "
                 "bounded worker-prefetched streaming.\n";
    return 2;
  }
  const auto seconds = positive_number(argv[3], 600);
  const auto target =
      argc == 5 ? std::optional<std::uint64_t>(positive_number(argv[4], UINT64_MAX)) : std::nullopt;
  auto store = take(persistence::ProjectStore::open_writer(std::filesystem::path(argv[2]), false));
  auto loaded = take(store.load_current());
  StoppedGate gate;
  auto model = take(project::ProjectModel::open(std::move(loaded.snapshot), gate));
  const auto snapshot = model.snapshot();
  // The graph and stream rings have explicit bounds. This is not reference-
  // workload qualification.
  constexpr std::size_t max_block = 8192;
  constexpr std::uint64_t buffer_budget = 64ULL * 1024 * 1024;
  constexpr std::uint64_t history_budget = 64ULL * 1024 * 1024;
  const engine::RoutedMixerLimits limits{{1, 8192}, max_block, buffer_budget, history_budget};
  app::RoutedPlaybackControl control(model, limits);
  audio::TransportCursor transport;
  auto graph = control.submit_current();
  if (!graph) {
    report(control, transport, {});
    throw std::runtime_error(graph.error().message);
  }
  constexpr std::size_t stream_frame_budget = std::size_t{512} * 1024;
  auto prepared = app::RoutedAudioSource::prepare(argv[2], *snapshot, control, stream_frame_budget);
  if (!prepared) {
    std::cerr << "model=" << snapshot->revision
              << " active=0 source_preparation=failed: " << prepared.error().message << '\n';
    return 1;
  }
  auto source = std::move(prepared).value();
  std::atomic<bool> allowed{false};
  const audio::TransportPermission permission{
      &allowed, [](const void* context) noexcept {
        return static_cast<const std::atomic<bool>*>(context)->load(std::memory_order_acquire);
      }};
  audio::PipeWireOutput output(source->render_source(), transport, permission);
  take(output.start(snapshot->sample_rate, 128, target));

  const auto ready_by = std::chrono::steady_clock::now() + 5s;
  auto status = output.status();
  while (std::chrono::steady_clock::now() < ready_by) {
    status = output.status();
    if (status.state == audio::OutputState::error ||
        status.state == audio::OutputState::disconnected)
      throw std::runtime_error("PipeWire output failed before playback");
    if (status.state == audio::OutputState::streaming &&
        status.negotiated_client_rate == snapshot->sample_rate && status.observed_graph_rate &&
        status.observed_block_frames && *status.observed_block_frames <= max_block)
      break;
    std::this_thread::sleep_for(10ms);
  }
  if (status.state != audio::OutputState::streaming ||
      status.negotiated_client_rate != snapshot->sample_rate || !status.observed_graph_rate ||
      !status.observed_block_frames || *status.observed_block_frames > max_block)
    throw std::runtime_error(
        "PipeWire stream did not negotiate the project rate and bounded block");
  allowed.store(true, std::memory_order_release);
  std::cout << "ready: gain SEND_ID VALUE | fail-master-latency | retry | undo | status\n";
  for (const auto& track : snapshot->tracks) {
    for (const auto& send : track.sends)
      std::cout << "send=" << send.id.to_string() << " gain=" << send.gain << '\n';
  }
  std::cout.flush();
  CommandInput input;
  EditPhase phase = EditPhase::idle;
  auto save_edit = [&](project::AppliedRevision applied, std::string_view label) {
    const auto outcome = take(store.save(*applied.snapshot, persistence::SaveKind::explicit_save));
    if (outcome.status != persistence::SaveStatus::committed)
      throw std::runtime_error("accepted routed edit has an uncertain save; inspect the project");
    const auto reopened = take(store.load_current());
    if (reopened.snapshot.revision != applied.snapshot->revision)
      throw std::runtime_error("saved routed edit reopened at the wrong revision");
    // An edit switches live at a block boundary or is refused at once; the
    // transport never holds for it.
    const auto begun = control.begin_edit(applied, label);
    if (!begun)
      std::cerr << "Routed graph build: " << begun.error().message << '\n';
    phase = begun ? EditPhase::awaiting_activation : EditPhase::idle;
    report(control, transport, output.status(), source.get());
  };
  auto command = [&](std::string_view line) {
    try {
      if (line == "status") {
        control.observe();
        report(control, transport, output.status(), source.get());
        return;
      }
      if (phase != EditPhase::idle)
        throw std::runtime_error("wait for the pending routed edit to finish");
      if (line == "retry") {
        const auto retried = control.retry();
        if (!retried)
          std::cerr << "Routed graph build: " << retried.error().message << '\n';
        phase = retried ? EditPhase::awaiting_activation : EditPhase::idle;
        report(control, transport, output.status(), source.get());
        return;
      }
      if (line == "undo") {
        save_edit(take(model.undo()), "undo");
        return;
      }
      if (line == "fail-master-latency") {
        // Ceiling's lookahead changes the master path latency, which a
        // playing graph cannot adopt: the edit is saved but not audible.
        const auto* ceiling = native_dsp::effects::find_engine("ceiling");
        if (ceiling == nullptr)
          throw std::runtime_error("Ceiling is not registered");
        save_edit(take(model.apply({project::AddNativeEffect{
                      project::MasterSignalPath{}, 0,
                      take(native_inserts::native_effect_patch_for_preset(*ceiling, {}))}})),
                  "latency-changing master effect");
        return;
      }
      std::istringstream words{std::string(line)};
      std::string action;
      std::string id_text;
      std::string gain_text;
      std::string extra;
      if (!(words >> action >> id_text >> gain_text) || words >> extra || action != "gain")
        throw std::runtime_error(
            "expected gain SEND_ID VALUE, fail-master-latency, retry, undo or status");
      const auto send = take(project::SendId::parse(id_text));
      save_edit(take(model.apply({project::SetSendGain{send, parse_gain(gain_text)}})),
                "send gain");
    } catch (const std::exception& error) {
      std::cerr << "Routed command: " << error.what() << '\n';
      if (model.snapshot()->revision != control.status().model_revision)
        throw;
    }
  };
  const auto finish_by = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  while (std::chrono::steady_clock::now() < finish_by) {
    std::this_thread::sleep_for(10ms);
    control.observe();
    status = output.status();
    if (status.state == audio::OutputState::disconnected ||
        status.state == audio::OutputState::error || source->status().worker_failed)
      break;
    if (phase == EditPhase::awaiting_activation &&
        control.status().application == app::PlaybackApplication::active) {
      phase = EditPhase::idle;
      const auto completed = transport.completed_snapshot();
      std::cout << "activated-by sample=" << completed.next_sample
                << " generation=" << completed.generation << '\n';
      report(control, transport, status, source.get());
    } else if (phase == EditPhase::awaiting_activation &&
               control.status().application == app::PlaybackApplication::failed) {
      phase = EditPhase::idle; // Withdrawn: the old graph played on.
      report(control, transport, status, source.get());
    }
    input.poll_lines(command);
  }
  allowed.store(false, std::memory_order_release);
  output.stop();
  control.observe();
  report(control, transport, status, source.get());
  if (status.state == audio::OutputState::disconnected ||
      status.state == audio::OutputState::error || status.render_failures != 0 ||
      source->status().missing_frames != 0 || source->status().worker_failed ||
      control.status().application == app::PlaybackApplication::pending)
    throw std::runtime_error(
        "routed PipeWire playback ended with an inactive graph or device error");
  return 0;
}
} // namespace

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "Routed playback: " << error.what() << '\n';
    return 1;
  }
}
