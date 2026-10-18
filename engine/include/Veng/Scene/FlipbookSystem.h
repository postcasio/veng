#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/Flipbook.h>
#include <Veng/Scene/SceneSystem.h>

namespace Veng
{
    class Scene;
    struct FlipbookSprite;

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

    /// @brief Returns where a sprite's playback stands between two frames now:
    ///        FlipbookFramePositionAt at its Time and resolved rate.
    /// @param sprite  The sprite.
    /// @param clip    Its flipbook's timing.
    /// @return The frame shown, the next, and the fraction between them.
    [[nodiscard]] VE_API FlipbookFramePosition
    SpriteFlipbookFramePosition(const FlipbookSprite& sprite, const FlipbookClip& clip);

    /// @brief How a sprite's flipbook is drawn, resolved from the sprite's choices and the asset.
    struct SpriteShading
    {
        /// @brief The blend between frames: never Motion without a motion sheet.
        FlipbookFrameBlend FrameBlend = FlipbookFrameBlend::Step;
        /// @brief The world depth the sprite fades over into opaque geometry; 0 is no fade.
        f32 SoftDistance = 0.0f;
    };

    /// @brief Resolves a sprite's frame blend and soft-edge distance. Pure and device-free.
    ///
    /// The frame blend is the sprite's own, or the flipbook's recommendation for
    /// SpriteFrameBlend::Asset; Motion on a flipbook with no motion sheet becomes Crossfade. A
    /// negative SoftDistance takes the default — half the quad's width for a six-way flipbook,
    /// which is how far a round puff of that width reaches in front of its centre plane, and no
    /// fade otherwise — and a positive one is scaled by the entity's world scale, as Size is. So a
    /// sprite on a stepped unlit flipbook resolves Step and no fade unless it opts in.
    /// @param sprite      The sprite.
    /// @param shading     Its flipbook's shading.
    /// @param recommended Its flipbook's recommended frame blend.
    /// @param hasMotion   Whether its flipbook carries a motion sheet.
    /// @param quadWidth   The quad's world width, the entity's scale applied.
    /// @param worldScale  The entity's world scale.
    /// @return The resolved blend and fade distance.
    [[nodiscard]] VE_API SpriteShading ResolveSpriteShading(const FlipbookSprite& sprite,
                                                            FlipbookShading shading,
                                                            FlipbookFrameBlend recommended,
                                                            bool hasMotion, f32 quadWidth,
                                                            f32 worldScale);

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
