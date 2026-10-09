#include <Veng/Behavior/BehaviorSystem.h>

#include <Veng/Behavior/BehaviorAgent.h>
#include <Veng/Diagnostics/Profiler.h>
#include <Veng/Log.h>
#include <Veng/Math/Random.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <algorithm>

namespace Veng
{
    namespace
    {
        // The agent acts through the pawn it possesses, or through itself when it possesses
        // nothing — a turret is its own body, a pilot is the mind behind a possessed vehicle.
        Entity ResolvePawn(const Scene& scene, const Entity agent)
        {
            if (const auto* possesses = scene.TryGet<Possesses>(agent);
                possesses != nullptr && !possesses->Pawn.IsNull() && scene.IsAlive(possesses->Pawn))
            {
                return possesses->Pawn;
            }
            return agent;
        }

        void AbortAgent(Scene& scene, const Entity entity, BehaviorAgent& agent, const f32 delta,
                        const SystemContext& context)
        {
            if (!agent.Tree)
            {
                return;
            }
            Rng random(agent.Seed);
            const BehaviorContext behaviorContext{
                .Scene = scene,
                .Agent = entity,
                .Pawn = ResolvePawn(scene, entity),
                .Delta = delta,
                .Tick = context.Tick,
                .Random = random,
                .System = context,
            };
            agent.Tree->Abort(agent.Slots, agent.Seed, behaviorContext);
        }
    }

    void BehaviorSystem::OnStart(Scene& scene, const SystemContext& context)
    {
        ResolveReferences(scene, 0.0f, context);
    }

    void BehaviorSystem::OnUpdate(Scene& scene, const f32 delta, const SystemContext& context)
    {
        ResolveReferences(scene, delta, context);

        // Gather agents before ticking any of them: a leaf task may spawn or destroy an entity,
        // which would invalidate a live View, so the tick walks a snapshot of the agent set.
        m_Agents.clear();
        for (auto [entity, agent] : scene.View<BehaviorAgent>())
        {
            m_Agents.push_back(entity);
        }
        VE_PROFILE_COUNTER("Behavior/Agents", static_cast<f64>(m_Agents.size()));

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

            const Entity pawn = ResolvePawn(scene, entity);

            // The slot vector mirrors the tree's node count; size it once per (agent, tree) pairing.
            if (agent.Slots.size() != agent.Tree->NodeCount())
            {
                agent.Slots.assign(agent.Tree->NodeCount(), NodeSlot{});
            }

            VE_PROFILE_SCOPE("Behavior/Agent");
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

    void BehaviorSystem::OnStop(Scene& scene, const SystemContext& context)
    {
        // An OnAbort may destroy entities, so the walk is over a snapshot, as a tick's is.
        m_Agents.clear();
        for (auto [entity, agent] : scene.View<BehaviorAgent>())
        {
            m_Agents.push_back(entity);
        }
        for (const Entity entity : m_Agents)
        {
            if (!scene.IsAlive(entity) || !HasAuthority(context, scene, entity))
            {
                continue;
            }
            if (auto* agent = scene.TryGet<BehaviorAgent>(entity))
            {
                AbortAgent(scene, entity, *agent, 0.0f, context);
            }
        }
    }

    void BehaviorSystem::ResolveReferences(Scene& scene, const f32 delta,
                                           const SystemContext& context)
    {
        // Gathered before any agent is added or removed (or an abort changes the scene), which
        // would invalidate a live View: every reference, and every authored agent whose reference
        // has gone.
        m_Resolving.clear();
        for (auto [entity, reference] : scene.View<BehaviorTreeRef>())
        {
            m_Resolving.push_back(entity);
        }
        for (auto [entity, agent] : scene.View<BehaviorAgent>())
        {
            if (agent.Source != BehaviorTreeId::Null && !scene.Has<BehaviorTreeRef>(entity))
            {
                m_Resolving.push_back(entity);
            }
        }

        for (const Entity entity : m_Resolving)
        {
            if (!scene.IsAlive(entity) || !HasAuthority(context, scene, entity))
            {
                continue;
            }

            if (auto* agent = scene.TryGet<BehaviorAgent>(entity))
            {
                const BehaviorTreeRef* reference = scene.TryGet<BehaviorTreeRef>(entity);
                const BehaviorTreeId wanted =
                    reference != nullptr ? reference->Tree : BehaviorTreeId::Null;
                if (agent->Source == BehaviorTreeId::Null || agent->Source == wanted)
                {
                    continue;
                }
                // The old run ends before the new tree's first leaf enters; a fresh agent then
                // starts the new tree from clean slots.
                AbortAgent(scene, entity, *agent, delta, context);
                if (!scene.IsAlive(entity))
                {
                    continue;
                }
                if (scene.Has<BehaviorAgent>(entity))
                {
                    (void)scene.Remove<BehaviorAgent>(entity);
                }
            }

            const BehaviorTreeRef* reference = scene.TryGet<BehaviorTreeRef>(entity);
            if (reference == nullptr)
            {
                continue;
            }
            const BehaviorTreeId id = reference->Tree;
            const u64 seed = reference->Seed;
            Ref<BehaviorTree> tree = ResolveTree(context.BehaviorTrees, id);
            if (!tree)
            {
                continue;
            }
            scene.Add<BehaviorAgent>(
                entity, BehaviorAgent{.Tree = std::move(tree), .Seed = seed, .Source = id});
        }
    }

    Ref<BehaviorTree> BehaviorSystem::ResolveTree(const BehaviorTreeRegistry& catalog,
                                                  const BehaviorTreeId id)
    {
        if (id == BehaviorTreeId::Null)
        {
            return nullptr;
        }
        Ref<BehaviorTree> tree = catalog.Resolve(id);
        if (!tree && std::ranges::find(m_LoggedUnknown, id) == m_LoggedUnknown.end())
        {
            m_LoggedUnknown.push_back(id);
            Log::Warn("BehaviorTreeRef names tree {:#018x}, which no module registered; the entity "
                      "runs no behaviour",
                      static_cast<u64>(id));
        }
        return tree;
    }
}
