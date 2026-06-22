#include <Veng/Renderer/Backend/SwapChain.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/Backend/Natives.h>
#include <Veng/Renderer/Backend/TypeMapping.h>

namespace Veng::Renderer
{
    /// @brief A concrete (format, color space) surface choice and the engine modes it resolves to.
    struct FormatCandidate
    {
        vk::Format Format;
        vk::ColorSpaceKHR ColorSpace;
        DisplayColorSpace Display;
        DisplayMode Mode;
    };

    /// @brief Extended-range linear sRGB (scRGB / Apple EDR): 16-bit float, linear values.
    static constexpr FormatCandidate ExtendedLinearCandidate{
        .Format = vk::Format::eR16G16B16A16Sfloat,
        .ColorSpace = vk::ColorSpaceKHR::eExtendedSrgbLinearEXT,
        .Display = DisplayColorSpace::ExtendedLinearSrgb,
        .Mode = DisplayMode::ExtendedLinear};

    /// @brief HDR10: 10-bit Rec.2020 primaries with the ST2084 (PQ) transfer function.
    static constexpr FormatCandidate Hdr10Candidate{.Format = vk::Format::eA2B10G10R10UnormPack32,
                                                    .ColorSpace =
                                                        vk::ColorSpaceKHR::eHdr10St2084EXT,
                                                    .Display = DisplayColorSpace::Hdr10St2084,
                                                    .Mode = DisplayMode::HDR10};

    /// @brief SDR, BGRA byte order — the most common swapchain format, guaranteed in practice.
    static constexpr FormatCandidate SdrBgraCandidate{.Format = vk::Format::eB8G8R8A8Srgb,
                                                      .ColorSpace =
                                                          vk::ColorSpaceKHR::eSrgbNonlinear,
                                                      .Display = DisplayColorSpace::SrgbNonlinear,
                                                      .Mode = DisplayMode::SDR};

    /// @brief SDR, RGBA byte order — the alternate SDR ordering some drivers report.
    static constexpr FormatCandidate SdrRgbaCandidate{.Format = vk::Format::eR8G8B8A8Srgb,
                                                      .ColorSpace =
                                                          vk::ColorSpaceKHR::eSrgbNonlinear,
                                                      .Display = DisplayColorSpace::SrgbNonlinear,
                                                      .Mode = DisplayMode::SDR};

    /// @brief Human-readable name of a display mode, for logging.
    static const char* DisplayModeName(DisplayMode mode)
    {
        switch (mode)
        {
        case DisplayMode::Auto:
            return "Auto";
        case DisplayMode::SDR:
            return "SDR";
        case DisplayMode::HDR10:
            return "HDR10";
        case DisplayMode::ExtendedLinear:
            return "ExtendedLinear";
        }
        return "?";
    }

    /// @brief Resolves a requested display mode to a surface choice the device supports.
    ///
    /// Builds a priority-ordered candidate list for the mode, returns the first candidate
    /// the device reports, and falls back to any sRGB-nonlinear surface (SDR-correct under
    /// a linear write) so selection never fails on a device that offers a presentable format.
    static FormatCandidate SelectSurfaceFormat(DisplayMode mode,
                                               const vector<vk::SurfaceFormatKHR>& available)
    {
        vector<FormatCandidate> candidates;
        switch (mode)
        {
        case DisplayMode::Auto:
            candidates = {ExtendedLinearCandidate, Hdr10Candidate, SdrBgraCandidate,
                          SdrRgbaCandidate};
            break;
        case DisplayMode::ExtendedLinear:
            candidates = {ExtendedLinearCandidate, SdrBgraCandidate, SdrRgbaCandidate};
            break;
        case DisplayMode::HDR10:
            candidates = {Hdr10Candidate, SdrBgraCandidate, SdrRgbaCandidate};
            break;
        case DisplayMode::SDR:
            candidates = {SdrBgraCandidate, SdrRgbaCandidate};
            break;
        }

        for (const auto& candidate : candidates)
        {
            const bool supported =
                std::ranges::any_of(available,
                                    [&](const vk::SurfaceFormatKHR& surface)
                                    {
                                        return surface.format == candidate.Format &&
                                               surface.colorSpace == candidate.ColorSpace;
                                    });
            if (supported)
            {
                return candidate;
            }
        }

        // No preferred candidate is offered; any sRGB-nonlinear surface presents correctly
        // under the composite's linear write (the hardware applies the sRGB transfer on store).
        for (const auto& surface : available)
        {
            if (surface.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear)
            {
                return {.Format = surface.format,
                        .ColorSpace = surface.colorSpace,
                        .Display = DisplayColorSpace::SrgbNonlinear,
                        .Mode = DisplayMode::SDR};
            }
        }

        VE_ASSERT(!available.empty(), "device reports no presentable surface formats");
        return {.Format = available[0].format,
                .ColorSpace = available[0].colorSpace,
                .Display = DisplayColorSpace::SrgbNonlinear,
                .Mode = DisplayMode::SDR};
    }

