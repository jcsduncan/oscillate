oscillate_add_library(processing SOURCES warp_map.cpp clip_frame_plan.cpp
  PUBLIC oscillate::foundation oscillate::media oscillate::project)

oscillate_add_library(warp_stream_map SOURCES warp_stream_map.cpp
  PUBLIC oscillate::processing oscillate::warp_dsp)

oscillate_add_library(verified_source_view SOURCES verified_source_view.cpp
  PUBLIC oscillate::warp_dsp oscillate::media oscillate::foundation
  PRIVATE oscillate::persistence)

# Clay renders as processed sources (ADR-0081 phase 2).
oscillate_add_library(clay_source SOURCES clay_source.cpp
  PUBLIC oscillate::clay_dsp oscillate::media oscillate::persistence oscillate::project
         oscillate::foundation)

oscillate_add_library(warp_render_cache
  SOURCES warp_render_cache.cpp warp_live_reader.cpp offline_warp_clip.cpp
  PUBLIC oscillate::warp_stream_map oscillate::verified_source_view oscillate::media
         oscillate::warp_dsp oscillate::processing oscillate::clay_source
  PRIVATE oscillate::persistence)
