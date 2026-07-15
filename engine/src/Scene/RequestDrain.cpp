#include "RequestDrain.h"

#include <Veng/Scene/Scene.h>
#include <Veng/WorldRunner.h>

#include <utility>

namespace Veng
{
    namespace
    {
        // Drains one request type across the captured world snapshot, applying the uniform
        // consumption semantics. A held-Failed component (its one-frame observation window expired)
        // is removed without re-dispatching; a Pending one is dispatched and the outcome applied.
        template <class T, class Fn>
        void DrainType(WorldRunner& runner, const vector<WorldInstanceId>& worlds,
                       const Fn& dispatch)
        {
            if (!dispatch)
            {
                return;
            }

            for (const WorldInstanceId id : worlds)
            {
                World* const world = runner.ResolveWorld(id);
                if (world == nullptr)
                {
                    // Closed by an earlier same-frame request; skip it this frame.
                    continue;
                }
                Scene& scene = world->GetScene();

                // Find the one request of this type (depth-one-per-scene). Break out of the view
                // before any structural change, so the remove below never mutates a live iteration.
                Entity holder = Entity::Null;
                for (auto [entity, request] : scene.template View<T>())
                {
                    holder = entity;
                    break;
                }
                if (holder.IsNull())
                {
                    continue;
                }

                T& request = scene.template Get<T>(holder);
                if (request.Status == RequestStatus::Failed)
                {
                    // The failure was held one frame for the stamping system to read; retire it now.
                    scene.template Remove<T>(holder);
                    continue;
                }

                string error;
                switch (dispatch(id, std::as_const(request), error))
                {
                case RequestResult::Handled:
                    scene.template Remove<T>(holder);
                    break;
                case RequestResult::Pending:
                    // Left in place, retried next frame.
                    break;
                case RequestResult::Failed:
                    request.Status = RequestStatus::Failed;
                    request.Error = std::move(error);
                    break;
                }
            }
        }
    }

    void DrainRequests(WorldRunner& runner, const RequestDispatch& dispatch)
    {
        // Snapshot the open-world ids in id order before draining: handling a request can open or
        // close worlds, and a world opened this frame must not be visited until next frame.
        vector<WorldInstanceId> worlds;
        worlds.reserve(runner.GetWorlds().size());
        for (const Unique<World>& world : runner.GetWorlds())
        {
            worlds.push_back(world->Id);
        }

        // Fixed type order: teardown (StopNet) before setup (Host/Connect/Travel), exit last, so a
        // same-frame "disconnect and quit" resolves both and a same-frame "stop net then host"
        // re-hosts rather than failing on an already-active net mode.
        DrainType<StopNetRequest>(runner, worlds, dispatch.StopNet);
        DrainType<HostRequest>(runner, worlds, dispatch.Host);
        DrainType<ConnectRequest>(runner, worlds, dispatch.Connect);
        DrainType<TravelRequest>(runner, worlds, dispatch.Travel);
        DrainType<ExitRequest>(runner, worlds, dispatch.Exit);
    }
}
