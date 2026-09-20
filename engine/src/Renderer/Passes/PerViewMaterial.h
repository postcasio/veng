#pragma once

#include <string_view>

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>

namespace Veng
{
    class AssetManager;
    class MaterialInstance;
}

namespace Veng::Renderer
{
    class Context;

    /// @brief One renderer's own MaterialInstance standing in for a shared one, for per-view writes.
    ///
    /// A material's parameter block rings by frame-in-flight and not by view
    /// (BindlessRegistry::MaterialArenaBytes), so a value a pass writes while recording one viewport
    /// is what every viewport sharing that instance reads at submit. A pass writing a per-view value
    /// — the recording view's extents, the bindless handle of a target the pass owns per viewport —
    /// therefore holds one of these per shared instance and draws through the mirror it returns.
    ///
    /// The mirror is built over the shared instance's parent and carries a copy of the shared
    /// instance's block, re-copied whenever the shared instance's revision moves, so an authored
    /// override and any later write to the shared instance both still reach the draw.
    class PerViewMaterial
    {
    public:
        /// @brief Returns this renderer's mirror of @p shared, building or re-syncing it as needed.
        ///
        /// Builds the mirror on the first call and whenever @p shared is a different instance, and
        /// copies @p shared's block into it whenever @p shared's revision has moved since the last
        /// copy. Render-thread only — the build registers a parameter block.
        /// @param assets  Asset manager the mirror is built through.
        /// @param context Render context the mirror allocates its parameter block on.
        /// @param shared  The shared, finalized instance to stand in for.
        /// @param name    Debug name for the mirror, used on the calls that build one.
        /// @return The mirror, ready for this view's writes and its own Bind.
        [[nodiscard]] MaterialInstance& Resolve(AssetManager& assets, Context& context,
                                                MaterialInstance& shared, std::string_view name);

    private:
        /// @brief The shared instance the mirror stands in for; compared, never dereferenced.
        const MaterialInstance* m_Shared = nullptr;
        /// @brief This renderer's own instance over the shared one's parent.
        AssetHandle<MaterialInstance> m_Material;
        /// @brief The shared instance's revision the mirror's block was last copied at.
        u32 m_SyncedRevision = 0;
        /// @brief Whether a copy has run since the mirror was built.
        bool m_Synced = false;
    };
}
