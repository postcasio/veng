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

    struct ImageInfo
    {
        string Name = "Image";
        uvec3 Extent = {1, 1, 1};
        u32 MipLevels = 1;
        u32 Layers = 1;
        Format Format;
        ImageType Type = ImageType::Type2D;
        ImageUsage Usage;
    };

    class Image : public std::enable_shared_from_this<Image>
    {
    public:
        static Ref<Image> Create(Context& context, const ImageInfo& info)
        {
            return Ref<Image>(new Image(context, info));
        }

        ~Image();

        Image(const Image&) = delete;
        Image& operator=(const Image&) = delete;

        [[nodiscard]] const string& GetName() const { return m_Name; }
        [[nodiscard]] Format GetFormat() const { return m_Format; }
        [[nodiscard]] ImageUsage GetUsage() const { return m_Usage; }
        [[nodiscard]] uvec3 GetExtent() const { return m_Extent; }
        [[nodiscard]] u32 GetWidth() const { return m_Extent.x; }
        [[nodiscard]] u32 GetHeight() const { return m_Extent.y; }
        [[nodiscard]] u32 GetDepth() const { return m_Extent.z; }
        [[nodiscard]] u32 GetMipLevels() const { return m_MipLevels; }
        [[nodiscard]] u32 GetLayers() const { return m_Layers; }
        [[nodiscard]] bool IsManaged() const { return m_Managed; }
        [[nodiscard]] ImageType GetType() const { return m_Type; }

        // Per-subresource layout/stage/access tracking lives in Native (it holds
        // vk:: types). The render graph and the engine's transfer paths read and
        // update it through the backend; consumers no longer reason about layout.

        void GenerateMipmaps(CommandBuffer& commandBuffer);

        // Stage and copy data into the image, blocking the caller until the copy
        // completes (the device is waited). The blocking path.
        void UploadSync(std::span<const u8> span);

        // Stage and copy data into the image on a worker thread, returning
        // immediately. The copy is recorded onto the worker's transfer command
        // buffer and submitted on the transfer queue under the submission lock;
        // the returned Task completes once the work is submitted (not once the
        // GPU finishes — that is gated by the transfer timeline). On first
        // graphics use the render graph acquires the image and folds a
        // transfer-timeline wait into the frame submit. The image is held alive
        // for the job's duration via shared_from_this(); registration into the
        // bindless set is the caller's responsibility on the main thread, not the
        // worker's.
        [[nodiscard]] Task<void> Upload(TaskSystem& tasks, std::span<const u8> data);

        [[nodiscard]] vector<u8> Download();

        // The context this image was created with. Backend transition code reads
        // it to resolve queue families and register the frame transfer-wait when
        // acquiring a transfer-produced image on first graphics use.
        [[nodiscard]] Context& GetContext() const { return m_Context; }

        struct Native;
        [[nodiscard]] Native& GetNative() const;

    private:
        Image(Context& context, const ImageInfo& info);

        // Presentable (swapchain) images: the Native already wraps an
        // externally-owned vk::Image, so this constructor only sets up the
        // engine-side bookkeeping.
        Image(Context& context, const ImageInfo& info, Unique<Native> native);

        // The context this resource was created with (deferred-destruction
        // back-ref; a resource must not outlive its context).
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
