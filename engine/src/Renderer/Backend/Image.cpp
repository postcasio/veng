#include <Veng/Renderer/Image.h>

#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/TimelineSemaphore.h>
#include <Veng/Renderer/Backend/Barrier.h>
#include <Veng/Renderer/Backend/DebugMarkers.h>
#include <Veng/Renderer/Backend/Natives.h>
#include <Veng/Renderer/Backend/TypeMapping.h>
#include <Veng/Task/TaskSystem.h>

#include <vulkan/vulkan_format_traits.hpp>

namespace Veng::Renderer
{
    /// @brief Returns the backend-native image handle.
    Image::Native& Image::GetNative() const
    {
        return *m_Native;
    }

    Ref<Image> Image::CreateImported(Context& context, const ImageInfo& info, Unique<Native> native)
    {
        return Ref<Image>(new Image(context, info, std::move(native), true));
    }

    /// @brief Constructs an Image wrapping an already-created Vulkan image.
    ///
    /// @param context  The owning render context.
    /// @param info     Image metadata (extent, format, usage, etc.).
    /// @param native   Backend native struct containing the pre-existing VkImage handle.
    /// @param managed  Whether the destructor retires the handle, its allocation and its teardown.
    Image::Image(Context& context, const ImageInfo& info, Unique<Native> native, const bool managed)
        : m_Context(context), m_Name(info.Name), m_Extent(info.Extent), m_MipLevels(info.MipLevels),
          m_Layers(info.Layers), m_Format(info.Format), m_Type(info.Type), m_Usage(info.Usage),
          m_Managed(managed), m_Native(std::move(native))
    {
        m_Native->InitStates(m_Layers, m_MipLevels);

        DebugMarkers::MarkImage(GetVkDevice(m_Context), m_Native->Image, m_Name);
    }

    /// @brief Constructs a managed Image, allocating a new Vulkan image via VMA.
    ///
    /// The destructor defers destruction of the VkImage and its allocation until the GPU is done with it.
    /// @param context  The owning render context.
    /// @param info     Image configuration.
    Image::Image(Context& context, const ImageInfo& info)
        : m_Context(context), m_Name(info.Name), m_Extent(info.Extent), m_MipLevels(info.MipLevels),
          m_Layers(info.Layers), m_Format(info.Format), m_Type(info.Type), m_Usage(info.Usage),
          m_Managed(true), m_Native(CreateUnique<Native>())
    {
        m_Native->InitStates(m_Layers, m_MipLevels);

        vk::ImageCreateFlags flags;

        if (m_Layers == 6)
        {
            flags |= vk::ImageCreateFlagBits::eCubeCompatible;
        }

        const VkImageCreateInfo imageCreateInfo = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .flags = static_cast<VkImageCreateFlags>(flags),
            .imageType = static_cast<VkImageType>(ToVk(m_Type)),
            .format = static_cast<VkFormat>(ToVk(m_Format)),
            .extent = {m_Extent.x, m_Extent.y, m_Extent.z},
            .mipLevels = m_MipLevels,
            .arrayLayers = m_Layers,
            .samples = static_cast<VkSampleCountFlagBits>(vk::SampleCountFlagBits::e1),
            .tiling = static_cast<VkImageTiling>(vk::ImageTiling::eOptimal),
            .usage = static_cast<VkImageUsageFlags>(ToVk(m_Usage)),
            .sharingMode = static_cast<VkSharingMode>(vk::SharingMode::eExclusive),
            .initialLayout = static_cast<VkImageLayout>(vk::ImageLayout::eUndefined)};

        const VmaAllocationCreateInfo allocationCreateInfo{
            .usage = VMA_MEMORY_USAGE_AUTO,
            .requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            .pool = VK_NULL_HANDLE,
        };

        VkImage image;

        VK_RAW_ASSERT(vmaCreateImage(GetVmaAllocator(m_Context), &imageCreateInfo,
                                     &allocationCreateInfo, &image, &m_Native->Allocation,
                                     &m_Native->AllocationInfo),
                      fmt::format("Failed to create image {}", m_Name));

