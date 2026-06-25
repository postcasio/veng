#pragma once

#include <span>

#include <Veng/Veng.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/Types.h>

namespace Veng
{
    class TaskSystem;

    template <typename T>
    class Task;
}

namespace Veng::Renderer
{
    class CommandBuffer;
    class SwapChain;
    class Context;
    struct BufferImageCopyRegion;

    /// @brief Creation parameters for a GPU image.
    struct ImageInfo
    {
        /// @brief Debug name.
        string Name = "Image";
        /// @brief Width, height, and depth in texels.
        uvec3 Extent = {1, 1, 1};
        /// @brief Number of mip levels.
        u32 MipLevels = 1;
        /// @brief Number of array layers.
        u32 Layers = 1;
        /// @brief Texel format.
        Format Format;
        /// @brief Image dimensionality.
        ImageType Type = ImageType::Type2D;
        /// @brief How the image will be bound and accessed.
        ImageUsage Usage;
    };

    /// @brief A GPU image with deferred destruction.
    ///
    /// Constructed only through Create(). enable_shared_from_this lets the async Upload
    /// capture an owning Ref<Image> into the worker job so the image cannot be destroyed
    /// before the job runs.
    class Image : public std::enable_shared_from_this<Image>
    {
    public:
        /// @brief Creates an image from the given parameters.
        /// @param context The owning context; the image must not outlive it.
        /// @param info    Creation parameters.
        /// @return A shared reference to the new image.
        static Ref<Image> Create(Context& context, const ImageInfo& info)
        {
            return Ref<Image>(new Image(context, info));
        }

        /// @brief Defers destruction of the underlying Vulkan image until the GPU is done with it.
        ///
        /// Unmanaged (swapchain) images are not owned by veng and are not destroyed.
        ~Image();

        Image(const Image&) = delete;
        Image& operator=(const Image&) = delete;

        /// @brief Returns the image's debug name.
        [[nodiscard]] const string& GetName() const { return m_Name; }

        /// @brief Returns the image's texel format.
        [[nodiscard]] Format GetFormat() const { return m_Format; }

        /// @brief Returns the image's usage flags.
        [[nodiscard]] ImageUsage GetUsage() const { return m_Usage; }

        /// @brief Returns the image extent (width, height, depth) in texels.
        [[nodiscard]] uvec3 GetExtent() const { return m_Extent; }

        /// @brief Returns the image width in texels.
        [[nodiscard]] u32 GetWidth() const { return m_Extent.x; }

        /// @brief Returns the image height in texels.
        [[nodiscard]] u32 GetHeight() const { return m_Extent.y; }

        /// @brief Returns the image depth in texels.
        [[nodiscard]] u32 GetDepth() const { return m_Extent.z; }

        /// @brief Returns the number of mip levels.
        [[nodiscard]] u32 GetMipLevels() const { return m_MipLevels; }

        /// @brief Returns the number of array layers.
        [[nodiscard]] u32 GetLayers() const { return m_Layers; }

        /// @brief Returns true if this image is managed (not a swapchain image).
        [[nodiscard]] bool IsManaged() const { return m_Managed; }

        /// @brief Returns the image dimensionality.
        [[nodiscard]] ImageType GetType() const { return m_Type; }

        // Per-subresource layout/stage/access tracking lives in Native (it holds vk::
        // types). The render graph and the engine's transfer paths read and update it
        // through the backend; callers do not reason about layout directly.

        /// @brief Generates a complete mip chain from the base level, recording into `commandBuffer`.
        void GenerateMipmaps(CommandBuffer& commandBuffer);

        /// @brief Copies data into the image, blocking the caller until the GPU has finished.
        ///
        /// Uses a staging buffer and WaitIdle. Used by the sync loaders, tests, and the
        /// smoke render; prefer Upload() off the render thread.
        /// @param span Source pixels, in the image's format and full extent.
        void UploadSync(std::span<const u8> span);

        /// @brief Uploads a precooked mip chain synchronously, one copy region per level.
        ///
        /// Records one VkBufferImageCopy per region from a single staging buffer and performs no
        /// GPU mip generation — every level's pixels come precomputed in `span`. Blocks until the
        /// copy completes. Used by the cooked-texture loader; runtime-built single-mip textures
        /// use the GPU-mipgen UploadSync(span) overload instead.
        /// @param span    All mip levels' pixels, tightly packed largest-first.
        /// @param regions One entry per mip level; BufferOffset indexes into `span`.
        /// @pre The image was created with MipLevels == regions.size() and ImageUsage::TransferDst.
        void UploadSync(std::span<const u8> span, std::span<const BufferImageCopyRegion> regions);

        /// @brief Copies data into the image on a worker thread, returning immediately.
        ///
        /// The copy is recorded onto the worker's transfer command buffer and submitted
        /// on the transfer queue under the submission lock; the returned Task completes
        /// once the work is submitted (not once the GPU finishes — that is gated by the
        /// transfer timeline). On first graphics use the render graph acquires the image
        /// and folds a transfer-timeline wait into the frame submit. The image is kept
        /// alive for the job's duration via shared_from_this(); registration into the
        /// bindless set is the caller's responsibility on the main thread.
        /// @param tasks The task system to dispatch the upload job on.
        /// @param data  Source pixels, in the image's format and full extent.
        /// @return A Task that completes once the transfer has been submitted.
        [[nodiscard]] Task<void> Upload(TaskSystem& tasks, std::span<const u8> data);

        /// @brief Uploads a precooked mip chain on a worker thread, one copy region per level.
        ///
        /// The transfer-queue sibling of UploadSync(span, regions): records one VkBufferImageCopy
        /// per region from a single staging buffer and performs no GPU mip generation. Lifetime
        /// and queue-handoff semantics match Upload(tasks, data).
        /// @param tasks   The task system to dispatch the upload job on.
        /// @param data    All mip levels' pixels, tightly packed largest-first.
        /// @param regions One entry per mip level; BufferOffset indexes into `data`.
        /// @return A Task that completes once the transfer has been submitted.
        [[nodiscard]] Task<void> Upload(TaskSystem& tasks, std::span<const u8> data,
                                        std::span<const BufferImageCopyRegion> regions);

        /// @brief Downloads the full image contents to the host, blocking until complete.
        /// @return A byte vector containing a snapshot of the image.
        [[nodiscard]] vector<u8> Download();

        /// @brief Returns the context this image was created with.
        ///
        /// Backend transition code reads it to resolve queue families and register the
        /// frame transfer-wait when acquiring a transfer-produced image on first graphics use.
        [[nodiscard]] Context& GetContext() const { return m_Context; }

        /// @brief Opaque backend handle; defined in Image.cpp.
        struct Native;
        /// @brief Returns the backend handle. Mutable ref from a const method by design — see Native.h.
        [[nodiscard]] Native& GetNative() const;

    private:
        Image(Context& context, const ImageInfo& info);

        /// @brief Presentable (swapchain) image constructor.
        ///
        /// The Native already wraps an externally-owned vk::Image; this constructor only
        /// sets up the engine-side bookkeeping.
        Image(Context& context, const ImageInfo& info, Unique<Native> native);

        /// @brief The owning context; used for deferred destruction. A resource must not outlive its context.
        Context& m_Context;
        string m_Name;
        uvec3 m_Extent;
        u32 m_MipLevels;
        u32 m_Layers;
        Format m_Format;
        ImageType m_Type;
        ImageUsage m_Usage;
        bool m_Managed;

        Unique<Native> m_Native;

        friend class SwapChain;
    };
}
