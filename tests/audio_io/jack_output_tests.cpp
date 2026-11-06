#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"

#include <oscillate/audio_io/jack_output.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {
using namespace oscillate::audio_io;

using oscillate::test::require;

struct FakeJack {
  std::uint32_t rate = oscillate::test::sample_rate_48k;
  std::uint32_t frames = 128;
  bool fail_open = false;
  bool fail_activate = false;
  bool rate_change_during_activate = false;
  std::uint32_t size_change_during_activate = 0;
  bool fail_register = false;
  bool fail_callback = false;
  bool null_left_buffer = false;
  bool opened = false;
  bool active = false;
  int opens = 0;
  int closes = 0;
  int activates = 0;
  int deactivates = 0;
  int playback_connections = 0;
  std::array<float, 8193> left{};
  std::array<float, 8193> right{};
  JackApi::Process process = nullptr;
  JackApi::Change rate_change = nullptr;
  JackApi::Change size_change = nullptr;
  JackApi::Event xrun = nullptr;
  JackApi::Shutdown shutdown = nullptr;
  void* callback_context = nullptr;

  static FakeJack& self(void* context) noexcept { return *static_cast<FakeJack*>(context); }
  static void* open(void* context, const char*) noexcept {
    auto& fake = self(context);
    ++fake.opens;
    if (fake.fail_open) {
      return nullptr;
    }
    fake.opened = true;
    return context;
  }
  static int close(void* context, void*) noexcept {
    auto& fake = self(context);
    fake.opened = false;
    fake.active = false;
    ++fake.closes;
    return 0;
  }
  static int activate(void* context, void*) noexcept {
    auto& fake = self(context);
    ++fake.activates;
    if (fake.fail_activate) {
      return -1;
    }
    fake.active = true;
    if (fake.rate_change_during_activate) {
      fake.rate = oscillate::test::sample_rate_44k;
      (void)fake.rate_change(44100, fake.callback_context);
    }
    if (fake.size_change_during_activate != 0) {
      fake.frames = fake.size_change_during_activate;
      (void)fake.size_change(fake.frames, fake.callback_context);
    }
    return 0;
  }
  static int deactivate(void* context, void*) noexcept {
    auto& fake = self(context);
    fake.active = false;
    ++fake.deactivates;
    return 0;
  }
  static std::uint32_t sample_rate(void* context, void*) noexcept { return self(context).rate; }
  static std::uint32_t buffer_size(void* context, void*) noexcept { return self(context).frames; }
  static void* register_output(void* context, void*, const char* name) noexcept {
    if (self(context).fail_register) {
      return nullptr;
    }
    return name[0] == 'l' ? static_cast<void*>(self(context).left.data())
                          : static_cast<void*>(self(context).right.data());
  }
  static float* port_buffer(void* context, void* port, std::uint32_t) noexcept {
    if (self(context).null_left_buffer && port == self(context).left.data()) {
      return nullptr;
    }
    return static_cast<float*>(port);
  }
  static int set_process(void* context, void*, JackApi::Process callback, void* data) noexcept {
    self(context).process = callback;
    self(context).callback_context = data;
    return self(context).fail_callback ? -1 : 0;
  }
  static int set_rate(void* context, void*, JackApi::Change callback, void*) noexcept {
    self(context).rate_change = callback;
    return 0;
  }
  static int set_size(void* context, void*, JackApi::Change callback, void*) noexcept {
    self(context).size_change = callback;
    return 0;
  }
  static int set_xrun(void* context, void*, JackApi::Event callback, void*) noexcept {
    self(context).xrun = callback;
    return 0;
  }
  static void set_shutdown(void* context, void*, JackApi::Shutdown callback, void*) noexcept {
    self(context).shutdown = callback;
  }
  static int connect_playback_ports(void* context, void*, void*, void*) noexcept {
    self(context).playback_connections += 2;
    return 2;
  }
  [[nodiscard]] JackApi api() noexcept {
    return {this,        open,        close,           activate,     deactivate,
            sample_rate, buffer_size, register_output, port_buffer,  set_process,
            set_rate,    set_size,    set_xrun,        set_shutdown, connect_playback_ports};
  }
  void run(std::uint32_t frame_count) noexcept { (void)process(frame_count, callback_context); }
  void change_rate(std::uint32_t new_rate) noexcept {
    rate = new_rate;
    (void)rate_change(new_rate, callback_context);
  }
  void change_size(std::uint32_t new_size) noexcept {
    frames = new_size;
    (void)size_change(new_size, callback_context);
  }
  void emit_xrun() noexcept { (void)xrun(callback_context); }
  void disconnect() noexcept { shutdown(callback_context); }
};

