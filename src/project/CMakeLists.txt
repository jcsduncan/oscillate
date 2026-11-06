add_library(oscillate_project
  model.cpp
  model_apply.cpp
  model_apply_automation.cpp
  model_apply_clips.cpp
  model_apply_effects.cpp
  model_apply_instruments.cpp
  model_apply_midi.cpp
  model_apply_presentation.cpp
  model_apply_routing.cpp
  model_apply_sources.cpp
  model_apply_timing.cpp
  model_apply_tracks.cpp
  model_history.cpp
  model_support.cpp
  model_validate.cpp
  clip_fade.cpp
  frozen_view.cpp
)
add_library(oscillate::project ALIAS oscillate_project)
target_include_directories(oscillate_project PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/../../include>
)
target_link_libraries(oscillate_project PUBLIC oscillate::foundation oscillate::timeline)
oscillate_target_defaults(oscillate_project)
