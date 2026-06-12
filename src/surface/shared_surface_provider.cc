#include <triengine_ipc/surface/shared_surface_provider.hh>
#include <triengine_ipc/proto/ipc_proto.hh>

#include "../logger.hh"

#include <utility>
#include <vector>
#include <string_view>
#include <exception>

namespace triengine::ipc::surface
{
    namespace proto = triengine::ipc::proto;

    namespace
    {
        void build_init_response(
            std::vector<uint8_t>& out,
            proto::packets::init_status status,
            DWORD renderer_process_id,
            LUID adapter_luid,
            HANDLE surface_handle)
        {
            packet_builder<proto::packets::init_response_t> pck{ proto::packet_type::init_response };
            pck.body()->status = status;
            pck.body()->renderer_process_id = renderer_process_id;
            pck.body()->target_adapter_luid = adapter_luid;
            pck.body()->surface_handle = surface_handle;
            out.assign(pck.data(), pck.data() + pck.size());
        }

    } // namespace

    shared_surface_provider::shared_surface_provider(surface_source src)
        : _src{ std::move(src) }
    {}

    shared_surface_provider::~shared_surface_provider() = default;

    void shared_surface_provider::attach(ipc_session& session)
    {
        session.set_request_callback(
            [this](
                [[maybe_unused]] uint32_t req_pck_id,
                std::string_view req_pck_data,
                std::vector<uint8_t>& rep_pck_data)
            {
                packet_view pck{ req_pck_data.data(), req_pck_data.size() };

                switch (pck.type()) {
                case proto::packet_type::init_request:
                {
                    const auto* body = pck.body<proto::packets::init_request_t>();

                    // Reject clients built against an incompatible protocol before doing any work.
                    if (!body ||
                        body->magic != proto::PROTO_MAGIC ||
                        body->proto_version != proto::PROTO_VERSION)
                    {
                        TEIPC_ERROR("init request rejected: protocol mismatch "
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

                    TEIPC_TRACE("init request: {}x{}", body->frame_width, body->frame_height);

                    bool ok = false;
                    try {
                        ok = _src.initialize && _src.initialize(body->frame_width, body->frame_height);
                    } catch (const std::exception& e) {
                        TEIPC_ERROR("renderer initialization failed: {}", e.what());
                        ok = false;
                    }

                    if (!ok) {
                        TEIPC_ERROR("renderer initialization failed");
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
                        _src.adapter_luid ? _src.adapter_luid() : LUID{},
                        _src.surface_handle ? _src.surface_handle() : nullptr
                    );
                    break;
                }
                case proto::packet_type::frame_resize_request:
                {
                    const auto* body = pck.body<proto::packets::frame_resize_request_t>();
                    if (!body) {
                        TEIPC_WARN("malformed resize request");
                        return;
                    }

                    TEIPC_TRACE("resize request: {}x{}", body->width, body->height);

                    HANDLE new_surface_handle = nullptr;
                    try {
                        new_surface_handle = _src.resize ? _src.resize(body->width, body->height) : nullptr;
                    } catch (const std::exception& e) {
                        TEIPC_ERROR("frame resize failed: {}", e.what());
                        new_surface_handle = nullptr;
                    }

                    if (!new_surface_handle) {
                        TEIPC_ERROR("frame resize failed");
                        return;
                    }

                    packet_builder<proto::packets::frame_resize_response_t> rep_pck{ proto::packet_type::frame_resize_response };
                    rep_pck.body()->surface_handle = new_surface_handle;
                    rep_pck_data.assign(rep_pck.data(), rep_pck.data() + rep_pck.size());
                    break;
                }
                default:
                    TEIPC_WARN("provider got unknown request packet");
                    break;
                } // switch
            });
    }

} // namespace triengine::ipc::surface
