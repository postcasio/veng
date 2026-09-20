#include "PerViewMaterial.h"

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/MaterialInstance.h>

namespace Veng::Renderer
{
    MaterialInstance& PerViewMaterial::Resolve(AssetManager& assets, Context& context,
                                               MaterialInstance& shared,
                                               const std::string_view name)
    {
        if (m_Shared != &shared)
        {
            // Over the parent rather than a Clone of the shared instance: the mirror then has no
            // seeded snapshot to go stale, and the copy below is what carries the shared instance's
            // overrides and writes onto it.
            m_Shared = &shared;
            m_Material = assets.BuildSync<MaterialInstance>(MaterialInstanceInfo{
                .Name = string(name),
                .Context = &context,
                .Parent = shared.GetParent(),
                .Overrides = {},
            });
            m_Synced = false;
        }

        MaterialInstance& mirror = *m_Material.Get();
        const u32 revision = shared.GetRevision();
        if (!m_Synced || m_SyncedRevision != revision)
        {
            mirror.CopyParamsFrom(shared);
            m_SyncedRevision = revision;
            m_Synced = true;
        }
        return mirror;
    }
}
