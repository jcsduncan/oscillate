oscillate_add_test_executable(oscillate_processing_warp_map_tests SOURCES warp_map_tests.cpp
  LIBS oscillate::processing
  TEST processing.warp_map)

oscillate_add_test_executable(oscillate_processing_clip_frame_plan_tests SOURCES clip_frame_plan_tests.cpp
  LIBS oscillate::processing
  TEST processing.clip_frame_plan)

oscillate_add_test_executable(oscillate_processing_warp_stream_map_tests SOURCES warp_stream_map_tests.cpp
  LIBS oscillate::warp_stream_map
  TEST processing.warp_stream_map)

oscillate_add_test_executable(oscillate_verified_source_view_tests SOURCES verified_source_view_tests.cpp
  LIBS oscillate::verified_source_view oscillate::persistence oscillate::timeline
  TEST processing.verified_source_view)

oscillate_add_test_executable(oscillate_warp_render_cache_tests SOURCES warp_render_cache_tests.cpp
  LIBS oscillate::warp_render_cache oscillate::persistence oscillate::timeline
  TEST processing.warp_render_cache)
