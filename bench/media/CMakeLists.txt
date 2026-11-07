if(OSCILLATE_BUILD_SOURCE_RESAMPLER)
  oscillate_add_executable(oscillate_source_rate_bench SOURCES source_rate_reader_bench.cpp
    LIBS oscillate::media)
endif()
