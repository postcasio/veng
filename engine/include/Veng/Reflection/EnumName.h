#pragma once

#include <Veng/Veng.h>
#include <Veng/Reflection/Reflect.h>

#include <string>
#include <string_view>

namespace Veng
{
    namespace Detail
    {
        /// @brief The cached enumerator table of a reflected enum, built once per type.
        /// @tparam T  A VE_ENUM-reflected enum type.
        /// @return The type's EnumEntry list, in declaration order.
        template <class T>
        const vector<EnumEntry>& EnumEntriesOf()
        {
            static const vector<EnumEntry> entries = VengReflect<T>::Enumerators();
            return entries;
        }
    }

    /// @brief Parses an enum value by its authored enumerator name (e.g. "Keyboard").
    ///
    /// Matches against the VE_ENUM enumerator table for T — the authored C++
    /// spellings, so JSON authors an enum by name (never ordinal) and a rename in
    /// C++ is a rebuild, not a silent shift. Matching is exact and case-sensitive.
    /// @tparam T  A VE_ENUM-reflected enum type.
    /// @param name  The authored enumerator name.
    /// @return The value, or nullopt when `name` matches no enumerator.
    template <class T>
    [[nodiscard]] optional<T> ParseEnum(std::string_view name)
    {
        for (const EnumEntry& entry : Detail::EnumEntriesOf<T>())
        {
            if (entry.Name == name)
            {
                return static_cast<T>(entry.Value);
            }
        }
        return std::nullopt;
    }

    /// @brief The authored enumerator name of an enum value, through the VE_ENUM table for T.
    ///
    /// The inverse of ParseEnum. A value matching no enumerator returns its raw
    /// integer in decimal, so a serialized out-of-range value stays readable.
    /// @tparam T  A VE_ENUM-reflected enum type.
    /// @param value  The value to name.
    /// @return The enumerator's name, or the decimal value when unmatched.
    template <class T>
    [[nodiscard]] string EnumeratorName(T value)
    {
        const i64 raw = static_cast<i64>(value);
        for (const EnumEntry& entry : Detail::EnumEntriesOf<T>())
        {
            if (entry.Value == raw)
            {
                return entry.Name;
            }
        }
        return std::to_string(raw);
    }
}
