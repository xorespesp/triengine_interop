#include <triengine_ipc/transport/ipc_service.hh>

#include <thread>
#include <future>
#include <atomic>
#include <unordered_map>
#include <cassert>

#include <boost/interprocess/ipc/message_queue.hpp>
#include <boost/interprocess/managed_shared_memory.hpp>
#include <boost/interprocess/allocators/allocator.hpp>
#include <boost/interprocess/containers/string.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

#include "logger.hh"
#include <fmt/format.h>

namespace boost_ipc = boost::interprocess;

namespace triengine::ipc
{

namespace detail
{
    using shm_segment_manager = boost_ipc::managed_shared_memory::segment_manager;

    template<typename T>
    using shm_allocator = boost_ipc::allocator<T, shm_segment_manager>;

    using shm_string = boost_ipc::basic_string<char, std::char_traits<char>, shm_allocator<char>>;

    enum class ipc_packet_type
    {
        invalid,
        handshake_request,
        handshake_response,
        heartbeat,
        notify,
        request,
        response,
        disconnect,
    };

    class received_packet
    {
    public:
        received_packet(
            std::shared_ptr<boost_ipc::managed_shared_memory> shm,
            ipc_packet_type pck_type,
            uint32_t pck_id,
            boost_ipc::managed_shared_memory::handle_t data_handle)
            : _shm{ shm }
            , _type{ pck_type }
            , _id{ pck_id }
            , _data_handle{ data_handle }
        { }

        ~received_packet() {
            if (_data_handle != 0) {
                _shm->destroy_ptr<shm_string>(
                    static_cast<shm_string*>(_shm->get_address_from_handle(_data_handle))
                );
            }
        }

        received_packet(received_packet&& other) noexcept
            : _shm(std::move(other._shm))
            , _type(other._type)
            , _id(other._id)
            , _data_handle(other._data_handle)
        {
            other._data_handle = 0;
        }

        received_packet& operator=(received_packet&& other) noexcept
        {
            if (this != &other)
            {
                if (_data_handle != 0 && _shm) {
                    _shm->destroy_ptr<shm_string>(
                        static_cast<shm_string*>(_shm->get_address_from_handle(_data_handle))
                    );
                }

                _shm = std::move(other._shm);
                _type = other._type;
                _id = other._id;
                _data_handle = other._data_handle;

                other._data_handle = 0;
            }
            return *this;
        }

        received_packet(const received_packet&) = delete;
        received_packet& operator=(const received_packet&) = delete;

        uint32_t id() const noexcept {
            return _id;
        }

        ipc_packet_type type() const noexcept {
            return _type;
        }

        std::string_view data() const
        {
            if (!_data_handle) {
                return std::string_view{};
            }

            shm_string* const body_ptr = static_cast<shm_string*>(_shm->get_address_from_handle(_data_handle));
            if (!body_ptr) {
                TEIPC_ERROR("Received packet with invalid data handle: {}", _data_handle);
                return std::string_view{};
            }

            return std::string_view{ body_ptr->data(), body_ptr->size() };
        }

    private:
        std::shared_ptr<boost_ipc::managed_shared_memory> _shm;
        ipc_packet_type _type{ ipc_packet_type::invalid };
        uint32_t _id{ 0 };
        boost_ipc::managed_shared_memory::handle_t _data_handle{ 0 };
    };

    // Handshake Request (Client -> Server)
    // Includes the name of the queue where the client will receive responses.
    struct handshake_req_mq
    {
        char server_name[64]{};
    };

    // Handshake Response (Server -> Client)
    struct handshake_rep_mq
    {
        char session_name[128]{};
    };

    /**
     * @class ipc_session_base
     * @brief A class that encapsulates Boost.Interprocess resources (shared memory, message queue).
     */
    class ipc_session_base
    {
    private:
        // Packet header for sending/receiving packets in the IPC queue (packet data pointer)
        struct packet_header_mq {
            ipc_packet_type pck_type{};
            std::uint32_t pck_id{ 0 };
            boost_ipc::managed_shared_memory::handle_t data_handle{ 0 };
        };

