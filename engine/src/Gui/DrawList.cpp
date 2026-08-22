#include <Veng/Gui/DrawList.h>

#include <cmath>

#include <Veng/Assert.h>
#include <Veng/Asset/Font.h>

namespace Veng::Gui
{
    namespace
    {
        // A negative texture index in the packed params means "untextured" to the shape shader.
        constexpr f32 UntexturedIndex = -1.0f;

        // The smallest blur magnitude a shadow quad transports. A zero blur lane means "not a
        // shadow", so a hard-edged shadow still needs a non-zero value; the fragment widens any
        // blur narrower than one screen pixel to the anti-aliasing width, so this reads as a crisp
        // edge rather than an aliased one.
        constexpr f32 MinShadowBlur = 1.0f / 1024.0f;

        // Decodes a UTF-8 byte sequence into Unicode codepoints. A malformed byte is emitted as
        // U+FFFD and skipped, so a bad encoding degrades to replacement glyphs rather than reading
        // out of bounds — the draw list never trusts an ill-formed string.
        vector<u32> DecodeUtf8(string_view text)
        {
            vector<u32> codepoints;
            codepoints.reserve(text.size());

            usize i = 0;
            while (i < text.size())
            {
                const u8 lead = static_cast<u8>(text[i]);
                u32 codepoint = 0;
                usize extra = 0;

                if (lead < 0x80)
                {
                    codepoint = lead;
                }
                else if ((lead & 0xE0) == 0xC0)
                {
                    codepoint = lead & 0x1F;
                    extra = 1;
                }
                else if ((lead & 0xF0) == 0xE0)
                {
                    codepoint = lead & 0x0F;
                    extra = 2;
                }
                else if ((lead & 0xF8) == 0xF0)
                {
                    codepoint = lead & 0x07;
                    extra = 3;
                }
                else
                {
                    codepoints.push_back(0xFFFD);
                    ++i;
                    continue;
                }

                if (i + extra >= text.size())
                {
                    codepoints.push_back(0xFFFD);
                    break;
                }

                bool valid = true;
                for (usize k = 1; k <= extra; ++k)
                {
                    const u8 cont = static_cast<u8>(text[i + k]);
                    if ((cont & 0xC0) != 0x80)
                    {
                        valid = false;
                        break;
                    }
                    codepoint = (codepoint << 6) | (cont & 0x3F);
                }

                if (valid)
                {
                    codepoints.push_back(codepoint);
                    i += extra + 1;
                }
                else
                {
                    codepoints.push_back(0xFFFD);
                    ++i;
                }
            }

            return codepoints;
        }
    }

    void DrawList::Clear()
    {
        m_Vertices.clear();
        m_Indices.clear();
        m_Runs.clear();
        m_Gradients.clear();
        m_ClipStack.clear();
        m_TransformStack.clear();
    }

    vec2 DrawList::ApplyTransform(vec2 point) const
    {
        if (m_TransformStack.empty())
        {
            return point;
        }
        const AffineTransform& transform = m_TransformStack.back();
        return transform.Linear * point + transform.Translation;
    }

    optional<Rect> DrawList::CurrentClip() const
    {
        if (m_ClipStack.empty())
        {
            return std::nullopt;
        }
        return m_ClipStack.back();
    }

