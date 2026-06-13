#include <triengine_interop/surface/shared_surface_client.hh>

#include <triengine_interop/utility/logger.hh>

#include <utility>

namespace triengine_interop::surface
{
    namespace proto = triengine_interop::proto;

    shared_surface_client::shared_surface_client() = default;

    shared_surface_client::~shared_surface_client()
    {
        this->disconnect();
    }

    bool shared_surface_client::is_connected() const noexcept
    {
        return _client && _client->is_connected();
    }

    bool shared_surface_client::connect(
        const std::string_view server_name,
        const int32_t initial_width,
        const int32_t initial_height,
        const surface_render_options& config)
    {
        if (this->is_connected()) {
            TEIO_ERROR("client already connected");
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

        if (!_consumer.create(*client, initial_width, initial_height, config)) {
            TEIO_ERROR("failed to create shared surface consumer");
            client->disconnect();
            return false;
        }

        _client = std::move(client);
        return true;
    }

    void shared_surface_client::disconnect()
    {
        if (_consumer.is_created()) {
            _consumer.destroy();
        }

        if (_client) {
            if (_client->is_connected()) {
                _client->disconnect();
            }
            _client.reset();
        }
    }

    ID3D11Device2* shared_surface_client::get_dx11_device() const noexcept {
        return _consumer.get_dx11_device();
    }

    ID3D11DeviceContext2* shared_surface_client::get_dx11_context() const noexcept {
        return _consumer.get_dx11_context();
    }

    int32_t shared_surface_client::get_width() const noexcept {
        return _consumer.get_width();
    }

    int32_t shared_surface_client::get_height() const noexcept {
        return _consumer.get_height();
    }

    bool shared_surface_client::sync_latest_frame(uint32_t timeout_ms)
    {
        return _consumer.sync_latest_frame(timeout_ms);
    }

    void shared_surface_client::blit_to_render_target(
        ID3D11RenderTargetView* const target_rtv, 
        const D3D11_VIEWPORT& viewport)
    {
        _consumer.blit_to_render_target(target_rtv, viewport);
    }

    void shared_surface_client::flush() noexcept
    {
        if (auto* ctx = _consumer.get_dx11_context()) {
            ctx->Flush();
        }
    }

    bool shared_surface_client::resize(int32_t new_width, int32_t new_height)
    {
        if (!_client) {
            TEIO_ERROR("resize called before connect");
            return false;
        }
        return _consumer.resize(*_client, new_width, new_height);
    }

    std::errc shared_surface_client::send_notify(
        const void* const payload,
        const size_t size)
    {
        if (!_client) {
            return std::errc::not_connected;
        }
        return _client->send_notify(payload, size);
    }

    std::errc shared_surface_client::send_mouse_button_event(
        const int32_t x,
        const int32_t y,
        const proto::mouse_button_type button,
        const proto::button_action_type action,
        const proto::modifier_button_type mods)
    {
        const auto pck = proto::make_mouse_button_event(x, y, button, action, mods);
        return this->send_notify(pck.data(), pck.size());
    }

    std::errc shared_surface_client::send_mouse_move_event(
        const int32_t x,
        const int32_t y,
        const proto::modifier_button_type mods)
    {
        const auto pck = proto::make_mouse_move_event(x, y, mods);
        return this->send_notify(pck.data(), pck.size());
    }

    std::errc shared_surface_client::send_mouse_scroll_event(float yoffset)
    {
        const auto pck = proto::make_mouse_scroll_event(yoffset);
        return this->send_notify(pck.data(), pck.size());
    }

    void shared_surface_client::set_disconnect_callback(std::function<void()> cb)
    {
        _on_disconnect = std::move(cb);
        if (_client) {
            _client->set_disconnect_callback(_on_disconnect);
        }
    }

} // namespace triengine_interop::surface
