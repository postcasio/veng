#pragma once

// The Metal backend's swap chain: a CAMetalLayer on the window's view, vending one drawable per
// frame. The engine addresses swap-chain images by index, so the chain keeps a fixed ring of image
// and view wrappers and points the current one at each frame's drawable texture. Objective-C++
// under ARC; included only by the backend's own .mm sources.

#import <QuartzCore/CAMetalLayer.h>

#include "Metal.h"

#include <Veng/Renderer/Types.h>
#include <Veng/Render/DisplayModes.h>

namespace Veng
{
    class Window;
}

namespace Veng::Renderer
{
    class Context;
    class Image;
    class ImageView;

    /// @brief The display format a swap chain presents in.
    struct SwapChainSurface
    {
        /// @brief The layer's pixel format.
        MTLPixelFormat PixelFormat = MTLPixelFormatBGRA8Unorm_sRGB;
        /// @brief The engine format of that pixel format.
        Format EngineFormat = Format::BGRA8Srgb;
        /// @brief The colour space the composite encodes for.
        DisplayColorSpace ColorSpace = DisplayColorSpace::SrgbNonlinear;
        /// @brief The display mode it realizes.
        DisplayMode Mode = DisplayMode::SDR;
    };

    /// @brief The surface a requested display mode resolves to.
    ///
    /// Auto takes extended-range linear sRGB, which every Metal layer offers and which the
    /// compositor maps onto whatever the display does, SDR or HDR.
    /// @param mode The requested mode.
    /// @return The surface.
    [[nodiscard]] SwapChainSurface ResolveSwapChainSurface(DisplayMode mode);

    /// @brief A window's chain of presentable images.
    class SwapChain
    {
    public:
        /// @brief Configures @p layer for @p context's device and creates the image ring.
        /// @param context      The owning context.
        /// @param layer        The window's layer.
        /// @param extent       The drawable extent in pixels.
        /// @param imageCount   The drawables the layer keeps, and the image ring's length.
        /// @param mode         The requested display mode.
        SwapChain(Context& context, CAMetalLayer* layer, uvec2 extent, u32 imageCount,
                  DisplayMode mode);
        ~SwapChain();
        SwapChain(const SwapChain&) = delete;
        SwapChain& operator=(const SwapChain&) = delete;

        /// @brief Takes the next drawable and points the next image of the ring at it.
        /// @param window The window, pumped while no drawable is available.
        void Acquire(Window& window);

        /// @brief Hands the acquired drawable to the queue's work: called before the frame's last
        ///        command buffer commits.
        /// @param queue The queue the frame commits on.
        void WaitForDrawable(id<MTL4CommandQueue> queue) const;

        /// @brief Presents the acquired drawable once the queue's work so far completes; called
        ///        after the frame's last command buffer commits.
        /// @param queue The queue the frame committed on.
        void Present(id<MTL4CommandQueue> queue);

        /// @brief Resizes the drawables and recreates the image ring at the new extent.
        void Resize(uvec2 extent);

        /// @brief Sets whether presentation waits for the display's refresh.
        void SetPresentMode(PresentMode mode);

        /// @brief Adds a callback run whenever the image ring is recreated.
        void AddInvalidationCallback(std::function<void()> callback);

        /// @brief Runs every invalidation callback.
        void Invalidated() const;

        /// @brief The drawable extent.
        [[nodiscard]] uvec2 GetExtent() const { return m_Extent; }

        /// @brief The surface the chain presents in.
        [[nodiscard]] const SwapChainSurface& GetSurface() const { return m_Surface; }

        /// @brief The image ring's length.
        [[nodiscard]] u32 GetImageCount() const { return static_cast<u32>(m_Images.size()); }

        /// @brief The index of the image the current frame renders into.
        [[nodiscard]] u32 GetCurrentImageIndex() const { return m_CurrentIndex; }

        /// @brief The image the current frame renders into.
        [[nodiscard]] Ref<Image> GetCurrentImage() const;

        /// @brief The view of the image the current frame renders into.
        [[nodiscard]] Ref<ImageView> GetCurrentImageView() const;

    private:
        /// @brief Creates the image ring at the current extent.
        void CreateImages();

        /// @brief Points every image and view of the ring at @p texture.
        void PointAt(id<MTLTexture> texture) const;

        /// @brief The owning context.
        Context& m_Context;
        /// @brief The window's layer.
        CAMetalLayer* m_Layer = nil;
        /// @brief The drawable extent.
        uvec2 m_Extent;
        /// @brief The surface the chain presents in.
        SwapChainSurface m_Surface;
        /// @brief The image wrappers, one per ring slot.
        vector<Ref<Image>> m_Images;
        /// @brief The view wrappers, one per ring slot.
        vector<Ref<ImageView>> m_Views;
        /// @brief The drawable acquired for the current frame, or nil.
        id<CAMetalDrawable> m_Drawable = nil;
        /// @brief The ring slot of the current frame.
        u32 m_CurrentIndex = 0;
        /// @brief Drawables acquired so far, which picks the next slot.
        u64 m_Acquired = 0;
        /// @brief Callbacks run when the ring is recreated.
        vector<std::function<void()>> m_OnInvalidated;
    };
}
