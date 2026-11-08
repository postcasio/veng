#pragma once

// The layout class of the device-free synchronization vocabulary. A backend maps each value onto
// its own image state (a Vulkan image layout; nothing on a backend whose images have none), so the
// barrier decision and the render graph's schedule reason about layout without naming a device.

#include <Veng/Veng.h>

namespace Veng::Renderer
{
    /// @brief The state an image subresource is kept in for a class of use; used by barrier
    ///        derivation.
    enum class ImageLayout : u8
    {
        /// @brief Unknown or discard-on-transition layout.
        Undefined,
        /// @brief Supports all access types.
        General,
        /// @brief Optimal for color attachment read/write.
        ColorAttachment,
        /// @brief Optimal for depth/stencil attachment read/write.
        DepthAttachment,
        /// @brief Optimal for shader-sample reads.
        ShaderReadOnly,
        /// @brief Optimal for transfer source operations.
        TransferSrc,
        /// @brief Optimal for transfer destination operations.
        TransferDst,
        /// @brief Required for swapchain present.
        PresentSrc,
    };
}
