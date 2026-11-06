oscillate_add_test_executable(oscillate_warp_dsp_time_map_tests SOURCES time_map_tests.cpp
  LIBS oscillate::warp_dsp
  TEST warp_dsp.time_map)

oscillate_add_test_executable(oscillate_warp_dsp_core_tests SOURCES core_tests.cpp
  LIBS oscillate::warp_dsp
  INCLUDES ${PROJECT_SOURCE_DIR}/src/warp_dsp
  TEST warp_dsp.core)

oscillate_add_test_executable(oscillate_warp_dsp_repitch_tests SOURCES repitch_tests.cpp
  LIBS oscillate::warp_dsp
  INCLUDES ${PROJECT_SOURCE_DIR}/src/warp_dsp
  TEST warp_dsp.repitch)

oscillate_add_test_executable(oscillate_warp_dsp_transient_tests SOURCES transient_tests.cpp
  LIBS oscillate::warp_dsp
  TEST warp_dsp.transients)

oscillate_add_test_executable(oscillate_warp_dsp_beats_tests SOURCES beats_tests.cpp
  LIBS oscillate::warp_dsp
  TEST warp_dsp.beats TIER slow)

oscillate_add_test_executable(oscillate_warp_dsp_complex_tests SOURCES complex_tests.cpp
  LIBS oscillate::warp_dsp
  TEST warp_dsp.complex TIER slow)

oscillate_add_test_executable(oscillate_warp_dsp_pitch_tests SOURCES pitch_tests.cpp
  LIBS oscillate::warp_dsp
  TEST warp_dsp.pitch)

oscillate_add_test_executable(oscillate_warp_dsp_grains_tests SOURCES grains_tests.cpp
  LIBS oscillate::warp_dsp
  TEST warp_dsp.grains TIER slow)

oscillate_add_test_executable(oscillate_warp_dsp_rate_view_tests SOURCES rate_view_tests.cpp
  LIBS oscillate::warp_dsp
  TEST warp_dsp.rate_view TIER slow)