        m_Native->Image = image;

        vmaSetAllocationName(GetVmaAllocator(m_Context), m_Native->Allocation, m_Name.c_str());

        DebugMarkers::MarkImage(GetVkDevice(m_Context), m_Native->Image, m_Name);
    }

    /// @brief Defers destruction of the backing Vulkan image (managed images only).
    Image::~Image()
    {
        if (m_Managed)
        {
            m_Context.GetNative().Retire(m_Native->Image, m_Native->Allocation);
            if (m_Native->Teardown)
            {
                m_Context.GetNative().Retire(std::move(m_Native->Teardown));
            }
        }
    }

    /// @brief Records blit commands to downsample each mip level from the previous one.
    ///
    /// Transitions each source mip to TransferSrc, blits to the next, then transitions to ShaderReadOnly.
    /// The caller must have already transitioned mip 0 to TransferDst.
    /// @param commandBuffer  Command buffer to record into.
    void Image::GenerateMipmaps(CommandBuffer& commandBuffer)
    {
        u32 mipWidth = m_Extent.x;
        u32 mipHeight = m_Extent.y;

        for (u32 i = 1; i < m_MipLevels; i++)
        {
            Backend::TransitionImage(commandBuffer, *this, ImageLayout::TransferSrc, 0, 1, i - 1,
                                     1);

            commandBuffer.BlitImage({.SourceImage = shared_from_this(),
                                     .DestinationImage = shared_from_this(),
                                     .SourceMipLevel = i - 1,
                                     .DestinationMipLevel = i,
                                     .SourceOffset = {0, 0, 0},
                                     .DestinationOffset = {0, 0, 0},
                                     .SourceExtent = {mipWidth, mipHeight, 1},
                                     .DestinationExtent = {mipWidth > 1 ? mipWidth / 2 : 1,
                                                           mipHeight > 1 ? mipHeight / 2 : 1, 1}});

            Backend::TransitionImage(commandBuffer, *this, ImageLayout::ShaderReadOnly, 0, 1, i - 1,
                                     1);

            if (mipWidth > 1)
            {
                mipWidth /= 2;
            }
            if (mipHeight > 1)
            {
                mipHeight /= 2;
            }
        }

        Backend::TransitionImage(commandBuffer, *this, ImageLayout::ShaderReadOnly, 0, 1,
                                 m_MipLevels - 1, 1);
    }

    /// @brief Uploads pixel data synchronously via a staging buffer, blocking until complete.
    ///
    /// Allocates a staging buffer, copies data, records a one-time command buffer, submits, and waits.
    /// Generates mipmaps if the image has more than one mip level.
    /// @param span  Source pixel data in the image's format.
    void Image::UploadSync(std::span<const u8> span)
    {
        auto stagingBuffer = Buffer::Create(m_Context, {
                                                           .Name = m_Name + " (Upload)",
                                                           .Size = span.size(),
                                                           .Usage = BufferUsage::TransferSrc,
                                                       });

        stagingBuffer->UploadSync(span);

        // Through ImmediateCommands, so setup work held for a frame records ahead of the copy.
        m_Context.ImmediateCommands(
            [&](CommandBuffer& commandBuffer)
            {
                Backend::TransitionImage(commandBuffer, *this, ImageLayout::TransferDst, 0,
                                         m_Layers, 0, m_MipLevels);
                commandBuffer.CopyBufferToImage(stagingBuffer, shared_from_this());

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

    /// @brief Uploads a precooked mip chain synchronously, one copy region per level.
    ///
    /// Records one buffer-to-image copy per level from a single staging buffer and transitions
    /// every level to ShaderReadOnly — no GPU mip generation, since the levels are precomputed.
    /// @param span     All mip levels' pixels, tightly packed largest-first.
    /// @param regions  One copy region per mip level; BufferOffset indexes into `span`.
    void Image::UploadSync(std::span<const u8> span,
                           const std::span<const BufferImageCopyRegion> regions)
    {
        auto stagingBuffer = Buffer::Create(m_Context, {
                                                           .Name = m_Name + " (Upload)",
                                                           .Size = span.size(),
                                                           .Usage = BufferUsage::TransferSrc,
                                                       });

        stagingBuffer->UploadSync(span);

        // Through ImmediateCommands, so setup work held for a frame records ahead of the copy.
        m_Context.ImmediateCommands(
            [&](CommandBuffer& commandBuffer)
            {
                Backend::TransitionImage(commandBuffer, *this, ImageLayout::TransferDst, 0,
                                         m_Layers, 0, m_MipLevels);
                commandBuffer.CopyBufferToImage(stagingBuffer, shared_from_this(), regions);
                Backend::TransitionImage(commandBuffer, *this, ImageLayout::ShaderReadOnly, 0,
                                         m_Layers, 0, m_MipLevels);
            });
    }

    void Image::UploadOnWorker(const std::span<const u8> data,
                               const std::span<const BufferImageCopyRegion> regions)
    {
        const u32 workerIndex = TaskSystem::GetCurrentWorkerIndex();

        const QueueFamilyIndices& families = m_Context.GetQueueFamilies();
        const u32 transferFamily = families.TransferFamily.value_or(VK_QUEUE_FAMILY_IGNORED);
        const u32 graphicsFamily = families.GraphicsFamily.value_or(VK_QUEUE_FAMILY_IGNORED);

        auto staging = Buffer::Create(m_Context, {
                                                     .Name = m_Name + " (Upload)",
                                                     .Size = data.size(),
                                                     .Usage = BufferUsage::TransferSrc,
                                                 });
        staging->UploadSync(data);

        // Command-pool allocation is not thread-safe, so the copy records onto this worker's own
        // transfer command buffer.
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

        Backend::ReleaseImageToGraphicsQueue(cmd, *this, transferFamily, graphicsFamily);

        const u64 value = m_Context.SubmitTransfer(workerIndex, m_Context.GetTransferTimeline());

        Backend::MarkProducedOn(*this, transferFamily, value);

        // The staging buffer must live until the transfer timeline value it signalled; letting
        // Buffer::~Buffer run would queue it on the per-frame graphics fence, not the transfer fence.
        const ReleasedBuffer released = ReleaseBuffer(*staging);
        m_Context.GetNative().RetireOnTransfer(released.Buffer, released.Allocation, value);
    }

    void Image::UploadOnWorker(const std::span<const u8> data)
    {
        UploadOnWorker(data, {});
    }

    Task<void> Image::Upload(TaskSystem& tasks, const std::span<const u8> data)
    {
        // Capture owning refs: the image must outlive the worker, and the caller's span may not.
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

    /// @brief Downloads image pixels to CPU memory synchronously, restoring the original layout.
    ///
    /// Allocates a readback buffer, copies image → buffer, submits and waits, then returns the bytes.
    /// @return Raw pixel bytes in the image's format, row-major.
    vector<u8> Image::Download()
    {
        auto buffer = Buffer::Create(
            m_Context,
            {
                .Name = m_Name + " (Download)",
                // Fold the depth axis in: a Type3D image's mip-0 copy spans all z slices.
                .Size = static_cast<u64>(m_Extent.x) * m_Extent.y * m_Extent.z *
                        vk::blockSize(ToVk(m_Format)),
                .Usage = BufferUsage::TransferDst,
            });

        // Through ImmediateCommands, so setup work held for a frame — a clear or upload of this
        // very image — records ahead of the copy, and the layout restored is the one it left.
        m_Context.ImmediateCommands(
            [&](CommandBuffer& commandBuffer)
            {
                const ImageLayout originalLayout = FromVk(m_Native->At(0, 0).Layout);

                Backend::TransitionImage(commandBuffer, *this, ImageLayout::TransferSrc);

                commandBuffer.CopyImageToBuffer(shared_from_this(), buffer);

                // Restore the image to the layout it had on entry so callers see no
                // change; skip if it was never transitioned (can't transition to
                // Undefined).
                if (originalLayout != ImageLayout::Undefined)
                {
                    Backend::TransitionImage(commandBuffer, *this, originalLayout);
                }
            });

        return buffer->Download();
    }
}
