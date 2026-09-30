#include <Veng/Scene/EffectPool.h>

#include <algorithm>

#include <Veng/Scene/Scene.h>

namespace Veng
{
    namespace
    {
        // Sets a component whether or not the entity already carries one.
        template <class T>
        void Put(Scene& scene, const Entity entity, const T& value)
        {
            if (T* existing = scene.TryGet<T>(entity))
            {
                *existing = value;
            }
            else
            {
                scene.Add<T>(entity, value);
            }
        }
    }

    EffectPool::EffectPool(const EffectPoolInfo& info) : m_Capacity(std::max(1u, info.Capacity))
    {
        m_Slots.reserve(m_Capacity);
    }

    Entity EffectPool::Spawn(Scene& scene, const EffectDesc& desc, const Transform& pose,
                             const f32 lifetime)
    {
        DropDead(scene);

        Slot* slot = nullptr;
        const auto free = std::ranges::find_if(m_Slots, [](const Slot& s) { return !s.Live; });
        if (free != m_Slots.end())
        {
            slot = &*free;
        }
        else if (m_Slots.size() < m_Capacity)
        {
            const Entity entity = scene.CreateEntity();
            // Local tier: an effect is each peer's own presentation and never replicates. ViewPose:
            // a reused entity resolves its new pose live rather than blending from its last one.
            scene.Add<Authority>(entity, Authority{.Tier = Tier::Local});
            scene.Add<ViewPose>(entity);
            m_Slots.push_back(Slot{.Id = entity});
            slot = &m_Slots.back();
        }
        else
        {
            slot = &*std::ranges::min_element(m_Slots, {}, &Slot::Serial);
            RetireSlot(scene, *slot);
        }

        Put(scene, slot->Id, pose);
        if (desc.Sprite)
        {
            FlipbookSprite sprite = *desc.Sprite;
            sprite.Time = 0.0f;
            sprite.Finished = false;
            Put(scene, slot->Id, sprite);
        }
        if (desc.Ribbon)
        {
            Ribbon ribbon = *desc.Ribbon;
            ribbon.Age = 0.0f;
            Put(scene, slot->Id, ribbon);
        }
        if (desc.Light)
        {
            Put(scene, slot->Id, *desc.Light);
        }

        slot->Live = true;
        slot->Age = 0.0f;
        slot->Lifetime = lifetime;
        slot->Serial = m_NextSerial++;
        return slot->Id;
    }

    void EffectPool::Update(Scene& scene, const f32 delta)
    {
        DropDead(scene);
        for (Slot& slot : m_Slots)
        {
            if (!slot.Live)
            {
                continue;
            }
            slot.Age += delta;
            const FlipbookSprite* sprite = scene.TryGet<FlipbookSprite>(slot.Id);
            const Ribbon* ribbon = scene.TryGet<Ribbon>(slot.Id);
            const bool expired = slot.Lifetime > 0.0f && slot.Age >= slot.Lifetime;
            const bool spriteDone = sprite == nullptr || sprite->Finished;
            const bool ribbonDone =
                ribbon == nullptr || (ribbon->Lifetime > 0.0f && ribbon->Age >= ribbon->Lifetime);
            if (expired || (spriteDone && ribbonDone))
            {
                RetireSlot(scene, slot);
            }
        }
    }

    void EffectPool::Retire(Scene& scene, const Entity entity)
    {
        const auto it = std::ranges::find_if(m_Slots, [entity](const Slot& s)
                                             { return s.Live && s.Id == entity; });
        if (it != m_Slots.end() && scene.IsAlive(entity))
        {
            RetireSlot(scene, *it);
        }
    }

    bool EffectPool::IsLive(const Entity entity) const
    {
        return std::ranges::any_of(m_Slots,
                                   [entity](const Slot& s) { return s.Live && s.Id == entity; });
    }

    u32 EffectPool::GetLiveCount() const
    {
        return static_cast<u32>(std::ranges::count_if(m_Slots, &Slot::Live));
    }

    u32 EffectPool::GetFreeCount() const
    {
        return static_cast<u32>(m_Slots.size()) - GetLiveCount();
    }

    void EffectPool::RetireSlot(Scene& scene, Slot& slot)
    {
        if (scene.Has<FlipbookSprite>(slot.Id))
        {
            (void)scene.Remove<FlipbookSprite>(slot.Id);
        }
        if (scene.Has<Ribbon>(slot.Id))
        {
            (void)scene.Remove<Ribbon>(slot.Id);
        }
        if (scene.Has<Light>(slot.Id))
        {
            (void)scene.Remove<Light>(slot.Id);
        }
        slot.Live = false;
    }

    void EffectPool::DropDead(const Scene& scene)
    {
        std::erase_if(m_Slots, [&scene](const Slot& s) { return !scene.IsAlive(s.Id); });
    }

    Entity SpawnTransientEffect(Scene& scene, const EffectDesc& desc, const Transform& pose,
                                const f32 lifetime)
    {
        if (scene.GetEffectPool() == nullptr)
        {
            scene.SetEffectPool(
                CreateUnique<EffectPool>(EffectPoolInfo{.Capacity = DefaultEffectPoolCapacity}));
        }
        return scene.GetEffectPool()->Spawn(scene, desc, pose, lifetime);
    }

    Entity SpawnTransientBeam(Scene& scene, const Ribbon& beam, const f32 lifetime)
    {
        Ribbon fading = beam;
        fading.Lifetime = std::max(lifetime, 0.0f);
        fading.Age = 0.0f;
        return SpawnTransientEffect(scene, EffectDesc{.Ribbon = fading},
                                    Transform{.Position = beam.From}, lifetime);
    }
}
