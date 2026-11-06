#pragma once

#include "oscillate/project/model.hpp"

#include <span>
#include <string>
#include <vector>

namespace oscillate::application {

// What changed between two snapshots of one project, in a few words: the
// undo history panel's labels, so every command is named without each one
// declaring a label
// ("Add Pump on Bass", "Edit notes on Lead", "Move 3 clips"). Never empty.
[[nodiscard]] std::string describe_change(const project::Snapshot& before,
                                          const project::Snapshot& after);

} // namespace oscillate::application
