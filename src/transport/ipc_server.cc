#include <triengine_interop/transport/ipc_server.hh>

#include "detail/ipc_detail.hh"
#include <triengine_interop/utility/logger.hh>

#include <thread>
#include <atomic>
#include <unordered_map>
#include <stdexcept>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace triengine_interop::transport
{
    struct ipc_server::context_t final
    {
        std::string server_name;
        std::string s2c_handshake_mq_name, c2s_handshake_mq_name;
        std::string shm_name;

        std::unique_ptr<boost_ipc::message_queue> mq_handshake_s2c, mq_handshake_c2s; // Handshake REQ/REP 송수신 큐
        std::shared_ptr<boost_ipc::managed_shared_memory> shm;

        std::unordered_map<std::string, std::shared_ptr<ipc_session>> sessions;
        size_t max_sessions{ 0 };
        mutable std::mutex sessions_mutex;

        std::atomic_uint32_t next_session_id{ 0 };

        context_t() = default;
    };

    void ipc_server::context_deleter::operator()(ipc_server::context_t* p) const
    {
        delete p;
    }

    std::shared_ptr<ipc_server> ipc_server::make()
    {
        // Not std::make_shared: the constructor is private and only this member can reach it.
        return std::shared_ptr<ipc_server>{ new ipc_server{} };
    }

    ipc_server::ipc_server()
    {}

    ipc_server::~ipc_server()
    {
        this->stop();
    }

    void ipc_server::start(std::string_view server_name, const size_t max_sessions)
    {
        std::scoped_lock ctx_lk{ _ctx_lock };
        if (_ctx) {
            throw std::runtime_error{ "already started" };
        }

        auto new_ctx = context_unique_ptr{ new context_t{} };
        new_ctx->server_name = server_name;
        new_ctx->s2c_handshake_mq_name = fmt::format("{}-handshake-s2c-mq", server_name);
        new_ctx->c2s_handshake_mq_name = fmt::format("{}-handshake-c2s-mq", server_name);
        new_ctx->shm_name = fmt::format("{}-shm", server_name);

        boost_ipc::message_queue::remove(new_ctx->s2c_handshake_mq_name.c_str());
        boost_ipc::message_queue::remove(new_ctx->c2s_handshake_mq_name.c_str());
        boost_ipc::shared_memory_object::remove(new_ctx->shm_name.c_str());

        constexpr size_t kMaxMQMessages = 1024;

        new_ctx->mq_handshake_s2c = std::make_unique<boost_ipc::message_queue>(
            boost_ipc::create_only,
            new_ctx->s2c_handshake_mq_name.c_str(),
            kMaxMQMessages,
            sizeof(detail::handshake_rep_mq)
        );

        new_ctx->mq_handshake_c2s = std::make_unique<boost_ipc::message_queue>(
            boost_ipc::create_only,
            new_ctx->c2s_handshake_mq_name.c_str(),
            kMaxMQMessages,
            sizeof(detail::handshake_req_mq)
        );

        new_ctx->shm = std::make_shared<boost_ipc::managed_shared_memory>(
            boost_ipc::create_only,
            new_ctx->shm_name.c_str(),
            65536
        );

        new_ctx->max_sessions = max_sessions;

        _ctx = std::move(new_ctx);

        _is_listening = true;
        _accept_thread = std::thread{ std::bind(&ipc_server::_do_accept, this) };
    }

    void ipc_server::stop()
    {
        _is_listening = false;
        if (_accept_thread.joinable()) {
            _accept_thread.join();
        }

        // Move the current context to a temporary variable to safely remove it
        // (to prevent deadlock due to lock contention)
        context_unique_ptr old_ctx; {
            std::scoped_lock ctx_lk{ _ctx_lock };
            old_ctx = std::move(_ctx);
        }

        if (old_ctx) {
            // Explicitly close all sessions
            std::scoped_lock sessions_lk{ old_ctx->sessions_mutex };
            for (auto const& [name, session] : old_ctx->sessions) {
                session->close();
            }
            old_ctx->sessions.clear();

            // Cleanup the IPC resources...
            boost_ipc::message_queue::remove(old_ctx->s2c_handshake_mq_name.c_str());
            boost_ipc::message_queue::remove(old_ctx->c2s_handshake_mq_name.c_str());
            boost_ipc::shared_memory_object::remove(old_ctx->shm_name.c_str());
        }
    }

    // NOTE: This function does NOT hold the context lock, be careful with synchronization.
    void ipc_server::_do_accept()
    {
        // Runs one iteration of the accept loop. May throw (boost IPC errors, thread creation);
        // the loop below catches so a single failure does not kill the accept thread. The
        // accepted session is published into the server's map only after it is fully brought up,
        // so a bring-up that throws discards the session here and leaves no never-reaped slot
        // holder.
        auto accept_once = [this]() {
            // Reap sessions whose receive thread has finished. Erasing here, on the accept
            // thread, destroys them off their own receive thread, so ~ipc_session joins that
            // (already finished) thread cross-thread instead of self-joining. This runs every
            // accept iteration (the receive below has a 100ms timeout), so dead sessions are
            // cleaned up promptly and the session slot is freed for reconnect.
            {
                std::scoped_lock sessions_lk{ _ctx->sessions_mutex };
                for (auto it = _ctx->sessions.begin(); it != _ctx->sessions.end(); ) {
                    if (it->second->is_recv_finished()) {
                        it = _ctx->sessions.erase(it);
                    } else {
                        ++it;
                    }
                }
            }

            detail::handshake_req_mq req_pck{};
            unsigned int priority{};
            boost_ipc::message_queue::size_type recvd_size{};
            if (!_ctx->mq_handshake_c2s->timed_receive(
                &req_pck,
                sizeof(req_pck),
                recvd_size,
                priority,
                boost::posix_time::second_clock::universal_time() + boost::posix_time::milliseconds(100)
            )) {
                return; // no handshake request this iteration
            }

            // Drop requests whose client already gave up (deadline passed) so we do not create a
            // phantom session that no one will connect to.
            //
            // The request queue has a single reader (this server's accept loop), so every
            // request received here is ours to handle. There is no wrong-recipient case like the
            // client side has, so the server only discards stale requests and never re-queues
            // them: a dropped request has no other reader that still needs it.
            if (detail::system_now_unix_ns() > req_pck.deadline_unix_ns) {
                TEIO_WARN("Stale handshake request dropped (client deadline already passed)");
                return;
            }

            // The accept thread is the only thread that adds sessions, and stop() joins it
            // before touching the map, so the slot checked here stays free until we publish below.
            {
                std::scoped_lock sessions_lk{ _ctx->sessions_mutex };
                if (_ctx->sessions.size() >= _ctx->max_sessions) {
                    TEIO_WARN("New session connection denied (maximum number of sessions reached)");
                    return;
                }
            }

            const std::string new_session_name = fmt::format("{}-session-{:X}",
                _ctx->server_name,
                static_cast<uint32_t>(++_ctx->next_session_id)
            );

            auto new_session = std::make_shared<ipc_session>(
                std::make_unique<detail::ipc_session_base>(detail::ipc_session_base::mode_type::create, new_session_name, _ctx->shm),
                [weak_self = std::weak_ptr{ shared_from_this() }](std::shared_ptr<ipc_session> session)
                {
                    TEIO_INFO("Session {} closed.", session->get_name());

                    auto self = weak_self.lock();
                    if (!self) {
                        return;
                    }

                    // Notify promptly, here on the session's receive thread, so the consumer can
                    // react immediately. The session is NOT removed from the server here; the
                    // accept loop reaps it once its receive thread has finished, so it is never
                    // destroyed on its own receive thread.
                    //
                    // Skip the notification when the server is no longer listening (an explicit
                    // stop() is tearing sessions down), matching the previous behavior.
                    const bool is_server_stopped = !self->_is_listening;
                    if (!is_server_stopped) {
                        auto session_disconn_cb = self->_on_session_disconnect;
                        if (session_disconn_cb) {
                            session_disconn_cb(session);
                        }
                    }
                });

            TEIO_INFO("New session accepted! (name: {})", new_session_name);

            // Bring the session up (the connect callback installs handlers and starts its
            // receive thread). If this throws, new_session is discarded here and was never
            // entered into the map, so a failed bring-up leaves nothing behind.
            if (_on_session_connect) {
                _on_session_connect(new_session);
            } else {
                TEIO_WARN("No session connect callback set, cannot notify about new session.");
            }

            // Send a handshake response(ACK) to the client, echoing the nonce/deadline for reply
            // correlation and stale filtering.
            {
                detail::handshake_rep_mq rep_pck{};
                rep_pck.client_nonce = req_pck.client_nonce;
                rep_pck.deadline_unix_ns = req_pck.deadline_unix_ns;
                // Copy the session name into the fixed-size field, capping at the field's
                // capacity and writing the null terminator after the copied bytes.
                const auto name_end = fmt::format_to_n(
                    rep_pck.session_name,
                    sizeof(rep_pck.session_name) - 1,
                    "{}",
                    new_session_name
                ).out;
                *name_end = '\0';

                _ctx->mq_handshake_s2c->send(
                    &rep_pck,
                    sizeof(rep_pck),
                    0
                );
            }

            // Publish the now-running session so the reaper and stop() can manage it.
            {
                std::scoped_lock sessions_lk{ _ctx->sessions_mutex };
                _ctx->sessions[new_session_name] = std::move(new_session);
            }
        };

        while (_is_listening)
        {
            try {
                accept_once();
            } catch (const std::exception& e) {
                // A handshake/queue error or a failed session bring-up must not take down the
                // accept thread: an uncaught exception here would terminate the process. Log it
                // and keep listening; the brief sleep avoids a tight error spin if the failure
                // is persistent.
                TEIO_ERROR("accept loop iteration failed: {}", e.what());
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }

} // namespace triengine_interop::transport
