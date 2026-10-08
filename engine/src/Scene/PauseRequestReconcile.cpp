#include "PauseRequestReconcile.h"

#include <Veng/Scene/Requests.h>

#include <utility>

namespace Veng
{
    RequestResult ReconcilePauseRequest(WorldRunner& runner, PauseRequestScopes& scopes,
                                        const WorldInstanceId world, const NetRole role,
                                        const PauseRequest& request, string& error)
    {
        if (role == NetRole::Client)
        {
            error = "cannot pause a client-tier world";
            return RequestResult::Failed;
        }

        const auto held = scopes.find(world.Value);
        if (request.Paused)
        {
            if (held == scopes.end())
            {
                if (WorldPauseScope scope = runner.PauseScope(world); scope.IsHeld())
                {
                    scopes.emplace(world.Value, std::move(scope));
                }
            }
        }
        else if (held != scopes.end())
        {
            scopes.erase(held);
        }
        return RequestResult::Handled;
    }

    void ForgetWorldPause(PauseRequestScopes& scopes, const WorldInstanceId world)
    {
        scopes.erase(world.Value);
    }
}
