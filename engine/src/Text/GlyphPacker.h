#pragma once

#include <Veng/Veng.h>
#include <Veng/Text/GlyphAtlas.h>
#include <Veng/Text/GlyphSource.h>

#include <map>
#include <optional>

namespace Veng::Text
{
    /// @brief The device-free residency and rectangle-packing brain behind GlyphAtlas.
    ///
    /// Owns the whole cache policy — the key→slot residency map, per-page rectangle packing, LRU
    /// eviction, the this-frame pin, per-field-type page allocation up to a cap, and the
    /// not-resident result when a frame's working set overflows — with no reference to any GPU
    /// object, so it is unit-testable apart from a device. GlyphAtlas is the thin GPU wrapper that
    /// creates a page whenever this reports one allocated and uploads pixels into the rectangle this
    /// hands back. Page indices are dense and assigned in allocation order, so they index
    /// GlyphAtlas's own page list directly.
    ///
    /// Packing is a guillotine free-rectangle allocator per page: a placement takes the
    /// smallest-area free rectangle that fits and splits the remainder into a right and a bottom
    /// rectangle; eviction returns a glyph's rectangle to the page's free list. Freed rectangles are
    /// not coalesced, so a glyph much larger than any single freed rectangle may not reuse the
    /// scattered space of several — bounded in practice because glyph renditions cluster in size and
    /// a re-requested rendition re-rasterizes at the same size a freed one held.
    class GlyphPacker
    {
    public:
        /// @brief Packing policy.
        struct Config
        {
            /// @brief Edge length, in texels, of every square page.
            u32 PageSize = 1024;
            /// @brief Empty texels reserved on a glyph's right and bottom so neighbours never bleed.
            u32 Gutter = 1;
            /// @brief Pages allocated per field type before eviction must reclaim space.
            u32 MaxPagesPerFieldType = 4;
        };

        /// @brief Size-independent placement data supplied at Insert and carried back unchanged.
        struct Metrics
        {
            /// @brief The glyph bitmap's texel dimensions; (0, 0) marks whitespace (no page rectangle).
            uvec2 Size{0};
            /// @brief The glyph quad's lower-left corner offset from the pen origin, in em units.
            vec2 PlaneMin{0.0f};
            /// @brief The glyph quad's upper-right corner offset from the pen origin, in em units.
            vec2 PlaneMax{0.0f};
            /// @brief The glyph's horizontal advance, in em units.
            f32 Advance = 0.0f;
        };

        /// @brief A resident glyph's placement and cache stamps.
        struct Slot
        {
            /// @brief The page index holding the glyph (meaningless when Empty).
            u32 Page = 0;
            /// @brief The glyph's top-left texel within the page.
            uvec2 Offset{0};
            /// @brief The glyph's texel dimensions.
            uvec2 Size{0};
            /// @brief The field type of the page the glyph sits on.
            GlyphFieldType FieldType = GlyphFieldType::Msdf;
            /// @brief Whether the glyph carries no pixels (whitespace): resident, occupies no space.
            bool Empty = false;
            /// @brief The glyph quad's lower-left corner offset from the pen origin, in em units.
            vec2 PlaneMin{0.0f};
            /// @brief The glyph quad's upper-right corner offset from the pen origin, in em units.
            vec2 PlaneMax{0.0f};
            /// @brief The glyph's horizontal advance, in em units.
            f32 Advance = 0.0f;
            /// @brief The frame epoch the glyph was last ensured in (LRU order and the this-frame pin).
            u64 LastFrame = 0;
            /// @brief The frame epoch the glyph was packed in (its upload is scheduled that frame).
            u64 ResidentFrame = 0;
        };

        /// @brief What an Insert did.
        enum class Outcome : u8
        {
            /// @brief The glyph was packed into a page.
            Packed,
            /// @brief No page of the field type could hold it without evicting a this-frame glyph.
            NotResident,
        };

        /// @brief The outcome of packing one glyph, plus what it cost.
        struct Result
        {
            /// @brief Whether the glyph was packed or refused.
            Outcome Outcome = Outcome::NotResident;
            /// @brief The packed slot; valid only when Outcome is Packed.
            Slot Slot;
            /// @brief Whether a new page was allocated to hold this glyph.
            bool NewPage = false;
            /// @brief The new page's index (valid only when NewPage).
            u32 NewPageIndex = 0;
            /// @brief The new page's field type (valid only when NewPage).
            GlyphFieldType NewPageFieldType = GlyphFieldType::Msdf;
            /// @brief The keys evicted to make room for this glyph, in eviction order.
            vector<GlyphKey> Evicted;
        };

