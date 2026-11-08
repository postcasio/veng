#pragma once

#include <Veng/Export.h>
#include "Vulkan.h"

namespace Veng::Renderer
{
    /// @brief Stateless format and layout query helpers used by the backend.
    class VE_API Utils
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
    };
}
