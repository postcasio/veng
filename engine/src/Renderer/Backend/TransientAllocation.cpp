#include <Veng/Renderer/Backend/TransientAllocation.h>

#include <Veng/Assert.h>

#include <algorithm>
#include <numeric>

namespace Veng::Renderer::Backend
{
    namespace
    {
        /// @brief Returns true iff the two live ranges share at least one pass index.
        [[nodiscard]] bool Overlaps(const TransientLifetime& a, const TransientLifetime& b)
        {
            return a.FirstUse <= b.LastUse && b.FirstUse <= a.LastUse;
        }
    }

    vector<u32> AssignTransientSlots(const vector<TransientLifetime>& lifetimes,
                                     const vector<AllocationKey>& keys)
    {
        VE_ASSERT(lifetimes.size() == keys.size(),
                  "AssignTransientSlots: lifetimes and keys must be parallel ({} vs {})",
                  lifetimes.size(), keys.size());

        // Each physical slot carries the key it was opened with and the union of
        // the lifetimes assigned to it so far. A new transient may join a slot
        // only if the key matches and its lifetime overlaps nothing already there.
        struct Slot
        {
            AllocationKey Key;
            vector<u32> Members; // indices into the input arrays
        };

        // Walk transients in FirstUse order so the greedy "first freed slot" choice
        // is the minimal one on a linear graph. Stable on ties keeps the result
        // deterministic.
        vector<u32> order(lifetimes.size());
        std::iota(order.begin(), order.end(), 0u);
        std::ranges::stable_sort(order, [&](const u32 a, const u32 b)
        {
            return lifetimes[a].FirstUse < lifetimes[b].FirstUse;
        });

        vector<Slot> slots;
        vector<u32> assignment(lifetimes.size());

        for (const u32 t : order)
        {
            u32 chosen = ~0u;
            for (u32 s = 0; s < slots.size(); s++)
            {
                if (slots[s].Key != keys[t])
                    continue;

                const bool collides = std::ranges::any_of(slots[s].Members, [&](const u32 m)
                {
                    return Overlaps(lifetimes[m], lifetimes[t]);
                });
                if (collides)
                    continue;

                chosen = s;
                break;
            }

            if (chosen == ~0u)
            {
                chosen = static_cast<u32>(slots.size());
                slots.push_back({.Key = keys[t]});
            }

            slots[chosen].Members.push_back(t);
            assignment[t] = chosen;
        }

        return assignment;
    }
}
