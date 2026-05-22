set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)

if(PORT STREQUAL "qtbase")
    list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS
        "-DFEATURE_opengl:BOOL=OFF"
        "-DFEATURE_timezone:BOOL=OFF"
    )
endif()

if(PORT STREQUAL "qtdeclarative")
    list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS
        "-DFEATURE_qml_debug:BOOL=OFF"
    )
endif()
