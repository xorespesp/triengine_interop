# triengine_ipc
#
# Standalone library providing the inter-process communication transport and the
# rendering wire-protocol used by Triengine's inter-process rendering (Windows).
# Consumed by both the Triengine side (renderer process) and clients
# (the ex04 example, the Flutter interop plugin) via FetchContent.

cmake_minimum_required(VERSION 3.28 FATAL_ERROR)
project(triengine_ipc LANGUAGES CXX)

set(TARGET_NAME triengine_ipc)

include(FetchContent)

# ----------------------------------------------------------------------------
# Dependencies
#
# Reuse targets already declared by a parent project (guarded by `if (NOT TARGET ...)`),
# otherwise declare them here so the library is buildable standalone.
# ----------------------------------------------------------------------------

if (NOT TARGET fmt)
    message(STATUS "[triengine_ipc] Fetching fmt...")
    FetchContent_Declare(
        fmt
        GIT_REPOSITORY https://github.com/fmtlib/fmt
        GIT_TAG 11.2.0
        GIT_SHALLOW TRUE
        GIT_PROGRESS TRUE
    )
    set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(fmt)
endif()

if (NOT TARGET Boost::interprocess)
    message(STATUS "[triengine_ipc] Fetching boost...")
    FetchContent_Declare(
        Boost
        URL "https://github.com/boostorg/boost/releases/download/boost-1.90.0/boost-1.90.0-cmake.7z" # downloading a zip release speeds up the download
        USES_TERMINAL_DOWNLOAD TRUE
        DOWNLOAD_NO_EXTRACT FALSE
    )
    set(BOOST_INCLUDE_LIBRARIES interprocess date_time CACHE STRING "" FORCE)
    FetchContent_MakeAvailable(Boost)
endif()

# ----------------------------------------------------------------------------
# Library target
# ----------------------------------------------------------------------------

add_library(${TARGET_NAME} STATIC)
add_library(Triengine::ipc ALIAS ${TARGET_NAME})

target_sources(${TARGET_NAME}
    PRIVATE
        "src/ipc_service.cc"
        "src/logger.hh"
        "include/triengine_ipc/transport/ipc_service.hh"
        "include/triengine_ipc/proto/ipc_proto.hh"
        "include/triengine_ipc/detail/bit.hh"
)

target_compile_features(${TARGET_NAME} PUBLIC cxx_std_17)

target_include_directories(${TARGET_NAME} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Boost (transport) and fmt (logging) are implementation details used only by
# ipc_service.cc; no public header exposes them, so they are linked PRIVATE.
# triengine_ipc therefore pulls in no external utility dependency of its own
# (in particular, no xutl), and a consumer that links `Triengine::ipc` needs
# nothing else. For a STATIC library CMake still propagates these to the
# consumer's final link automatically.
target_link_libraries(${TARGET_NAME} PRIVATE
    Boost::interprocess
    Boost::date_time
    fmt::fmt
)

# Compile the library's own sources at the highest warning level and keep them
# warning-clean. The Flutter dart plugin links this library with warnings-as-errors,
# so any leftover warning would break the Flutter build. Kept PRIVATE so the flag
# does not propagate to consumers (they apply their own warning settings).
if (MSVC)
    target_compile_options(${TARGET_NAME} PRIVATE /W4)
else()
    target_compile_options(${TARGET_NAME} PRIVATE -Wall -Wextra -Wpedantic)
endif()
