#pragma once

#include <Veng/Veng.h>
#include <Veng/Behavior/BehaviorTreeRegistry.h>
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
    ///
    /// Authored references are resolved at start and ahead of each tick's gather, against
    /// `SystemContext::BehaviorTrees`: an entity carrying a BehaviorTreeRef and no agent gets one
    /// running the resolved tree; an agent resolved from a reference whose tree id has changed is
    /// aborted and given the new tree; and one whose reference is gone is aborted and removed. An
    /// agent built in code (BehaviorAgent::Source Null) is never touched by this step. At stop,
    /// every agent with a run in progress is aborted, so a run ends in exactly one of OnExit or
    /// OnAbort however the simulation ends.
    class BehaviorSystem final : public SceneSystem
    {
    public:
        /// @brief Gives each authored BehaviorTreeRef its agent before the first tick.
        /// @param scene    The scene starting.
        /// @param context  The start context, carrying authority and the tree catalog.
        void OnStart(Scene& scene, const SystemContext& context) override;

        /// @brief Ticks every authoritative agent's tree, resolving each agent's pawn first.
        ///
        /// Brings authored agents in step with their references first, so a reference added,
        /// retargeted or removed takes effect this tick.
        /// @param scene    The scene whose agents are ticked.
        /// @param delta    Seconds since the previous tick.
        /// @param context  Per-tick services carrying authority, replay, and debug state.
        void OnUpdate(Scene& scene, f32 delta, const SystemContext& context) override;

        /// @brief Aborts every authoritative agent's run in progress as the simulation ends.
        ///
        /// Each aborted leaf gets OnAbort with the stop context's tick and a zero delta, while the
        /// scene and every service are still live. The agents themselves stay on their entities.
        /// @param scene    The scene stopping.
        /// @param context  The stop context.
        void OnStop(Scene& scene, const SystemContext& context) override;

    private:
        /// @brief Adds, retargets and removes authored agents to match their references.
        /// @param scene    The scene whose references are resolved.
        /// @param delta    The delta the leaves an abort reaches see.
        /// @param context  The context carrying authority and the tree catalog.
        void ResolveReferences(Scene& scene, f32 delta, const SystemContext& context);

        /// @brief Resolves @p id, logging once per id that no tree claims.
        /// @param catalog  The tree catalog.
        /// @param id       The referenced tree.
        /// @return The tree, or null for Null or an unclaimed id.
        Ref<BehaviorTree> ResolveTree(const BehaviorTreeRegistry& catalog, BehaviorTreeId id);

        /// @brief Agents gathered before ticking, so a task's structural changes are safe.
        vector<Entity> m_Agents;
        /// @brief Entities gathered by the resolve step before it adds or removes any agent.
        vector<Entity> m_Resolving;
        /// @brief Unclaimed tree ids already logged, so each warns once.
        vector<BehaviorTreeId> m_LoggedUnknown;
    };
}

VE_SYSTEM(::Veng::BehaviorSystem, 0x3F53D670D1990B6EULL, "Behavior");
