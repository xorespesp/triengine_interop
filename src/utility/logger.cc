#include <triengine_ipc/utility/logger.hh>

#include <Windows.h>
#include <fmt/format.h>

#include <string>
#include <string_view>
#include <cstdint>

namespace triengine::ipc::utility
{
    constexpr std::string_view log_level_to_string(log_level lv) noexcept {
        switch (lv) {
        case log_level::trace: return "TRACE";
        case log_level::debug: return "DEBUG";
        case log_level::info:  return "INFO";
        case log_level::warn:  return "WARN";
        case log_level::error: return "ERROR";
        default:               return "?????";
        }
    }

    void emit_log(log_level lv, const source_loc& loc, std::string_view message)
    {
        const uint32_t thread_id = static_cast<uint32_t>(::GetCurrentThreadId());
        const std::string log = fmt::format("(triengine_ipc) | {} | TID {} | {}:{} | {}\n"
            , log_level_to_string(lv)
            , thread_id
            , loc.filename()
            , loc.line
            , message
        );
        ::OutputDebugStringA(log.c_str());
    }

} // namespace triengine::ipc::utility
