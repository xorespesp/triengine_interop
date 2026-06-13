# triengine_interop
#
# Standalone library providing the inter-process communication transport and the
# rendering wire-protocol used by Triengine's inter-process rendering (Windows).
# Consumed by both the Triengine side (renderer process) and clients
# (the ex04 example, the Flutter interop plugin) via FetchContent.

cmake_minimum_required(VERSION 3.28 FATAL_ERROR)
project(triengine_interop LANGUAGES CXX)

set(TARGET_NAME triengine_interop)

include(FetchContent)

# ----------------------------------------------------------------------------
# Dependencies
#
# Reuse targets already declared by a parent project (guarded by `if (NOT TARGET ...)`),
# otherwise declare them here so the library is buildable standalone.
# ----------------------------------------------------------------------------

if (NOT TARGET fmt)
    message(STATUS "[triengine_interop] Fetching fmt...")
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
    message(STATUS "[triengine_interop] Fetching boost...")
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
add_library(Triengine::interop ALIAS ${TARGET_NAME})

target_sources(${TARGET_NAME}
    PRIVATE
        "src/transport/ipc_session.cc"
        "src/transport/ipc_server.cc"
        "src/transport/ipc_client.cc"
        "src/utility/logger.cc"
        "src/transport/detail/ipc_detail.hh"
        "include/triengine_interop/transport/ipc_session.hh"
        "include/triengine_interop/transport/ipc_server.hh"
        "include/triengine_interop/transport/ipc_client.hh"
        "include/triengine_interop/transport/ipc_packet.hh"
        "include/triengine_interop/utility/bit.hh"
        "include/triengine_interop/utility/logger.hh"
)

target_compile_features(${TARGET_NAME} PUBLIC cxx_std_17)

target_include_directories(${TARGET_NAME} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# fmt is exposed through a public header (the logging utility), so it is linked
# PUBLIC: a consumer of `Triengine::interop` needs fmt on its include/link path. fmt is
# lightweight and consumers (e.g. the Flutter plugin) already depend on it directly,
# so this is the deliberate minimal external dependency.
#
# Boost.Interprocess (transport) stays PRIVATE: it is used only by the internal
# transport sources and no public header exposes it, so it is bundled and hidden.
# The dependency contract is therefore "link `Triengine::interop` + fmt; Boost and the
# DirectX libraries are bundled and need not be linked by the consumer".
target_link_libraries(${TARGET_NAME}
    PUBLIC
        fmt::fmt
    PRIVATE
        Boost::interprocess
        Boost::date_time
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

# ----------------------------------------------------------------------------
# Surface toolkit target (Triengine::interop_surface)
#
# A second library, layered on top of the transport/proto body, that holds the
# DX11 shared-surface interop: client-side endpoints that consume a renderer's
# shared surface (shared_surface_consumer / shared_surface_client) and a
# server-side endpoint that serves the surface handshake (shared_surface_server).
# It depends on the body PUBLIC and on the DirectX libraries PRIVATE, so the body
# itself stays free of any graphics dependency; only consumers that need surface
# sharing link this target.
# ----------------------------------------------------------------------------

set(SURFACE_TARGET_NAME triengine_interop_surface)

add_library(${SURFACE_TARGET_NAME} STATIC)
add_library(Triengine::interop_surface ALIAS ${SURFACE_TARGET_NAME})

target_sources(${SURFACE_TARGET_NAME}
    PRIVATE
        "src/surface/detail/unique_handle.hh"
        "src/surface/detail/shared_surface_consumer.cc"
        "src/surface/shared_surface_server.cc"
        "src/surface/shared_surface_client.cc"
        "include/triengine_interop/surface/detail/shared_surface_consumer.hh"
        "include/triengine_interop/surface/proto/surface_proto.hh"
        "include/triengine_interop/surface/surface_render_options.hh"
        "include/triengine_interop/surface/shared_surface_server.hh"
        "include/triengine_interop/surface/shared_surface_client.hh"
)

target_compile_features(${SURFACE_TARGET_NAME} PUBLIC cxx_std_17)

# fmt comes transitively from the body (linked PUBLIC there), so the surface target
# does not list it again. d3d11/dxgi/d3dcompiler are PRIVATE: needed by the
# consumer translation unit but never exposed through a public header.
target_link_libraries(${SURFACE_TARGET_NAME}
    PUBLIC
        ${TARGET_NAME}        # Triengine::interop (proto/transport)
    PRIVATE
        d3d11
        dxgi
        d3dcompiler
)

# Same warning policy/rationale as the body (Flutter links with warnings-as-errors).
if (MSVC)
    target_compile_options(${SURFACE_TARGET_NAME} PRIVATE /W4)
else()
    target_compile_options(${SURFACE_TARGET_NAME} PRIVATE -Wall -Wextra -Wpedantic)
endif()
