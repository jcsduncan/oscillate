# Developer programs: demo and reference renders, A/B and audition tools.
# Built on request only (EXCLUDE_FROM_ALL); each lands in
# <build>/tools/<directory>/, as its README says.
function(oscillate_add_tool target directory)
  oscillate_add_executable(${target} ${ARGN})
  set_target_properties(${target} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${directory}")
endfunction()

foreach(tool IN ITEMS demo_song lock_ab minimal_demo mix_check nova_audition)
  oscillate_add_tool(oscillate_${tool} ${tool} EXCLUDE_FROM_ALL SOURCES ${tool}/main.cpp
    LIBS oscillate::application oscillate::persistence oscillate::synth_engines
         oscillate::native_effects oscillate::export)
endforeach()
oscillate_add_tool(oscillate_anvil_loops anvil_reference EXCLUDE_FROM_ALL
  SOURCES anvil_reference/render_loops.cpp LIBS oscillate::synth_engines)

# Listening tools: render instruments and warp modes to WAV files for review
# by ear. Not tests and not part of the DAW; configure with
# -DOSCILLATE_BUILD_BENCHES=ON.
if(OSCILLATE_BUILD_BENCHES)
  foreach(instrument IN ITEMS concert ember fret wren)
    # The synth test helpers render notes the same way the contract tests do.
    oscillate_add_tool(oscillate_${instrument}_audition listening
      SOURCES listening/${instrument}_audition.cpp
      LIBS oscillate::synth_engines
      INCLUDES ${OSCILLATE_SOURCE_DIR}/tests/native_dsp)
  endforeach()
  add_subdirectory(listening/warp)
endif()
