#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h>

namespace Veng::Renderer
{
    class Image;
    class Context;

    /// @brief Creation parameters for an image view.
    struct ImageViewInfo
    {
        /// @brief Debug name.
        string Name;
        /// @brief The image to create a view of.
        Ref<Image> Image;
        /// @brief View dimensionality.
        ImageViewType ViewType = ImageViewType::Type2D;
        /// @brief First mip level exposed by this view.
        u32 BaseMipLevel = 0;
        /// @brief Number of mip levels exposed.
        u32 MipLevels = 1;
        /// @brief First array layer exposed.
        u32 BaseArrayLayer = 0;
        /// @brief Number of array layers exposed.
        u32 ArrayLayers = 1;
    };

    /// @brief A view into a contiguous subresource range of a GPU image.
    class ImageView
    {
    public:
        /// @brief Creates an image view from the given parameters.
        /// @param context The owning context.
        /// @param info    Creation parameters.
        /// @return A shared reference to the new view.
        static Ref<ImageView> Create(Context& context, const ImageViewInfo& info)
        {
            return Ref<ImageView>(new ImageView(context, info));
        }

        /// @brief Defers destruction of the Vulkan image view until the GPU is done with it.
        ///
        /// Swapchain image views are destroyed immediately (the GPU is idle during swapchain teardown).
        ~ImageView();

        ImageView(const ImageView&) = delete;
        ImageView& operator=(const ImageView&) = delete;

        /// @brief Returns the view's debug name.
        [[nodiscard]] const string& GetName() const { return m_Name; }

        /// @brief Returns the texel format of the viewed image.
        [[nodiscard]] Format GetFormat() const { return m_Format; }

        /// @brief Returns the image this view was created from.
        [[nodiscard]] Ref<Image> GetImage() const { return m_Image; }

        /// @brief Returns the first mip level exposed by this view.
        ///
        /// The render graph reads the subresource range to emit barriers over exactly the
        /// affected mips/layers.
        [[nodiscard]] u32 GetBaseMipLevel() const { return m_BaseMipLevel; }

        /// @brief Returns the number of mip levels exposed by this view.
        [[nodiscard]] u32 GetMipLevels() const { return m_MipLevels; }

        /// @brief Returns the first array layer exposed by this view.
        [[nodiscard]] u32 GetBaseArrayLayer() const { return m_BaseArrayLayer; }

        /// @brief Returns the number of array layers exposed by this view.
        [[nodiscard]] u32 GetArrayLayers() const { return m_ArrayLayers; }

        /// @brief Opaque backend handle; defined in ImageView.cpp.
        struct Native;
        /// @brief Returns the backend handle. Mutable ref from a const method by design — see Native.h.
        [[nodiscard]] Native& GetNative() const;

    private:
        ImageView(Context& context, const ImageViewInfo& info);

        /// @brief The owning context; used for deferred destruction. A resource must not outlive its context.
        Context& m_Context;
        string m_Name;
        Format m_Format;
        u32 m_BaseMipLevel;
        u32 m_MipLevels;
        u32 m_BaseArrayLayer;
        u32 m_ArrayLayers;
        Unique<Native> m_Native;
        Ref<Image> m_Image;
    };
}
