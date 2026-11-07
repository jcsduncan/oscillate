#pragma once

#include <filesystem>

namespace oscillate::quick::screenshot {

// Writes "Harbour Lights", the documentation screenshots' project: 32 bars
// of melodic house at 124 BPM in A minor on seven built-in instruments, with
// built-in effects on the chords, arpeggio, bells and master. Throws when the
// folder already exists or the project does not save.
void create_demo_project(const std::filesystem::path& root);

} // namespace oscillate::quick::screenshot
