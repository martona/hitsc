qt_add_library(hitsc_launcher_qml STATIC)

qt_add_qml_module(
  hitsc_launcher_qml
  URI Hitsc.Launcher
  VERSION 1.0
  NO_PLUGIN
  OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/Hitsc/Launcher"
  QML_FILES
    LauncherWindow.qml
)

if(HITSC_STATIC_VCPKG_TRIPLET)
  qt_import_qml_plugins(hitsc_launcher_qml)
endif()
