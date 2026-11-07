#include "../../../apps/daw_quick/src/realtime_export_run.hpp"

#include "oscillate/application/project_session.hpp"
#include "oscillate/export/offline_master.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/wait.hpp"
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
#include "oscillate/plugins/vst3_scan.hpp"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <new>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {
// Summed across every fake JACK callback, whichever thread ran it.
std::atomic<std::size_t> callback_allocations{0};
} // namespace

namespace {
namespace app = oscillate::application;
namespace audio = oscillate::audio_io;
namespace quick = oscillate::quick;
namespace fs = std::filesystem;

using oscillate::test::require;

template <class T> T take(oscillate::foundation::Result<T> result) {
  if (!result)
    throw std::runtime_error(result.error().message);
  return std::move(result).value();
}

struct Directory {
  Directory() {
    auto pattern = fs::temp_directory_path().string() + "/oscillate-quick-jack-XXXXXX";
    auto* created = ::mkdtemp(pattern.data());
    require(created != nullptr, "test directory creation failed");
    root = created;
  }
  ~Directory() {
    std::error_code ignored;
    fs::remove_all(root, ignored);
  }
  fs::path root;
};

void put16(std::vector<std::uint8_t>& data, std::uint16_t value) {
  data.push_back(static_cast<std::uint8_t>(value));
  data.push_back(static_cast<std::uint8_t>(value >> 8U));
}
void put32(std::vector<std::uint8_t>& data, std::uint32_t value) {
  put16(data, static_cast<std::uint16_t>(value));
  put16(data, static_cast<std::uint16_t>(value >> 16U));
}
fs::path fixture(const fs::path& root) {
  const auto path = root / "source.wav";
  std::vector<std::uint8_t> data{'R', 'I', 'F', 'F'};
  put32(data, 36 + 4096 * 2);
  data.insert(data.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  put32(data, 16);
  put16(data, 1);
  put16(data, 1);
  put32(data, 48000);
  put32(data, 96000);
  put16(data, 2);
  put16(data, 16);
  data.insert(data.end(), {'d', 'a', 't', 'a'});
  put32(data, 4096 * 2);
  for (int index = 0; index < 4096; ++index)
    put16(data, 8192);
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  require(out.good(), "fixture write failed");
  return path;
}

// Mono 16-bit source: `silent` leading zero frames, then a 997 Hz sine at
// half scale. The onset gives an independent timing oracle.
fs::path tone_fixture(const fs::path& root, int frames, int silent) {
  const auto path = root / "tone.wav";
  std::vector<std::uint8_t> data{'R', 'I', 'F', 'F'};
  put32(data, 36 + static_cast<std::uint32_t>(frames) * 2);
  data.insert(data.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  put32(data, 16);
  put16(data, 1);
  put16(data, 1);
  put32(data, 48000);
  put32(data, 96000);
  put16(data, 2);
  put16(data, 16);
  data.insert(data.end(), {'d', 'a', 't', 'a'});
  put32(data, static_cast<std::uint32_t>(frames) * 2);
  for (int index = 0; index < frames; ++index) {
    const double phase = 2.0 * std::numbers::pi * 997.0 * (index - silent + 0.5) / 48'000.0;
    const auto value =
        index < silent ? 0 : static_cast<int>(std::lround(16'384.0 * std::sin(phase)));
    put16(data, static_cast<std::uint16_t>(static_cast<std::int16_t>(value)));
  }
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  require(out.good(), "fixture write failed");
  return path;
}

std::vector<char> bytes(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

struct StereoWav {
  std::vector<float> left, right;
};

// Independent of the writers: walks RIFF chunks and decodes float32 stereo.
StereoWav read_float_wav(const fs::path& path) {
  const auto data = bytes(path);
  const auto u16 = [&](std::size_t at) {
    require(at + 2 <= data.size(), "WAV truncated");
    return static_cast<std::uint16_t>(static_cast<std::uint8_t>(data[at]) |
                                      static_cast<std::uint8_t>(data[at + 1]) << 8U);
  };
  const auto u32 = [&](std::size_t at) {
    return static_cast<std::uint32_t>(u16(at)) | static_cast<std::uint32_t>(u16(at + 2)) << 16U;
  };
  require(data.size() >= 12 && std::memcmp(data.data(), "RIFF", 4) == 0 &&
              std::memcmp(data.data() + 8, "WAVE", 4) == 0,
          "not a RIFF/WAVE file");
  bool float_stereo = false;
  for (std::size_t at = 12; at + 8 <= data.size();) {
    const auto size = u32(at + 4);
    const auto body = at + 8;
    require(body + size <= data.size(), "WAV chunk exceeds file");
    if (std::memcmp(data.data() + at, "fmt ", 4) == 0) {
      const auto tag = u16(body);
      const bool extensible_float = tag == 0xFFFEU && size >= 26 && u16(body + 24) == 3;
      float_stereo = (tag == 3 || extensible_float) && u16(body + 2) == 2 &&
                     u32(body + 4) == 48'000 && u16(body + 14) == 32;
    } else if (std::memcmp(data.data() + at, "data", 4) == 0) {
      require(float_stereo && size % 8 == 0, "WAV is not 48 kHz float32 stereo");
      StereoWav wav;
      wav.left.resize(size / 8);
      wav.right.resize(size / 8);
      for (std::size_t frame = 0; frame < wav.left.size(); ++frame) {
        std::memcpy(&wav.left[frame], data.data() + body + frame * 8, 4);
        std::memcpy(&wav.right[frame], data.data() + body + frame * 8 + 4, 4);
      }
      return wav;
    }
    at = body + size + (size & 1U);
  }
  throw std::runtime_error("WAV has no data chunk");
}

std::size_t onset(const StereoWav& wav) {
  for (std::size_t frame = 0; frame < wav.left.size(); ++frame)
    if (std::fabs(wav.left[frame]) > 1e-6F || std::fabs(wav.right[frame]) > 1e-6F)
      return frame;
  return wav.left.size();
}

struct FakeJack {
  audio::JackApi calls{};
  audio::JackApi::Process process = nullptr;
  audio::JackApi::Event xrun = nullptr;
  void* process_context = nullptr;
  std::array<float, 128> left{}, right{};
  bool active = false;
  unsigned opens = 0;
  unsigned stops = 0;
  unsigned closes = 0;
  std::uint32_t block_frames = 64;

  static FakeJack& self(void* context) noexcept { return *static_cast<FakeJack*>(context); }
  static void* open(void* context, const char*) noexcept {
    ++self(context).opens;
    return context;
  }
  static int close(void* context, void*) noexcept {
    ++self(context).closes;
    return 0;
  }
  static int activate(void* context, void*) noexcept {
    self(context).active = true;
    return 0;
  }
  static int deactivate(void* context, void*) noexcept {
    auto& jack = self(context);
    jack.active = false;
    ++jack.stops;
    return 0;
  }
  static std::uint32_t rate(void*, void*) noexcept { return 48'000; }
  static std::uint32_t size(void* context, void*) noexcept { return self(context).block_frames; }
  static void* port(void* context, void*, const char* name) noexcept {
    auto& jack = self(context);
    return name[0] == 'l' ? static_cast<void*>(jack.left.data())
                          : static_cast<void*>(jack.right.data());
  }
  static float* buffer(void*, void* port, std::uint32_t) noexcept {
    return static_cast<float*>(port);
  }
  static int set_process(void* context, void*, audio::JackApi::Process process,
                         void* data) noexcept {
    self(context).process = process;
    self(context).process_context = data;
    return 0;
  }
  static int set_change(void*, void*, audio::JackApi::Change, void*) noexcept { return 0; }
  static int set_xrun(void* context, void*, audio::JackApi::Event xrun, void*) noexcept {
    self(context).xrun = xrun;
    return 0;
  }
  static void set_shutdown(void*, void*, audio::JackApi::Shutdown, void*) noexcept {}

  const audio::JackApi* api() noexcept {
    calls = {this, open,   close,       activate,   deactivate, rate,     size,
             port, buffer, set_process, set_change, set_change, set_xrun, set_shutdown};
    return &calls;
  }
  void run() noexcept {
    require(active && process, "fake JACK is not active");
    counted_allocations = 0;
    counting_allocations = true;
    (void)process(block_frames, process_context);
    counting_allocations = false;
    callback_allocations.fetch_add(counted_allocations, std::memory_order_relaxed);
  }
  void emit_xrun() noexcept { (void)xrun(process_context); }
};

std::unique_ptr<app::ProjectSession> project(const fs::path& root, const fs::path& source) {
  auto session = take(app::ProjectSession::create(root));
  const auto fd = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "fixture open failed");
  auto imported = session->imports().import_wav_audio(fd);
  (void)::close(fd);
  require(imported.has_value(), "audio import failed");
  require(session->saving().save().has_value(), "save failed");
  return session;
}

class StoppedGate final : public oscillate::project::TransportGate {
  class Lease final : public oscillate::project::StoppedTransportLease {};

public:
  oscillate::foundation::Result<std::unique_ptr<oscillate::project::StoppedTransportLease>>
  acquire_stopped() override {
    return oscillate::foundation::
        Result<std::unique_ptr<oscillate::project::StoppedTransportLease>>::success(
            std::make_unique<Lease>());
  }
};

void drive(FakeJack& jack, quick::RealtimeExportRun& run, std::uint64_t expected) {
  require(run.expected_frames() == expected, "run expects a different content-plus-tail length");
  oscillate::test::wait_until(
      [&] {
        jack.run();
        return run.interval_captured();
      },
      "fake JACK captured the interval");
  require(run.interval_captured() && run.frames_captured() == expected,
          "fake JACK did not capture exact interval");
}

void saved_project_exact_interval_and_control_finish() {
  Directory directory;
  const auto root = directory.root / "song";
  auto session = project(root, fixture(directory.root));
  const auto saved = session->snapshot();
  session.reset();
  session = take(app::ProjectSession::open(root));
  require(session->snapshot().revision == saved.revision, "reopen changed revision");
  FakeJack jack;
  auto run = take(quick::RealtimeExportRun::begin(
      root, session->snapshot(), take(session->reserve_playback_start()),
      directory.root / "render.wav", 65, 64, {}, jack.api()));
  require(run->tail_frames() == 0 && run->output_latency_frames() == 0,
          "an insert-free project has no tail or output latency");
  drive(jack, *run, 65);
  std::atomic<bool> cancelled{false};
  require(run->wait_until_ready(cancelled, {}).has_value(),
          "worker did not accept completed interval");
  require(jack.left[1] == 0.0F, "device output continued after export endpoint");
  auto result = run->finish_if_current(saved.revision, session->snapshot().revision, false);
  // Control thread, after callbacks stop.
  require(result.has_value() && jack.stops == 1 && jack.closes == 1,
          "control-thread finish did not join JACK and publish");
  const auto wav = bytes(directory.root / "render.wav");
  require(wav.size() == 56 + 65 * 8, "WAV interval frame count is wrong");
  require(callback_allocations.load() == 0, "JACK callback allocated");
  run.reset();
  require(session->exports()
              .prepare_wav_master_export(
                  {directory.root / "after.wav", oscillate::exporting::WavFormat::float32})
              .has_value(),
          "export did not release project gate");
}

void xrun_and_cancellation_preserve_target() {
  Directory directory;
  const auto root = directory.root / "song";
  auto session = project(root, fixture(directory.root));
  const auto target = directory.root / "render.wav";
  {
    std::ofstream old(target, std::ios::binary);
    old << "previous master";
  }
  const auto original = bytes(target);
  {
    FakeJack jack;
    auto run = take(quick::RealtimeExportRun::begin(root, session->snapshot(),
                                                    take(session->reserve_playback_start()), target,
                                                    64, 64, {}, jack.api()));
    drive(jack, *run, 64);
    jack.emit_xrun();
    require(!run->finish(), "xrun published a target");
    require(bytes(target) == original, "xrun changed old target");
  }
  {
    FakeJack jack;
    auto run = take(quick::RealtimeExportRun::begin(root, session->snapshot(),
                                                    take(session->reserve_playback_start()), target,
                                                    128, 64, {}, jack.api()));
    jack.run();
    run.reset(); // cancellation via owner retirement, without finish
    require(bytes(target) == original, "cancel changed old target");
  }
  {
    FakeJack jack;
    auto run = take(quick::RealtimeExportRun::begin(root, session->snapshot(),
                                                    take(session->reserve_playback_start()), target,
                                                    64, 64, {}, jack.api()));
    drive(jack, *run, 64);
    require(!run->finish_if_current(session->snapshot().revision, session->snapshot().revision + 1,
                                    false),
            "stale project revision published a target");
    require(bytes(target) == original, "stale revision changed old target");
  }
  {
    FakeJack jack;
    auto run = take(quick::RealtimeExportRun::begin(root, session->snapshot(),
                                                    take(session->reserve_playback_start()), target,
                                                    64, 64, {}, jack.api()));
    drive(jack, *run, 64);
    require(
        !run->finish_if_current(session->snapshot().revision, session->snapshot().revision, true),
        "cancelled run published a target");
    require(bytes(target) == original, "cancelled run changed old target");
  }
}

float max_difference(const StereoWav& a, const StereoWav& b, std::size_t frames) {
  float worst = 0.0F;
  for (std::size_t frame = 0; frame < frames; ++frame)
    worst = std::max({worst, std::fabs(a.left[frame] - b.left[frame]),
                      std::fabs(a.right[frame] - b.right[frame])});
  return worst;
}

struct JackRender {
  StereoWav wav;
  std::uint64_t tail_frames;
  std::uint32_t latency_frames;
};

JackRender jack_render(const fs::path& root, app::ProjectSession& session, const fs::path& target,
                       std::uint64_t end, std::uint32_t block) {
  const auto revision = session.snapshot().revision;
  FakeJack jack;
  jack.block_frames = block;
  auto run = take(quick::RealtimeExportRun::begin(root, session.snapshot(),
                                                  take(session.reserve_playback_start()), target,
                                                  end, block, {}, jack.api()));
  drive(jack, *run, end + run->tail_frames());
  std::atomic<bool> cancelled{false};
  require(run->wait_until_ready(cancelled, {}).has_value(), "worker rejected interval");
  require(run->finish_if_current(revision, session.snapshot().revision, false).has_value(),
          "fake JACK render did not publish");
  return {read_float_wav(target), run->tail_frames(), run->output_latency_frames()};
}

StereoWav offline_render(app::ProjectSession& session, const fs::path& target) {
  auto task = take(session.exports().prepare_wav_master_export(
      {target, oscillate::exporting::WavFormat::float32}));
  require(std::move(task).run().has_value(), "offline master render failed");
  return read_float_wav(target);
}

float window_peak(const StereoWav& wav, std::size_t first, std::size_t last) {
  float worst = 0.0F;
  for (std::size_t frame = first; frame < std::min(last, wav.left.size()); ++frame)
    worst = std::max({worst, std::fabs(wav.left[frame]), std::fabs(wav.right[frame])});
  return worst;
}

// An independent offline master render of the same saved project is the
// reference. A track EQ and master compressor keep per-sample state across
// periods; the endpoint falls mid-period for both 64- and 128-frame buffers.
void offline_and_fake_jack_renders_match() {
  constexpr int source_frames = 6'000;
  constexpr int silent = 300;
  constexpr std::uint64_t end = 4'001;
  Directory directory;
  const auto root = directory.root / "song";
  {
    auto session = take(app::ProjectSession::create(root));
    const auto fd =
        ::open(tone_fixture(directory.root, source_frames, silent).c_str(), O_RDONLY | O_CLOEXEC);
    require(fd >= 0, "fixture open failed");
    auto track = session->imports().import_wav_audio(fd);
    (void)::close(fd);
    require(track.has_value(), "audio import failed");
    namespace project = oscillate::project;
    // Shape's band 4 is a 1 kHz bell: +6 dB at Q 1.
    require(
        session->effects()
            .add_native_effect(
                project::SignalPathId{track.value()}, 0,
                project::NativeEffectPatch{
                    "shape", 1, {{"shape.on_4", 1.0}, {"shape.q_4", 1.0}, {"shape.gain_4", 6.0}}})
            .has_value(),
        "track EQ insert failed");
    require(
        session->effects()
            .add_native_effect(project::SignalPathId{project::MasterSignalPath{}}, 0,
                               project::NativeEffectPatch{
                                   "press", 1, {{"press.attack", 1.0}, {"press.release", 50.0}}})
            .has_value(),
        "master compressor insert failed");
    require(session->saving().save().has_value(), "save failed");
  }
  auto session = take(app::ProjectSession::open(root));
  const auto reopened = session->snapshot();
  require(reopened.tracks.size() == 1 && reopened.tracks[0].effects.size() == 1 &&
              reopened.tracks[0].effects[0].enabled && reopened.master_effects.size() == 1 &&
              reopened.master_effects[0].enabled,
          "reopened project lost an enabled native insert");
  const auto offline = offline_render(*session, directory.root / "offline.wav");
  const auto eq_tail = take(quick::RealtimeExportRun::bounded_tail_frames(reopened));
  const auto narrow = jack_render(root, *session, directory.root / "jack-64.wav", end, 64);
  (void)jack_render(root, *session, directory.root / "jack-64-repeat.wav", end, 64);
  const auto broad = jack_render(root, *session, directory.root / "jack-128.wav", end, 128);
  const auto& first = narrow.wav;
  const auto& wide = broad.wav;

  require(eq_tail > 0 && narrow.tail_frames == eq_tail && broad.tail_frames == eq_tail &&
              narrow.latency_frames == 0 && broad.latency_frames == 0,
          "EQ tail or zero output latency was not applied");
  require(first.left.size() == end + eq_tail && wide.left.size() == end + eq_tail,
          "real-time render did not stop at the content end plus the EQ tail");
  require(bytes(directory.root / "jack-64.wav") == bytes(directory.root / "jack-64-repeat.wav"),
          "repeat fake-JACK render changed");
  require(offline.left.size() > end + eq_tail, "offline render is shorter than the tail end");
  // Offline keeps playing the tone; the cut source leaves only EQ ringing,
  // which has settled long before the second half of its bound.
  const auto late = end + eq_tail / 2;
  require(window_peak(offline, late, end + eq_tail) > 0.01F,
          "offline audio is silent past the endpoint, so the source cut is untested");
  require(window_peak(first, late, end + eq_tail) < 1e-4F &&
              window_peak(wide, late, end + eq_tail) < 1e-4F,
          "source content continued past the selected end");
  require(onset(offline) == silent && onset(first) == silent && onset(wide) == silent,
          "real-time or offline onset moved");
  // The dry source peaks at 0.5 on each channel (zero pan is unity). The +6 dB
  // EQ alone would reach about 1.0 and the compressor alone stays below 0.5,
  // so this window shows both inserts shaped the reference.
  float peak = 0.0F;
  for (std::size_t frame = silent; frame < end; ++frame)
    peak = std::max(peak, std::fabs(offline.left[frame]));
  require(peak > 0.55F && peak < 0.9F, "offline reference does not show both native inserts");
  // Both paths run the same float32 processors with per-sample state; only the
  // block partition differs (offline 1024, JACK 64/128). 1e-6 (-120 dBFS)
  // admits a few ulps of reordering but fails any one-sample shift (a step of
  // about 0.065 here) or block-rate parameter change. Observed: 0.
  constexpr float tolerance = 1e-6F;
  const auto offline_64 = max_difference(offline, first, end);
  const auto offline_128 = max_difference(offline, wide, end);
  std::cout << "parity: offline peak " << peak << ", |offline-jack64| " << offline_64
            << ", |offline-jack128| " << offline_128 << '\n';
  require(offline_64 <= tolerance && offline_128 <= tolerance,
          "fake-JACK render differs from offline master");
  require(callback_allocations.load() == 0, "JACK callback allocated");
}

void add_master_tail(app::ProjectSession& session) {
  namespace project = oscillate::project;
  const project::SignalPathId master{project::MasterSignalPath{}};
  require(session.effects()
              .add_native_effect(master, 0,
                                 project::NativeEffectPatch{
                                     "expanse", 1, {{"expanse.decay", 0.4}, {"expanse.mix", 0.4}}})
              .has_value(),
          "master reverb insert failed");
  require(session.effects()
              .add_native_effect(master, 1, project::NativeEffectPatch{"ceiling", 1, {}})
              .has_value(),
          "master limiter insert failed");
}

// The content end falls mid-clip and mid-period; a later copy of the clip
// starts after it. A master reverb rings past the end and a lookahead limiter
// delays the master output. The reference is an independent offline render
// of a project whose only source is the same tone truncated at the end, so a
// later clip, any source frame past the end, a dropped tail or a latency
// shift breaks the match.
void reverb_tail_rings_out_without_later_source() {
  constexpr int source_frames = 3'000;
  constexpr int silent = 300;
  constexpr std::uint64_t end = 2'501;
  Directory directory;
  const auto root = directory.root / "song";
  const auto reference_root = directory.root / "reference";
  fs::create_directories(directory.root / "full");
  fs::create_directories(directory.root / "cut");
  const auto make = [](const fs::path& project_root, const fs::path& tone, bool later_clip) {
    auto session = take(app::ProjectSession::create(project_root));
    const auto fd = ::open(tone.c_str(), O_RDONLY | O_CLOEXEC);
    require(fd >= 0, "fixture open failed");
    auto track = session->imports().import_wav_audio(fd);
    (void)::close(fd);
    require(track.has_value(), "audio import failed");
    if (later_clip)
      require(session->audio_clips()
                  .duplicate_audio_clip_after(session->snapshot().tracks[0].clips[0].id)
                  .has_value(),
              "later clip placement failed");
    add_master_tail(*session);
    require(session->saving().save().has_value(), "save failed");
  };
  make(root, tone_fixture(directory.root / "full", source_frames, silent), true);
  make(reference_root, tone_fixture(directory.root / "cut", static_cast<int>(end), silent), false);

  auto session = take(app::ProjectSession::open(root));
  require(session->snapshot().tracks.size() == 1 && session->snapshot().tracks[0].clips.size() == 2,
          "reopened project lost its later clip");
  auto reference_session = take(app::ProjectSession::open(reference_root));
  const auto reference = offline_render(*reference_session, directory.root / "reference.wav");
  const auto full = offline_render(*session, directory.root / "full.wav");
  const auto narrow = jack_render(root, *session, directory.root / "jack-64.wav", end, 64);
  const auto broad = jack_render(root, *session, directory.root / "jack-128.wav", end, 128);
  const auto expected_tail =
      take(quick::RealtimeExportRun::bounded_tail_frames(session->snapshot()));

  // 5 ms lookahead at 48 kHz.
  require(narrow.latency_frames == 240 && broad.latency_frames == 240,
          "limiter lookahead is not the prepared output latency");
  require(expected_tail > 4'800 && narrow.tail_frames == expected_tail &&
              broad.tail_frames == expected_tail,
          "reverb tail bound was not applied");
  require(narrow.wav.left.size() == end + expected_tail &&
              broad.wav.left.size() == end + expected_tail &&
              reference.left.size() == end + expected_tail,
          "real-time render length differs from the offline content-plus-tail length");
  require(onset(narrow.wav) == silent && onset(broad.wav) == silent && onset(reference) == silent,
          "output latency was not removed from the capture");
  // The full project keeps sounding after the end: the rest of the first
  // clip, then the later clip's tone.
  require(window_peak(full, end, 3'000) > 0.3F && window_peak(full, 3'400, 5'900) > 0.3F,
          "offline project has no content after the end, so the cut is untested");
  // The cut source leaves only reverb in the tail; it is audible well after
  // the end and below the dry tone level.
  const auto ringing = window_peak(narrow.wav, end + 480, end + 4'800);
  require(ringing > 1e-3F && ringing < 0.3F, "reverb tail is missing after the content end");
  constexpr float tolerance = 1e-6F;
  const auto difference_64 = max_difference(reference, narrow.wav, reference.left.size());
  const auto difference_128 = max_difference(reference, broad.wav, reference.left.size());
  std::cout << "tail: frames " << expected_tail << ", latency " << narrow.latency_frames
            << ", tail peak " << ringing << ", |reference-jack64| " << difference_64
            << ", |reference-jack128| " << difference_128 << '\n';
  require(difference_64 <= tolerance && difference_128 <= tolerance,
          "fake-JACK tail render differs from the truncated offline reference");
  require(callback_allocations.load() == 0, "JACK callback allocated");
}

void native_synth_release_cuts_midi_and_matches_offline() {
  namespace model = oscillate::project;
  namespace exporting = oscillate::exporting;
  Directory directory;
  const auto root = directory.root / "synth-song";
  auto session = take(app::ProjectSession::create(root));
  const auto track = take(session->instruments().add_synth_track());
  require(session->instruments().select_native_instrument(track, "prism", 2).has_value(),
          "native synth preset failed");
  require(session->midi_clips().add_midi_clip(track, {0}).has_value(), "native synth clip failed");
  const auto clip = session->snapshot().tracks.front().midi_clips.front().id;
  require(session->notes().add_note(clip, {0}, 48).has_value() &&
              session->notes()
                  .add_note(clip, {oscillate::timeline::ticks_per_quarter / 2}, 72)
                  .has_value() &&
              session->saving().save().has_value(),
          "native synth notes or save failed");
  session.reset();
  session = take(app::ProjectSession::open(root));
  const auto saved = session->snapshot();
  constexpr std::uint64_t end = 4'000; // One-sixth beat at 120 BPM; mid 64/128 period.
  auto reference_snapshot = saved;
  auto& notes = reference_snapshot.tracks.front().midi_clips.front().notes;
  require(notes.size() == 2, "native synth fixture has wrong note count");
  const auto held =
      std::ranges::find(notes, oscillate::timeline::TickPosition{0}, &model::MidiNote::start);
  require(held != notes.end(), "native synth held note is missing");
  held->end = {oscillate::timeline::ticks_per_quarter / 6};
  std::erase_if(notes, [](const auto& note) { return note.start.value != 0; });
  const auto reference_path = directory.root / "native-reference.wav";
  {
    StoppedGate gate;
    auto store = take(oscillate::persistence::ProjectStore::open_reader(root));
    auto authority = take(exporting::StoppedExportAuthority::acquire(reference_snapshot, gate));
    take(exporting::render_wav_master(reference_snapshot, store, std::move(authority),
                                      {reference_path, exporting::WavFormat::float32}));
  }
  const auto reference = read_float_wav(reference_path);
  const auto narrow_path = directory.root / "native-64.wav";
  const auto narrow = jack_render(root, *session, narrow_path, end, 64);
  const auto broad = jack_render(root, *session, directory.root / "native-128.wav", end, 128);
  require(narrow.tail_frames > 2'000 && narrow.tail_frames == broad.tail_frames,
          "native release has no consistent prepared bound");
  const auto frames = end + narrow.tail_frames;
  require(reference.left.size() == frames && narrow.wav.left.size() == frames &&
              broad.wav.left.size() == frames,
          "native release end differs from the offline reference");
  require(window_peak(narrow.wav, end + 100, end + 2'000) > 1e-4F,
          "native synth release was cut to silence");
  require(max_difference(reference, narrow.wav, frames) < 1e-6F &&
              max_difference(reference, broad.wav, frames) < 1e-6F,
          "native synth MIDI end or release differs from offline at 64/128 frames");
  require(bytes(narrow_path) == bytes(directory.root / "native-128.wav"),
          "native synth export changes with callback period");
  require(callback_allocations.load() == 0, "native synth JACK callback allocated");

  const auto original = bytes(narrow_path);
  FakeJack jack;
  auto run = take(quick::RealtimeExportRun::begin(root, session->snapshot(),
                                                  take(session->reserve_playback_start()),
                                                  narrow_path, end, 64, {}, jack.api()));
  jack.run();
  jack.emit_xrun();
  require(!run->finish() && bytes(narrow_path) == original,
          "native synth xrun replaced the prior WAV");
  run.reset();
  take(session->parameter_automation().set_native_automation_point(track, "prism.filter.cutoff", 0,
                                                                   500.0));
  FakeJack automated;
  const auto rejected = quick::RealtimeExportRun::begin(root, session->snapshot(),
                                                        take(session->reserve_playback_start()),
                                                        narrow_path, end, 64, {}, automated.api());
  require(!rejected && rejected.error().code == oscillate::foundation::ErrorCode::unsupported &&
              !automated.active && automated.closes == 0 && bytes(narrow_path) == original,
          "automated native synth tail reached JACK or changed prior WAV");
}

// Tails that cannot be bounded here fail before any graph or device opens,
// and the rejected run releases the playback reservation.
void unbounded_tail_shapes_fail_before_preparation() {
  namespace model = oscillate::project;
  Directory directory;
  const auto root = directory.root / "song";
  auto session = project(root, fixture(directory.root));
  add_master_tail(*session);
  const auto base = session->snapshot();
  const model::Vst3PluginInstance plugin{
      take(model::PluginInstanceId::from_bits({1, 1})), "", "", "", "", "", "", {}, {}, false};
  const auto failure = [](const model::Snapshot& snapshot) {
    const auto result = quick::RealtimeExportRun::bounded_tail_frames(snapshot);
    return result ? oscillate::foundation::ErrorCode::internal : result.error().code;
  };

  auto effect = base;
  effect.master_effects[0].processor = plugin;
  require(failure(effect) == oscillate::foundation::ErrorCode::unsupported,
          "VST3 effect tail was accepted");
  effect.master_effects[0].enabled = false;
  require(quick::RealtimeExportRun::bounded_tail_frames(effect).has_value(),
          "a bypassed VST3 slot contributed a tail");

  auto instrument = base;
  instrument.tracks[0].instrument_source = plugin;
  require(failure(instrument) == oscillate::foundation::ErrorCode::unsupported,
          "VST3 instrument tail was accepted");

  auto notes = base;
  notes.tracks[0].kind = model::TrackKind::instrument;
  notes.tracks[0].midi_clips.emplace_back(
      base.tracks[0].clips[0].id, base.tracks[0].clips[0].placement,
      oscillate::timeline::TickPosition{960}, false, std::vector<model::MidiNote>{});
  require(failure(notes) == oscillate::foundation::ErrorCode::unsupported,
          "instrument note releases were accepted");

  auto long_tail = base;
  long_tail.master_effects[0].processor =
      model::NativeEffectPatch{"expanse", 1, {{"expanse.decay", 30.0}, {"expanse.mix", 1.0}}};
  long_tail.master_effects[1] = long_tail.master_effects[0];
  require(failure(long_tail) == oscillate::foundation::ErrorCode::resource_exhausted,
          "tails above ten seconds were accepted");

  effect.master_effects[0].enabled = true;
  FakeJack jack;
  require(!quick::RealtimeExportRun::begin(root, effect, take(session->reserve_playback_start()),
                                           directory.root / "render.wav", 64, 64, {}, jack.api()),
          "VST3 effect run started");
  require(!jack.active && jack.closes == 0 && !fs::exists(directory.root / "render.wav"),
          "rejected run opened JACK or wrote a target");
  require(session->exports()
              .prepare_wav_master_export(
                  {directory.root / "after.wav", oscillate::exporting::WavFormat::float32})
              .has_value(),
          "rejected run did not release project gate");
}
void exact_vst3_tail_evidence_reaches_live_export() {
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  namespace plugins = oscillate::plugins;
  Directory directory;
  const auto root = directory.root / "vst3-song";
  auto session = project(root, fixture(directory.root));
  plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto scan = scanner.scan(VST3_GAIN_BUNDLE);
  require(scan.ok() && scan.metadata.classes.size() == 1, "fixed-tail fixture scan failed");
  const auto& klass = scan.metadata.classes.front();
  const auto id = take(session->effects().insert_vst3_plugin(
      session->snapshot().tracks.front().id, 0,
      {.class_id = klass.id,
       .vendor = klass.vendor,
       .name = klass.name,
       .version = klass.version,
       .canonical_bundle_path = scan.binary.canonical_bundle.string(),
       .binary_sha256 = scan.binary.sha256}));
  const auto& saved = std::get<oscillate::project::Vst3PluginInstance>(
      session->snapshot().tracks.front().effects.front().processor);
  plugins::Vst3FixedTailQualification record{.binary_sha256 = scan.binary.sha256,
                                             .binary_byte_size = scan.binary.byte_size,
                                             .class_id = saved.class_id,
                                             .vendor = saved.vendor,
                                             .version = saved.version,
                                             .sample_rate = 48'000,
                                             .declared_tail_frames = 0,
                                             .component_state = saved.component_state,
                                             .parameters = saved.parameters,
                                             .tested_64_frames = true,
                                             .tested_128_frames = true};
  for (const auto block : {64U, 128U}) {
    const auto target = directory.root / ("vst3-" + std::to_string(block) + ".wav");
    FakeJack rejected;
    rejected.block_frames = block;
    const auto absent = quick::RealtimeExportRun::begin(root, session->snapshot(),
                                                        take(session->reserve_playback_start()),
                                                        target, 64, block, {}, rejected.api());
    require(!absent && !rejected.active && rejected.closes == 0 && !fs::exists(target),
            "unqualified VST3 tail reached JACK or wrote output");

    quick::Vst3LiveQualification stale;
    auto wrong_binary = record;
    wrong_binary.binary_sha256.assign(64, '0');
    stale.fixed_effect_tails.emplace_back(id, std::move(wrong_binary));
    FakeJack mismatched;
    mismatched.block_frames = block;
    const auto declined = quick::RealtimeExportRun::begin(
        root, session->snapshot(), take(session->reserve_playback_start()), target, 64, block,
        std::move(stale), mismatched.api());
    require(!declined && !mismatched.active && mismatched.closes == 0 && !fs::exists(target),
            "changed VST3 binary evidence reached JACK or wrote output");

    quick::Vst3LiveQualification qualified;
    qualified.fixed_effect_tails.emplace_back(id, record);
    FakeJack jack;
    jack.block_frames = block;
    auto run = take(quick::RealtimeExportRun::begin(root, session->snapshot(),
                                                    take(session->reserve_playback_start()), target,
                                                    64, block, std::move(qualified), jack.api()));
    require(run->output_latency_frames() == 7, "qualified effect lost its prepared latency");
    drive(jack, *run, run->expected_frames());
    std::atomic<bool> cancelled{false};
    require(run->wait_until_ready(cancelled, {}).has_value() &&
                run->finish_if_current(session->snapshot().revision, session->snapshot().revision,
                                       false)
                    .has_value(),
            "qualified VST3 export did not publish");
    const auto wav = read_float_wav(target);
    require(onset(wav) == 0 && std::fabs(wav.left.front() - 0.5F) < 1e-6F &&
                window_peak(wav, 0, wav.left.size()) <= 0.5F,
            "qualified VST3 output was not processed and latency-aligned");
  }
#endif
}

void two_saved_vst3_sources_need_independent_release_receipts() {
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  namespace model = oscillate::project;
  namespace plugins = oscillate::plugins;
  require(::setenv("OSCILLATE_VST3_INSTRUMENT_TAIL", "8", 1) == 0,
          "two-source release fixture setup failed");
  Directory directory;
  const auto root = directory.root / "two-vst3-sources";
  plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto scan = scanner.scan(VST3_INSTRUMENT_BUNDLE);
  require(scan.ok() && scan.metadata.classes.size() == 1, "two-source export fixture scan failed");
  const auto& klass = scan.metadata.classes.front();
  auto session = take(app::ProjectSession::create(root));
  const auto track = take(session->instruments().create_vst3_instrument_track(
      {.class_id = klass.id,
       .vendor = klass.vendor,
       .name = klass.name,
       .version = klass.version,
       .canonical_bundle_path = scan.binary.canonical_bundle.string(),
       .binary_sha256 = scan.binary.sha256}));
  require(session->midi_clips().add_midi_clip(track, {0}).has_value(),
          "two-source export MIDI clip creation failed");
  const auto clip = session->snapshot().tracks.front().midi_clips.front().id;
  require(session->notes().add_note(clip, {0}, 60).has_value() &&
              session->saving().save().has_value(),
          "two-source export note or save failed");
  const auto second = take(session->instruments().create_vst3_instrument_track(
      {.class_id = klass.id,
       .vendor = klass.vendor,
       .name = klass.name,
       .version = klass.version,
       .canonical_bundle_path = scan.binary.canonical_bundle.string(),
       .binary_sha256 = scan.binary.sha256}));
  require(second != track, "second VST3 source reused the first track identity");
  require(session->midi_clips().add_midi_clip(second, {0}).has_value(),
          "second source MIDI clip creation failed");
  const auto second_clip = session->snapshot().tracks[1].midi_clips.front().id;
  require(session->notes().add_note(second_clip, {1'000'000}, 60).has_value(),
          "second source staggered MIDI note failed");
  const float second_level = 0.25F;
  std::vector<std::uint8_t> second_state(sizeof(second_level));
  std::memcpy(second_state.data(), &second_level, sizeof(second_level));
  require(session->instruments().set_vst3_instrument_state(
              session->snapshot().revision, second,
              std::get<model::Vst3PluginInstance>(session->snapshot().tracks[1].instrument_source),
              std::move(second_state), {{1, 0.25}}) &&
              session->saving().save(),
          "second source level and project save failed");
  session.reset();
  session = take(app::ProjectSession::open(root));
  require(session->snapshot().tracks.size() == 2 &&
              std::get_if<oscillate::project::Vst3PluginInstance>(
                  &session->snapshot().tracks[0].instrument_source) &&
              std::get_if<oscillate::project::Vst3PluginInstance>(
                  &session->snapshot().tracks[1].instrument_source) &&
              std::get<oscillate::project::Vst3PluginInstance>(
                  session->snapshot().tracks[0].instrument_source)
                      .id != std::get<oscillate::project::Vst3PluginInstance>(
                                 session->snapshot().tracks[1].instrument_source)
                                 .id &&
              session->snapshot().tracks[1].midi_clips.front().notes.front().start.value ==
                  1'000'000,
          "two saved VST3 instrument sources or MIDI schedules did not reopen");
  const auto& first_saved =
      std::get<model::Vst3PluginInstance>(session->snapshot().tracks[0].instrument_source);
  const auto& second_saved =
      std::get<model::Vst3PluginInstance>(session->snapshot().tracks[1].instrument_source);
  const auto receipt_for = [&](const model::Vst3PluginInstance& saved) {
    return plugins::Vst3InstrumentTailQualification{.binary_sha256 = scan.binary.sha256,
                                                    .binary_byte_size = scan.binary.byte_size,
                                                    .class_id = saved.class_id,
                                                    .vendor = saved.vendor,
                                                    .name = saved.name,
                                                    .version = saved.version,
                                                    .sample_rate = 48'000,
                                                    .declared_tail_frames = 8,
                                                    .component_state = saved.component_state,
                                                    .parameters = saved.parameters,
                                                    .note_off_release_tested_64_frames = true,
                                                    .note_off_release_tested_128_frames = true};
  };
  const auto first_receipt = receipt_for(first_saved);
  const auto second_receipt = receipt_for(second_saved);
  const auto target = directory.root / "existing.wav";
  {
    std::ofstream output(target, std::ios::binary);
    output << "prior valid render";
    require(output.good(), "existing export fixture write failed");
  }
  const auto prior = bytes(target);
  for (const auto block : {64U, 128U}) {
    constexpr std::uint64_t end = 65; // A note-off inside the second JACK block.
    const auto refused = [&](quick::Vst3LiveQualification qualification,
                             std::string_view description) {
      FakeJack jack;
      jack.block_frames = block;
      auto result = quick::RealtimeExportRun::begin(
          root, session->snapshot(), take(session->reserve_playback_start()), target, end, block,
          std::move(qualification), jack.api());
      require(!result && jack.opens == 0 && jack.closes == 0 && !jack.active &&
                  bytes(target) == prior,
              description);
    };
    refused({}, "two-source export without receipts reached JACK");
    quick::Vst3LiveQualification only_first;
    only_first.fixed_instrument_tails.emplace_back(first_saved.id, first_receipt);
    refused(std::move(only_first), "two-source export accepted a missing second receipt");
    quick::Vst3LiveQualification stale_second;
    stale_second.fixed_instrument_tails.emplace_back(first_saved.id, first_receipt);
    auto wrong_state = second_receipt;
    wrong_state.component_state = {0, 0, 0, 0};
    stale_second.fixed_instrument_tails.emplace_back(second_saved.id, std::move(wrong_state));
    refused(std::move(stale_second), "two-source export accepted stale second-source state");
    quick::Vst3LiveQualification wrong_binary;
    wrong_binary.fixed_instrument_tails.emplace_back(first_saved.id, first_receipt);
    auto stale_binary = second_receipt;
    stale_binary.binary_sha256.assign(64, '0');
    wrong_binary.fixed_instrument_tails.emplace_back(second_saved.id, std::move(stale_binary));
    refused(std::move(wrong_binary), "two-source export accepted a changed second binary");
    quick::Vst3LiveQualification wrong_tail;
    wrong_tail.fixed_instrument_tails.emplace_back(first_saved.id, first_receipt);
    auto stale_tail = second_receipt;
    stale_tail.declared_tail_frames = 9;
    wrong_tail.fixed_instrument_tails.emplace_back(second_saved.id, std::move(stale_tail));
    refused(std::move(wrong_tail), "two-source export accepted a stale second tail bound");
    quick::Vst3LiveQualification admitted;
    admitted.fixed_instrument_tails.emplace_back(first_saved.id, first_receipt);
    admitted.fixed_instrument_tails.emplace_back(second_saved.id, second_receipt);
    FakeJack jack;
    jack.block_frames = block;
    auto run = take(quick::RealtimeExportRun::begin(root, session->snapshot(),
                                                    take(session->reserve_playback_start()), target,
                                                    end, block, std::move(admitted), jack.api()));
    require(jack.opens == 1 && jack.active && run->tail_frames() == 8 &&
                run->output_latency_frames() == 0,
            "qualified two-source export has a wrong device or tail receipt");
    drive(jack, *run, end + 8);
    std::atomic<bool> cancelled{false};
    require(run->wait_until_ready(cancelled, {}) &&
                run->finish_if_current(session->snapshot().revision, session->snapshot().revision,
                                       false),
            "qualified two-source export did not publish");
    const auto wav = read_float_wav(target);
    constexpr float first_level = 0.5F * 100.0F / 127.0F;
    constexpr float second_level_in_mix = 0.25F * 100.0F / 127.0F;
    require(wav.left.size() == end + 8 && std::fabs(wav.left[0] - first_level) < 0.00001F &&
                std::fabs(wav.left[24] - first_level) < 0.00001F &&
                std::fabs(wav.left[25] - first_level - second_level_in_mix) < 0.00001F &&
                std::fabs(wav.right[25] + wav.left[25]) < 0.00001F &&
                std::fabs(wav.left[end]) > 0.0001F && std::fabs(wav.left[end + 7]) > 0.0001F &&
                std::fabs(wav.left[end + 7]) < std::fabs(wav.left[end]) * 0.2F &&
                callback_allocations.load() == 0,
            "two-source JACK output lost independent MIDI, level, note-off tails or RT safety");
    run.reset();
    require(jack.stops == 1 && jack.closes == 1,
            "two-source JACK export did not retire the device");
    {
      std::ofstream output(target, std::ios::binary | std::ios::trunc);
      output.write(prior.data(), static_cast<std::streamsize>(prior.size()));
      require(output.good(), "two-source prior render reset failed");
    }
  }
  require(::unsetenv("OSCILLATE_VST3_INSTRUMENT_TAIL") == 0,
          "two-source release fixture cleanup failed");
#endif
}

// A held note must receive an exact MIDI note-off at the selected content
// endpoint. The synthetic instrument emits eight nonzero release samples only
// after that event, independently of the JACK callback size.
void exact_vst3_instrument_release_reaches_live_export() {
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  namespace model = oscillate::project;
  namespace plugins = oscillate::plugins;
  require(::setenv("OSCILLATE_VST3_INSTRUMENT_TAIL", "8", 1) == 0,
          "instrument tail fixture setup failed");
  Directory directory;
  const auto root = directory.root / "vst3-instrument-song";
  plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto scan = scanner.scan(VST3_INSTRUMENT_BUNDLE);
  require(scan.ok() && scan.metadata.classes.size() == 1, "instrument tail fixture scan failed");
  const auto& klass = scan.metadata.classes.front();
  auto session = take(app::ProjectSession::create(root));
  require(session->tracks().add_instrument_track().has_value(), "instrument track creation failed");
  const auto track = session->snapshot().tracks.front().id;
  require(session->midi_clips().add_midi_clip(track, {0}).has_value(),
          "instrument MIDI clip failed");
  const auto clip = session->snapshot().tracks.front().midi_clips.front().id;
  require(session->notes().add_note(clip, {0}, 60).has_value(), "held MIDI note failed");
  const auto id = take(session->effects().insert_vst3_plugin(
      track, 0,
      {.class_id = klass.id,
       .vendor = klass.vendor,
       .name = klass.name,
       .version = klass.version,
       .canonical_bundle_path = scan.binary.canonical_bundle.string(),
       .binary_sha256 = scan.binary.sha256}));
  require(session->saving().save().has_value(), "instrument tail project save failed");
  session.reset();
  session = take(app::ProjectSession::open(root));
  const auto& saved =
      std::get<model::Vst3PluginInstance>(session->snapshot().tracks.front().instrument_source);
  require(saved.id == id, "instrument identity changed on reopen");

  plugins::Vst3InstrumentTailQualification evidence{.binary_sha256 = scan.binary.sha256,
                                                    .binary_byte_size = scan.binary.byte_size,
                                                    .class_id = saved.class_id,
                                                    .vendor = saved.vendor,
                                                    .name = saved.name,
                                                    .version = saved.version,
                                                    .sample_rate = 48'000,
                                                    .declared_tail_frames = 8,
                                                    .component_state = saved.component_state,
                                                    .parameters = saved.parameters,
                                                    .note_off_release_tested_64_frames = true,
                                                    .note_off_release_tested_128_frames = true};
  constexpr std::uint64_t end = 4'000; // Mid-period and before the saved note-off.
  for (const auto block : {64U, 128U}) {
    const auto target = directory.root / ("instrument-" + std::to_string(block) + ".wav");
    FakeJack missing;
    missing.block_frames = block;
    const auto absent = quick::RealtimeExportRun::begin(root, session->snapshot(),
                                                        take(session->reserve_playback_start()),
                                                        target, end, block, {}, missing.api());
    require(!absent && !missing.active && missing.closes == 0 && !fs::exists(target),
            "unqualified instrument release reached JACK or wrote output");

    quick::Vst3LiveQualification stale;
    auto wrong_state = evidence;
    wrong_state.component_state = {0, 0, 0, 0};
    stale.fixed_instrument_tails.emplace_back(id, std::move(wrong_state));
    FakeJack mismatched;
    mismatched.block_frames = block;
    const auto declined = quick::RealtimeExportRun::begin(
        root, session->snapshot(), take(session->reserve_playback_start()), target, end, block,
        std::move(stale), mismatched.api());
    require(!declined && !mismatched.active && mismatched.closes == 0 && !fs::exists(target),
            "stale instrument state evidence reached JACK or wrote output");

    quick::Vst3LiveQualification admitted;
    admitted.fixed_instrument_tails.emplace_back(id, evidence);
    FakeJack jack;
    jack.block_frames = block;
    auto run = take(quick::RealtimeExportRun::begin(root, session->snapshot(),
                                                    take(session->reserve_playback_start()), target,
                                                    end, block, std::move(admitted), jack.api()));
    require(run->tail_frames() == 8 && run->output_latency_frames() == 0,
            "instrument release or latency receipt differs from evidence");
    drive(jack, *run, end + 8);
    std::atomic<bool> cancelled{false};
    require(run->wait_until_ready(cancelled, {}).has_value() &&
                run->finish_if_current(session->snapshot().revision, session->snapshot().revision,
                                       false)
                    .has_value(),
            "qualified instrument export did not publish");
    const auto wav = read_float_wav(target);
    require(wav.left.size() == end + 8 && onset(wav) == 0,
            "instrument export duration or note-on position is wrong");
    const auto release_start = std::fabs(wav.left[end]);
    const auto release_last = std::fabs(wav.left[end + 7]);
    require(window_peak(wav, 0, end) > 1e-4F && release_start > 1e-4F && release_last > 1e-4F &&
                release_last < release_start * 0.2F,
            "MIDI cutoff failed to trigger the exact decaying release");
    require(callback_allocations.load() == 0, "VST3 instrument JACK callback allocated");
  }
  require(::unsetenv("OSCILLATE_VST3_INSTRUMENT_TAIL") == 0,
          "instrument tail fixture cleanup failed");
#endif
}

// A serial effect after the MIDI source must process the selected-end release,
// and both exact runtimes must contribute their tail receipts before JACK opens.
void vst3_instrument_release_through_serial_effect() {
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  namespace model = oscillate::project;
  namespace plugins = oscillate::plugins;
  require(::setenv("OSCILLATE_VST3_INSTRUMENT_TAIL", "8", 1) == 0,
          "instrument/effect release fixture setup failed");
  Directory directory;
  const auto root = directory.root / "vst3-instrument-effect-song";
  plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto instrument_scan = scanner.scan(VST3_INSTRUMENT_BUNDLE);
  const auto effect_scan = scanner.scan(VST3_GAIN_BUNDLE);
  require(instrument_scan.ok() && instrument_scan.metadata.classes.size() == 1 &&
              effect_scan.ok() && effect_scan.metadata.classes.size() == 1,
          "instrument/effect release fixture scans failed");
  const auto& instrument_class = instrument_scan.metadata.classes.front();
  const auto& effect_class = effect_scan.metadata.classes.front();
  auto session = take(app::ProjectSession::create(root));
  const auto track = take(session->instruments().create_vst3_instrument_track(
      {.class_id = instrument_class.id,
       .vendor = instrument_class.vendor,
       .name = instrument_class.name,
       .version = instrument_class.version,
       .canonical_bundle_path = instrument_scan.binary.canonical_bundle.string(),
       .binary_sha256 = instrument_scan.binary.sha256}));
  require(session->midi_clips().add_midi_clip(track, {0}).has_value(),
          "instrument/effect MIDI clip failed");
  const auto clip = session->snapshot().tracks.front().midi_clips.front().id;
  require(session->notes().add_note(clip, {0}, 60).has_value(),
          "instrument/effect held note failed");
  const auto effect_id = take(session->effects().insert_vst3_plugin(
      track, 0,
      {.class_id = effect_class.id,
       .vendor = effect_class.vendor,
       .name = effect_class.name,
       .version = effect_class.version,
       .canonical_bundle_path = effect_scan.binary.canonical_bundle.string(),
       .binary_sha256 = effect_scan.binary.sha256}));
  require(session->saving().save().has_value(), "instrument/effect project save failed");
  session.reset();
  session = take(app::ProjectSession::open(root));
  const auto& saved_track = session->snapshot().tracks.front();
  const auto& instrument = std::get<model::Vst3PluginInstance>(saved_track.instrument_source);
  const auto& effect = std::get<model::Vst3PluginInstance>(saved_track.effects.front().processor);
  require(saved_track.id == track && saved_track.effects.front().enabled && effect.id == effect_id,
          "instrument/effect serial identities did not reopen");
  const plugins::Vst3InstrumentTailQualification instrument_evidence{
      .binary_sha256 = instrument_scan.binary.sha256,
      .binary_byte_size = instrument_scan.binary.byte_size,
      .class_id = instrument.class_id,
      .vendor = instrument.vendor,
      .name = instrument.name,
      .version = instrument.version,
      .sample_rate = 48'000,
      .declared_tail_frames = 8,
      .component_state = instrument.component_state,
      .parameters = instrument.parameters,
      .note_off_release_tested_64_frames = true,
      .note_off_release_tested_128_frames = true};
  const plugins::Vst3FixedTailQualification effect_evidence{
      .binary_sha256 = effect_scan.binary.sha256,
      .binary_byte_size = effect_scan.binary.byte_size,
      .class_id = effect.class_id,
      .vendor = effect.vendor,
      .version = effect.version,
      .sample_rate = 48'000,
      .declared_tail_frames = 0,
      .component_state = effect.component_state,
      .parameters = effect.parameters,
      .tested_64_frames = true,
      .tested_128_frames = true};
  constexpr std::uint64_t end = 4'000; // Deliberately inside both callback sizes.
  const auto expected_level = 2.0F * 0.5F * 100.0F / 127.0F;
  for (const auto block : {64U, 128U}) {
    const auto target = directory.root / ("instrument-effect-" + std::to_string(block) + ".wav");
    quick::Vst3LiveQualification instrument_only;
    instrument_only.fixed_instrument_tails.emplace_back(instrument.id, instrument_evidence);
    FakeJack missing_effect;
    missing_effect.block_frames = block;
    const auto absent = quick::RealtimeExportRun::begin(
        root, session->snapshot(), take(session->reserve_playback_start()), target, end, block,
        std::move(instrument_only), missing_effect.api());
    require(!absent && !missing_effect.active && missing_effect.closes == 0 && !fs::exists(target),
            "instrument/effect export reached JACK without exact effect-tail evidence");

    quick::Vst3LiveQualification both;
    both.fixed_instrument_tails.emplace_back(instrument.id, instrument_evidence);
    both.fixed_effect_tails.emplace_back(effect.id, effect_evidence);
    FakeJack jack;
    jack.block_frames = block;
    auto run = take(quick::RealtimeExportRun::begin(root, session->snapshot(),
                                                    take(session->reserve_playback_start()), target,
                                                    end, block, std::move(both), jack.api()));
    require(run->tail_frames() == 8 && run->output_latency_frames() == 7 &&
                run->expected_frames() == end + 8,
            "instrument/effect serial tail or delay receipt is wrong");
    drive(jack, *run, end + 8);
    std::atomic<bool> cancelled{false};
    require(run->wait_until_ready(cancelled, {}).has_value() &&
                run->finish_if_current(session->snapshot().revision, session->snapshot().revision,
                                       false)
                    .has_value(),
            "qualified serial instrument/effect export did not publish");
    const auto wav = read_float_wav(target);
    require(wav.left.size() == end + 8 && onset(wav) == 0 &&
                std::fabs(wav.left.front() - expected_level) < 1e-5F &&
                std::fabs(wav.right.front() + expected_level) < 1e-5F,
            "serial effect was omitted or latency alignment changed");
    require(wav.left[end] > 1e-4F && wav.left[end + 7] > 1e-4F &&
                wav.left[end + 7] < wav.left[end] * 0.2F &&
                std::fabs(wav.left[end - 1] - expected_level) < 1e-5F,
            "selected-end note cut or downstream release tail was truncated");
    require(callback_allocations.load() == 0,
            "serial VST3 instrument/effect JACK callback allocated");
  }
  require(::unsetenv("OSCILLATE_VST3_INSTRUMENT_TAIL") == 0,
          "instrument/effect release fixture cleanup failed");
#endif
}

void two_vst3_instrument_effect_paths_reach_live_export() {
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  namespace model = oscillate::project;
  namespace plugins = oscillate::plugins;
  require(::setenv("OSCILLATE_VST3_INSTRUMENT_TAIL", "8", 1) == 0,
          "paired instrument/effect tail fixture setup failed");
  Directory directory;
  const auto root = directory.root / "paired-instrument-effects";
  plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto instrument_scan = scanner.scan(VST3_INSTRUMENT_BUNDLE);
  const auto effect_scan = scanner.scan(VST3_GAIN_BUNDLE);
  require(instrument_scan.ok() && instrument_scan.metadata.classes.size() == 1 &&
              effect_scan.ok() && effect_scan.metadata.classes.size() == 1,
          "paired instrument/effect fixture scan failed");
  const auto& instrument_class = instrument_scan.metadata.classes.front();
  const auto& effect_class = effect_scan.metadata.classes.front();
  auto session = take(app::ProjectSession::create(root));
  for (int index = 0; index < 2; ++index) {
    const auto track = take(session->instruments().create_vst3_instrument_track(
        {.class_id = instrument_class.id,
         .vendor = instrument_class.vendor,
         .name = instrument_class.name,
         .version = instrument_class.version,
         .canonical_bundle_path = instrument_scan.binary.canonical_bundle.string(),
         .binary_sha256 = instrument_scan.binary.sha256}));
    require(session->midi_clips().add_midi_clip(track, {0}).has_value(),
            "paired instrument MIDI clip creation failed");
    const auto clip_id = session->snapshot().tracks.back().midi_clips.front().id;
    require(session->notes().add_note(clip_id, {0}, 60).has_value(),
            "paired instrument note creation failed");
    require(session->effects()
                .insert_vst3_plugin(
                    track, 0,
                    {.class_id = effect_class.id,
                     .vendor = effect_class.vendor,
                     .name = effect_class.name,
                     .version = effect_class.version,
                     .canonical_bundle_path = effect_scan.binary.canonical_bundle.string(),
                     .binary_sha256 = effect_scan.binary.sha256})
                .has_value(),
            "paired instrument effect insertion failed");
  }
  require(session->saving().save().has_value(), "paired instrument project save failed");
  session.reset();
  session = take(app::ProjectSession::open(root));
  require(session->snapshot().tracks.size() == 2, "paired instrument tracks did not reopen");
  quick::Vst3LiveQualification admitted;
  for (const auto& track : session->snapshot().tracks) {
    const auto& instrument = std::get<model::Vst3PluginInstance>(track.instrument_source);
    const auto& effect = std::get<model::Vst3PluginInstance>(track.effects.front().processor);
    admitted.fixed_instrument_tails.emplace_back(
        instrument.id, plugins::Vst3InstrumentTailQualification{
                           .binary_sha256 = instrument_scan.binary.sha256,
                           .binary_byte_size = instrument_scan.binary.byte_size,
                           .class_id = instrument.class_id,
                           .vendor = instrument.vendor,
                           .name = instrument.name,
                           .version = instrument.version,
                           .sample_rate = 48'000,
                           .declared_tail_frames = 8,
                           .component_state = instrument.component_state,
                           .parameters = instrument.parameters,
                           .note_off_release_tested_64_frames = true,
                           .note_off_release_tested_128_frames = true});
    admitted.fixed_effect_tails.emplace_back(
        effect.id,
        plugins::Vst3FixedTailQualification{.binary_sha256 = effect_scan.binary.sha256,
                                            .binary_byte_size = effect_scan.binary.byte_size,
                                            .class_id = effect.class_id,
                                            .vendor = effect.vendor,
                                            .version = effect.version,
                                            .sample_rate = 48'000,
                                            .declared_tail_frames = 0,
                                            .component_state = effect.component_state,
                                            .parameters = effect.parameters,
                                            .tested_64_frames = true,
                                            .tested_128_frames = true});
  }
  constexpr std::uint64_t end = 65;
  for (const auto block : {64U, 128U}) {
    const auto target = directory.root / ("paired-" + std::to_string(block) + ".wav");
    {
      std::ofstream output(target, std::ios::binary);
      output << "prior valid render";
      require(output.good(), "paired export prior target write failed");
    }
    const auto prior = bytes(target);
    auto incomplete = admitted;
    incomplete.fixed_effect_tails.pop_back();
    FakeJack refused_jack;
    refused_jack.block_frames = block;
    const auto refused = quick::RealtimeExportRun::begin(
        root, session->snapshot(), take(session->reserve_playback_start()), target, end, block,
        std::move(incomplete), refused_jack.api());
    require(!refused && refused_jack.opens == 0 && !refused_jack.active && bytes(target) == prior,
            "paired export accepted a missing second effect receipt");

    FakeJack jack;
    jack.block_frames = block;
    auto run = take(quick::RealtimeExportRun::begin(root, session->snapshot(),
                                                    take(session->reserve_playback_start()), target,
                                                    end, block, admitted, jack.api()));
    require(run->tail_frames() == 8 && run->output_latency_frames() == 7 &&
                run->expected_frames() == end + 8,
            "paired export tail or delay receipt is wrong");
    drive(jack, *run, end + 8);
    std::atomic<bool> cancelled{false};
    require(run->wait_until_ready(cancelled, {}).has_value() &&
                run->finish_if_current(session->snapshot().revision, session->snapshot().revision,
                                       false)
                    .has_value(),
            "paired export did not publish");
    const auto wav = read_float_wav(target);
    const auto expected = 4.0F * 0.5F * 100.0F / 127.0F;
    require(wav.left.size() == end + 8 && onset(wav) == 0 &&
                std::fabs(wav.left.front() - expected) < 1e-5F &&
                std::fabs(wav.right.front() + expected) < 1e-5F && wav.left[end] > 1e-4F &&
                wav.left[end + 7] > 1e-4F && callback_allocations.load() == 0,
            "paired export lost an effect path, release tail or callback safety");
  }
  require(::unsetenv("OSCILLATE_VST3_INSTRUMENT_TAIL") == 0,
          "paired instrument/effect tail fixture cleanup failed");
#endif
}
} // namespace

int main() {
  saved_project_exact_interval_and_control_finish();
  xrun_and_cancellation_preserve_target();
  offline_and_fake_jack_renders_match();
  reverb_tail_rings_out_without_later_source();
  native_synth_release_cuts_midi_and_matches_offline();
  unbounded_tail_shapes_fail_before_preparation();
  exact_vst3_tail_evidence_reaches_live_export();
  two_saved_vst3_sources_need_independent_release_receipts();
  exact_vst3_instrument_release_reaches_live_export();
  vst3_instrument_release_through_serial_effect();
  two_vst3_instrument_effect_paths_reach_live_export();
}
