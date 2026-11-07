#pragma once

#include "oscillate/application/instrument_playback.hpp"

namespace oscillate::quick {

// Empty when this build has no JACK client library. JACK uses the same
// stereo render source and transport boundary as the PipeWire output.
[[nodiscard]] application::PlaybackOutputFactory make_jack_playback_output_factory();
// A compiled JACK adapter does not imply that a server is running.
[[nodiscard]] bool jack_server_available() noexcept;

} // namespace oscillate::quick