    /// @brief Selects mailbox present mode if available, falling back to FIFO.
    static vk::PresentModeKHR
    GetPresentMode(const vector<vk::PresentModeKHR>& availablePresentModes)
    {
        for (const auto& availablePresentMode : availablePresentModes)
        {
            if (availablePresentMode == vk::PresentModeKHR::eMailbox)
            {
                return availablePresentMode;
            }
        }

        return vk::PresentModeKHR::eFifo;
    }

    void SwapChain::Initialize()
    {
        auto& contextNative = m_Context.GetNative();
        auto swapChainSupport = contextNative.QuerySwapChainSupport(contextNative.PhysicalDevice);

        u32 imageCount = std::max(m_MaxImageCount, swapChainSupport.Capabilities.minImageCount);

        if (swapChainSupport.Capabilities.maxImageCount > 0 &&
            imageCount > swapChainSupport.Capabilities.maxImageCount)
        {
            imageCount = swapChainSupport.Capabilities.maxImageCount;
        }

        const auto extent = GetSurfaceExtent(m_Context.GetWindow(), swapChainSupport);

        m_Width = extent.width;
        m_Height = extent.height;

        vk::SwapchainCreateInfoKHR swapChainCreateInfo{
            .surface = contextNative.Surface,
            .minImageCount = imageCount,
            .imageFormat = m_Format,
            .imageColorSpace = m_ColorSpace,
            .imageExtent = {.width = m_Width, .height = m_Height},
            .imageArrayLayers = 1,
            .imageUsage = vk::ImageUsageFlagBits::eColorAttachment};

        QueueFamilyIndices indices = contextNative.FindQueueFamilies(contextNative.PhysicalDevice);

        u32 queueFamilyIndices[] = {indices.GraphicsFamily.value(), indices.PresentFamily.value()};

        if (indices.GraphicsFamily != indices.PresentFamily)
        {
            swapChainCreateInfo.imageSharingMode = vk::SharingMode::eConcurrent;
            swapChainCreateInfo.queueFamilyIndexCount = 2;
            swapChainCreateInfo.pQueueFamilyIndices = queueFamilyIndices;
        }
        else
        {
            swapChainCreateInfo.imageSharingMode = vk::SharingMode::eExclusive;
            swapChainCreateInfo.queueFamilyIndexCount = 0;
            swapChainCreateInfo.pQueueFamilyIndices = nullptr;
        }

        swapChainCreateInfo.preTransform = swapChainSupport.Capabilities.currentTransform;

        swapChainCreateInfo.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;

        swapChainCreateInfo.presentMode = GetPresentMode(swapChainSupport.PresentModes);
        swapChainCreateInfo.clipped = VK_TRUE;

        swapChainCreateInfo.oldSwapchain = VK_NULL_HANDLE;

        m_VkSwapChain = contextNative.Device.createSwapchainKHR(swapChainCreateInfo).value;
        auto images = contextNative.Device.getSwapchainImagesKHR(m_VkSwapChain).value;

        m_ImageCount = images.size();
        m_ImageViews.reserve(m_ImageCount);
        m_Images.reserve(m_ImageCount);

        for (auto image : images)
        {
            auto native = CreateUnique<Image::Native>();
            native->Image = image;

            m_Images.emplace_back(Ref<Image>(
                new Image(m_Context,
                          ImageInfo{.Name = fmt::format("SwapChain Image [{}]", m_Images.size()),
                                    .Extent = uvec3{m_Width, m_Height, 1u},
                                    .MipLevels = 1,
                                    .Format = FromVk(m_Format),
                                    .Type = ImageType::Type2D,
                                    .Usage = ImageUsage::ColorAttachment},
                          std::move(native))));

            // Swapchain images come from vkAcquireNextImageKHR, whose
            // image-available semaphore is waited at AcquireWaitStage on submit.
            // Seed the tracked state's stage to match: the first transition's
            // srcStageMask then covers that stage, forming the execution
            // dependency validation requires against the acquire (otherwise the
            // default TopOfPipe srcStage lets the transition race ahead of the
            // semaphore wait — a WRITE_AFTER_READ hazard on first use).
            m_Images.back()->GetNative().At(0, 0).Stage = AcquireWaitStage;

            m_ImageViews.emplace_back(ImageView::Create(
                m_Context, ImageViewInfo{
                               .Name = fmt::format("SwapChain ImageView [{}]", m_ImageViews.size()),
                               .Image = m_Images.back(),
                           }));
        }
    }

