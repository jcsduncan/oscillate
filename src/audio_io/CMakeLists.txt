oscillate_add_library(audio_io
  SOURCES alsa_output.cpp device_reservation.cpp graph_source.cpp metronome.cpp
          pipewire_output.cpp pipewire_sinks.cpp render_ahead.cpp stereo_block.cpp telemetry.cpp
  PUBLIC oscillate::engine oscillate::foundation
  PRIVATE PkgConfig::PIPEWIRE PkgConfig::ALSA PkgConfig::DBUS)
target_compile_definitions(oscillate_audio_io PUBLIC OSCILLATE_HAVE_JACK=$<BOOL:${JACK_FOUND}>)

if(JACK_FOUND)
  target_sources(oscillate_audio_io PRIVATE jack_output.cpp)
  target_link_libraries(oscillate_audio_io PRIVATE PkgConfig::JACK)
  if(BUILD_TESTING)
    # Declared beside the optional adapter it tests.
    oscillate_add_test_executable(oscillate_jack_output_tests
      SOURCES ${PROJECT_SOURCE_DIR}/tests/audio_io/jack_output_tests.cpp
      LIBS oscillate::audio_io
      TEST audio_io_jack_contract)
  endif()
endif()
