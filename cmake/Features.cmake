# Every optional feature and third-party dependency, declared and looked up
# once. Components test OSCILLATE_BUILD_* (or JACK_FOUND) and link the
# imported targets found here; none calls find_package itself.
#
# Locally built dependencies are found the usual way: CMAKE_PREFIX_PATH or
# <Package>_DIR for CMake packages, PKG_CONFIG_PATH and PKG_CONFIG_SYSROOT_DIR
# for pkg-config ones (docs/dependencies/local-development.md).

option(OSCILLATE_BUILD_QUICK_UI "Build the Qt Quick DAW application" OFF)
option(OSCILLATE_BUILD_VST3_SCANNER "Build the pinned-SDK VST3 host, scanner helper and contract tests" OFF)
set(OSCILLATE_VST3_SDK_ROOT "" CACHE PATH "Path to pinned VST3 SDK v3.8.1_build_84 checkout")
option(OSCILLATE_BUILD_CODEC_ADAPTER "Build the worker-only libsndfile codec adapter" OFF)
option(OSCILLATE_BUILD_EXPORT_RESAMPLER "Enable high-quality offline export rate conversion (libsamplerate)" OFF)
option(OSCILLATE_BUILD_SOURCE_RESAMPLER "Build worker-only source-rate conversion (libsamplerate)" OFF)
option(OSCILLATE_CONVOLUTION_SIMD "Enable PFFFT x86 SIMD" ON)
option(OSCILLATE_REQUIRE_JACK "Fail configuration when the JACK backend is unavailable" OFF)
option(OSCILLATE_BUILD_FUZZ "Build opt-in malformed-input parser fuzz targets" OFF)
option(OSCILLATE_BUILD_BENCHES "Build manual benchmarks (bench/) and listening tools (tools/listening/)" OFF)
option(OSCILLATE_AUDIO_IO_LIVE_TESTS "Run local PipeWire (and JACK) server qualification smoke tests" OFF)
option(OSCILLATE_BUILD_QUALIFICATION "Add the opt-in qualification programs (tests/qualification), excluded from all" OFF)

# Always required.
find_package(Threads REQUIRED)
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
find_package(nlohmann_json 3.11.3 REQUIRED CONFIG)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET GLOBAL libpipewire-0.3)
# Direct ALSA output and its card reservation on the session bus (ADR-0130).
pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET GLOBAL alsa)
pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET GLOBAL dbus-1)

# JACK is optional unless OSCILLATE_REQUIRE_JACK; JACK_FOUND selects the
# JACK output and real-time export adapters.
pkg_check_modules(JACK QUIET IMPORTED_TARGET GLOBAL jack)
if(NOT JACK_FOUND)
  if(OSCILLATE_REQUIRE_JACK)
    message(FATAL_ERROR "OSCILLATE_REQUIRE_JACK is set but the JACK development package (jack.pc) was not found")
  endif()
  message(STATUS "JACK development package not found; JACK adapter is unavailable")
endif()

if(OSCILLATE_BUILD_CODEC_ADAPTER)
  find_path(OSCILLATE_SNDFILE_INCLUDE_DIR NAMES sndfile.h REQUIRED)
  find_library(OSCILLATE_SNDFILE_LIBRARY NAMES sndfile REQUIRED)
  # MP3 decoding scans the stream with libmpg123 1.33 or later.
  find_path(OSCILLATE_MPG123_INCLUDE_DIR NAMES mpg123.h REQUIRED)
  find_library(OSCILLATE_MPG123_LIBRARY NAMES mpg123 REQUIRED)
  # MP3 import needs libsndfile 1.1 or later built with MPEG support: the
  # library found must list MPEG among its major formats. Both checks rerun on
  # each configure, so a corrected include directory or library is seen.
  include(CheckCXXSourceCompiles)
  include(CheckCXXSourceRuns)
  set(CMAKE_REQUIRED_INCLUDES "${OSCILLATE_SNDFILE_INCLUDE_DIR}")
  set(CMAKE_REQUIRED_LIBRARIES "${OSCILLATE_SNDFILE_LIBRARY}")
  set(oscillate_sndfile_mpeg_probe "#include <sndfile.h>
int main() {
  int count = 0;
  sf_command(nullptr, SFC_GET_FORMAT_MAJOR_COUNT, &count, sizeof(count));
  for (int index = 0; index < count; ++index) {
    SF_FORMAT_INFO info{};
    info.format = index;
    sf_command(nullptr, SFC_GET_FORMAT_MAJOR, &info, sizeof(info));
    if ((info.format & SF_FORMAT_TYPEMASK) == SF_FORMAT_MPEG) {
      return 0;
    }
  }
  return 1;
}")
  unset(OSCILLATE_SNDFILE_HAS_MPEG CACHE)
  if(CMAKE_CROSSCOMPILING)
    check_cxx_source_compiles("${oscillate_sndfile_mpeg_probe}" OSCILLATE_SNDFILE_HAS_MPEG)
  else()
    check_cxx_source_runs("${oscillate_sndfile_mpeg_probe}" OSCILLATE_SNDFILE_HAS_MPEG)
  endif()
  unset(CMAKE_REQUIRED_INCLUDES)
  unset(CMAKE_REQUIRED_LIBRARIES)
  if(NOT OSCILLATE_SNDFILE_HAS_MPEG)
    message(FATAL_ERROR "The codec adapter needs libsndfile 1.1 or later with MPEG support; "
                        "checked ${OSCILLATE_SNDFILE_INCLUDE_DIR} and ${OSCILLATE_SNDFILE_LIBRARY}")
  endif()
endif()

if(OSCILLATE_BUILD_EXPORT_RESAMPLER OR OSCILLATE_BUILD_SOURCE_RESAMPLER)
  find_path(OSCILLATE_SAMPLERATE_INCLUDE_DIR NAMES samplerate.h REQUIRED)
  find_library(OSCILLATE_SAMPLERATE_LIBRARY NAMES samplerate REQUIRED)
endif()

if(OSCILLATE_BUILD_QUICK_UI)
  set(oscillate_qt_components Core Gui Qml Quick Widgets)
  if(BUILD_TESTING)
    list(APPEND oscillate_qt_components Test)
  endif()
  find_package(Qt6 6.10 REQUIRED COMPONENTS ${oscillate_qt_components})
endif()

if(OSCILLATE_BUILD_VST3_SCANNER)
  if(NOT OSCILLATE_VST3_SDK_ROOT)
    message(FATAL_ERROR "Set OSCILLATE_VST3_SDK_ROOT to the pinned VST3 SDK checkout")
  endif()
  # The editor attachment embeds plug-in views in X11 windows.
  if(OSCILLATE_BUILD_QUICK_UI)
    find_package(X11 REQUIRED)
  endif()
endif()

if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
endif()

# The libFuzzer build instruments the parsers and all project libraries, not
# just the fuzzer entry points. Only Clang has libFuzzer; with another
# compiler the corpus replay tests still build and run.
if(OSCILLATE_BUILD_FUZZ AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  if(OSCILLATE_ENABLE_COVERAGE OR OSCILLATE_ENABLE_SANITIZERS OR
     OSCILLATE_ENABLE_THREAD_SANITIZER)
    message(FATAL_ERROR "The libFuzzer build needs its own instrumentation build tree")
  endif()
  set(oscillate_fuzz_flags -fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer)
  add_compile_options(
    "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:${oscillate_fuzz_flags}>"
    "$<$<COMPILE_LANG_AND_ID:C,Clang>:${oscillate_fuzz_flags}>")
  add_link_options(-fsanitize=address,undefined)
endif()
