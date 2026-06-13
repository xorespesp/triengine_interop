#pragma once
#include <functional>
#include <system_error>
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

        // True once the receive thread has fully exited. The server uses this to reap
        // the session from a thread other than the receive thread (so the session is
        // never destroyed on the thread its destructor joins).
        bool is_recv_finished() const noexcept;

        void start();

        // Synchronously close the session: stop the receive loop, JOIN its thread, and
        // send a disconnect notification to the peer. MUST be called from a thread other
        // than the receive thread, since it joins that thread. This is the owner-driven
        // graceful close (server stop / client disconnect).
        void close();

        // Asynchronously request a close: flip the session to not-alive, send a
        // best-effort disconnect notification to the peer, and return immediately WITHOUT
        // joining the receive thread. Safe to call from any thread, including the receive
        // thread itself or a packet callback. The receive loop observes this on its next
        // iteration and exits; the session's owner (the server's reaper or the client)
        // joins the receive thread later via ~ipc_session. For a synchronous shutdown that
        // also waits for the receive thread to finish, use close() (non-receive thread only).
        void request_close() noexcept;

        // Install the packet handlers. MUST be called before start(): the receive thread
        // reads them without locking, so they are immutable once it is running.
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
        void _do_recv();

    private:
        mutable std::mutex _session_lock;

        const std::unique_ptr<detail::ipc_session_base> _base;
        std::unique_ptr<session_state_t> _state;
        std::atomic_bool _is_alive{ false };
        std::atomic_bool _is_recv_done{ false };
        std::thread _recv_thread;

        notify_packet_callback _cb_notify_pck;
        request_packet_callback _cb_req_pck;
        close_callback _cb_close;
    };

} // namespace triengine::ipc
