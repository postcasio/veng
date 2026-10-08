#pragma once

#include <Veng/Veng.h>
#include <Veng/Input.h>
#include <Veng/Scene/PresentationScope.h>

namespace Veng
{
    class Scene;

    template <typename T>
    class AssetHandle;
}

namespace Veng::Haptics
{
    class HapticsEngine;
    class RumbleClip;
    struct RumbleChannels;
    struct RumbleTarget;

    /// @brief The haptics engine as one scene's systems reach it: plays are owned by the scene's
    ///        presentation scope, and gated on a reconciliation replay.
    ///
    /// What SystemContext::Haptics is, held by value: the engine, the calling scene's presentation
    /// scope, the scene a seat target is resolved in, the input whose pads the implicit seat reads,
    /// and whether the call runs inside a replay. Bound by the context factory, so a system never
    /// names a world or a scope itself — what it plays holds while its world is paused, is silent
    /// while nothing presents it, and ends when its scene goes. Application::GetApplicationHaptics is
    /// the same facade over the application scope, for code outside every scene.
    ///
    /// An unbound facade (Unbound()) is over nothing — a Gui driver in a viewport handed no engine —
    /// and plays nothing, so a caller needs no null-guard. It has no public default, so a context that
    /// omits its facade does not compile.
    ///
    /// Cheap to copy: it borrows everything it holds, and every borrowed object outlives the context
    /// it rides on.
    class ScopedHaptics
    {
    public:
        /// @brief Returns a facade over nothing: it plays nothing and resolves every target to no pad.
        [[nodiscard]] static ScopedHaptics Unbound() { return {}; }

        /// @brief Binds the facade.
        /// @param engine    The engine plays route into.
        /// @param scope     The scope that owns everything played through this facade.
        /// @param input     The input the implicit seat resolves against.
        /// @param scene     The scene a seat target names an entity of; null for none (a seat entity
        ///                  then resolves to no pad).
        /// @param isReplay  Whether calls run inside a reconciliation replay (SystemContext::IsReplay).
        ScopedHaptics(HapticsEngine& engine, PresentationScopeId scope, const Input& input,
                      const Scene* scene, bool isReplay);

        /// @brief Plays a clip once through on a target, fire and forget.
        ///
        /// The target resolves to a pad now, in the calling scene (ResolveRumbleTarget), and the
        /// one-shot keeps that pad until it ends; rumble that must follow a seat's reassignment, or
        /// loop, is a RumbleSource. It belongs to this facade's scope, so it holds with a pause, is
        /// silent while the scene is unpresented, and stops when the scene goes. Inside a replay it
        /// starts nothing, so a Sim system re-run to re-derive predicted state never re-triggers one
        /// and needs no IsReplay gate of its own.
        /// @param target     Where it plays.
        /// @param clip       The clip; one still loading starts nothing.
        /// @param intensity  Scales every channel.
        void PlayOneShot(const RumbleTarget& target, const AssetHandle<RumbleClip>& clip,
                         f32 intensity = 1.0f) const;

        /// @brief Adds this frame's layer on a pad, owned by this facade's scope.
        ///
        /// What HapticsSystem calls for each sounding RumbleSource, and what a View system evaluating
        /// rumble of its own may call: the level lasts for the coming mix only, so a continuous
        /// effect submits every frame. Not gated on a replay — a layer lasts one frame and mixes by
        /// maximum, so a repeat changes nothing.
        /// @param pad       The pad slot.
        /// @param channels  The levels, already scaled.
        void Submit(GamepadId pad, const RumbleChannels& channels) const;

        /// @brief Resolves a target to the pad it plays on now, in this facade's scene.
        /// @param target  The target.
        /// @return The pad slot, or GamepadId::None.
        [[nodiscard]] GamepadId Resolve(const RumbleTarget& target) const;

        /// @brief Returns whether the facade is bound to an engine.
        [[nodiscard]] bool IsBound() const { return m_Engine != nullptr; }

        /// @brief Returns the engine plays route into.
        /// @pre The facade is bound (IsBound()).
        [[nodiscard]] HapticsEngine& GetEngine() const { return *m_Engine; }

        /// @brief Returns the scope that owns what this facade plays.
        [[nodiscard]] PresentationScopeId GetScope() const { return m_Scope; }

        /// @brief Whether calls run inside a reconciliation replay, so PlayOneShot starts nothing.
        [[nodiscard]] bool IsReplay() const { return m_IsReplay; }

    private:
        /// @brief The unbound facade Unbound returns.
        ScopedHaptics() = default;

        /// @brief The engine plays route into; null when unbound.
        HapticsEngine* m_Engine = nullptr;
        /// @brief The owning scope.
        PresentationScopeId m_Scope;
        /// @brief The input the implicit seat resolves against; null when unbound.
        const Input* m_Input = nullptr;
        /// @brief The scene a seat target resolves in; null for none.
        const Scene* m_Scene = nullptr;
        /// @brief Whether calls run inside a reconciliation replay.
        bool m_IsReplay = false;
    };
}
