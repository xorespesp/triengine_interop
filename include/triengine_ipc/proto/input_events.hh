#pragma once
#include <cstdint>
#include <triengine_ipc/proto/ipc_proto.hh>

// Inline builders for client-side input event packets.
//
// These centralize the `packet_builder` boilerplate that interactive clients
// (the ex04 viewer, the Flutter interop plugin) previously duplicated when
// forwarding mouse input to the renderer process. They are pure protocol
// helpers: they touch only packet structs, with no transport or graphics
// dependency, so they live in the header-only proto layer.

namespace triengine::ipc::proto
{
    inline packet_builder<packets::mouse_button_event_t> make_mouse_button_event(
        int32_t x,
        int32_t y,
        mouse_button_type button,
        button_action_type action,
        modifier_button_type mods)
    {
        packet_builder<packets::mouse_button_event_t> pck{ packet_type::mouse_button_event };
        pck.body()->x = x;
        pck.body()->y = y;
        pck.body()->button = button;
        pck.body()->action = action;
        pck.body()->mods = mods;
        return pck;
    }

    inline packet_builder<packets::mouse_move_event_t> make_mouse_move_event(
        int32_t x,
        int32_t y,
        modifier_button_type mods)
    {
        packet_builder<packets::mouse_move_event_t> pck{ packet_type::mouse_move_event };
        pck.body()->x = x;
        pck.body()->y = y;
        pck.body()->mods = mods;
        return pck;
    }

    inline packet_builder<packets::mouse_scroll_event_t> make_mouse_scroll_event(float yoffset)
    {
        packet_builder<packets::mouse_scroll_event_t> pck{ packet_type::mouse_scroll_event };
        pck.body()->yoffset = yoffset;
        return pck;
    }

} // namespace triengine::ipc::proto
