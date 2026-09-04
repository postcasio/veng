#include <Veng/Behavior/BehaviorSystem.h>

#include <Veng/Behavior/BehaviorAgent.h>
#include <Veng/Math/Random.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

namespace Veng
{
    void BehaviorSystem::OnUpdate(Scene& scene, const f32 delta, const SystemContext& context)
    {
        // Gather agents before ticking any of them: a leaf task may spawn or destroy an entity,
        // which would invalidate a live View, so the tick walks a snapshot of the agent set.
        m_Agents.clear();
        for (auto [entity, agent] : scene.View<BehaviorAgent>())
        {
            m_Agents.push_back(entity);
        }

        for (const Entity entity : m_Agents)
        {
            // A prior agent's task may have destroyed this one this same tick.
            if (!scene.IsAlive(entity) || !HasAuthority(context, scene, entity))
            {
                continue;
            }

            auto& agent = scene.Get<BehaviorAgent>(entity);
            if (!agent.Tree)
            {
                continue;
            }

            // The agent acts through the pawn it possesses, or through itself when it possesses
            // nothing — a turret is its own body, a pilot is the mind behind a possessed vehicle.
            Entity pawn = entity;
            if (const Possesses* possesses = scene.TryGet<Possesses>(entity);
                possesses != nullptr && !possesses->Pawn.IsNull() && scene.IsAlive(possesses->Pawn))
            {
                pawn = possesses->Pawn;
            }

            // The slot vector mirrors the tree's node count; size it once per (agent, tree) pairing.
            if (agent.Slots.size() != agent.Tree->NodeCount())
            {
                agent.Slots.assign(agent.Tree->NodeCount(), NodeSlot{});
            }

            Rng random(agent.Seed);
            const BehaviorContext behaviorContext{
                .Scene = scene,
                .Agent = entity,
                .Pawn = pawn,
                .Delta = delta,
                .Tick = context.Tick,
                .Random = random,
                .System = context,
            };
            agent.Tree->Tick(agent.Slots, agent.Seed, behaviorContext);
        }
    }
}