struct Renderer {
  std::uint64_t calls = 0;
  std::uint64_t last_position = 0;
  bool fail = false;
  static bool render(void* context, std::span<oscillate::engine::StereoFrame> output,
                     oscillate::engine::BlockContext block) noexcept {
    auto& self = *static_cast<Renderer*>(context);
    ++self.calls;
    self.last_position = block.project_sample_position;
    for (std::size_t index = 0; index < output.size(); ++index) {
      output[index] = {static_cast<float>(index + 1), -static_cast<float>(index + 1)};
    }
    return !self.fail;
  }
};

struct Permission {
  bool enabled = true;
  static bool get(const void* data) noexcept {
    return static_cast<const Permission*>(data)->enabled;
  }
};

void matching_rate_and_planar_output() {
  FakeJack fake;
  auto api = fake.api();
  Renderer renderer;
  Permission permission;
  TransportCursor cursor(100);
  JackOutput output({&renderer, &Renderer::render}, cursor, {&permission, &Permission::get}, &api);
  REQUIRE_OK(output.start({48000, 128}), "matching JACK rate starts");
  REQUIRE_EQ(fake.playback_connections, 2, "JACK outputs connect to physical playback ports");
  require(!output.start({48000, 128}), "running JACK output rejects a second start");
  auto status = output.status();
  REQUIRE_EQ(status.state, OutputState::streaming, "status reports observed JACK server values");
  REQUIRE_EQ(status.requested_project_rate, 48000, "status reports observed JACK server values");
  REQUIRE_EQ(status.requested_buffer_frames, 128, "status reports observed JACK server values");
  REQUIRE_EQ(status.server_rate, 48000, "status reports observed JACK server values");
  REQUIRE_EQ(status.server_buffer_frames, 128, "status reports observed JACK server values");
  REQUIRE_EQ(status.xrun_notifications, 0, "status reports observed JACK server values");
  REQUIRE_EQ(status.physical_connections, 2, "status reports observed JACK server values");
  fake.run(0);
  REQUIRE_EQ(output.status().state, OutputState::streaming,
             "zero-frame JACK callback must not latch an error or advance transport");
  REQUIRE_EQ(renderer.calls, 0,
             "zero-frame JACK callback must not latch an error or advance transport");
  REQUIRE_EQ(cursor.completed_snapshot().next_sample, 100,
             "zero-frame JACK callback must not latch an error or advance transport");
  fake.run(4);
  REQUIRE_EQ(renderer.calls, 1, "valid callback advances requested transport");
  REQUIRE_EQ(renderer.last_position, 100, "valid callback advances requested transport");
  status = output.status();
  REQUIRE_TRUE(status.callback_timing_distribution,
               "JACK reports a format-tagged callback timing observation");
  REQUIRE_EQ(status.callback_timing_distribution->rate, 48'000,
             "JACK reports a format-tagged callback timing observation");
  REQUIRE_EQ(status.callback_timing_distribution->frames, 4,
             "JACK reports a format-tagged callback timing observation");
  std::uint64_t measured = 0;
  for (const auto bin : status.callback_timing_distribution->bins) {
    measured += bin;
  }
  REQUIRE_EQ(measured, 1, "callback timing is separate from JACK xrun notifications");
  REQUIRE_EQ(status.xrun_notifications, 0,
             "callback timing is separate from JACK xrun notifications");
  REQUIRE_EQ(status.measured_callback_blocks, 1,
             "JACK callback timing and load come from the shared DeviceTelemetry");
  REQUIRE_OK(status.callback_load,
             "JACK callback timing and load come from the shared DeviceTelemetry");
  REQUIRE_OK(status.callback_over_period_blocks,
             "JACK callback timing and load come from the shared DeviceTelemetry");
  REQUIRE_EQ(fake.left[0], 1.0F, "interleaved graph output reaches distinct planar ports");
  REQUIRE_EQ(fake.left[3], 4.0F, "interleaved graph output reaches distinct planar ports");
  REQUIRE_EQ(fake.right[0], -1.0F, "interleaved graph output reaches distinct planar ports");
  REQUIRE_EQ(fake.right[3], -4.0F, "interleaved graph output reaches distinct planar ports");
  permission.enabled = false;
  fake.left[0] = 99.0F;
  fake.run(4);
  REQUIRE_EQ(renderer.calls, 1, "permission gate silences without advancing transport");
  REQUIRE_EQ(fake.left[0], 0.0F, "permission gate silences without advancing transport");
  permission.enabled = true;
  fake.run(4);
  REQUIRE_EQ(renderer.last_position, 104, "transport resumes at exact paused position");
  renderer.fail = true;
  fake.run(4);
  REQUIRE_EQ(fake.left[0], 0.0F, "failed render clears partial data and increments counter");
  REQUIRE_EQ(output.status().render_failures, 1,
             "failed render clears partial data and increments counter");
  counting_allocations = true;
  fake.emit_xrun();
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "JACK xrun notification is allocation-free");
  REQUIRE_EQ(output.status().xrun_notifications, 1, "JACK xrun callback is counted");
  fake.emit_xrun();
  REQUIRE_EQ(output.status().xrun_notifications, 2,
             "each JACK client notification is counted without stopping playback");
  REQUIRE_EQ(output.status().state, OutputState::streaming,
             "each JACK client notification is counted without stopping playback");
  fake.null_left_buffer = true;
  fake.right[0] = 99.0F;
  fake.run(4);
  REQUIRE_EQ(output.status().invalid_buffers, 1,
             "missing JACK port buffer stops transport and silences the valid port");
  REQUIRE_EQ(renderer.calls, 3,
             "missing JACK port buffer stops transport and silences the valid port");
  REQUIRE_EQ(output.status().state, OutputState::error,
             "missing JACK port buffer stops transport and silences the valid port");
  REQUIRE_EQ(fake.right[0], 0.0F,
             "missing JACK port buffer stops transport and silences the valid port");
  fake.null_left_buffer = false;
  fake.run(4);
  REQUIRE_EQ(renderer.calls, 3, "invalid port does not auto resume on a later callback");
  output.stop();
  REQUIRE_EQ(output.status().state, OutputState::stopped,
             "stop closes the client and retains its completed-run xrun evidence");
  REQUIRE_EQ(fake.closes, 1, "stop closes the client and retains its completed-run xrun evidence");
  REQUIRE_TRUE(!fake.active, "stop closes the client and retains its completed-run xrun evidence");
  REQUIRE_EQ(output.status().xrun_notifications, 2,
             "stop closes the client and retains its completed-run xrun evidence");
  REQUIRE_OK(output.start({48000, 128}),
             "a new JACK output run begins with a fresh notification count");
  REQUIRE_EQ(output.status().xrun_notifications, 0,
             "a new JACK output run begins with a fresh notification count");
  output.stop();
}