        /// @brief Constructs a packer with no pages.
        explicit GlyphPacker(const Config& config);

        /// @brief Advances the frame epoch. Glyphs ensured in the new frame pin against eviction.
        /// @param frame The new, strictly increasing frame epoch.
        void BeginFrame(u64 frame);

        /// @brief Looks up a resident glyph, bumping its LRU stamp to the current frame.
        /// @param key The glyph to find.
        /// @return The glyph's slot, or nullopt when it is not resident.
        [[nodiscard]] std::optional<Slot> Touch(const GlyphKey& key);

        /// @brief Packs a glyph the caller has confirmed is not resident (Touch returned nullopt).
        ///
        /// Tries every page of the field type, then allocates a new page under the cap, then evicts
        /// the least-recently-used glyphs from a prior frame — one page at a time — until the glyph
        /// fits. Returns NotResident only when the field type's pages hold nothing but this-frame
        /// glyphs. A whitespace glyph (Metrics.Size is zero) is resident immediately, occupying no
        /// page space.
        /// @param key     The glyph to pack.
        /// @param metrics The glyph's dimensions and size-independent placement data.
        [[nodiscard]] Result Insert(const GlyphKey& key, const Metrics& metrics);

        /// @brief Whether a glyph is currently resident (without bumping its LRU stamp).
        [[nodiscard]] bool Contains(const GlyphKey& key) const;

        /// @brief The number of pages allocated across every field type.
        [[nodiscard]] u32 PageCount() const;

        /// @brief A page's field type.
        /// @param page A page index in [0, PageCount()).
        [[nodiscard]] GlyphFieldType PageFieldType(u32 page) const;

        /// @brief The number of resident glyphs (including whitespace).
        [[nodiscard]] usize ResidentCount() const;

        /// @brief The current frame epoch.
        [[nodiscard]] u64 CurrentFrame() const { return m_Frame; }

    private:
        /// @brief A free rectangle within a page's guillotine free list.
        struct FreeRect
        {
            /// @brief Top-left texel.
            uvec2 Min{0};
            /// @brief Texel dimensions.
            uvec2 Size{0};
        };

        /// @brief One page: its field type and its list of free rectangles.
        struct Page
        {
            /// @brief The field type every glyph on this page shares.
            GlyphFieldType FieldType = GlyphFieldType::Msdf;
            /// @brief The disjoint free rectangles available for packing.
            vector<FreeRect> Free;
        };

        /// @brief The internal residency record for one glyph.
        struct Entry
        {
            /// @brief The page holding the glyph.
            u32 Page = 0;
            /// @brief The glyph's top-left texel within the page.
            uvec2 Offset{0};
            /// @brief The glyph's texel dimensions (gutter excluded).
            uvec2 GlyphSize{0};
            /// @brief The allocated rectangle including gutter, returned to the free list on eviction.
            uvec2 AllocSize{0};
            /// @brief The glyph's field type (kept for the whitespace case, which owns no page).
            GlyphFieldType FieldType = GlyphFieldType::Msdf;
            /// @brief Whether the glyph carries no pixels.
            bool Empty = false;
            /// @brief Size-independent placement data.
            Metrics M;
            /// @brief The frame epoch the glyph was last ensured in.
            u64 LastFrame = 0;
            /// @brief The frame epoch the glyph was packed in.
            u64 ResidentFrame = 0;
        };

        /// @brief Tries to place an allocation on a page, splitting the free rectangle it takes.
        /// @return The placement's top-left texel, or nullopt when nothing fits.
        [[nodiscard]] std::optional<uvec2> PlaceOnPage(Page& page, uvec2 alloc);

        /// @brief Returns a freed allocation rectangle to a page's free list.
        void FreeOnPage(Page& page, uvec2 offset, uvec2 alloc);

        /// @brief Allocates a fresh page of a field type, returning its index.
        [[nodiscard]] u32 AllocatePage(GlyphFieldType fieldType);

        /// @brief The number of pages currently allocated for a field type.
        [[nodiscard]] u32 PageCountFor(GlyphFieldType fieldType) const;

        /// @brief Builds the public slot for a resident entry.
        [[nodiscard]] Slot MakeSlot(const Entry& entry) const;

        Config m_Config;
        u64 m_Frame = 0;
        vector<Page> m_Pages;
        std::map<GlyphKey, Entry> m_Entries;
    };
}
