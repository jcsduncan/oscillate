oscillate_add_test_executable(oscillate_audio_io_tests SOURCES audio_io_tests.cpp
  LIBS oscillate::audio_io
  TEST audio_io_contract)

if(OSCILLATE_AUDIO_IO_LIVE_TESTS)
  oscillate_add_test_executable(oscillate_audio_io_live_smoke SOURCES pipewire_live_smoke.cpp
    LIBS oscillate::audio_io
    TEST audio_io_pipewire_live TIER live)
  if(JACK_FOUND)
    oscillate_add_test_executable(oscillate_jack_live_smoke SOURCES jack_live_smoke.cpp
      LIBS oscillate::audio_io PkgConfig::JACK)
    oscillate_add_test(NAME audio_io_jack_live COMMAND oscillate_jack_live_smoke connect TIER live)
    oscillate_add_test(NAME audio_io_jack_live_mismatch COMMAND oscillate_jack_live_smoke mismatch TIER live)
  endif()
endif()
