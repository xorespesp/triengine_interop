#pragma once
#include <triengine_interop/utility/bit.hh>

#include <array>
#include <cstdint>
#include <cstddef>
#include <string_view>
#include <stdexcept>
#include <type_traits>

namespace triengine_interop::transport
{
    // Fixed-size header prefixed to every packet body on the wire.
    struct packet_header_t
    {
        uint32_t type;
        size_t body_size;
    };

    template <typename _PckBody>
    class packet_builder
    {
        static_assert(std::is_standard_layout_v<_PckBody> && std::is_trivial_v<_PckBody>);

    public:
        packet_builder(uint32_t type) {
            auto* const hdr = this->header();
            hdr->type = type;
            hdr->body_size = sizeof(_PckBody);
        }

        const packet_header_t* header() const noexcept {
            return utility::bit_cast<packet_header_t*>(_pck_buff.data());
        }

        packet_header_t* header() noexcept {
            return utility::bit_cast<packet_header_t*>(_pck_buff.data());
        }

        const _PckBody* body() const noexcept {
            return utility::bit_cast<_PckBody*>(_pck_buff.data() + sizeof(packet_header_t));
        }

        _PckBody* body() noexcept {
            return utility::bit_cast<_PckBody*>(_pck_buff.data() + sizeof(packet_header_t));
        }

        const uint8_t* data() const noexcept {
            return _pck_buff.data();
        }

        size_t size() const noexcept {
            return _pck_buff.size();
        }

    private:
        std::array<uint8_t, sizeof(packet_header_t) + sizeof(_PckBody)> _pck_buff;
    };

    class packet_view
    {
    public:
        packet_view() = default;
        packet_view(const void* data, size_t size)
            : _data_view{ static_cast<const char*>(data), size }
        {
            if (!this->_validate_format()) {
                throw std::invalid_argument{ "Invalid packet format" };
            }
        }

        bool empty() const noexcept {
            return _data_view.empty();
        }

        uint32_t type() const {
            if (this->empty()) {
                throw std::logic_error{ "type() called on an empty packet_view" };
            }
            return this->_header()->type;
        }

        template <typename _PckBody>
        const _PckBody* body() const noexcept
        {
            static_assert(std::is_standard_layout_v<_PckBody> && std::is_trivial_v<_PckBody>);

            if (this->empty() || this->_header()->body_size < sizeof(_PckBody)) {
                return nullptr;
            }

            return utility::bit_cast<const _PckBody*>(_data_view.data() + sizeof(packet_header_t));
        }

        const uint8_t* data() const noexcept {
            return utility::bit_cast<const uint8_t*>(_data_view.data());
        }

        size_t size() const noexcept {
            return _data_view.size();
        }

    private:
        inline const packet_header_t* _header() const noexcept {
            return utility::bit_cast<packet_header_t*>(_data_view.data());
        }

        inline packet_header_t* _header() noexcept {
            return utility::bit_cast<packet_header_t*>(_data_view.data());
        }

        inline bool _validate_format() const noexcept
        {
            if (_data_view.empty()) {
                return false;
            }

            if (_data_view.size() < sizeof(packet_header_t)) {
                return false;
            }

            const auto header = utility::bit_cast<const packet_header_t*>(_data_view.data());
            if (_data_view.size() != header->body_size + sizeof(packet_header_t)) {
                return false;
            }

            return true;
        }

    private:
        std::string_view _data_view{};
    };

} // namespace triengine_interop::transport
