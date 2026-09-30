#include <Veng/Asset/Flipbook.h>

#include <algorithm>
#include <cmath>

#include <Veng/Assert.h>
#include <Veng/Asset/Texture.h>

namespace Veng
{
    f32 ResolveFlipbookRate(const FlipbookClip& clip, const f32 playbackRate,
                            const f32 durationOverride)
    {
        if (durationOverride > 0.0f)
        {
            return static_cast<f32>(clip.FrameCount) / durationOverride;
        }
        return std::max(0.0f, clip.Fps * playbackRate);
    }

    u32 FlipbookFrameAt(const FlipbookClip& clip, const f32 rate, const f32 time)
    {
        const u32 frames = std::max(1u, clip.FrameCount);
        const f64 position = static_cast<f64>(std::max(0.0f, time)) * static_cast<f64>(rate);
        if (!std::isfinite(position))
        {
            return clip.Loop ? 0u : frames - 1;
        }
        const auto index = static_cast<u64>(std::floor(position));
        if (clip.Loop)
        {
            return static_cast<u32>(index % frames);
        }
        return static_cast<u32>(std::min<u64>(index, frames - 1));
    }

    bool IsFlipbookFinished(const FlipbookClip& clip, const f32 rate, const f32 time)
    {
        if (clip.Loop || rate <= 0.0f)
        {
            return false;
        }
        return static_cast<f64>(time) * static_cast<f64>(rate) >=
               static_cast<f64>(std::max(1u, clip.FrameCount));
    }

    Ref<Flipbook> Flipbook::Create(const FlipbookInfo& info)
    {
        VE_ASSERT(info.Atlas != nullptr, "Flipbook::Create: '{}' has no atlas texture", info.Name);
        VE_ASSERT(info.Columns > 0 && info.Rows > 0, "Flipbook::Create: '{}' has an empty grid",
                  info.Name);
        VE_ASSERT(info.Clip.FrameCount > 0 && static_cast<u64>(info.Clip.FrameCount) <=
                                                  static_cast<u64>(info.Columns) * info.Rows,
                  "Flipbook::Create: '{}' has {} frames for a {}x{} grid", info.Name,
                  info.Clip.FrameCount, info.Columns, info.Rows);
        VE_ASSERT(info.FrameSize.x > 0 && info.FrameSize.y > 0,
                  "Flipbook::Create: '{}' has an empty frame", info.Name);
        return Ref<Flipbook>(new Flipbook(info));
    }

    f32 Flipbook::GetAspect() const
    {
        return static_cast<f32>(m_Info.FrameSize.y) / static_cast<f32>(m_Info.FrameSize.x);
    }

    vec4 Flipbook::GetFrameRect(const u32 frame) const
    {
        const u32 clamped = std::min(frame, m_Info.Clip.FrameCount - 1);
        const u32 column = clamped % m_Info.Columns;
        const u32 row = clamped / m_Info.Columns;

        const vec2 tile(1.0f / static_cast<f32>(m_Info.Columns),
                        1.0f / static_cast<f32>(m_Info.Rows));
        const vec2 halfTexel(0.5f / static_cast<f32>(m_Info.Columns * m_Info.FrameSize.x),
                             0.5f / static_cast<f32>(m_Info.Rows * m_Info.FrameSize.y));
        const vec2 origin = vec2(static_cast<f32>(column), static_cast<f32>(row)) * tile;
        return {origin + halfTexel, origin + tile - halfTexel};
    }

    vec2 Flipbook::GetAnchor() const
    {
        if (!m_Info.Pivot || !m_Info.WorldExtent)
        {
            return {0.5f, 0.5f};
        }
        const vec3& extent = *m_Info.WorldExtent;
        const vec3& pivot = *m_Info.Pivot;
        const f32 x = extent.x > 0.0f ? pivot.x / extent.x : 0.5f;
        const f32 y = extent.y > 0.0f ? pivot.y / extent.y : 0.5f;
        return {x, y};
    }
}
