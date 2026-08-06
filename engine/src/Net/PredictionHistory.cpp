#include <Veng/Net/PredictionHistory.h>

#include <Veng/Assert.h>
#include <Veng/Log.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Scene.h>

#include <algorithm>
#include <new>

namespace Veng::Net
{
    namespace
    {
        // Aligned scratch storage for one type-erased component value, default-constructed through the
        // type's thunks — a value is decoded and moved onto the live component only after the decode
        // succeeds, so a malformed capture never half-writes live state (the Replication.cpp idiom).
        struct ScratchComponent
        {
            const TypeInfo& Info;
            void* Ptr;

            explicit ScratchComponent(const TypeInfo& info)
                : Info(info), Ptr(::operator new(info.Size, std::align_val_t{info.Align}))
            {
                Info.DefaultConstruct(Ptr);
            }

            ~ScratchComponent()
            {
                Info.Destruct(Ptr);
                ::operator delete(Ptr, std::align_val_t{Info.Align});
            }

            ScratchComponent(const ScratchComponent&) = delete;
            ScratchComponent& operator=(const ScratchComponent&) = delete;
        };

        // The replicated component TypeIds, sorted so a capture's per-entity component order is stable.
        vector<TypeId> ReplicatedTypeIds(const TypeRegistry& registry)
        {
            vector<TypeId> ids;
            for (const auto& [id, info] : registry.All())
            {
                if (info.Replicated)
                {
                    ids.push_back(id);
                }
            }
            std::ranges::sort(ids);
            return ids;
        }
    }

    void PredictionHistory::Track(const Entity entity)
    {
        if (std::ranges::find(m_Tracked, entity) == m_Tracked.end())
        {
            m_Tracked.push_back(entity);
        }
    }

    void PredictionHistory::Untrack(const Entity entity)
    {
        std::erase(m_Tracked, entity);
    }

    void PredictionHistory::CaptureInto(Frame& frame, const Scene& scene) const
    {
        const TypeRegistry& registry = scene.GetTypeRegistry();
        const vector<TypeId> replicated = ReplicatedTypeIds(registry);

        usize entityCount = 0;
        for (const Entity entity : m_Tracked)
        {
            if (entity.IsNull() || !scene.IsAlive(entity))
            {
                continue;
            }

            // Reuse the frame's existing slot at this index so a steady tracked set reallocates
            // nothing; its component byte buffers are cleared but keep their capacity.
            CapturedEntity& captured = entityCount < frame.Entities.size()
                                           ? frame.Entities[entityCount]
                                           : frame.Entities.emplace_back();
            captured.Entity = entity;

            usize componentCount = 0;
            for (const TypeId typeId : replicated)
            {
                const void* component = scene.TryGetComponent(entity, typeId);
                if (component == nullptr)
                {
                    continue;
                }
                CapturedComponent& slot = componentCount < captured.Components.size()
                                              ? captured.Components[componentCount]
                                              : captured.Components.emplace_back();
                slot.Type = typeId;
                slot.Bytes.clear();
                WriteFields(slot.Bytes, component, registry.Info(typeId), registry);
                ++componentCount;
            }
            captured.Components.resize(componentCount);
            ++entityCount;
        }
        frame.Entities.resize(entityCount);
    }

    void PredictionHistory::RetireOldest(usize count)
    {
        count = std::min(count, m_Inputs.size());
        if (count == 0)
        {
            return;
        }
        for (usize i = 0; i < count; ++i)
        {
            m_Pool.push_back(std::move(m_Frames[i]));
        }
        m_Inputs.erase(m_Inputs.begin(), m_Inputs.begin() + static_cast<isize>(count));
        m_Frames.erase(m_Frames.begin(), m_Frames.begin() + static_cast<isize>(count));
    }

    void PredictionHistory::Record(const u64 tick, const PlayerInput& input, const Scene& scene)
    {
        if (!m_Inputs.empty())
        {
            const u64 newest = m_Inputs.back().Tick;
            VE_ASSERT(tick >= newest,
                      "PredictionHistory: ticks must be recorded in ascending order");
            if (tick == newest)
            {
                // Re-recording the newest tick overwrites it (a re-simulated tick), in place.
                m_Inputs.back().Input = input;
                CaptureInto(m_Frames.back(), scene);
                return;
            }
        }

        Frame frame;
        if (!m_Pool.empty())
        {
            frame = std::move(m_Pool.back());
            m_Pool.pop_back();
        }
        CaptureInto(frame, scene);

        m_Inputs.push_back(StoredInput{.Tick = tick, .Input = input});
        m_Frames.push_back(std::move(frame));

        const usize capacity = std::max<usize>(m_Settings.Capacity, 1);
        if (m_Inputs.size() > capacity)
        {
            RetireOldest(m_Inputs.size() - capacity);
            Log::Warn("PredictionHistory: capacity {} exceeded, oldest tick dropped", capacity);
        }
    }

