#pragma once

#include <oscillate/foundation/error.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace oscillate::audio_io {

// Object serials, unlike registry node IDs, are stable for the lifetime of a
// PipeWire object. A vanished object must be selected again after rediscovery.
struct PipeWireSink {
  std::uint64_t serial = 0;
  std::string name;
};

struct PipeWireSinkProperties {
  const char* media_class = nullptr;
  const char* serial = nullptr;
  const char* description = nullptr;
  const char* node_name = nullptr;
};

// Pure parser for registry properties; exposed for deterministic contract tests.
[[nodiscard]] std::optional<PipeWireSink> parse_pipewire_sink(PipeWireSinkProperties properties);

// Control/worker thread only. Returns one bounded registry snapshot. The system
// default output is represented by a null target serial, not a synthetic sink.
[[nodiscard]] foundation::Result<std::vector<PipeWireSink>>
list_pipewire_sinks(std::chrono::milliseconds timeout = std::chrono::seconds(2));

} // namespace oscillate::audio_io
