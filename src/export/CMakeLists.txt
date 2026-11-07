oscillate_add_library(export
  SOURCES wav_writer.cpp master_output.cpp offline_master.cpp processed_clip.cpp
          realtime_capture.cpp realtime_tail_plan.cpp
  # master_output.hpp is shared with the VST3 exporters.
  PUBLIC oscillate::foundation oscillate::project oscillate::warp_render_cache oscillate::routing
  # The graph comes from render_graph; native_dsp supplies the loudness and
  # true-peak meters.
  PRIVATE oscillate::render_graph oscillate::source_prep oscillate::engine oscillate::media
          oscillate::native_dsp oscillate::persistence
  DEFINITIONS OSCILLATE_HAVE_CODEC=$<BOOL:${OSCILLATE_BUILD_CODEC_ADAPTER}>
              OSCILLATE_HAVE_EXPORT_RESAMPLER=$<BOOL:${OSCILLATE_BUILD_EXPORT_RESAMPLER}>
              OSCILLATE_HAVE_CONVOLUTION=$<BOOL:${OSCILLATE_BUILD_CONVOLUTION_DSP}>)
if(OSCILLATE_BUILD_EXPORT_RESAMPLER)
  target_include_directories(oscillate_export SYSTEM PRIVATE ${OSCILLATE_SAMPLERATE_INCLUDE_DIR})
  target_link_libraries(oscillate_export PRIVATE ${OSCILLATE_SAMPLERATE_LIBRARY})
endif()
if(OSCILLATE_BUILD_CODEC_ADAPTER)
  target_sources(oscillate_export PRIVATE encoded_writer.cpp)
  target_include_directories(oscillate_export SYSTEM PRIVATE ${OSCILLATE_SNDFILE_INCLUDE_DIR})
  target_link_libraries(oscillate_export PRIVATE ${OSCILLATE_SNDFILE_LIBRARY})
endif()

if(JACK_FOUND)
  # Keep the JACK-specific capture adapter separate from the offline exporter.
  oscillate_add_library(jack_realtime_export SOURCES jack_realtime_export.cpp
    PUBLIC oscillate::export oscillate::audio_io)
endif()
