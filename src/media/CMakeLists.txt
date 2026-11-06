oscillate_add_library(media
  SOURCES prefetch.cpp audio_reader.cpp source_rate_reader.cpp cache_identity.cpp
          processing_job.cpp cache_store.cpp
  PUBLIC oscillate::foundation oscillate::project oscillate::foundation_digest
  DEFINITIONS OSCILLATE_HAVE_SOURCE_RESAMPLER=$<BOOL:${OSCILLATE_BUILD_SOURCE_RESAMPLER}>
              OSCILLATE_HAVE_CODEC=$<BOOL:${OSCILLATE_BUILD_CODEC_ADAPTER}>)
if(OSCILLATE_BUILD_SOURCE_RESAMPLER)
  target_include_directories(oscillate_media SYSTEM PRIVATE ${OSCILLATE_SAMPLERATE_INCLUDE_DIR})
  target_link_libraries(oscillate_media PRIVATE ${OSCILLATE_SAMPLERATE_LIBRARY})
endif()
if(OSCILLATE_BUILD_CODEC_ADAPTER)
  target_link_libraries(oscillate_media PRIVATE oscillate::codec)
endif()
