#pragma once
#include <fmt/core.h>
#include <fmt/format.h>
#include <string_view>
#include <cstdint>
#include <utility>

namespace triengine_interop::utility
{
    enum class log_level {
        trace,
        debug,
        info,
        warn,
        error,
    };

    // `std::source_location`(since C++20) like object
    class source_loc {
    public:
        std::string_view filepath;
        int line{};
        std::string_view funcname;

    public:
        constexpr source_loc() = default;

        template <std::size_t N, std::size_t M>
        constexpr source_loc(const char(&filepath_)[N], int line_, const char(&funcname_)[M])
            : filepath{ filepath_, N - 1 }
            , line{ line_ }
            , funcname{ funcname_, M - 1 }
        {}

        constexpr source_loc(
            const std::string_view filepath_,
            int line_,
            const std::string_view funcname_)
            : filepath{ filepath_ }
            , line{ line_ }
            , funcname{ funcname_ }
        {}

        constexpr bool empty() const noexcept {
            return filepath.empty();
        }

        constexpr std::string_view filename() const {
            const auto pos = filepath.find_last_of("/\\");
            return (pos != std::string_view::npos)
                ? filepath.substr(pos + 1) // split filename
                : filepath; // if no path separator is found, the whole filepath is the filename
        }
    };

    void emit_log(log_level lv, const source_loc& loc, std::string_view message);

    template <typename... Args>
    inline void log_message(
        log_level lv,
        const source_loc& loc,
        fmt::format_string<Args...> fmt_str,
        Args&&... args)
    {
        emit_log(lv, loc, fmt::format(fmt_str, std::forward<Args>(args)...));
    }

} // namespace triengine_interop::utility

#define _TEIO_SRC_LOC() ::triengine_interop::utility::source_loc{ __FILE__, __LINE__, __func__ }

#define TEIO_TRACE(...) ::triengine_interop::utility::log_message(::triengine_interop::utility::log_level::trace, _TEIO_SRC_LOC(), __VA_ARGS__)
#define TEIO_DEBUG(...) ::triengine_interop::utility::log_message(::triengine_interop::utility::log_level::debug, _TEIO_SRC_LOC(), __VA_ARGS__)
#define TEIO_INFO(...)  ::triengine_interop::utility::log_message(::triengine_interop::utility::log_level::info,  _TEIO_SRC_LOC(), __VA_ARGS__)
#define TEIO_WARN(...)  ::triengine_interop::utility::log_message(::triengine_interop::utility::log_level::warn,  _TEIO_SRC_LOC(), __VA_ARGS__)
#define TEIO_ERROR(...) ::triengine_interop::utility::log_message(::triengine_interop::utility::log_level::error, _TEIO_SRC_LOC(), __VA_ARGS__)
