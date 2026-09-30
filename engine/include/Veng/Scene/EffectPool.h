#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Entity.h>

namespace Veng
{
    class Scene;

    /// @brief What a transient effect shows: a flipbook sprite, and optionally a light.
    struct EffectDesc
    {
        /// @brief The sprite the effect plays. Its Time and Finished are reset at spawn.
        FlipbookSprite Sprite;
        /// @brief A light the effect carries for its lifetime, or none.
        optional<Light> Light;
    };

    /// @brief Sizing for an EffectPool.
    struct EffectPoolInfo
    {
        /// @brief The most effects alive at once; a spawn past it recycles the oldest (at least 1).
        u32 Capacity = 64;
    };

    /// @brief The default capacity SpawnTransientEffect gives a scene's first pool.
    inline constexpr u32 DefaultEffectPoolCapacity = 64;

    /// @brief A bounded pool of short-lived, spawn-and-forget effect entities in one scene.
    ///
    /// A transient effect — an impact flash, a burst, a puff — is an entity that stands for a
    /// moment and goes, and a busy scene stands many. The pool keeps a capped set of entities and
    /// reuses them: Spawn poses one and gives it the effect's sprite and light, and Update returns
    /// it to the free list when its sprite finishes or its lifetime runs out. Past the cap, a spawn
    /// recycles the oldest live effect rather than growing, so the bound is hard.
    ///
    /// A pooled entity is Tier::Local — a per-peer presentation that never replicates — and is a
    /// root (no parent). A free entity keeps its Transform but carries no sprite and no light, so
    /// it draws and lights nothing. Because entities are reused, an Entity a caller keeps from
    /// Spawn names a later effect once its own has ended; IsLive answers whether it is still the
    /// same one only until then, so a caller wanting to follow an effect copies what it needs at
    /// spawn. An entity destroyed out from under the pool is dropped from it.
    ///
    /// A scene's own pool is installed through Scene::SetEffectPool (SpawnTransientEffect installs
    /// a default one), and FlipbookSystem calls its Update each frame.
    class EffectPool
    {
    public:
        /// @brief Constructs an empty pool.
        /// @param info  The pool's sizing.
        explicit EffectPool(const EffectPoolInfo& info = {});

        /// @brief Stands a transient effect in @p scene at @p pose.
        ///
        /// Reuses a free pooled entity when one exists, creates one while the pool is under its
        /// capacity, and otherwise recycles the oldest live effect.
        /// @param scene     The scene the pool's entities live in.
        /// @param desc      The sprite (and light) the effect shows.
        /// @param pose      The effect's world pose.
        /// @param lifetime  Seconds until the effect is retired regardless of its sprite; 0 or less
        ///                  lets the sprite alone decide, so a looping sprite with no lifetime
        ///                  lives until it is recycled or retired.
        /// @return The effect's entity.
        Entity Spawn(Scene& scene, const EffectDesc& desc, const Transform& pose, f32 lifetime);

        /// @brief Ages every live effect and retires those whose sprite finished or lifetime ended.
        /// @param scene  The scene the pool's entities live in.
        /// @param delta  Seconds since the previous update.
        void Update(Scene& scene, f32 delta);

        /// @brief Retires one live effect early, returning its entity to the free list.
        /// @param scene   The scene the pool's entities live in.
        /// @param entity  The effect to retire; a no-op when it is not a live effect of this pool.
        void Retire(Scene& scene, Entity entity);

        /// @brief Returns whether @p entity is currently a live effect of this pool.
        [[nodiscard]] bool IsLive(Entity entity) const;

        /// @brief Returns the most effects this pool keeps alive at once.
        [[nodiscard]] u32 GetCapacity() const { return m_Capacity; }

        /// @brief Returns how many effects are live.
        [[nodiscard]] u32 GetLiveCount() const;

        /// @brief Returns how many pooled entities are free for reuse.
        [[nodiscard]] u32 GetFreeCount() const;

    private:
        /// @brief One pooled entity and, while it is live, the effect it stands for.
        struct Slot
        {
            /// @brief The pooled entity.
            Entity Id = Entity::Null;
            /// @brief True while the entity stands an effect.
            bool Live = false;
            /// @brief Seconds the effect has been live.
            f32 Age = 0.0f;
            /// @brief Seconds after which the effect retires; 0 or less for none.
            f32 Lifetime = 0.0f;
            /// @brief Spawn order, so the oldest live effect is the one recycled.
            u64 Serial = 0;
        };

        /// @brief Strips a slot's sprite and light and marks it free.
        void RetireSlot(Scene& scene, Slot& slot);

        /// @brief Drops slots whose entity no longer exists in @p scene.
        void DropDead(const Scene& scene);

        /// @brief The most effects alive at once.
        u32 m_Capacity;
        /// @brief Every pooled entity, live or free.
        vector<Slot> m_Slots;
        /// @brief The next spawn's order.
        u64 m_NextSerial = 1;
    };

    /// @brief Stands a transient effect from @p scene's own effect pool, installing one if needed.
    ///
    /// The spawn-and-forget front door: the scene's pool (Scene::GetEffectPool) stands the effect,
    /// and FlipbookSystem retires it when its sprite finishes or its lifetime ends. A scene with no
    /// pool gets one of DefaultEffectPoolCapacity; install a sized one with Scene::SetEffectPool
    /// first to choose the bound. The scene's level must run FlipbookSystem for the effect to play
    /// and retire.
    /// @param scene     The scene to stand the effect in.
    /// @param desc      The sprite (and light) the effect shows.
    /// @param pose      The effect's world pose.
    /// @param lifetime  Seconds until the effect is retired regardless of its sprite; 0 or less lets
    ///                  the sprite alone decide.
    /// @return The effect's entity (see EffectPool for how long it stays this effect's).
    VE_API Entity SpawnTransientEffect(Scene& scene, const EffectDesc& desc, const Transform& pose,
                                       f32 lifetime);
}
