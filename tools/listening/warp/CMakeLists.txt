# Recorded-material warp audition harness (docs/performance/warp-recorded-review.md).
oscillate_add_executable(oscillate_warp_audition SOURCES warp_audition.cpp
  LIBS oscillate::warp_dsp oscillate::media OpenSSL::Crypto)

# Warp Lab: a Qt Quick sandbox for auditioning warp modes on local audio
# (warp_lab/README.md).
if(OSCILLATE_BUILD_QUICK_UI)
  add_executable(oscillate_warp_lab
    warp_lab/main.cpp
    warp_lab/warp_lab_controller.cpp
    warp_lab/warp_lab_controller.hpp
    warp_lab/warp_lab.qrc)
  set_target_properties(oscillate_warp_lab PROPERTIES AUTOMOC ON AUTORCC ON)
  target_link_libraries(oscillate_warp_lab PRIVATE oscillate::warp_dsp oscillate::media
    Qt6::Gui Qt6::Qml Qt6::Quick)
  # The pinned local Qt is not on the default QML import path.
  get_filename_component(oscillate_warp_lab_qml_dir "${Qt6_DIR}/../../qt6/qml" ABSOLUTE)
  target_compile_definitions(oscillate_warp_lab PRIVATE
    OSCILLATE_WARP_LAB_QML_DIR="${oscillate_warp_lab_qml_dir}")
  # QML plugins are dlopen()ed; an inherited RPATH lets their Qt dependencies
  # (Controls, Dialogs, styles) resolve from the same pinned Qt.
  target_link_options(oscillate_warp_lab PRIVATE -Wl,--disable-new-dtags)
  oscillate_target_defaults(oscillate_warp_lab)
endif()
