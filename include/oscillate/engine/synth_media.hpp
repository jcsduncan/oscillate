#pragma once

#include "oscillate/engine/graph.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace oscillate::engine {

// Decoded audio for one managed media slot of a built-in synth's document
// (ADR-0057), at its source rate. Immutable once shared; decoded on control
// or a worker, never the callback. Source preparation produces it and the
// synth runtime reads it, so it lives here, below both.
struct SynthMediaAsset {
  std::uint32_t source_rate = 0;
  std::vector<StereoFrame> frames;
};
struct SynthMediaEntry {
  std::string slot;   // The document's media slot id.
  std::string sha256; // The managed source the asset was decoded from.
  std::shared_ptr<const SynthMediaAsset> asset;
};
// Every managed slot of a document, decoded. Builtin slots are the engine's
// own content and never appear here.
using SynthMedia = std::vector<SynthMediaEntry>;

} // namespace oscillate::engine
