# QWindowKit's installed config only find_dependency()s Qt (Core/Gui [+Widgets/
# Quick]) -- NOT qmsetup -- so qmsetup stays a build-time-only dependency and never
# reaches the hitsc build. qmsetup is a regular (same-triplet) dependency here: this
# project only targets x64-windows, so corecmd is host-runnable and find_package(
# qmsetup) resolves off CMAKE_PREFIX_PATH. (For a cross-compile, make it host:true.)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO stdware/qwindowkit
    REF "${VERSION}"
    SHA512 417c48789350ba6462507521e770d6093f64525b7bd39a3e96883c289cb1782393d0aabf4af26e1ebe2fe48d74444cb39f2e0d8220131a6a6fe0fd32b58e7cb8
    HEAD_REF main
)

string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "static" QWK_BUILD_STATIC)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DQWINDOWKIT_BUILD_STATIC=${QWK_BUILD_STATIC}
        -DQWINDOWKIT_BUILD_WIDGETS=ON
        -DQWINDOWKIT_BUILD_QUICK=ON
        -DQWINDOWKIT_BUILD_EXAMPLES=OFF
        -DQWINDOWKIT_BUILD_DOCUMENTATIONS=OFF
        -DQWINDOWKIT_INSTALL=ON
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(PACKAGE_NAME QWindowKit CONFIG_PATH lib/cmake/QWindowKit)

vcpkg_copy_pdbs()

# CMake consumers don't need the qmake/.pri or msbuild/.props integration files, and
# leaving them trips vcpkg's "files in wrong directory" layout check.
file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
    "${CURRENT_PACKAGES_DIR}/share/QWindowKit/qmake"
    "${CURRENT_PACKAGES_DIR}/share/QWindowKit/msbuild"
)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