void fixed_rate_mismatch_and_live_invalidation() {
  FakeJack fake;
  fake.rate = oscillate::test::sample_rate_44k;
  auto api = fake.api();
  Renderer renderer;
  TransportCursor cursor;
  JackOutput output({&renderer, &Renderer::render}, cursor, {}, &api);
  const auto mismatch = output.start({48000, 128});
  REQUIRE_TRUE(!mismatch, "fixed JACK server mismatch rejects activation");
  REQUIRE_EQ(mismatch.error().code, oscillate::foundation::ErrorCode::unsupported,
             "fixed JACK server mismatch rejects activation");
  REQUIRE_TRUE(output.status().rate_mismatch,
               "mismatch retains diagnostic rate and never activates");
  REQUIRE_EQ(output.status().server_rate, 44100,
             "mismatch retains diagnostic rate and never activates");
  REQUIRE_EQ(fake.activates, 0, "mismatch retains diagnostic rate and never activates");
  REQUIRE_EQ(fake.closes, 1, "mismatch retains diagnostic rate and never activates");
  fake.rate = oscillate::test::sample_rate_48k;
  REQUIRE_OK(output.start({48000, 128}), "matching server can be retried");
  fake.run(4);
  const auto before_rate_change = output.status().callback_timing_distribution;
  fake.change_rate(44100);
  fake.run(4);
  REQUIRE_TRUE(output.status().callback_timing_distribution,
               "JACK callback timing changes epoch with server rate");
  REQUIRE_EQ(output.status().callback_timing_distribution->rate, 44100,
             "JACK callback timing changes epoch with server rate");
  REQUIRE_NE(output.status().callback_timing_distribution->epoch, before_rate_change->epoch,
             "JACK callback timing changes epoch with server rate");
  REQUIRE_EQ(output.status().state, OutputState::error,
             "live rate change immediately mutes and preserves cursor");
  REQUIRE_TRUE(output.status().rate_mismatch,
               "live rate change immediately mutes and preserves cursor");
  REQUIRE_EQ(renderer.calls, 1, "live rate change immediately mutes and preserves cursor");
  REQUIRE_EQ(fake.left[0], 0.0F, "live rate change immediately mutes and preserves cursor");
  fake.change_rate(48000);
  fake.run(4);
  REQUIRE_EQ(renderer.calls, 1, "old client cannot auto resume after rate restoration");
  output.stop();
}

