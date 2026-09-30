#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/SceneSystem.h>

namespace Veng
{
    class Scene;
    struct FlipbookSprite;
    struct FlipbookClip;

    /// @brief Resolves the frames per second a sprite plays its flipbook's clip at.
    ///
    /// ResolveFlipbookRate over the sprite's PlaybackRate and DurationOverride.
    /// @param sprite  The sprite.
    /// @param clip    Its flipbook's timing.
    /// @return The frames advanced per second.
    [[nodiscard]] VE_API f32 SpriteFlipbookRate(const FlipbookSprite& sprite,
                                                const FlipbookClip& clip);

    /// @brief Returns the frame a sprite shows now: FlipbookFrameAt at its Time and resolved rate.
    /// @param sprite  The sprite.
    /// @param clip    Its flipbook's timing.
    /// @return The frame index, in [0, clip.FrameCount).
    [[nodiscard]] VE_API u32 SpriteFlipbookFrame(const FlipbookSprite& sprite,
                                                 const FlipbookClip& clip);

    /// @brief View-phase system that plays every FlipbookSprite and retires the scene's finished
    ///        transient effects.
    ///
    /// Each frame it advances every sprite whose flipbook is resident by the frame delta, setting
    /// Finished once a one-shot sequence has played its last frame (a sprite whose flipbook is still
    /// loading holds at its start). It then updates the scene's EffectPool, when one is installed,
    /// which returns effects whose sprite finished or whose lifetime ran out to its free list.
    /// Presentation only: it runs in the View phase, on every peer, and writes nothing authoritative.
    class VE_API FlipbookSystem final : public SceneSystem
    {
    public:
        /// @brief Returns Phase::View — sprite playback is presentation, advanced once per frame.
        [[nodiscard]] Phase GetPhase() const override { return Phase::View; }

        /// @brief Advances every sprite and updates the scene's effect pool.
        /// @param scene    The scene whose sprites play.
        /// @param delta    Time in seconds since the previous frame.
        /// @param context  Per-tick services (unused).
        void OnUpdate(Scene& scene, f32 delta, const SystemContext& context) override;
    };
}

VE_SYSTEM(::Veng::FlipbookSystem, 0xCB4A31C2F659E7A1ULL, "Flipbook");
