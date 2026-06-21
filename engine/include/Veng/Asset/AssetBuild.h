#pragma once

#include <Veng/Veng.h>
#include <Veng/Result.h>
#include <Veng/Task/TaskSystem.h>

namespace Veng::Renderer
{
    class Context;
    class PipelineLayout;
}

namespace Veng
{
    class Texture;
    struct TextureInfo;
    class Material;
    struct MaterialInfo;
    class Mesh;
    struct MeshData;
}

namespace Veng::Detail
{
    /// @brief A worker-built, not-yet-finalized asset paired with its main-thread finalize step.
    ///
    /// AssetManager::Build runs an asset's worker-legal construction (decode/upload) off the
    /// render thread, yielding this pair, then runs Finalize on the main-thread continuation
    /// pump — keeping the render-thread-only bindless registration off the worker. A null
    /// Finalize means the asset is resident as built (a Mesh has no bindless step).
    /// @tparam T  The asset resource type produced.
    template <typename T>
    struct BuiltAsset
    {
        /// @brief The constructed resource, unfinalized until Finalize runs.
        Ref<T> Resource;
        /// @brief Main-thread finalize step (bindless registration), or null if none.
        function<VoidResult()> Finalize;
    };

    /// @brief Submits the worker-legal build of a Texture, returning its streaming result.
    ///
    /// The worker creates the image/view/sampler and records the transfer-queue upload; the
    /// returned BuiltAsset's Finalize registers it into the bindless registry on the main thread.
    /// @param context Render context the texture is created on.
    /// @param tasks   Task system the worker job and transfer upload run on.
    /// @param info    Texture description; its pixels are copied into the worker job.
    /// @return A Task yielding the unfinalized texture and its finalize step.
    [[nodiscard]] Task<BuiltAsset<Texture>> SubmitAssetBuild(Renderer::Context& context,
                                                             TaskSystem& tasks, TextureInfo info);

    /// @brief Submits the worker-legal build of a Mesh, returning its streaming result.
    ///
    /// The worker creates the canonical vertex + index buffers and memcpys the geometry into
    /// them (host-visible, no transfer queue). The Mesh has no bindless step, so the returned
    /// BuiltAsset's Finalize is null.
    /// @param context Render context the buffers are created on.
    /// @param tasks   Task system the worker job runs on.
    /// @param data    CPU geometry, moved into the worker job.
    /// @param name    Debug name for the mesh and its buffers.
    /// @return A Task yielding the resident mesh and a null finalize step.
    [[nodiscard]] Task<BuiltAsset<Mesh>>
    SubmitAssetBuild(Renderer::Context& context, TaskSystem& tasks, MeshData data, string name);

    /// @brief Submits the worker-legal build of a Material, returning its streaming result.
    ///
    /// The worker constructs the material from the info; the returned BuiltAsset's Finalize
    /// patches its bindless handle fields, allocates the per-material SSBO slot, and writes the
    /// parameter block on the main thread.
    /// @param context Unused; the material's context rides MaterialInfo::Context.
    /// @param tasks   Task system the worker job runs on.
    /// @param info    Material description; its shaders and textures must already be resident.
    /// @param layout  The reflected pipeline layout passed to Finalize.
    /// @return A Task yielding the unfinalized material and its finalize step.
    [[nodiscard]] Task<BuiltAsset<Material>> SubmitAssetBuild(Renderer::Context& context,
                                                              TaskSystem& tasks, MaterialInfo info,
                                                              Ref<Renderer::PipelineLayout> layout);
}
