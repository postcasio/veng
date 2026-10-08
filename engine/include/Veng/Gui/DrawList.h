#pragma once

#include <array>
#include <span>

#include <Veng/Veng.h>
#include <Veng/Renderer/BindlessRegistry.h>

namespace Veng
{
    class Font;
    class MaterialInstance;
    struct ShapeResult;
    struct ShapedGlyph;
}

/// @brief Device-free UI primitives: the draw list, its runs, and the shared value types.
///
/// Everything here is pure CPU data — no Vulkan, no ImGui. A DrawList is built each frame
/// from UI primitives and resolves to one interleaved vertex/index stream partitioned into
/// runs, which a render pass replays. The value types (Rect, CornerRadii, Border, Insets)
/// are the shared vocabulary a layout or styling layer above the draw list authors against.
namespace Veng::Gui
{
    /// @brief An axis-aligned rectangle: a top-left corner and a size, in the space it is used in.
    ///
    /// UI geometry is expressed here in framebuffer pixels with a top-left origin and y
    /// increasing downward. Min is the top-left corner, Min + Size the bottom-right.
    struct Rect
    {
        /// @brief Top-left corner.
        vec2 Min{0.0f};
        /// @brief Width and height (non-negative).
        vec2 Size{0.0f};

        /// @brief Returns the bottom-right corner (Min + Size).
        [[nodiscard]] vec2 Max() const { return Min + Size; }

        /// @brief Returns the geometric center.
        [[nodiscard]] vec2 Center() const { return Min + Size * 0.5f; }

        /// @brief Returns whether the rectangle has a positive area.
        [[nodiscard]] bool IsEmpty() const { return Size.x <= 0.0f || Size.y <= 0.0f; }

        /// @brief Returns the intersection of this rectangle with another.
        ///
        /// The largest rectangle contained in both; empty (zero size) when they do not overlap.
        /// @param other  The rectangle to intersect with.
        /// @return The overlapping region, or an empty rectangle when disjoint.
        [[nodiscard]] Rect Intersect(const Rect& other) const
        {
            const vec2 min = glm::max(Min, other.Min);
            const vec2 max = glm::min(Max(), other.Max());
            return Rect{.Min = min, .Size = glm::max(max - min, vec2(0.0f))};
        }
    };

    /// @brief Per-corner radius of a rounded rectangle, in pixels.
    ///
    /// A single radius applies to every corner; the four fields allow independent corners.
    /// The draw list emits one rounded-rect quad whose fragment SDF reads these; the current
    /// shape path uses a single radius, so all four are expected equal (the uniform case).
    struct CornerRadii
    {
        /// @brief Top-left corner radius, in pixels.
        f32 TopLeft = 0.0f;
        /// @brief Top-right corner radius, in pixels.
        f32 TopRight = 0.0f;
        /// @brief Bottom-right corner radius, in pixels.
        f32 BottomRight = 0.0f;
        /// @brief Bottom-left corner radius, in pixels.
        f32 BottomLeft = 0.0f;

        /// @brief Builds a uniform radius applied to all four corners.
        /// @param radius  The radius in pixels.
        /// @return CornerRadii with every corner set to radius.
        static CornerRadii All(f32 radius)
        {
            return {
                .TopLeft = radius,
                .TopRight = radius,
                .BottomRight = radius,
                .BottomLeft = radius,
            };
        }
    };

    /// @brief A rectangle's border: a width and a color.
    ///
    /// A zero width draws no border (a filled shape). A positive width fills only the ring
    /// within Width pixels of the shape's edge, in Color, leaving the interior transparent.
    /// The ring lies **inside** the quad's own rect and never grows it — the draw-list tier of the
    /// one box model a document lays out in, where an element's rect is its border box.
    struct Border
    {
        /// @brief Border thickness in pixels; zero fills the whole shape instead.
        f32 Width = 0.0f;
        /// @brief Border color, linear straight-alpha RGBA.
        vec4 Color{0.0f};
    };

    /// @brief A soft drop or inset shadow of a rounded rectangle.
    ///
    /// The shadow silhouette is the element's rounded box translated by Offset and grown by Spread
    /// (shrunk, for a negative Spread), its edge softened across Blur pixels. Inset flips it: the
    /// shadow paints *inside* the box, between the box edge and the same displaced silhouette, so a
    /// panel reads as recessed. A Blur of zero is a hard-edged (anti-aliased) shadow.
    struct BoxShadow
    {
        /// @brief Displacement of the shadow silhouette from the box, in pixels.
        vec2 Offset{0.0f};
        /// @brief Softening radius of the shadow edge, in pixels; zero is a hard edge.
        f32 Blur = 0.0f;
        /// @brief Growth of the shadow silhouette on every side, in pixels; negative shrinks it.
        f32 Spread = 0.0f;
        /// @brief Shadow color, linear straight-alpha RGBA; a zero alpha draws nothing.
        vec4 Color{0.0f};
        /// @brief Whether the shadow paints inside the box (an inner shadow) instead of behind it.
        bool Inset = false;
    };

    /// @brief How an arc band's two ends terminate.
    enum class ArcCap : u8
    {
        /// @brief Each end is a straight radial cut.
        Butt,
        /// @brief Each end is a semicircle the band's own thickness across — the gauge look.
        Round,
    };