void buffer_change_and_disconnect() {
  FakeJack fake;
  auto api = fake.api();
  Renderer renderer;
  TransportCursor cursor(17);
  JackOutput output({&renderer, &Renderer::render}, cursor, {}, &api);
  REQUIRE_OK(output.start({48000, 128}), "start before buffer change");
  fake.change_size(64);
  fake.run(64);
  REQUIRE_EQ(renderer.calls, 1, "bounded buffer changes remain playable");
  REQUIRE_EQ(output.status().server_buffer_frames, 64, "bounded buffer changes remain playable");
  fake.change_size(8193);
  fake.run(4);
  REQUIRE_EQ(output.status().state, OutputState::error,
             "unsupported buffer change closes callback gate");
  REQUIRE_EQ(renderer.calls, 1, "unsupported buffer change closes callback gate");
  fake.disconnect();
  fake.run(4);
  REQUIRE_EQ(output.status().state, OutputState::disconnected,
             "server shutdown stays silent even if callback repeats");
  REQUIRE_EQ(renderer.calls, 1, "server shutdown stays silent even if callback repeats");
  output.stop();
  REQUIRE_EQ(fake.deactivates, 0,
             "shutdown client must close off callback without deactivating a dead server");
  REQUIRE_EQ(fake.closes, 1,
             "shutdown client must close off callback without deactivating a dead server");
  fake.frames = 128;
  REQUIRE_OK(output.start({48000, 128}),
             "JACK client cannot restart after control-side shutdown cleanup");
  output.stop();
  REQUIRE_EQ(fake.deactivates, 1, "healthy restarted client did not deactivate and close normally");
  REQUIRE_EQ(fake.closes, 2, "healthy restarted client did not deactivate and close normally");
}

void oversized_callback_is_sticky() {
  FakeJack fake;
  auto api = fake.api();
  Renderer renderer;
  TransportCursor cursor(31);
  JackOutput output({&renderer, &Renderer::render}, cursor, {}, &api);
  REQUIRE_OK(output.start({48000, 128}), "start before oversized callback");
  fake.left.fill(99.0F);
  fake.right.fill(99.0F);
  fake.run(8193);
  REQUIRE_EQ(output.status().oversized_blocks, 1,
             "oversized callback clears the entire JACK buffer and closes the playback gate");
  REQUIRE_EQ(output.status().state, OutputState::error,
             "oversized callback clears the entire JACK buffer and closes the playback gate");
  REQUIRE_EQ(fake.left[8192], 0.0F,
             "oversized callback clears the entire JACK buffer and closes the playback gate");
  REQUIRE_EQ(fake.right[8192], 0.0F,
             "oversized callback clears the entire JACK buffer and closes the playback gate");
  REQUIRE_EQ(renderer.calls, 0,
             "oversized callback clears the entire JACK buffer and closes the playback gate");
  fake.run(4);
  REQUIRE_EQ(renderer.calls, 0, "smaller subsequent callback cannot reactivate transport");
  REQUIRE_EQ(fake.left[0], 0.0F, "smaller subsequent callback cannot reactivate transport");
  output.stop();
}

