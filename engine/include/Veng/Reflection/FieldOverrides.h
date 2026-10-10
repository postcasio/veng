#pragma once

#include <Veng/Veng.h>
#include <Veng/Assert.h>
#include <Veng/Reflection/Reflect.h>

#include <array>
#include <bit>
#include <cstddef>

namespace Veng
{
    namespace Detail
    {
        /// @brief One reflected field as an override set addresses it: its offset and a typed copy.
        struct OverridableField
        {
            /// @brief The field's byte offset in its owner (`offsetof`).
            usize Offset = 0;
            /// @brief Copy-assigns the field's value from @p source to @p destination.
            ///
            /// Both pointers address the field itself (owner base plus Offset), never the owner.
            void (*Copy)(const void* source, void* destination) = nullptr;
        };

        /// @brief A reflected struct's fields in describe order, as offsets and typed copies.
        ///
        /// A fixed array rather than a vector, so building and reading it allocates nothing.
        struct OverridableFieldTable
        {
            /// @brief The most fields one override set addresses: one bit of its mask each.
            static constexpr usize Capacity = 64;
            /// @brief The fields, in the order the type's describe block declares them.
            std::array<OverridableField, Capacity> Fields{};
            /// @brief The number of entries of Fields in use.
            usize Count = 0;
        };

        /// @brief Sink a describe block drives once to fill an OverridableFieldTable.
        ///
        /// Each callback's body depends on the field type, so it is instantiated only where a
        /// table is built.
        struct OverridableFieldCollector
        {
            /// @brief The table receiving one entry per described field.
            OverridableFieldTable& Table;

            /// @brief Appends one field, capturing a copy of its value type.
            /// @tparam Owner  The struct type declaring the field.
            /// @tparam Field  The field's value type.
            /// @param desc  The field's descriptor; only its offset is kept.
            template <class Owner, class Field>
            void Field_(const FieldDescriptor& desc)
            {
                Append<Field>(desc.Offset);
            }

            /// @brief Appends one array field, copied as a whole container.
            /// @tparam Owner    The struct type declaring the field.
            /// @tparam Element  The array's element type.
            /// @param desc  The field's descriptor; only its offset is kept.
            template <class Owner, class Element>
            void ArrayField_(const FieldDescriptor& desc)
            {
                Append<vector<Element>>(desc.Offset);
            }

            /// @brief Appends a field of type @p Field at @p offset.
            /// @tparam Field  The field's value type.
            /// @param offset  The field's byte offset in its owner.
            template <class Field>
            void Append(const usize offset)
            {
                VE_ASSERT(Table.Count < OverridableFieldTable::Capacity,
                          "FieldOverrides: a type with more than {} reflected fields",
                          OverridableFieldTable::Capacity);
                Table.Fields[Table.Count++] = OverridableField{
                    .Offset = offset,
                    .Copy = [](const void* source, void* destination)
                    { *static_cast<Field*>(destination) = *static_cast<const Field*>(source); },
                };
            }
        };
    }

    /// @brief A field-masked override set over a reflected struct: a value and a set bit per field.
    ///
    /// Holds one instance of @p T whose fields are the override values, and one bit per reflected
    /// field saying whether that field is overridden. ApplyTo writes exactly the set fields over a
    /// target and leaves every other field as it was, so an empty set changes nothing. Fields are
    /// the ones @p T's VE_REFLECT block declares, addressed by their index in that block (the order
    /// `VengReflect<T>::Fields()` lists them in, so a reflected-field UI drives a set by the index
    /// of the descriptor it drew) or by member pointer. A field @p T does not reflect cannot be
    /// overridden.
    ///
    /// Runtime state only: the set is never serialized, and the indices are positions in this
    /// build's describe block, not stable identities.
    /// @tparam T  A VE_REFLECT-ed struct with at most 64 reflected, copy-assignable fields.
    template <class T>
    class FieldOverrides
    {
    public:
        /// @brief The most fields a set can address.
        static constexpr usize MaxFields = Detail::OverridableFieldTable::Capacity;

        /// @brief Returns the number of reflected fields of @p T a set can address.
        [[nodiscard]] static usize GetFieldCount() { return GetTable().Count; }

        /// @brief Returns whether the field at @p field (its describe-block index) is overridden.
        [[nodiscard]] bool IsSet(const usize field) const
        {
            return field < MaxFields && (m_Set & Bit(field)) != 0;
        }

        /// @brief Returns whether @p member is overridden.
        /// @tparam F  The member's type.
        /// @param member  A pointer to a reflected member of @p T.
        template <class F>
        [[nodiscard]] bool IsSet(F T::* member) const
        {
            return IsSet(IndexOf(member));
        }

