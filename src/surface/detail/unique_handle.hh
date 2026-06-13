#pragma once
#include <Windows.h>
#include <memory>

// Minimal RAII wrapper for a Win32 NT handle, kept dependency-free so the surface
// toolkit owns its handle lifetime without pulling in any external utility library.

namespace triengine_interop::surface::detail
{
    struct handle_deleter
    {
        void operator()(HANDLE handle) const noexcept
        {
            if (handle) {
                ::CloseHandle(handle);
            }
        }
    };

    using unique_handle = std::unique_ptr<std::remove_pointer_t<HANDLE>, handle_deleter>;

} // namespace
