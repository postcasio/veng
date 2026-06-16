#include "CookSession.h"

#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>

namespace VengEditor
{
    using namespace Veng;

    Task<vector<u8>> CookSession::Cook(const CookRequest& request, TaskSystem& tasks)
    {
        // Capture by value: the worker outlives this call. The cook is Vulkan-free
        // and builds its own Cooker, so it holds no shared state.
        return tasks.Submit([request]() -> Result<vector<u8>>
        {
            Cook::Cooker cooker;
            Cook::RegisterBuiltinImporters(cooker);
            return cooker.CookSource(request.SourcePath, request.TargetId, request.Type);
        });
    }
}
