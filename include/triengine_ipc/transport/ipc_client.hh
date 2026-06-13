#pragma once
#include "ipc_session.hh"

#include <functional>
#include <system_error>
#include <chrono>
#include <vector>
#include <memory>
#include <mutex>
#include <cstdint>
#include <string_view>

namespace triengine::ipc
{
    using namespace std::chrono_literals;

    class ipc_client : public std::enable_shared_from_this<ipc_client>
    {
    public:
        using notify_packet_callback = ipc_session::notify_packet_callback;
        using request_packet_callback = ipc_session::request_packet_callback;
        using disconnect_callback = std::function<void()>;

    private:
        struct context_t;
        struct context_deleter { void operator()(context_t* p) const; };
        using context_unique_ptr = std::unique_ptr<context_t, context_deleter>;

    public:
        // ipc_client hands weak references to itself to its session (via shared_from_this),
        // so it must be owned by a shared_ptr. Construct it only through this factory.
        static std::shared_ptr<ipc_client> make();

    private:
        ipc_client();

    public:
        ~ipc_client();

        ipc_client(const ipc_client&) = delete;
        ipc_client& operator=(const ipc_client&) = delete;

        bool is_connected() const noexcept;

        bool connect(std::string_view server_name, std::chrono::milliseconds timeout = 5s);
        void disconnect();

        void set_notify_callback(notify_packet_callback cb);
        void set_request_callback(request_packet_callback cb);
        void set_disconnect_callback(disconnect_callback cb);

        std::errc send_notify(
            const void* payload,
            size_t payload_size
        );

        std::errc send_request_sync(
            const void* payload,
            size_t payload_size,
            std::vector<uint8_t>& response,
            std::chrono::milliseconds timeout = 10s
        );

    private:
        context_unique_ptr _ctx;
        mutable std::mutex _ctx_lock;

        notify_packet_callback _on_notify;
        request_packet_callback _on_request;
        disconnect_callback _on_disconnect;
    };

} // namespace triengine::ipc
