#pragma once

#include <array>

#include <Veng/Veng.h>
#include <Veng/Renderer/BindlessRegistry.h>

namespace Veng
{
    class Font;
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

    /// @brief Selects which fragment path a run replays.
    enum class GuiPipeline : u8
    {
        /// @brief Rounded-rect SDF with optional border and optional texture modulation.
        Shape,
        /// @brief MSDF glyph coverage sampled from a font atlas.
        Msdf,
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
    };

    /// @brief A contiguous slice of the index stream sharing one pipeline, clip, and texture.
    ///
    /// The pass replays runs in order, changing pipeline / scissor / bound texture only at run
    /// boundaries. A run's clip is already intersected with the enclosing clip stack, so the
    /// pass applies it as an absolute scissor with no further nesting math.
    struct DrawRun
    {
        /// @brief The fragment path this run replays.
        GuiPipeline Pipeline = GuiPipeline::Shape;
        /// @brief First index of this run in the draw list's index stream.
        u32 FirstIndex = 0;
        /// @brief Number of indices in this run.
        u32 IndexCount = 0;
        /// @brief Absolute scissor rectangle in framebuffer pixels; the whole surface when unclipped.
        Rect Clip;
        /// @brief True when Clip is a real clip rectangle; false means "no scissor" (full surface).
        bool HasClip = false;
    };

    /// @brief A device-free command buffer of UI primitives resolving to one geometry stream.
    ///
    /// Each primitive call (Quad / Texture / NineSlice / Text) appends geometry to a single
    /// interleaved vertex/index stream and extends or opens a run keyed by {pipeline, clip,
    /// texture}. PushClip / PopClip maintain a scissor stack whose entries intersect. A render
    /// pass consumes GetVertices() / GetIndices() / GetRuns() and replays each run. Colors are
    /// linear by contract; the pass blends them in linear space.
    class DrawList
    {
    public:
        /// @brief Constructs an empty draw list.
        DrawList() = default;

        /// @brief Clears all geometry, runs, and the clip stack for reuse across frames.
        void Clear();

        /// @brief Appends a filled or bordered rounded rectangle.
        /// @param rect     The rectangle, in framebuffer pixels.
        /// @param color    Fill color, linear straight-alpha RGBA (ignored where a border replaces it).
        /// @param radii    Per-corner radius; the shape path uses the uniform radius.
        /// @param border   Optional border; a positive width draws a ring in the border color.
        void Quad(const Rect& rect, vec4 color, const CornerRadii& radii = {},
                  const Border& border = {});

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
        /// @param rect     The destination rectangle, in framebuffer pixels.
        /// @param texture  Bindless texture slot to sample.
        /// @param sampler  Bindless sampler slot to sample with.
        /// @param sliceUv  The 3×3 split as fractions of the sampled sub-rect in [0,1].
        /// @param sizePx   The corner/edge sizes in the destination, in pixels.
        /// @param tint     Multiplied over the sampled texels, linear straight-alpha RGBA.
        /// @param uv       The sub-rect of the texture the 3×3 split divides (the whole texture by
        ///                 default), so an atlas region frames exactly as a standalone texture does.
        void NineSlice(const Rect& rect, Renderer::TextureHandle texture,
                       Renderer::SamplerHandle sampler, const Insets& sliceUv, const Insets& sizePx,
                       vec4 tint = vec4(1.0f),
                       const Rect& uv = {.Min = {0.0f, 0.0f}, .Size = {1.0f, 1.0f}});

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
        void Text(vec2 pen, const Font& font, string_view text, f32 pixelSize, vec4 color);

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
        /// The run-partitioning core: a primitive that shares {pipeline, clip, texture} with the
        /// trailing run extends it; any difference (including a change of clip nesting) opens a new
        /// run. The texture key is folded into the params-carried index, so a distinct texture is a
        /// distinct run even at the same pipeline and clip.
        /// @param pipeline    The pipeline this primitive draws with.
        /// @param textureKey  The bindless texture index keying the run (Invalid for untextured shapes).
        void EnsureRun(GuiPipeline pipeline, u32 textureKey);

        /// @brief Appends one axis-aligned quad (four vertices, six indices) into the current run.
        /// @param corners   The four corner positions in framebuffer pixels (TL, TR, BR, BL order).
        /// @param uvs        The four corner UVs matching the corner order.
        /// @param color      Per-vertex color, linear straight-alpha RGBA.
        /// @param rectHalf   Shape half-extent for the SDF (zero for text/texture).
        /// @param center     Rect center in pixels, for the per-vertex RectCoord (shape path).
        /// @param params     Packed fragment params written to every vertex.
        /// @param selector   Gradient record selector (record index plus one); zero for no gradient.
        /// @param shadow     Shadow parameters (see GuiVertex::Shadow); zero for an ordinary quad.
        void PushQuad(const std::array<vec2, 4>& corners, const std::array<vec2, 4>& uvs,
                      vec4 color, vec2 rectHalf, vec2 center, vec4 params, u32 selector = 0,
                      vec4 shadow = vec4(0.0f));

        /// @brief Emits one textured quad, opening a Shape run keyed by its texture.
        /// @param radii  Per-corner radius; the shape path uses the uniform radius (zero for square).
        void EmitTexturedQuad(const Rect& rect, Renderer::TextureHandle texture,
                              Renderer::SamplerHandle sampler, const Rect& uv, vec4 tint,
                              const CornerRadii& radii = {});

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
        /// @brief Bindless texture index keying the trailing run (Invalid for an untextured shape run).
        ///
        /// The run table stores no texture, so the merge test in EnsureRun compares this against the
        /// incoming key to keep a distinct texture in its own run.
        u32 m_RunTextureKey = Renderer::TextureHandle::Invalid;
    };
}
