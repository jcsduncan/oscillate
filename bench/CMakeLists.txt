# Manual, uninstrumented timing programs. Run them in a Release build; timing
# is never a CTest gate. Configure with -DOSCILLATE_BUILD_BENCHES=ON.
add_subdirectory(automation)
add_subdirectory(engine)
add_subdirectory(media)
add_subdirectory(native_dsp)
add_subdirectory(warp_dsp)
