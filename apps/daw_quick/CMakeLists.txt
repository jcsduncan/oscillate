set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
find_package(Qt6 6.10 REQUIRED COMPONENTS Widgets)

set(oscillate_quick_core_sources
  src/background_job.hpp
  src/status_text.hpp
  src/bundled_fonts.cpp src/bundled_fonts.hpp
  src/automation_projection.cpp
  src/automation_projection.hpp
  src/app_logging.cpp
  src/app_logging.hpp
  src/crash_report_controller.cpp
  src/crash_report_controller.hpp
  src/diagnostics_controller.cpp
  src/diagnostics_controller.hpp
  src/audio_import_controller.cpp
  src/audio_import_controller.hpp
  src/insert_chain_view.cpp
  src/insert_chain_view.hpp
  src/audio_output_controller.cpp
  src/audio_output_controller.hpp
  src/jack_playback_output.cpp
  src/jack_playback_output.hpp
  src/arrangement_automation_controller.cpp
  src/arrangement_automation_controller.hpp
  src/clip_tempo_controller.cpp
  src/clip_tempo_controller.hpp
  src/cache_ui_controller.cpp
  src/cache_ui_controller.hpp
  src/clip_automation_controller.cpp
  src/clip_warp_controller.cpp
  src/clip_warp_controller.hpp
  src/loop_transport.cpp
  src/loop_transport.hpp
  src/performance_trace.cpp
  src/performance_trace.hpp
  src/transport_controller.cpp
  src/transport_controller.hpp
  src/track_routing_view.cpp
  src/track_routing_view.hpp
  src/clip_automation_controller.hpp
  src/plugin_automation_controller.cpp
  src/plugin_automation_controller.hpp
  src/track_commands.cpp
  src/track_commands.hpp
  src/tempo_lane_controller.cpp
  src/tempo_lane_controller.hpp
  src/bar_ticks.hpp
  src/workspace_layout_controller.cpp
  src/workspace_layout_controller.hpp
  src/route_commands.cpp
  src/route_commands.hpp
  src/key_review_controller.cpp
  src/key_review_controller.hpp
  src/managed_media_controller.cpp
  src/managed_media_controller.hpp
  src/project_collection_controller.cpp
  src/project_collection_controller.hpp
  src/project_lifecycle_controller.cpp
  src/project_lifecycle_controller.hpp
  src/source_analysis_worker.hpp
  src/metered_output.cpp
  src/metered_output.hpp
  src/true_peak_monitor.cpp
  src/true_peak_monitor.hpp
  src/project_bridge.cpp
  src/project_bridge_composition.cpp
  src/project_bridge_edits.cpp
  src/project_bridge_meters.cpp
  src/project_bridge_project.cpp
  src/project_bridge_autosave.cpp
  src/project_bridge_projection.cpp
  src/project_bridge_projection.hpp
  src/project_summary_view.cpp
  src/project_summary_view.hpp
  src/import_controller.cpp
  src/import_controller.hpp
  src/project_bridge_clips.cpp
  src/project_bridge_selection.cpp
  src/project_bridge_song.cpp
  src/project_bridge_inserts.cpp
  src/project_bridge_native_synth.cpp
  src/project_bridge_monitor.cpp
  src/project_bridge_recording.cpp
  src/project_bridge_native_effect.cpp
  src/project_bridge_macros.cpp
  src/device_strip_catalog.cpp
  src/device_strip_catalog.hpp
  src/device_catalog.cpp
  src/device_catalog.hpp
  src/project_bridge_browser.cpp
  src/browser_preview_controller.cpp
  src/browser_preview_controller.hpp
  src/key_match_controller.cpp
  src/key_match_controller.hpp
  src/preview_pipewire_output.hpp
  src/sample_library.cpp
  src/sample_library.hpp
  src/sample_preview.cpp
  src/sample_preview.hpp
  src/native_synth_automation_controller.cpp
  src/native_synth_automation_controller.hpp
  src/native_synth_audition.cpp
  src/native_synth_audition.hpp
  src/computer_keyboard.cpp
  src/computer_keyboard.hpp
  src/export_controller.cpp
  src/export_controller.hpp
  src/vst3_browser_controller.cpp
  src/vst3_browser_controller.hpp
  src/vst3_editor_controller.cpp
  src/vst3_editor_controller.hpp
  src/vst3_editor_commit.cpp
  src/vst3_editor_commit.hpp
  src/project_bridge_transport.cpp
  src/project_bridge_playback.cpp
  src/project_bridge.hpp
  src/routed_playback.cpp
  src/routed_playback.hpp
  src/vst3_qualification_policy.cpp
  src/vst3_qualification_policy.hpp
  src/tempo_worker.cpp
  src/tempo_worker.hpp
  src/waveform_view.cpp
  src/waveform_view.hpp
  src/waveform_detail.cpp
  src/waveform_detail.hpp
  src/clip_waveform_projection.cpp
  src/clip_waveform_projection.hpp
  src/waveform_worker.cpp
  src/waveform_worker.hpp
  src/waveform_controller.cpp
  src/waveform_controller.hpp
  src/meter_model.cpp
  src/meter_model.hpp
  src/note_effects_controller.cpp
  src/note_effects_controller.hpp
  src/note_tools_controller.cpp
  src/note_tools_controller.hpp
  src/freeze_controller.cpp
  src/freeze_controller.hpp
  src/mix_check_controller.cpp
  src/mix_check_controller.hpp
  src/arrange_controller.cpp
  src/arrange_controller.hpp
  src/harmony_controller.cpp
  src/harmony_controller.hpp
  src/recording_controller.cpp
  src/recording_controller.hpp
  src/groove_capture_controller.cpp
  src/groove_capture_controller.hpp
  src/clay_controller.cpp
  src/clay_controller.hpp
  src/history_controller.cpp
  src/history_controller.hpp
  src/command_palette.cpp
  src/command_palette.hpp
  src/master_section.cpp
  src/master_section.hpp
  src/macro_controller.cpp
  src/macro_controller.hpp
  src/rack_library_controller.cpp
  src/rack_library_controller.hpp)

