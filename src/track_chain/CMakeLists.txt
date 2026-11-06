# One chain per track (ADR-0050): live playback and export prepare and switch
# every track and master chain here.
oscillate_add_library(track_chain SOURCES track_chain.cpp
  PUBLIC oscillate::project oscillate::engine oscillate::routing oscillate::native_inserts)
