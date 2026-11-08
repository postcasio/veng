#include "SwapChain.h"

#include <CoreGraphics/CoreGraphics.h>

#include <Veng/Log.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Window.h>

#include "Natives.h"

namespace Veng::Renderer
{
    SwapChainSurface ResolveSwapChainSurface(const DisplayMode mode)
    {
        switch (mode)
        {
        case DisplayMode::SDR:
            return {};
        case DisplayMode::HDR10:
            return {.PixelFormat = MTLPixelFormatRGB10A2Unorm,
                    .EngineFormat = Format::A2B10G10R10Unorm,
                    .ColorSpace = DisplayColorSpace::Hdr10St2084,
                    .Mode = DisplayMode::HDR10};
        case DisplayMode::Auto:
        case DisplayMode::ExtendedLinear:
            return {.PixelFormat = MTLPixelFormatRGBA16Float,
                    .EngineFormat = Format::RGBA16Sfloat,
                    .ColorSpace = DisplayColorSpace::ExtendedLinearSrgb,
                    .Mode = DisplayMode::ExtendedLinear};
        }
        VE_ASSERT(false, "ResolveSwapChainSurface: unmapped display mode {}",
                  static_cast<u32>(mode));
        return {};
    }

    SwapChain::SwapChain(Context& context, CAMetalLayer* layer, const uvec2 extent,
                         const u32 imageCount, const DisplayMode mode)
        : m_Context(context), m_Layer(layer), m_Extent(extent),
          m_Surface(ResolveSwapChainSurface(mode))
    {
        m_Layer.device = context.GetNative().Device;
        m_Layer.pixelFormat = m_Surface.PixelFormat;
        // The composite renders into the drawable and a capture may copy out of it.
        m_Layer.framebufferOnly = NO;
        m_Layer.maximumDrawableCount = imageCount;
        m_Layer.displaySyncEnabled = YES;

        CFStringRef colorSpaceName = kCGColorSpaceSRGB;
        switch (m_Surface.ColorSpace)
        {
        case DisplayColorSpace::SrgbNonlinear:
            colorSpaceName = kCGColorSpaceSRGB;
            break;
        case DisplayColorSpace::Hdr10St2084:
            colorSpaceName = kCGColorSpaceITUR_2100_PQ;
            break;
        case DisplayColorSpace::ExtendedLinearSrgb:
            colorSpaceName = kCGColorSpaceExtendedLinearSRGB;
            break;
        }
        const CGColorSpaceRef colorSpace = CGColorSpaceCreateWithName(colorSpaceName);
        m_Layer.colorspace = colorSpace;
        CGColorSpaceRelease(colorSpace);
        m_Layer.wantsExtendedDynamicRangeContent =
            m_Surface.ColorSpace != DisplayColorSpace::SrgbNonlinear;

        m_Layer.drawableSize = CGSizeMake(m_Extent.x, m_Extent.y);
        m_Images.resize(imageCount);
        m_Views.resize(imageCount);
        CreateImages();
    }

    SwapChain::~SwapChain()
    {
        m_Drawable = nil;
        PointAt(nil);
    }

    void SwapChain::CreateImages()
    {
        // A view is made from its image's texture, so the ring is built over a drawable that is
        // then given back unpresented; each frame points the ring at its own drawable.
        id<CAMetalDrawable> drawable = nil;
        while (drawable == nil)
        {
            drawable = [m_Layer nextDrawable];
        }
        for (usize i = 0; i < m_Images.size(); ++i)
        {
            auto native = CreateUnique<Image::Native>();
            native->Texture = [drawable texture];
            m_Images[i] = Ref<Image>(
                new Image(m_Context,
                          ImageInfo{.Name = fmt::format("SwapChain Image [{}]", i),
                                    .Extent = uvec3{m_Extent.x, m_Extent.y, 1u},
                                    .MipLevels = 1,
                                    .Format = m_Surface.EngineFormat,
                                    .Type = ImageType::Type2D,
                                    .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc},
                          std::move(native), false));
            m_Views[i] =
                ImageView::Create(m_Context, ImageViewInfo{
                                                 .Name = fmt::format("SwapChain ImageView [{}]", i),
                                                 .Image = m_Images[i],
                                             });
        }
        PointAt(nil);
    }

    void SwapChain::PointAt(const id<MTLTexture> texture) const
    {
        for (usize i = 0; i < m_Images.size(); ++i)
        {
            if (m_Images[i])
            {
                m_Images[i]->GetNative().Texture = texture;
            }
            if (m_Views[i])
            {
                m_Views[i]->GetNative().Texture = texture;
            }
        }
    }

    void SwapChain::Acquire(Window& window)
    {
        VE_ASSERT(m_Drawable == nil, "SwapChain::Acquire: the last drawable was not presented");
        // nextDrawable gives up after a second when the window system holds every drawable, as it
        // does while the window cannot be shown; the window is pumped until one comes.
        m_Drawable = [m_Layer nextDrawable];
        while (m_Drawable == nil)
        {
            window.WaitUntilPresentable();
            m_Drawable = [m_Layer nextDrawable];
        }
        m_CurrentIndex = static_cast<u32>(m_Acquired++ % m_Images.size());
        const id<MTLTexture> texture = [m_Drawable texture];
        m_Images[m_CurrentIndex]->GetNative().Texture = texture;
        m_Views[m_CurrentIndex]->GetNative().Texture = texture;
    }

    void SwapChain::WaitForDrawable(const id<MTL4CommandQueue> queue) const
    {
        if (m_Drawable != nil)
        {
            [queue waitForDrawable:m_Drawable];
        }
    }

    void SwapChain::Present(const id<MTL4CommandQueue> queue)
    {
        if (m_Drawable == nil)
        {
            return;
        }
        [queue signalDrawable:m_Drawable];
        [m_Drawable present];
        // The drawable goes back to the layer's pool once presented; holding its texture would
        // keep it from being vended again. A command buffer retains nothing it renders into, so
        // the drawable lives until the frame's slot retires.
        m_Images[m_CurrentIndex]->GetNative().Texture = nil;
        m_Views[m_CurrentIndex]->GetNative().Texture = nil;
        m_Context.GetNative().Retire(m_Drawable);
        m_Drawable = nil;
    }

    void SwapChain::Resize(const uvec2 extent)
    {
        VE_ASSERT(m_Drawable == nil, "SwapChain::Resize: a drawable is still acquired");
        m_Extent = extent;
        m_Layer.drawableSize = CGSizeMake(m_Extent.x, m_Extent.y);
        CreateImages();
    }

    void SwapChain::SetPresentMode(const PresentMode mode)
    {
        // A layer either waits for the refresh or not; Mailbox has no Metal form and waits.
        m_Layer.displaySyncEnabled = mode != PresentMode::Immediate;
    }

    void SwapChain::AddInvalidationCallback(std::function<void()> callback)
    {
        m_OnInvalidated.push_back(std::move(callback));
    }

    void SwapChain::Invalidated() const
    {
        for (const auto& callback : m_OnInvalidated)
        {
            callback();
        }
    }

    Ref<Image> SwapChain::GetCurrentImage() const
    {
        return m_Images[m_CurrentIndex];
    }

    Ref<ImageView> SwapChain::GetCurrentImageView() const
    {
        return m_Views[m_CurrentIndex];
    }
}
