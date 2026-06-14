#pragma once
#include <Windows.h>
#include <triengine_interop/transport/ipc_client.hh>
#include <triengine_interop/surface/detail/shared_texture_blitter.hh>
#include <triengine_interop/surface/surface_render_options.hh>
#include <triengine_interop/surface/proto/surface_proto.hh>

#include <functional>
#include <memory>
#include <string_view>
#include <system_error>
#include <cstddef>
#include <cstdint>

namespace triengine_interop::surface
{
    // Consumer side of the shared-surface interop: consumes a renderer (producer)
    // process's shared DX11 surface. Owns the IPC connection and the surface interop:
    // it connects, performs the init handshake, opens the shared keyed-mutex texture,
    // synchronizes the latest frame into a private copy, and blits that copy onto a
    // render target the caller owns.
    //
    // The caller builds its own present target (a swap-chain back buffer, an exported
    // render texture, etc.) on get_dx11_device() and passes that target's RTV to
    // blit_to_render_target(); the consumer itself is agnostic to how the frame is
    // finally presented. This lets the same consumer serve both a swap-chain viewer and
    // a Flutter plugin that exports a render texture.
    class surface_consumer
    {
    public:
        surface_consumer();
        ~surface_consumer();

        surface_consumer(const surface_consumer&) = delete;
        surface_consumer& operator=(const surface_consumer&) = delete;

        bool is_connected() const noexcept;

        // Connect to the producer, perform the init handshake, open the shared
        // surface, and build the blit pipeline. Returns false on any failure (and
        // leaves the consumer disconnected).
        bool connect(
            std::string_view server_name,
            SIZE initial_frame_size,
            const surface_render_options& config = {}
        );

        void disconnect();

        // The device/context the surface lives on, so the caller can build its own
        // present target on the same device.
        ID3D11Device2* get_dx11_device() const noexcept;
        ID3D11DeviceContext2* get_dx11_context() const noexcept;

        // The current frame size of the shared surface ({0,0} if not connected).
        SIZE get_frame_size() const noexcept;

        // Pull the latest renderer frame into the private copy, guarded by the shared
        // surface's keyed mutex. Returns true on success or when no new frame is
        // ready; false only if the shared surface became inconsistent (the renderer
        // process is likely gone) and must be recreated.
        bool sync_latest_frame(uint32_t timeout_ms = 1);

        // Blit the private copy onto a caller-provided render target.
        void blit_to_render_target(
            ID3D11RenderTargetView* target_rtv, 
            const D3D11_VIEWPORT& viewport
        );

        // Submit the pending GPU commands on the surface's context. Call this after
        // blitting when another device will sample the target (e.g. the Flutter
        // engine reading an exported render texture), so the blit is guaranteed to
        // have been issued before that device reads it. Not needed when presenting
        // through a swap chain, since Present() flushes implicitly.
        void flush() noexcept;

        // Request a renderer resize and recreate the shared-surface side resources.
        // The caller is responsible for recreating its own present target.
        bool resize_frame(SIZE new_size);

        // Send a raw input notify packet to the renderer.
        std::errc send_notify(const void* payload, size_t size);

        // Typed input helpers built on the proto packet builders.
        std::errc send_mouse_button_event(
            POINT pos,
            proto::mouse_button_type button,
            proto::button_action_type action,
            proto::modifier_button_type mods
        );

        std::errc send_mouse_move_event(
            POINT pos,
            proto::modifier_button_type mods
        );

        std::errc send_mouse_scroll_event(float yoffset);

        // Notified when the renderer process disconnects. May be set before connect();
        // it is applied to the connection as soon as it is established.
        void set_disconnect_callback(std::function<void()> cb);

    private:
        std::shared_ptr<transport::ipc_client> _client;
        detail::shared_texture_blitter _blitter;
        std::function<void()> _on_disconnect;
    };

} // namespace triengine_interop::surface