        /// @brief Overrides the field at @p field with that field's value in @p source.
        ///
        /// The copy-one-field form a reflected-field UI uses: it edits a whole copy of the struct
        /// and hands back the field the edit touched.
        /// @param field   The field's describe-block index; asserted in range.
        /// @param source  The struct whose value of that field becomes the override.
        void Set(const usize field, const T& source)
        {
            VE_ASSERT(field < GetFieldCount(), "FieldOverrides::Set: field {} of {}", field,
                      GetFieldCount());
            const Detail::OverridableField& entry = GetTable().Fields[field];
            entry.Copy(reinterpret_cast<const std::byte*>(&source) + entry.Offset,
                       reinterpret_cast<std::byte*>(&m_Values) + entry.Offset);
            m_Set |= Bit(field);
        }

        /// @brief Overrides @p member with @p value.
        /// @tparam F  The member's type.
        /// @param member  A pointer to a reflected member of @p T; asserted reflected.
        /// @param value   The override value.
        template <class F>
        void Set(F T::* member, const F& value)
        {
            const usize field = IndexOf(member);
            m_Values.*member = value;
            m_Set |= Bit(field);
        }

        /// @brief Drops the override of the field at @p field; a field not overridden is unchanged.
        void Clear(const usize field)
        {
            if (field < MaxFields)
            {
                m_Set &= ~Bit(field);
            }
        }

        /// @brief Drops the override of @p member.
        /// @tparam F  The member's type.
        /// @param member  A pointer to a reflected member of @p T; asserted reflected.
        template <class F>
        void Clear(F T::* member)
        {
            Clear(IndexOf(member));
        }

        /// @brief Drops every override.
        void ClearAll() { m_Set = 0; }

        /// @brief Returns whether no field is overridden.
        [[nodiscard]] bool IsEmpty() const { return m_Set == 0; }

        /// @brief Returns the number of fields overridden.
        [[nodiscard]] usize GetCount() const { return static_cast<usize>(std::popcount(m_Set)); }

        /// @brief Writes every overridden field's value over @p target, leaving the rest as they are.
        /// @param target  The struct the overrides win over.
        void ApplyTo(T& target) const
        {
            if (m_Set == 0)
            {
                return;
            }
            const Detail::OverridableFieldTable& table = GetTable();
            for (usize field = 0; field < table.Count; ++field)
            {
                if ((m_Set & Bit(field)) != 0)
                {
                    const Detail::OverridableField& entry = table.Fields[field];
                    entry.Copy(reinterpret_cast<const std::byte*>(&m_Values) + entry.Offset,
                               reinterpret_cast<std::byte*>(&target) + entry.Offset);
                }
            }
        }

        /// @brief Returns the override values; a field's value is meaningful only while it is set.
        [[nodiscard]] const T& GetValues() const { return m_Values; }

        /// @brief Returns the describe-block index of @p member.
        /// @tparam F  The member's type.
        /// @param member  A pointer to a member of @p T; asserted to be a reflected one.
        /// @return The member's index, the one IsSet/Set/Clear take.
        template <class F>
        [[nodiscard]] usize IndexOf(F T::* member) const
        {
            const auto offset =
                static_cast<usize>(reinterpret_cast<const std::byte*>(&(m_Values.*member)) -
                                   reinterpret_cast<const std::byte*>(&m_Values));
            const Detail::OverridableFieldTable& table = GetTable();
            usize field = 0;
            while (field < table.Count && table.Fields[field].Offset != offset)
            {
                ++field;
            }
            VE_ASSERT(field < table.Count,
                      "FieldOverrides: the member at offset {} is not a reflected field", offset);
            return field;
        }

    private:
        /// @brief Returns the field table of @p T, built from its describe block on first use.
        [[nodiscard]] static const Detail::OverridableFieldTable& GetTable()
        {
            static const Detail::OverridableFieldTable table = []
            {
                Detail::OverridableFieldTable built;
                Detail::OverridableFieldCollector collector{.Table = built};
                VengReflect<T>::Describe(collector);
                return built;
            }();
            return table;
        }

        /// @brief Returns the mask bit of the field at @p field.
        [[nodiscard]] static constexpr u64 Bit(const usize field) { return u64{1} << field; }

        /// @brief The override values, one per reflected field; read only where the bit is set.
        T m_Values{};

        /// @brief One bit per reflected field, set while that field is overridden.
        u64 m_Set = 0;
    };
}