    bool PredictionHistory::Restore(const u64 tick, Scene& scene) const
    {
        const auto it = std::ranges::lower_bound(m_Inputs, tick, {}, &StoredInput::Tick);
        if (it == m_Inputs.end() || it->Tick != tick)
        {
            return false;
        }
        const Frame& frame = m_Frames[static_cast<usize>(it - m_Inputs.begin())];
        const TypeRegistry& registry = scene.GetTypeRegistry();

        for (const CapturedEntity& captured : frame.Entities)
        {
            if (captured.Entity.IsNull() || !scene.IsAlive(captured.Entity))
            {
                continue;
            }

            // Restore each captured component onto the live entity, adding an absent one.
            for (const CapturedComponent& component : captured.Components)
            {
                if (!registry.IsRegistered(component.Type))
                {
                    continue;
                }
                const TypeInfo& info = registry.Info(component.Type);
                const ScratchComponent scratch(info);
                if (const VoidResult read =
                        ReadFields(component.Bytes, scratch.Ptr, info, registry);
                    !read)
                {
                    continue;
                }
                void* dest = scene.TryGetComponent(captured.Entity, component.Type);
                if (dest == nullptr)
                {
                    dest = scene.AddComponent(captured.Entity, component.Type);
                }
                info.Destruct(dest);
                info.MoveConstruct(dest, scratch.Ptr);
            }

            // Remove any replicated component the entity holds now that the capture did not, so its
            // replicated state matches the recorded tick exactly (a component added since is undone).
            vector<TypeId> stale;
            scene.ForEachComponent(captured.Entity,
                                   [&](const TypeId id, void*)
                                   {
                                       if (!registry.Info(id).Replicated)
                                       {
                                           return;
                                       }
                                       const bool kept = std::ranges::any_of(
                                           captured.Components, [id](const CapturedComponent& c)
                                           { return c.Type == id; });
                                       if (!kept)
                                       {
                                           stale.push_back(id);
                                       }
                                   });
            for (const TypeId id : stale)
            {
                (void)scene.RemoveComponent(captured.Entity, id);
            }
        }
        return true;
    }

    std::span<const u8> PredictionHistory::Captured(const u64 tick, const Entity entity,
                                                    const TypeId type) const
    {
        const auto it = std::ranges::lower_bound(m_Inputs, tick, {}, &StoredInput::Tick);
        if (it == m_Inputs.end() || it->Tick != tick)
        {
            return {};
        }
        const Frame& frame = m_Frames[static_cast<usize>(it - m_Inputs.begin())];
        for (const CapturedEntity& captured : frame.Entities)
        {
            if (captured.Entity != entity)
            {
                continue;
            }
            for (const CapturedComponent& component : captured.Components)
            {
                if (component.Type == type)
                {
                    return component.Bytes;
                }
            }
            return {};
        }
        return {};
    }

    std::span<const StoredInput> PredictionHistory::InputsAfter(const u64 tick) const
    {
        const auto it = std::ranges::upper_bound(m_Inputs, tick, {}, &StoredInput::Tick);
        return std::span<const StoredInput>(it, m_Inputs.end());
    }

    void PredictionHistory::TrimThrough(const u64 tick)
    {
        const auto it = std::ranges::upper_bound(m_Inputs, tick, {}, &StoredInput::Tick);
        RetireOldest(static_cast<usize>(it - m_Inputs.begin()));
    }

    void PredictionHistory::Clear()
    {
        RetireOldest(m_Inputs.size());
    }

    bool PredictionHistory::Contains(const u64 tick) const
    {
        const auto it = std::ranges::lower_bound(m_Inputs, tick, {}, &StoredInput::Tick);
        return it != m_Inputs.end() && it->Tick == tick;
    }

    u64 PredictionHistory::OldestTick() const
    {
        return m_Inputs.empty() ? 0 : m_Inputs.front().Tick;
    }

    u64 PredictionHistory::NewestTick() const
    {
        return m_Inputs.empty() ? 0 : m_Inputs.back().Tick;
    }
}

namespace Veng
{
    vector<Entity> DefaultPredictedEntities(const Scene& scene, const Entity pawn)
    {
        vector<Entity> set;
        if (pawn.IsNull() || !scene.IsAlive(pawn))
        {
            return set;
        }

        vector<TypeId> replicated;
        for (const auto& [id, info] : scene.GetTypeRegistry().All())
        {
            if (info.Replicated)
            {
                replicated.push_back(id);
            }
        }
        const auto carriesReplicated = [&](const Entity entity)
        {
            for (const TypeId id : replicated)
            {
                if (scene.TryGetComponent(entity, id) != nullptr)
                {
                    return true;
                }
            }
            return false;
        };

        // The pawn always predicts; a descendant joins only when it carries replicated state.
        set.push_back(pawn);
        vector<Entity> stack;
        scene.ForEachChild(pawn, [&](const Entity child) { stack.push_back(child); });
        while (!stack.empty())
        {
            const Entity entity = stack.back();
            stack.pop_back();
            if (scene.IsAlive(entity) && carriesReplicated(entity))
            {
                set.push_back(entity);
            }
            scene.ForEachChild(entity, [&](const Entity child) { stack.push_back(child); });
        }
        return set;
    }
}
