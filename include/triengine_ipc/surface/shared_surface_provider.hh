#pragma once
#include <triengine_ipc/transport/ipc_service.hh>
#include <triengine_ipc/surface/surface_source.hh>

namespace triengine::ipc::surface
{
    // Server-side handshake glue for inter-process surface sharing.
    //
    // Installs a request handler on an ipc_session that serves the init and resize
    // handshake on behalf of a renderer server: it validates the protocol identity
    // (magic/version), reports the renderer process id / adapter LUID / shared
    // surface handle in the init response, and relays resize requests. The actual
    // surface production/resize is delegated to the injected surface_source, so this
    // class touches no graphics API and stays triengine-independent.
    //
    // The provider must outlive the session binding installed by attach() (the
    // request handler captures `this`). In practice a server owns both together per
    // session.
    class shared_surface_provider
    {
    public:
        explicit shared_surface_provider(surface_source src);
        ~shared_surface_provider();

        shared_surface_provider(const shared_surface_provider&) = delete;
        shared_surface_provider& operator=(const shared_surface_provider&) = delete;

        // Install the init/resize request handler onto the given session.
        void attach(ipc_session& session);

    private:
        surface_source _src;
    };

} // namespace triengine::ipc::surface
