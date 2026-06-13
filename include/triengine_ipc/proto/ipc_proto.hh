#pragma once
#include <Windows.h>
#include <dxgiformat.h>
#include <array>
#include <vector>
#include <cstdint>
#include <string_view>
#include <stdexcept>
#include <type_traits>
#include <triengine_ipc/utility/bit.hh>

namespace triengine::ipc::proto
{
    // Protocol identity. Both peers (renderer process and client) MUST agree on these.
    // The values are carried in the init handshake so a mismatch can be rejected loudly
    // instead of silently corrupting memory when the wire layout diverges between builds.
    inline constexpr uint32_t PROTO_MAGIC = 0x54564950u; // 'TVIP'
    inline constexpr uint32_t PROTO_VERSION = 1u;

    enum button_action_type
    {
        ACTION_PRESS,
        ACTION_RELEASE,
        ACTION_REPEAT,
    };

    enum key_button_type
    {
        KEY_UNKNOWN,

        KEY_A, KEY_B, KEY_C, KEY_D, KEY_E,
        KEY_F, KEY_G, KEY_H, KEY_I, KEY_J,
        KEY_K, KEY_L, KEY_M, KEY_N, KEY_O,
        KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T,
        KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y,
        KEY_Z,

        KEY_0, KEY_1, KEY_2, KEY_3, KEY_4,
        KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,

        KEY_F1, KEY_F2, KEY_F3, KEY_F4,
        KEY_F5, KEY_F6, KEY_F7, KEY_F8,
        KEY_F9, KEY_F10, KEY_F11, KEY_F12,

        KEY_ESCAPE,
        KEY_BACK,
        KEY_RETURN,
        KEY_SPACE,
        KEY_LEFT,
        KEY_UP,
        KEY_RIGHT,
        KEY_DOWN,
        KEY_MULTIPLY,
        KEY_ADD,
        KEY_SUBTRACT,
        KEY_DIVIDE,
        // ...
    };

    enum mouse_button_type
    {
        MOUSE_L, // left mouse button
        MOUSE_R, // right mouse button
        MOUSE_M, // middle mouse button
    };

    enum modifier_button_type : uint16_t
    {
        // key button modifiers
        MOD_KEY_SHIFT = 1u << 0,
        MOD_KEY_CTRL = 1u << 1,
        MOD_KEY_ALT = 1u << 2,
        MOD_KEY_CAPSLOCK = 1u << 3,
        MOD_KEY_NUMLOCK = 1u << 4,
        // ...

        // mouse button modifiers
        MOD_MOUSE_L = 1u << 13,
        MOD_MOUSE_R = 1u << 14,
        MOD_MOUSE_M = 1u << 15,
    };

    // Add bitwise operators for modifier_button_type
    inline modifier_button_type operator|(modifier_button_type lhs, modifier_button_type rhs)
    {
        return static_cast<modifier_button_type>(static_cast<int>(lhs) | static_cast<int>(rhs));
    }

    inline modifier_button_type& operator|=(modifier_button_type& lhs, modifier_button_type rhs)
    {
        lhs = lhs | rhs;
        return lhs;
    }

    inline key_button_type translate_vkcode(DWORD vkcode)
    {
        if (vkcode >= 'A' && vkcode <= 'Z') {
            return static_cast<key_button_type>(KEY_A + (vkcode - 'A'));
        }

        if (vkcode >= '0' && vkcode <= '9') {
            return static_cast<key_button_type>(KEY_0 + (vkcode - '0'));
        }

        if (vkcode >= VK_F1 && vkcode <= VK_F12) {
            return static_cast<key_button_type>(KEY_F1 + (vkcode - VK_F1));
        }

        switch (vkcode) {
        case VK_ESCAPE: return KEY_ESCAPE;
        case VK_BACK: return KEY_BACK;
        case VK_RETURN: return KEY_RETURN;
        case VK_SPACE: return KEY_SPACE;
        case VK_LEFT: return KEY_LEFT;
        case VK_UP: return KEY_UP;
        case VK_RIGHT: return KEY_RIGHT;
        case VK_DOWN: return KEY_DOWN;
        case VK_MULTIPLY: return KEY_MULTIPLY;
        case VK_ADD:  return KEY_ADD;
        case VK_SUBTRACT: return KEY_SUBTRACT;
        case VK_DIVIDE: return KEY_DIVIDE;
        default: break;
        }

        return KEY_UNKNOWN;
    }

    enum class packet_type
    {
        invalid = 0,
        init_request,
        init_response,
        frame_resize_request,
        frame_resize_response,
        mouse_move_event,
        mouse_scroll_event,
        mouse_button_event,
    };

    struct packet_header_t
    {
        packet_type type;
        size_t body_size;
    };