# These translation units do not include the feature-dependent ProjectBridge
# or RoutedPlayback layouts and do not inspect the optional feature macros.
# Compile them once for the production, test-input, and minimal Quick cores.
# The remaining sources retain their variant-specific compilation and ABI.
set(oscillate_quick_shared_sources
  src/automation_projection.cpp
  src/automation_projection.hpp
  src/arrangement_automation_controller.cpp
  src/arrangement_automation_controller.hpp
  src/clip_tempo_controller.cpp
  src/clip_tempo_controller.hpp
  src/cache_ui_controller.cpp
  src/cache_ui_controller.hpp
  src/clip_automation_controller.cpp
  src/clip_automation_controller.hpp
  src/clip_warp_controller.cpp
  src/clip_warp_controller.hpp
  src/insert_chain_view.cpp
  src/insert_chain_view.hpp
  src/device_strip_catalog.cpp
  src/device_strip_catalog.hpp
  src/device_catalog.cpp
  src/device_catalog.hpp
  src/jack_playback_output.cpp
  src/jack_playback_output.hpp
  src/key_review_controller.cpp
  src/key_review_controller.hpp
  src/managed_media_controller.cpp
  src/managed_media_controller.hpp
  src/metered_output.cpp
  src/metered_output.hpp
  src/native_synth_automation_controller.cpp
  src/native_synth_automation_controller.hpp
  src/native_synth_audition.cpp
  src/native_synth_audition.hpp
  src/computer_keyboard.cpp
  src/computer_keyboard.hpp
  src/plugin_automation_controller.cpp
  src/plugin_automation_controller.hpp
  src/project_summary_view.cpp
  src/project_summary_view.hpp
  src/project_collection_controller.cpp
  src/project_collection_controller.hpp
  src/project_lifecycle_controller.cpp
  src/project_lifecycle_controller.hpp
  src/route_commands.cpp
  src/route_commands.hpp
  src/tempo_lane_controller.cpp
  src/tempo_lane_controller.hpp
  src/bar_ticks.hpp
  src/tempo_worker.cpp
  src/tempo_worker.hpp
  src/track_commands.cpp
  src/track_commands.hpp
  src/track_routing_view.cpp
  src/track_routing_view.hpp
  src/transport_controller.cpp
  src/transport_controller.hpp
  src/true_peak_monitor.cpp
  src/true_peak_monitor.hpp
  src/vst3_editor_commit.cpp
  src/vst3_editor_commit.hpp
  src/waveform_controller.cpp
  src/waveform_controller.hpp
  src/meter_model.cpp
  src/meter_model.hpp
  src/waveform_view.cpp
  src/waveform_view.hpp
  src/waveform_detail.cpp
  src/waveform_detail.hpp
  src/waveform_worker.cpp
  src/waveform_worker.hpp
  src/clip_waveform_projection.cpp
  src/clip_waveform_projection.hpp
  src/note_effects_controller.cpp
  src/note_effects_controller.hpp
  src/note_tools_controller.cpp
  src/note_tools_controller.hpp
  src/freeze_controller.cpp
  src/freeze_controller.hpp
  src/mix_check_controller.cpp
  src/mix_check_controller.hpp
  src/arrange_controller.cpp
  src/arrange_controller.hpp
  src/harmony_controller.cpp
  src/harmony_controller.hpp
  src/recording_controller.cpp
  src/recording_controller.hpp
  src/groove_capture_controller.cpp
  src/groove_capture_controller.hpp
  src/clay_controller.cpp
  src/clay_controller.hpp
  src/history_controller.cpp
  src/history_controller.hpp
  src/command_palette.cpp
  src/command_palette.hpp
  src/master_section.cpp
  src/master_section.hpp
  src/macro_controller.cpp
  src/macro_controller.hpp
  src/rack_library_controller.cpp
  src/rack_library_controller.hpp)
