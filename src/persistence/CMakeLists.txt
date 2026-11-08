oscillate_add_library(persistence SOURCES store.cpp codec.cpp groove_file.cpp project_creation.cpp
  PUBLIC oscillate::project
  PRIVATE OpenSSL::Crypto oscillate::foundation_digest oscillate::media
          nlohmann_json::nlohmann_json
  DEFINITIONS OSCILLATE_HAVE_CODEC=$<BOOL:${OSCILLATE_BUILD_CODEC_ADAPTER}>)
if(OSCILLATE_BUILD_CODEC_ADAPTER)
  target_link_libraries(oscillate_persistence PRIVATE oscillate::codec)
endif()
