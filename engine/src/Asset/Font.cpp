#include <Veng/Asset/Font.h>

#include <algorithm>
#include <array>
#include <cstddef>

#include <Veng/Text/GlyphAtlas.h>

namespace Veng
{
    namespace
    {
        // Whether a codepoint is an ideographic CJK codepoint a line may break between. The minimal
        // inter-ideograph break covers the ranges a Chinese/Japanese UI run is written in — CJK
        // symbols and punctuation, Hiragana, Katakana, the CJK Unified Ideographs (and Extension A
        // and Compatibility). It is deliberately not UAX #14: no dictionary, no prohibited-start/end
        // (kinsoku) rules, no Thai/Lao — just enough that a space-less run wraps within its box.
        bool IsCjkCodepoint(u32 cp)
        {
            return (cp >= 0x3000 && cp <= 0x303F) || // CJK Symbols and Punctuation
                   (cp >= 0x3040 && cp <= 0x309F) || // Hiragana
                   (cp >= 0x30A0 && cp <= 0x30FF) || // Katakana
                   (cp >= 0x3400 && cp <= 0x4DBF) || // CJK Unified Ideographs Extension A
                   (cp >= 0x4E00 && cp <= 0x9FFF) || // CJK Unified Ideographs
                   (cp >= 0xF900 && cp <= 0xFAFF);   // CJK Compatibility Ideographs
        }

        // A line break is permitted between two adjacent ideographic codepoints; this is the whole of
        // the minimal CJK break, in addition to the ordinary break after a space.
        bool IsCjkBreakable(u32 prev, u32 next)
        {
            return IsCjkCodepoint(prev) && IsCjkCodepoint(next);
        }
    }

    Font::~Font() = default;

    Font::ResolvedGlyph Font::Resolve(u32 codepoint) const
    {
        if (m_GlyphSource != nullptr && m_FaceId != Text::FaceId::Invalid)
        {
            if (m_GlyphSource->HasGlyph(m_FaceId, codepoint))
            {
                return ResolvedGlyph{.Face = m_FaceId,
                                     .FieldType = m_FieldType,
                                     .GlyphIndex = m_GlyphSource->GlyphIndex(m_FaceId, codepoint),
                                     .Covered = true};
            }
            for (const AssetHandle<Font>& handle : m_Fallbacks)
            {
                const Font* fallback = handle.Get();
                if (fallback == nullptr || fallback->m_FaceId == Text::FaceId::Invalid)
                {
                    continue;
                }
                if (m_GlyphSource->HasGlyph(fallback->m_FaceId, codepoint))
                {
                    return ResolvedGlyph{
                        .Face = fallback->m_FaceId,
                        .FieldType = fallback->m_FieldType,
                        .GlyphIndex = m_GlyphSource->GlyphIndex(fallback->m_FaceId, codepoint),
                        .Covered = true};
                }
            }
        }
        // No face covers it: this font's own face renders .notdef (glyph index 0).
        return ResolvedGlyph{
            .Face = m_FaceId, .FieldType = m_FieldType, .GlyphIndex = 0, .Covered = false};
    }

    bool Font::HasGlyph(u32 codepoint) const
    {
        return Resolve(codepoint).Covered;
    }

    FontGlyph Font::GetGlyphMetrics(u32 codepoint) const
    {
        FontGlyph out;
        if (m_GlyphSource == nullptr || m_FaceId == Text::FaceId::Invalid)
        {
            return out;
        }
        const ResolvedGlyph resolved = Resolve(codepoint);
        const Text::GlyphMetrics metrics =
            m_GlyphSource->GetGlyphMetrics(resolved.Face, resolved.GlyphIndex);
        out.Advance = metrics.Advance;
        out.PlaneMin = metrics.PlaneMin;
        out.PlaneMax = metrics.PlaneMax;
        out.FieldType = resolved.FieldType;
        return out;
    }

    FontGlyph Font::GetGlyph(u32 codepoint, f32 pixelSize) const
    {
        FontGlyph out;
        if (m_GlyphSource == nullptr || m_GlyphAtlas == nullptr ||
            m_FaceId == Text::FaceId::Invalid)
        {
            return out;
        }
        const ResolvedGlyph resolved = Resolve(codepoint);
        out.FieldType = resolved.FieldType;

        const Text::GlyphKey key =
            m_GlyphAtlas->KeyFor(resolved.Face, resolved.GlyphIndex, pixelSize, resolved.FieldType);
        const Text::GlyphSlot slot = m_GlyphAtlas->Ensure(key);
        if (slot.Resident)
        {
            out.Advance = slot.Advance;
            out.PlaneMin = slot.PlaneMin;
            out.PlaneMax = slot.PlaneMax;
            out.UvMin = slot.UvMin;
            out.UvMax = slot.UvMax;
            out.Page = slot.Page;
            out.FieldType = slot.FieldType;
            return out;
        }

        // Over the atlas capacity this frame: the pixels lag, but the layout must not. Keep the
        // device-free advance so the run breaks and sizes identically; the invalid page leaves the
        // draw with no quad for this glyph rather than spinning on a slot that will not land.
        out.Advance = m_GlyphSource->GetGlyphMetrics(resolved.Face, resolved.GlyphIndex).Advance;
        return out;
    }

