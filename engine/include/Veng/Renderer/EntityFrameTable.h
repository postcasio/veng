#pragma once

#include <utility>

#include <Veng/Veng.h>
#include <Veng/Scene/Entity.h>

namespace Veng::Renderer
{
    /// @brief One value per entity for one frame, stored flat by entity slot.
    ///
    /// The per-frame bookkeeping a renderer keeps per drawn entity — this frame's world matrix, a
    /// skinned entity's palette base — read back by entity on the same frame or the next. Each entry
    /// carries the generation it was written for and the frame stamp it was written under, so a
    /// recycled slot never answers for its previous owner and an entry from an earlier frame never
    /// answers at all. Nothing is cleared: Begin moves the stamp, which retires every earlier entry
    /// at once. The storage grows to the highest slot written and is reused, so a steady-state
    /// frame allocates nothing, and a lookup is an index rather than a hash.
    ///
    /// Two tables make a current/previous pair: write the current one, read the previous one, and
    /// Swap them when the frame ends. A swap moves each table's entries together with its stamp,
    /// so the invariant Find relies on — no entry carries a stamp newer than its own table's — holds
    /// across it.
    /// @tparam T The per-entity value; default-constructible and copyable.
    template <typename T>
    class EntityFrameTable
    {
    public:
        /// @brief Starts a new frame: every entry written before this call stops answering Find.
        void Begin()
        {
            ++m_Stamp;
            // A wrapped stamp could meet an entry written four billion frames ago; clearing on the
            // wrap keeps that impossible rather than merely improbable.
            if (m_Stamp == 0)
            {
                m_Entries.clear();
                m_Stamp = 1;
            }
        }

        /// @brief Records @p value for @p entity in the current frame, replacing any earlier value.
        /// @param entity The entity to record for; must not be null.
        /// @param value  The value Find returns for it until the next Begin.
        void Set(const Entity entity, const T& value)
        {
            if (entity.Index >= m_Entries.size())
            {
                m_Entries.resize(static_cast<usize>(entity.Index) + 1);
            }
            m_Entries[entity.Index] =
                Entry{.Generation = entity.Generation, .Stamp = m_Stamp, .Value = value};
        }

        /// @brief Returns @p entity's value from the current frame, or null when none was set.
        /// @param entity The entity to look up.
        /// @return The value Set recorded since the last Begin for this exact entity, or null.
        [[nodiscard]] const T* Find(const Entity entity) const
        {
            if (m_Stamp == 0 || entity.Index >= m_Entries.size())
            {
                return nullptr;
            }
            const Entry& entry = m_Entries[entity.Index];
            if (entry.Stamp != m_Stamp || entry.Generation != entity.Generation)
            {
                return nullptr;
            }
            return &entry.Value;
        }

        /// @brief Exchanges two tables' entries and stamps, without copying either.
        /// @param other The table to exchange with.
        void Swap(EntityFrameTable& other) noexcept
        {
            m_Entries.swap(other.m_Entries);
            std::swap(m_Stamp, other.m_Stamp);
        }

    private:
        /// @brief One slot's record: whose it is, which frame wrote it, and the value.
        struct Entry
        {
            /// @brief Generation of the entity the value was written for.
            u32 Generation = 0;
            /// @brief The table stamp the value was written under; 0 = never written.
            u32 Stamp = 0;
            /// @brief The recorded value.
            T Value{};
        };

        /// @brief Entries by entity slot.
        vector<Entry> m_Entries;
        /// @brief The current frame's stamp; 0 before the first Begin, which nothing answers to.
        u32 m_Stamp = 0;
    };
}
