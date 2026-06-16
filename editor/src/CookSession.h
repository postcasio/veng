#pragma once

#include <VengEditor/CookRequest.h>

#include <Veng/Task/TaskSystem.h>
#include <Veng/Veng.h>

// CookSession: the editor exe's cook-on-demand seam. It links libveng_cook
// (the importer table) — which libveng and libveng_editor never do — and drives
// a single source asset through its importer off the render thread, returning an
// in-memory .vengpack the AssetManager can shadow-mount.
//
// The boundary holds: this type is compiled into the editor exe, not into
// libveng_editor, so the importer dependency stays out of the editor framework
// library.

namespace VengEditor
{
    class CookSession
    {
    public:
        // Submits the cook on a task worker and returns a Task whose continuation
        // fires on the main thread carrying the in-memory archive bytes (a
        // single-entry .vengpack) or a located error string. The cook is
        // Vulkan-free, so it never touches the render context.
        [[nodiscard]] Veng::Task<Veng::vector<Veng::u8>> Cook(
            const CookRequest& request, Veng::TaskSystem& tasks);
    };
}
