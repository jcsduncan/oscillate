#pragma once

#include <oscillate/engine/synth_media.hpp>
#include <oscillate/foundation/error.hpp>

#include <stop_token>

namespace oscillate::persistence {
class ProjectStore;
}
namespace oscillate::project {
struct NativeSynthDocument;
struct Snapshot;
} // namespace oscillate::project

namespace oscillate::source_prep {

// Worker/control-thread preparation (ADR-0057): decodes every managed media
// slot of a built-in synth's document from verified managed sources. Slots
// that name the same source share one decoded asset. Builtin slots are the
// engine's own and are skipped.
[[nodiscard]] foundation::Result<engine::SynthMedia>
load_synth_media(const persistence::ProjectStore& store, const project::Snapshot& snapshot,
                 const project::NativeSynthDocument& document, const std::stop_token& stop = {});

} // namespace oscillate::source_prep
