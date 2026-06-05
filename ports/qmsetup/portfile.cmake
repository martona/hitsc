# qmsetup is a build-time helper only (CMake modules + the `corecmd` tool) used by
# qwindowkit; nothing from it is linked into hitsc at runtime. Its syscmdline
# dependency is a git submodule with no release tags, so we vendor it pinned to the
# exact commit qmsetup 1.0.0.0 references (qmsetup's own build does the same via
# `add_subdirectory(syscmdline)` when find_package(syscmdline) fails).

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO stdware/qmsetup
    REF "${VERSION}"
    SHA512 76fa4caab4f733b89d82db16436899c2c42f8ba18269acd11952dbd0b3abe0ac6dec1cec8c4550dcfa609ce30614ca01e74ba12d380d9301a319b60772542f24
    HEAD_REF main
)

vcpkg_from_github(
    OUT_SOURCE_PATH SYSCMDLINE_SOURCE_PATH
    REPO SineStriker/syscmdline
    REF 0c9f3de8b11bd2f33b03bea5521bf446af4ead69
    SHA512 0268dd94bad848e4d05957327d3149c13e2459fb8e82a7e8577ec464108bf69f8d7719df847194c1b6efc8edacd8353c26444e978b40e114b4e4e0ab06b5c0bf
    HEAD_REF main
)
file(COPY "${SYSCMDLINE_SOURCE_PATH}/" DESTINATION "${SOURCE_PATH}/src/syscmdline")

# Header-only interface library + a host build tool: a release-only build is enough.
set(VCPKG_BUILD_TYPE release)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        # Install corecmd to tools/qmsetup (vcpkg layout) instead of bin.
        -DQMSETUP_VCPKG_TOOLS_HINT=ON
        # Let vcpkg's toolchain pick the CRT (CMAKE_MSVC_RUNTIME_LIBRARY) rather
        # than qmsetup forcing /MT, so it matches the triplet on both linkages.
        -DQMSETUP_STATIC_RUNTIME=OFF
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(PACKAGE_NAME qmsetup CONFIG_PATH lib/cmake/qmsetup)

# Bundle both licenses since syscmdline is vendored into the build.
vcpkg_install_copyright(
    FILE_LIST
        "${SOURCE_PATH}/LICENSE"
        "${SOURCE_PATH}/src/syscmdline/LICENSE"
)