    /// @brief An annular-sector silhouette that replaces the rounded box of the quads drawn under it.
    ///
    /// The sector is centred at Center, bounded outside by Radius, and runs clockwise from
    /// StartRadians through SweepRadians, angles measured clockwise from 12 o'clock in the y-down
    /// space. A Thickness of zero — or one at least Radius — fills to the centre (a pie wedge, or a
    /// disc at a full sweep); a smaller one leaves a band of that radial width, measured inward from
    /// Radius, whose ends Cap shapes. A sweep of at least a full turn is a whole ring or disc with no
    /// seam, and a sweep of zero covers nothing.
    ///
    /// Center is in the same space as the rects of the primitives it masks — the untransformed
    /// space PushTransform rotates out of — so an arc turns rigidly with a rotated subtree.
    struct ArcShape
    {
        /// @brief The sector's centre, in framebuffer pixels.
        vec2 Center{0.0f};
        /// @brief The outer radius, in pixels.
        f32 Radius = 0.0f;
        /// @brief Where the sector begins, in radians clockwise from 12 o'clock.
        f32 StartRadians = 0.0f;
        /// @brief The sector's clockwise extent, in radians; clamped to [0, 2π] at emission.
        f32 SweepRadians = 0.0f;
        /// @brief The band's radial thickness in pixels, inward from Radius; zero fills to the centre.
        f32 Thickness = 0.0f;
        /// @brief How a band's two ends terminate; irrelevant to a filled wedge or a full sweep.
        ArcCap Cap = ArcCap::Butt;
    };

    /// @brief Per-edge inset distances, in pixels: the 9-slice margins and the padding vocabulary.
    struct Insets
    {
        /// @brief Left inset, in pixels.
        f32 Left = 0.0f;
        /// @brief Top inset, in pixels.
        f32 Top = 0.0f;
        /// @brief Right inset, in pixels.
        f32 Right = 0.0f;
        /// @brief Bottom inset, in pixels.
        f32 Bottom = 0.0f;

        /// @brief Builds uniform insets applied to all four edges.
        /// @param inset  The inset in pixels.
        /// @return Insets with every edge set to inset.
        static Insets All(f32 inset)
        {
            return {.Left = inset, .Top = inset, .Right = inset, .Bottom = inset};
        }
    };

    /// @brief Whether a fill spans its box once or repeats at the source's intrinsic pixel size.
    ///
    /// Tiling is never repeated geometry: an unsliced fill is one quad with a scaled UV rect the
    /// texture's own wrapping sampler repeats, so it costs the same however large the box grows —
    /// and a texture whose `*.tex.json` authors a clamp address mode clamps instead of repeating.
    /// A *sliced* fill cannot use the sampler, because a wrap at the whole texture's bounds would
    /// sample the neighbouring cell; it wraps arithmetically in the fragment against a per-quad UV
    /// sub-rect instead (GuiVertex::UvWrap), so there the address mode does not decide the repeat.
    /// The draw list speaks the mode because a nine-slice frame decides tiling *per cell* — the
    /// fixed corners never repeat, the edges repeat along their growing axis, the center on both.
    enum class ImageRepeat : u8
    {
        /// @brief The fill spans the box once, mapped by its ImageFit.
        Stretch,
        /// @brief The fill repeats at the texture's intrinsic pixel size from the box's top-left.
        Tile,
    };

    /// @brief The shape of a gradient fill: how a fragment's box-local position maps to a ramp offset.
    ///
    /// Every kind reduces the fragment's normalized box coordinate (RectCoord / RectHalf, in [-1, 1])
    /// to a single t in [0, 1] that samples the 1D ramp LUT; only the reduction differs. Multi-stop
    /// color is baked into the ramp, so the runtime evaluates one t per fragment and samples. The
    /// geometry that drives each reduction rides a GpuGradient record (P0/P1/AngleOffset), so a
    /// gradient carries explicit endpoints and elliptical radii rather than a box-fit approximation.
    enum class GradientKind : u8
    {
        /// @brief Linear fill between two points: t = saturate(dot(p - P0, P1 - P0) / |P1 - P0|²).
        Linear,
        /// @brief Elliptical radial fill: t = length((p - P0) / P1), P0 the center, P1 the radii.
        Radial,
        /// @brief Angular sweep about P0: t = frac(atan2(p - P0) / TAU - AngleOffset).
        Conic,
    };

    /// @brief A resolved gradient fill: its shape, geometry, and the ramp LUT to sample.
    ///
    /// Device-free: the ramp is a bindless texture/sampler slot pair (a runtime-built N×1 ramp),
    /// never an asset handle. Geometry is in the element's normalized box space (p = RectCoord /
    /// RectHalf, in [-1, 1]) and interpreted per Kind: Linear takes P0/P1 as the start/end points,
    /// Radial takes P0 as the center and P1 as the (x, y) radii, Conic takes P0 as the center and
    /// AngleOffset as the start turn. A Gui::DrawList packs this into a GpuGradient record the pass
    /// uploads to a storage buffer, and the vertex carries only the record's index.
    struct GradientFill
    {
        /// @brief Which reduction maps box position to the ramp offset t.
        GradientKind Kind = GradientKind::Linear;
        /// @brief Linear start point / radial + conic center, in normalized box space.
        vec2 P0{0.0f};
        /// @brief Linear end point / radial (x, y) radii, in normalized box space.
        vec2 P1{0.0f};
        /// @brief Conic start turn in [0, 1); unused by the other kinds.
        f32 AngleOffset = 0.0f;
        /// @brief Bindless slot of the 1D ramp LUT (linear straight-alpha), sampled at (t, 0.5).
        Renderer::TextureHandle Ramp;
        /// @brief Bindless slot of the ramp's sampler (clamp-to-edge, linear).
        Renderer::SamplerHandle Sampler;
    };