list(REMOVE_ITEM oscillate_quick_core_sources ${oscillate_quick_shared_sources})

add_library(oscillate_quick_shared OBJECT ${oscillate_quick_shared_sources})
target_include_directories(oscillate_quick_shared PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(oscillate_quick_shared PRIVATE
  oscillate::application oscillate::cache_control oscillate::analysis oscillate::native_dsp
  oscillate::harmony
  Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Widgets)
oscillate_target_defaults(oscillate_quick_shared)

# The bridge library. The app and its tests share one build of it; a second,
# minimal build without the optional codec, resampler and VST3 features
# backs the feature-disabled tests.
function(oscillate_add_quick_core target codec export_resampler vst3)
  # The shell's QML is compiled into the core once, not into every program.
  # Paths resolve against this file, so the test suite can add a variant.
  set(here ${CMAKE_CURRENT_FUNCTION_LIST_DIR})
  list(TRANSFORM oscillate_quick_core_sources PREPEND "${here}/" OUTPUT_VARIABLE sources)
  set(resources ${here}/qml.qrc ${here}/fonts/fonts.qrc)
  if(BUILD_SHARED_LIBS)
    add_library(${target} ${sources} ${resources})
  else()
    # A static archive's resource initialisers have no caller, so the linker
    # drops them and the app finds no QML. Link their objects into each program.
    add_library(${target} ${sources})
    add_library(${target}_resources OBJECT ${resources})
    set_target_properties(${target}_resources PROPERTIES AUTORCC ON)
    target_link_libraries(${target}_resources PRIVATE Qt6::Core)
    target_sources(${target} INTERFACE $<TARGET_OBJECTS:${target}_resources>)
  endif()
  target_sources(${target} PRIVATE $<TARGET_OBJECTS:oscillate_quick_shared>)
  target_include_directories(${target} PUBLIC ${here}/src)
  target_link_libraries(${target} PUBLIC
    oscillate::application oscillate::cache_control oscillate::analysis oscillate::native_dsp
    oscillate::source_prep oscillate::persistence oscillate::warp_dsp oscillate::harmony
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Widgets)
  # The browser's sample preview decodes any supported format directly.
  if(codec)
    target_link_libraries(${target} PUBLIC oscillate::codec)
  endif()
  if(TARGET oscillate_jack_realtime_export)
    target_sources(${target} PRIVATE ${here}/src/realtime_export_run.cpp ${here}/src/realtime_export_run.hpp)
    target_link_libraries(${target} PUBLIC oscillate::jack_realtime_export)
    target_compile_definitions(${target} PUBLIC OSCILLATE_HAVE_JACK_EXPORT=1)
  else()
    target_compile_definitions(${target} PUBLIC OSCILLATE_HAVE_JACK_EXPORT=0)
  endif()
  target_compile_definitions(${target} PUBLIC
    OSCILLATE_HAVE_CODEC=$<BOOL:${codec}>
    OSCILLATE_HAVE_EXPORT_RESAMPLER=$<BOOL:${export_resampler}>
    OSCILLATE_HAVE_VST3=$<BOOL:${vst3}>)
  # Only the diagnostics bundle reports the version and these two switches,
  # so a version bump recompiles that one file.
  set_source_files_properties(${here}/src/diagnostics_controller.cpp
    TARGET_DIRECTORY ${target}
    PROPERTIES COMPILE_DEFINITIONS
      "OSCILLATE_VERSION=\"${PROJECT_VERSION}\";OSCILLATE_HAVE_SOURCE_RESAMPLER=$<BOOL:${OSCILLATE_BUILD_SOURCE_RESAMPLER}>")
  if(vst3)
    target_link_libraries(${target} PUBLIC
      oscillate::vst3_track_insert oscillate::vst3_instrument_provider
      oscillate::vst3_manual_effect_edit oscillate::vst3_insert_service oscillate::vst3_offline_export
      oscillate::plugin_ui)
  endif()
  oscillate_target_defaults(${target})
endfunction()

oscillate_add_quick_core(oscillate_quick_core
  ${OSCILLATE_BUILD_CODEC_ADAPTER} ${OSCILLATE_BUILD_EXPORT_RESAMPLER}
  ${OSCILLATE_BUILD_VST3_SCANNER})

add_executable(oscillate main.cpp)
target_link_libraries(oscillate PRIVATE oscillate_quick_core)
# Recorded in each session's crash-report description.
target_compile_definitions(oscillate PRIVATE OSCILLATE_VERSION="${PROJECT_VERSION}")
if(OSCILLATE_BUILD_VST3_SCANNER)
  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/oscillate_vst3_scan_helper
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:oscillate_vst3_scan_helper>
      ${CMAKE_CURRENT_BINARY_DIR}/oscillate_vst3_scan_helper
    DEPENDS oscillate_vst3_scan_helper
    VERBATIM)
  add_custom_target(oscillate_quick_vst3_helper_copy
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/oscillate_vst3_scan_helper)
  add_dependencies(oscillate oscillate_quick_vst3_helper_copy)
