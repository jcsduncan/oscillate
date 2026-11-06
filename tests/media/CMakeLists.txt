oscillate_add_test_executable(oscillate_media_tests SOURCES prefetch_tests.cpp
  LIBS oscillate::media
  TEST media_prefetch)

oscillate_add_test_executable(oscillate_media_cache_contract_tests SOURCES cache_contract_tests.cpp
  LIBS oscillate::media
  TEST media_cache_contract)

oscillate_add_test_executable(oscillate_media_cache_store_tests SOURCES cache_store_tests.cpp
  LIBS oscillate::media oscillate::persistence
  TEST media_cache_store)

oscillate_add_test_executable(oscillate_source_rate_duration_tests SOURCES source_rate_duration_tests.cpp
  LIBS oscillate::media
  DEFINITIONS OSCILLATE_HAVE_SOURCE_RESAMPLER=$<BOOL:${OSCILLATE_BUILD_SOURCE_RESAMPLER}>
  TEST media_source_rate_duration)

if(OSCILLATE_BUILD_SOURCE_RESAMPLER)
  oscillate_add_test_executable(oscillate_source_rate_tests SOURCES source_rate_reader_tests.cpp
    LIBS oscillate::media
    TEST media_source_rate)
endif()
