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

        const bool sixWay = info.Shading == FlipbookShading::SixWay;
        VE_ASSERT(sixWay == (info.LightB != nullptr),
                  "Flipbook::Create: '{}' has a second lightmap exactly when it is six-way",
                  info.Name);
        VE_ASSERT(sixWay || info.Temperature == nullptr,
                  "Flipbook::Create: '{}' is unlit and has a temperature texture", info.Name);
        VE_ASSERT(
            (info.Motion != nullptr) == (info.MotionScale > 0.0f),
            "Flipbook::Create: '{}' has a motion sheet exactly when its MotionScale is positive",
            info.Name);
        const uvec2 extent = info.Atlas->GetExtent();
        for (const Ref<Texture>* texture : {&info.LightB, &info.Motion, &info.Temperature})
        {
            VE_ASSERT(*texture == nullptr || (*texture)->GetExtent() == extent,
                      "Flipbook::Create: '{}' has a texture whose extent is not the atlas's",
                      info.Name);
        }
        VE_ASSERT(info.Emission.Ramp.size() <= MaxFlipbookEmissionKeys,
                  "Flipbook::Create: '{}' has {} emission keys", info.Name,
                  info.Emission.Ramp.size());
        for (usize key = 1; key < info.Emission.Ramp.size(); ++key)
        {
            VE_ASSERT(info.Emission.Ramp[key].T > info.Emission.Ramp[key - 1].T,
                      "Flipbook::Create: '{}' has emission keys out of order", info.Name);
        }
        VE_ASSERT(info.FrameScale.empty() || info.FrameScale.size() == info.Clip.FrameCount,
                  "Flipbook::Create: '{}' has {} frame scales for {} frames", info.Name,
                  info.FrameScale.size(), info.Clip.FrameCount);
        for (const f32 scale : info.FrameScale)
        {
            VE_ASSERT(scale > 0.0f && scale <= 1.0f,
                      "Flipbook::Create: '{}' has a frame scale outside (0, 1]", info.Name);
        }
        return Ref<Flipbook>(new Flipbook(info));
    }

    f32 Flipbook::GetFrameScale(const u32 frame) const
    {
        if (m_Info.FrameScale.empty())
        {
            return 1.0f;
        }
        return m_Info.FrameScale[std::min<usize>(frame, m_Info.FrameScale.size() - 1)];
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
