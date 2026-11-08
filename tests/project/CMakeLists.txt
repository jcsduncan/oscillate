oscillate_add_test_executable(oscillate_project_tests SOURCES model_tests.cpp
  LIBS oscillate::project
  TEST project_contract)

oscillate_add_test_executable(oscillate_midi_project_tests SOURCES midi_model_tests.cpp
  LIBS oscillate::project
  TEST midi_project_contract)

oscillate_add_test_executable(oscillate_clip_fade_tests SOURCES clip_fade_tests.cpp
  LIBS oscillate::project
  TEST project_clip_fade_contract)

oscillate_add_test_executable(oscillate_native_instrument_model_tests SOURCES native_instrument_model_tests.cpp
  LIBS oscillate::project
  TEST project_native_instrument_contract)

oscillate_add_test_executable(oscillate_project_history_tests SOURCES model_history_tests.cpp
  LIBS oscillate::project
  TEST project_history_contract)

oscillate_add_test_executable(oscillate_render_slices_tests SOURCES render_slices_tests.cpp
  LIBS oscillate::project
  TEST project_render_slices)
