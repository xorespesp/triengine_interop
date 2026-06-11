# triengine_ipc

**triengine_ipc** is a standalone C++ library that provides the inter-process
communication (IPC) transport and the rendering wire-protocol used by
[Triengine](https://github.com/xorespesp/triengine)'s inter-process rendering
(Windows only).

## Components

All symbols live under the `triengine::ipc` namespace.

**Transport** (`triengine::ipc`)
- `ipc_server` — accepts client sessions (renderer process side)
- `ipc_client` — connects to a server (client side)
- `ipc_session` — a connected session with notify / request-response messaging and heartbeat
- `packet_builder<T>` / `packet_view` — packet framing helpers

**Protocol** (`triengine::ipc::proto`)
- `packet_type`, `packets::*` — init / resize / mouse / keyboard packets
- input enums (`key_button_type`, `mouse_button_type`, `modifier_button_type`, ...)

## Protocol versioning

The init handshake carries a magic value and a protocol version:

```cpp
namespace triengine::ipc::proto {
    inline constexpr uint32_t PROTO_MAGIC   = 0x54564950u; // 'TVIP'
    inline constexpr uint32_t PROTO_VERSION = 1;
}
```

`init_request_t` includes `magic` and `proto_version`; the renderer validates them
and reports the outcome in `init_response_t::status` (`init_status::version_mismatch`
on mismatch). Bump `PROTO_VERSION` whenever the wire layout changes so peers built
from different versions fail loudly instead of corrupting memory.

Each packet struct is guarded with a `static_assert(sizeof(...) == N)` so an
accidental layout change is caught at compile time.

## Dependencies

- [Boost.Interprocess](https://www.boost.org/) + Boost.DateTime (message queues, shared memory)
- [fmt](https://github.com/fmtlib/fmt) (log message formatting)
- **Windows only** (the protocol uses `HANDLE` / `LUID` / `DXGI_FORMAT` for DX11 surface sharing)

Both are implementation details linked `PRIVATE`: no public header exposes them, so
a consumer that links `Triengine::ipc` pulls in no extra dependency of its own.

## Consuming via CMake (FetchContent)

```cmake
include(FetchContent)
FetchContent_Declare(
    triengine_ipc
    GIT_REPOSITORY https://github.com/xorespesp/triengine_ipc.git
    GIT_TAG <commit-or-tag>
    GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(triengine_ipc)

target_link_libraries(<your-target> PRIVATE Triengine::ipc) # Boost + fmt come transitively (final link only)
```