void exact_buffer_policy() {
  for (const std::uint32_t prepared : {64U, 128U}) {
    FakeJack fake;
    fake.frames = prepared;
    auto api = fake.api();
    Renderer renderer;
    TransportCursor cursor(37);
    JackOutput output({&renderer, &Renderer::render}, cursor, {}, &api);
    const JackStartConfig config{48000, prepared, JackBufferPolicy::exact};
    REQUIRE_OK(output.start(config), "exact JACK size starts when prepared size matches");
    REQUIRE_TRUE(output.status().exact_buffer_size_required,
                 "exact policy is visible in device status");
    REQUIRE_TRUE(!output.status().buffer_size_mismatch, "exact policy is visible in device status");
    counting_allocations = true;
    fake.run(prepared);
    counting_allocations = false;
    REQUIRE_EQ(counted_allocations, 0,
               "exact-size render is allocation-free and advances precisely one callback");
    REQUIRE_EQ(renderer.calls, 1,
               "exact-size render is allocation-free and advances precisely one callback");
    REQUIRE_EQ(renderer.last_position, 37,
               "exact-size render is allocation-free and advances precisely one callback");
    REQUIRE_EQ(cursor.completed_snapshot().next_sample, 37 + prepared,
               "exact-size render is allocation-free and advances precisely one callback");
    const auto last_completed = cursor.completed_snapshot().next_sample;
    fake.left.fill(99.0F);
    fake.right.fill(99.0F);
    counting_allocations = true;
    fake.change_size(prepared == 64 ? 128 : 64);
    fake.run(fake.frames);
    counting_allocations = false;
    auto status = output.status();
    REQUIRE_EQ(counted_allocations, 0,
               "size-change callback invalidates qualification and silences full device block");
    REQUIRE_EQ(status.state, OutputState::error,
               "size-change callback invalidates qualification and silences full device block");
    REQUIRE_TRUE(status.buffer_size_mismatch,
                 "size-change callback invalidates qualification and silences full device block");
    REQUIRE_EQ(status.mismatched_buffer_frames, fake.frames,
               "size-change callback invalidates qualification and silences full device block");
    REQUIRE_EQ(status.server_buffer_frames, fake.frames,
               "size-change callback invalidates qualification and silences full device block");
    REQUIRE_EQ(renderer.calls, 1,
               "size-change callback invalidates qualification and silences full device block");
    REQUIRE_EQ(cursor.completed_snapshot().next_sample, last_completed,
               "size-change callback invalidates qualification and silences full device block");
    REQUIRE_EQ(fake.left[0], 0.0F,
               "size-change callback invalidates qualification and silences full device block");
    REQUIRE_EQ(fake.left[fake.frames - 1], 0.0F,
               "size-change callback invalidates qualification and silences full device block");
    REQUIRE_EQ(fake.right[fake.frames - 1], 0.0F,
               "size-change callback invalidates qualification and silences full device block");
    fake.change_size(prepared);
    fake.run(prepared);
    REQUIRE_EQ(renderer.calls, 1, "restoring server size cannot reactivate the unqualified graph");
    REQUIRE_EQ(cursor.completed_snapshot().next_sample, last_completed,
               "restoring server size cannot reactivate the unqualified graph");
    output.stop();
    REQUIRE_OK(output.start(config), "control-side stop and reprepare permits restart");
    REQUIRE_TRUE(!output.status().buffer_size_mismatch,
                 "fresh start clears the prior mismatch diagnostic");
    REQUIRE_TRUE(!output.status().mismatched_buffer_frames,
                 "fresh start clears the prior mismatch diagnostic");
    fake.run(prepared);
    REQUIRE_EQ(renderer.calls, 2, "restarted graph resumes at the last completed sample");
    REQUIRE_EQ(renderer.last_position, last_completed,
               "restarted graph resumes at the last completed sample");
    output.stop();
  }
}

