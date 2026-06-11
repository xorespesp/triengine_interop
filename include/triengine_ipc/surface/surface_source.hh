#pragma once
#include <Windows.h>
#include <functional>
#include <cstdint>

namespace triengine::ipc::surface
{
    // Injection point for shared_surface_provider.
    //
    // A renderer server implements these callbacks to expose its own shared surface
    // (produced by, e.g., triengine's offscreen_renderer_dx). The provider invokes
    // them from the IPC thread while serving the init/resize handshake; an
    // implementation that drives a non-thread-safe renderer MUST marshal to its
    // render thread internally (e.g. through a task dispatcher). Keeping the work
    // injected this way lets the provider stay both triengine-independent and
    // agnostic to the server's lifecycle (lazy-create vs pre-created/persistent).
    struct surface_source
    {
        // Initialize the renderer to the given frame size. Return false on failure.
        std::function<bool(int32_t width, int32_t height)> initialize;

        // LUID of the adapter that owns the shared surface.
        std::function<LUID()> adapter_luid;

        // Current shared surface NT handle (DX11 shared texture).
        std::function<HANDLE()> surface_handle;

        // Resize the renderer's frame; return the new shared surface NT handle,
        // or nullptr on failure.
        std::function<HANDLE(int32_t width, int32_t height)> resize;
    };

} // namespace triengine::ipc::surface
