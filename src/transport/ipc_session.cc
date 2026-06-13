#include <triengine_ipc/transport/ipc_session.hh>

#include "detail/ipc_detail.hh"
#include <triengine_ipc/utility/logger.hh>

#include <thread>
#include <future>
#include <atomic>
#include <unordered_map>
#include <cassert>
#include <stdexcept>
#include <chrono>
#include <vector>
#include <mutex>
#include <utility>

namespace triengine::ipc
{

ipc_session::ipc_session(
    std::unique_ptr<detail::ipc_session_base> base,
    close_callback close_cb)
    : _base{ std::move(base) }
    , _cb_close{ std::move(close_cb) }
{
    TEIPC_TRACE("{} ENTER", __func__);

    if (!_base) {
        throw std::invalid_argument{ "ipc_session base cannot be null" };
    }

    TEIPC_TRACE("{} LEAVE", __func__);
}

ipc_session::~ipc_session()
{
    TEIPC_TRACE("{} ENTER", __func__);

    // NOTE: We can't directly call `close()` here, because `shared_from_this()` requires the object to be alive.
    // see: https://stackoverflow.com/q/28338978/3865427
    _is_alive = false;
    if (_recv_thread.joinable()) {
        _recv_thread.join();
    }

    TEIPC_TRACE("{} LEAVE", __func__);
}

std::string_view ipc_session::get_name() const
{
    return _base->get_name();
}

bool ipc_session::is_alive() const noexcept
{
    return _is_alive;
}

bool ipc_session::is_recv_finished() const noexcept
{
    return _is_recv_done;
}

void ipc_session::start()
{
    std::scoped_lock lk{ _session_lock };

    auto new_state = std::make_unique<ipc_session::session_state_t>();
    new_state->last_peer_heartbeat = std::chrono::steady_clock::now();

    _state = std::move(new_state);
    _is_alive = true;
    _recv_thread = std::thread{ &ipc_session::_do_recv, this };
}

// Precondition: never invoked on the receive thread. close() joins that thread, so
// running it there would self-join. The only callers are owner-driven graceful closes
// (`ipc_server::stop()` / `ipc_client::disconnect()`), which always run on a thread
// other than the receive thread. To close from the receive thread or a packet callback,
// use request_close() (asynchronous, no join).
void ipc_session::close()
{
    assert(std::this_thread::get_id() != _recv_thread.get_id());

    std::scoped_lock lk{ _session_lock };

    TEIPC_DEBUG("session close start..");

    // Signal the receive loop to stop and best-effort notify the peer (fires once).
    this->request_close();

    // Wait for the receive thread to finish, then release the per-connection state.
    if (_recv_thread.joinable()) {
        TEIPC_TRACE("terminating recv thread..");
        _recv_thread.join(); // NOTE: `_cb_close` will be called in this thread.
    } else {
        // recv thread not running, calling disconnect callback directly
        if (_cb_close) {
            _cb_close(shared_from_this());
        }
    }

    _state.reset();
    TEIPC_DEBUG("session close complete.");
}

// Flip the session to not-alive and best-effort notify the peer, without joining the
// receive thread. The receive loop observes this on its next iteration, exits, and runs
// `_cb_close`; the owner joins the thread later via `~ipc_session`. See the header for
// the full contract.
void ipc_session::request_close() noexcept
{
    // exchange() makes the disconnect notification fire at most once, even if close()
    // and request_close() race or are both called.
    if (_is_alive.exchange(false)) {
        // Best-effort: the send may fail if the peer is already gone. Ignored on purpose.
        static_cast<void>(_base->send_packet(detail::ipc_packet_type::disconnect, 0));
    }
}

void ipc_session::set_notify_callback(notify_packet_callback cb)
{
    if (_is_alive) {
        throw std::logic_error{ "set_notify_callback must be called before start()" };
    }
    _cb_notify_pck = std::move(cb);
}

void ipc_session::set_request_callback(request_packet_callback cb)
{
    if (_is_alive) {
        throw std::logic_error{ "set_request_callback must be called before start()" };
    }
    _cb_req_pck = std::move(cb);
}

std::errc ipc_session::send_notify(
    const void* const payload,
    const size_t payload_size)
{
    // Holding the lock for a long time while `send_packet()` performs I/O operations can be burdensome,
    // but it is unavoidable for state consistency (to prevent TOCTTOU race conditions)!
    std::scoped_lock lk{ _session_lock };
    if (!_is_alive) {
        TEIPC_WARN("Session is not alive, cannot send notify.");
        return std::errc::not_connected;
    }

    if (boost_ipc::no_error != _base->send_packet(
        detail::ipc_packet_type::notify,
        0,
        payload,
        payload_size))
    {
        TEIPC_ERROR("failed to send notify");
        this->request_close();
        return std::errc::io_error;
    }

    return std::errc{};
}

std::errc ipc_session::send_request_sync(
    const void* const payload,
    const size_t payload_size,
    std::vector<uint8_t>& response,
    const std::chrono::milliseconds timeout)
{
    if (payload_size == 0 || !payload) {
        TEIPC_WARN("Invalid payload for request.");
        return std::errc::invalid_argument;
    }

    uint32_t curr_req_id;
    std::future<std::vector<uint8_t>> req_future;

    // acquire the session lock...
    {
        std::scoped_lock lk{ _session_lock };
        if (!_state) { // is the session still valid?
            return std::errc::not_connected;
        }
        curr_req_id = ++_state->next_req_pck_id;

        std::promise<std::vector<uint8_t>> prm;
        req_future = prm.get_future();

        {
            std::scoped_lock lk_req{ _state->req_map_lock };
            _state->req_map[curr_req_id] = std::move(prm);
        }

        // Holding the lock for a long time while `send_packet()` performs I/O operations can be burdensome,
        // but it is unavoidable for state consistency (to prevent TOCTTOU race conditions)!
        if (boost_ipc::no_error != _base->send_packet(
            detail::ipc_packet_type::request,
            curr_req_id,
            payload,
            payload_size))
        {
            TEIPC_ERROR("failed to send request");

            {
                std::scoped_lock lk_req{ _state->req_map_lock };
                _state->req_map.erase(curr_req_id);
            }

            this->request_close();
            return std::errc::io_error;
        }
    }

    // release the session lock & start waiting for the future without the lock...
    if (req_future.wait_for(timeout) == std::future_status::timeout)
    {
        bool timeout_occurred{ false };
        {
            // re-acquire the session lock
            std::scoped_lock lk{ _session_lock };
            if (!_state) { // is the session still valid?
                return std::errc::not_connected;
            }

            std::unique_lock lk_req{ _state->req_map_lock };
            if (auto it = _state->req_map.find(curr_req_id);
                it != _state->req_map.end())
            {
                try {
                    // Attempt to set a timeout exception in promise (to prevent race conditions when handling timeouts)
                    it->second.set_exception(std::make_exception_ptr(std::runtime_error("Request timed out")));

                    // If `set_exception()` succeeded, then a timeout actually occurred, so remove this promise from the map.
                    _state->req_map.erase(it);
                    lk_req.unlock();

                    timeout_occurred = true;
                } catch (const std::future_error&) {
                    // If the receiving thread has already called `set_value()`, `set_exception()` will fail.
                    // in this case, we do nothing and call `future.get()` below to handle the fall-through.
                    TEIPC_TRACE("Request(seq={}) timed out, but response arrived just in time.", curr_req_id);
                }
            }
        }

        if (timeout_occurred) {
            TEIPC_ERROR("Request(seq={}) timed out.", curr_req_id);
            return std::errc::timed_out;
        }
    }

    // Reaching here means the request was not turned into a timeout above, which happens
    // when `wait_for` returned ready, or it expired but the receive thread had already
    // removed this request from `req_map` (the response arrived right at the deadline, or
    // the session closed). In every such case the promise is already satisfied or about to
    // be by the receive thread, so this get() returns almost immediately rather than
    // blocking for another full timeout. A delivered response is returned as success
    // instead of being discarded as a timeout.
    try {
        response = req_future.get();
    } catch (const std::exception& e) {
        // The promise was broken or set to an exception because the session closed (e.g.
        // the peer disconnected) while waiting for the response.
        TEIPC_WARN("request failed: {}", e.what());
        return std::errc::not_connected;
    }
    return std::errc{};
}

// NOTE: This function does NOT hold the context lock, be careful with synchronization.
void ipc_session::_do_recv()
{
    TEIPC_TRACE("IPC session recv started...");

    constexpr auto kHeartBeatInterval = 5s;
    constexpr auto kHeartBeatTimeout = 40s;
    constexpr auto kRecvTimeout = 100ms;

    auto last_sent_heartbeat = std::chrono::steady_clock::now();

    std::vector<uint8_t> rep_pck_buff;
    rep_pck_buff.reserve(1024);

    while (_is_alive)
    {
        // Check heartbeat timed out...
        if (std::chrono::steady_clock::now() - _state->last_peer_heartbeat.load() > kHeartBeatTimeout) {
            TEIPC_ERROR("session {}: Remote disconnected (Peer heartbeat timed out)", this->get_name());
            _is_alive = false;
            break;
        }

        // Check if it's time to send a heartbeat...
        if (std::chrono::steady_clock::now() - last_sent_heartbeat > kHeartBeatInterval) {
            // Send heartbeat packet
            if (boost_ipc::no_error != _base->send_packet(detail::ipc_packet_type::heartbeat, 0)) {
                TEIPC_ERROR("failed to send heartbeat");
            }
            last_sent_heartbeat = std::chrono::steady_clock::now();
        }

        // Try to receive a packet...
        auto recvd_res = _base->receive_packet(kRecvTimeout);
        if (!recvd_res) {
            continue; // If failed, do busy-wait
        }

        auto& recvd_pck = recvd_res.value();

        // Process the received packet. The notify/request callbacks below are read without
        // a lock: this is safe because they are installed before start() and never change
        // while this thread runs (see set_notify_callback / set_request_callback).
        if (recvd_pck.type() == detail::ipc_packet_type::heartbeat)
        {
            TEIPC_TRACE("session {}: update heartbeat", this->get_name());
            _state->last_peer_heartbeat.store(std::chrono::steady_clock::now());
            continue;
        }
        else if (recvd_pck.type() == detail::ipc_packet_type::disconnect)
        {
            TEIPC_WARN("session {}: Remote disconnected", this->get_name());
            _is_alive = false;
            break;
        }
        else if (recvd_pck.type() == detail::ipc_packet_type::notify)
        {
            if (_cb_notify_pck) {
                _cb_notify_pck(
                    recvd_pck.id(),
                    recvd_pck.data()
                );
            }
        }
        else if (recvd_pck.type() == detail::ipc_packet_type::request)
        {
            if (_cb_req_pck)
            {
                rep_pck_buff.clear();
                _cb_req_pck(
                    recvd_pck.id(),
                    recvd_pck.data(),
                    rep_pck_buff
                );
                if (!rep_pck_buff.empty()) {
                    if (boost_ipc::no_error != _base->send_packet(
                        detail::ipc_packet_type::response,
                        recvd_pck.id(),
                        rep_pck_buff.data(),
                        rep_pck_buff.size()))
                    {
                        TEIPC_ERROR("failed to send response");
                    }
                } else {
                    TEIPC_WARN("response packet empty");
                }
            }
            else
            {
                TEIPC_WARN("request handler not registered");
            }
        }
        else if (recvd_pck.type() == detail::ipc_packet_type::response)
        {
            // If it is a response packet, find the promise from request map and pass the result.
            std::unique_lock lk_req{ _state->req_map_lock };
            auto req_node = _state->req_map.extract(recvd_pck.id());
            lk_req.unlock();
            if (!req_node.empty()) {
                auto& prm = req_node.mapped();
                std::vector<uint8_t> payload_copy;
                payload_copy.assign(recvd_pck.data().begin(), recvd_pck.data().end());
                try {
                    prm.set_value(std::move(payload_copy));
                } catch (const std::future_error& e) {
                    TEIPC_WARN("Failed to set value for request ID {}: {}", recvd_pck.id(), e.what());
                }
            } else {
                TEIPC_WARN("Received response for unknown request ID: {}", recvd_pck.id());
            }
        }
        else
        {
            TEIPC_WARN("Got unknown packet ({})", static_cast<int>(recvd_pck.type()));
        }

    } // while

    // Fail any in-flight requests so their callers stop waiting for a response that can no
    // longer arrive now that the receive loop has stopped, instead of blocking until each
    // request's individual timeout elapses.
    {
        std::scoped_lock lk_req{ _state->req_map_lock };
        for ([[maybe_unused]] auto& [pending_req_id, pending_req] : _state->req_map) {
            try {
                pending_req.set_exception(std::make_exception_ptr(std::runtime_error("session closed")));
            } catch (const std::future_error&) {
                // Entries still in req_map are unsatisfied, so this should not throw;
                // swallow it regardless so one stray promise cannot abort the cleanup.
            }
        }
        _state->req_map.clear();
    }

    if (_cb_close) {
        _cb_close(shared_from_this());
    }

    // Mark the receive thread as finished AFTER the close callback and after this
    // thread has dropped its own reference (the shared_from_this temporary above).
    // The server reaps the session only once this is set, by which point the session's
    // sole owner is the server's map, so destruction happens on the server's thread.
    _is_recv_done = true;

    TEIPC_TRACE("IPC session recv terminated.");
}

} // namespace triengine::ipc
