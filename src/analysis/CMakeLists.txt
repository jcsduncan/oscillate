oscillate_add_library(analysis
  SOURCES waveform.cpp tempo.cpp key.cpp cache_payload.cpp mix_check.cpp
  PUBLIC oscillate::media
  PRIVATE oscillate::dsp_primitives)
