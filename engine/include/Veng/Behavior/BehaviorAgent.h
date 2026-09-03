#pragma once

#include <Veng/Veng.h>
#include <Veng/Behavior/BehaviorTree.h>
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
    /// serialised and never replicated. A spawner adds it and assigns the tree and seed.
    struct BehaviorAgent
    {
        /// @brief The immutable behaviour tree this agent runs; a null tree makes the agent inert.
        Ref<BehaviorTree> Tree;
        /// @brief This agent's per-node running state, sized to the tree's node count by the system.
        vector<NodeSlot> Slots;
        /// @brief The agent's seed for its per-node random streams.
        u64 Seed = 0;
    };
}

VE_TYPE(::Veng::BehaviorAgent, 0xFA3CCE660F9896C4ULL);
