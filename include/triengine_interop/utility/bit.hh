#pragma once
#include <type_traits>
#include <memory>
#include <cstring>

namespace triengine_interop::utility
{
    template <class _Dst, class _Src>
    static inline auto bit_cast(_Src src) noexcept
        -> std::enable_if_t<
            sizeof(_Dst) == sizeof(_Src) &&
            std::is_trivially_copyable_v<_Src> &&
            std::is_trivially_copyable_v<_Dst>
            , _Dst>
    {
        static_assert(std::is_trivially_constructible_v<_Dst>,
            "Destination type for bit_cast must be trivially constructible.");

        _Dst dst;
        std::memcpy(std::addressof(dst), std::addressof(src), sizeof(_Dst));
        return dst;
    }

} // namespace
