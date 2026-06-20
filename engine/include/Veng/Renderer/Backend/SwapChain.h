#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Backend/Vulkan.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/Semaphore.h>
#include <Veng/Window.h>
#include <Veng/Renderer/Backend/SwapChainSupport.h>

namespace Veng::Renderer
{
    class Context;

    /// @brief Construction parameters for SwapChain.
    struct SwapChainInfo
    {
        /// @brief Requested number of swapchain images (clamped to surface capabilities).
        u32 MaxImageCount;
        /// @brief Initial framebuffer width in pixels.
        u32 Width;
        /// @brief Initial framebuffer height in pixels.
        u32 Height;
        /// @brief Requested image format (determines color space automatically).
        vk::Format Format;
    };

    /// @brief Owns the Vulkan swapchain and its per-image resources.
    class SwapChain
    {
    public:
        /// @brief Pipeline stage at which the image-available semaphore
        /// (signalled by vkAcquireNextImageKHR) is waited on submit.
        ///
        /// Two sites must agree on this and are coupled through this constant:
        ///   - Context::SubmitFrame's pWaitDstStageMask for that semaphore, and
        ///   - the srcStage each swapchain image's tracked state is seeded with,
        ///     so its first layout transition forms an execution dependency
        ///     against the acquire instead of racing ahead of the wait (a
        ///     WRITE_AFTER_READ hazard on first use).
        static constexpr vk::PipelineStageFlags AcquireWaitStage =
            vk::PipelineStageFlagBits::eColorAttachmentOutput;

        /// @brief Creates and initializes a SwapChain.
        static Unique<SwapChain> Create(Context& context, const SwapChainInfo& info)
        {
            return CreateUnique<SwapChain>(context, info);
        }

        /// @brief Creates the Vulkan swapchain, its images, and their image views.
        ///
        /// Seeds each image's tracked pipeline stage to AcquireWaitStage so the
        /// first layout transition forms a correct execution dependency against
        /// the image-available semaphore wait (prevents a WRITE_AFTER_READ hazard).
        void Initialize();
        SwapChain(Context& context, const SwapChainInfo& info);
        ~SwapChain();

        /// @brief Destroys and recreates the swapchain after a resize.
        void RenderExtentChanged();

        [[nodiscard]] u32 GetWidth() const { return m_Width; }
        [[nodiscard]] u32 GetHeight() const { return m_Height; }
        [[nodiscard]] uvec2 GetExtent() const { return {m_Width, m_Height}; }
        /// @brief Returns the surface extent, clamping to the window framebuffer size if needed.
        [[nodiscard]] vk::Extent2D GetSurfaceExtent(Window& window,
                                                    SwapChainSupportDetails& swapChainSupport);
        [[nodiscard]] u32 GetMaxImageCount() const { return m_MaxImageCount; }
        [[nodiscard]] u32 GetImageCount() const { return m_ImageCount; }
        [[nodiscard]] u32 GetCurrentImageIndex() const { return m_CurrentImageIndex; }
        /// @brief Engine format of the presentable images.
        ///
        /// The raw vk::Format is internal; GetVkFormat() exposes it for backend code.
        [[nodiscard]] Format GetFormat() const;
        /// @brief Raw Vulkan format of the presentable images.
        [[nodiscard]] vk::Format GetVkFormat() const { return m_Format; }

        /// @brief Acquires the next presentable image, signalling @p semaphore on completion.
        ///
        /// Returns eSuboptimalKHR or eErrorOutOfDateKHR when the surface has changed;
        /// the caller is responsible for recreating the swapchain in those cases.
        [[nodiscard]] vk::Result AcquireNextImage(Semaphore& semaphore);

        [[nodiscard]] vk::SwapchainKHR GetVkSwapChain() const { return m_VkSwapChain; }
        [[nodiscard]] Ref<Image> GetImage(const u32 index) const { return m_Images[index]; }
        [[nodiscard]] Ref<Image> GetCurrentImage() const { return m_Images[m_CurrentImageIndex]; }

        [[nodiscard]] Ref<ImageView> GetImageView(const u32 index) const
        {
            return m_ImageViews[index];
        }
        [[nodiscard]] Ref<ImageView> GetCurrentImageView() const
        {
            return m_ImageViews[m_CurrentImageIndex];
        }

        /// @brief Registers a callback fired after the swapchain has been recreated.
        void AddInvalidationCallback(std::function<void()> func)
        {
            m_OnInvalidated.push_back(std::move(func));
        }

        /// @brief Fires all registered invalidation callbacks.
        void Invalidated();

    private:
        Context& m_Context;
        u32 m_Width;
        u32 m_Height;
        u32 m_MaxImageCount;
        u32 m_ImageCount{};
        u32 m_CurrentImageIndex = 0;
        vk::Format m_Format;
        vk::ColorSpaceKHR m_ColorSpace;
        vk::SwapchainKHR m_VkSwapChain;
        vector<Ref<Image>> m_Images{};
        vector<Ref<ImageView>> m_ImageViews{};
        vector<std::function<void()>> m_OnInvalidated{};

        void Dispose();
    };
}
