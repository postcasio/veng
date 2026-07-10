#include <Veng/Scene/SceneSystem.h>

#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

namespace Veng
{
    bool HasAuthority(const SystemContext& context, const Scene& scene, const Entity entity)
    {
        const Authority* authority = scene.TryGet<Authority>(entity);
        const Tier tier = authority != nullptr ? authority->Tier : Tier::Server;
        switch (tier)
        {
        case Tier::Local:
            // Client-local simulation (view/UI state machines, particles-as-entities) always runs.
            return true;
        case Tier::Remote:
            // A remote mirror is never simulated — the interpolation system displays it.
            return false;
        case Tier::Server:
            // Server-authoritative state advances only on the peer that owns it.
            return context.Role == NetRole::Server;
        }
        return context.Role == NetRole::Server;
    }
}
