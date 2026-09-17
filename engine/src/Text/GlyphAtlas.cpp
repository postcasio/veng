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
        if (m_Pending.empty())
        {
            return;
        }

        // Batch every staged glyph destined for one page into a single staging buffer and copy, so a
        // burst of new glyphs costs one transition pair and one command per page.
        for (u32 pageIndex = 0; pageIndex < m_Pages.size(); pageIndex++)
        {
            usize totalBytes = 0;
            for (const Pending& pending : m_Pending)
            {
                if (pending.Page == pageIndex)
                {
                    totalBytes += pending.Pixels.size();
                }
            }
            if (totalBytes == 0)
            {
                continue;
            }

            vector<u8> blob;
            blob.reserve(totalBytes);
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

            const Page& page = m_Pages[pageIndex];
            cmd.PrepareForAccess(page.View, AccessKind::TransferDst);
            cmd.CopyBufferToImage(staging, page.Image, regions);
            cmd.PrepareForAccess(page.View, AccessKind::SampleGraphics);
            // The staging Ref drops here; deferred destruction keeps it alive until the frame fence,
            // so the recorded copy still has its source when the GPU runs it.
        }

        m_Pending.clear();
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
