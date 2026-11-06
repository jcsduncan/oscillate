oscillate_add_test_executable(oscillate_source_prep_tests SOURCES managed_audio_tests.cpp
  LIBS oscillate::source_prep oscillate::persistence
  DEFINITIONS OSCILLATE_HAVE_CODEC=$<BOOL:${OSCILLATE_BUILD_CODEC_ADAPTER}>)
if(OSCILLATE_BUILD_CODEC_ADAPTER)
  target_compile_definitions(oscillate_source_prep_tests PRIVATE
    OSCILLATE_CODEC_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/codec/assets")
endif()
oscillate_add_test(NAME source_prep_managed_audio COMMAND oscillate_source_prep_tests)
