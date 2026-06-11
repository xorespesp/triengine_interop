#pragma once
#include <Windows.h>
#include <d3d11_2.h> // DX11.2 API header (ID3D11Device2 / ID3D11DeviceContext2 / RTV / viewport)

#include <cstdint>
#include <memory>

#include <triengine_ipc/transport/ipc_service.hh>

namespace triengine::ipc::surface
{
    struct consumer_config
    {
        bool flip_y = true;                // OpenGL (bottom-left origin) -> DX (top-left)
        bool convert_rgba_to_bgra = false; // swap R/B channels in the blit shader
    };

    // Client-side consumer of a renderer process's shared DX11 surface.
    //
    // Encapsulates everything a viewer client needs to display the renderer's
    // output: the init/resize handshake, opening the shared keyed-mutex texture,
    // synchronizing the latest frame into a private copy, and blitting that copy
    // (optionally Y-flipped / channel-swapped) onto a render target the caller owns.
    //
    // The consumer is present-target-agnostic: it does not own a swap chain or an
    // exported texture. The caller creates its own present target (a swap-chain
    // back buffer, an exported render texture, etc.) on `device()` and passes that
    // target's RTV to `blit()`. This is what lets the same consumer serve both the
    // ex04 viewer (swap chain) and the Flutter plugin (exported render texture).
    class shared_surface_consumer
    {
    public:
        shared_surface_consumer();
        ~shared_surface_consumer();

        shared_surface_consumer(const shared_surface_consumer&) = delete;
        shared_surface_consumer& operator=(const shared_surface_consumer&) = delete;

        bool is_created() const noexcept;

        // Perform the init handshake on a connected client, then create the D3D11
        // device, open the shared surface, and build the blit pipeline.
        bool create(
            ipc_client& cli,
            int32_t width,
            int32_t height,
            const consumer_config& config = {});

        void destroy();

        // Pull the latest renderer frame into the private copy texture, guarded by
        // the shared surface's keyed mutex. Returns true on success or when no new
        // frame is ready (the previous copy is kept); false only if the shared
        // surface became inconsistent (abandoned mutex) and must be recreated.
        bool sync_latest_frame(uint32_t timeout_ms = 1);

        // Blit the private copy onto the caller-provided render target.
        void blit(ID3D11RenderTargetView* target, const D3D11_VIEWPORT& viewport);

        // Request a renderer resize and recreate the shared-surface side resources.
        // The caller is responsible for recreating its own present target.
        bool resize(ipc_client& cli, int32_t width, int32_t height);

        // Exposed so the caller can build its own present target on the same device.
        ID3D11Device2* device() const noexcept;
        ID3D11DeviceContext2* context() const noexcept;
        int32_t width() const noexcept;
        int32_t height() const noexcept;

    private:
        class impl;
        std::unique_ptr<impl> _impl;
    };

} // namespace triengine::ipc::surface