    public:
        enum class mode_type {
            create,
            open
        };

    public:
        ipc_session_base(
            mode_type mode, 
            std::string_view session_name,
            std::shared_ptr<boost_ipc::managed_shared_memory> shm)
            : _mode{ mode }
            , _session_name{ session_name }
            , _c2s_mq_name{ fmt::format("{}-c2s-mq", session_name) }
            , _s2c_mq_name{ fmt::format("{}-s2c-mq", session_name) }
            , _shm{ std::move(shm) }
        {
            if (mode == mode_type::create)
            {
                // Server mode: Clean up existing IPC resources and create new ones.
                boost_ipc::message_queue::remove(_c2s_mq_name.c_str());
                boost_ipc::message_queue::remove(_s2c_mq_name.c_str());

                constexpr size_t kMaxNumMessages = 1024;
                constexpr size_t kMaxMessageSize = sizeof(packet_header_mq);

                _mq_c2s = std::make_unique<boost_ipc::message_queue>(boost_ipc::create_only, _c2s_mq_name.c_str(), kMaxNumMessages, kMaxMessageSize);
                _mq_s2c = std::make_unique<boost_ipc::message_queue>(boost_ipc::create_only, _s2c_mq_name.c_str(), kMaxNumMessages, kMaxMessageSize);
            }
            else //if (mode == mode_type::open)
            {
                // Client mode: Connect to existing resources.
                _mq_c2s = std::make_unique<boost_ipc::message_queue>(boost_ipc::open_only, _c2s_mq_name.c_str());
                _mq_s2c = std::make_unique<boost_ipc::message_queue>(boost_ipc::open_only, _s2c_mq_name.c_str());
            }
        }

        ~ipc_session_base()
        {
            if (_mode == mode_type::create) {
                boost_ipc::message_queue::remove(_c2s_mq_name.c_str());
                boost_ipc::message_queue::remove(_s2c_mq_name.c_str());
            }
        }

        ipc_session_base(const ipc_session_base&) = delete;
        ipc_session_base& operator=(const ipc_session_base&) = delete;

        const std::string& get_name() const noexcept {
            return _session_name;
        }

        [[nodiscard]] boost_ipc::error_code_t send_packet(
            ipc_packet_type type,
            uint32_t id,
            const void* payload = nullptr,
            size_t payload_size = 0) noexcept
        {
            boost_ipc::error_code_t ec{ boost_ipc::no_error };

            try
            {
                packet_header_mq header;
                header.pck_type = type;
                header.pck_id = id;

                if (payload != nullptr && payload_size > 0)
                {
                    auto body_ptr_handle = this->construct<shm_string>(this->get_char_allocator());
                    auto* body_ptr = static_cast<shm_string*>(this->get_address_from_handle(body_ptr_handle));
                    body_ptr->assign(
                        static_cast<const uint8_t*>(payload),
                        static_cast<const uint8_t*>(payload) + payload_size
                    );
                    header.data_handle = body_ptr_handle;
                }

                this->get_tx_queue()
                    ->send(&header, sizeof(header), 0);

            } catch (const boost_ipc::interprocess_exception& e) {
                ec = e.get_error_code();
                TEIPC_ERROR("Failed to send packet (error: {})", e.what());
            }

            return ec;
        }

        [[nodiscard]] std::optional<received_packet> receive_packet(
            std::chrono::milliseconds timeout = 30s,
            boost_ipc::error_code_t* ec = nullptr)
        {
            if (ec) {
                *ec = boost_ipc::no_error;
            }

            try
            {
                packet_header_mq header;
                unsigned int priority;
                boost_ipc::message_queue::size_type recvd_size;
                if (!this->get_rx_queue()->timed_receive(
                    &header, sizeof(header),
                    recvd_size, 
                    priority,
                    boost::posix_time::second_clock::universal_time() + boost::posix_time::milliseconds(timeout.count())))
                {
                    if (ec) {
                        *ec = boost_ipc::error_code_t::timeout_when_waiting_error;
                    }
                    return std::nullopt;
                }

                return received_packet{
                    _shm,
                    header.pck_type,
                    header.pck_id,
                    header.data_handle
                };

            } catch (const boost_ipc::interprocess_exception& e) {
                if (ec) {
                    *ec = e.get_error_code();
                }
                TEIPC_ERROR("Failed to receive packet (error: {})", e.what());
            }

            return std::nullopt;
        }

