#pragma once

#include <Veng/Renderer/Backend/Vulkan.h>

namespace Veng::Renderer
{
    /// @brief Stateless format and layout query helpers used by the backend.
    class Utils
    {
    public:
        /// @brief Returns true if @p format has a depth component.
        static bool IsDepthFormat(vk::Format format);
        /// @brief Returns true if @p format has a stencil component.
        static bool IsStencilFormat(vk::Format format);
        /// @brief Returns the optimal attachment layout for @p format
        /// (color, depth-stencil, or depth-only).
        static vk::ImageLayout GetFormatAttachmentImageLayout(vk::Format format);
        /// @brief Returns the aspect flags implied by @p format
        /// (color, depth, stencil, or depth|stencil).
        static vk::ImageAspectFlags GetAspectFlags(vk::Format format);
        /// @brief Returns the access mask associated with @p imageLayout for
        /// barrier source/destination access fields.
        static vk::AccessFlags GetAccessMask(vk::ImageLayout imageLayout);
        /// @brief Returns the pipeline stage mask appropriate as a barrier
        /// destination for @p imageLayout.
        static vk::PipelineStageFlags GetDestinationStageMask(vk::ImageLayout imageLayout);
    };
}