    void DrawList::EnsureRun(GuiPipeline pipeline, u32 textureKey, const MaterialInstance* material)
    {
        const optional<Rect> clip = CurrentClip();
        const u32 firstIndex = static_cast<u32>(m_Indices.size());

        if (!m_Runs.empty())
        {
            const DrawRun& last = m_Runs.back();
            const bool clipMatches =
                (last.HasClip == clip.has_value()) &&
                (!clip.has_value() || (last.Clip.Min == clip->Min && last.Clip.Size == clip->Size));
            // A run's texture key rides its trailing vertices' params; the run stores none, so a
            // texture change is detected by the caller opening a distinct run. The run merges only
            // when the pipeline, clip, texture key, and material all match the trailing run —
            // tracked through m_RunTextureKey / m_RunMaterial below.
            if (last.Pipeline == pipeline && clipMatches && m_RunTextureKey == textureKey &&
                m_RunMaterial == material)
            {
                return;
            }
        }

        m_Runs.push_back(DrawRun{
            .Pipeline = pipeline,
            .Material = material,
            .FirstIndex = firstIndex,
            .IndexCount = 0,
            .Clip = clip.value_or(Rect{}),
            .HasClip = clip.has_value(),
        });
        m_RunTextureKey = textureKey;
        m_RunMaterial = material;
    }

    void DrawList::PushQuad(const std::array<vec2, 4>& corners, const std::array<vec2, 4>& uvs,
                            vec4 color, vec2 rectHalf, vec2 center, vec4 params, u32 selector,
                            vec4 shadow, vec4 uvWrap)
    {
        const u32 base = static_cast<u32>(m_Vertices.size());
        for (usize i = 0; i < 4; ++i)
        {
            // The active transform rotates the vertex position, but the shape SDF's local box
            // coordinate stays in the unrotated frame (corner minus the unrotated center), so the
            // rounded-rect SDF, border, and gradient evaluate exactly as unrotated — a rigid turn.
            m_Vertices.push_back(GuiVertex{
                .Position = ApplyTransform(corners[i]),
                .Uv = uvs[i],
                .Color = color,
                .RectHalf = rectHalf,
                .RectCoord = corners[i] - center,
                .Params = params,
                .GradientSelector = selector,
                .Shadow = shadow,
                .UvWrap = uvWrap,
            });
        }

        // Two triangles (TL, TR, BR, BL corner order): TL-TR-BR, TL-BR-BL.
        const std::array<u32, 6> quadIndices = {base + 0, base + 1, base + 2,
                                                base + 0, base + 2, base + 3};
        for (const u32 index : quadIndices)
        {
            m_Indices.push_back(index);
        }
        m_Runs.back().IndexCount += 6;
    }

    void DrawList::Quad(const Rect& rect, vec4 color, const CornerRadii& radii,
                        const Border& border)
    {
        if (rect.IsEmpty())
        {
            return;
        }

        EnsureRun(GuiPipeline::Shape, Renderer::TextureHandle::Invalid);

        const vec2 min = rect.Min;
        const vec2 max = rect.Max();
        const std::array<vec2, 4> corners = {min, vec2(max.x, min.y), max, vec2(min.x, max.y)};
        // The shape path reads no UV when untextured; a unit UV keeps the vertices well-formed.
        const std::array<vec2, 4> uvs = {vec2(0.0f, 0.0f), vec2(1.0f, 0.0f), vec2(1.0f, 1.0f),
                                         vec2(0.0f, 1.0f)};

        const vec2 half = rect.Size * 0.5f;
        const vec2 center = rect.Center();
        const vec4 fill = border.Width > 0.0f ? border.Color : color;
        // A radius past half the box clamps to the largest the box supports (the CSS behavior),
        // so an oversized authored radius reads as a circle/capsule, not an SDF artifact.
        const f32 radius = std::min(radii.TopLeft, std::min(half.x, half.y));
        const vec4 params{radius, border.Width, UntexturedIndex, UntexturedIndex};

        PushQuad(corners, uvs, fill, half, center, params);
    }