endif()
oscillate_target_defaults(oscillate)

# Prism device panel QML lives under qml/prism, listed in qml.qrc.
# Built-in synth device panels: an engine-generic controller and adapter
# seam, with per-engine curve helpers. QML skins live in qml/<engine>/.
add_library(oscillate_quick_native_synth
  src/native_synth_device_controller.cpp src/native_synth_device_controller.hpp
  # The device-chain strip's live view of an engine's telemetry.
  src/device_strip_view.cpp src/device_strip_view.hpp
  # Crate's panel model: chopping orchestrated over the application's crate functions.
  src/crate_controller.cpp src/crate_controller.hpp
  # Nova's Constellation (ADR-0077): presets placed by sound, morph and mutate.
  src/nova_constellation.cpp src/nova_constellation.hpp
  # Nova's arp pattern library, mutate and groove (ADR-0078).
  src/nova_arp.cpp src/nova_arp.hpp
  src/native_synth_document_model.cpp src/native_synth_document_model.hpp
  src/native_synth_panel_adapter.hpp
  src/macro_map_state.hpp
  src/native_synth_visuals.cpp src/native_synth_visuals.hpp
  src/native_effect_visuals.cpp src/native_effect_visuals.hpp
  # Engine contracts the panels and the shell draw with (Oscillate.Contracts).
  src/ui_contracts.cpp src/ui_contracts.hpp
  # The display formatter for QML (Oscillate.Format).
  src/value_format.cpp src/value_format.hpp
  # Each built-in device's panel file, design size and host inputs.
  src/device_panels.cpp src/device_panels.hpp
  # The keyed list model lives here so synth panels can use it; the Quick
  # cores get it through their link to this library.
  src/keyed_list_model.cpp src/keyed_list_model.hpp)
target_include_directories(oscillate_quick_native_synth PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(oscillate_quick_native_synth PUBLIC oscillate::synth_engines
  oscillate::native_effects oscillate::application Qt6::Core Qt6::Gui Qt6::Qml Qt6::Widgets)
oscillate_target_defaults(oscillate_quick_native_synth)

# Native effect panels (ADR-0040) reuse the device controller; each effect
# with a designed panel adds its own small model beside it.
add_library(oscillate_quick_relay_panel
  src/relay_panel_model.cpp src/relay_panel_model.hpp
  src/anvil_panel_model.cpp src/anvil_panel_model.hpp
  src/knock_panel_model.cpp src/knock_panel_model.hpp src/drum_pads.hpp
  src/knock_patterns_model.cpp src/knock_patterns_model.hpp)
target_include_directories(oscillate_quick_relay_panel PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(oscillate_quick_relay_panel PUBLIC oscillate_quick_native_synth
  oscillate::native_effects)
oscillate_target_defaults(oscillate_quick_relay_panel)

# Session-backed panel adapter: root constructs it for the selected synth track.
add_library(oscillate_quick_native_synth_session src/native_synth_session_adapter.cpp)
target_include_directories(oscillate_quick_native_synth_session PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(oscillate_quick_native_synth_session PUBLIC oscillate::application
  oscillate::synth_engines oscillate::native_effects oscillate::source_prep oscillate::analysis
  oscillate::media Qt6::Core)
oscillate_target_defaults(oscillate_quick_native_synth_session)

target_link_libraries(oscillate_quick_core PUBLIC
  oscillate_quick_native_synth oscillate_quick_native_synth_session oscillate_quick_relay_panel)

if(BUILD_TESTING)
  set(OSCILLATE_QUICK_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
  add_subdirectory(${PROJECT_SOURCE_DIR}/tests/ui/quick ${PROJECT_BINARY_DIR}/tests/ui/quick)
endif()