void exact_buffer_startup_and_unannounced_change() {
  FakeJack fake;
  fake.frames = 64;
  auto api = fake.api();
  Renderer renderer;
  TransportCursor cursor(17);
  JackOutput output({&renderer, &Renderer::render}, cursor, {}, &api);
  const JackStartConfig config{48000, 128, JackBufferPolicy::exact};
  const auto mismatch = output.start(config);
  REQUIRE_TRUE(!mismatch, "startup refuses unprepared JACK server size before activation");
  REQUIRE_EQ(mismatch.error().code, oscillate::foundation::ErrorCode::unsupported,
             "startup refuses unprepared JACK server size before activation");
  REQUIRE_EQ(fake.activates, 0, "startup refuses unprepared JACK server size before activation");
  REQUIRE_EQ(fake.closes, 1, "startup refuses unprepared JACK server size before activation");
  REQUIRE_TRUE(output.status().buffer_size_mismatch,
               "startup refuses unprepared JACK server size before activation");
  REQUIRE_EQ(output.status().mismatched_buffer_frames, 64,
             "startup refuses unprepared JACK server size before activation");
  fake.frames = 128;
  fake.size_change_during_activate = 64;
  REQUIRE_TRUE(!output.start(config),
               "activation-time size change cannot leave the graph streaming");
  REQUIRE_EQ(fake.closes, 2, "activation-time size change cannot leave the graph streaming");
  REQUIRE_TRUE(!fake.active, "activation-time size change cannot leave the graph streaming");
  REQUIRE_TRUE(output.status().buffer_size_mismatch,
               "activation-time size change cannot leave the graph streaming");
  fake.size_change_during_activate = 0;
  fake.frames = 128;
  REQUIRE_OK(output.start(config), "valid exact-size client can start after failed race");
  fake.left.fill(99.0F);
  fake.right.fill(99.0F);
  counting_allocations = true;
  fake.run(64); // Deliberately omit the JACK size notification.
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0,
             "actual process size is checked before rendering even without a size event");
  REQUIRE_EQ(renderer.calls, 0,
             "actual process size is checked before rendering even without a size event");
  REQUIRE_EQ(cursor.completed_snapshot().next_sample, 17,
             "actual process size is checked before rendering even without a size event");
  REQUIRE_EQ(output.status().mismatched_buffer_frames, 64,
             "actual process size is checked before rendering even without a size event");
  REQUIRE_EQ(fake.left[63], 0.0F,
             "actual process size is checked before rendering even without a size event");
  REQUIRE_EQ(fake.right[63], 0.0F,
             "actual process size is checked before rendering even without a size event");
  fake.run(128);
  REQUIRE_EQ(renderer.calls, 0, "matching callbacks stay silent until control-side restart");
  REQUIRE_EQ(cursor.completed_snapshot().next_sample, 17,
             "matching callbacks stay silent until control-side restart");
  output.stop();
  REQUIRE_OK(output.start(config), "exact policy restarts for zero-size callback test");
  fake.run(0);
  REQUIRE_TRUE(output.status().buffer_size_mismatch,
               "zero-frame exact callback invalidates without advancing transport");
  REQUIRE_EQ(output.status().mismatched_buffer_frames, 0,
             "zero-frame exact callback invalidates without advancing transport");
  REQUIRE_EQ(cursor.completed_snapshot().next_sample, 17,
             "zero-frame exact callback invalidates without advancing transport");
  fake.run(128);
  REQUIRE_EQ(renderer.calls, 0, "zero-frame invalidation is sticky");
  output.stop();
}

void startup_failures() {
  FakeJack fake;
  auto api = fake.api();
  Renderer renderer;
  TransportCursor cursor;
  JackOutput output({&renderer, &Renderer::render}, cursor, {}, &api);
  require(!output.start({12345, 128}), "unsupported project rate rejected");
  REQUIRE_TRUE(!output.start({48000, 0}), "invalid requested buffer sizes rejected");
  REQUIRE_TRUE(!output.start({48000, 8193}), "invalid requested buffer sizes rejected");
  REQUIRE_EQ(fake.opens, 0, "invalid settings never open JACK");
  fake.fail_open = true;
  require(!output.start({48000, 128}), "unavailable JACK client reported");
  fake.fail_open = false;
  fake.fail_activate = true;
  require(!output.start({48000, 128}), "activation failure reported");
  REQUIRE_EQ(fake.closes, 1, "activation failure closes client");
  fake.fail_activate = false;
  fake.fail_register = true;
  require(!output.start({48000, 128}), "port registration failure reported");
  REQUIRE_EQ(fake.closes, 2, "port registration failure closes client");
  fake.fail_register = false;
  fake.fail_callback = true;
  require(!output.start({48000, 128}), "callback registration failure reported");
  REQUIRE_EQ(fake.closes, 3, "callback registration failure closes client");
  fake.fail_callback = false;
  fake.frames = 8193;
  require(!output.start({48000, 128}), "unsupported server buffer size rejected");
  REQUIRE_EQ(fake.activates, 1, "oversized server buffer is rejected before activation");
  REQUIRE_EQ(fake.closes, 4, "oversized server buffer is rejected before activation");
  fake.frames = 128;
  fake.rate_change_during_activate = true;
  require(!output.start({48000, 128}), "rate change during activation rejects stale ready state");
  REQUIRE_TRUE(!fake.opened, "activation race closes client and retains mismatch diagnostic");
  REQUIRE_TRUE(output.status().rate_mismatch,
               "activation race closes client and retains mismatch diagnostic");
}
} // namespace

int main() {
  try {
    matching_rate_and_planar_output();
    fixed_rate_mismatch_and_live_invalidation();
    buffer_change_and_disconnect();
    oversized_callback_is_sticky();
    exact_buffer_policy();
    exact_buffer_startup_and_unannounced_change();
    startup_failures();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