    void DrawList::Shadow(const Rect& rect, const BoxShadow& shadow, const CornerRadii& radii)
    {
        if (rect.IsEmpty() || shadow.Color.a <= 0.0f)
        {
            return;
        }

        // A shadow is untextured and carries no gradient, so it merges with the solid and gradient
        // quads it sits between rather than opening a run of its own.
        EnsureRun(GuiPipeline::Shape, Renderer::TextureHandle::Invalid);

        const vec2 half = rect.Size * 0.5f;
        const vec2 center = rect.Center();
        const f32 radius = std::min(radii.TopLeft, std::min(half.x, half.y));
        const f32 blur = std::max(std::abs(shadow.Blur), MinShadowBlur);

        // An outer shadow needs fragments *outside* the box to soften into, so its quad grows by
        // the silhouette's own spread plus the blur ramp and slides by the offset. An inset shadow
        // is bounded by the box it darkens, so its quad is the box itself.
        Rect quad = rect;
        if (!shadow.Inset)
        {
            const f32 grow = std::max(shadow.Spread + blur, 0.0f);
            quad = Rect{.Min = rect.Min + shadow.Offset - vec2(grow),
                        .Size = rect.Size + vec2(2.0f * grow)};
        }

        const vec2 min = quad.Min;
        const vec2 max = quad.Max();
        const std::array<vec2, 4> corners = {min, vec2(max.x, min.y), max, vec2(min.x, max.y)};
        // The shadow reads no UV; a unit UV keeps the vertices well-formed.
        const std::array<vec2, 4> uvs = {vec2(0.0f, 0.0f), vec2(1.0f, 0.0f), vec2(1.0f, 1.0f),
                                         vec2(0.0f, 1.0f)};

        // RectHalf/RectCoord stay the *element's* box however far the quad reaches; the fragment
        // displaces and grows that box into the shadow silhouette, which is what keeps an inset
        // shadow exact inside a rounded corner. Zero border, so the silhouette fills rather than rings.
        const vec4 params{radius, 0.0f, UntexturedIndex, UntexturedIndex};
        const vec4 shadowParams{shadow.Inset ? -blur : blur, shadow.Spread, shadow.Offset.x,
                                shadow.Offset.y};

        PushQuad(corners, uvs, shadow.Color, half, center, params, 0, shadowParams);
    }

    void DrawList::Gradient(const Rect& rect, const GradientFill& fill, const CornerRadii& radii,
                            const Border& border, vec4 tint)
    {
        if (rect.IsEmpty())
        {
            return;
        }

        VE_ASSERT(fill.Ramp.IsValid(), "DrawList gradient requires a valid ramp texture handle");
        VE_ASSERT(fill.Sampler.IsValid(), "DrawList gradient requires a valid ramp sampler handle");

        // A gradient's ramp rides its record (loaded bindlessly in the fragment), so no texture keys
        // the run — an untextured shape run, mergeable with solids and with other gradients.
        EnsureRun(GuiPipeline::Shape, Renderer::TextureHandle::Invalid);

        const vec2 min = rect.Min;
        const vec2 max = rect.Max();
        const std::array<vec2, 4> corners = {min, vec2(max.x, min.y), max, vec2(min.x, max.y)};
        // The gradient reads no UV; a unit UV keeps the vertices well-formed.
        const std::array<vec2, 4> uvs = {vec2(0.0f, 0.0f), vec2(1.0f, 0.0f), vec2(1.0f, 1.0f),
                                         vec2(0.0f, 1.0f)};

        const vec2 half = rect.Size * 0.5f;
        const vec2 center = rect.Center();
        const f32 radius = std::min(radii.TopLeft, std::min(half.x, half.y));
        // The shape params carry only the SDF corner radius and border width; the fill (untextured)
        // leaves the texture/sampler slots negative, so the fragment takes the gradient path.
        const vec4 params{radius, border.Width, UntexturedIndex, UntexturedIndex};

        // Append the gradient record and select it by index-plus-one on every vertex of the quad.
        m_Gradients.push_back(GpuGradient{
            .Kind = static_cast<u32>(fill.Kind),
            .RampTexture = fill.Ramp.Index,
            .RampSampler = fill.Sampler.Index,
            .P0 = fill.P0,
            .P1 = fill.P1,
            .AngleOffset = fill.AngleOffset,
        });
        const u32 selector = static_cast<u32>(m_Gradients.size());

        PushQuad(corners, uvs, tint, half, center, params, selector);
    }

