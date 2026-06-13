#include <triengine_interop/surface/surface_producer.hh>
#include <triengine_interop/surface/proto/surface_proto.hh>
#include <triengine_interop/transport/ipc_packet.hh>

#include <triengine_interop/utility/logger.hh>

#include <utility>
#include <vector>
#include <string_view>
#include <exception>
#include <cstdint>

namespace triengine_interop::surface
{
    using transport::ipc_server;
    using transport::ipc_session;
    using transport::packet_builder;
    using transport::packet_view;

    namespace
    {
        using session_interface = surface_producer::session_interface;

        void build_init_response(
            std::vector<uint8_t>& out,
            proto::packets::init_status status,
            DWORD renderer_process_id,
            LUID adapter_luid,
            HANDLE surface_handle)
        {
            packet_builder<proto::packets::init_response_t> pck{ static_cast<uint32_t>(proto::packet_type::init_response) };
            pck.body()->status = status;
            pck.body()->renderer_process_id = renderer_process_id;
            pck.body()->target_adapter_luid = adapter_luid;
            pck.body()->surface_handle = surface_handle;
            out.assign(pck.data(), pck.data() + pck.size());
        }

        // Install the init/resize handshake handler, delegating the actual surface work
        // to the session interface. `iface` must outlive the session.
        void install_surface_handshake(ipc_session& session, session_interface& iface)
        {
            session.set_request_callback(
                [&iface](
                    [[maybe_unused]] uint32_t req_pck_id,
                    std::string_view req_pck_data,
                    std::vector<uint8_t>& rep_pck_data)
                {
                    packet_view pck{ req_pck_data.data(), req_pck_data.size() };

                    switch (static_cast<proto::packet_type>(pck.type())) {
                    case proto::packet_type::init_request:
                    {
                        const auto* body = pck.body<proto::packets::init_request_t>();

                        // Reject a consumer built against an incompatible protocol before doing any work.
                        if (!body ||
                            body->magic != proto::PROTO_MAGIC ||
                            body->proto_version != proto::PROTO_VERSION)
                        {
                            TEIO_ERROR("init request rejected: protocol mismatch "
                                "(got magic=0x{:X}, version={}; expected magic=0x{:X}, version={})"
                                , body ? body->magic : 0u
                                , body ? body->proto_version : 0u
                                , proto::PROTO_MAGIC
                                , proto::PROTO_VERSION
                            );
                            build_init_response(rep_pck_data,
                                proto::packets::init_status::version_mismatch,
                                0,
                                LUID{},
                                nullptr
                            );
                            return;
                        }

                        TEIO_TRACE("init request: {}x{}", body->frame_width, body->frame_height);

                        LUID adapter_luid{};
                        HANDLE surface_handle = nullptr;
                        try {
                            iface.on_session_init(
                                body->frame_width, body->frame_height, 
                                adapter_luid, 
                                surface_handle
                            );
                        } catch (const std::exception& e) {
                            TEIO_ERROR("renderer initialization failed: {}", e.what());
                            build_init_response(rep_pck_data,
                                proto::packets::init_status::internal_error,
                                0,
                                LUID{},
                                nullptr
                            );
                            return;
                        }

                        build_init_response(rep_pck_data,
                            proto::packets::init_status::ok,
                            ::GetCurrentProcessId(),
                            adapter_luid,
                            surface_handle
                        );
                        break;
                    }
                    case proto::packet_type::frame_resize_request:
                    {
                        const auto* body = pck.body<proto::packets::frame_resize_request_t>();
                        if (!body) {
                            TEIO_WARN("malformed resize request");
                            return;
                        }

                        TEIO_TRACE("resize request: {}x{}", body->width, body->height);

                        HANDLE new_surface_handle = nullptr;
                        try {
                            iface.on_frame_resize_event(
                                body->width, body->height, 
                                new_surface_handle
                            );
                        } catch (const std::exception& e) {
                            TEIO_ERROR("frame resize failed: {}", e.what());
                            // Always reply with a well-formed response; a null handle signals
                            // the resize failure to the consumer.
                            new_surface_handle = nullptr;
                        }

                        packet_builder<proto::packets::frame_resize_response_t> rep_pck{ static_cast<uint32_t>(proto::packet_type::frame_resize_response) };
                        rep_pck.body()->surface_handle = new_surface_handle;
                        rep_pck_data.assign(rep_pck.data(), rep_pck.data() + rep_pck.size());
                        break;
                    }
                    default:
                        TEIO_WARN("server got unknown request packet");
                        break;
                    } // switch
                });
        }

        // Install the notify handler, decoding each input packet into the matching
        // typed method on the session interface. Unrecognized packets go to on_session_notify.
        void install_input_dispatch(ipc_session& session, session_interface& iface)
        {
            session.set_notify_callback(
                [&iface](uint32_t id, std::string_view data)
                {
                    packet_view pck{ data.data(), data.size() };

                    switch (static_cast<proto::packet_type>(pck.type())) {
                    case proto::packet_type::mouse_button_event:
                    {
                        const auto* body = pck.body<proto::packets::mouse_button_event_t>();
                        if (body) {
                            iface.on_mouse_button_event(body->x, body->y, body->button, body->action, body->mods);
                        }
                        break;
                    }
                    case proto::packet_type::mouse_move_event:
                    {
                        const auto* body = pck.body<proto::packets::mouse_move_event_t>();
                        if (body) {
                            iface.on_mouse_move_event(body->x, body->y, body->mods);
                        }
                        break;
                    }
                    case proto::packet_type::mouse_scroll_event:
                    {
                        const auto* body = pck.body<proto::packets::mouse_scroll_event_t>();
                        if (body) {
                            iface.on_mouse_scroll_event(body->yoffset);
                        }
                        break;
                    }
                    default:
                        iface.on_session_notify(id, data);
                        break;
                    } // switch
                });
        }

    } // namespace

    surface_producer::surface_producer() = default;

    surface_producer::~surface_producer()
    {
        this->stop();
    }

    void surface_producer::start(
        std::string_view server_name, 
        std::shared_ptr<session_interface> iface)
    {
        _iface = std::move(iface);
        _server = ipc_server::make();

        _server->set_session_connect_callback(
            [this](std::shared_ptr<ipc_session> session)
            {
                // Wire up input delivery and the surface handshake, then start
                // receiving. All callbacks are installed before start(), so no request
                // or event is missed.
                install_input_dispatch(*session, *_iface);
                install_surface_handshake(*session, *_iface);
                session->start();
            });

        _server->set_session_disconnect_callback(
            [this]([[maybe_unused]] std::shared_ptr<ipc_session> session)
            {
                if (_iface) {
                    _iface->on_session_disconnect();
                }
            });

        // A shared scene is driven by a single consumer: host exactly one session.
        _server->start(server_name, 1);
    }

    void surface_producer::stop()
    {
        if (_server) {
            _server->stop();
            _server.reset();
        }
        _iface.reset();
    }

} // namespace triengine_interop::surface
