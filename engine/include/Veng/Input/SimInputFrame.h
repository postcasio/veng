#pragma once

#include <Veng/Veng.h>
#include <Veng/InputRouter.h>

namespace Veng
{
    class Input;
    class Scene;

    /// @brief The per-frame input protocol every simulation stepped in one frame shares.
    ///
    /// The Input snapshot is one stream however many simulations a frame steps — every world the
    /// scheduler ticks. This decides, for all of them alike, what each frame does with that stream:
    ///
    /// - **Edges.** The next frame holds the pressed/released edges when something simulated and
    ///   nothing stepped, so a press and release landing between two steps is still read down by the
    ///   next one (Input::BeginFrame); a frame with nothing simulating rolls like an ordinary UI.
    /// - **Sim deltas.** Motion a step has not consumed is kept while anything simulates, however
    ///   late in the frame its steps run, and dropped once a whole frame passes with nothing
    ///   simulating (Input::DropSimDeltas), so a stopped stretch's travel never arrives as the
    ///   resuming step's look.
    /// - **The pointer.** The frame's pointer routing is scoped to one scene (SetPointer). Only that
    ///   scene's seats read the routing, and only its steps latch the pointer's motion, since the
    ///   latch consumes it; with no scene routed every step latches, so an unrouted stretch never
    ///   banks. Every step latches every pad's touchpad, the first step of the frame taking the
    ///   frame's motion.
    class SimInputFrame
    {
    public:
        /// @brief Closes the reported frame and opens the next one on the snapshot.
        ///
        /// Drops @p input's Sim deltas when no simulation reported the closing frame, then rolls its
        /// edges, or holds them when something simulated and nothing stepped. Clears the reports and
        /// the pointer scope, so each frame is judged on its own.
        /// @param input  The snapshot the frame's simulations read.
        /// @pre Called once per frame, before the frame's input events are applied.
        VE_API void BeginFrame(Input& input);

        /// @brief Scopes this frame's pointer routing to one scene.
        /// @param routing  The pointer's owner seat and region-local position.
        /// @param scene    The scene @p routing applies to, or null when it applies to none. Only
        ///                 compared by address, never dereferenced.
        /// @pre Called after BeginFrame and before the frame's first Sim step.
        VE_API void SetPointer(const PointerRouting& routing, const Scene* scene);

        /// @brief Returns this frame's pointer routing as a simulation of a scene reads it.
        /// @param scene  The scene the simulation steps.
        /// @return The routing when the frame's pointer is scoped to @p scene; otherwise an empty
        ///         routing, so none of its seats reads the pointer.
        [[nodiscard]] VE_API PointerRouting GetPointer(const Scene& scene) const;

        /// @brief Prepares the snapshot for one Sim step of a simulation of a scene.
        ///
        /// Latches every pad's touchpad motion (Input::BeginGamepadSimTick), and the pointer's
        /// (Input::BeginSimTick) when the frame's pointer is scoped to @p scene or to no scene.
        /// @param input  The snapshot the step reads.
        /// @param scene  The scene the step simulates.
        /// @pre Called once per Sim step, before the step's systems run.
        VE_API void BeginSimStep(Input& input, const Scene& scene) const;

        /// @brief Records one simulation's frame.
        /// @param stepped  Whether the simulation ran one or more Sim steps this frame.
        /// @pre Called once per frame per simulation running that frame, before the next BeginFrame;
        ///      a simulation that is stopped or paused does not report.
        VE_API void Report(bool stepped);

    private:
        /// @brief The frame's pointer routing, applying to m_PointerScene only.
        PointerRouting m_Pointer;
        /// @brief The scene the frame's pointer is scoped to, or null for none.
        const Scene* m_PointerScene = nullptr;
        /// @brief Whether any simulation reported the current frame.
        bool m_Active = false;
        /// @brief Whether any reporting simulation ran a Sim step in the current frame.
        bool m_Stepped = false;
    };
}
