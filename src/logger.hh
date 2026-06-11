#pragma once
#include <Windows.h>
#include <fmt/core.h>
#include <fmt/format.h>
#include <string_view>
#include <cstdint>
#include <utility>

// Internal logging for triengine_ipc.
//
// triengine_ipc has no external logging dependency: log records are formatted with
// fmt and emitted via OutputDebugStringA (viewable in DebugView / the debugger output
// window), mirroring the approach used by the Flutter interop plugin.
//
// This header is private to the library build (lives under src/, not the public
// include tree). Consumers never see it.

namespace triengine::ipc::detail
{
    enum class log_level
    {
        trace,
        debug,
        info,
        warn,
        error,
    };

    constexpr std::string_view log_level_to_string(log_level lv) noexcept
    {
        switch (lv) {
        case log_level::trace: return "TRACE";
        case log_level::debug: return "DEBUG";
        case log_level::info:  return "INFO";
        case log_level::warn:  return "WARN";
        case log_level::error: return "ERROR";
        default:               return "?????";
        }
    }

    struct source_loc
    {
        std::string_view file;
        int line{ 0 };
    };

    inline std::string_view filename_only(std::string_view path) noexcept
    {
        const auto pos = path.find_last_of("/\\");
        return (pos == std::string_view::npos) ? path : path.substr(pos + 1);
    }

    inline void emit_log(log_level lv, const source_loc& loc, std::string_view message)
    {
        const uint32_t thread_id = static_cast<uint32_t>(::GetCurrentThreadId());
        const std::string output = fmt::format("(triengine_ipc) | {} | TID {} | {}:{} | {}\n"
            , log_level_to_string(lv)
            , thread_id
            , filename_only(loc.file)
            , loc.line
            , message
        );
        ::OutputDebugStringA(output.c_str());
    }

    template <typename... Args>
    inline void log_message(
        log_level lv,
        const source_loc& loc,
        fmt::format_string<Args...> fmt_str,
        Args&&... args)
    {
        emit_log(lv, loc, fmt::format(fmt_str, std::forward<Args>(args)...));
    }

} // namespace triengine::ipc::detail

#define _TEIPC_SRC_LOC() ::triengine::ipc::detail::source_loc{ __FILE__, __LINE__ }

#define TEIPC_TRACE(...) ::triengine::ipc::detail::log_message(::triengine::ipc::detail::log_level::trace, _TEIPC_SRC_LOC(), __VA_ARGS__)
#define TEIPC_DEBUG(...) ::triengine::ipc::detail::log_message(::triengine::ipc::detail::log_level::debug, _TEIPC_SRC_LOC(), __VA_ARGS__)
#define TEIPC_INFO(...)  ::triengine::ipc::detail::log_message(::triengine::ipc::detail::log_level::info,  _TEIPC_SRC_LOC(), __VA_ARGS__)
#define TEIPC_WARN(...)  ::triengine::ipc::detail::log_message(::triengine::ipc::detail::log_level::warn,  _TEIPC_SRC_LOC(), __VA_ARGS__)
#define TEIPC_ERROR(...) ::triengine::ipc::detail::log_message(::triengine::ipc::detail::log_level::error, _TEIPC_SRC_LOC(), __VA_ARGS__)
