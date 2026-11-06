oscillate_add_test_executable(oscillate_midi_file_tests SOURCES smf_tests.cpp
  LIBS oscillate::midi_file
  TEST midi_file_contract TIER slow)
