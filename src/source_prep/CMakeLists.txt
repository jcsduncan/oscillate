oscillate_add_library(source_prep SOURCES managed_audio.cpp synth_media.cpp
  PUBLIC oscillate::engine oscillate::foundation
  PRIVATE oscillate::media oscillate::persistence oscillate::project)
