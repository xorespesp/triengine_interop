#pragma once
#include <Windows.h>
#include <triengine_interop/transport/ipc_server.hh>
#include <triengine_interop/surface/proto/surface_proto.hh>

#include <memory>
#include <string_view>
#include <cstdint>

namespace triengine_interop::surface
{
    // Producer side of the shared-surface interop: owns an IPC server and serves a single
    // connected consumer (the viewer process) with the init/resize handshake plus the
    // delivery of decoded input events.
    //
    // The application implements session_interface and passes it to start(); the producer
    // validates the protocol, answers the handshake, decodes input notify packets into
    // the typed methods, and reports disconnect. It touches no graphics API (stays
    // triengine-independent) and never exposes the underlying transport session.
    class surface_producer
    {
    public:
        // The per-session handlers the application implements. All are required except
        // `on_session_notify`, which is an optional fallback (default no-op).
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
            // through the out-parameters. Throws on failure; the producer then reports
            // the failure to the consumer and rejects the connection.
            virtual void on_session_init(
                int32_t width, int32_t height,
                LUID& out_adapter_luid,
                HANDLE& out_surface_handle
            ) = 0;

            // Resize the renderer's frame, reporting the new shared surface NT handle
            // (DX11 shared texture) through the out-parameter. Throws on failure, which
            // the producer treats as a fatal session error.
            virtual void on_frame_resize_event(
                int32_t width, int32_t height,
                HANDLE& out_surface_handle
            ) = 0;

            // Decoded input events, already parsed into their fields (x, y are Win32
            // screen coordinates; yoffset matches GLFW's scroll value).
            virtual void on_mouse_button_event(
                int32_t x, int32_t y,
                proto::mouse_button_type button,
                proto::button_action_type action,
                proto::modifier_button_type mods
            ) = 0;

            virtual void on_mouse_move_event(
                int32_t x, int32_t y,
                proto::modifier_button_type mods
            ) = 0;

            virtual void on_mouse_scroll_event(float yoffset) = 0;

            // Fallback for notify packets not covered by the typed handlers above;
            // receives the raw packet id and bytes.
            virtual void on_session_notify(uint32_t /*id*/, std::string_view /*data*/) {}

            // Invoked once when the consumer disconnects.
            virtual void on_session_disconnect() = 0;
        }; // class

        surface_producer();
        ~surface_producer();

        surface_producer(const surface_producer&) = delete;
        surface_producer& operator=(const surface_producer&) = delete;

        // Start listening for the single consumer. The producer shares ownership of `iface`
        // (it is held until stop() or destruction), so the caller does not need to keep it
        // alive separately. Its methods are invoked as the consumer connects, sends input,
        // resizes, and disconnects.
        void start(std::string_view server_name, std::shared_ptr<session_interface> iface);
        void stop();

    private:
        std::shared_ptr<transport::ipc_server> _server;
        std::shared_ptr<session_interface> _iface;
    };

} // namespace triengine_interop::surface