    /// @brief The GPU-side gradient record: one per gradient fill, indexed from the vertex.
    ///
    /// A tightly-packed 48-byte record the draw list accumulates and the pass uploads to a
    /// byte-address storage buffer; the fragment loads it by index and evaluates the ramp offset t.
    /// The layout is scalar and matches the shader's GpuGradient one-to-one — every field is 4 bytes,
    /// so there are no alignment gaps. Geometry is in normalized box space (see GradientFill).
    struct GpuGradient
    {
        /// @brief The GradientKind ordinal (0 Linear, 1 Radial, 2 Conic).
        u32 Kind = 0;
        /// @brief Bindless slot of the ramp LUT texture.
        u32 RampTexture = 0;
        /// @brief Bindless slot of the ramp sampler.
        u32 RampSampler = 0;
        /// @brief Padding to keep the following vec2 pair at an 8-byte-aligned offset.
        u32 Pad0 = 0;
        /// @brief Linear start point / radial + conic center.
        vec2 P0{0.0f};
        /// @brief Linear end point / radial radii.
        vec2 P1{0.0f};
        /// @brief Conic start turn; unused otherwise.
        f32 AngleOffset = 0.0f;
        /// @brief Padding to a 48-byte record.
        f32 Pad1 = 0.0f;
        /// @brief Padding to a 48-byte record.
        f32 Pad2 = 0.0f;
        /// @brief Padding to a 48-byte record.
        f32 Pad3 = 0.0f;
    };
    static_assert(sizeof(GpuGradient) == 48, "GpuGradient must match the shader's 48-byte record");

    /// @brief How long after an element's state age restarts a material fill on it keeps being
    ///        redrawn, in seconds.
    ///
    /// A draw is re-recorded only when something about its element moves, so a fill animating off
    /// `Element::StateAge` is re-recorded every frame for this long after a restart and then left
    /// alone; a reaction a fill draws to a state change has this long to run.
    inline constexpr f32 MaterialStateWindow = 1.0f;

    /// @brief Selects which fragment path a run replays.
    enum class GuiPipeline : u8
    {
        /// @brief Rounded-rect SDF with optional border and optional texture modulation.
        Shape,
        /// @brief MSDF glyph coverage sampled from a font atlas.
        Msdf,
        /// @brief An authored GuiFill material shading the fill, the engine's SDF coverage multiplying it.
        Material,
    };

    /// @brief One interleaved vertex of the draw list's single geometry stream.
    ///
    /// Positions are framebuffer pixels (top-left origin, y down). Color is linear
    /// straight-alpha RGBA. RectHalf and RectCoord drive the shape SDF (the rect half-extent
    /// and this vertex's signed local coordinate from the rect center, both in pixels); the
    /// text path leaves them zero. Params packs the fragment inputs: for the shape path
    /// (corner radius, border width, texture index, sampler index), for the text path
    /// (atlas distance range, 0, atlas texture index, atlas sampler index). A negative
    /// texture index means untextured.
    struct GuiVertex
    {
        /// @brief Position in framebuffer pixels (top-left origin, y down).
        vec2 Position{0.0f};
        /// @brief Texture / atlas UV.
        vec2 Uv{0.0f};
        /// @brief Linear straight-alpha RGBA color.
        vec4 Color{0.0f};
        /// @brief Rounded-rect half-extent in pixels (shape path; zero for text).
        vec2 RectHalf{0.0f};
        /// @brief Signed local coordinate from the rect center in pixels (shape path; zero for text).
        vec2 RectCoord{0.0f};
        /// @brief Packed fragment params (see the struct brief).
        vec4 Params{0.0f};
        /// @brief Gradient record selector: 0 means no gradient, else the record index plus one.
        ///
        /// A zero (the default) means the fill is the solid color or the modulating texture; a
        /// positive value selects the GpuGradient record at (value - 1) in the draw list's gradient
        /// table, and the fragment loads it from the storage buffer to evaluate the ramp offset.
        u32 GradientSelector = 0;
        /// @brief Shadow parameters: blur (x, signed), spread (y), and offset (zw), all in pixels.
        ///
        /// A zero x (the default) means the quad is not a shadow and the fragment takes its ordinary
        /// fill path. A **positive** x is an outer (drop) shadow and a **negative** x an inset one —
        /// the sign is the only transport the inset flag has, which is why a hard-edged shadow still
        /// carries a tiny non-zero blur. The magnitude is the softening radius; spread grows the
        /// silhouette and offset displaces it, both evaluated against RectHalf/RectCoord in the
        /// fragment rather than baked into the quad, so the silhouette stays exact under a rounded
        /// corner.
        vec4 Shadow{0.0f};
        /// @brief The UV sub-rect this quad's sampling wraps within: min in xy, size in zw.
        ///
        /// A zero size (the default) means the lane is inactive and the fragment samples the
        /// interpolated UV exactly as it otherwise would. A non-zero size makes the fragment wrap
        /// the UV into [min, min + size) arithmetically, which is the only way a nine-slice cell
        /// can repeat: a wrapping sampler wraps at the *whole texture's* bounds and would sample
        /// the neighbouring cell instead. The quad's own UV rect spans as many copies of the
        /// sub-rect as the cell repeats, and the fragment takes its texture derivatives from that
        /// unwrapped UV, so the seam frac() introduces does not collapse a mipped texture to its
        /// smallest level along a one-pixel line.
        vec4 UvWrap{0.0f};
        /// @brief The arc silhouette this quad is masked to: start (x) and sweep (y) in radians,
        ///        band thickness (z) and signed outer radius (w) in pixels.
        ///
        /// A zero w (the default) means the lane is inactive and the fragment takes the rounded-box
        /// silhouette from RectHalf/RectCoord. A **positive** w is an arc with butt caps and a
        /// **negative** w one with round caps, the magnitude being the outer radius — the sign is the
        /// cap's only transport, as a shadow's blur sign is its inset flag's. See ArcShape.
        vec4 Arc{0.0f};
        /// @brief This vertex's position relative to the arc's centre, in unrotated pixels.
        ///
        /// The arc's counterpart to RectCoord, kept separate because the arc belongs to the element
        /// rather than to the quad: a padding-box image, a letterboxed fit, or a nine-slice cell is
        /// a quad whose own centre and half-extent are not the element's, and each is masked by the
        /// one arc all the same. Zero when the Arc lane is inactive.
        vec2 ArcCoord{0.0f};
    };
    static_assert(sizeof(GuiVertex) == 124, "GuiVertex must stay packed with no padding");

