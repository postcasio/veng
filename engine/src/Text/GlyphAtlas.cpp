#include <Veng/Text/GlyphAtlas.h>

#include "GlyphPacker.h"

#include <Veng/Assert.h>
#include <Veng/Log.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/Types.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace Veng::Text
{
    using namespace Veng::Renderer;

    namespace
    {
        GlyphPacker::Config PackerConfigFrom(const GlyphAtlasInfo& info)
        {
            return GlyphPacker::Config{
                .PageSize = info.PageSize,
                .Gutter = info.Gutter,
                .MaxPagesPerFieldType = info.MaxPagesPerFieldType,
            };
        }
    }

    GlyphAtlas::GlyphAtlas(Context& context, GlyphSource& source, const GlyphAtlasInfo& info)
        : m_Context(context), m_Source(source), m_Info(info),
          m_Packer(std::make_unique<GlyphPacker>(PackerConfigFrom(info)))
    {
        VE_ASSERT(m_Info.PageSize > 0, "GlyphAtlas: page size must be positive");
        VE_ASSERT(m_Info.MsdfPixelSize > 0, "GlyphAtlas: MSDF pixel size must be positive");

        // One shared clamp-to-edge linear sampler for every page's distance field; acquired from the
        // registry cache, so it is never released (a shared slot the registry owns for its lifetime).
        m_SamplerHandle = m_Context.GetBindlessRegistry()
                              .AcquireSampler({
                                  .Name = "GlyphAtlas Sampler",
                                  .MagFilter = Filter::Linear,
                                  .MinFilter = Filter::Linear,
                                  .MipmapMode = MipmapMode::Linear,
                                  .AddressModeU = AddressMode::ClampToEdge,
                                  .AddressModeV = AddressMode::ClampToEdge,
                                  .AddressModeW = AddressMode::ClampToEdge,
                              })
                              .Handle;
    }

    GlyphAtlas::~GlyphAtlas()
    {
        // Release each page's bindless slot while the context (and its registry) is still alive; the
        // image and view Refs then retire through the ordinary deferred-destruction path.
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        for (const Page& page : m_Pages)
        {
            bindless.Release(page.Handle);
        }
    }

    void GlyphAtlas::BeginFrame()
    {
        m_Frame++;
        m_Packer->BeginFrame(m_Frame);
    }

    GlyphKey GlyphAtlas::KeyFor(const FaceId face, const u32 glyphIndex, const f32 pixelSize,
                                const GlyphFieldType fieldType) const
    {
        u32 quantized = m_Info.MsdfPixelSize;
        if (fieldType == GlyphFieldType::Sdf)
        {
            const f32 requested = std::max(pixelSize, 1.0f);
            const u32 quantum = std::max(m_Info.SdfSizeQuantum, 1u);
            const auto rounded =
                static_cast<u32>(std::lround(requested / static_cast<f32>(quantum))) * quantum;
            quantized = std::max(rounded, std::max(m_Info.MinSdfPixelSize, quantum));
        }
        return GlyphKey{
            .Face = face,
            .GlyphIndex = glyphIndex,
            .QuantizedPixelSize = quantized,
            .FieldType = fieldType,
        };
    }

    GlyphSlot GlyphAtlas::Ensure(const GlyphKey& key)
    {
        // A hit returns immediately without touching the rasterizer.
        if (const std::optional<GlyphPacker::Slot> hit = m_Packer->Touch(key))
        {
            GlyphSlot slot;
            slot.Resident = true;
            slot.Empty = hit->Empty;
            slot.FieldType = hit->FieldType;
            slot.PageIndex = hit->Page;
            slot.PlaneMin = hit->PlaneMin;
            slot.PlaneMax = hit->PlaneMax;
            slot.Advance = hit->Advance;
            slot.ResidentFromFrame = hit->ResidentFrame;
            if (!hit->Empty)
            {
                slot.Page = m_Pages[hit->Page].Handle;
                const auto pageSize = static_cast<f32>(m_Info.PageSize);
                slot.UvMin = vec2{static_cast<f32>(hit->Offset.x) / pageSize,
                                  static_cast<f32>(hit->Offset.y) / pageSize};
                slot.UvMax = vec2{static_cast<f32>(hit->Offset.x + hit->Size.x) / pageSize,
                                  static_cast<f32>(hit->Offset.y + hit->Size.y) / pageSize};
            }
            return slot;
        }

        // A miss rasterizes through the source at the key's quantized size.
        Result<RasterizedGlyph> rasterized =
            m_Source.Rasterize(key.Face, key.GlyphIndex, static_cast<f32>(key.QuantizedPixelSize),
                               key.FieldType, m_Info.DistanceRangePx);
        if (!rasterized.has_value())
        {
            Log::Warn("GlyphAtlas: failed to rasterize glyph {}: {}", key.GlyphIndex,
                      rasterized.error());
            return GlyphSlot{.FieldType = key.FieldType};
        }

        const GlyphPacker::Metrics metrics{
            .Size = {rasterized->Width, rasterized->Height},
            .PlaneMin = rasterized->PlaneMin,
            .PlaneMax = rasterized->PlaneMax,
            .Advance = rasterized->Advance,
        };
        const GlyphPacker::Result packed = m_Packer->Insert(key, metrics);
        if (packed.Outcome == GlyphPacker::Outcome::NotResident)
        {
            return GlyphSlot{.FieldType = key.FieldType};
        }

        if (packed.NewPage)
        {
            CreatePage(packed.NewPageIndex, packed.NewPageFieldType);
            // Zero the whole fresh page so every gutter texel — and every not-yet-packed region —
            // reads as "fully outside"; the glyph copies below land on top of it.
            m_Clears.push_back(Clear{.Page = packed.NewPageIndex,
                                     .Offset = {0, 0},
                                     .Size = {m_Info.PageSize, m_Info.PageSize}});
        }

        // Re-zero any rectangle eviction just freed, so a glyph later packed there inherits a clean
        // gutter rather than the evicted glyph's stale texels.
        for (const GlyphPacker::FreedRect& freed : packed.Freed)
        {
            m_Clears.push_back(
                Clear{.Page = freed.Page, .Offset = freed.Offset, .Size = freed.Size});
        }

        // Stage the pixels for the next RecordUploads; a whitespace glyph has none.
        if (!rasterized->Pixels.empty())
        {
            m_Pending.push_back(Pending{
                .Page = packed.Slot.Page,
                .Offset = packed.Slot.Offset,
                .Size = packed.Slot.Size,
                .Channels = rasterized->Channels,
                .Pixels = std::move(rasterized->Pixels),
            });
        }

        GlyphSlot slot;
        slot.Resident = true;
        slot.Empty = packed.Slot.Empty;
        slot.FieldType = packed.Slot.FieldType;
        slot.PageIndex = packed.Slot.Page;
        slot.PlaneMin = packed.Slot.PlaneMin;
        slot.PlaneMax = packed.Slot.PlaneMax;
        slot.Advance = packed.Slot.Advance;
        slot.ResidentFromFrame = packed.Slot.ResidentFrame;
        if (!packed.Slot.Empty)
        {
            slot.Page = m_Pages[packed.Slot.Page].Handle;
            const auto pageSize = static_cast<f32>(m_Info.PageSize);
            slot.UvMin = vec2{static_cast<f32>(packed.Slot.Offset.x) / pageSize,
                              static_cast<f32>(packed.Slot.Offset.y) / pageSize};
            slot.UvMax =
                vec2{static_cast<f32>(packed.Slot.Offset.x + packed.Slot.Size.x) / pageSize,
                     static_cast<f32>(packed.Slot.Offset.y + packed.Slot.Size.y) / pageSize};
        }
        return slot;
    }

    void GlyphAtlas::CreatePage(const u32 pageIndex, const GlyphFieldType fieldType)
    {
        VE_ASSERT(pageIndex == m_Pages.size(),
                  "GlyphAtlas: page index out of step with the packer");

        // MSDF packs a three-channel field padded to RGBA8; SDF is a single R8 channel. TransferSrc
        // lets a diagnostic read a page back.
        const Format format =
            fieldType == GlyphFieldType::Msdf ? Format::RGBA8Unorm : Format::R8Unorm;

        Page page;
        page.FieldType = fieldType;
        page.Image =
            Image::Create(m_Context, {
                                         .Name = "GlyphAtlas Page",
                                         .Extent = {m_Info.PageSize, m_Info.PageSize, 1},
                                         .Format = format,
                                         .Usage = ImageUsage::Sampled | ImageUsage::TransferDst |
                                                  ImageUsage::TransferSrc,
                                     });
        page.View =
            ImageView::Create(m_Context, {.Name = "GlyphAtlas Page View", .Image = page.Image});
        page.Handle = m_Context.GetBindlessRegistry().Register(page.View);
        m_Pages.push_back(std::move(page));
    }

    void GlyphAtlas::RecordUploads(CommandBuffer& cmd)
    {
        if (m_Pending.empty() && m_Clears.empty())
        {
            return;
        }
        m_Context.BeginGpuScope(cmd, "Glyph Uploads");

        // Each page is touched in one pass: its clear rectangles (a fresh page whole, an evicted
        // glyph's rectangle) are zeroed first, then a write-after-write barrier orders the glyph
        // copies after them, so a glyph always lands on a cleared gutter. Every clear writes zero, so
        // the clear rectangles may overlap harmlessly; the glyph rectangles are disjoint by
        // construction. A burst of glyphs for one page still costs one staging buffer and one copy.
        for (u32 pageIndex = 0; pageIndex < m_Pages.size(); pageIndex++)
        {
            const Page& page = m_Pages[pageIndex];
            const u32 channels = page.FieldType == GlyphFieldType::Msdf ? 4u : 1u;

            usize clearBytes = 0;
            for (const Clear& clear : m_Clears)
            {
                if (clear.Page == pageIndex)
                {
                    clearBytes += static_cast<usize>(clear.Size.x) * clear.Size.y * channels;
                }
            }

            usize glyphBytes = 0;
            for (const Pending& pending : m_Pending)
            {
                if (pending.Page == pageIndex)
                {
                    glyphBytes += pending.Pixels.size();
                }
            }

            if (clearBytes == 0 && glyphBytes == 0)
            {
                continue;
            }

            cmd.PrepareForAccess(page.View, AccessKind::TransferDst);

            if (clearBytes > 0)
            {
                const vector<u8> zeros(clearBytes, 0);
                vector<BufferImageCopyRegion> regions;
                usize offset = 0;
                for (const Clear& clear : m_Clears)
                {
                    if (clear.Page != pageIndex)
                    {
                        continue;
                    }
                    regions.push_back(BufferImageCopyRegion{
                        .BufferOffset = offset,
                        .MipLevel = 0,
                        .ImageOffset = {clear.Offset.x, clear.Offset.y, 0},
                        .Extent = {clear.Size.x, clear.Size.y, 1},
                    });
                    offset += static_cast<usize>(clear.Size.x) * clear.Size.y * channels;
                }
                const Ref<Buffer> staging =
                    Buffer::Create(m_Context, {
                                                  .Name = "GlyphAtlas Clear",
                                                  .Size = zeros.size(),
                                                  .Usage = BufferUsage::TransferSrc,
                                              });
                staging->UploadSync(zeros);
                cmd.CopyBufferToImage(staging, page.Image, regions);
                // Both the clear and the glyph copy are transfer writes to this page, so re-preparing
                // for TransferDst inserts the write-after-write barrier that orders them.
                if (glyphBytes > 0)
                {
                    cmd.PrepareForAccess(page.View, AccessKind::TransferDst);
                }
            }

            if (glyphBytes > 0)
            {
                vector<u8> blob;
                blob.reserve(glyphBytes);
                vector<BufferImageCopyRegion> regions;
                for (const Pending& pending : m_Pending)
                {
                    if (pending.Page != pageIndex)
                    {
                        continue;
                    }
                    regions.push_back(BufferImageCopyRegion{
                        .BufferOffset = blob.size(),
                        .MipLevel = 0,
                        .ImageOffset = {pending.Offset.x, pending.Offset.y, 0},
                        .Extent = {pending.Size.x, pending.Size.y, 1},
                    });
                    blob.insert(blob.end(), pending.Pixels.begin(), pending.Pixels.end());
                }
                const Ref<Buffer> staging =
                    Buffer::Create(m_Context, {
                                                  .Name = "GlyphAtlas Upload",
                                                  .Size = blob.size(),
                                                  .Usage = BufferUsage::TransferSrc,
                                              });
                staging->UploadSync(blob);
                cmd.CopyBufferToImage(staging, page.Image, regions);
            }

            cmd.PrepareForAccess(page.View, AccessKind::SampleGraphics);
            // The staging Refs drop here; deferred destruction keeps them alive until the frame
            // fence, so the recorded copies still have their source when the GPU runs them.
        }

        m_Pending.clear();
        m_Clears.clear();
        m_Context.EndGpuScope(cmd);
    }

    u32 GlyphAtlas::GetPageCount() const
    {
        return static_cast<u32>(m_Pages.size());
    }

    TextureHandle GlyphAtlas::GetPageHandle(const u32 page) const
    {
        VE_ASSERT(page < m_Pages.size(), "GlyphAtlas: page index out of range");
        return m_Pages[page].Handle;
    }

    Ref<ImageView> GlyphAtlas::GetPageView(const u32 page) const
    {
        VE_ASSERT(page < m_Pages.size(), "GlyphAtlas: page index out of range");
        return m_Pages[page].View;
    }

    u64 GlyphAtlas::GetCurrentFrame() const
    {
        return m_Frame;
    }
}
