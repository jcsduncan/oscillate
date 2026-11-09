add_library(oscillate_application
  browser_preview.cpp
  clip_playback.cpp
  clip_warp_edit.cpp
  clip_prefetch_worker.cpp
  control_transport_gate.cpp
  crate_chops.cpp
  device_control.cpp
  graph_control.cpp
  instrument_playback.cpp
  key_cache.cpp
  knock_patterns.cpp
  midi_file_import.cpp
  midi_pedal_edit.cpp
  midi_note_tools.cpp
  # Groove from a record (ADR-0079).
  groove_capture.cpp
  midi_velocity_edit.cpp
  prefetch_control.cpp
  project_collection.cpp
  project_session.cpp
  # The services ProjectSession composes.
  session/audio_clip_service.cpp
  session/audio_clip_service_conversion.cpp
  session/automation_service.cpp
  session/clipboard_service.cpp
  session/edit_support.cpp
  session/effect_service.cpp
  session/export_service.cpp
  session/freeze_service.cpp
  session/history_service.cpp
  session/import_service.cpp
  session/instrument_service.cpp
  session/media_repair_service.cpp
  session/midi_clip_service.cpp
  session/note_service.cpp
  session/parameter_automation_service.cpp
  session/pedal_service.cpp
  session/routing_service.cpp
  session/save_service.cpp
  session/session_service.cpp
  session/song_service.cpp
  session/source_analysis_service.cpp
  session/track_device_service.cpp
  session/track_service.cpp
  session/warp_service.cpp
  clay_edit.cpp
  # Arrange from a loop (ADR-0080): the rule engine.
  arrange_from_loop.cpp
  mix_check.cpp
  mix_check_history.cpp
  # The Constellation (ADR-0077): presets placed by sound, morphed and mutated.
  sound_map.cpp
  nova_morph.cpp
  # The Nova arp's pattern library, mutate and groove (ADR-0078).
  nova_arp_patterns.cpp
  # Comp's patterns (ADR-0083).
  comp_patterns.cpp
  midi_recording.cpp
  history_labels.cpp
  revision_status.cpp
  routed_audio_source.cpp
  routed_playback_control.cpp
  routed_source_selection.cpp
  source_analysis_cache_directory.cpp
  spectrum_monitor.cpp
  spectrum_tap.cpp
  tempo_cache.cpp
  waveform_cache.cpp
  wav_audio_import.cpp
)
add_library(oscillate::application ALIAS oscillate_application)
oscillate_add_library(cache_control SOURCES cache_control.cpp
  PUBLIC oscillate::media oscillate::persistence
  PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(oscillate_application PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)
target_link_libraries(oscillate_application PUBLIC
  oscillate::audio_io oscillate::automation_project oscillate::engine oscillate::media oscillate::native_dsp
  oscillate::project oscillate::timeline oscillate::persistence oscillate::export oscillate::routing
  oscillate::analysis oscillate::processing oscillate::warp_render_cache oscillate::synth_engines
  oscillate::native_inserts oscillate::track_chain oscillate::midi_file
  PRIVATE oscillate::source_prep nlohmann_json::nlohmann_json
)
target_compile_definitions(oscillate_application PRIVATE
  OSCILLATE_HAVE_CODEC=$<BOOL:${OSCILLATE_BUILD_CODEC_ADAPTER}>
  OSCILLATE_HAVE_CONVOLUTION=$<BOOL:${OSCILLATE_BUILD_CONVOLUTION_DSP}>
  OSCILLATE_HAVE_VST3=$<BOOL:${OSCILLATE_BUILD_VST3_SCANNER}>)
if(OSCILLATE_BUILD_CODEC_ADAPTER)
  # Mix Check and Groove from a record decode a reference in any supported format.
  target_link_libraries(oscillate_application PRIVATE oscillate::codec)
endif()
if(OSCILLATE_BUILD_CONVOLUTION_DSP)
  target_link_libraries(oscillate_application PRIVATE oscillate::convolution_ir)
endif()
if(OSCILLATE_BUILD_VST3_SCANNER)
  target_link_libraries(oscillate_application PRIVATE oscillate::vst3_graph_runtime)
endif()
oscillate_target_defaults(oscillate_application)
if(OSCILLATE_BUILD_VST3_SCANNER)
  add_subdirectory(vst3)
endif()
