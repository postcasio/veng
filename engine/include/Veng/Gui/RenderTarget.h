#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Types.h>

namespace Veng::Renderer
{
    class Context;
    class Image;
    class ImageView;
}

namespace Veng::Gui
{
    /// @brief Construction parameters for RenderTarget.
    struct RenderTargetInfo
    {
        /// @brief Context the color image, view, and bindless handle are allocated on.
        Renderer::Context& Context;
        /// @brief Initial color-image extent, in pixels; both components must be positive.
        uvec2 Extent;
        /// @brief Debug name prefix for the owned image and view.
        string_view Name = "Gui Render Target";
    };

    /// @brief A persistent, sampleable HDR color target a document renders into.
    ///
    /// Owns an RGBA16Sfloat color image, its view, and a bindless texture handle, sized to a
    /// requested extent and resizable on demand. A driven document records into it through
    /// Renderer::GuiScenePass and hands its handle downstream through GetOutputHandle() — the
    /// render-to-texture contract a consumer binds with Renderer::MaterialInstance::SetTextureHandle,
    /// byte-for-byte the one an Offscreen viewport offers. The half-float format preserves color
    /// components above 1.0 end to end, so an emissive document texel survives into a sampling pass
    /// unclamped.
    ///
    /// Single-owner (Unique); Create is the factory. The bindless handle is released, and the image
    /// and view retired, at destruction.
    class RenderTarget
    {
    public:
        /// @brief The fixed color format of the owned image: half-float RGBA.
        static constexpr Renderer::Format ColorFormat = Renderer::Format::RGBA16Sfloat;

        /// @brief Creates the target's color image, view, and bindless handle.
        /// @param info  Construction parameters.
        /// @return The owning Unique.
        static Unique<RenderTarget> Create(const RenderTargetInfo& info);

        /// @brief Releases the bindless handle and retires the owned image and view.
        ~RenderTarget();

        RenderTarget(const RenderTarget&) = delete;
        RenderTarget& operator=(const RenderTarget&) = delete;

        /// @brief Resizes the color image to a new extent, reallocating its view and handle.
        ///
        /// A zero component or an unchanged extent is a no-op. Invalidates GetOutput() and
        /// GetOutputHandle() when the extent changes — re-fetch after.
        /// @param extent  The new extent, in pixels.
        void Resize(uvec2 extent);

        /// @brief Returns the color image view a producer records into and a consumer samples.
        ///
        /// Invalidated by a Resize that changes the extent — re-fetch after.
        [[nodiscard]] const Ref<Renderer::ImageView>& GetOutput() const;

        /// @brief Returns the bindless handle naming the color image, for a downstream sampler.
        ///
        /// The handle a material binds with Renderer::MaterialInstance::SetTextureHandle. A consumer
        /// registration is ordered after the producer records into the target. Invalidated by a
        /// Resize that changes the extent — re-fetch after.
        [[nodiscard]] Renderer::TextureHandle GetOutputHandle() const;

        /// @brief Returns the current color-image extent, in pixels.
        [[nodiscard]] uvec2 GetExtent() const;

        /// @brief Returns the color format, always ColorFormat.
        [[nodiscard]] Renderer::Format GetFormat() const;

    private:
        explicit RenderTarget(const RenderTargetInfo& info);

        /// @brief (Re)allocates the color image, view, and bindless handle at the current extent.
        void Allocate();

        Renderer::Context& m_Context;
        uvec2 m_Extent;
        string m_Name;
        Ref<Renderer::Image> m_Image;
        Ref<Renderer::ImageView> m_View;
        Renderer::TextureHandle m_Handle;
    };
}
