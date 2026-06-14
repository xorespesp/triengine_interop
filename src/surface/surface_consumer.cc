#include <triengine_interop/surface/surface_consumer.hh>
#include <triengine_interop/transport/ipc_packet.hh>

#include <triengine_interop/utility/bit.hh>
#include <triengine_interop/utility/logger.hh>

#include <utility>
#include <vector>
#include <chrono>

namespace triengine_interop::surface
{
    using transport::ipc_client;
    using transport::packet_builder;
    using transport::packet_view;

    namespace
    {
        // Surveys all active monitors and returns the highest refresh rate scaled by the
        // over-produce margin, to be used as the adaptive frame-rate cap. Taking the maximum
        // across monitors keeps the cap adequate if the consumer window is later moved to a
        // faster display (the handshake samples this only once, at connect). 
        // Falls back to a fixed cap if no refresh rate could be determined.
        uint32_t adaptive_max_fps()
        {
            uint32_t max_hz{ 0 };
            ::EnumDisplayMonitors(nullptr, nullptr,
                [](HMONITOR hmon, HDC, LPRECT, LPARAM lparam) -> BOOL
                {
                    uint32_t& out_max_hz = *utility::bit_cast<uint32_t*>(lparam);

                    MONITORINFOEXA mi{};
                    mi.cbSize = sizeof(mi);
                    if (::GetMonitorInfoA(hmon, &mi)) {
                        DEVMODEA dm{};
                        dm.dmSize = sizeof(dm);
                        if (::EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
                            // 0 or 1 means "default/unknown" rather than an actual hardware rate.
                            const uint32_t hz = static_cast<uint32_t>(dm.dmDisplayFrequency);
                            TEIO_DEBUG("{}: refresh rate {} Hz", mi.szDevice, hz);
                            if (hz > 1 && hz > out_max_hz) {
                                out_max_hz = hz;
                            }
                        }
                    }
                    return TRUE; // continue enumeration
                },
                utility::bit_cast<LPARAM>(&max_hz)
            );

            if (max_hz == 0) {
                constexpr uint32_t kFallbackMaxFps{ 200 };
                TEIO_WARN("failed to determine display refresh rate, falling back to {} fps", kFallbackMaxFps);
                return kFallbackMaxFps;
            }

            // Over-produce margin applied on top of the display refresh rate. The producer and
            // consumer run as two unsynchronized loops, so producing somewhat faster than the
            // display keeps the consumed frame fresh (lower input latency) at a modest CPU cost.
            constexpr double kOverproduceFactor{ 2.0 };

            // Absolute ceiling on the requested cap. Staleness shrinks as ~1/fps, so beyond a
            // few hundred fps the extra frames cost CPU/power for negligible latency gain. Clamp
            // so high-refresh displays do not push production into wasteful territory.
            constexpr uint32_t kMaxRequestedFps{ 240 };

            const uint32_t requested = static_cast<uint32_t>(std::floor(max_hz * kOverproduceFactor));
            return requested < kMaxRequestedFps ? requested : kMaxRequestedFps;
        }

        // Send the init request and await the renderer's init response, which carries the
        // renderer process id, the target adapter LUID, and the shared surface NT handle.
        bool request_init(
            ipc_client& cli,
            const SIZE initial_frame_size,
            const uint32_t requested_max_fps,
            proto::packets::init_response_t& out)
        {
            packet_builder<proto::packets::init_request_t> req{ static_cast<uint32_t>(proto::packet_type::init_request) };
            req.body()->magic = proto::PROTO_MAGIC;
            req.body()->proto_version = proto::PROTO_VERSION;
            req.body()->frame_width = static_cast<int32_t>(initial_frame_size.cx);
            req.body()->frame_height = static_cast<int32_t>(initial_frame_size.cy);
            req.body()->requested_max_fps = requested_max_fps;

            constexpr std::chrono::seconds init_request_timeout{ 30 };

            std::vector<uint8_t> rep_bytes;
            if (std::errc{} != cli.send_request_sync(req.data(), req.size(), rep_bytes, init_request_timeout)) {
                TEIO_ERROR("failed to send init request");
                return false;
            }

            packet_view view{ rep_bytes.data(), rep_bytes.size() };
            const auto* init_rep = view.body<proto::packets::init_response_t>();
            if (!init_rep || init_rep->status != proto::packets::init_status_code::ok) {
                TEIO_ERROR("init request rejected by renderer (status: {})"
                    , init_rep ? static_cast<int>(init_rep->status) : -1);
                return false;
            }

            out = *init_rep;
            TEIO_DEBUG("init response (pid: {}, adapter: {:x}-{:x}, surface: {:p})"
                , out.renderer_process_id
                , out.target_adapter_luid.HighPart
                , out.target_adapter_luid.LowPart
                , out.surface_handle
            );
            return true;
        }

        // Ask the renderer to resize its surface and return the new shared surface NT handle.
        bool request_resize(
            ipc_client& cli,
            const SIZE new_frame_size,
            HANDLE& out_surface_handle)
        {
            packet_builder<proto::packets::frame_resize_request_t> req{ static_cast<uint32_t>(proto::packet_type::frame_resize_request) };
            req.body()->width = static_cast<int32_t>(new_frame_size.cx);
            req.body()->height = static_cast<int32_t>(new_frame_size.cy);

            std::vector<uint8_t> rep_bytes;
            if (std::errc{} != cli.send_request_sync(req.data(), req.size(), rep_bytes)) {
                TEIO_ERROR("failed to send resize request");
                return false;
            }

            packet_view view{ rep_bytes.data(), rep_bytes.size() };
            const auto* resize_rep = view.body<proto::packets::frame_resize_response_t>();
            if (!resize_rep || !resize_rep->surface_handle) {
                TEIO_ERROR("resize rejected by renderer");
                return false;
            }

            out_surface_handle = resize_rep->surface_handle;
            return true;
        }

    } // namespace

