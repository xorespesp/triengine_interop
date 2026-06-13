#include <triengine_ipc/transport/ipc_client.hh>

#include "detail/ipc_detail.hh"
#include <triengine_ipc/utility/logger.hh>

#include <thread>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <cstdio>
#include <utility>

namespace triengine::ipc
{

struct ipc_client::context_t final
{
    std::shared_ptr<boost_ipc::managed_shared_memory> shm;
    std::shared_ptr<ipc_session> session;
};

void ipc_client::context_deleter::operator()(context_t* p) const
{
    delete p;
}

std::shared_ptr<ipc_client> ipc_client::make()
{
    // Not std::make_shared: the constructor is private and only this member can reach it.
    return std::shared_ptr<ipc_client>{ new ipc_client{} };
}

ipc_client::ipc_client()
{}

ipc_client::~ipc_client()
{
    this->disconnect();
}

bool ipc_client::is_connected() const noexcept
{
    std::scoped_lock ctx_lk{ _ctx_lock };
    return _ctx && _ctx->session && _ctx->session->is_alive();
}

bool ipc_client::connect(
    const std::string_view server_name,
    const std::chrono::milliseconds timeout)
{
    std::scoped_lock ctx_lk{ _ctx_lock };
    if (_ctx) {
        TEIPC_ERROR("Already connected or connection in progress.");
        return false;
    }

    std::unique_ptr<boost_ipc::message_queue> mq_handshake_s2c, mq_handshake_c2s;
    std::shared_ptr<boost_ipc::managed_shared_memory> shm;
    try {
        auto s2c_handshake_mq_name = fmt::format("{}-handshake-s2c-mq", server_name);
        auto c2s_handshake_mq_name = fmt::format("{}-handshake-c2s-mq", server_name);
        auto shm_name = fmt::format("{}-shm", server_name);

        mq_handshake_s2c = std::make_unique<boost_ipc::message_queue>(boost_ipc::open_only, s2c_handshake_mq_name.c_str());
        mq_handshake_c2s = std::make_unique<boost_ipc::message_queue>(boost_ipc::open_only, c2s_handshake_mq_name.c_str());
        shm = std::make_shared<boost_ipc::managed_shared_memory>(boost_ipc::open_only, shm_name.c_str());
    } catch (const boost_ipc::interprocess_exception& e) {
        TEIPC_ERROR("Failed to open server handshake resources: {}", e.what());
        return false;
    }

    // Send a handshake request carrying a unique nonce and an absolute wall-clock deadline,
    // then wait for the reply that echoes our nonce. The reply queue is shared by all
    // clients, so a reply meant for another client can arrive here: put it back if its owner
    // may still be waiting (deadline not yet passed), or drop it if expired so it cannot
    // circulate forever. This correlates replies without a per-client queue.
    const uint64_t client_nonce = detail::make_handshake_nonce();
    const auto deadline_tp = std::chrono::system_clock::now() + timeout;

    detail::handshake_req_mq handshake_req_pck{};
    handshake_req_pck.client_nonce = client_nonce;
    handshake_req_pck.deadline_unix_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(deadline_tp.time_since_epoch()).count();
    std::snprintf(handshake_req_pck.server_name, sizeof(handshake_req_pck.server_name), "%s", server_name.data());
    mq_handshake_c2s->send(&handshake_req_pck, sizeof(handshake_req_pck), 0);

    const boost::posix_time::ptime recv_deadline =
        boost::posix_time::second_clock::universal_time() + boost::posix_time::milliseconds(timeout.count());

    detail::handshake_rep_mq handshake_rep_pck{};
    bool got_response = false;
    while (true) {
        unsigned int priority;
        boost_ipc::message_queue::size_type recvd_size;
        if (!mq_handshake_s2c->timed_receive(
            &handshake_rep_pck,
            sizeof(handshake_rep_pck),
            recvd_size,
            priority,
            recv_deadline))
        {
            break; // overall deadline reached without our reply
        }

        if (handshake_rep_pck.client_nonce == client_nonce) {
            got_response = true;
            break;
        }

        // Not our reply: re-queue it for its owner if still valid, otherwise drop it.
        //
        // This reply queue is read by every connecting client, so a reply destined for
        // another client can legitimately land in our receive. A receive is a destructive
        // dequeue with no recipient targeting, so discarding a still-valid reply would
        // destroy it for its owner and starve that client. We therefore put valid replies
        // back; only an expired one (its owner has already given up) is safe to drop. This
        // re-queue is needed precisely because the reply queue has multiple readers, unlike
        // the server's single-reader request queue.
        if (detail::system_now_unix_ns() <= handshake_rep_pck.deadline_unix_ns) {
            mq_handshake_s2c->send(&handshake_rep_pck, sizeof(handshake_rep_pck), 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(1)); // avoid a tight re-read spin
        }
    }

    if (!got_response) {
        TEIPC_ERROR("Handshake with server '{}' timed out.", server_name);
        return false;
    }

    // Create a new session
    const std::string_view new_session_name = handshake_rep_pck.session_name;
    auto new_session = std::make_shared<ipc_session>(
        std::make_unique<detail::ipc_session_base>(detail::ipc_session_base::mode_type::open, new_session_name, shm),
        [weak_self = std::weak_ptr{ shared_from_this() }]([[maybe_unused]] std::shared_ptr<ipc_session> session)
        {
            TEIPC_INFO("Client disconnected!");

            auto self = weak_self.lock();
            if (!self) {
                TEIPC_WARN("Client instance no longer exists, cannot handle disconnection.");
                return;
            }

            disconnect_callback disconn_cb; {
                std::scoped_lock ctx_lk{ self->_ctx_lock };
                disconn_cb = self->_on_disconnect;
            }

            if (disconn_cb) {
                disconn_cb();
            }
        });
    new_session->set_notify_callback(_on_notify);
    new_session->set_request_callback(_on_request);

    auto new_ctx = context_unique_ptr{ new context_t{} };
    new_ctx->shm = shm;
    new_ctx->session = new_session;
    _ctx = std::move(new_ctx);
    _ctx->session->start();

    TEIPC_INFO("Connected to server! (session name: {})", new_session_name);
    return true;
}

void ipc_client::disconnect()
{
    // Move the current context to a temporary variable to safely remove it
    // (to prevent deadlock due to lock contention)
    context_unique_ptr old_ctx; {
        std::scoped_lock ctx_lk{ _ctx_lock };
        old_ctx = std::move(_ctx);
    }

    if (old_ctx && old_ctx->session) {
        old_ctx->session->close();
    }
}

// Stored here and applied to the session in connect(). A session's callbacks are fixed
// once its receive thread starts, so changing these after connect() affects only the next
// connection, not the current one.
void ipc_client::set_notify_callback(notify_packet_callback cb) {
    std::scoped_lock ctx_lk{ _ctx_lock };
    _on_notify = std::move(cb);
}

void ipc_client::set_request_callback(request_packet_callback cb) {
    std::scoped_lock ctx_lk{ _ctx_lock };
    _on_request = std::move(cb);
}

void ipc_client::set_disconnect_callback(disconnect_callback cb) {
    std::scoped_lock ctx_lk{ _ctx_lock };
    _on_disconnect = std::move(cb);
}

std::errc ipc_client::send_notify(
    const void* const payload,
    const size_t payload_size)
{
    // Take a reference to the session under the lock, then release the lock before the
    // blocking call: holding _ctx_lock for the whole request (up to `timeout`) would stall
    // disconnect(), is_connected() and the disconnect callback for that entire duration.
    std::shared_ptr<ipc_session> session; {
        std::scoped_lock ctx_lk{ _ctx_lock };
        if (!_ctx || !_ctx->session) {
            return std::errc::not_connected;
        }
        session = _ctx->session;
    }

    return session->send_notify(
        payload,
        payload_size
    );
}

std::errc ipc_client::send_request_sync(
    const void* const payload,
    const size_t payload_size,
    std::vector<uint8_t>& response,
    const std::chrono::milliseconds timeout)
{
    // Take a reference to the session under the lock, then release the lock before the
    // blocking call: holding _ctx_lock for the whole request (up to `timeout`) would stall
    // disconnect(), is_connected() and the disconnect callback for that entire duration.
    std::shared_ptr<ipc_session> session; {
        std::scoped_lock ctx_lk{ _ctx_lock };
        if (!_ctx || !_ctx->session) {
            return std::errc::not_connected;
        }
        session = _ctx->session;
    }

    return session->send_request_sync(
        payload,
        payload_size,
        response,
        timeout
    );
}

} // namespace triengine::ipc
