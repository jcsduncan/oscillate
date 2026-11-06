#pragma once

namespace oscillate::quick {

// Adds the fonts compiled into the shell (apps/daw_quick/fonts: Inter and
// JetBrains Mono) to the application's font database. Call once after the
// application object exists and before QML loads. Returns false if any
// failed to load; the panels then fall back to system fonts.
bool register_bundled_fonts();

} // namespace oscillate::quick
