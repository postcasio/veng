#pragma once

#include <VengEditor/CookRequest.h>

#include <Veng/Task/TaskSystem.h>
#include <Veng/Veng.h>

/// @brief Cook-on-demand driver compiled into the editor exe (not libveng_editor).
///
/// Links libveng_cook (the importer table), which libveng and libveng_editor
/// never link, keeping the importer dependency confined to the exe layer.

namespace VengEditor
{
    /// @brief Drives a single source asset through its importer off the render thread.
    class CookSession
    {
    public:
        /// @brief Submits the cook on a task worker and returns a Task.
        ///
        /// The continuation fires on the main thread carrying the in-memory archive
        /// bytes (a single-entry .vengpack) or a located error string. The cook is
        /// Vulkan-free and never touches the render context.
        /// @param request The source asset and target id to cook.
        /// @param tasks   Task system used to schedule the worker.
        /// @return Task resolving to the archive bytes or an error.
        [[nodiscard]] Veng::Task<Veng::vector<Veng::u8>> Cook(const CookRequest& request,
                                                              Veng::TaskSystem& tasks);
    };
}