    void DrawList::MaterialFill(const Rect& rect, const MaterialInstance* material,
                                const CornerRadii& radii, const Border& border, vec4 tint,
                                const Rect& uv)
    {
        if (rect.IsEmpty() || material == nullptr)
        {
            return;
        }

        // The material *is* the run key: its pipeline and parameter block are what the pass binds,
        // so no texture keys the run and two fills merge only when one material serves both.
        EnsureRun(GuiPipeline::Material, Renderer::TextureHandle::Invalid, material);

        const vec2 min = rect.Min;
        const vec2 max = rect.Max();
        const std::array<vec2, 4> corners = {min, vec2(max.x, min.y), max, vec2(min.x, max.y)};

        const vec2 uvMin = uv.Min;
        const vec2 uvMax = uv.Max();
        const std::array<vec2, 4> uvs = {uvMin, vec2(uvMax.x, uvMin.y), uvMax,
                                         vec2(uvMin.x, uvMax.y)};

        const vec2 half = rect.Size * 0.5f;
        const f32 radius = std::min(radii.TopLeft, std::min(half.x, half.y));
        // The generated fragment reads only the radius and border lanes (through GuiFillResolve);
        // the texture/sampler lanes stay negative, since a material samples through its own params.
        const vec4 params{radius, border.Width, UntexturedIndex, UntexturedIndex};

        PushQuad(corners, uvs, tint, half, rect.Center(), params);
    }

    void DrawList::EmitTexturedQuad(const Rect& rect, Renderer::TextureHandle texture,
                                    Renderer::SamplerHandle sampler, const Rect& uv, vec4 tint,
                                    const CornerRadii& radii, vec4 uvWrap)
    {
        if (rect.IsEmpty())
        {
            return;
        }

        VE_ASSERT(texture.IsValid(), "DrawList textured quad requires a valid texture handle");
        VE_ASSERT(sampler.IsValid(), "DrawList textured quad requires a valid sampler handle");

        EnsureRun(GuiPipeline::Shape, texture.Index);

        const vec2 min = rect.Min;
        const vec2 max = rect.Max();
        const std::array<vec2, 4> corners = {min, vec2(max.x, min.y), max, vec2(min.x, max.y)};

        const vec2 uvMin = uv.Min;
        const vec2 uvMax = uv.Max();
        const std::array<vec2, 4> uvs = {uvMin, vec2(uvMax.x, uvMin.y), uvMax,
                                         vec2(uvMin.x, uvMax.y)};

        // The half-extent must be the real size so the box has an interior the tinted texture shows
        // through; the corner radius clamps to half the box (the CSS behavior, like Quad). Zero
        // border, so the fill covers the whole rounded shape rather than only a ring.
        const vec2 half = rect.Size * 0.5f;
        const f32 radius = std::min(radii.TopLeft, std::min(half.x, half.y));
        const vec4 params{radius, 0.0f, static_cast<f32>(texture.Index),
                          static_cast<f32>(sampler.Index)};
        PushQuad(corners, uvs, tint, half, rect.Center(), params, 0, vec4(0.0f), uvWrap);
    }

    void DrawList::Texture(const Rect& rect, Renderer::TextureHandle texture,
                           Renderer::SamplerHandle sampler, const Rect& uv, vec4 tint,
                           const CornerRadii& radii)
    {
        EmitTexturedQuad(rect, texture, sampler, uv, tint, radii);
    }

