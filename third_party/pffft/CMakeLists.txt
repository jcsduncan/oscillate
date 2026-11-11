# PFFFT v1.1.0 (commit 1fdcb508e5082052ab406d560b7c4622b8b4b0dd), the subset
# of files this project compiles, with patches/allocation-failure.patch
# applied. See README.md and docs/dependencies/pffft-convolution.md.
# OSCILLATE_CONVOLUTION_SIMD is declared in cmake/Features.cmake.
enable_language(C)

add_library(oscillate_pffft STATIC src/pffft.c src/pffft_common.c)
set_target_properties(oscillate_pffft PROPERTIES
  C_STANDARD 99 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_include_directories(oscillate_pffft SYSTEM PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>)
target_include_directories(oscillate_pffft PRIVATE src include/pffft)
if(NOT OSCILLATE_CONVOLUTION_SIMD)
  target_compile_definitions(oscillate_pffft PRIVATE PFFFT_SIMD_DISABLE)
endif()
target_link_libraries(oscillate_pffft PUBLIC m)
# The realtime FFT on the audio thread (ADR-0126): optimised in Debug too,
# like the DSP that calls it.
oscillate_optimize_in_debug(oscillate_pffft)