    surface_consumer::surface_consumer() = default;

    surface_consumer::~surface_consumer()
    {
        this->disconnect();
    }

    bool surface_consumer::is_connected() const noexcept
    {
        return _client && _client->is_connected();
    }

    bool surface_consumer::connect(
        const std::string_view server_name,
        const SIZE initial_frame_size,
        const surface_render_options& config)
    {
        if (this->is_connected()) {
            TEIO_ERROR("consumer already connected");
            return false;
        }

        auto client = ipc_client::make();
        if (_on_disconnect) {
            client->set_disconnect_callback(_on_disconnect);
        }

        if (!client->connect(server_name)) {
            TEIO_ERROR("failed to connect to server '{}'", server_name);
            return false;
        }

        // Resolve the frame-rate cap to request: an explicit override from the caller, or
        // the adaptive value derived from the local displays when left unset (nullopt).
        const uint32_t requested_max_fps = config.max_fps.has_value()
            ? config.max_fps.value()
            : adaptive_max_fps();

        TEIO_DEBUG("Requesting initialization... (frame size: {}x{}, max fps: {})"
            , initial_frame_size.cx, initial_frame_size.cy
            , requested_max_fps
        );

        // Run the init handshake to obtain the renderer process id, target adapter LUID,
        // and shared surface handle, then hand them to the blitter to open the surface.
        proto::packets::init_response_t init_rep{};
        if (!request_init(
            *client,
            initial_frame_size,
            requested_max_fps,
            init_rep))
        {
            client->disconnect();
            return false;
        }

        if (!_blitter.create(
            init_rep.renderer_process_id,
            init_rep.target_adapter_luid,
            init_rep.surface_handle,
            proto::SHARED_SURFACE_MUTEX_KEY,
            config))
        {
            TEIO_ERROR("failed to create shared texture blitter");
            client->disconnect();
            return false;
        }

        _client = std::move(client);
        return true;
    }

    void surface_consumer::disconnect()
    {
        if (_blitter.is_created()) {
            _blitter.destroy();
        }

        if (_client) {
            if (_client->is_connected()) {
                _client->disconnect();
            }
            _client.reset();
        }
    }

    ID3D11Device2* surface_consumer::get_dx11_device() const noexcept {
        return _blitter.get_dx11_device();
    }

    ID3D11DeviceContext2* surface_consumer::get_dx11_context() const noexcept {
        return _blitter.get_dx11_context();
    }

    SIZE surface_consumer::get_frame_size() const noexcept {
        return _blitter.get_frame_size();
    }

    bool surface_consumer::sync_latest_frame(uint32_t timeout_ms)
    {
        return _blitter.sync_latest_frame(timeout_ms);
    }

    void surface_consumer::blit_to_render_target(
        ID3D11RenderTargetView* const target_rtv, 
        const D3D11_VIEWPORT& viewport)
    {
        _blitter.blit_to_render_target(target_rtv, viewport);
    }

    void surface_consumer::flush() noexcept
    {
        if (auto* ctx = _blitter.get_dx11_context()) {
            ctx->Flush();
        }
    }

    bool surface_consumer::resize_frame(SIZE new_size)
    {
        if (!_client) {
            TEIO_ERROR("resize_frame called before connect");
            return false;
        }

        // Ask the renderer to resize, then re-open the new shared surface in the blitter.
        HANDLE new_surface_handle{ nullptr };
        if (!request_resize(*_client, new_size, new_surface_handle)) {
            return false;
        }
        return _blitter.reallocate_frame(new_surface_handle);
    }

    std::errc surface_consumer::send_notify(
        const void* const payload,
        const size_t size)
    {
        if (!_client) {
            return std::errc::not_connected;
        }
        return _client->send_notify(payload, size);
    }

    std::errc surface_consumer::send_mouse_button_event(
        const POINT pos,
        const proto::mouse_button_type button,
        const proto::button_action_type action,
        const proto::modifier_button_type mods)
    {
        const auto pck = proto::make_mouse_button_event(pos.x, pos.y, button, action, mods);
        return this->send_notify(pck.data(), pck.size());
    }

    std::errc surface_consumer::send_mouse_move_event(
        const POINT pos,
        const proto::modifier_button_type mods)
    {
        const auto pck = proto::make_mouse_move_event(pos.x, pos.y, mods);
        return this->send_notify(pck.data(), pck.size());
    }

    std::errc surface_consumer::send_mouse_scroll_event(float yoffset)
    {
        const auto pck = proto::make_mouse_scroll_event(yoffset);
        return this->send_notify(pck.data(), pck.size());
    }

    std::errc surface_consumer::send_key_event(
        const proto::key_button_type key,
        const proto::button_action_type action,
        const proto::modifier_button_type mods)
    {
        if (key == proto::KEY_UNKNOWN) {
            return std::errc::invalid_argument;
        }
        const auto pck = proto::make_key_event(key, action, mods);
        return this->send_notify(pck.data(), pck.size());
    }

    void surface_consumer::set_disconnect_callback(std::function<void()> cb)
    {
        _on_disconnect = std::move(cb);
        if (_client) {
            _client->set_disconnect_callback(_on_disconnect);
        }
    }

} // namespace triengine_interop::surface