    void DrawList::NineSlice(const Rect& rect, Renderer::TextureHandle texture,
                             Renderer::SamplerHandle sampler, const Insets& sliceUv,
                             const Insets& sizePx, vec4 tint, const Rect& uv, ImageRepeat repeat,
                             vec2 sourcePx)
    {
        if (rect.IsEmpty())
        {
            return;
        }

        // Destination column x-edges and row y-edges: outer corners keep sizePx, the center spans
        // the remainder. A destination smaller than its fixed corners collapses the center to zero.
        const f32 x0 = rect.Min.x;
        const f32 x3 = rect.Max().x;
        const f32 x1 = glm::min(x0 + sizePx.Left, x3);
        const f32 x2 = glm::max(x3 - sizePx.Right, x1);
        const f32 y0 = rect.Min.y;
        const f32 y3 = rect.Max().y;
        const f32 y1 = glm::min(y0 + sizePx.Top, y3);
        const f32 y2 = glm::max(y3 - sizePx.Bottom, y1);

        // Source UV column and row edges from the slice insets (fractions of the sampled sub-rect,
        // mapped back into the texture so an atlas region slices like a standalone texture).
        const f32 u0 = uv.Min.x;
        const f32 u1 = uv.Min.x + sliceUv.Left * uv.Size.x;
        const f32 u2 = uv.Min.x + (1.0f - sliceUv.Right) * uv.Size.x;
        const f32 u3 = uv.Min.x + uv.Size.x;
        const f32 v0 = uv.Min.y;
        const f32 v1 = uv.Min.y + sliceUv.Top * uv.Size.y;
        const f32 v2 = uv.Min.y + (1.0f - sliceUv.Bottom) * uv.Size.y;
        const f32 v3 = uv.Min.y + uv.Size.y;

        const std::array<f32, 4> xs = {x0, x1, x2, x3};
        const std::array<f32, 4> ys = {y0, y1, y2, y3};
        const std::array<f32, 4> us = {u0, u1, u2, u3};
        const std::array<f32, 4> vs = {v0, v1, v2, v3};

        // Texels per unit of UV across the sampled sub-rect, which turns a cell's UV extent into
        // the source pixels its repeat count is measured against. A degenerate axis leaves the
        // scale at zero, which the per-cell guard below reads as "stretch this axis".
        const bool tiling = repeat == ImageRepeat::Tile;
        const vec2 texelsPerUv{
            tiling && uv.Size.x > 0.0f ? sourcePx.x / uv.Size.x : 0.0f,
            tiling && uv.Size.y > 0.0f ? sourcePx.y / uv.Size.y : 0.0f,
        };

        for (usize row = 0; row < 3; ++row)
        {
            for (usize col = 0; col < 3; ++col)
            {
                const Rect tileRect{
                    .Min = vec2(xs[col], ys[row]),
                    .Size = vec2(xs[col + 1] - xs[col], ys[row + 1] - ys[row]),
                };
                if (tileRect.IsEmpty())
                {
                    continue;
                }
                const Rect tileUv{
                    .Min = vec2(us[col], vs[row]),
                    .Size = vec2(us[col + 1] - us[col], vs[row + 1] - vs[row]),
                };

                // A cell repeats only along the axis it grows on: the outer columns and rows are
                // the frame's fixed-size corners and edge thicknesses, so column 0/2 never repeats
                // horizontally and row 0/2 never vertically. The four corners are therefore
                // untouched — zero UvWrap, the exact quad the stretching path emitted.
                const vec2 cellSourcePx = tileUv.Size * texelsPerUv;
                const vec2 repeats{
                    col == 1 && cellSourcePx.x > 0.0f ? tileRect.Size.x / cellSourcePx.x : 1.0f,
                    row == 1 && cellSourcePx.y > 0.0f ? tileRect.Size.y / cellSourcePx.y : 1.0f,
                };
                if (repeats == vec2(1.0f))
                {
                    EmitTexturedQuad(tileRect, texture, sampler, tileUv, tint);
                    continue;
                }

                // The quad spans `repeats` copies of the cell's sub-rect and carries that sub-rect
                // as its wrap bound, so the fragment folds the run-on UV back into the one cell
                // instead of letting the sampler run into its neighbour.
                const Rect spannedUv{.Min = tileUv.Min, .Size = tileUv.Size * repeats};
                const vec4 wrap{tileUv.Min.x, tileUv.Min.y, tileUv.Size.x, tileUv.Size.y};
                EmitTexturedQuad(tileRect, texture, sampler, spannedUv, tint, {}, wrap);
            }
        }
    }