    /// @brief A contiguous slice of the index stream sharing one pipeline, clip, texture, and material.
    ///
    /// The pass replays runs in order, changing pipeline / scissor / bound texture only at run
    /// boundaries. A run's clip is already intersected with the enclosing clip stack, so the
    /// pass applies it as an absolute scissor with no further nesting math.
    struct DrawRun
    {
        /// @brief The fragment path this run replays.
        GuiPipeline Pipeline = GuiPipeline::Shape;
        /// @brief The material shading a GuiPipeline::Material run; null on every other run.
        ///
        /// Borrowed for the frame, not owned: the resident AssetHandle keeping it alive lives on
        /// the Style that authored the fill, and a draw list is rebuilt from that tree each frame.
        /// It is part of the run key, so two adjacent fills with different materials are two runs.
        const MaterialInstance* Material = nullptr;
        /// @brief First index of this run in the draw list's index stream.
        u32 FirstIndex = 0;
        /// @brief Number of indices in this run.
        u32 IndexCount = 0;
        /// @brief Absolute scissor rectangle in framebuffer pixels; the whole surface when unclipped.
        Rect Clip;
        /// @brief True when Clip is a real clip rectangle; false means "no scissor" (full surface).
        bool HasClip = false;
    };

    /// @brief A position in a draw list's streams: how much of each it held when the mark was taken.
    ///
    /// Two marks bracket the geometry emitted between them, which DrawList::AppendRange copies into
    /// another list — how a retained UI re-emits an unchanged subtree from its last frame instead of
    /// rebuilding it. Each count is measured from the list's start, so the difference of two marks
    /// is the extent of what they bracket, and a mark rebases onto another list by adding the offset
    /// its range landed at.
    struct DrawMark
    {
        /// @brief Vertices emitted before the mark.
        u32 Vertex = 0;
        /// @brief Indices emitted before the mark.
        u32 Index = 0;
        /// @brief Gradient records emitted before the mark.
        u32 Gradient = 0;
        /// @brief Glyphs drawn before the mark, counted only while the list records glyphs.
        u32 Glyph = 0;

        /// @brief Returns the mark this one becomes when everything before `base` is removed.
        /// @param base  A mark at or before this one.
        [[nodiscard]] DrawMark Since(const DrawMark& base) const
        {
            return DrawMark{.Vertex = Vertex - base.Vertex,
                            .Index = Index - base.Index,
                            .Gradient = Gradient - base.Gradient,
                            .Glyph = Glyph - base.Glyph};
        }

        /// @brief Returns this relative mark placed after `base`.
        /// @param base  The absolute mark this one is measured from.
        [[nodiscard]] DrawMark After(const DrawMark& base) const
        {
            return DrawMark{.Vertex = Vertex + base.Vertex,
                            .Index = Index + base.Index,
                            .Gradient = Gradient + base.Gradient,
                            .Glyph = Glyph + base.Glyph};
        }
    };

    /// @brief The clip, transform, and arc in force at a point of a draw list's build.
    ///
    /// The three stacks' tops, which are everything a primitive emitted at that point takes from its
    /// surroundings: its run's scissor, its transformed positions, and its arc mask. Geometry
    /// recorded under one state reproduces exactly only under an equal state, which is what a
    /// retained replay checks before it copies a range rather than re-emitting it.
    struct DrawState
    {
        /// @brief Whether a clip is open; Clip is meaningful only when it is.
        bool HasClip = false;
        /// @brief The effective (already intersected) clip rectangle.
        Rect Clip;
        /// @brief The linear part of the composed transform.
        mat2 Linear{1.0f};
        /// @brief The translation of the composed transform.
        vec2 Translation{0.0f};
        /// @brief Whether an arc silhouette is open; Arc is meaningful only when it is.
        bool HasArc = false;
        /// @brief The arc silhouette masking shape quads.
        ArcShape Arc;

        /// @brief Returns whether two states would emit identical geometry for identical primitives.
        /// @param other  The state to compare against.
        [[nodiscard]] bool Matches(const DrawState& other) const;
    };

    /// @brief A device-free command buffer of UI primitives resolving to one geometry stream.
    ///
    /// Each primitive call (Quad / Line / Texture / NineSlice / Text) appends geometry to a single
    /// interleaved vertex/index stream and extends or opens a run keyed by {pipeline, clip,
    /// texture}. PushClip / PopClip maintain a scissor stack whose entries intersect. A render
    /// pass consumes GetVertices() / GetIndices() / GetRuns() and replays each run. Colors are
    /// linear by contract; the pass blends them in linear space.
    class DrawList
    {
    public:
        /// @brief Constructs an empty draw list.
        DrawList() = default;

        /// @brief Clears all geometry, runs, and the clip, transform, and arc stacks for reuse across frames.
        void Clear();

        /// @brief Appends a filled or bordered rounded rectangle.
        /// @param rect     The rectangle, in framebuffer pixels.
        /// @param color    Fill color, linear straight-alpha RGBA (ignored where a border replaces it).
        /// @param radii    Per-corner radius; the shape path uses the uniform radius.
        /// @param border   Optional border; a positive width draws a ring in the border color.
        void Quad(const Rect& rect, vec4 color, const CornerRadii& radii = {},
                  const Border& border = {});

        /// @brief Appends one round-capped line segment — a capsule — from one point to another.
        ///
        /// The segment is the rounded rectangle of a box `|to - from| + width` long and `width`
        /// high, with a radius of `width / 2`, turned about the segment's midpoint through the
        /// transform stack — so it is one quad on the ordinary shape run, batching with the fills
        /// around it and rotating with any enclosing transform. The ends are semicircles centred on
        /// the two points. Consecutive segments sharing an endpoint therefore meet in a round join
        /// with no extra geometry; a translucent line covers each join twice, since the caps
        /// overlap there. A zero-length segment is a dot of that diameter. Under PushArc it is
        /// masked like any other shape quad.
        /// @param from   The segment's start, in framebuffer pixels.
        /// @param to     The segment's end, in framebuffer pixels.
        /// @param width  The line width in pixels; a non-positive width draws nothing.
        /// @param color  Line color, linear straight-alpha RGBA; a zero alpha draws nothing.
        void Line(vec2 from, vec2 to, f32 width, vec4 color);

