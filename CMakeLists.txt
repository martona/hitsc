cmake_minimum_required(VERSION 3.24)

project(
  hitsc
  VERSION 0.1.0
  DESCRIPTION "HTTPS IPMI terminal services client"
  LANGUAGES C CXX
)

set(HITSC_STATIC_VCPKG_TRIPLET OFF)
if(DEFINED VCPKG_TARGET_TRIPLET AND VCPKG_TARGET_TRIPLET MATCHES "-static($|-)")
  set(HITSC_STATIC_VCPKG_TRIPLET ON)
endif()

if(MSVC AND HITSC_STATIC_VCPKG_TRIPLET)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  set(OPENSSL_MSVC_STATIC_RT TRUE)
endif()

find_package(Boost REQUIRED CONFIG COMPONENTS json log log_setup system)
find_package(CLI11 CONFIG REQUIRED)
find_package(FFMPEG REQUIRED)
find_package(OpenSSL REQUIRED)
set(QT_SKIP_SETUP_DEPLOYMENT ON)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 QuickLayouts)
find_package(SDL3 CONFIG REQUIRED)
find_package(ZLIB REQUIRED)

qt_policy(SET QTP0001 NEW)
qt_policy(SET QTP0004 NEW)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

add_library(
  aspeed_codec
  STATIC
  third_party/aspeed_codec/src/decoder.c
)

target_include_directories(
  aspeed_codec
  PRIVATE
    src/aspeed_compat
    third_party/aspeed_codec/src
)

if(MSVC)
  target_compile_options(aspeed_codec PRIVATE /W0)
else()
  target_compile_options(aspeed_codec PRIVATE -w)
endif()

add_executable(
  hitsc
  src/aten_network.cpp
  src/aten_protocol.cpp
  src/aten_session.cpp
  src/aten_view.cpp
  src/aspeed_decoder.cpp
  src/aspeed_presenter.cpp
  src/bmc_session.cpp
  src/cli.cpp
  src/console.cpp
  src/cookie_jar.cpp
  src/diagnostics.cpp
  src/http_client.cpp
  src/launcher_child_protocol.cpp
  src/launcher_gui.cpp
  src/launcher_host_model.cpp
  src/launcher_host_store.cpp
  src/launcher_reachability_probe.cpp
  src/launcher_theme.cpp
  src/launcher_theme.hpp
  src/launcher_types.cpp
  src/log.cpp
  src/megarac_view.cpp
  src/main.cpp
  src/hardware_cursor.cpp
  src/hardware_cursor_presenter.cpp
  src/megarac_hid.cpp
  src/megarac_view_session.cpp
  src/megarac_protocol.cpp
  src/megarac_video.cpp
  src/megarac_session.cpp
  src/pikvm_events.cpp
  src/pikvm_input.cpp
  src/pikvm_session.cpp
  src/pikvm_video.cpp
  src/pikvm_view.cpp
  src/text.cpp
  src/tls.cpp
  src/tls_session_cache.cpp
  src/trace.cpp
  src/url.cpp
  src/view_status.cpp
)

target_compile_features(hitsc PRIVATE cxx_std_20)

add_subdirectory(src/gui)

target_compile_definitions(
  hitsc
  PRIVATE
    BOOST_ASIO_NO_DEPRECATED
    BOOST_BEAST_USE_STD_STRING_VIEW
    NOMINMAX
    SDL_MAIN_HANDLED
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
    _WIN32_WINNT=0x0A00
)

if(NOT HITSC_STATIC_VCPKG_TRIPLET)
  target_compile_definitions(hitsc PRIVATE BOOST_LOG_DYN_LINK)
endif()

if(MSVC)
  target_compile_options(hitsc PRIVATE /W4 /permissive- /bigobj)
  target_compile_options(hitsc PRIVATE "$<$<CONFIG:Release>:/Zi>")
  target_link_options(
    hitsc
    PRIVATE
      "$<$<CONFIG:Release>:/DEBUG>"
      "$<$<CONFIG:Release>:/MAP>"
      "$<$<CONFIG:Release>:/OPT:REF>"
      "$<$<CONFIG:Release>:/OPT:ICF>"
  )
else()
  target_compile_options(hitsc PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(TARGET Boost::headers)
  set(HITSC_BOOST_HEADERS_TARGET Boost::headers)
elseif(TARGET Boost::boost)
  set(HITSC_BOOST_HEADERS_TARGET Boost::boost)
else()
  message(FATAL_ERROR "Boost headers target was not provided by find_package(Boost).")
endif()

target_include_directories(hitsc PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(hitsc PRIVATE ${FFMPEG_LIBRARY_DIRS})

target_link_libraries(
  hitsc
  PRIVATE
    aspeed_codec
    ${HITSC_BOOST_HEADERS_TARGET}
    Boost::json
    Boost::log
    Boost::log_setup
    Boost::system
    CLI11::CLI11
    ${FFMPEG_LIBRARIES}
    OpenSSL::SSL
    OpenSSL::Crypto
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::QuickLayouts
    SDL3::SDL3
    hitsc_launcher_qml
    ZLIB::ZLIB
)

if(WIN32)
  target_link_libraries(hitsc PRIVATE Crypt32 D3d11 Dbghelp Dwmapi Dxgi Iphlpapi Ws2_32)
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(
    hitsc_tests
    src/launcher_host_store.cpp
    src/launcher_types.cpp
    src/url.cpp
    tests/launcher_tests.cpp
  )

  target_compile_features(hitsc_tests PRIVATE cxx_std_20)
  target_include_directories(hitsc_tests PRIVATE src)
  target_link_libraries(hitsc_tests PRIVATE Qt6::Core)

  if(WIN32)
    target_compile_definitions(
      hitsc_tests
      PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00
    )
    target_link_libraries(hitsc_tests PRIVATE Crypt32)
  endif()

  add_test(NAME hitsc_tests COMMAND hitsc_tests)
endif()
