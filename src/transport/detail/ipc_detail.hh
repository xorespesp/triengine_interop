#pragma once

#include <optional>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <cstdint>
#include <cstddef>
#include <random>

#include <boost/interprocess/ipc/message_queue.hpp>
#include <boost/interprocess/managed_shared_memory.hpp>
#include <boost/interprocess/allocators/allocator.hpp>
#include <boost/interprocess/containers/string.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

#include <triengine_ipc/utility/logger.hh>
#include <fmt/format.h>

namespace boost_ipc = boost::interprocess;

namespace triengine::ipc
{
    using namespace std::chrono_literals;

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
    struct handshake_req_mq
    {
        uint64_t client_nonce{ 0 };       // unique per connect attempt; correlates the reply
        int64_t  deadline_unix_ns{ 0 };   // wall-clock (system_clock) expiry of the attempt
        char     server_name[64]{};       // name of the queue where the client will receive responses.
    };

    // Handshake Response (Server -> Client)
    struct handshake_rep_mq
    {
        uint64_t client_nonce{ 0 };       // echoes the request nonce
        int64_t  deadline_unix_ns{ 0 };   // echoes the request deadline (for stale filtering)
        char     session_name[128]{};
    };

    // Wall-clock nanoseconds since epoch. The handshake reply queue is shared by all
    // clients, so a deadline carried in a packet must be comparable across processes;
    // system_clock reads the shared OS wall clock (unlike steady_clock, whose epoch is
    // per-process).
    inline int64_t system_now_unix_ns()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // A random 64-bit value identifying a single handshake attempt.
    inline uint64_t make_handshake_nonce()
    {
        std::random_device rd;
        return (static_cast<uint64_t>(rd()) << 32) ^ static_cast<uint64_t>(rd());
    }

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

} // namespace detail

} // namespace triengine::ipc
