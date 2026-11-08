#include <Veng/Renderer/Image.h>

#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/FormatInfo.h>
#include <Veng/Renderer/TimelineSemaphore.h>
#include <Veng/Task/TaskSystem.h>

#include "Barrier.h"
#include "Natives.h"
#include "Recording.h"
#include "TypeMapping.h"

namespace Veng::Renderer
{
    namespace
    {
        // Every use the engine may make of a texture of these usages. A transfer destination that
        // can be a render target is one, since a colour clear on Metal is a render pass's load.
        MTLTextureUsage ToMetalUsage(const ImageUsage usage, const Format format)
        {
            MTLTextureUsage out = MTLTextureUsageShaderRead;
            if (HasFlag(usage, ImageUsage::Storage))
            {
                out |= MTLTextureUsageShaderWrite;
                // A storage image in a format Metal has texture atomics for may be the target of
                // one. Without this usage an atomic on it does nothing, and no layer reports it.
                if (format == Format::R32Uint)
                {
                    out |= MTLTextureUsageShaderAtomic;
                }
            }
            const bool attachment = HasFlag(usage, ImageUsage::ColorAttachment) ||
                                    HasFlag(usage, ImageUsage::DepthAttachment);
            const bool clearable = HasFlag(usage, ImageUsage::TransferDst) &&
                                   !Metal::IsCompressed(format) && !Metal::IsDepthStencil(format);
            if (attachment || clearable)
            {
                out |= MTLTextureUsageRenderTarget;
            }
            return out;
        }
    }

    Image::Native& Image::GetNative() const
    {
        return *m_Native;
    }

    Ref<Image> Image::CreateImported(Context& context, const ImageInfo& info, Unique<Native> native)
    {
        return Ref<Image>(new Image(context, info, std::move(native), true));
    }

    Image::Image(Context& context, const ImageInfo& info, Unique<Native> native, const bool managed)
        : m_Context(context), m_Name(info.Name), m_Extent(info.Extent), m_MipLevels(info.MipLevels),
          m_Layers(info.Layers), m_Format(info.Format), m_Type(info.Type), m_Usage(info.Usage),
          m_Managed(managed), m_Native(std::move(native))
    {
        m_Native->InitStates(m_Layers, m_MipLevels);
    }

    Image::Image(Context& context, const ImageInfo& info)
        : m_Context(context), m_Name(info.Name), m_Extent(info.Extent), m_MipLevels(info.MipLevels),
          m_Layers(info.Layers), m_Format(info.Format), m_Type(info.Type), m_Usage(info.Usage),
          m_Managed(true), m_Native(CreateUnique<Native>())
    {
        m_Native->InitStates(m_Layers, m_MipLevels);

        Context::Native& native = m_Context.GetNative();
        MTLTextureDescriptor* const descriptor = [[MTLTextureDescriptor alloc] init];
        descriptor.textureType = Metal::ToMetalTextureType(m_Type, m_Layers);
        descriptor.pixelFormat = Metal::RequireMetal(m_Format, fmt::format("Image '{}'", m_Name));
        descriptor.width = m_Extent.x;
        descriptor.height = m_Extent.y;
        descriptor.depth = m_Type == ImageType::Type3D ? m_Extent.z : 1;
        descriptor.mipmapLevelCount = m_MipLevels;
        descriptor.arrayLength = m_Type == ImageType::Type3D ? 1 : m_Layers;
        descriptor.usage = ToMetalUsage(m_Usage, m_Format);
        descriptor.storageMode = MTLStorageModePrivate;
        descriptor.hazardTrackingMode = MTLHazardTrackingModeUntracked;

        if (HasFlag(m_Usage, ImageUsage::Transient))
        {
            VE_ASSERT(!HasFlag(m_Usage, ImageUsage::Sampled | ImageUsage::Storage |
                                            ImageUsage::TransferSrc | ImageUsage::TransferDst),
                      "Image '{}': a transient attachment is only ever an attachment", m_Name);
            // A memoryless texture has no memory to place, so it cannot come from a heap.
            descriptor.usage = MTLTextureUsageRenderTarget;
            descriptor.storageMode = MTLStorageModeMemoryless;
            m_Native->Texture = [native.Device newTextureWithDescriptor:descriptor];
            VE_ASSERT(m_Native->Texture != nil,
                      "Image '{}': failed to create a {}x{} memoryless {} texture", m_Name,
                      m_Extent.x, m_Extent.y, FormatName(m_Format));
            [m_Native->Texture setLabel:Metal::ToNSString(m_Name)];
            return;
        }

        const MTLSizeAndAlign sizeAndAlign =
            [native.Device heapTextureSizeAndAlignWithDescriptor:descriptor];
        m_Native->Allocation = native.Allocate(false, sizeAndAlign.size, sizeAndAlign.align);
        m_Native->Texture =
            [m_Native->Allocation.Heap newTextureWithDescriptor:descriptor
                                                         offset:m_Native->Allocation.Offset];
        VE_ASSERT(m_Native->Texture != nil, "Image '{}': failed to place a {}x{}x{} {} texture",
                  m_Name, m_Extent.x, m_Extent.y, m_Extent.z, FormatName(m_Format));
        [m_Native->Texture setLabel:Metal::ToNSString(m_Name)];
        // Its heap is resident already; the texture joins the residency set itself too, because
        // Metal's shader validation counts a placed texture resident only then and reads one it does
        // not as zero.
        native.MakeResident(m_Native->Texture);
    }

