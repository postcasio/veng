#pragma once

#include <Veng/Veng.h>
#include <Veng/Input/Actions.h>
#include <Veng/Input/InputConsumer.h>
#include <Veng/Input/SeatRef.h>

#include <span>

// Input/RoleResolver.h — the engine-internal per-frame resolution of role-tagged actions.
//
// A role action is an application-declared action the engine itself acts on (ActionRole): the
// navigation roles drive Gui focus. The resolver resolves every seat's role actions once per frame,
// whatever the seat's focus, so a press is a Started edge exactly once however long it is held and
// whatever focus changes it is held across; only the dispatch is gated on focus. It reads no
// Sim-resolved PlayerInput: a paused world does not step, a frame may run zero or several steps,
// and a seatless viewport has no seat to resolve.

namespace Veng
{
    class Input;
    class InputMappingContext;
    class InputRouter;
    class Scene;
    class WorldRunner;
    struct PointerRouting;

    /// @brief One press of a role action a seat's resolution raised this frame.
    struct RoleFire
    {
        /// @brief The role that fired.
        ActionRole Role = ActionRole::None;
        /// @brief Whether this is a held action's repeat rather than its press.
        bool Repeat = false;
    };

    /// @brief What one frame's role resolution reads, borrowed for the Update call.
    struct RoleFrameInfo
    {
        /// @brief The frame-coherent input snapshot every seat's view reads.
        const Input& Snapshot;
        /// @brief The router: focus, claimed keys, the cursor seat, and the consumer dispatch.
        InputRouter& Router;
        /// @brief The worlds whose locally-owned seats resolve, paused or not.
        const WorldRunner& Worlds;
        /// @brief The default UI context beneath every non-exclusive seat and alone on the implicit
        ///        seat; null when the application has none, so nothing navigates by default.
        const InputMappingContext* DefaultUi = nullptr;
        /// @brief This frame's pointer routing, which applies in PointerScene only.
        const PointerRouting& Pointer;
        /// @brief The scene the pointer routing belongs to, or null when no scene owns it.
        const Scene* PointerScene = nullptr;
        /// @brief This frame's delta in seconds, which advances the repeat timers.
        f32 Delta = 0.0f;
    };

    /// @brief Resolves every seat's role actions once per frame and dispatches their presses.
    ///
    /// Keeps, per seat, the ActionState it resolved last frame — the phase source, as a seat's
    /// PlayerInput is for the Sim resolve — and one repeat timer per held repeating action. A seat
    /// not resolved in a frame is forgotten, so a seat that leaves is not carried.
    ///
    /// Which contexts a seat resolves: its own InputContextStack's resident contexts (focus-gated as
    /// InputMappingSystem gates them) over the default UI context, which sits lowest so a seat
    /// context re-binding a role action's id shadows its controls. Beneath a stack a SeatFocusScope
    /// marked Exclusive the default fires nothing — it is resolved only so its presses keep their
    /// phase across the takeover, and one held through it is not new when it ends. The implicit seat resolves the default UI context alone, against
    /// every device, and drives only documents on viewports bound to no seat. Every navigation role
    /// a seat fires is dispatched through the router only when that seat held UI focus as the frame's
    /// dispatch began — the implicit seat taking the cursor seat's focus, as the window events it
    /// reads do.
    class RoleResolver
    {
    public:
        /// @brief Resolves every seat for the frame, then dispatches the presses focus admits.
        /// @param frame  What the frame reads; borrowed for the call.
        void Update(const RoleFrameInfo& frame);

        /// @brief Resolves one seat's role actions and appends the presses it raises this frame.
        ///
        /// Phase comes from the seat's previous resolution, so a press fires once on its Started
        /// edge; a held action with a RepeatRate fires again once held RepeatDelay (or one
        /// RepeatRate when the delay is 0) and then every RepeatRate, at most once per frame. A role
        /// fires at most once per seat per call, however many of its actions pressed. An action
        /// that leaves the seat's contexts while held is remembered as held, so it does not press
        /// again if it returns while still held. Marks the seat seen for RetireUnseen.
        /// @param seat         The seat resolved, which keys its state.
        /// @param contexts     The seat's contexts, lowest priority first.
        /// @param trackedOnly  How many leading contexts are resolved for phase alone: an action they
        ///                     declare fires only when a later context declares it too.
        /// @param raw          The seat's frame-rate view.
        /// @param delta        The frame delta in seconds.
        /// @param fires        Receives the presses, in the seat's stack-declared action order.
        void ResolveSeat(SeatRef seat, std::span<const ResolvedContext> contexts, usize trackedOnly,
                         const RawInputView& raw, f32 delta, vector<RoleFire>& fires);

        /// @brief Forgets every seat ResolveSeat did not see since the previous call.
        void RetireUnseen();

    private:
        /// @brief One held repeating action's timer.
        struct RepeatTimer
        {
            /// @brief The action timed.
            ActionId Action = ActionId::Null;
            /// @brief Seconds held since the press.
            f32 Held = 0.0f;
            /// @brief The held time at which the next repeat fires.
            f32 NextRepeat = 0.0f;
        };

        /// @brief One seat's carried resolution state.
        struct SeatState
        {
            /// @brief The ActionState last resolved, plus any held action that left the contexts.
            ActionState Previous;
            /// @brief The repeat timers of the seat's held repeating actions.
            vector<RepeatTimer> Timers;
            /// @brief Whether ResolveSeat saw the seat since the last RetireUnseen.
            bool Seen = false;
        };

        /// @brief The carried state of every seat resolved recently.
        unordered_map<SeatRef, SeatState> m_Seats;

        /// @brief Reused per seat: its contexts, lowest priority first.
        vector<ResolvedContext> m_Contexts;

        /// @brief Reused per seat: the presses its resolution raised.
        vector<RoleFire> m_Fires;

        /// @brief Reused per frame: the role presses awaiting dispatch.
        vector<RoleEvent> m_Pending;
    };

    /// @brief Reads the modifier keys a view reports held, either side counting.
    /// @param raw  The seat's view.
    /// @return The held modifiers.
    [[nodiscard]] ModifierKeys ReadModifierKeys(const RawInputView& raw);
}
