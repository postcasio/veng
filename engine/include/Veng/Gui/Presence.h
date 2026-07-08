#pragma once

#include <utility>

#include <Veng/Veng.h>
#include <Veng/Math/Ease.h>

namespace Veng::Gui
{
    /// @brief An eased open/close presence: an alpha chases a boolean goal, frame-rate-independent.
    ///
    /// A small device-free value type. `Update(goal, delta)` stores the goal and eases `GetAlpha()`
    /// toward 1 (open) or 0 (closed) through `Math::ExpApproach` at `Speed`. `IsHidden()` reports
    /// the alpha at or below `HiddenThreshold` — the point a caller stops laying the element out.
    /// `GetSlide(travel)` returns the signed enter/exit offset a caller adds to the element's
    /// placement so it slides in as it opens and out as it closes. The caller applies the alpha and
    /// the slide (`SetVisible`, `SetOpacity`, a placement offset); a `Presence` never touches a
    /// Document, so placement stays caller-owned.
    struct Presence
    {
        /// @brief The decay rate of the alpha ease, in 1/seconds (see `Math::ExpApproach`).
        f32 Speed = 12.0f;

        /// @brief The alpha at or below which `IsHidden()` reports the element hidden.
        f32 HiddenThreshold = 0.02f;

        /// @brief Stores the goal and eases the alpha toward it by one time step.
        /// @param goal   True to open (alpha eases toward 1), false to close (toward 0).
        /// @param delta  The elapsed time step, in seconds.
        void Update(const bool goal, const f32 delta)
        {
            m_Goal = goal;
            m_Alpha = ExpApproach(m_Alpha, goal ? 1.0f : 0.0f, delta, Speed);
        }

        /// @brief Returns the current eased alpha, in 0..1.
        [[nodiscard]] f32 GetAlpha() const { return m_Alpha; }

        /// @brief Returns whether the alpha has decayed to (or below) `HiddenThreshold`.
        [[nodiscard]] bool IsHidden() const { return m_Alpha <= HiddenThreshold; }

        /// @brief Returns the signed enter/exit offset for a slide-in / slide-out placement.
        ///
        /// `(goal ? +1 : -1) * (1 - alpha) * travel`: an opening presence slides in from `+travel`
        /// toward zero as the alpha rises to 1; a closing presence slides out toward `-travel` as
        /// the alpha falls. Continuous through alpha = 1, where it is exactly zero regardless of the
        /// goal.
        /// @param travel  The peak offset magnitude, in the caller's placement units.
        /// @return The signed offset to add to the element's placement this frame.
        [[nodiscard]] f32 GetSlide(const f32 travel) const
        {
            return (m_Goal ? 1.0f : -1.0f) * (1.0f - m_Alpha) * travel;
        }

    private:
        /// @brief The last goal `Update` stored; sets the slide's sign.
        bool m_Goal = false;

        /// @brief The eased alpha, in 0..1.
        f32 m_Alpha = 0.0f;
    };

    /// @brief A keyed open/close swap: closes over the stale key's content, opens on the new one.
    ///
    /// Wraps a `Presence` for a panel whose content is keyed — a panel that shows one of several
    /// interchangeable subjects picked by `Key`. When the desired key changes while the panel is
    /// open, the machine closes over the still-shown stale content, adopts the new key only once
    /// fully hidden, then reopens — so the open animation replays with fresh content rather than
    /// cross-fading. `Key` need only be equality-comparable (a game id, a small composite struct).
    ///
    /// The caller drives it per frame: `SetDesired(key)` with the live subject (nullopt to show
    /// nothing), then `Update(delta)`. It refreshes the bound content only while `GetShown()`
    /// equals the desired key — the stale sheet then holds steady under the close animation by
    /// construction. Header-only; device-free.
    /// @tparam Key  The subject key type; must be equality-comparable.
    template <typename Key>
    class KeyedPresence
    {
    public:
        /// @brief Sets the key the panel should display, or nullopt to show nothing.
        ///
        /// The desired key is compared against the shown key each `Update`; a change while open
        /// drives the close-over-stale swap. This does not itself move the presence — `Update`
        /// advances it.
        /// @param desired  The live subject key, or nullopt for nothing to show.
        void SetDesired(optional<Key> desired) { m_Desired = std::move(desired); }

        /// @brief Advances the swap machine and the embedded presence by one time step.
        ///
        /// One step of the four-state machine: with nothing desired it closes and forgets the
        /// shown key once hidden; with an empty panel it adopts the desired key and opens; showing
        /// a stale key it closes, adopting the desired key and reopening only once fully hidden;
        /// showing the desired key it opens.
        /// @param delta  The elapsed time step, in seconds.
        void Update(const f32 delta)
        {
            bool goal = false;
            if (!m_Desired.has_value())
            {
                // Nothing to show: close, and forget the shown key once hidden so a later
                // re-desire reopens from empty rather than treating it as already shown.
                goal = false;
                if (m_Presence.IsHidden())
                {
                    m_Shown.reset();
                }
            }
            else if (!m_Shown.has_value())
            {
                // Empty panel: adopt the desired key and open on it.
                m_Shown = m_Desired;
                goal = true;
            }
            else if (m_Shown == m_Desired)
            {
                // Showing the desired key: open (or hold open).
                goal = true;
            }
            else
            {
                // Showing a stale key: close over it, adopting the desired key and reopening only
                // once fully hidden — the open animation replays with fresh content.
                if (m_Presence.IsHidden())
                {
                    m_Shown = m_Desired;
                    goal = true;
                }
                else
                {
                    goal = false;
                }
            }
            m_Presence.Update(goal, delta);
        }

        /// @brief Returns the key the panel currently displays (lags desired across a swap).
        ///
        /// While a swap closes over stale content this reports the old key until fully hidden, so a
        /// caller refreshes bound content only while it equals the desired key.
        /// @return The shown key, or nullopt when the panel shows nothing.
        [[nodiscard]] const optional<Key>& GetShown() const { return m_Shown; }

        /// @brief Returns whether the panel is currently visible (alpha above the hidden threshold).
        [[nodiscard]] bool IsDisplayed() const { return !m_Presence.IsHidden(); }

        /// @brief Returns the embedded presence (read the alpha and slide, or its hidden state).
        [[nodiscard]] const Presence& GetPresence() const { return m_Presence; }

        /// @brief Returns the embedded presence for configuration (`Speed`, `HiddenThreshold`).
        [[nodiscard]] Presence& GetPresence() { return m_Presence; }

    private:
        /// @brief The eased presence the machine drives.
        Presence m_Presence;

        /// @brief The desired key set this frame, or nullopt for nothing to show.
        optional<Key> m_Desired;

        /// @brief The key currently shown; lags the desired key across a close-over-stale swap.
        optional<Key> m_Shown;
    };
}
