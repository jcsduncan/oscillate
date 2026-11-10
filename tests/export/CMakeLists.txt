oscillate_add_test_executable(oscillate_export_tests SOURCES wav_writer_tests.cpp
  LIBS oscillate::export
  INCLUDES ${CMAKE_CURRENT_LIST_DIR}/../../src/export
  TEST export_wav_writer)

# Dither and noise shaping for integer output (ADR-0117).
oscillate_add_test_executable(oscillate_dither_tests SOURCES dither_tests.cpp
  LIBS oscillate::export)
oscillate_add_case_tests(export_dither_contract oscillate_dither_tests
  CASES flat_dither_is_white_at_a_quarter_lsb shaped_noise_follows_its_curve
        high_rates_push_the_noise_above_the_band feedback_stays_bounded_when_clipping
        flat_is_the_seeded_tpdf the_writer_applies_the_shape)

oscillate_add_test_executable(oscillate_realtime_capture_tests SOURCES realtime_capture_tests.cpp
  LIBS oscillate::export
  TEST export_realtime_capture)
oscillate_add_test_executable(oscillate_realtime_tail_plan_tests SOURCES realtime_tail_plan_tests.cpp
  LIBS oscillate::export
  TEST export_realtime_tail_plan)
if(JACK_FOUND)
  oscillate_add_test_executable(oscillate_jack_realtime_export_tests SOURCES jack_realtime_export_tests.cpp
    LIBS oscillate::jack_realtime_export
    TEST export_jack_realtime)
endif()

oscillate_add_test_executable(oscillate_offline_master_tests SOURCES offline_master_tests.cpp
  LIBS oscillate::export oscillate::persistence oscillate::native_dsp oscillate::native_inserts
  INCLUDES ${CMAKE_CURRENT_LIST_DIR}/../../src/export
  DEFINITIONS OSCILLATE_HAVE_CODEC=$<BOOL:${OSCILLATE_BUILD_CODEC_ADAPTER}>
    OSCILLATE_HAVE_EXPORT_RESAMPLER=$<BOOL:${OSCILLATE_BUILD_EXPORT_RESAMPLER}>
    OSCILLATE_CODEC_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/codec/assets"
  TEST export_offline_master TIER slow)

oscillate_add_test_executable(oscillate_warp_export_tests SOURCES warp_export_tests.cpp
  LIBS oscillate::application oscillate::export oscillate::persistence
  TEST export_warp TIER slow)
if(OSCILLATE_BUILD_CODEC_ADAPTER)
  target_include_directories(oscillate_offline_master_tests SYSTEM PRIVATE
    ${OSCILLATE_SNDFILE_INCLUDE_DIR})
  target_link_libraries(oscillate_offline_master_tests PRIVATE ${OSCILLATE_SNDFILE_LIBRARY})
endif()

if(OSCILLATE_BUILD_CODEC_ADAPTER)
  oscillate_add_test_executable(oscillate_encoded_writer_tests SOURCES encoded_writer_tests.cpp
    LIBS oscillate::export ${OSCILLATE_SNDFILE_LIBRARY}
    INCLUDES ${CMAKE_CURRENT_LIST_DIR}/../../src/export
    TEST export_encoded_writer)
  target_include_directories(oscillate_encoded_writer_tests SYSTEM PRIVATE
    ${OSCILLATE_SNDFILE_INCLUDE_DIR})
endif()
