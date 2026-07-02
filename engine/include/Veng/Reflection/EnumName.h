#pragma once

#include <Veng/Veng.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Reflection/TypeRegistry.h>

#include <cstring>
#include <span>
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

        /// @brief The one matching loop behind both the templated and runtime-typed enum-name functions.
        /// @param entries  The enum's {name, value} table, in declaration order.
        /// @param value    The value to name.
        /// @return The matching enumerator's name, or the decimal value when unmatched.
        [[nodiscard]] inline string EnumeratorNameOf(std::span<const EnumEntry> entries, i64 value)
        {
            for (const EnumEntry& entry : entries)
            {
                if (entry.Value == value)
                {
                    return entry.Name;
                }
            }
            return std::to_string(value);
        }

        /// @brief The one matching loop behind both the templated and runtime-typed enum-parse functions.
        /// @param entries  The enum's {name, value} table, in declaration order.
        /// @param name     The authored enumerator name.
        /// @return The matching enumerator's value, or nullopt when `name` matches no enumerator.
        [[nodiscard]] inline optional<i64> ParseEnumValueOf(std::span<const EnumEntry> entries,
                                                            std::string_view name)
        {
            for (const EnumEntry& entry : entries)
            {
                if (entry.Name == name)
                {
                    return entry.Value;
                }
            }
            return std::nullopt;
        }
    }

    /// @brief The authored enumerator name of a runtime-typed enum value, via the type's VE_ENUM table.
    ///
    /// The runtime-typed sibling of EnumeratorName<T>, over a TypeInfo rather than a
    /// compile-time type — what a reflection-walking serializer needs, since it holds
    /// only a TypeId at the call site. A value matching no enumerator (or a
    /// VE_LEAF-authored enum with no table) returns its raw integer in decimal, so a
    /// corrupt or unmigrated value stays readable.
    /// @param info   The enum's TypeInfo (Class == FieldClass::Enum).
    /// @param value  The value to name, widened to i64.
    /// @return The enumerator's name, or the decimal value when unmatched.
    [[nodiscard]] inline string EnumeratorName(const TypeInfo& info, i64 value)
    {
        return Detail::EnumeratorNameOf(info.Enumerators, value);
    }

    /// @brief Parses a runtime-typed enum value by its authored enumerator name.
    ///
    /// The runtime-typed sibling of ParseEnum<T>. Matching is exact and case-sensitive.
    /// @param info  The enum's TypeInfo (Class == FieldClass::Enum).
    /// @param name  The authored enumerator name.
    /// @return The value, or nullopt when `name` matches no enumerator.
    [[nodiscard]] inline optional<i64> ParseEnumValue(const TypeInfo& info, std::string_view name)
    {
        return Detail::ParseEnumValueOf(info.Enumerators, name);
    }

    /// @brief The authored enumerator name of a value, against an enumerator table read directly
    /// off a FieldDescriptor::Enumerators span — no TypeRegistry lookup.
    ///
    /// For a consumer holding a FieldDescriptor with no TypeRegistry in scope (a node-graph
    /// property walk). A value matching no enumerator (or an empty table) returns its raw
    /// integer in decimal, so a corrupt or unmigrated value stays readable.
    /// @param entries  The enum's {name, value} table, in declaration order.
    /// @param value    The value to name, widened to i64.
    /// @return The enumerator's name, or the decimal value when unmatched.
    [[nodiscard]] inline string EnumeratorName(std::span<const EnumEntry> entries, i64 value)
    {
        return Detail::EnumeratorNameOf(entries, value);
    }

    /// @brief Parses an enum value by its authored enumerator name, against a
    /// FieldDescriptor::Enumerators span — no TypeRegistry lookup.
    ///
    /// The registry-free sibling of ParseEnumValue(const TypeInfo&, …). Matching is exact
    /// and case-sensitive.
    /// @param entries  The enum's {name, value} table, in declaration order.
    /// @param name     The authored enumerator name.
    /// @return The value, or nullopt when `name` matches no enumerator.
    [[nodiscard]] inline optional<i64> ParseEnumValue(std::span<const EnumEntry> entries,
                                                      std::string_view name)
    {
        return Detail::ParseEnumValueOf(entries, name);
    }

    /// @brief Reads an enum field's backing bytes, widened to i64 per the type's size.
    ///
    /// Reads the low `info.Size` bytes at `fieldPtr` (host byte order) — the same
    /// size-aware load the reflection walkers use for an Enum leaf.
    /// @param fieldPtr  Pointer to the enum field's storage.
    /// @param info      The enum's TypeInfo, whose Size gives the backing width.
    /// @return The value widened to i64.
    [[nodiscard]] inline i64 LoadEnumBits(const void* fieldPtr, const TypeInfo& info)
    {
        i64 value = 0;
        std::memcpy(&value, fieldPtr, info.Size);
        return value;
    }

    /// @brief Writes an enum field's backing bytes from an i64 value, per the type's size.
    ///
    /// Writes the low `info.Size` bytes of `value` at `fieldPtr` (host byte order).
    /// @param fieldPtr  Pointer to the enum field's storage.
    /// @param info      The enum's TypeInfo, whose Size gives the backing width.
    /// @param value     The value to store, truncated to the field's byte width.
    inline void StoreEnumBits(void* fieldPtr, const TypeInfo& info, i64 value)
    {
        std::memcpy(fieldPtr, &value, info.Size);
    }

    /// @brief The enumerator table of a reflected enum, for attaching to a hand-authored FieldDescriptor.
    ///
    /// A registry-free alternative to a TypeRegistry lookup: an authoring site that has T at
    /// compile time (a node-graph property table, built with no TypeRegistry in scope) hands
    /// this straight into FieldDescriptor::Enumerators rather than requiring a registry.
    /// @tparam T  A VE_ENUM-reflected enum type.
    /// @return The type's {name, value} table, in declaration order. Stable for the program's lifetime.
    template <class T>
    [[nodiscard]] std::span<const EnumEntry> EnumeratorsOf()
    {
        return Detail::EnumEntriesOf<T>();
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
        const optional<i64> value = Detail::ParseEnumValueOf(Detail::EnumEntriesOf<T>(), name);
        if (!value)
        {
            return std::nullopt;
        }
        return static_cast<T>(*value);
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
        return Detail::EnumeratorNameOf(Detail::EnumEntriesOf<T>(), static_cast<i64>(value));
    }
}
