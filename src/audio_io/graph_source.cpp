#include <oscillate/audio_io/graph_source.hpp>

#include <oscillate/engine/graph.hpp>

namespace oscillate::audio_io {
namespace {
bool render_graph(void* context, std::span<engine::StereoFrame> stereo,
                  engine::BlockContext block) noexcept {
  return static_cast<engine::GraphPublisher*>(context)->render_block(stereo, block);
}
} // namespace

StereoRenderSource graph_render_source(engine::GraphPublisher& publisher) noexcept {
  return {&publisher, &render_graph};
}
} // namespace oscillate::audio_io