        /// @brief Appends a soft drop or inset shadow of a rounded rectangle.
        ///
        /// One extra quad on the same shape pipeline: an outer shadow's quad is the box translated
        /// by the shadow's offset and grown by its spread plus its blur, so the softened silhouette
        /// has fragments to shade outside the box; an inset shadow's quad is the box itself, the
        /// geometry bounding what an inner shadow may cover. Untextured, so it batches with the
        /// solid and gradient quads around it. A caller draws an outer shadow *before* the element's
        /// fill and an inset shadow *after* it.
        /// @param rect    The element's box, in framebuffer pixels — not the shadow silhouette.
        /// @param shadow  The shadow's offset, blur, spread, color, and inset flag.
        /// @param radii   The element's per-corner radius; the shape path uses the uniform radius.
        void Shadow(const Rect& rect, const BoxShadow& shadow, const CornerRadii& radii = {});

        /// @brief Appends a rounded rectangle filled by a gradient sampled from a ramp LUT.
        ///
        /// Shares the rounded-rect SDF and border of Quad, so a gradient composes with corner radius
        /// and a border ring; the fill color comes from the gradient's ramp instead of a flat color.
        /// The fill is appended to the draw list's gradient table (GetGradients) and the vertex
        /// carries only the record index, so many gradients batch into one run regardless of ramp.
        /// @param rect    The rectangle, in framebuffer pixels.
        /// @param fill    The gradient shape, geometry, and ramp/sampler slots.
        /// @param radii   Per-corner radius; the shape path uses the uniform radius.
        /// @param border  Optional border; a positive width draws a ring in the border color.
        /// @param tint    Multiplied over the sampled ramp texel, linear straight-alpha RGBA.
        void Gradient(const Rect& rect, const GradientFill& fill, const CornerRadii& radii = {},
                      const Border& border = {}, vec4 tint = vec4(1.0f));

        /// @brief Appends a rounded rectangle whose fill color an authored GuiFill material shades.
        ///
        /// A material is a fill *source*, never a silhouette: the fragment's authored RGBA is
        /// multiplied by the same rounded-rect SDF coverage and border ring the flat and gradient
        /// fills ride, so corner radius, border, clip, opacity, and rotation compose with it for
        /// free. The quad opens a run keyed by the material instance, so a distinct material breaks
        /// batching exactly as a distinct texture does.
        /// @param rect      The rectangle, in framebuffer pixels.
        /// @param material  The GuiFill material instance shading the fill; null draws nothing.
        /// @param radii     Per-corner radius; the shape path uses the uniform radius.
        /// @param border    Optional border; a positive width restricts the fill to the ring.
        /// @param tint      Forwarded to the fragment as the vertex color, linear straight-alpha RGBA.
        /// @param uv        UV rectangle the quad interpolates (defaults to the whole 0..1 box).
        /// @param stateAge  The element's state age (`Element::StateAge`), up to
        ///                  `MaterialStateWindow` (the default, "settled"); the fragment reads it
        ///                  through `GuiFillStateAge`.
        void MaterialFill(const Rect& rect, const MaterialInstance* material,
                          const CornerRadii& radii = {}, const Border& border = {},
                          vec4 tint = vec4(1.0f),
                          const Rect& uv = {.Min = {0.0f, 0.0f}, .Size = {1.0f, 1.0f}},
                          f32 stateAge = MaterialStateWindow);

        /// @brief Appends a textured quad modulated by a tint, optionally rounded.
        ///
        /// Shares the rounded-rect SDF of Quad, so a positive radius rounds the textured box's
        /// corners exactly as a Panel background rounds — the textured-quad path runs through the
        /// same shape fragment. The default (zero) radius is the plain square textured quad.
        /// @param rect     The rectangle, in framebuffer pixels.
        /// @param texture  Bindless texture slot to sample.
        /// @param sampler  Bindless sampler slot to sample with.
        /// @param uv       UV rectangle to sample (defaults to the whole texture).
        /// @param tint     Multiplied over the sampled texel, linear straight-alpha RGBA.
        /// @param radii    Per-corner radius; the shape path uses the uniform radius (zero for square).
        void Texture(const Rect& rect, Renderer::TextureHandle texture,
                     Renderer::SamplerHandle sampler,
                     const Rect& uv = {.Min = {0.0f, 0.0f}, .Size = {1.0f, 1.0f}},
                     vec4 tint = vec4(1.0f), const CornerRadii& radii = {});

        /// @brief Appends a nine-slice frame: a texture split into corners, edges, and center.
        ///
        /// The source texture is divided into a 3×3 grid by the slice insets (in texture UV
        /// [0,1]); the destination rectangle is divided by the same insets in pixels. Corners
        /// keep their size, edges stretch along one axis, and the center stretches both — the
        /// standard resizable-panel-art primitive. Emitted as nine textured quads.
        ///
        /// ImageRepeat::Tile replaces those stretches with repeats of each cell's own source
        /// sub-rect: the four corners are fixed-size by definition and never repeat, each edge
        /// repeats along its growing axis only, and the center repeats on both. A repeating cell
        /// carries its sub-rect in GuiVertex::UvWrap and spans (cell destination ÷ cell source)
        /// copies of it, which is why the repeat count is derived rather than authored — exactly as
        /// an unsliced tiled fill derives it from the box and the texture size.
        /// @param rect     The destination rectangle, in framebuffer pixels.
        /// @param texture  Bindless texture slot to sample.
        /// @param sampler  Bindless sampler slot to sample with.
        /// @param sliceUv  The 3×3 split as fractions of the sampled sub-rect in [0,1].
        /// @param sizePx   The corner/edge sizes in the destination, in pixels.
        /// @param tint     Multiplied over the sampled texels, linear straight-alpha RGBA.
        /// @param uv       The sub-rect of the texture the 3×3 split divides (the whole texture by
        ///                 default), so an atlas region frames exactly as a standalone texture does.
        /// @param repeat   Whether the stretchable cells stretch (the default) or repeat.
        /// @param sourcePx The sampled sub-rect's size in texels, which is what a repeating cell's
        ///                 count is measured against; ignored when the cells stretch, and a
        ///                 degenerate (non-positive) axis stretches rather than dividing by zero.
        void NineSlice(const Rect& rect, Renderer::TextureHandle texture,
                       Renderer::SamplerHandle sampler, const Insets& sliceUv, const Insets& sizePx,
                       vec4 tint = vec4(1.0f),
                       const Rect& uv = {.Min = {0.0f, 0.0f}, .Size = {1.0f, 1.0f}},
                       ImageRepeat repeat = ImageRepeat::Stretch, vec2 sourcePx = vec2(0.0f));

