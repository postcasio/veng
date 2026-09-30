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
