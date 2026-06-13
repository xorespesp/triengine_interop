# triengine_interop

Standalone C++ library providing the inter-process transport and rendering
wire-protocol behind [Triengine](https://github.com/xorespesp/triengine)'s
inter-process rendering (Windows only). All symbols live under `triengine_interop`.

It ships two CMake targets.

## `Triengine::interop` — transport core

Dependency-light core: Boost is linked `PRIVATE` (bundled, transport-internal), `fmt`
is `PUBLIC` (exposed by the logging utility), and there is no graphics dependency. The
consumer contract is "link `Triengine::interop` (`fmt` comes transitively); Boost and the
DirectX libraries are bundled".

- **transport** (`triengine_interop::transport`) — `ipc_server` / `ipc_client` / `ipc_session`
  carry notify and request-response messages over shared memory; the payload-agnostic
  `packet_header_t` / `packet_builder` / `packet_view` frame those bytes as a generic
  header-plus-body packet.

## `Triengine::interop_surface` — DX11 shared-surface toolkit

Layered on the core (adds `d3d11` / `dxgi` / `d3dcompiler`, all `PRIVATE`, so the core
stays graphics-free). Under `triengine_interop::surface`.

- **protocol** (`triengine_interop::surface::proto`) — the wire packets (init / resize /
  mouse), the input enums, and the `make_mouse_*` packet builders. These define the
  surface-sharing payloads carried in the transport's generic packet body.
- **`shared_surface_client`** (viewer side) — connects, runs the handshake, opens the
  renderer's shared texture, and blits each frame onto a render target the caller owns.
- **`shared_surface_server`** (renderer side) — owns an `ipc_server` and serves a
  single client (one shared scene, one viewer): the surface handshake plus decoded
  input events. The application implements `shared_surface_server::session_interface`
  and passes it to `start()`.

## Consuming via CMake

```cmake
include(FetchContent)
FetchContent_Declare(triengine_interop
    GIT_REPOSITORY https://github.com/xorespesp/triengine_interop.git
    GIT_TAG <commit-or-tag>)
FetchContent_MakeAvailable(triengine_interop)

target_link_libraries(<your-target> PRIVATE Triengine::interop)          # core only
target_link_libraries(<your-target> PRIVATE Triengine::interop_surface)  # + DX11 toolkit
```