        /// @brief Appends a run of shaped text at a pen origin.
        ///
        /// Shapes the string through the font (Font::ShapeRun) and emits one MSDF glyph quad
        /// per positioned glyph, tagged as a text run bound to the font's atlas. The pen is the
        /// top-left of the shaped block, in framebuffer pixels.
        /// @param pen        Top-left origin of the shaped block, in framebuffer pixels.
        /// @param font       The resident font whose atlas and metrics drive shaping.
        /// @param text       The UTF-8 text to shape and draw.
        /// @param pixelSize  The em size to render at, in pixels.
        /// @param color      Text tint, linear straight-alpha RGBA.
        /// @param maxWidth   Width to word-wrap within, or empty for a single unwrapped line per
        ///                   newline. A caller passes the same value its measure was taken at, so
        ///                   the run drawn is the run the box was sized for.
        void Text(vec2 pen, const Font& font, string_view text, f32 pixelSize, vec4 color,
                  optional<f32> maxWidth = {});

        /// @brief Appends a run of text that was already shaped through the font.
        ///
        /// The run's glyph positions are used as they stand: nothing is shaped again. Each glyph is
        /// only ensured resident, which pins it against eviction for the frame and fetches its atlas
        /// page and UV rect, and its quad is placed from the glyph's Pen and the resident
        /// rendition's own bounds. So a caller that keeps a ShapeResult across frames draws it every
        /// frame without re-shaping, and a run shaped device-free (TextShapeMode::Measure) draws
        /// exactly as one shaped to draw.
        /// @param pen        Top-left origin of the shaped block, in framebuffer pixels.
        /// @param font       The resident font the run was shaped through.
        /// @param shaped     The shaped run.
        /// @param pixelSize  The em size the run was shaped at, in pixels.
        /// @param color      Text tint, linear straight-alpha RGBA.
        void Text(vec2 pen, const Font& font, const ShapeResult& shaped, f32 pixelSize, vec4 color);

        /// @brief Appends a paragraph shaped through Font::ShapeSpans, each span in its own colour.
        ///
        /// Draws as the single-run overload does — nothing is shaped again, each glyph is only
        /// ensured resident — except that each glyph is placed at the size it was shaped at
        /// (ShapedGlyph::PixelSize) and tinted by the colour of its span (ShapedGlyph::Span). A glyph
        /// whose span has no colour is skipped.
        /// @param pen     Top-left origin of the shaped block, in framebuffer pixels.
        /// @param font    The resident font the paragraph was shaped through.
        /// @param shaped  The shaped paragraph.
        /// @param colors  Each span's tint, linear straight-alpha RGBA, indexed by span.
        void Text(vec2 pen, const Font& font, const ShapeResult& shaped,
                  std::span<const vec4> colors);

        /// @brief Appends another draw list's geometry with every vertex position remapped.
        ///
        /// Copies @p src's vertices (each position run through @p project), indices, runs, and
        /// gradient records into this list, offsetting index and gradient references so the two
        /// streams concatenate — the way several projected overlays merge into one screen-space list a
        /// single pass records. A run's clip rectangle is projected to a screen-space bounding
        /// scissor (dropped to full-surface when a corner maps behind the eye). @p project returns
        /// nullopt for a point behind the projection's eye; a single such vertex culls the whole
        /// source (a flat plane crossing the eye plane shows nothing) and nothing is appended.
        /// @param src      The source draw list to append.
        /// @param project  Maps a source position to an output position, or nullopt to cull.
        /// @return True when the source was appended; false when it was culled behind the eye.
        bool AppendProjected(const DrawList& src, const function<optional<vec2>(vec2)>& project);

        /// @brief Pushes a clip rectangle onto the scissor stack.
        ///
        /// The new clip is intersected with the current top of the stack, so nested clips
        /// narrow monotonically. Subsequent primitives are tagged with the intersected clip
        /// until the matching PopClip.
        /// @param rect  The clip rectangle, in framebuffer pixels.
        void PushClip(const Rect& rect);

        /// @brief Pops the top clip rectangle off the scissor stack.
        /// @pre A matching PushClip was issued — popping an empty stack is a fatal assert.
        void PopClip();

        /// @brief Pushes a rotation about a pivot onto the transform stack.
        ///
        /// Every subsequently emitted primitive has its corner positions rotated by this angle
        /// about the pivot until the matching PopTransform. The new transform composes onto the
        /// current top (like PushClip intersects), so a rotation pushed under another rotation
        /// rotates within the enclosing frame — nested rotations accumulate. Only vertex positions
        /// transform; the shape SDF's local box coordinate (RectHalf / RectCoord) and every UV are
        /// left in their unrotated space, so rounded corners, borders, gradients, textures, and MSDF
        /// glyphs rotate rigidly with no shader change, and run batching is unaffected.
        /// @param pivot    The center of rotation, in framebuffer pixels.
        /// @param radians  The rotation angle in radians, clockwise-positive in the y-down space.
        void PushTransform(vec2 pivot, f32 radians);

