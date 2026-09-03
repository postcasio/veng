#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/SceneSystem.h>

namespace Veng
{
    /// @brief Ticks every BehaviorAgent's tree each Sim step, with the ECS as the blackboard.
    ///
    /// The AI arm of the control pipeline: for each agent it has authority over, it resolves the
    /// pawn the agent acts through — the agent's Possesses target, or the agent itself when it has
    /// none, so an agent can be its own body or the mind behind a possessed one — and ticks the
    /// agent's tree, handing each leaf the scene, the agent, the pawn, the step, and the node's
    /// seeded random stream. A leaf that writes the pawn's Intent thus drives it through the same
    /// MovementSystem a player's control system feeds, which is why this registers in the same
    /// ordering slot a control system takes: after InputMappingSystem and before MovementSystem.
    ///
    /// It skips an agent failing the authority filter, exactly as the other authoritative Sim
    /// advancers do, so a client never ticks a tree for an entity it only mirrors. On a
    /// reconciliation replay the tree still ticks — intent is re-derived as prediction re-runs
    /// control — and a leaf with an *external* side effect guards it on `SystemContext::IsReplay`
    /// itself. Agents are gathered into a member vector before any is ticked, so a task that spawns
    /// or destroys an entity mid-tick does not invalidate the iteration.
    class BehaviorSystem final : public SceneSystem
    {
    public:
        /// @brief Ticks every authoritative agent's tree, resolving each agent's pawn first.
        /// @param scene    The scene whose agents are ticked.
        /// @param delta    Seconds since the previous tick.
        /// @param context  Per-tick services carrying authority, replay, and debug state.
        void OnUpdate(Scene& scene, f32 delta, const SystemContext& context) override;

    private:
        /// @brief Agents gathered before ticking, so a task's structural changes are safe.
        vector<Entity> m_Agents;
    };
}

VE_SYSTEM(::Veng::BehaviorSystem, 0x3F53D670D1990B6EULL, "Behavior");
