#pragma once

#include <Veng/Export.h>

namespace Veng::Renderer
{
    /// @brief Returns true when the GPU backend compiled in finds a device it can use.
    ///
    /// A cheap probe that creates nothing lasting: what a process asks before it creates a
    /// Context, so a GPU test can report itself skipped on a machine with no usable device rather
    /// than fail. Answered by the backend compiled in (VE_GPU_BACKEND).
    /// @return True when a Context could be created on this machine.
    [[nodiscard]] VE_API bool HasGpuDevice();

    /// @brief Returns true when a device the backend finds samples ASTC LDR textures.
    ///
    /// Gates work whose cooked assets are ASTC-compressed: a device without the codec samples
    /// the fallback instead. False when HasGpuDevice() is.
    /// @return True when some usable device supports ASTC LDR.
    [[nodiscard]] VE_API bool HasGpuDeviceWithAstc();
}
