oscillate_add_test_executable(oscillate_analysis_tests SOURCES waveform_tests.cpp
  LIBS oscillate::analysis
  DEFINITIONS OSCILLATE_HAVE_CODEC=$<BOOL:${OSCILLATE_BUILD_CODEC_ADAPTER}>
    OSCILLATE_CODEC_FIXTURE_DIR="${CMAKE_CURRENT_LIST_DIR}/../codec/assets"
  TEST analysis_waveform)

oscillate_add_test_executable(oscillate_tempo_tests SOURCES tempo_tests.cpp
  LIBS oscillate::analysis
  TEST analysis_tempo)
oscillate_add_test(NAME analysis_tempo_groove COMMAND oscillate_tempo_tests --groove TIER slow)

oscillate_add_test_executable(oscillate_analysis_cache_tests SOURCES cache_payload_tests.cpp
  LIBS oscillate::analysis
  TEST analysis_cache_payload)

oscillate_add_test_executable(oscillate_key_tests SOURCES key_tests.cpp
  LIBS oscillate::analysis
  TEST analysis_key)

# Mix Check's measurements (ADR-0074) on planted problems.
oscillate_add_test_executable(oscillate_mix_check_analysis_tests SOURCES mix_check_tests.cpp
  LIBS oscillate::analysis
  TEST analysis_mix_check TIER slow)
