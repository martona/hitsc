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
find_package(SDL3 CONFIG REQUIRED)
find_package(ZLIB REQUIRED)

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
  src/aten_session.cpp
  src/aten_view.cpp
  src/aspeed_decoder.cpp
  src/bmc_session.cpp
  src/cli.cpp
  src/console.cpp
  src/cookie_jar.cpp
  src/diagnostics.cpp
  src/http_client.cpp
  src/log.cpp
  src/megarac_view.cpp
  src/main.cpp
  src/megarac_cursor.cpp
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
  src/trace.cpp
  src/url.cpp
  src/view_status.cpp
)

target_compile_features(hitsc PRIVATE cxx_std_20)

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
      "$<$<CONFIG:Release>:/DEBUG:FULL>"
      "$<$<CONFIG:Release>:/MAP>"
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
    SDL3::SDL3
    ZLIB::ZLIB
)

if(WIN32)
  target_link_libraries(hitsc PRIVATE D3d11 Dbghelp Dxgi)
endif()
