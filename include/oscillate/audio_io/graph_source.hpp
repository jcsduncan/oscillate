#pragma once

#include <oscillate/audio_io/stereo_block.hpp>

namespace oscillate::engine {
class GraphPublisher;
}

namespace oscillate::audio_io {
[[nodiscard]] StereoRenderSource graph_render_source(engine::GraphPublisher& publisher) noexcept;
}
