#include <Veng/Renderer/Backend/SwapChain.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/Backend/Natives.h>
#include <Veng/Renderer/Backend/TypeMapping.h>

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
            VE_ASSERT(false, "Unsupported format!");
        }
    }

    static vk::PresentModeKHR GetPresentMode(const vector<vk::PresentModeKHR>& availablePresentModes)
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
        auto& contextNative = Context::Instance().GetNative();
        auto swapChainSupport = contextNative.QuerySwapChainSupport(contextNative.PhysicalDevice);

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
            .surface = contextNative.Surface,
            .minImageCount = imageCount,
            .imageFormat = m_Format,
            .imageColorSpace = m_ColorSpace,
            .imageExtent = {m_Width, m_Height},
            .imageArrayLayers = 1,
            .imageUsage = vk::ImageUsageFlagBits::eColorAttachment
        };

        QueueFamilyIndices indices = contextNative.FindQueueFamilies(contextNative.PhysicalDevice);

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

            m_Images.emplace_back(Ref<Image>(new Image(ImageInfo{
                                                    .Name = fmt::format("SwapChain Image [{}]", m_Images.size()),
                                                    .Extent = uvec3{m_Width, m_Height, 1u},
                                                    .MipLevels = 1,
                                                    .Format = FromVk(m_Format),
                                                    .Type = ImageType::Type2D,
                                                    .Usage = ImageUsage::ColorAttachment
                                                }, std::move(native))));

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

        GetVkDevice(Context::Instance()).destroySwapchainKHR(m_VkSwapChain);
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

    Format SwapChain::GetFormat() const
    {
        return FromVk(m_Format);
    }

    vk::Result SwapChain::AcquireNextImage(Semaphore& semaphore)
    {
        return GetVkDevice(Context::Instance()).acquireNextImageKHR(m_VkSwapChain, UINT64_MAX,
                                                                     semaphore.GetNative().Semaphore, VK_NULL_HANDLE,
                                                                     &m_CurrentImageIndex);
    }
}
