#include <oscillate/audio_io/pipewire_sinks.hpp>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <mutex>
#include <ranges>
#include <system_error>

#include <pipewire/pipewire.h>

namespace oscillate::audio_io {

std::optional<PipeWireSink> parse_pipewire_sink(PipeWireSinkProperties properties) {
  if (properties.media_class == nullptr || std::strcmp(properties.media_class, "Audio/Sink") != 0 ||
      properties.serial == nullptr) {
    return std::nullopt;
  }
  const auto length = strnlen(properties.serial, 21);
  if (length == 0 || length > 20) {
    return std::nullopt;
  }
  std::uint64_t parsed_serial = 0;
  const auto parsed = std::from_chars(properties.serial, properties.serial + length, parsed_serial);
  if (parsed.ec != std::errc{} || parsed.ptr != properties.serial + length || parsed_serial == 0) {
    return std::nullopt;
  }
  const char* label = properties.description != nullptr && *properties.description != '\0'
                          ? properties.description
                          : properties.node_name;
  if (label == nullptr || *label == '\0') {
    return std::nullopt;
  }
  return PipeWireSink{parsed_serial, std::string(label, strnlen(label, 160))};
}

namespace {
struct RegistrySnapshot {
  static void on_global(void* data, std::uint32_t, std::uint32_t, const char* type, std::uint32_t,
                        const spa_dict* props) noexcept {
    auto& self = *static_cast<RegistrySnapshot*>(data);
    if (type == nullptr || std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0 || props == nullptr) {
      return;
    }
    try {
      auto sink = parse_pipewire_sink({spa_dict_lookup(props, PW_KEY_MEDIA_CLASS),
                                       spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL),
                                       spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION),
                                       spa_dict_lookup(props, PW_KEY_NODE_NAME)});
      if (sink && self.sinks.size() < 256) {
        self.sinks.push_back(std::move(*sink));
      }
    } catch (...) {
      self.failed = true;
      pw_thread_loop_signal(self.loop, false);
    }
  }

  static void on_done(void* data, std::uint32_t, int sequence) noexcept {
    auto& self = *static_cast<RegistrySnapshot*>(data);
    if (sequence == self.sequence) {
      self.done = true;
      pw_thread_loop_signal(self.loop, false);
    }
  }

  static void on_error(void* data, std::uint32_t, int, int, const char*) noexcept {
    auto& self = *static_cast<RegistrySnapshot*>(data);
    self.failed = true;
    pw_thread_loop_signal(self.loop, false);
  }

  pw_thread_loop* loop = nullptr;
  pw_context* context = nullptr;
  pw_core* core = nullptr;
  pw_registry* registry = nullptr;
  spa_hook core_hook{};
  spa_hook registry_hook{};
  pw_core_events core_events{};
  pw_registry_events registry_events{};
  std::vector<PipeWireSink> sinks;
  int sequence = -1;
  bool done = false;
  bool failed = false;
  bool registry_listening = false;
  bool core_listening = false;
};
} // namespace

foundation::Result<std::vector<PipeWireSink>>
list_pipewire_sinks(std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero() || timeout > std::chrono::seconds(10)) {
    return foundation::Result<std::vector<PipeWireSink>>::failure(
        {foundation::ErrorCode::invalid_argument, "Invalid PipeWire discovery timeout"});
  }
  static std::once_flag initialized;
  std::call_once(initialized, [] { pw_init(nullptr, nullptr); });

  RegistrySnapshot state;
  state.loop = pw_thread_loop_new("Oscillate sink discovery", nullptr);
  if (state.loop == nullptr) {
    return foundation::Result<std::vector<PipeWireSink>>::failure(
        {foundation::ErrorCode::unavailable, "Cannot create PipeWire discovery loop"});
  }
  if (pw_thread_loop_start(state.loop) < 0) {
    pw_thread_loop_destroy(state.loop);
    return foundation::Result<std::vector<PipeWireSink>>::failure(
        {foundation::ErrorCode::unavailable, "Cannot start PipeWire discovery loop"});
  }

  pw_thread_loop_lock(state.loop);
  state.context = pw_context_new(pw_thread_loop_get_loop(state.loop), nullptr, 0);
  if (state.context != nullptr) {
    state.core = pw_context_connect(state.context, nullptr, 0);
  }
  if (state.core != nullptr) {
    state.registry = pw_core_get_registry(state.core, PW_VERSION_REGISTRY, 0);
  }
  if (state.registry != nullptr) {
    state.registry_events.version = PW_VERSION_REGISTRY_EVENTS;
    state.registry_events.global = &RegistrySnapshot::on_global;
    state.registry_listening = pw_registry_add_listener(state.registry, &state.registry_hook,
                                                        &state.registry_events, &state) >= 0;
    if (state.registry_listening) {
      state.core_events.version = PW_VERSION_CORE_EVENTS;
      state.core_events.done = &RegistrySnapshot::on_done;
      state.core_events.error = &RegistrySnapshot::on_error;
      state.core_listening =
          pw_core_add_listener(state.core, &state.core_hook, &state.core_events, &state) >= 0;
    }
    if (state.core_listening) {
      state.sequence = pw_core_sync(state.core, PW_ID_CORE, 0);
    }
    if (state.sequence >= 0) {
      timespec deadline{};
      if (pw_thread_loop_get_time(
              state.loop, &deadline,
              std::chrono::duration_cast<std::chrono::nanoseconds>(timeout).count()) >= 0) {
        while (!state.done && !state.failed &&
               pw_thread_loop_timed_wait_full(state.loop, &deadline) == 0) {
        }
      }
    }
  }
  pw_thread_loop_unlock(state.loop);
  pw_thread_loop_stop(state.loop);
  if (state.registry != nullptr) {
    if (state.registry_listening) {
      spa_hook_remove(&state.registry_hook);
    }
    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(state.registry));
  }
  if (state.core != nullptr) {
    if (state.core_listening) {
      spa_hook_remove(&state.core_hook);
    }
    pw_core_disconnect(state.core);
  }
  if (state.context != nullptr) {
    pw_context_destroy(state.context);
  }
  pw_thread_loop_destroy(state.loop);
  if (!state.done || state.failed) {
    return foundation::Result<std::vector<PipeWireSink>>::failure(
        {foundation::ErrorCode::unavailable, "PipeWire output discovery failed or timed out"});
  }
  std::ranges::sort(state.sinks,
                    [](const auto& left, const auto& right) { return left.serial < right.serial; });
  const auto duplicates = std::ranges::unique(
      state.sinks, [](const auto& left, const auto& right) { return left.serial == right.serial; });
  state.sinks.erase(duplicates.begin(), duplicates.end());
  std::ranges::sort(state.sinks, [](const auto& left, const auto& right) {
    return left.name == right.name ? left.serial < right.serial : left.name < right.name;
  });
  return foundation::Result<std::vector<PipeWireSink>>::success(std::move(state.sinks));
}

} // namespace oscillate::audio_io
