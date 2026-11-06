# Harmony (ADR-0083): the chord table, voicing, the Progression Lab, next
# chords, substitutions and hints. Pure; Comp voices on the callback with it.
oscillate_add_library(harmony
  SOURCES chords.cpp voicing.cpp lab.cpp edits.cpp clip.cpp
  PUBLIC oscillate::project oscillate::timeline
  OPTIMIZE)
# GCC 15 at -O3 inlines std::sort over a counted prefix of a fixed std::array
# (a voicing's notes) and reports libstdc++'s heap code reading past the
# array, on paths the count rules out (a known -Warray-bounds false positive).
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set_source_files_properties(clip.cpp PROPERTIES COMPILE_OPTIONS -Wno-array-bounds)
endif()
