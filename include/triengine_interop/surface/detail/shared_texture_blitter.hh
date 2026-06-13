#pragma once
#include <Windows.h>
#include <d3d11_2.h> // DX11.2 API header (ID3D11Device2 / ID3D11DeviceContext2 / RTV / viewport)

#include <cstdint>
#include <memory>

#include <triengine_interop/surface/surface_render_options.hh>

namespace triengine_interop::surface::detail
{
    // The DX11 engine behind surface_consumer: opens a producer process's shared DX11
    // surface and presents it locally.
    //
    // Given the renderer process id, the target adapter LUID, and the shared surface NT
    // handle, it creates a D3D11 device on that adapter, opens the shared keyed-mutex
    // texture, synchronizes the latest frame into a private copy, and blits that copy
    // (optionally Y-flipped / channel-swapped) onto a render target the caller owns. It
    // performs no IPC of its own: the caller resolves those three inputs (via the init
    // handshake) and hands them in.
    //
    // It is present-target-agnostic: it does not own a swap chain or an exported texture.
    // The caller creates its own present target (a swap-chain back buffer, an exported
    // render texture, etc.) on `get_dx11_device()` and passes that target's RTV to
    // `blit_to_render_target()`. This is what lets the same engine serve both the ex04
    // viewer (swap chain) and the Flutter plugin (exported render texture).
    class shared_texture_blitter
    {
    public:
        shared_texture_blitter();
        ~shared_texture_blitter();

        shared_texture_blitter(const shared_texture_blitter&) = delete;
        shared_texture_blitter& operator=(const shared_texture_blitter&) = delete;

        // Exposed so the caller can build its own present target on the same device.
        ID3D11Device2* get_dx11_device() const noexcept;
        ID3D11DeviceContext2* get_dx11_context() const noexcept;

        // The current frame size, taken from the opened shared surface ({0,0} if not created).
        SIZE get_frame_size() const noexcept;

        bool is_created() const noexcept;

        // Create the D3D11 device on the given adapter, open the producer's shared
        // surface, and build the blit pipeline. The frame size is read from the opened
        // surface (no width/height is passed in). `renderer_process_id` is used to
        // duplicate the shared NT handle into this process.
        bool create(
            DWORD renderer_process_id,
            LUID target_adapter_luid,
            HANDLE surface_handle,
            const surface_render_options& config = {}
        );

        void destroy();

        // Pull the latest renderer frame into the private copy texture, guarded by
        // the shared surface's keyed mutex. Returns true on success or when no new
        // frame is ready (the previous copy is kept); false only if the shared
        // surface became inconsistent (abandoned mutex) and must be recreated.
        bool sync_latest_frame(uint32_t timeout_ms = 1);

        // Blit the private copy onto the caller-provided render target.
        void blit_to_render_target(
            ID3D11RenderTargetView* target_rtv, 
            const D3D11_VIEWPORT& viewport
        );

        // Re-open the shared surface from a new NT handle, rebuilding the sampling
        // resources on the existing device (called after the producer has resized its
        // surface). The new frame size is read from the reopened surface. The caller is
        // responsible for recreating its own present target.
        bool reallocate_frame(HANDLE new_surface_handle);

    private:
        class impl;
        std::unique_ptr<impl> _imp;
    };

} // namespace triengine_interop::surface::detail