        /// @brief Pops the top transform off the transform stack.
        /// @pre A matching PushTransform was issued — popping an empty stack is a fatal assert.
        void PopTransform();

        /// @brief Pushes an arc silhouette that masks every subsequent shape and material quad.
        ///
        /// Until the matching PopArc, each quad a shape-path or material primitive emits (Quad,
        /// Shadow, Gradient, MaterialFill, Texture, NineSlice) carries the arc in its GuiVertex::Arc
        /// and ArcCoord lanes, and the fragment takes the annular-sector signed distance in place of
        /// the rounded-rect one — for the fill's coverage, a border's ring (inset along the arc), and
        /// a shadow's silhouette (displaced by its offset, grown by its spread) alike. Every quad
        /// under one arc is masked by that one arc, whatever its own rect: a background image in the
        /// padding box, a fitted texture, and each cell of a nine-slice all cut to the same sector.
        /// MSDF text quads never carry it.
        ///
        /// The top of the stack applies alone — a pushed arc replaces the enclosing one rather than
        /// intersecting it, since two sectors do not compose into a sector. The arc does not narrow
        /// the run's scissor and does not change batching.
        /// @param arc  The sector, in the untransformed space of the primitives it masks.
        void PushArc(const ArcShape& arc);

        /// @brief Pops the top arc silhouette, restoring the enclosing one (or the rounded box).
        /// @pre A matching PushArc was issued — popping an empty stack is a fatal assert.
        void PopArc();

        /// @brief Returns the interleaved vertex stream.
        [[nodiscard]] const vector<GuiVertex>& GetVertices() const { return m_Vertices; }

        /// @brief Returns the index stream referencing the vertex stream.
        [[nodiscard]] const vector<u32>& GetIndices() const { return m_Indices; }

        /// @brief Returns the run table partitioning the index stream.
        [[nodiscard]] const vector<DrawRun>& GetRuns() const { return m_Runs; }

        /// @brief Returns the gradient records, indexed by a vertex's GradientSelector minus one.
        [[nodiscard]] const vector<GpuGradient>& GetGradients() const { return m_Gradients; }

        /// @brief Returns whether the draw list has no geometry.
        [[nodiscard]] bool IsEmpty() const { return m_Runs.empty(); }

        /// @brief Returns the current position in every stream, for bracketing what is emitted next.
        [[nodiscard]] DrawMark Mark() const;

        /// @brief Returns the clip, transform, and arc currently in force.
        [[nodiscard]] DrawState GetState() const;

        /// @brief Returns whether any clip, transform, or arc is currently pushed.
        [[nodiscard]] bool HasOpenState() const
        {
            return !m_ClipStack.empty() || !m_TransformStack.empty() || !m_ArcStack.empty();
        }

        /// @brief Sets whether drawn text records which glyphs it drew and where they sat.
        ///
        /// A recording list keeps, per glyph drawn, the font, codepoint, size, and the atlas slot
        /// the glyph sampled, so EnsureGlyphs can later pin a range's glyphs again and tell whether
        /// any of them moved in the atlas. Off by default: only a list that is replayed from needs it.
        /// @param records  True to record each drawn glyph.
        void SetRecordsGlyphs(bool records) { m_RecordsGlyphs = records; }

        /// @brief Ensures every glyph a bracketed range drew is resident again, in the slot it drew from.
        ///
        /// Re-ensures each recorded glyph between the marks, which pins it for this frame exactly as
        /// drawing it would. Returns false when any glyph now resolves to a different page or UV rect
        /// — evicted and repacked since, or newly resident where it was missing — because the range's
        /// copied geometry would then sample the wrong texels; the caller re-emits instead. Requires a
        /// list that recorded glyphs while the range was emitted.
        /// @param begin  The mark opening the range.
        /// @param end    The mark closing the range.
        /// @return True when every glyph in the range still sits where the range's geometry samples.
        [[nodiscard]] bool EnsureGlyphs(const DrawMark& begin, const DrawMark& end) const;

        /// @brief Appends the geometry another list emitted between two of its marks.
        ///
        /// Copies the bracketed vertices, indices, gradient records, and recorded glyphs, rebasing
        /// every index and gradient reference, and re-partitions the copied indices into runs with
        /// the same merge rule a primitive follows — so the result is the run table emitting the same
        /// primitives here would have produced. The copied runs keep the clip they were recorded
        /// under, so the range reproduces exactly only when this list's state Matches the state the
        /// range began under; that check is the caller's.
        /// @param src    The list to copy from; must not be this list.
        /// @param begin  The mark in `src` opening the range.
        /// @param end    The mark in `src` closing the range.
        void AppendRange(const DrawList& src, const DrawMark& begin, const DrawMark& end);

        /// @brief Appends another list's whole geometry, as AppendRange over all of it.
        /// @param src  The list to copy from; must not be this list.
        void Append(const DrawList& src);

    private:
        /// @brief An affine transform applied to vertex positions: a linear part and a translation.
        ///
        /// Maps a position p to Linear * p + Translation. A push builds a rotation-about-pivot and
        /// composes it onto the enclosing transform, so the stack top is always the full local→output
        /// map every emitted position runs through.
        struct AffineTransform
        {
            /// @brief The 2×2 linear part (a rotation).
            mat2 Linear{1.0f};
            /// @brief The translation added after the linear part.
            vec2 Translation{0.0f};
        };

        /// @brief Returns the current effective clip (top of the stack), or nullopt when unclipped.
        [[nodiscard]] optional<Rect> CurrentClip() const;

        /// @brief Applies the top transform to a position, or returns it unchanged when the stack is empty.
        /// @param point  A position in framebuffer pixels.
        /// @return The transformed position, or the input when no transform is active.
        [[nodiscard]] vec2 ApplyTransform(vec2 point) const;

