#pragma once
#include "ipc_session.hh"

#include <functional>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <string_view>
#include <stdexcept>
#include <cstddef>

namespace triengine_interop::transport
{
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
        // ipc_server hands weak references to itself to its sessions (via shared_from_this),
        // so it must be owned by a shared_ptr. Construct it only through this factory.
        static std::shared_ptr<ipc_server> make();

    private:
        ipc_server();

    public:
        ~ipc_server();

        ipc_server(const ipc_server&) = delete;
        ipc_server& operator=(const ipc_server&) = delete;

        void start(std::string_view server_name, size_t max_sessions);
        void stop();

        // Both MUST be set before start(): the accept and receive threads read them
        // without locking, so they are immutable once the server is listening. Throws
        // std::logic_error if called while the server is listening.
        void set_session_connect_callback(session_connect_callback cb) {
            if (_is_listening) {
                throw std::logic_error{ "set_session_connect_callback must be called before start()" };
            }
            _on_session_connect = std::move(cb);
        }

        void set_session_disconnect_callback(session_disconnect_callback cb) {
            if (_is_listening) {
                throw std::logic_error{ "set_session_disconnect_callback must be called before start()" };
            }
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

} // namespace triengine_interop::transport
