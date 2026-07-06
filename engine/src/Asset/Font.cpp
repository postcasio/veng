#include <Veng/Asset/Font.h>

#include <algorithm>
#include <cstddef>

#include <Veng/Asset/Texture.h>

namespace Veng
{
    Font::~Font() = default;

    const FontGlyph* Font::GetGlyph(u32 codepoint) const
    {
        const auto it = m_Glyphs.find(codepoint);
        return it != m_Glyphs.end() ? &it->second : nullptr;
    }

    f32 Font::GetKerning(u32 left, u32 right) const
    {
        const auto it = m_Kerning.find({left, right});
        return it != m_Kerning.end() ? it->second : 0.0f;
    }

    Renderer::TextureHandle Font::GetAtlasHandle() const
    {
        return m_Atlas->GetHandle();
    }

    Renderer::SamplerHandle Font::GetAtlasSamplerHandle() const
    {
        return m_Atlas->GetSamplerHandle();
    }

    ShapeResult Font::ShapeRun(std::span<const u32> codepoints, f32 pixelSize,
                               optional<f32> maxWidth) const
    {
        ShapeResult result;

        // The em-normalized metrics scale to pixels by the requested size; y grows downward, so the
        // baseline sits at the ascender height and each new line steps down by the line height.
        const f32 lineStep = m_LineHeight * pixelSize;
        const f32 firstBaseline = m_Ascender * pixelSize;

        // A pending line accumulates its glyphs before it is committed to result.Lines — the width
        // wrap decides retroactively where a line ends, so a line is only finalized once broken.
        struct PendingGlyph
        {
            u32 Codepoint;
            f32 PenX; // pen origin x, in pixels, before this glyph's quad offset
            FontGlyph Metrics;
        };

        vector<PendingGlyph> lineGlyphs;
        f32 penX = 0.0f;
        f32 baseline = firstBaseline;
        f32 maxLineWidth = 0.0f;

        // Emits the accumulated line's visible quads into the result and records its ShapedLine,
        // then resets the pen for the next line. `advanceBaseline` steps to the next baseline unless
        // this is the trailing flush of the final line.
        const auto commitLine = [&](bool advanceBaseline)
        {
            const u32 start = static_cast<u32>(result.Glyphs.size());
            for (const PendingGlyph& pending : lineGlyphs)
            {
                const FontGlyph& glyph = pending.Metrics;
                // A whitespace/zero-geometry glyph advances the pen but emits no quad.
                if (glyph.PlaneMax.x <= glyph.PlaneMin.x || glyph.PlaneMax.y <= glyph.PlaneMin.y)
                {
                    continue;
                }
                // Plane bounds are baseline-relative with y up; convert to run-origin pixels with y
                // down: the quad top is baseline - PlaneMax.y, the bottom baseline - PlaneMin.y.
                ShapedGlyph shaped;
                shaped.Codepoint = pending.Codepoint;
                shaped.Min = {pending.PenX + glyph.PlaneMin.x * pixelSize,
                              baseline - glyph.PlaneMax.y * pixelSize};
                shaped.Max = {pending.PenX + glyph.PlaneMax.x * pixelSize,
                              baseline - glyph.PlaneMin.y * pixelSize};
                shaped.UvMin = glyph.UvMin;
                shaped.UvMax = glyph.UvMax;
                result.Glyphs.push_back(shaped);
            }

            const f32 lineWidth = penX;
            maxLineWidth = std::max(maxLineWidth, lineWidth);
            result.Lines.push_back(ShapedLine{
                .Start = start,
                .Count = static_cast<u32>(result.Glyphs.size()) - start,
                .Width = lineWidth,
                .Baseline = baseline,
            });

            lineGlyphs.clear();
            penX = 0.0f;
            if (advanceBaseline)
            {
                baseline += lineStep;
            }
        };

        // Moves the trailing word (glyphs after the last space on the current line) to a fresh line,
        // re-flowing the pen — the word-wrap break. Returns false when the line has no earlier space
        // to break at (a single word wider than the constraint hard-breaks in the caller instead).
        const auto wrapTrailingWord = [&]() -> bool
        {
            usize wordStart = lineGlyphs.size();
            while (wordStart > 0 && lineGlyphs[wordStart - 1].Codepoint != ' ')
            {
                wordStart--;
            }
            if (wordStart == 0)
            {
                return false;
            }

            vector<PendingGlyph> carried(
                lineGlyphs.begin() + static_cast<std::ptrdiff_t>(wordStart), lineGlyphs.end());
            lineGlyphs.resize(wordStart);
            commitLine(true);

            // Re-lay the carried word from the new pen origin, re-applying kerning within it.
            for (usize i = 0; i < carried.size(); i++)
            {
                PendingGlyph next = carried[i];
                if (i > 0)
                {
                    penX += GetKerning(carried[i - 1].Codepoint, next.Codepoint) * pixelSize;
                }
                next.PenX = penX;
                penX += next.Metrics.Advance * pixelSize;
                lineGlyphs.push_back(next);
            }
            return true;
        };

        u32 previous = 0;
        bool havePrevious = false;
        for (const u32 codepoint : codepoints)
        {
            if (codepoint == '\n')
            {
                commitLine(true);
                havePrevious = false;
                continue;
            }

            const FontGlyph* glyph = GetGlyph(codepoint);
            if (glyph == nullptr)
            {
                // An uncooked codepoint contributes nothing — skip it without breaking kerning
                // across the gap is intentional: the missing glyph has no advance to apply.
                continue;
            }

            if (havePrevious)
            {
                penX += GetKerning(previous, codepoint) * pixelSize;
            }

            PendingGlyph pending{.Codepoint = codepoint, .PenX = penX, .Metrics = *glyph};
            penX += glyph->Advance * pixelSize;
            lineGlyphs.push_back(pending);
            previous = codepoint;
            havePrevious = true;

            if (maxWidth && penX > *maxWidth && lineGlyphs.size() > 1)
            {
                // The line overflowed: wrap the trailing word, or hard-break before this glyph when
                // the word itself is wider than the constraint.
                if (!wrapTrailingWord())
                {
                    lineGlyphs.pop_back();
                    commitLine(true);
                    pending.PenX = 0.0f;
                    penX = pending.Metrics.Advance * pixelSize;
                    lineGlyphs.push_back(pending);
                }
                previous = lineGlyphs.empty() ? 0 : lineGlyphs.back().Codepoint;
                havePrevious = !lineGlyphs.empty();
            }
        }

        commitLine(false);

        const f32 blockHeight = result.Lines.empty()
                                    ? 0.0f
                                    : (static_cast<f32>(result.Lines.size() - 1) * lineStep +
                                       (m_Ascender - m_Descender) * pixelSize);
        result.Size = {maxLineWidth, blockHeight};
        return result;
    }
}