        /// @brief Ensures the trailing run matches the key, opening a new run when it differs.
        ///
        /// The run-partitioning core: a primitive that shares {pipeline, clip, texture, material}
        /// with the trailing run extends it; any difference (including a change of clip nesting)
        /// opens a new run. The texture key is folded into the params-carried index, so a distinct
        /// texture is a distinct run even at the same pipeline and clip; the material is the same
        /// key one level up, since two material fills share a run only when one pipeline and one
        /// parameter block serve both.
        /// @param pipeline    The pipeline this primitive draws with.
        /// @param textureKey  The bindless texture index keying the run (Invalid for untextured shapes).
        /// @param material    The material instance keying the run (null for every non-material run).
        void EnsureRun(GuiPipeline pipeline, u32 textureKey,
                       const MaterialInstance* material = nullptr);

        /// @brief EnsureRun against an explicit clip rather than the clip stack's top.
        /// @param pipeline    The pipeline this primitive draws with.
        /// @param textureKey  The bindless texture index keying the run.
        /// @param material    The material instance keying the run.
        /// @param clip        The absolute clip the run takes, or nullopt for none.
        void EnsureRunKeyed(GuiPipeline pipeline, u32 textureKey, const MaterialInstance* material,
                            const optional<Rect>& clip);

        /// @brief Appends one axis-aligned quad (four vertices, six indices) into the current run.
        /// @param corners   The four corner positions in framebuffer pixels (TL, TR, BR, BL order).
        /// @param uvs        The four corner UVs matching the corner order.
        /// @param color      Per-vertex color, linear straight-alpha RGBA.
        /// @param rectHalf   Shape half-extent for the SDF (zero for text/texture).
        /// @param center     Rect center in pixels, for the per-vertex RectCoord (shape path).
        /// @param params     Packed fragment params written to every vertex.
        /// @param selector   Gradient record selector (record index plus one); zero for no gradient.
        /// @param shadow     Shadow parameters (see GuiVertex::Shadow); zero for an ordinary quad.
        /// @param uvWrap     UV sub-rect to wrap within (see GuiVertex::UvWrap); zero for no wrap.
        void PushQuad(const std::array<vec2, 4>& corners, const std::array<vec2, 4>& uvs,
                      vec4 color, vec2 rectHalf, vec2 center, vec4 params, u32 selector = 0,
                      vec4 shadow = vec4(0.0f), vec4 uvWrap = vec4(0.0f));

        /// @brief Ensures one shaped glyph resident and emits its quad, recording it when asked.
        /// @param pen        Top-left origin of the shaped block, in framebuffer pixels.
        /// @param font       The resident font the glyph was shaped through.
        /// @param shaped     The shaped glyph.
        /// @param pixelSize  The em size it draws at, in pixels.
        /// @param color      Its tint, linear straight-alpha RGBA.
        void EmitGlyph(vec2 pen, const Font& font, const ShapedGlyph& shaped, f32 pixelSize,
                       vec4 color);

        /// @brief Emits one textured quad, opening a Shape run keyed by its texture.
        /// @param radii   Per-corner radius; the shape path uses the uniform radius (zero for square).
        /// @param uvWrap  UV sub-rect the fragment wraps sampling within; zero samples the UV as-is.
        void EmitTexturedQuad(const Rect& rect, Renderer::TextureHandle texture,
                              Renderer::SamplerHandle sampler, const Rect& uv, vec4 tint,
                              const CornerRadii& radii = {}, vec4 uvWrap = vec4(0.0f));

        /// @brief The interleaved vertex stream.
        vector<GuiVertex> m_Vertices;
        /// @brief The index stream.
        vector<u32> m_Indices;
        /// @brief The run table over the index stream.
        vector<DrawRun> m_Runs;
        /// @brief The gradient records a Gradient() appends to, indexed by a vertex's selector minus one.
        vector<GpuGradient> m_Gradients;
        /// @brief The active clip stack; each entry is already intersected with the one below it.
        vector<Rect> m_ClipStack;
        /// @brief The active transform stack; each entry is already composed with the one below it.
        vector<AffineTransform> m_TransformStack;
        /// @brief The active arc silhouettes; only the top entry masks an emitted quad.
        vector<ArcShape> m_ArcStack;
        /// @brief Bindless texture index keying the trailing run (Invalid for an untextured shape run).
        ///
        /// The run table stores no texture, so the merge test in EnsureRun compares this against the
        /// incoming key to keep a distinct texture in its own run.
        u32 m_RunTextureKey = Renderer::TextureHandle::Invalid;
        /// @brief Material instance keying the trailing run; null on a non-material run.
        ///
        /// The run table does carry its material (the pass needs it to bind), so this mirrors the
        /// trailing run's value only to keep the merge test in EnsureRun uniform with the texture key.
        const MaterialInstance* m_RunMaterial = nullptr;
        /// @brief Each run's texture key, parallel to m_Runs, so a copied range re-partitions exactly.
        vector<u32> m_RunTextureKeys;

        /// @brief One glyph a recording list drew, and the atlas slot it sampled.
        struct GlyphUse
        {
            /// @brief The font the glyph was drawn through; resident while its run is replayed.
            const Veng::Font* Source = nullptr;
            /// @brief The glyph's codepoint.
            u32 Codepoint = 0;
            /// @brief The size it was drawn at, in pixels.
            f32 PixelSize = 0.0f;
            /// @brief The atlas page it sampled; Invalid when it was not resident and drew nothing.
            u32 Page = Renderer::TextureHandle::Invalid;
            /// @brief The UV corner it sampled from.
            vec2 UvMin{0.0f};
        };
        /// @brief The glyphs drawn while recording, in draw order.
        vector<GlyphUse> m_GlyphUses;
        /// @brief Whether Text records each glyph it draws into m_GlyphUses.
        bool m_RecordsGlyphs = false;
    };
}
