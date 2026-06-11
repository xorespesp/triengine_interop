# triengine_ipc

**triengine_ipc** is a standalone C++ library that provides the inter-process
communication (IPC) transport and the rendering wire-protocol used by
[Triengine](https://github.com/xorespesp/triengine)'s inter-process rendering
(Windows only).

## Components

All symbols live under the `triengine::ipc` namespace. The library ships two
targets: a dependency-light transport/protocol core (`Triengine::ipc`) and an
optional DX11 shared-surface toolkit layered on top (`Triengine::ipc_surface`).

### Core — `Triengine::ipc`

**Transport** (`triengine::ipc`)
- `ipc_server` — accepts client sessions (renderer process side)
- `ipc_client` — connects to a server (client side)
- `ipc_session` — a connected session with notify / request-response messaging and heartbeat
- `packet_builder<T>` / `packet_view` — packet framing helpers

**Protocol** (`triengine::ipc::proto`)
- `packet_type`, `packets::*` — init / resize / mouse / keyboard packets
- input enums (`key_button_type`, `mouse_button_type`, `modifier_button_type`, ...)
- `make_mouse_*` (in `proto/input_events.hh`) — inline input-event packet builders

### Surface toolkit — `Triengine::ipc_surface`

DX11 shared-surface interop, shared by inter-process viewer clients and renderer
servers, under `triengine::ipc::surface`. It depends on the core `PUBLIC` and on
the DirectX libraries `PRIVATE`, so the core stays free of any graphics dependency.

- `shared_surface_consumer` (client) — runs the init/resize handshake, opens the
  renderer's shared keyed-mutex texture, and blits the latest frame onto a render
  target the caller owns (a swap-chain back buffer, an exported render texture, ...).
  It is present-target-agnostic, so a single implementation serves any viewer client.
- `shared_surface_provider` (server) — installs the init/resize handshake handler on
  an `ipc_session` (protocol validation + response building). The actual surface
  production/resize is delegated to an injected `surface_source`, keeping the provider
  graphics-free and agnostic to the server's threading and lifecycle.

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

These are implementation details linked `PRIVATE`: no public header exposes them, so
a consumer that links `Triengine::ipc` pulls in no extra dependency of its own. The
`Triengine::ipc_surface` target additionally links `d3d11` / `dxgi` / `d3dcompiler`
(also `PRIVATE`).

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

# Core transport/protocol only:
target_link_libraries(<your-target> PRIVATE Triengine::ipc)

# ...or, for a DX11 viewer client / renderer server, the surface toolkit
# (pulls in Triengine::ipc transitively):
target_link_libraries(<your-target> PRIVATE Triengine::ipc_surface)
```