#pragma once
#include <Windows.h>
#include <dxgiformat.h>
#include <triengine_interop/transport/ipc_packet.hh>
#include <vector>
#include <cstdint>

namespace triengine_interop::surface::proto
{
    // Protocol identity. Both endpoints (the surface producer and consumer) MUST agree on these.
    // The values are carried in the init handshake so a mismatch can be rejected loudly
    // instead of silently corrupting memory when the wire layout diverges between builds.
    inline constexpr uint32_t PROTO_MAGIC = 0x54564950u; // 'TVIP'
    inline constexpr uint32_t PROTO_VERSION = 2u;

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
            // Frame-rate cap requested by the consumer (0 == renderer picks a fallback).
            uint32_t requested_max_fps;
        };
        static_assert(sizeof(init_request_t) == 20);

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

        // packet_type::mouse_button_event
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

    // Consumer-side input helpers: translate Win32 key codes and build input notify packets.
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

    inline transport::packet_builder<packets::mouse_button_event_t> make_mouse_button_event(
        int32_t x,
        int32_t y,
        mouse_button_type button,
        button_action_type action,
        modifier_button_type mods)
    {
        transport::packet_builder<packets::mouse_button_event_t> pck{ static_cast<uint32_t>(packet_type::mouse_button_event) };
        pck.body()->x = x;
        pck.body()->y = y;
        pck.body()->button = button;
        pck.body()->action = action;
        pck.body()->mods = mods;
        return pck;
    }

    inline transport::packet_builder<packets::mouse_move_event_t> make_mouse_move_event(
        int32_t x,
        int32_t y,
        modifier_button_type mods)
    {
        transport::packet_builder<packets::mouse_move_event_t> pck{ static_cast<uint32_t>(packet_type::mouse_move_event) };
        pck.body()->x = x;
        pck.body()->y = y;
        pck.body()->mods = mods;
        return pck;
    }

    inline transport::packet_builder<packets::mouse_scroll_event_t> make_mouse_scroll_event(float yoffset)
    {
        transport::packet_builder<packets::mouse_scroll_event_t> pck{ static_cast<uint32_t>(packet_type::mouse_scroll_event) };
        pck.body()->yoffset = yoffset;
        return pck;
    }

} // namespace triengine_interop::surface::proto
