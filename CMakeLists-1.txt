cmake_minimum_required(VERSION 3.22)

project(packet_pause LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(PRELOADER_ANDROID_ROOT "" CACHE PATH "Path to LiteLDev/preloader-android")

if(NOT EXISTS "${PRELOADER_ANDROID_ROOT}/include/pl/Config.hpp")
    message(FATAL_ERROR
        "PRELOADER_ANDROID_ROOT must point to a preloader-android checkout. "
        "Missing: ${PRELOADER_ANDROID_ROOT}/include/pl/Config.hpp")
endif()

include(FetchContent)

set(PP_DEPS)
if(NOT TARGET nlohmann_json::nlohmann_json)
    FetchContent_Declare(nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3)
    list(APPEND PP_DEPS nlohmann_json)
endif()

if(NOT TARGET Boost::pfr)
    FetchContent_Declare(boost_pfr
        GIT_REPOSITORY https://github.com/boostorg/pfr.git
        GIT_TAG boost-1.88.0)
    list(APPEND PP_DEPS boost_pfr)
endif()

if(NOT TARGET magic_enum::magic_enum)
    FetchContent_Declare(magic_enum
        GIT_REPOSITORY https://github.com/Neargye/magic_enum.git
        GIT_TAG v0.9.7)
    list(APPEND PP_DEPS magic_enum)
endif()

if(NOT TARGET fmt::fmt)
    FetchContent_Declare(fmt
        GIT_REPOSITORY https://github.com/fmtlib/fmt.git
        GIT_TAG 11.2.0)
    set(FMT_DOC OFF CACHE BOOL "" FORCE)
    set(FMT_TEST OFF CACHE BOOL "" FORCE)
    set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
    list(APPEND PP_DEPS fmt)
endif()

if(PP_DEPS)
    FetchContent_MakeAvailable(${PP_DEPS})
endif()

if(NOT ANDROID)
    message(FATAL_ERROR "Este projeto so compila para Android (use o NDK).")
endif()

if(NOT TARGET preloader)
    add_subdirectory("${PRELOADER_ANDROID_ROOT}"
                     "${CMAKE_BINARY_DIR}/_preloader_android")
endif()

if(ANDROID_ABI)
    set(PP_OUTPUT_ABI "${ANDROID_ABI}")
else()
    set(PP_OUTPUT_ABI "android")
endif()
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out/${PP_OUTPUT_ABI}")

find_library(PP_LOG_LIB log)

add_library(packet_pause SHARED src/PacketPause.cpp)
set_target_properties(packet_pause PROPERTIES OUTPUT_NAME "packet_pause")

target_include_directories(packet_pause PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/src"
    "${PRELOADER_ANDROID_ROOT}/include")

target_link_libraries(packet_pause PRIVATE
    nlohmann_json::nlohmann_json
    Boost::pfr
    magic_enum::magic_enum
    fmt::fmt
    preloader)

if(PP_LOG_LIB)
    target_link_libraries(packet_pause PRIVATE "${PP_LOG_LIB}")
endif()

target_compile_options(packet_pause PRIVATE
    -fvisibility=hidden -fvisibility-inlines-hidden)
