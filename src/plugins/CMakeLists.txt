oscillate_add_library(plugins
  SOURCES vst3_scan.cpp vst3_catalogue.cpp scan_protocol.cpp vst3_binary_identity.cpp
  PUBLIC oscillate::foundation
  PRIVATE oscillate::foundation_digest)

# The pinned SDK (OSCILLATE_VST3_SDK_ROOT, Features.cmake) is verified here.
if(NOT EXISTS "${OSCILLATE_VST3_SDK_ROOT}/pluginterfaces/base/ipluginbase.h")
  message(FATAL_ERROR "VST3 SDK pluginterfaces submodule is missing")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${OSCILLATE_VST3_SDK_ROOT}/CMakeLists.txt"
  "${OSCILLATE_VST3_SDK_ROOT}/LICENSE.txt"
  "${OSCILLATE_VST3_SDK_ROOT}/pluginterfaces/LICENSE.txt")

# The host compiles against interface headers only. Pin the complete interface
# header set as well as the release manifest and both licence notices so a
# different SDK checkout cannot silently change the ABI or terms.
file(SHA256 "${OSCILLATE_VST3_SDK_ROOT}/CMakeLists.txt" _vst3_release_manifest_sha256)
if(NOT _vst3_release_manifest_sha256 STREQUAL
    "5f3d95f1037d501e5c0018e282cff2c5e4a0f35e4c7f0c20ee6cf5d3357d6497")
  message(FATAL_ERROR "VST3 SDK root does not match pinned v3.8.1_build_84")
endif()
foreach(_vst3_notice IN ITEMS LICENSE.txt pluginterfaces/LICENSE.txt)
  file(SHA256 "${OSCILLATE_VST3_SDK_ROOT}/${_vst3_notice}" _vst3_notice_sha256)
  if(NOT _vst3_notice_sha256 STREQUAL
      "e86d79e19e4a33ebc442d2e0d083887dfc75a2a8de9fe17eeddedb84810ad74e")
    message(FATAL_ERROR "VST3 SDK licence notice does not match pinned MIT release: ${_vst3_notice}")
  endif()
endforeach()
file(GLOB_RECURSE _vst3_interface_headers
  CONFIGURE_DEPENDS
  RELATIVE "${OSCILLATE_VST3_SDK_ROOT}/pluginterfaces"
  "${OSCILLATE_VST3_SDK_ROOT}/pluginterfaces/*.h")
list(SORT _vst3_interface_headers)
list(LENGTH _vst3_interface_headers _vst3_interface_header_count)
if(NOT _vst3_interface_header_count EQUAL 63)
  message(FATAL_ERROR "VST3 SDK interface header set does not match pinned release")
endif()
set(_vst3_interface_manifest "")
foreach(_vst3_header IN LISTS _vst3_interface_headers)
  set(_vst3_header_path "${OSCILLATE_VST3_SDK_ROOT}/pluginterfaces/${_vst3_header}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_vst3_header_path}")
  file(SHA256 "${_vst3_header_path}" _vst3_header_sha256)
  string(APPEND _vst3_interface_manifest "${_vst3_header}=${_vst3_header_sha256}\n")
endforeach()
string(SHA256 _vst3_interface_manifest_sha256 "${_vst3_interface_manifest}")
if(NOT _vst3_interface_manifest_sha256 STREQUAL
    "d0685aee31019ccd0c760adcd93bab75d964677be393a67701eb6f19e2484b18")
  message(FATAL_ERROR "VST3 SDK interface headers do not match pinned pluginterfaces revision")
endif()

add_subdirectory(effect_host)
add_subdirectory(bundle_loader)
add_subdirectory(project_binding)
add_subdirectory(graph_node)

oscillate_add_executable(oscillate_vst3_scan_helper
  SOURCES vst3_scan_helper.cpp scan_protocol.cpp
  LIBS oscillate::foundation dl
  INCLUDES "${OSCILLATE_VST3_SDK_ROOT}" "${PROJECT_SOURCE_DIR}/include")
