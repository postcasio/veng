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

    /// @brief Constructs an Image wrapping an externally-owned Vulkan image (e.g. a swapchain image).
    ///
    /// The image is marked unmanaged: the destructor does not destroy the underlying VkImage.
    /// @param context  The owning render context.
    /// @param info     Image metadata (extent, format, usage, etc.).
    /// @param native   Backend native struct containing the pre-existing VkImage handle.
    Image::Image(Context& context, const ImageInfo& info, Unique<Native> native)
        : m_Context(context), m_Name(info.Name), m_Extent(info.Extent), m_MipLevels(info.MipLevels),
          m_Layers(info.Layers), m_Format(info.Format), m_Type(info.Type), m_Usage(info.Usage),
          m_Managed(false), m_Native(std::move(native))
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

        auto commandBuffer = CommandBuffer::Create(m_Context);

        commandBuffer->Begin(CommandBufferUsage::OneTimeSubmit);
        Backend::TransitionImage(*commandBuffer, *this, ImageLayout::TransferDst, 0, m_Layers, 0,
                                 m_MipLevels);
        commandBuffer->CopyBufferToImage(stagingBuffer, shared_from_this());

        if (m_MipLevels > 1)
        {
            GenerateMipmaps(*commandBuffer);
        }
        else
        {
            Backend::TransitionImage(*commandBuffer, *this, ImageLayout::ShaderReadOnly, 0,
                                     m_Layers, 0, 1);
        }

        commandBuffer->End();

        m_Context.SubmitImmediateCommands(*commandBuffer);
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

        auto commandBuffer = CommandBuffer::Create(m_Context);

        commandBuffer->Begin(CommandBufferUsage::OneTimeSubmit);
        Backend::TransitionImage(*commandBuffer, *this, ImageLayout::TransferDst, 0, m_Layers, 0,
                                 m_MipLevels);
        commandBuffer->CopyBufferToImage(stagingBuffer, shared_from_this(), regions);
        Backend::TransitionImage(*commandBuffer, *this, ImageLayout::ShaderReadOnly, 0, m_Layers, 0,
                                 m_MipLevels);

        commandBuffer->End();

        m_Context.SubmitImmediateCommands(*commandBuffer);
    }

    /// @brief Uploads pixel data asynchronously via the transfer queue.
    ///
    /// Records the copy on a worker's transfer command buffer and signals the transfer timeline.
    /// The image is released to the graphics queue after the transfer; the first graphics use
    /// acquires it and folds the timeline wait into the frame submit.
    /// @param tasks  The task system; determines the worker context and transfer pool.
    /// @param data   Source pixel data; a private copy is made so the caller's span need not outlive this call.
    /// @return A task that completes when the transfer has been submitted (not necessarily finished on the GPU).
    Task<void> Image::Upload(TaskSystem& tasks, const std::span<const u8> data)
    {
        // Capture owning refs: the image must outlive the worker, and the caller's span may not.
        Ref<Image> self = shared_from_this();
        vector<u8> bytes(data.begin(), data.end());

        return tasks.Submit(
            [self = std::move(self), bytes = std::move(bytes)]
            {
                Context& context = self->m_Context;
                const u32 workerIndex = TaskSystem::GetCurrentWorkerIndex();

                const QueueFamilyIndices& families = context.GetQueueFamilies();
                const u32 transferFamily =
                    families.TransferFamily.value_or(VK_QUEUE_FAMILY_IGNORED);
                const u32 graphicsFamily =
                    families.GraphicsFamily.value_or(VK_QUEUE_FAMILY_IGNORED);

                // Command-pool allocation is not thread-safe, so the copy records onto
                // this worker's own transfer command buffer.
                auto staging = Buffer::Create(context, {
                                                           .Name = self->m_Name + " (Upload)",
                                                           .Size = bytes.size(),
                                                           .Usage = BufferUsage::TransferSrc,
                                                       });
                staging->UploadSync(bytes);

                CommandBuffer& cmd = context.BeginTransferRecording(workerIndex);

                Backend::TransitionImage(cmd, *self, ImageLayout::TransferDst, 0, self->m_Layers, 0,
                                         self->m_MipLevels);
                cmd.CopyBufferToImage(staging, self);

                Backend::ReleaseImageToGraphicsQueue(cmd, *self, transferFamily, graphicsFamily);

                const u64 value =
                    context.SubmitTransfer(workerIndex, context.GetTransferTimeline());

                Backend::MarkProducedOn(*self, transferFamily, value);

                // The staging buffer must live until the transfer timeline value it signalled;
                // letting Buffer::~Buffer run would queue it on the per-frame graphics fence, not the transfer fence.
                const ReleasedBuffer released = ReleaseBuffer(*staging);
                context.GetNative().RetireOnTransfer(released.Buffer, released.Allocation, value);
            });
    }

    /// @brief Uploads a precooked mip chain asynchronously via the transfer queue.
    ///
    /// The transfer-queue sibling of UploadSync(span, regions): records one copy region per level
    /// onto a worker's transfer command buffer, performs no GPU mip generation, and releases the
    /// image to the graphics queue. Lifetime and queue-handoff semantics match Upload(tasks, data).
    /// @param tasks    The task system; determines the worker context and transfer pool.
    /// @param data     All mip levels' pixels; a private copy is made.
    /// @param regions  One copy region per mip level; BufferOffset indexes into `data`.
    /// @return A task that completes when the transfer has been submitted.
    Task<void> Image::Upload(TaskSystem& tasks, const std::span<const u8> data,
                             const std::span<const BufferImageCopyRegion> regions)
    {
        // Capture owning refs and private copies: the image must outlive the worker, and the
        // caller's span/regions may not.
        Ref<Image> self = shared_from_this();
        vector<u8> bytes(data.begin(), data.end());
        vector<BufferImageCopyRegion> copyRegions(regions.begin(), regions.end());

        return tasks.Submit(
            [self = std::move(self), bytes = std::move(bytes), copyRegions = std::move(copyRegions)]
            {
                Context& context = self->m_Context;
                const u32 workerIndex = TaskSystem::GetCurrentWorkerIndex();

                const QueueFamilyIndices& families = context.GetQueueFamilies();
                const u32 transferFamily =
                    families.TransferFamily.value_or(VK_QUEUE_FAMILY_IGNORED);
                const u32 graphicsFamily =
                    families.GraphicsFamily.value_or(VK_QUEUE_FAMILY_IGNORED);

                auto staging = Buffer::Create(context, {
                                                           .Name = self->m_Name + " (Upload)",
                                                           .Size = bytes.size(),
                                                           .Usage = BufferUsage::TransferSrc,
                                                       });
                staging->UploadSync(bytes);

                CommandBuffer& cmd = context.BeginTransferRecording(workerIndex);

                Backend::TransitionImage(cmd, *self, ImageLayout::TransferDst, 0, self->m_Layers, 0,
                                         self->m_MipLevels);
                cmd.CopyBufferToImage(staging, self, copyRegions);

                Backend::ReleaseImageToGraphicsQueue(cmd, *self, transferFamily, graphicsFamily);

                const u64 value =
                    context.SubmitTransfer(workerIndex, context.GetTransferTimeline());

                Backend::MarkProducedOn(*self, transferFamily, value);

                const ReleasedBuffer released = ReleaseBuffer(*staging);
                context.GetNative().RetireOnTransfer(released.Buffer, released.Allocation, value);
            });
    }

    /// @brief Downloads image pixels to CPU memory synchronously, restoring the original layout.
    ///
    /// Allocates a readback buffer, copies image → buffer, submits and waits, then returns the bytes.
    /// @return Raw pixel bytes in the image's format, row-major.
    vector<u8> Image::Download()
    {
        auto buffer = Buffer::Create(
            m_Context, {
                           .Name = m_Name + " (Download)",
                           .Size = m_Extent.x * m_Extent.y * vk::blockSize(ToVk(m_Format)),
                           .Usage = BufferUsage::TransferDst,
                       });

        const ImageLayout originalLayout = FromVk(m_Native->At(0, 0).Layout);

        auto commandBuffer = CommandBuffer::Create(m_Context);

        commandBuffer->Begin(CommandBufferUsage::OneTimeSubmit);

        Backend::TransitionImage(*commandBuffer, *this, ImageLayout::TransferSrc);

        commandBuffer->CopyImageToBuffer(shared_from_this(), buffer);

        // Restore the image to the layout it had on entry so callers see no
        // change; skip if it was never transitioned (can't transition to
        // Undefined).
        if (originalLayout != ImageLayout::Undefined)
        {
            Backend::TransitionImage(*commandBuffer, *this, originalLayout);
        }

        commandBuffer->End();

        m_Context.SubmitImmediateCommands(*commandBuffer);

        return buffer->Download();
    }
}
