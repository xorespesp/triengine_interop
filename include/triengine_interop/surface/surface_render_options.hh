#pragma once
#include <cstdint>

namespace triengine_interop::surface
{
    // Explicit max_fps value that disables the frame-rate cap.
    inline constexpr uint32_t MAX_FPS_UNCAPPED{ 0 };

    struct surface_render_options
    {
        bool flip_y = true;                // OpenGL (bottom-left origin) -> DX (top-left)
        bool convert_rgba_to_bgra = false; // swap R/B channels in the blit shader

        // Frame-rate cap requested of the renderer (a production cap, NOT a vsync).
        //   MAX_FPS_UNCAPPED (default) : uncapped (no limit).
        //   N (> 0)                    : cap at N fps.
        uint32_t max_fps = MAX_FPS_UNCAPPED;
    };

} // namespace triengine_interop::surface