    private:
        boost_ipc::message_queue* get_tx_queue() noexcept {
            return _mode == mode_type::create
                ? _mq_s2c.get()
                : _mq_c2s.get();
        }

        boost_ipc::message_queue* get_rx_queue() noexcept {
            return _mode == mode_type::create
                ? _mq_c2s.get()
                : _mq_s2c.get();
        }

        // Constructs an object in shared memory and returns the memory handle of the object.
        template<typename _Ty, typename... _Args>
        [[nodiscard]] boost_ipc::managed_shared_memory::handle_t construct(_Args&&... args) {
            auto* ptr = _shm->construct<_Ty>(boost_ipc::anonymous_instance)(std::forward<_Args>(args)...);
            return _shm->get_handle_from_address(ptr);
        }

        // Converts a memory handle to a memory address.
        [[nodiscard]] void* get_address_from_handle(boost_ipc::managed_shared_memory::handle_t handle) const {
            return _shm->get_address_from_handle(handle);
        }

        // Converts a memory address to a memory handle.
        [[nodiscard]] boost_ipc::managed_shared_memory::handle_t get_handle_from_address(void* ptr) const {
            return _shm->get_handle_from_address(ptr);
        }

        // Destroys an object created in shared memory.
        template<typename _Ty>
        void destroy(boost_ipc::managed_shared_memory::handle_t memory_handle) {
            if (auto* addr = this->get_address_from_handle(memory_handle)) {
                _shm->destroy_ptr(static_cast<_Ty*>(addr));
            }
        }

        shm_allocator<char> get_char_allocator() {
            return shm_allocator<char>{ _shm->get_segment_manager() };
        }

    private:
        const mode_type _mode;
        const std::string _session_name;
        const std::string _c2s_mq_name, _s2c_mq_name;
        std::shared_ptr<boost_ipc::managed_shared_memory> _shm;
        std::unique_ptr<boost_ipc::message_queue> _mq_c2s; // client -> server queue
        std::unique_ptr<boost_ipc::message_queue> _mq_s2c; // server -> client queue
    };

} // namespace

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
    std::scoped_lock lk{ _session_lock };
    _cb_notify_pck = std::move(cb);
}