    void DrawList::Text(vec2 pen, const Font& font, string_view text, f32 pixelSize, vec4 color,
                        const optional<f32> maxWidth)
    {
        const vector<u32> codepoints = DecodeUtf8(text);
        if (codepoints.empty())
        {
            return;
        }

        const ShapeResult shaped = font.ShapeRun(codepoints, pixelSize, maxWidth);
        if (shaped.Glyphs.empty())
        {
            return;
        }

        const Renderer::TextureHandle atlas = font.GetAtlasHandle();
        const Renderer::SamplerHandle sampler = font.GetAtlasSamplerHandle();
        VE_ASSERT(atlas.IsValid(), "DrawList text requires a font with a finalized atlas handle");
        VE_ASSERT(sampler.IsValid(),
                  "DrawList text requires a font with a finalized atlas sampler handle");

        EnsureRun(GuiPipeline::Msdf, atlas.Index);

        // The text path carries no shape SDF: the fragment reconstructs coverage from the atlas.
        // Params pack the distance range (atlas texels) and the atlas texture/sampler slots.
        const vec4 params{font.GetDistanceRange(), 0.0f, static_cast<f32>(atlas.Index),
                          static_cast<f32>(sampler.Index)};

        for (const ShapedGlyph& glyph : shaped.Glyphs)
        {
            const vec2 min = pen + glyph.Min;
            const vec2 max = pen + glyph.Max;
            const std::array<vec2, 4> corners = {min, vec2(max.x, min.y), max, vec2(min.x, max.y)};
            const std::array<vec2, 4> uvs = {glyph.UvMin, vec2(glyph.UvMax.x, glyph.UvMin.y),
                                             glyph.UvMax, vec2(glyph.UvMin.x, glyph.UvMax.y)};
            PushQuad(corners, uvs, color, vec2(0.0f), (min + max) * 0.5f, params);
        }
    }

    void DrawList::PushClip(const Rect& rect)
    {
        // A nested clip narrows monotonically: intersect with the enclosing clip so a run's stored
        // clip is already absolute and the pass applies it as a plain scissor.
        const Rect clipped = m_ClipStack.empty() ? rect : m_ClipStack.back().Intersect(rect);
        m_ClipStack.push_back(clipped);
    }

    void DrawList::PopClip()
    {
        VE_ASSERT(!m_ClipStack.empty(), "DrawList::PopClip on an empty clip stack");
        m_ClipStack.pop_back();
    }

    void DrawList::PushTransform(vec2 pivot, f32 radians)
    {
        const f32 c = std::cos(radians);
        const f32 s = std::sin(radians);
        // Columns (c, s), (-s, c): rotating (1, 0) toward (c, s) turns clockwise in the y-down
        // document space, matching the rotation property's sign convention.
        const mat2 rotation(c, s, -s, c);
        // Rotate about the pivot: p' = R * (p - pivot) + pivot = R * p + (pivot - R * pivot).
        AffineTransform pushed{.Linear = rotation, .Translation = pivot - rotation * pivot};

        // A nested transform composes within the enclosing one: newTop(p) = curTop(pushed(p)), so a
        // rotation under another rotation turns within the parent's already-rotated frame.
        if (!m_TransformStack.empty())
        {
            const AffineTransform& current = m_TransformStack.back();
            pushed = AffineTransform{
                .Linear = current.Linear * pushed.Linear,
                .Translation = current.Linear * pushed.Translation + current.Translation,
            };
        }
        m_TransformStack.push_back(pushed);
    }

    void DrawList::PopTransform()
    {
        VE_ASSERT(!m_TransformStack.empty(), "DrawList::PopTransform on an empty transform stack");
        m_TransformStack.pop_back();
    }
}
