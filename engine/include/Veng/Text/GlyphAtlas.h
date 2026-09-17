#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Text/GlyphSource.h>

namespace Veng::Renderer
{
    class Context;
    class CommandBuffer;
    class Image;
    class ImageView;
}

namespace Veng::Text
{
    class GlyphPacker;

    /// @brief The identity of one glyph rendition in the atlas.
    ///
    /// A glyph is resident once per distinct key. Two of the fields carry a size policy that
    /// differs by field type: an Sdf rendition is size-dependent, so QuantizedPixelSize bins the
    /// requested draw size into a small set of buckets (many draw sizes share one bucket); an Msdf
    /// rendition is size-independent — one field reconstructs coverage at every scale — so its
    /// QuantizedPixelSize is a single constant across all sizes. GlyphAtlas::KeyFor applies that
    /// policy; a caller may also build a key directly.
    struct GlyphKey
    {
        /// @brief The face the glyph is drawn from, in the GlyphSource that rasterizes it.
        FaceId Face = FaceId::Invalid;
        /// @brief The face's own glyph index (from GlyphSource::GlyphIndex).
        u32 GlyphIndex = 0;
        /// @brief The rasterization pixel size, quantized: an Sdf size bucket, or the fixed Msdf size.
        u32 QuantizedPixelSize = 0;
        /// @brief The signed-distance field representation this rendition encodes.
        GlyphFieldType FieldType = GlyphFieldType::Msdf;

        /// @brief Orders keys so a residency map can store them; identity is all four fields.
        friend auto operator<=>(const GlyphKey&, const GlyphKey&) = default;
        /// @brief Two keys are equal when every field matches.
        friend bool operator==(const GlyphKey&, const GlyphKey&) = default;
    };

    /// @brief Where a glyph lives in the atlas, and whether it can be sampled yet.
    ///
    /// The result of Ensure. A non-resident slot is the bounded degradation the atlas returns when
    /// a frame's distinct-glyph working set exceeds the page cap: the draw path renders .notdef for
    /// it rather than evicting a glyph the same frame still needs. A resident slot names the page
    /// (as a bindless TextureHandle) and the glyph's texel rectangle within it (as normalized UVs),
    /// plus the size-independent em-space placement metrics the layout reuses. An empty (whitespace)
    /// glyph is resident but occupies no page space — only its advance is meaningful.
    struct GlyphSlot
    {
        /// @brief Whether the glyph is packed and its pixels are (or will be) on a page this frame.
        bool Resident = false;
        /// @brief Whether the glyph carries no pixels (whitespace): resident, advance-only.
        bool Empty = false;
        /// @brief The field representation of the resident rendition.
        GlyphFieldType FieldType = GlyphFieldType::Msdf;
        /// @brief The bindless handle of the page holding the glyph; invalid when not resident or empty.
        Renderer::TextureHandle Page;
        /// @brief The page's index within the atlas (for diagnostics; matches GetPageHandle).
        u32 PageIndex = 0;
        /// @brief The glyph rectangle's lower UV corner within the page, normalized to [0, 1].
        vec2 UvMin{0.0f};
        /// @brief The glyph rectangle's upper UV corner within the page, normalized to [0, 1].
        vec2 UvMax{0.0f};
        /// @brief The glyph quad's lower-left corner offset from the pen origin, in em units.
        vec2 PlaneMin{0.0f};
        /// @brief The glyph quad's upper-right corner offset from the pen origin, in em units.
        vec2 PlaneMax{0.0f};
        /// @brief The glyph's horizontal advance, in em units.
        f32 Advance = 0.0f;
        /// @brief The frame epoch the glyph's upload was scheduled in.
        ///
        /// On this frame the pixels ride the frame's copy and are sampleable once RecordUploads has
        /// recorded and the frame is submitted; the draw path reads this to tell a freshly packed
        /// glyph from one resident since a prior frame.
        u64 ResidentFromFrame = 0;
    };

    /// @brief Sizing and rasterization policy for a GlyphAtlas.
    struct GlyphAtlasInfo
    {
        /// @brief Edge length, in texels, of every atlas page (pages are square).
        u32 PageSize = 1024;
        /// @brief Maximum pages allocated per field type before eviction reclaims space.
        u32 MaxPagesPerFieldType = 4;
        /// @brief Empty texels kept between packed glyphs so a bilinear tap never bleeds a neighbour.
        u32 Gutter = 1;
        /// @brief The signed-distance range, in pixels, both field types encode (the shader constant).
        u32 DistanceRangePx = 4;
        /// @brief The fixed pixel size every Msdf glyph is rasterized at (Msdf is size-independent).
        u32 MsdfPixelSize = 48;
        /// @brief The bucket step, in pixels, an Sdf draw size is quantized to.
        u32 SdfSizeQuantum = 8;
        /// @brief The smallest Sdf rasterization size a quantized bucket clamps up to.
        u32 MinSdfPixelSize = 8;
    };

