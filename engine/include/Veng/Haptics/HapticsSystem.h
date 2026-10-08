#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/SceneSystem.h>

namespace Veng
{
    class Scene;

    /// @brief View-phase system that plays the scene's RumbleSources: the haptics peer of AudioSystem.
    ///
    /// Each update, for each RumbleSource: a rise of Playing restarts it from the clip's start and a
    /// fall begins its fade; while it sounds its time advances by the frame delta (and its fade falls
    /// over FadeOutSeconds); its target resolves in this scene (ResolveRumbleTarget), and its clip's
    /// level at that time — scaled by Intensity and the fade — is submitted through
    /// SystemContext::Haptics as this frame's layer on that pad. A once-through clip that has ended
    /// clears Playing.
    ///
    /// It keeps no handles and has no OnStop: the scene's presentation scope does the rest. A paused
    /// world runs no View phase, so nothing advances and nothing is submitted, and it resumes from the
    /// same time; an unpresented scene's scope is Muted, so the mixer drops its layers while its
    /// sources keep time; a removed component or a destroyed entity simply stops being submitted.
    class HapticsSystem final : public SceneSystem
    {
    public:
        /// @brief Returns Phase::View — rumble is presentation, derived from the frame's state.
        [[nodiscard]] Phase GetPhase() const override { return Phase::View; }

        /// @brief Advances every RumbleSource and submits each sounding one's level.
        /// @param scene    The scene whose RumbleSources play.
        /// @param delta    Seconds since the previous View pass.
        /// @param context  Per-tick services (the scoped haptics facade and the input).
        void OnUpdate(Scene& scene, f32 delta, const SystemContext& context) override;
    };
}

VE_SYSTEM(::Veng::HapticsSystem, 0x50A74EFB72674B7DULL, "Haptics");
