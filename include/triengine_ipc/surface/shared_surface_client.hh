#pragma once
#include <triengine_ipc/transport/ipc_service.hh>
#include <triengine_ipc/surface/shared_surface_consumer.hh>
#include <triengine_ipc/proto/ipc_proto.hh>
#include <triengine_ipc/proto/input_events.hh>

#include <functional>
#include <memory>
#include <string_view>
#include <system_error>
#include <cstddef>
#include <cstdint>

namespace triengine::ipc::surface
{
    // Client-side endpoint for consuming a renderer process's shared DX11 surface.
    // Owns the IPC connection and the surface interop: it connects, performs the
    // init handshake, opens the shared keyed-mutex texture, synchronizes the latest
    // frame into a private copy, and blits that copy onto a render target the caller
    // owns.
    //
    // The caller builds its own present target (a swap-chain back buffer, an exported
    // render texture, etc.) on get_dx11_device() and passes that target's RTV to
    // blit_to_render_target(); the client itself is agnostic to how the frame is
    // finally presented. This lets the same client serve both a swap-chain viewer and
    // a Flutter plugin that exports a render texture.
    class shared_surface_client
    {
    public:
        shared_surface_client();
        ~shared_surface_client();

        shared_surface_client(const shared_surface_client&) = delete;
        shared_surface_client& operator=(const shared_surface_client&) = delete;

        bool is_connected() const noexcept;

        // Connect to the renderer server, perform the init handshake, open the shared
        // surface, and build the blit pipeline. Returns false on any failure (and
        // leaves the client disconnected).
        bool connect(
            std::string_view server_name,
            int32_t initial_width,
            int32_t initial_height,
            const consumer_config& config = {}
        );

        void disconnect();

        // The device/context the surface lives on, so the caller can build its own
        // present target on the same device.
        ID3D11Device2* get_dx11_device() const noexcept;
        ID3D11DeviceContext2* get_dx11_context() const noexcept;
        int32_t get_width() const noexcept;
        int32_t get_height() const noexcept;

        // Pull the latest renderer frame into the private copy, guarded by the shared
        // surface's keyed mutex. Returns true on success or when no new frame is
        // ready; false only if the shared surface became inconsistent (the renderer
        // process is likely gone) and must be recreated.
        bool sync_latest_frame(uint32_t timeout_ms = 1);

        // Blit the private copy onto a caller-provided render target.
        void blit_to_render_target(ID3D11RenderTargetView* target_rtv, const D3D11_VIEWPORT& viewport);

        // Submit the pending GPU commands on the surface's context. Call this after
        // blitting when another device will sample the target (e.g. the Flutter
        // engine reading an exported render texture), so the blit is guaranteed to
        // have been issued before that device reads it. Not needed when presenting
        // through a swap chain, since Present() flushes implicitly.
        void flush() noexcept;

        // Request a renderer resize and recreate the shared-surface side resources.
        // The caller is responsible for recreating its own present target.
        bool resize(int32_t new_width, int32_t new_height);

        // Send a raw input notify packet to the renderer.
        std::errc send_notify(const void* payload, size_t size);

        // Typed input helpers built on the proto packet builders.
        std::errc send_mouse_button(
            int32_t x,
            int32_t y,
            proto::mouse_button_type button,
            proto::button_action_type action,
            proto::modifier_button_type mods
        );

        std::errc send_mouse_move(
            int32_t x,
            int32_t y,
            proto::modifier_button_type mods
        );

        std::errc send_mouse_scroll(float yoffset);

        // Notified when the renderer process disconnects. May be set before connect();
        // it is applied to the connection as soon as it is established.
        void set_disconnect_callback(std::function<void()> cb);

    private:
        std::shared_ptr<ipc_client> _client;
        shared_surface_consumer _consumer;
        std::function<void()> _on_disconnect;
    };

} // namespace triengine::ipc::surface