    /// @brief A dynamic, paged, LRU-evicting glyph atlas shared across every font.
    ///
    /// Glyphs are rasterized on first request through the GlyphSource, packed into a bindless page
    /// of their field type (Msdf and Sdf never share a page — their formats and sampling differ),
    /// and uploaded into that page's sub-rectangle. A page fills, a new one is allocated up to the
    /// per-field-type cap, and once the cap is reached the least-recently-used glyphs from a prior
    /// frame are evicted to reclaim space; a glyph evicted and later re-requested simply
    /// re-rasterizes into a fresh slot. Residency is therefore a cache, never a correctness event.
    ///
    /// Two frame invariants make the cache safe to draw through. A glyph ensured this frame is
    /// **pinned** this frame — eviction never touches a slot whose frame epoch is the current one —
    /// so an ensure-then-draw loop cannot evict a glyph it is still about to draw, and every ensured
    /// glyph stays resident for the frame. When a single frame's working set exceeds the pinned
    /// capacity, the surplus Ensure calls return **not-resident** rather than dropping a pinned
    /// slot; the draw path renders .notdef for those, a bounded, visible degradation. The upload
    /// rides the frame's own command buffer on the graphics queue (RecordUploads), so a reclaimed
    /// rectangle is never overwritten while a prior frame's draw could still sample it: the
    /// overwrite is a later submit on the same queue than every prior frame's read, and the
    /// this-frame pin covers the current frame's own reads.
    ///
    /// Single-threaded with the renderer: Ensure, BeginFrame and RecordUploads run on the main
    /// thread only, matching the GlyphSource contract. The atlas holds no cross-run state — it is
    /// rebuilt from the faces each session.
    class GlyphAtlas
    {
    public:
        /// @brief Constructs an empty atlas; pages are allocated lazily on first Ensure.
        /// @param context The owning render context; the atlas must not outlive it.
        /// @param source  The rasterizer glyphs are produced through; must outlive the atlas.
        /// @param info    Sizing and rasterization policy.
        GlyphAtlas(Renderer::Context& context, GlyphSource& source,
                   const GlyphAtlasInfo& info = {});

        /// @brief Retires every page's GPU resources; must run before the context tears down.
        ~GlyphAtlas();

        GlyphAtlas(const GlyphAtlas&) = delete;
        GlyphAtlas& operator=(const GlyphAtlas&) = delete;
        GlyphAtlas(GlyphAtlas&&) = delete;
        GlyphAtlas& operator=(GlyphAtlas&&) = delete;

        /// @brief Advances the frame epoch: last frame's slots become evictable again.
        ///
        /// Call once per frame, before the frame's Ensure calls, so glyphs ensured this frame pin
        /// against eviction and glyphs untouched since a prior frame are eligible.
        void BeginFrame();

        /// @brief Builds the canonical key for a glyph at a draw size, applying the size policy.
        ///
        /// Msdf renditions fold to one size-independent key per face+glyph; Sdf renditions quantize
        /// the draw size into a bucket. Ensure rasterizes at the key's QuantizedPixelSize.
        /// @param face       The face the glyph is drawn from.
        /// @param glyphIndex The face's glyph index.
        /// @param pixelSize  The desired draw size in pixels (the Sdf bucket source; ignored for Msdf).
        /// @param fieldType  The field representation to key.
        [[nodiscard]] GlyphKey KeyFor(FaceId face, u32 glyphIndex, f32 pixelSize,
                                      GlyphFieldType fieldType) const;

        /// @brief Ensures a glyph is resident, rasterizing, packing and scheduling its upload on a miss.
        ///
        /// A hit bumps the glyph's LRU stamp and returns its slot. A miss rasterizes through the
        /// GlyphSource, packs into a page of the key's field type (allocating a page or evicting a
        /// prior frame's LRU glyphs as needed), stages the pixels for the next RecordUploads, and
        /// returns the new slot. When the field type's pages are full of this-frame-pinned glyphs,
        /// the returned slot is not resident.
        /// @param key The glyph to ensure (see KeyFor for the canonical construction).
        /// @return The glyph's slot; Resident is false when the frame's working set exceeds the cap.
        [[nodiscard]] GlyphSlot Ensure(const GlyphKey& key);

