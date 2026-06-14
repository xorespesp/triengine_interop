#pragma once

#include <optional>
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
        //   nullopt (default) : derive the cap adaptively from the local displays.
        //   MAX_FPS_UNCAPPED  : uncapped (no limit).
        //   N (> 0)           : cap at N fps, sent as-is.
        std::optional<uint32_t> max_fps = std::nullopt;
    };

} // namespace triengine_interop::surface
