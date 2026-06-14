#pragma once

#include <optional>
#include <cstdint>

namespace triengine_interop::surface
{
    struct surface_render_options
    {
        bool flip_y = true;                // OpenGL (bottom-left origin) -> DX (top-left)
        bool convert_rgba_to_bgra = false; // swap R/B channels in the blit shader

        // Frame-rate cap requested of the renderer (a production cap, NOT a vsync).
        // `std::nullopt` (default) derives the cap adaptively from the local displays;
        // an explicit value is sent as-is (0 == let the renderer pick its fallback).
        std::optional<uint32_t> max_fps = std::nullopt;
    };

} // namespace triengine_interop::surface