    namespace packets
    {
        // Result of the init handshake, reported by the renderer in init_response_t::status.
        enum class init_status : int32_t
        {
            ok = 0,
            version_mismatch = 1, // magic/proto_version did not match PROTO_MAGIC/PROTO_VERSION
            internal_error = 2,   // renderer failed to initialize for another reason
        };

        struct init_request_t
        {
            uint32_t magic;         // must equal PROTO_MAGIC
            uint32_t proto_version; // must equal PROTO_VERSION
            int32_t frame_width;
            int32_t frame_height;
        };
        static_assert(sizeof(init_request_t) == 16);

        struct init_response_t
        {
            init_status status;
            DWORD renderer_process_id;
            LUID target_adapter_luid;
            HANDLE surface_handle; // DX11 shared texture handle (NT handle)
        };

        struct frame_resize_request_t
        {
            int32_t width;
            int32_t height;
        };
        static_assert(sizeof(frame_resize_request_t) == 8);

        struct frame_resize_response_t
        {
            HANDLE surface_handle; // DX11 shared texture handle (NT handle)
        };

        // packet_type::mouse_move_event
        struct mouse_move_event_t
        {
            // Win32 screen coordinates
            // (0, 0) is the top-left corner of the screen area.
            int32_t x, y;

            modifier_button_type mods;
        };

        struct mouse_scroll_event_t
        {
            // same as GLFW's scroll value
            // `float(GET_WHEEL_DELTA_WPARAM(wParam)) / float(WHEEL_DELTA)`
            float yoffset;
        };

        // packet_type::mouse_click_event
        struct mouse_button_event_t
        {
            // Win32 screen coordinates
            // (0, 0) is the top-left corner of the screen area.
            int32_t x, y;

            mouse_button_type button;
            button_action_type action;
            modifier_button_type mods;
        };

    } // namespace packets

} // namespace triengine::ipc::proto

namespace triengine::ipc
{
    template <typename _PckBody>
    class packet_builder
    {
        static_assert(std::is_standard_layout_v<_PckBody> && std::is_trivial_v<_PckBody>);

    public:
        packet_builder(proto::packet_type type) {
            auto* const hdr = this->header();
            hdr->type = type;
            hdr->body_size = sizeof(_PckBody);
        }

        const proto::packet_header_t* header() const noexcept {
            return utility::bit_cast<proto::packet_header_t*>(_pck_buff.data());
        }

        proto::packet_header_t* header() noexcept {
            return utility::bit_cast<proto::packet_header_t*>(_pck_buff.data());
        }

        const _PckBody* body() const noexcept {
            return utility::bit_cast<_PckBody*>(_pck_buff.data() + sizeof(proto::packet_header_t));
        }

        _PckBody* body() noexcept {
            return utility::bit_cast<_PckBody*>(_pck_buff.data() + sizeof(proto::packet_header_t));
        }

        const uint8_t* data() const noexcept {
            return _pck_buff.data();
        }

        size_t size() const noexcept {
            return _pck_buff.size();
        }

    private:
        std::array<uint8_t, sizeof(proto::packet_header_t) + sizeof(_PckBody)> _pck_buff;
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
            return !_data_view.empty();
        }

        proto::packet_type type() const noexcept {
            return this->empty()
                ? this->_header()->type
                : proto::packet_type::invalid;
        }

        template <typename _PckBody>
        const _PckBody* body() const noexcept
        {
            static_assert(std::is_standard_layout_v<_PckBody> && std::is_trivial_v<_PckBody>);

            if (!this->empty() || this->_header()->body_size < sizeof(_PckBody)) {
                return nullptr;
            }

            return utility::bit_cast<const _PckBody*>(_data_view.data() + sizeof(proto::packet_header_t));
        }

        const uint8_t* data() const noexcept {
            return utility::bit_cast<const uint8_t*>(_data_view.data());
        }

        size_t size() const noexcept {
            return _data_view.size();
        }

    private:
        inline const proto::packet_header_t* _header() const noexcept {
            return utility::bit_cast<proto::packet_header_t*>(_data_view.data());
        }

        inline proto::packet_header_t* _header() noexcept {
            return utility::bit_cast<proto::packet_header_t*>(_data_view.data());
        }

        inline bool _validate_format() const noexcept
        {
            if (_data_view.empty()) {
                return false;
            }

            if (_data_view.size() < sizeof(proto::packet_header_t)) {
                return false;
            }

            const auto header = utility::bit_cast<const proto::packet_header_t*>(_data_view.data());
            if (_data_view.size() != header->body_size + sizeof(proto::packet_header_t)) {
                return false;
            }

            return true;
        }

    private:
        std::string_view _data_view{};
    };

} // namespace triengine::ipc
