#include <Veng/Renderer/Backend/SwapChain.h>
#include <Veng/Renderer/Backend/Context.h>

namespace Veng::Renderer
{
    static vk::ColorSpaceKHR GetColorSpace(vk::Format format)
    {
        switch (format)
        {
        case vk::Format::eR16G16B16A16Sfloat:
            return vk::ColorSpaceKHR::eExtendedSrgbLinearEXT;
        case vk::Format::eR8G8B8A8Srgb:
            return vk::ColorSpaceKHR::eSrgbNonlinear;
        case vk::Format::eB8G8R8A8Srgb:
            return vk::ColorSpaceKHR::eSrgbNonlinear;

        default:
            throw std::runtime_error("Unsupported format!");
        }
    }

    void SwapChain::Initialize()
    {
        auto swapChainSupport = Context::Instance().QuerySwapChainSupport(Context::Instance().GetVkPhysicalDevice());

        u32 imageCount = std::max(m_MaxImageCount, swapChainSupport.Capabilities.minImageCount);

        if (swapChainSupport.Capabilities.maxImageCount > 0 &&
            imageCount > swapChainSupport.Capabilities.maxImageCount)
        {
            imageCount = swapChainSupport.Capabilities.maxImageCount;
        }

        const auto extent = GetSurfaceExtent(Context::Instance().GetWindow(), swapChainSupport);

        m_Width = extent.width;
        m_Height = extent.height;

        vk::SwapchainCreateInfoKHR swapChainCreateInfo{
            .surface = Context::Instance().GetVkSurface(),
            .minImageCount = imageCount,
            .imageFormat = m_Format,
            .imageColorSpace = m_ColorSpace,
            .imageExtent = {m_Width, m_Height},
            .imageArrayLayers = 1,
            .imageUsage = vk::ImageUsageFlagBits::eColorAttachment
        };

        QueueFamilyIndices indices = Context::Instance().FindQueueFamilies(Context::Instance().GetVkPhysicalDevice());

        u32 queueFamilyIndices[] = {
            indices.GraphicsFamily.value(),
            indices.PresentFamily.value()
        };

        if (indices.GraphicsFamily != indices.PresentFamily)
        {
            swapChainCreateInfo.imageSharingMode = vk::SharingMode::eConcurrent;
            swapChainCreateInfo.queueFamilyIndexCount = 2;
            swapChainCreateInfo.pQueueFamilyIndices = queueFamilyIndices;
        }
        else
        {
            swapChainCreateInfo.imageSharingMode = vk::SharingMode::eExclusive;
            swapChainCreateInfo.queueFamilyIndexCount = 0; // Optional
            swapChainCreateInfo.pQueueFamilyIndices = nullptr; // Optional
        }

        swapChainCreateInfo.preTransform = swapChainSupport.Capabilities.currentTransform;

        swapChainCreateInfo.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;

        swapChainCreateInfo.presentMode = Context::GetPresentMode(swapChainSupport.PresentModes);
        swapChainCreateInfo.clipped = VK_TRUE;

        swapChainCreateInfo.oldSwapchain = VK_NULL_HANDLE;

        m_VkSwapChain = Context::Instance().GetVkDevice().createSwapchainKHR(swapChainCreateInfo);
        auto images = Context::Instance().GetVkDevice().getSwapchainImagesKHR(m_VkSwapChain);

        m_ImageCount = images.size();
        m_ImageViews.reserve(m_ImageCount);
        m_Images.reserve(m_ImageCount);

        for (auto image : images)
        {
            m_Images.emplace_back(Image::Create(image, ImageInfo{
                                                    .Name = fmt::format("SwapChain Image [{}]", m_Images.size()),
                                                    .Extent = uvec3{m_Width, m_Height, 1u},
                                                    .MipLevels = 1,
                                                    .Format = m_Format,
                                                    .Type = vk::ImageType::e2D,
                                                    .Usage = vk::ImageUsageFlagBits::eColorAttachment
                                                }));

            m_ImageViews.emplace_back(ImageView::Create(ImageViewInfo{
                .Name = fmt::format("SwapChain ImageView [{}]", m_ImageViews.size()),
                .Image = m_Images.back(),
            }));
        }
    }

    SwapChain::SwapChain(const SwapChainInfo& info) :
        m_Width(info.Width),
        m_Height(info.Height),
        m_MaxImageCount(info.MaxImageCount),
        m_Format(info.Format),
        m_ColorSpace(GetColorSpace(info.Format))
    {
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

        Context::Instance().GetVkDevice().destroySwapchainKHR(m_VkSwapChain);
    }

    void SwapChain::Invalidated()
    {
        // Fire invalidation callbacks after the swapchain has been recreated
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

    vk::Extent2D SwapChain::GetSurfaceExtent(Window& window, SwapChainSupportDetails& swapChainSupport)
    {
        auto capabilities = swapChainSupport.Capabilities;

        if (capabilities.currentExtent.width !=
            std::numeric_limits<u32>::max())
        {
            return capabilities.currentExtent;
        }
        else
        {
            vk::Extent2D actualExtent = {window.GetWidth(), window.GetHeight()};

            actualExtent.width =
                std::clamp(actualExtent.width, capabilities.minImageExtent.width,
                           capabilities.maxImageExtent.width);
            actualExtent.height =
                std::clamp(actualExtent.height, capabilities.minImageExtent.height,
                           capabilities.maxImageExtent.height);

            return actualExtent;
        }
    }

    vk::Result SwapChain::AcquireNextImage(Semaphore& semaphore)
    {
        return Context::Instance().GetVkDevice().acquireNextImageKHR(m_VkSwapChain, UINT64_MAX,
                                                                     semaphore.GetVkSemaphore(), VK_NULL_HANDLE,
                                                                     &m_CurrentImageIndex);
    }
}
