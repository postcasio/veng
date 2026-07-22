#pragma once

// The arithmetic a texture fill resolves to before it reaches the draw list: the fit sub-rect, the
// pixel→UV slice conversion, and the tiled UV scale. Shared by the container `background-image`
// path and the Image widget's content fill, which differ only in which box they size against.

#include <algorithm>

#include <Veng/Gui/DrawList.h>
#include <Veng/Gui/Style.h>

namespace Veng::Gui
{
    /// @brief A texture fill's destination rectangle and the UV sub-rect it samples.
    struct FittedFill
    {
        /// @brief The rectangle the quad covers, in document pixels.
        Rect Dest;
        /// @brief The normalized texture sub-rect that maps onto Dest.
        Rect Uv;
    };

    /// @brief Maps the `base` sub-rect of a texture — `source` pixels of it — into `box` under `fit`.
    ///
    /// Fill stretches the sub-rect over the box; Cover keeps the box and crops the UV; Contain and
    /// None keep the whole texel aspect and center a smaller destination inside the box (None
    /// additionally crops when the texture is larger than the box, so an intrinsic-size fill never
    /// spills). A degenerate box or source maps the whole sub-rect onto the box unchanged.
    /// @param box     The destination box the fill sizes against.
    /// @param source  The sampled region's size in texels.
    /// @param fit     How the source maps into the box.
    /// @param base    The sub-rect being fitted — the whole texture for a background fill, the
    ///                Image widget's `uv` sub-rect for an atlas frame, so a flipbook cell fits its
    ///                own cell.
    /// @return The destination rectangle and the UV sub-rect to sample.
    inline FittedFill FitTexture(const Rect& box, vec2 source, ImageFit fit,
                                 const Rect& base = {.Min = vec2(0.0f), .Size = vec2(1.0f)})
    {
        if (source.x <= 0.0f || source.y <= 0.0f || box.Size.x <= 0.0f || box.Size.y <= 0.0f)
        {
            return {.Dest = box, .Uv = base};
        }

        const auto centered = [&](vec2 size)
        { return Rect{.Min = box.Min + (box.Size - size) * 0.5f, .Size = size}; };
        const auto croppedUv = [&](vec2 visible)
        {
            const vec2 size = visible / source;
            return Rect{.Min = base.Min + (vec2(1.0f) - size) * 0.5f * base.Size,
                        .Size = size * base.Size};
        };

        switch (fit)
        {
        case ImageFit::Fill:
            return {.Dest = box, .Uv = base};
        case ImageFit::Contain:
        {
            const f32 scale = std::min(box.Size.x / source.x, box.Size.y / source.y);
            return {.Dest = centered(source * scale), .Uv = base};
        }
        case ImageFit::Cover:
        {
            const f32 scale = std::max(box.Size.x / source.x, box.Size.y / source.y);
            return {.Dest = box, .Uv = croppedUv(box.Size / scale)};
        }
        case ImageFit::None:
        {
            const vec2 drawn = glm::min(source, box.Size);
            return {.Dest = centered(drawn), .Uv = croppedUv(drawn)};
        }
        }
        return {.Dest = box, .Uv = base};
    }

    /// @brief Whether any slice edge is set, which is what turns a plain texture fill into a
    ///        nine-slice.
    /// @param slice  The authored slice insets, in source-texture pixels.
    /// @return True when at least one edge is positive.
    inline bool IsSliced(const Insets& slice)
    {
        return slice.Left > 0.0f || slice.Top > 0.0f || slice.Right > 0.0f || slice.Bottom > 0.0f;
    }

    /// @brief Converts slice insets authored in source-texture pixels into UV fractions.
    ///
    /// The nine-slice primitive takes the source split as fractions of the sampled region while the
    /// destination corners keep their pixel size, so the two forms travel together. A degenerate
    /// source axis yields a zero fraction rather than a division by zero.
    /// @param slice   The authored slice insets, in source-texture pixels.
    /// @param source  The sampled region's size in texels.
    /// @return The same edges as fractions of the sampled region.
    inline Insets SliceToUv(const Insets& slice, vec2 source)
    {
        return Insets{
            .Left = source.x > 0.0f ? slice.Left / source.x : 0.0f,
            .Top = source.y > 0.0f ? slice.Top / source.y : 0.0f,
            .Right = source.x > 0.0f ? slice.Right / source.x : 0.0f,
            .Bottom = source.y > 0.0f ? slice.Bottom / source.y : 0.0f,
        };
    }

    /// @brief The UV extent one tiled quad samples: the box measured in whole textures.
    ///
    /// Tiling is a sampler address mode, so the repeat count is arithmetic on one quad's UV rect
    /// rather than a quad per tile — a box a third of a tile wide samples a third of the texture,
    /// and a non-integral ratio ends mid-tile exactly as a wrapping sampler would.
    /// @param boxSize      The destination box's size in document pixels.
    /// @param textureSize  The texture's own size in texels.
    /// @param fallback     The UV extent used when the texture has no size to tile at.
    /// @return The UV rect size the tiled quad samples.
    inline vec2 TileUvSize(vec2 boxSize, vec2 textureSize, vec2 fallback)
    {
        if (textureSize.x <= 0.0f || textureSize.y <= 0.0f)
        {
            return fallback;
        }
        return boxSize / textureSize;
    }
}
