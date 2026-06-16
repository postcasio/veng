#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Task/TaskSystem.h>

// The cook-on-demand contract as seen by libveng_editor. The importer table
// lives in libveng_cook, which libveng_editor does not link — so the editor
// framework names a CookRequest and a cook backend (a function), and the editor
// exe (which links libveng_cook) supplies the concrete backend. This keeps the
// importer dependency in the exe layer, not the framework library.

namespace VengEditor
{
    // A request to cook one source asset on demand. SourcePath is the per-asset
    // JSON source file (e.g. assets/textures/brick.tex.json); TargetId is the
    // AssetId the cooked blob is addressable as once mounted.
    struct CookRequest
    {
        Veng::path SourcePath;
        Veng::AssetId TargetId;
        Veng::AssetType Type{};
    };

    // The cook backend the editor exe injects into the host. It runs the cook off
    // the render thread (on the supplied task system) and hands back the in-memory
    // archive bytes (a single-entry .vengpack) or a located error on the main
    // thread. The host passes its own TaskSystem, so the backend holds no engine
    // state — only the importer table.
    // The Task payload is the archive bytes; the worker reports a cook failure as
    // the Task's Result error (TaskSystem unwraps a Result-returning job), so the
    // continuation receives Result<vector<u8>>.
    using CookBackend = Veng::function<
        Veng::Task<Veng::vector<Veng::u8>>(const CookRequest&, Veng::TaskSystem&)>;
}