    Image::~Image()
    {
        if (!m_Managed)
        {
            return;
        }
        Context::Native& native = m_Context.GetNative();
        if (m_Native->Allocation.Heap != nil)
        {
            // Left resident until the GPU is done with it, like its placement.
            native.Retire(function<void()>([&native, texture = m_Native->Texture]
                                           { native.Evict(texture); }));
            native.Retire(m_Native->Allocation);
        }
        native.Retire(m_Native->Texture);
        if (m_Native->Teardown)
        {
            native.Retire(std::move(m_Native->Teardown));
        }
        m_Native->Texture = nil;
    }

    void Image::GenerateMipmaps(CommandBuffer& commandBuffer)
    {
        if (m_MipLevels <= 1)
        {
            Backend::TransitionImage(commandBuffer, *this, ImageLayout::ShaderReadOnly, 0, m_Layers,
                                     0, 1);
            return;
        }
        // The levels below the first are written by the device's own mip filter, after every
        // earlier write of the image.
        Backend::TransitionImage(commandBuffer, *this, ImageLayout::TransferDst, 0, m_Layers, 0,
                                 m_MipLevels);
        [Metal::Recording::ComputeEncoder(commandBuffer)
            generateMipmapsForTexture:m_Native->Texture];
        Backend::TransitionImage(commandBuffer, *this, ImageLayout::ShaderReadOnly, 0, m_Layers, 0,
                                 m_MipLevels);
    }

    void Image::UploadSync(std::span<const u8> span)
    {
        const Ref<Buffer> staging = Buffer::Create(m_Context, {
                                                                  .Name = m_Name + " (Upload)",
                                                                  .Size = span.size(),
                                                                  .Usage = BufferUsage::TransferSrc,
                                                              });
        staging->UploadSync(span);

        // Through ImmediateCommands, so setup work held for a frame records ahead of the copy.
        m_Context.ImmediateCommands(
            [&](CommandBuffer& commandBuffer)
            {
                Backend::TransitionImage(commandBuffer, *this, ImageLayout::TransferDst, 0,
                                         m_Layers, 0, m_MipLevels);
                commandBuffer.CopyBufferToImage(staging, shared_from_this());
                if (m_MipLevels > 1)
                {
                    GenerateMipmaps(commandBuffer);
                }
                else
                {
                    Backend::TransitionImage(commandBuffer, *this, ImageLayout::ShaderReadOnly, 0,
                                             m_Layers, 0, 1);
                }
            });
    }

