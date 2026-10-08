#include "RequestDrain.h"

#include <Veng/Scene/Scene.h>
#include <Veng/WorldRunner.h>

#include <utility>

namespace Veng
{
    namespace
    {
        constexpr string_view SandboxRefusal = "not available in a sandboxed world";

        bool IsSandboxed(const WorldRequestPolicyLookup& policies, const WorldInstanceId world)
        {
            const WorldRequestPolicy* policy = policies(world);
            return policy != nullptr && policy->Mode == WorldRequestMode::Sandboxed;
        }

        // Wraps one operation so a sandboxed world's request fails before reaching it; an operation
        // the dispatch leaves unset stays unset, so the drain still skips its type.
        template <class T>
        function<RequestResult(WorldInstanceId, const T&, string&)> RefuseSandboxed(
            const function<RequestResult(WorldInstanceId, const T&, string&)>& operation,
            const Ref<const WorldRequestPolicyLookup>& policies)
        {
            if (!operation)
            {
                return {};
            }
            return
                [operation, policies](const WorldInstanceId world, const T& request, string& error)
            {
                if (IsSandboxed(*policies, world))
                {
                    error = string{SandboxRefusal};
                    return RequestResult::Failed;
                }
                return operation(world, request, error);
            };
        }

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
                    (void)scene.template Remove<T>(holder);
                    continue;
                }

                string error;
                const RequestResult result = dispatch(id, std::as_const(request), error);
                // Re-resolved before the outcome lands: the dispatch may have closed the world, or
                // stopped it and let an OnStop restructure its scene under the request reference.
                World* const after = runner.ResolveWorld(id);
                Scene* const live = after != nullptr ? &after->GetScene() : nullptr;
                T* const held = live != nullptr && live->IsAlive(holder)
                                    ? live->template TryGet<T>(holder)
                                    : nullptr;
                if (held == nullptr)
                {
                    continue;
                }
                switch (result)
                {
                case RequestResult::Handled:
                    (void)live->template Remove<T>(holder);
                    break;
                case RequestResult::Pending:
                    // Left in place, retried next frame.
                    break;
                case RequestResult::Failed:
                    held->Status = RequestStatus::Failed;
                    held->Error = std::move(error);
                    break;
                }
            }
        }
    }

    RequestDispatch ApplyRequestPolicies(RequestDispatch dispatch, WorldRequestPolicyLookup lookup)
    {
        const auto policies = CreateRef<const WorldRequestPolicyLookup>(std::move(lookup));

        RequestDispatch result = dispatch;
        result.Travel = RefuseSandboxed(dispatch.Travel, policies);
        result.Host = RefuseSandboxed(dispatch.Host, policies);
        result.Connect = RefuseSandboxed(dispatch.Connect, policies);
        if (dispatch.StopNet)
        {
            result.StopNet =
                [policies, stopNet = std::move(dispatch.StopNet)](
                    const WorldInstanceId world, const StopNetRequest& request, string& error)
            {
                // A sandboxed world has no transport of its own, so there is nothing for it to stop.
                return IsSandboxed(*policies, world) ? RequestResult::Handled
                                                     : stopNet(world, request, error);
            };
        }
        result.Exit = [policies, exit = std::move(dispatch.Exit)](
                          const WorldInstanceId world, const ExitRequest& request, string& error)
        {
            const WorldRequestPolicy* policy = (*policies)(world);
            if (policy != nullptr && policy->OnExit)
            {
                // Copied first: an OnExit closing its world drops the policy it is read from.
                const function<void(WorldInstanceId)> onExit = policy->OnExit;
                onExit(world);
                return RequestResult::Handled;
            }
            if (policy != nullptr && policy->Mode == WorldRequestMode::Sandboxed)
            {
                error = string{SandboxRefusal};
                return RequestResult::Failed;
            }
            return exit ? exit(world, request, error) : RequestResult::Pending;
        };
        return result;
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
        DrainType<FocusRequest>(runner, worlds, dispatch.Focus);
        DrainType<PauseRequest>(runner, worlds, dispatch.Pause);
        DrainType<ExitRequest>(runner, worlds, dispatch.Exit);
    }
}