void ipc_session::set_request_callback(request_packet_callback cb)
{
    std::scoped_lock lk{ _session_lock };
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
            return std::errc::not_connected;
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

    response = req_future.get();
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

        // Process the received packet...
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

///////////////////////////////////////////////////////////////////////////////////////////////////////

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

ipc_server::ipc_server()
{
}

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
    while (_is_listening)
    {
        // Reap sessions whose receive thread has finished. Erasing here, on the accept
        // thread, destroys them off their own receive thread, so ~ipc_session joins
        // that (already finished) thread cross-thread instead of self-joining. This
        // runs every accept iteration (the receive below has a 100ms timeout), so dead
        // sessions are cleaned up promptly and the session slot is freed for reconnect.
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
            continue;
        }

        std::shared_ptr<ipc_session> new_session;
        std::string new_session_name;

        {
            std::scoped_lock sessions_lk{ _ctx->sessions_mutex };
            if (_ctx->sessions.size() >= _ctx->max_sessions) {
                TEIPC_WARN("New session connection denied (maximum number of sessions reached)");
                continue;
            }

            new_session_name = fmt::format("{}-session-{:X}",
                _ctx->server_name,
                static_cast<uint32_t>(++_ctx->next_session_id)
            );

            new_session = std::make_shared<ipc_session>(
                std::make_unique<detail::ipc_session_base>(detail::ipc_session_base::mode_type::create, new_session_name, _ctx->shm),
                [weak_self = std::weak_ptr{ shared_from_this() }](std::shared_ptr<ipc_session> session)
                {
                    TEIPC_INFO("Session {} closed.", session->get_name());

                    auto self = weak_self.lock();
                    if (!self) {
                        return;
                    }

                    // Notify promptly, here on the session's receive thread, so the
                    // consumer can react immediately. The session is NOT removed from
                    // the server here; the accept loop reaps it once its receive thread
                    // has finished, so it is never destroyed on its own receive thread.
                    //
                    // Skip the notification when the server is no longer listening (an
                    // explicit stop() is tearing sessions down), matching the previous
                    // behavior.
                    const bool is_server_stopped = !self->_is_listening;
                    if (!is_server_stopped) {
                        auto session_disconn_cb = self->_on_session_disconnect;
                        if (session_disconn_cb) {
                            session_disconn_cb(session);
                        }
                    }
                });

            _ctx->sessions[new_session_name] = new_session;
        }

        TEIPC_INFO("New session accepted! (name: {})", new_session_name);

        // Instead of calling `ipc_session::start()` here, call it manually from the session connect callback.
        //new_session->start();

        if (_on_session_connect) {
            _on_session_connect(new_session);
        } else {
            TEIPC_WARN("No session connect callback set, cannot notify about new session.");
        }

        // Send a handshake response(ACK) to the client
        {
            detail::handshake_rep_mq rep_pck{};
            ::strncpy_s(
                rep_pck.session_name, 
                sizeof(rep_pck.session_name), 
                new_session_name.c_str(), 
                new_session_name.size()
            );

            _ctx->mq_handshake_s2c->send(
                &rep_pck,
                sizeof(rep_pck),
                0
            );
        }

    } // while
}

///////////////////////////////////////////////////////////////////////////////////////////////////////

struct ipc_client::context_t final
{
    std::shared_ptr<boost_ipc::managed_shared_memory> shm;
    std::shared_ptr<ipc_session> session;
};

void ipc_client::context_deleter::operator()(context_t* p) const
{
    delete p;
}

ipc_client::ipc_client()
{
}

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

    // Send a handshake request to the server and wait for a response.
    detail::handshake_req_mq handshake_req_pck{};
    std::snprintf(handshake_req_pck.server_name, sizeof(handshake_req_pck.server_name), "%s", server_name.data());
    mq_handshake_c2s->send(&handshake_req_pck, sizeof(handshake_req_pck), 0);
    detail::handshake_rep_mq handshake_rep_pck;
    unsigned int priority;
    boost_ipc::message_queue::size_type recvd_size;
    if (!mq_handshake_s2c->timed_receive(
        &handshake_rep_pck, 
        sizeof(handshake_rep_pck), 
        recvd_size, 
        priority,
        boost::posix_time::second_clock::universal_time() + boost::posix_time::milliseconds(timeout.count())))
    {
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

void ipc_client::set_notify_callback(notify_packet_callback cb) {
    std::scoped_lock ctx_lk{ _ctx_lock };
    _on_notify = std::move(cb);
    if (_ctx && _ctx->session) {
        _ctx->session->set_notify_callback(_on_notify);
    }
}

void ipc_client::set_request_callback(request_packet_callback cb) {
    std::scoped_lock ctx_lk{ _ctx_lock };
    _on_request = std::move(cb);
    if (_ctx && _ctx->session) {
        _ctx->session->set_request_callback(_on_request);
    }
}

void ipc_client::set_disconnect_callback(disconnect_callback cb) {
    std::scoped_lock ctx_lk{ _ctx_lock };
    _on_disconnect = std::move(cb);
}

std::errc ipc_client::send_notify(
    const void* const payload, 
    const size_t payload_size)
{
    std::scoped_lock ctx_lk{ _ctx_lock };
    if (!_ctx || !_ctx->session) {
        return std::errc::not_connected;
    }

    return _ctx->session->send_notify(
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
    std::scoped_lock ctx_lk{ _ctx_lock };
    if (!_ctx || !_ctx->session) {
        return std::errc::not_connected;
    }

    return _ctx->session->send_request_sync(
        payload,
        payload_size,
        response,
        timeout
    );
}

} // namespace triengine::ipc