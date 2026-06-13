#pragma once

namespace triengine::ipc::surface
{
    struct surface_render_options
    {
        bool flip_y = true;                // OpenGL (bottom-left origin) -> DX (top-left)
        bool convert_rgba_to_bgra = false; // swap R/B channels in the blit shader
    };

} // namespace triengine::ipc::surface
