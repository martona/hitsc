cmake_minimum_required(VERSION 3.24)

project(
  hitsc
  VERSION 0.1.0
  DESCRIPTION "HTTPS IPMI terminal services client"
  LANGUAGES C CXX
)

find_package(Boost REQUIRED CONFIG COMPONENTS json system)
find_package(CLI11 CONFIG REQUIRED)
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
  src/aspeed_decoder.cpp
  src/cli.cpp
  src/console.cpp
  src/cookie_jar.cpp
  src/http_client.cpp
  src/kvm_capture.cpp
  src/kvm_capture_decode.cpp
  src/kvm_probe.cpp
  src/kvm_video.cpp
  src/kvm_view.cpp
  src/main.cpp
  src/megarac_session.cpp
  src/text.cpp
  src/tls.cpp
  src/trace.cpp
  src/url.cpp
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

if(MSVC)
  target_compile_options(hitsc PRIVATE /W4 /permissive-)
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

target_link_libraries(
  hitsc
  PRIVATE
    aspeed_codec
    ${HITSC_BOOST_HEADERS_TARGET}
    Boost::json
    Boost::system
    CLI11::CLI11
    OpenSSL::SSL
    OpenSSL::Crypto
    SDL3::SDL3
    ZLIB::ZLIB
)
