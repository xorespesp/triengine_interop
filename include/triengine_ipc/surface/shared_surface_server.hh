#pragma once
#include <Windows.h>
#include <triengine_ipc/transport/ipc_service.hh>
#include <triengine_ipc/proto/ipc_proto.hh>

#include <memory>
#include <string_view>
#include <cstdint>

namespace triengine::ipc::surface
{
    // Owns an IPC server and serves a single connected client: the shared-surface
    // init/resize handshake plus the delivery of decoded input events. Rendering is a
    // single shared scene driven by one viewer, so a server hosts exactly one session.
    //
    // The application implements session_interface and passes it to start(); the server
    // validates the protocol, answers the handshake, decodes input notify packets into
    // the typed methods, and reports disconnect. It touches no graphics API (stays
    // triengine-independent) and never exposes the underlying transport session.
    class shared_surface_server
    {
    public:
        // The per-session handlers the application implements. `on_session_init` /
        // `on_frame_resize` are required; the input and disconnect hooks are optional
        // (default no-op).
        //
        // All methods are invoked on the IPC threads. A renderer that is not
        // thread-safe must marshal the work to its render thread internally (e.g.
        // through a task dispatcher).
        class session_interface
        {
        public:
            virtual ~session_interface() = default;

            // Initialize the renderer to the given frame size, reporting the adapter
            // LUID and the shared surface NT handle (DX11 shared texture) it produced
            // through the out-parameters. Return false on failure.
            virtual bool on_session_init(
                int32_t width, int32_t height,
                LUID& out_adapter_luid,
                HANDLE& out_surface_handle
            ) = 0;

            // Resize the renderer's frame; return the new shared surface NT handle, or
            // nullptr on failure.
            virtual HANDLE on_frame_resize(int32_t width, int32_t height) = 0;

            // Decoded input events, already parsed into their fields (x, y are Win32
            // screen coordinates; yoffset matches GLFW's scroll value).
            virtual void on_mouse_button(
                int32_t /*x*/, int32_t /*y*/, 
                proto::mouse_button_type /*button*/, 
                proto::button_action_type /*action*/, 
                proto::modifier_button_type /*mods*/
            ) {}

            virtual void on_mouse_move(
                int32_t /*x*/, int32_t /*y*/, 
                proto::modifier_button_type /*mods*/
            ) {}

            virtual void on_mouse_scroll(float /*yoffset*/) {}

            // Fallback for notify packets not covered by the typed handlers above;
            // receives the raw packet id and bytes.
            virtual void on_session_notify(uint32_t /*id*/, std::string_view /*data*/) {}

            // Invoked once when the client disconnects.
            virtual void on_session_disconnect() {}
        };

        shared_surface_server();
        ~shared_surface_server();

        shared_surface_server(const shared_surface_server&) = delete;
        shared_surface_server& operator=(const shared_surface_server&) = delete;

        // Start listening for the single client. `iface` must outlive the server; its
        // methods are invoked as the client connects, sends input, resizes, and
        // disconnects.
        void start(std::string_view server_name, session_interface& iface);
        void stop();

    private:
        std::shared_ptr<ipc_server> _server;
        session_interface* _iface = nullptr;
    };

} // namespace triengine::ipc::surface
