#include <Veng/Scene/FlipbookSystem.h>

#include <Veng/Asset/Flipbook.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/EffectPool.h>
#include <Veng/Scene/Scene.h>

namespace Veng
{
    f32 SpriteFlipbookRate(const FlipbookSprite& sprite, const FlipbookClip& clip)
    {
        return ResolveFlipbookRate(clip, sprite.PlaybackRate, sprite.DurationOverride);
    }

    u32 SpriteFlipbookFrame(const FlipbookSprite& sprite, const FlipbookClip& clip)
    {
        return FlipbookFrameAt(clip, SpriteFlipbookRate(sprite, clip), sprite.Time);
    }

    FlipbookFramePosition SpriteFlipbookFramePosition(const FlipbookSprite& sprite,
                                                      const FlipbookClip& clip)
    {
        return FlipbookFramePositionAt(clip, SpriteFlipbookRate(sprite, clip), sprite.Time);
    }

    SpriteShading ResolveSpriteShading(const FlipbookSprite& sprite, const FlipbookShading shading,
                                       const FlipbookFrameBlend recommended, const bool hasMotion,
                                       const f32 quadWidth, const f32 worldScale)
    {
        SpriteShading resolved{.FrameBlend = recommended};
        switch (sprite.FrameBlend)
        {
        case SpriteFrameBlend::Asset:
            break;
        case SpriteFrameBlend::Step:
            resolved.FrameBlend = FlipbookFrameBlend::Step;
            break;
        case SpriteFrameBlend::Crossfade:
            resolved.FrameBlend = FlipbookFrameBlend::Crossfade;
            break;
        case SpriteFrameBlend::Motion:
            resolved.FrameBlend = FlipbookFrameBlend::Motion;
            break;
        }
        if (resolved.FrameBlend == FlipbookFrameBlend::Motion && !hasMotion)
        {
            resolved.FrameBlend = FlipbookFrameBlend::Crossfade;
        }

        if (sprite.SoftDistance < 0.0f)
        {
            resolved.SoftDistance = shading == FlipbookShading::SixWay ? 0.5f * quadWidth : 0.0f;
        }
        else
        {
            resolved.SoftDistance = sprite.SoftDistance * worldScale;
        }
        return resolved;
    }

    void FlipbookSystem::OnUpdate(Scene& scene, const f32 delta, const SystemContext& /*context*/)
    {
        for (auto [entity, sprite] : scene.View<FlipbookSprite>())
        {
            const Flipbook* flipbook = sprite.Flipbook.Get();
            if (flipbook == nullptr || sprite.Finished)
            {
                continue;
            }
            sprite.Time += delta;
            const FlipbookClip& clip = flipbook->GetClip();
            sprite.Finished =
                IsFlipbookFinished(clip, SpriteFlipbookRate(sprite, clip), sprite.Time);
        }

        if (EffectPool* pool = scene.GetEffectPool())
        {
            pool->Update(scene, delta);
        }
    }
}
