#pragma once
#include <functional>
#include <system_error>
#include <optional>
#include <chrono>
#include <vector>
#include <memory>
#include <thread>
#include <future>
#include <atomic>
#include <mutex>
#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace triengine::ipc
{
    using namespace std::chrono_literals;

    namespace detail
    {
        class ipc_session_base; // forward declaration
    }

    class ipc_session : public std::enable_shared_from_this<ipc_session>
    {
    public:
        using notify_packet_callback = std::function<void(uint32_t pck_id, std::string_view pck_data)>;
        using request_packet_callback = std::function<void(uint32_t req_pck_id, std::string_view req_pck_data, std::vector<uint8_t>& rep_pck_data)>;
        using close_callback = std::function<void(std::shared_ptr<ipc_session>)>;

    private:
        struct session_state_t
        {
            std::atomic<std::chrono::steady_clock::time_point> last_peer_heartbeat;
            std::atomic_uint32_t next_req_pck_id{ 0 };

            std::unordered_map<uint32_t, std::promise<std::vector<uint8_t>>> req_map;
            mutable std::mutex req_map_lock;
        };

    public:
        ipc_session(
            std::unique_ptr<detail::ipc_session_base> base,
            close_callback close_cb
        );

        ~ipc_session();

        std::string_view get_name() const;

        bool is_alive() const noexcept;

        void start();
        void close();

        void set_notify_callback(notify_packet_callback cb);
        void set_request_callback(request_packet_callback cb);

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
        void _do_close();
        void _do_recv();

    private:
        mutable std::mutex _session_lock;

        const std::unique_ptr<detail::ipc_session_base> _base;
        std::unique_ptr<session_state_t> _state;
        std::atomic_bool _is_alive{ false };
        std::thread _recv_thread;

        notify_packet_callback _cb_notify_pck;
        request_packet_callback _cb_req_pck;
        close_callback _cb_close;
    };

    class ipc_server : public std::enable_shared_from_this<ipc_server>
    {
    public:
        using session_connect_callback = std::function<void(std::shared_ptr<ipc_session> session)>;
        using session_disconnect_callback = std::function<void(std::shared_ptr<ipc_session> session)>;

    private:
        struct context_t;
        struct context_deleter { void operator()(context_t* p) const; }; // https://stackoverflow.com/a/32269374
        using  context_unique_ptr = std::unique_ptr<context_t, context_deleter>;

    public:
        ipc_server();
        ~ipc_server();

        ipc_server(const ipc_server&) = delete;
        ipc_server& operator=(const ipc_server&) = delete;

        void start(std::string_view server_name, size_t max_sessions);
        void stop();

        void set_session_connect_callback(session_connect_callback cb) {
            _on_session_connect = std::move(cb);
        }

        void set_session_disconnect_callback(session_disconnect_callback cb) {
            _on_session_disconnect = std::move(cb);
        }

    private:
        void _do_accept();

    private:
        context_unique_ptr _ctx;
        mutable std::mutex _ctx_lock;

        std::thread _accept_thread;
        std::atomic_bool _is_listening{ false };

        session_connect_callback _on_session_connect;
        session_disconnect_callback _on_session_disconnect;
    };

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
        ipc_client();
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
