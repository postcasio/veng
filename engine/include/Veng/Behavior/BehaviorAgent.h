#pragma once

#include <Veng/Veng.h>
#include <Veng/Behavior/BehaviorTree.h>
#include <Veng/Behavior/BehaviorTreeRegistry.h>
#include <Veng/Reflection/Reflect.h>

namespace Veng
{
    /// @brief Gives an entity a behaviour: a shared tree, this agent's running state, and a seed.
    ///
    /// An agent is an entity carrying this component. BehaviorSystem ticks its @ref Tree each Sim
    /// step, resolving the pawn the agent acts through, handing each leaf the ECS as its blackboard.
    /// @ref Slots is this agent's per-node running state — sized to the tree's node count on the
    /// first tick — so many agents share one immutable tree without sharing state. @ref Seed makes
    /// the agent reproducible: each node's random stream is seeded from it and the node's index, so
    /// a WaitRandom or a chance-taking task replays identically and two agents with the same seed
    /// make the same random choices.
    ///
    /// It is runtime-only (VE_TYPE): a behaviour is re-decided from world state on any peer that has
    /// authority, and a client mirror has no business ticking one, so the component is never
    /// serialised and never replicated. Either a spawner adds it and assigns the tree and seed, or
    /// BehaviorSystem adds it from an authored BehaviorTreeRef, recording the id in @ref Source.
    struct BehaviorAgent
    {
        /// @brief The immutable behaviour tree this agent runs; a null tree makes the agent inert.
        Ref<BehaviorTree> Tree;
        /// @brief This agent's per-node running state, sized to the tree's node count by the system.
        vector<NodeSlot> Slots;
        /// @brief The agent's seed for its per-node random streams.
        u64 Seed = 0;
        /// @brief The catalog id the agent's tree was resolved from; Null for a code-built agent.
        ///
        /// BehaviorSystem keeps an agent with a valid Source in step with its entity's
        /// BehaviorTreeRef, and never touches one whose Source is Null.
        BehaviorTreeId Source = BehaviorTreeId::Null;
    };

    /// @brief Names the registered behaviour tree an entity runs: the authored half of an agent.
    ///
    /// Carried by a prefab or a level entity, it selects a tree a module registered in the
    /// BehaviorTreeRegistry by id, so placing an entity is enough to give it a behaviour. On a peer
    /// with authority, BehaviorSystem gives the entity a BehaviorAgent running the resolved tree with
    /// @ref Seed (at start and on the first step it sees the reference), swaps the agent's tree when
    /// @ref Tree changes — aborting the running one first — and aborts and removes the agent when the
    /// reference goes. An id no tree claims gives the entity no agent. @ref Seed is read when the
    /// tree is resolved; editing it alone does not restart a running agent.
    struct BehaviorTreeRef
    {
        /// @brief The registered tree the entity runs; Null runs none.
        BehaviorTreeId Tree = BehaviorTreeId::Null;
        /// @brief The agent's seed for its per-node random streams.
        u64 Seed = 0;
    };
}

VE_TYPE(::Veng::BehaviorAgent, 0xFA3CCE660F9896C4ULL);

VE_REFLECT(::Veng::BehaviorTreeRef, 0x0A395649ED5835B4ULL)
VE_FIELD(Tree, .DisplayName = "Tree", .Tooltip = "The registered behaviour tree the entity runs")
VE_FIELD(Seed, .DisplayName = "Seed", .Tooltip = "Seeds the agent's per-node random streams")
VE_REFLECT_END();