    void Image::UploadSync(std::span<const u8> span,
                           const std::span<const BufferImageCopyRegion> regions)
    {
        const Ref<Buffer> staging = Buffer::Create(m_Context, {
                                                                  .Name = m_Name + " (Upload)",
                                                                  .Size = span.size(),
                                                                  .Usage = BufferUsage::TransferSrc,
                                                              });
        staging->UploadSync(span);

        m_Context.ImmediateCommands(
            [&](CommandBuffer& commandBuffer)
            {
                Backend::TransitionImage(commandBuffer, *this, ImageLayout::TransferDst, 0,
                                         m_Layers, 0, m_MipLevels);
                commandBuffer.CopyBufferToImage(staging, shared_from_this(), regions);
                Backend::TransitionImage(commandBuffer, *this, ImageLayout::ShaderReadOnly, 0,
                                         m_Layers, 0, m_MipLevels);
            });
    }

    void Image::UploadOnWorker(const std::span<const u8> data,
                               const std::span<const BufferImageCopyRegion> regions)
    {
        const u32 workerIndex = TaskSystem::GetCurrentWorkerIndex();
        const u32 transferFamily =
            m_Context.GetQueueFamilies().TransferFamily.value_or(QueueFamilyIgnored);

        const Ref<Buffer> staging = Buffer::Create(m_Context, {
                                                                  .Name = m_Name + " (Upload)",
                                                                  .Size = data.size(),
                                                                  .Usage = BufferUsage::TransferSrc,
                                                              });
        staging->UploadSync(data);

        // Each worker records onto its own command buffer and allocator.
        CommandBuffer& cmd = m_Context.BeginTransferRecording(workerIndex);
        const Ref<Image> self = shared_from_this();
        Backend::TransitionImage(cmd, *this, ImageLayout::TransferDst, 0, m_Layers, 0, m_MipLevels);
        if (regions.empty())
        {
            cmd.CopyBufferToImage(staging, self);
        }
        else
        {
            cmd.CopyBufferToImage(staging, self, regions);
        }
        const u64 value = m_Context.SubmitTransfer(workerIndex, m_Context.GetTransferTimeline());
        Backend::MarkProducedOn(*this, transferFamily, value);

        // The staging buffer lives until the transfer timeline reaches the copy's value, not the
        // frame fence its own destructor would retire it behind.
        m_Context.GetNative().RetireOnTransfer(staging, value);
    }

    void Image::UploadOnWorker(const std::span<const u8> data)
    {
        UploadOnWorker(data, {});
    }

    Task<void> Image::Upload(TaskSystem& tasks, const std::span<const u8> data)
    {
        return tasks.Submit(
            [self = shared_from_this(), bytes = vector<u8>(data.begin(), data.end())]
            { self->UploadOnWorker(bytes); });
    }

    Task<void> Image::Upload(TaskSystem& tasks, const std::span<const u8> data,
                             const std::span<const BufferImageCopyRegion> regions)
    {
        return tasks.Submit(
            [self = shared_from_this(), bytes = vector<u8>(data.begin(), data.end()),
             copyRegions = vector<BufferImageCopyRegion>(regions.begin(), regions.end())]
            { self->UploadOnWorker(bytes, copyRegions); });
    }

    vector<u8> Image::Download()
    {
        const auto buffer = Buffer::Create(
            m_Context,
            {
                .Name = m_Name + " (Download)",
                // A Type3D image's mip-0 copy spans every depth slice.
                .Size =
                    static_cast<u64>(BytesForLevel(m_Format, m_Extent.x, m_Extent.y)) * m_Extent.z,
                .Usage = BufferUsage::TransferDst,
            });

        m_Context.ImmediateCommands(
            [&](CommandBuffer& commandBuffer)
            {
                const ImageLayout originalLayout = m_Native->At(0, 0).Layout;
                Backend::TransitionImage(commandBuffer, *this, ImageLayout::TransferSrc);
                commandBuffer.CopyImageToBuffer(shared_from_this(), buffer);
                if (originalLayout != ImageLayout::Undefined)
                {
                    Backend::TransitionImage(commandBuffer, *this, originalLayout);
                }
            });

        return buffer->Download();
    }
}