    f32 Font::GetKerning(u32 left, u32 right) const
    {
        if (m_GlyphSource == nullptr || m_FaceId == Text::FaceId::Invalid)
        {
            return 0.0f;
        }
        const ResolvedGlyph l = Resolve(left);
        const ResolvedGlyph r = Resolve(right);
        // Kerning is a property of one face's own glyph pair; an uncovered codepoint or a pair split
        // across two faces (this face and a fallback's) carries none.
        if (!l.Covered || !r.Covered || l.Face != r.Face)
        {
            return 0.0f;
        }
        return m_GlyphSource->GetKerning(l.Face, l.GlyphIndex, r.GlyphIndex);
    }

    ShapeResult Font::ShapeRun(std::span<const u32> codepoints, f32 pixelSize,
                               optional<f32> maxWidth, TextShapeMode mode) const
    {
        const std::array<ShapeSpan, 1> span{
            ShapeSpan{.Codepoints = codepoints, .PixelSize = pixelSize}};
        return ShapeSpans(span, maxWidth, mode);
    }

    ShapeResult Font::ShapeSpans(std::span<const ShapeSpan> spans, optional<f32> maxWidth,
                                 TextShapeMode mode) const
    {
        ShapeResult result;
        if (spans.empty())
        {
            return result;
        }

        // One size for the whole paragraph keeps the single-run arithmetic exactly: the block height
        // is multiplied out rather than accumulated, so a one-span paragraph is bit-identical to a
        // run shaped alone.
        const bool uniform =
            std::ranges::all_of(spans, [&](const ShapeSpan& span)
                                { return span.PixelSize == spans.front().PixelSize; });

        // A pending line accumulates its glyphs before it is committed to result.Lines — the width
        // wrap decides retroactively where a line ends, so a line is only finalized once broken.
        struct PendingGlyph
        {
            u32 Codepoint;
            f32 PenX; // pen origin x, in pixels, before this glyph's quad offset
            FontGlyph Metrics;
            u32 Span;
            f32 PixelSize;
        };

        vector<PendingGlyph> lineGlyphs;
        f32 penX = 0.0f;
        f32 maxLineWidth = 0.0f;
        // The size an empty line takes its height from: the span the pen stands in.
        f32 penSize = spans.front().PixelSize;

        // The previous line's metrics, which place the next line's baseline: each line is as tall as
        // the tallest glyph on it, its baseline at the largest ascender among them. y grows downward.
        f32 baseline = 0.0f;
        f32 lineAscent = 0.0f;
        f32 lineStep = 0.0f;
        f32 lineDepth = 0.0f;

        // Emits the accumulated line's visible quads into the result and records its ShapedLine,
        // then resets the pen for the next line.
        const auto commitLine = [&]()
        {
            f32 ascent = 0.0f;
            f32 step = 0.0f;
            f32 depth = 0.0f;
            if (lineGlyphs.empty())
            {
                ascent = m_Ascender * penSize;
                step = m_LineHeight * penSize;
                depth = -m_Descender * penSize;
            }
            for (const PendingGlyph& pending : lineGlyphs)
            {
                ascent = std::max(ascent, m_Ascender * pending.PixelSize);
                step = std::max(step, m_LineHeight * pending.PixelSize);
                depth = std::max(depth, -m_Descender * pending.PixelSize);
            }
            // The next baseline steps down the previous line's height, shifted by how much taller
            // this line's ascender is: with one size the shift is exactly zero.
            baseline = result.Lines.empty() ? ascent : baseline + lineStep + (ascent - lineAscent);
            lineAscent = ascent;
            lineStep = step;
            lineDepth = depth;

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
                const f32 size = pending.PixelSize;
                ShapedGlyph shaped;
                shaped.Codepoint = pending.Codepoint;
                shaped.Pen = {pending.PenX, baseline};
                shaped.Min = {pending.PenX + glyph.PlaneMin.x * size,
                              baseline - glyph.PlaneMax.y * size};
                shaped.Max = {pending.PenX + glyph.PlaneMax.x * size,
                              baseline - glyph.PlaneMin.y * size};
                shaped.UvMin = glyph.UvMin;
                shaped.UvMax = glyph.UvMax;
                shaped.Page = glyph.Page;
                shaped.FieldType = glyph.FieldType;
                shaped.PixelSize = size;
                shaped.Span = pending.Span;
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
        };

        // The kerning between two neighbouring glyphs, in pixels: only between glyphs of one size,
        // since a pair's adjustment is a property of one face at one scale.
        const auto kerningBetween = [&](const PendingGlyph& left, const PendingGlyph& right)
        {
            return left.PixelSize == right.PixelSize
                       ? GetKerning(left.Codepoint, right.Codepoint) * right.PixelSize
                       : 0.0f;
        };

        // Moves the trailing run (glyphs after the latest break opportunity on the current line) to a
        // fresh line, re-flowing the pen. A break opportunity is after a space, or between two
        // adjacent CJK codepoints (the minimal inter-ideograph break). Returns false when the line
        // carries no earlier break to fall back on (a single unbreakable word wider than the
        // constraint hard-breaks in the caller instead).
        const auto wrapTrailingRun = [&]() -> bool
        {
            usize breakAt = 0;
            for (usize i = lineGlyphs.size(); i >= 2; i--)
            {
                const u32 prev = lineGlyphs[i - 2].Codepoint;
                const u32 next = lineGlyphs[i - 1].Codepoint;
                if (prev == ' ' || IsCjkBreakable(prev, next))
                {
                    breakAt = i - 1;
                    break;
                }
            }
            if (breakAt == 0)
            {
                return false;
            }

            vector<PendingGlyph> carried(lineGlyphs.begin() + static_cast<std::ptrdiff_t>(breakAt),
                                         lineGlyphs.end());
            lineGlyphs.resize(breakAt);
            commitLine();

            // Re-lay the carried run from the new pen origin, re-applying kerning within it.
            for (usize i = 0; i < carried.size(); i++)
            {
                PendingGlyph next = carried[i];
                if (i > 0)
                {
                    penX += kerningBetween(carried[i - 1], next);
                }
                next.PenX = penX;
                penX += next.Metrics.Advance * next.PixelSize;
                lineGlyphs.push_back(next);
            }
            return true;
        };

        optional<PendingGlyph> previous;
        for (u32 spanIndex = 0; spanIndex < spans.size(); ++spanIndex)
        {
            const ShapeSpan& span = spans[spanIndex];
            const f32 pixelSize = span.PixelSize;
            penSize = pixelSize;
            for (const u32 codepoint : span.Codepoints)
            {
                if (codepoint == '\n')
                {
                    commitLine();
                    previous.reset();
                    continue;
                }

                // A covered codepoint resolves through the fallback chain; an uncovered one lands on
                // the resolved face's .notdef box (glyph 0), so a coverage gap stays visible and the
                // layout honest rather than the character silently vanishing.
                const FontGlyph glyph = mode == TextShapeMode::Draw ? GetGlyph(codepoint, pixelSize)
                                                                    : GetGlyphMetrics(codepoint);

                PendingGlyph pending{.Codepoint = codepoint,
                                     .PenX = 0.0f,
                                     .Metrics = glyph,
                                     .Span = spanIndex,
                                     .PixelSize = pixelSize};
                if (previous.has_value())
                {
                    penX += kerningBetween(*previous, pending);
                }
                pending.PenX = penX;
                penX += glyph.Advance * pixelSize;
                lineGlyphs.push_back(pending);
                previous = pending;

                if (maxWidth && penX > *maxWidth && lineGlyphs.size() > 1)
                {
                    // The line overflowed: wrap at the latest break opportunity, or hard-break before
                    // this glyph when the line offers no earlier break (one unbreakable run wider
                    // than the constraint).
                    if (!wrapTrailingRun())
                    {
                        lineGlyphs.pop_back();
                        commitLine();
                        pending.PenX = 0.0f;
                        penX = pending.Metrics.Advance * pixelSize;
                        lineGlyphs.push_back(pending);
                    }
                    previous = lineGlyphs.empty() ? optional<PendingGlyph>{}
                                                  : optional<PendingGlyph>{lineGlyphs.back()};
                }
            }
        }

        commitLine();

        const f32 pixelSize = spans.front().PixelSize;
        const f32 blockHeight =
            uniform ? (static_cast<f32>(result.Lines.size() - 1) * (m_LineHeight * pixelSize) +
                       (m_Ascender - m_Descender) * pixelSize)
                    : baseline + lineDepth;
        result.Size = {maxLineWidth, blockHeight};
        return result;
    }
}