    SwapChain::SwapChain(Context& context, const SwapChainInfo& info)
        : m_Context(context), m_Width(info.Width), m_Height(info.Height),
          m_MaxImageCount(info.MaxImageCount)
    {
        auto& contextNative = m_Context.GetNative();
        const auto support = contextNative.QuerySwapChainSupport(contextNative.PhysicalDevice);
        const FormatCandidate selected = SelectSurfaceFormat(info.Mode, support.Formats);

        m_Format = selected.Format;
        m_ColorSpace = selected.ColorSpace;
        m_DisplayMode = selected.Mode;
        m_DisplayColorSpace = selected.Display;

        if (info.Mode != DisplayMode::Auto && selected.Mode != info.Mode)
        {
            Log::Warn("Requested display mode {} is unavailable; using {}",
                      DisplayModeName(info.Mode), DisplayModeName(selected.Mode));
        }
        else
        {
            Log::Info("Swapchain display mode: {}", DisplayModeName(selected.Mode));
        }

        Initialize();
    }

    SwapChain::~SwapChain()
    {
        Dispose();
    }

    void SwapChain::Dispose()
    {
        m_Images.clear();
        m_ImageViews.clear();

        GetVkDevice(m_Context).destroySwapchainKHR(m_VkSwapChain);
    }

    void SwapChain::Invalidated()
    {
        for (const auto& callback : m_OnInvalidated)
        {
            callback();
        }
    }

    void SwapChain::RenderExtentChanged()
    {
        Dispose();
        Initialize();
    }

    vk::Extent2D SwapChain::GetSurfaceExtent(Window& window,
                                             SwapChainSupportDetails& swapChainSupport)
    {
        auto capabilities = swapChainSupport.Capabilities;

        if (capabilities.currentExtent.width != std::numeric_limits<u32>::max())
        {
            return capabilities.currentExtent;
        }
        else
        {
            vk::Extent2D actualExtent = {.width = window.GetWidth(), .height = window.GetHeight()};

            actualExtent.width = std::clamp(actualExtent.width, capabilities.minImageExtent.width,
                                            capabilities.maxImageExtent.width);
            actualExtent.height =
                std::clamp(actualExtent.height, capabilities.minImageExtent.height,
                           capabilities.maxImageExtent.height);

            return actualExtent;
        }
    }

    Format SwapChain::GetFormat() const
    {
        return FromVk(m_Format);
    }

    vk::Result SwapChain::AcquireNextImage(Semaphore& semaphore)
    {
        return GetVkDevice(m_Context).acquireNextImageKHR(m_VkSwapChain, UINT64_MAX,
                                                          semaphore.GetNative().Semaphore,
                                                          VK_NULL_HANDLE, &m_CurrentImageIndex);
    }
}