        /// @brief Records every glyph staged since the last call into `cmd`, on the graphics queue.
        ///
        /// One staging buffer and one batched copy per touched page, bracketed by the transitions
        /// that leave the page sampleable this frame. Call once per frame, inside the frame's
        /// command recording, before any draw samples the atlas.
        /// @param cmd The frame's command buffer.
        void RecordUploads(Renderer::CommandBuffer& cmd);

        /// @brief Returns the number of pages currently allocated across all field types.
        [[nodiscard]] u32 GetPageCount() const;

        /// @brief Returns the shared bindless sampler every page is sampled through.
        ///
        /// A clamp-to-edge linear sampler suited to the padded distance fields, acquired once from
        /// the shared registry cache and held for the atlas's lifetime. The text draw path packs
        /// its index into the per-glyph draw params.
        [[nodiscard]] Renderer::SamplerHandle GetSamplerHandle() const { return m_SamplerHandle; }

        /// @brief Returns the signed-distance range, in atlas texels, both field types encode.
        ///
        /// The single shader constant the text coverage math divides screen-space distance by
        /// (scaled by the draw size). One value serves Msdf and Sdf alike, since both rasterize
        /// over this same range.
        [[nodiscard]] f32 GetDistanceRange() const
        {
            return static_cast<f32>(m_Info.DistanceRangePx);
        }

        /// @brief Returns a page's bindless handle, for a draw that samples it.
        /// @param page A page index in [0, GetPageCount()).
        [[nodiscard]] Renderer::TextureHandle GetPageHandle(u32 page) const;

        /// @brief Returns a page's sampled view, for a diagnostic that inspects or reads it back.
        /// @param page A page index in [0, GetPageCount()).
        [[nodiscard]] Ref<Renderer::ImageView> GetPageView(u32 page) const;

        /// @brief Returns the current frame epoch (advanced by BeginFrame).
        [[nodiscard]] u64 GetCurrentFrame() const;

    private:
        /// @brief One GPU atlas page: its image, a sampled view, its bindless slot, and field type.
        struct Page
        {
            /// @brief The page image (RGBA8 for Msdf, R8 for Sdf).
            Ref<Renderer::Image> Image;
            /// @brief The full sampled view registered into the bindless set.
            Ref<Renderer::ImageView> View;
            /// @brief The page's bindless texture slot.
            Renderer::TextureHandle Handle;
            /// @brief The field type every glyph on this page shares.
            GlyphFieldType FieldType = GlyphFieldType::Msdf;
        };

        /// @brief A glyph's pixels staged for the next RecordUploads.
        struct Pending
        {
            /// @brief The destination page index.
            u32 Page = 0;
            /// @brief The glyph's top-left texel within the page.
            uvec2 Offset{0};
            /// @brief The glyph's texel dimensions.
            uvec2 Size{0};
            /// @brief Bytes per texel (1 for Sdf, 4 for Msdf).
            u32 Channels = 0;
            /// @brief The rasterized bitmap, row-major top-down, Size.x * Size.y * Channels bytes.
            vector<u8> Pixels;
        };

        /// @brief A page rectangle to zero before the next RecordUploads' glyph copies.
        ///
        /// A newly created page is cleared whole and an evicted glyph's rectangle is re-cleared, so
        /// every inter-glyph gutter texel reads as "fully outside" (zero coverage). Without it the
        /// gutter holds undefined memory, and the atlas sampler's linear filtering blends that
        /// garbage into a glyph's edge as a thin fringe.
        struct Clear
        {
            /// @brief The page to clear into.
            u32 Page = 0;
            /// @brief The rectangle's top-left texel.
            uvec2 Offset{0};
            /// @brief The rectangle's texel dimensions.
            uvec2 Size{0};
        };

        /// @brief Creates and registers a new GPU page for a field type, at the packer's page index.
        void CreatePage(u32 pageIndex, GlyphFieldType fieldType);

        /// @brief The owning render context.
        Renderer::Context& m_Context;
        /// @brief The rasterizer; borrowed, must outlive the atlas.
        GlyphSource& m_Source;
        /// @brief Sizing and rasterization policy.
        GlyphAtlasInfo m_Info;
        /// @brief The device-free residency and packing brain; page indices match m_Pages.
        Unique<GlyphPacker> m_Packer;
        /// @brief The GPU pages, indexed by the packer's page id.
        vector<Page> m_Pages;
        /// @brief The shared clamp-to-edge linear sampler every page is sampled through.
        Renderer::SamplerHandle m_SamplerHandle;
        /// @brief Glyphs staged since the last RecordUploads.
        vector<Pending> m_Pending;
        /// @brief Page rectangles to zero ahead of the staged glyphs on the next RecordUploads.
        vector<Clear> m_Clears;
        /// @brief The current frame epoch.
        u64 m_Frame = 0;
    };
}
