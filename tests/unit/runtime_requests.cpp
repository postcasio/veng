// Runtime request components: the builtin, local-only request drain. Device-free — a bare
// WorldRunner opens empty-scene worlds, a system stamps a request onto a world's scene, and
// DrainRequests carries it out through stub dispatch hooks (standing in for the Application
// operations). The cases assert the consumption semantics: handled removes the component the same
// frame, a failure holds Status/Error for exactly one frame then retires, a pending request is
// retried and can be withdrawn, two worlds drain in id order, the fixed type order lets a
// same-frame stop-net + host re-host, the request components are all unreplicated, and a host
// request in a client-tier world fails without reaching server state. The focus and pause cases drive
// the engine's own reconcile helpers. The policy cases wrap the stub dispatch in the engine's per-world
// request policy (ApplyRequestPolicies), the stub exit standing in for the application's exit flag.

#include <doctest/doctest.h>

#include <map>
#include <string>
#include <vector>

#include <Veng/Input.h>
#include <Veng/InputRouter.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/ViewportRegistry.h>
#include <Veng/Scene/Requests.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneSystem.h>
#include <Veng/Scene/SystemRegistry.h>
#include <Veng/World.h>
#include <Veng/WorldRunner.h>

#include <Scene/FocusRequestReconcile.h>
#include <Scene/PauseRequestReconcile.h>
#include <Scene/RequestDrain.h>

using namespace Veng;

namespace
{
    void RegisterRequests(TypeRegistry& types)
    {
        types.Register<TravelRequest>();
        types.Register<HostRequest>();
        types.Register<ConnectRequest>();
        types.Register<StopNetRequest>();
        types.Register<ExitRequest>();
        types.Register<FocusRequest>();
        types.Register<PauseRequest>();
    }

    // Two distinct seat entities standing in for router seats.
    constexpr Entity SeatA{.Index = 1, .Generation = 1};
    constexpr Entity SeatB{.Index = 2, .Generation = 1};

    // Opens an empty-scene world with no simulation — device-free, nothing to tick; the drain only
    // scans each world's scene for request components.
    WorldInstanceId OpenEmpty(WorldRunner& runner)
    {
        return runner.OpenWorld(WorldOpenInfo{.StartSimulation = false});
    }

    // Opens a world whose scene carries a simulation running no systems, so it has a pause to hold.
    WorldInstanceId OpenSimulated(WorldRunner& runner)
    {
        return runner.OpenWorld(
            WorldOpenInfo{.StartSimulation = false, .Systems = vector<SystemId>{}});
    }

    // Routes the drain's pause hook through the engine reconcile, under a fixed role for every world.
    RequestDispatch PauseDispatch(WorldRunner& runner, PauseRequestScopes& scopes,
                                  const NetRole role = NetRole::Server)
    {
        RequestDispatch dispatch;
        dispatch.Pause = [&runner, &scopes, role](const WorldInstanceId from,
                                                  const PauseRequest& request, std::string& error)
        { return ReconcilePauseRequest(runner, scopes, from, role, request, error); };
        return dispatch;
    }

    template <class T>
    Entity Stamp(WorldRunner& runner, WorldInstanceId world, T value = {})
    {
        Scene& scene = runner.ResolveWorld(world)->GetScene();
        const Entity entity = scene.CreateEntity();
        scene.Add<T>(entity, std::move(value));
        return entity;
    }

    template <class T>
    const T* Find(WorldRunner& runner, WorldInstanceId world)
    {
        return runner.ResolveWorld(world)->GetScene().TryGetFirst<T>();
    }
}

TEST_CASE("An ExitRequest is handled and removed the same frame")
{
    TypeRegistry types;
    RegisterRequests(types);
    SystemRegistry systems;
    WorldRunner runner(WorldRunnerInfo{.Types = &types, .Systems = &systems});
    const WorldInstanceId world = OpenEmpty(runner);

    bool exited = false;
    RequestDispatch dispatch;
    dispatch.Exit = [&](WorldInstanceId, const ExitRequest&, std::string&)
    {
        exited = true;
        return RequestResult::Handled;
    };

    Stamp<ExitRequest>(runner, world);
    DrainRequests(runner, dispatch);

    CHECK(exited);
    CHECK(Find<ExitRequest>(runner, world) == nullptr);
}

TEST_CASE("A failed request holds Status + Error for exactly one frame, then retires")
{
    TypeRegistry types;
    RegisterRequests(types);
    SystemRegistry systems;
    WorldRunner runner(WorldRunnerInfo{.Types = &types, .Systems = &systems});
    const WorldInstanceId world = OpenEmpty(runner);

    int hostCalls = 0;
    RequestDispatch dispatch;
    dispatch.Host = [&](WorldInstanceId, const HostRequest&, std::string& error)
    {
        ++hostCalls;
        error = "no transport available";
        return RequestResult::Failed;
    };

    Stamp<HostRequest>(runner, world);

    // Frame 1: the operation fails, and the component is held in place carrying the outcome.
    DrainRequests(runner, dispatch);
    const auto* held = Find<HostRequest>(runner, world);
    REQUIRE(held != nullptr);
    CHECK(held->Status == RequestStatus::Failed);
    CHECK(held->Error == "no transport available");
    CHECK(hostCalls == 1);

    // Frame 2: the one-frame observation window has expired; the component retires without a
    // second dispatch.
    DrainRequests(runner, dispatch);
    CHECK(Find<HostRequest>(runner, world) == nullptr);
    CHECK(hostCalls == 1);
}

TEST_CASE("A pending request is retried and can be withdrawn before it is acted on")
{
    TypeRegistry types;
    RegisterRequests(types);
    SystemRegistry systems;
    WorldRunner runner(WorldRunnerInfo{.Types = &types, .Systems = &systems});
    const WorldInstanceId world = OpenEmpty(runner);

    int connectCalls = 0;
    const bool connected = false;
    RequestDispatch dispatch;
    dispatch.Connect = [&](WorldInstanceId, const ConnectRequest&, std::string&)
    {
        ++connectCalls;
        return RequestResult::Pending; // transport still resolving
    };

    const Entity entity = Stamp<ConnectRequest>(runner, world);

    // Frame 1: not yet handleable — left Pending in place.
    DrainRequests(runner, dispatch);
    CHECK(connectCalls == 1);
    const auto* pending = Find<ConnectRequest>(runner, world);
    REQUIRE(pending != nullptr);
    CHECK(pending->Status == RequestStatus::Pending);

    // The stamping system withdraws it by removing the component itself.
    (void)runner.ResolveWorld(world)->GetScene().Remove<ConnectRequest>(entity);

    // Frame 2: nothing to dispatch — the request was never acted on.
    DrainRequests(runner, dispatch);
    CHECK(connectCalls == 1);
    CHECK_FALSE(connected);
    CHECK(Find<ConnectRequest>(runner, world) == nullptr);
}

TEST_CASE("Two requests in two worlds drain in world-id order")
{
    TypeRegistry types;
    RegisterRequests(types);
    SystemRegistry systems;
    WorldRunner runner(WorldRunnerInfo{.Types = &types, .Systems = &systems});

    // World a is opened first, so it holds the lower id; the drain visits worlds in id order.
    const WorldInstanceId a = OpenEmpty(runner);
    const WorldInstanceId b = OpenEmpty(runner);

    std::vector<WorldInstanceId> order;
    RequestDispatch dispatch;
    dispatch.Travel = [&](WorldInstanceId world, const TravelRequest&, std::string&)
    {
        order.push_back(world);
        return RequestResult::Handled;
    };

    // Stamp the higher-id world first, to prove the order follows world id, not stamp order.
    Stamp<TravelRequest>(runner, b);
    Stamp<TravelRequest>(runner, a);
    DrainRequests(runner, dispatch);

    REQUIRE(order.size() == 2);
    CHECK(order[0] == a);
    CHECK(order[1] == b);
}

TEST_CASE("The fixed type order lets a same-frame stop-net + host re-host rather than fail")
{
    TypeRegistry types;
    RegisterRequests(types);
    SystemRegistry systems;
    WorldRunner runner(WorldRunnerInfo{.Types = &types, .Systems = &systems});
    const WorldInstanceId world = OpenEmpty(runner);

    bool netActive = true; // a net mode is already active this frame
    bool hosted = false;
    RequestDispatch dispatch;
    dispatch.StopNet = [&](WorldInstanceId, const StopNetRequest&, std::string&)
    {
        netActive = false;
        return RequestResult::Handled;
    };
    dispatch.Host = [&](WorldInstanceId, const HostRequest&, std::string& error)
    {
        if (netActive)
        {
            error = "a net mode is already active";
            return RequestResult::Failed;
        }
        hosted = true;
        return RequestResult::Handled;
    };

    Stamp<StopNetRequest>(runner, world);
    Stamp<HostRequest>(runner, world);
    DrainRequests(runner, dispatch);

    // StopNet drains before Host (teardown before setup), so the host succeeds instead of failing
    // on the already-active net mode; both are handled and removed.
    CHECK(hosted);
    CHECK(Find<StopNetRequest>(runner, world) == nullptr);
    CHECK(Find<HostRequest>(runner, world) == nullptr);
}

TEST_CASE("Every request component is registered unreplicated")
{
    TypeRegistry types;
    RegisterRequests(types);

    const auto assertUnreplicated = [&](TypeId id)
    {
        REQUIRE(types.IsRegistered(id));
        CHECK_FALSE(types.Info(id).Replicated);
    };

    assertUnreplicated(types.IdOf<TravelRequest>());
    assertUnreplicated(types.IdOf<HostRequest>());
    assertUnreplicated(types.IdOf<ConnectRequest>());
    assertUnreplicated(types.IdOf<StopNetRequest>());
    assertUnreplicated(types.IdOf<ExitRequest>());
    assertUnreplicated(types.IdOf<FocusRequest>());
    assertUnreplicated(types.IdOf<PauseRequest>());
}

TEST_CASE("A FocusRequest drain reconciles the engine-owned per-seat focus token")
{
    TypeRegistry types;
    RegisterRequests(types);
    SystemRegistry systems;
    WorldRunner runner(WorldRunnerInfo{.Types = &types, .Systems = &systems});
    const WorldInstanceId world = OpenEmpty(runner);

    // The reconcile is device-free: a headless router (no window, no ICD) over a bare token list.
    Input input(nullptr);
    const Renderer::ViewportRegistry viewportRegistry;
    InputRouter router(nullptr, input, viewportRegistry);

    FocusRequestTokens tokens;
    RequestDispatch dispatch;
    dispatch.Focus =
        [&](const WorldInstanceId from, const FocusRequest& request, std::string& error)
    { return ReconcileFocusRequest(router, tokens, from, request, error); };

    // A Gameplay request captures the seat and stores one engine-held token; the component is
    // consumed the same frame (absence is the ack).
    Stamp<FocusRequest>(runner, world, FocusRequest{.Seat = SeatA, .Focus = InputFocus::Gameplay});
    DrainRequests(runner, dispatch);
    CHECK(router.IsGameplayFocused(SeatRef{.World = world, .Viewer = SeatA}));
    REQUIRE(tokens.size() == 1);
    CHECK(Find<FocusRequest>(runner, world) == nullptr);
    const FocusToken firstToken = tokens.front();

    // A second Gameplay request while already held is a no-op success: no extra push, the same token,
    // still consumed.
    Stamp<FocusRequest>(runner, world, FocusRequest{.Seat = SeatA, .Focus = InputFocus::Gameplay});
    DrainRequests(runner, dispatch);
    CHECK(router.IsGameplayFocused(SeatRef{.World = world, .Viewer = SeatA}));
    REQUIRE(tokens.size() == 1);
    CHECK(tokens.front() == firstToken);
    CHECK(Find<FocusRequest>(runner, world) == nullptr);

    // An interleaved SeatFocusScope-style token pushed ABOVE the engine's request token.
    const FocusToken scopeToken =
        router.PushFocus(SeatRef{.World = world, .Viewer = SeatA}, InputFocus::Gameplay);

    // A UI request pops only the engine's own token, wherever it sits, leaving the interleaved scope
    // token intact — so the seat stays gameplay-focused through the scope.
    Stamp<FocusRequest>(runner, world, FocusRequest{.Seat = SeatA, .Focus = InputFocus::UI});
    DrainRequests(runner, dispatch);
    CHECK(tokens.empty());
    CHECK(router.IsGameplayFocused(SeatRef{.World = world, .Viewer = SeatA}));
    CHECK(Find<FocusRequest>(runner, world) == nullptr);

    // The scope drops its own token, returning the seat to UI — proving the drain never touched it.
    router.PopFocus(scopeToken);
    CHECK(router.GetFocus(SeatRef{.World = world, .Viewer = SeatA}) == InputFocus::UI);

    // A UI request with nothing engine-held is a no-op success, still consumed.
    Stamp<FocusRequest>(runner, world, FocusRequest{.Seat = SeatA, .Focus = InputFocus::UI});
    DrainRequests(runner, dispatch);
    CHECK(tokens.empty());
    CHECK(Find<FocusRequest>(runner, world) == nullptr);
}

TEST_CASE("A FocusRequest with a null seat resolves to the router's cursor seat")
{
    TypeRegistry types;
    RegisterRequests(types);
    SystemRegistry systems;
    WorldRunner runner(WorldRunnerInfo{.Types = &types, .Systems = &systems});
    const WorldInstanceId world = OpenEmpty(runner);

    Input input(nullptr);
    const Renderer::ViewportRegistry viewportRegistry;
    InputRouter router(nullptr, input, viewportRegistry);
    router.SetCursorSeat(SeatRef{.World = world, .Viewer = SeatB});

    FocusRequestTokens tokens;
    RequestDispatch dispatch;
    dispatch.Focus =
        [&](const WorldInstanceId from, const FocusRequest& request, std::string& error)
    { return ReconcileFocusRequest(router, tokens, from, request, error); };

    // Seat left default (Entity::Null) resolves to the cursor seat, so the capture lands on SeatB.
    Stamp<FocusRequest>(runner, world, FocusRequest{.Focus = InputFocus::Gameplay});
    DrainRequests(runner, dispatch);
    CHECK(router.IsGameplayFocused(SeatRef{.World = world, .Viewer = SeatB}));
    REQUIRE(tokens.size() == 1);
    CHECK(router.IsFocusTokenOn(SeatRef{.World = world, .Viewer = SeatB}, tokens.front()));
    CHECK(Find<FocusRequest>(runner, world) == nullptr);
}

TEST_CASE("A FocusRequest releases its capture after the cursor carried it to another world")
{
    TypeRegistry types;
    RegisterRequests(types);
    SystemRegistry systems;
    WorldRunner runner(WorldRunnerInfo{.Types = &types, .Systems = &systems});
    const WorldInstanceId first = OpenEmpty(runner);
    const WorldInstanceId second = OpenEmpty(runner);

    Input input(nullptr);
    const Renderer::ViewportRegistry viewportRegistry;
    InputRouter router(nullptr, input, viewportRegistry);
    router.SetCursorSeat(SeatRef{.World = first, .Viewer = SeatA});

    FocusRequestTokens tokens;
    RequestDispatch dispatch;
    dispatch.Focus =
        [&](const WorldInstanceId from, const FocusRequest& request, std::string& error)
    { return ReconcileFocusRequest(router, tokens, from, request, error); };

    Stamp<FocusRequest>(runner, first, FocusRequest{.Focus = InputFocus::Gameplay});
    DrainRequests(runner, dispatch);

    // The second world presents the same handle; the capture follows the user onto it.
    const SeatRef arrived{.World = second, .Viewer = SeatA};
    router.MoveCursorSeat(arrived);
    REQUIRE(router.IsGameplayFocused(arrived));

    // The arrived world's own capture request is already satisfied, and pushes nothing.
    Stamp<FocusRequest>(runner, second, FocusRequest{.Seat = SeatA, .Focus = InputFocus::Gameplay});
    DrainRequests(runner, dispatch);
    CHECK(tokens.size() == 1);

    // Its release finds the carried token on its seat, so the cursor is actually freed.
    Stamp<FocusRequest>(runner, second, FocusRequest{.Seat = SeatA, .Focus = InputFocus::UI});
    DrainRequests(runner, dispatch);
    CHECK(tokens.empty());
    CHECK(router.GetFocus() == InputFocus::UI);
}

TEST_CASE("Closing a world drops the request-driven focus token it held")
{
    TypeRegistry types;
    RegisterRequests(types);
    SystemRegistry systems;
    WorldRunner runner(WorldRunnerInfo{.Types = &types, .Systems = &systems});
    const WorldInstanceId closing = OpenEmpty(runner);
    const WorldInstanceId peer = OpenEmpty(runner);

    Input input(nullptr);
    const Renderer::ViewportRegistry viewportRegistry;
    InputRouter router(nullptr, input, viewportRegistry);

    FocusRequestTokens tokens;
    RequestDispatch dispatch;
    dispatch.Focus =
        [&](const WorldInstanceId from, const FocusRequest& request, std::string& error)
    { return ReconcileFocusRequest(router, tokens, from, request, error); };
    runner.SetWorldClosedHook([&](const WorldInstanceId world)
                              { ForgetWorldFocus(router, tokens, world); });

    Stamp<FocusRequest>(runner, closing,
                        FocusRequest{.Seat = SeatA, .Focus = InputFocus::Gameplay});
    Stamp<FocusRequest>(runner, peer, FocusRequest{.Seat = SeatA, .Focus = InputFocus::Gameplay});
    DrainRequests(runner, dispatch);
    REQUIRE(tokens.size() == 2);
    const FocusToken dropped = tokens.front();

    runner.CloseWorld(closing);

    // The closed world's token is gone from the engine's list and from the router — popped as well as
    // forgotten, so the router keeps no retired record of it — while the peer's capture holds.
    CHECK(tokens.size() == 1);
    CHECK_FALSE(router.IsFocusTokenLive(dropped));
    CHECK_FALSE(router.IsFocusTokenRetired(dropped));
    CHECK(router.GetFocus(SeatRef{.World = closing, .Viewer = SeatA}) == InputFocus::UI);
    CHECK(router.IsGameplayFocused(SeatRef{.World = peer, .Viewer = SeatA}));
}

TEST_CASE("A host request in a client-tier world fails without reaching server state")
{
    TypeRegistry types;
    RegisterRequests(types);
    SystemRegistry systems;
    WorldRunner runner(WorldRunnerInfo{.Types = &types, .Systems = &systems});
    const WorldInstanceId clientWorld = OpenEmpty(runner);

    const bool serverTouched = false;
    RequestDispatch dispatch;
    dispatch.Host = [&](WorldInstanceId, const HostRequest&, std::string& error)
    {
        // The Application host hook lowers to a role check first: a client-tier world cannot host,
        // so it fails before any server-state operation runs.
        error = "cannot start hosting from a client-tier world";
        return RequestResult::Failed;
    };

    Stamp<HostRequest>(runner, clientWorld);
    DrainRequests(runner, dispatch);

    CHECK_FALSE(serverTouched);
    const auto* held = Find<HostRequest>(runner, clientWorld);
    REQUIRE(held != nullptr);
    CHECK(held->Status == RequestStatus::Failed);
    CHECK(held->Error == "cannot start hosting from a client-tier world");
}

TEST_CASE("A PauseRequest pauses its own world the same frame through one engine-held pause")
{
    TypeRegistry types;
    RegisterRequests(types);
    SystemRegistry systems;
    WorldRunner runner(WorldRunnerInfo{.Types = &types, .Systems = &systems});
    const WorldInstanceId world = OpenSimulated(runner);
    const WorldInstanceId peer = OpenSimulated(runner);

    PauseRequestScopes scopes;
    const RequestDispatch dispatch = PauseDispatch(runner, scopes);

    Stamp<PauseRequest>(runner, world);
    DrainRequests(runner, dispatch);
    CHECK(runner.IsWorldPaused(world));
    CHECK_FALSE(runner.IsWorldPaused(peer));
    CHECK(Find<PauseRequest>(runner, world) == nullptr);

    // A repeat is a no-op success, and an outside holder composes with the engine's pause.
    Stamp<PauseRequest>(runner, world);
    DrainRequests(runner, dispatch);
    CHECK(Find<PauseRequest>(runner, world) == nullptr);
    {
        const WorldPauseScope external = runner.PauseScope(world);
    }
    CHECK(runner.IsWorldPaused(world));

    // One resume releases it, so the repeat took no second ref.
    Stamp<PauseRequest>(runner, world, PauseRequest{.Paused = false});
    DrainRequests(runner, dispatch);
    CHECK_FALSE(runner.IsWorldPaused(world));
    CHECK(scopes.empty());

    // A resume with no request pause held releases nobody else's.
    const WorldPauseScope overlay = runner.PauseScope(world);
    Stamp<PauseRequest>(runner, world, PauseRequest{.Paused = false});
    DrainRequests(runner, dispatch);
    CHECK(runner.IsWorldPaused(world));
    CHECK(Find<PauseRequest>(runner, world) == nullptr);
}

TEST_CASE("A PauseRequest on a client-tier world fails and is held one frame")
{
    TypeRegistry types;
    RegisterRequests(types);
    SystemRegistry systems;
    WorldRunner runner(WorldRunnerInfo{.Types = &types, .Systems = &systems});
    const WorldInstanceId world = OpenSimulated(runner);

    PauseRequestScopes scopes;
    const RequestDispatch dispatch = PauseDispatch(runner, scopes, NetRole::Client);

    Stamp<PauseRequest>(runner, world);
    DrainRequests(runner, dispatch);
    CHECK_FALSE(runner.IsWorldPaused(world));
    CHECK(scopes.empty());
    const auto* held = Find<PauseRequest>(runner, world);
    REQUIRE(held != nullptr);
    CHECK(held->Status == RequestStatus::Failed);
    CHECK(held->Error == "cannot pause a client-tier world");

    DrainRequests(runner, dispatch);
    CHECK(Find<PauseRequest>(runner, world) == nullptr);
}

TEST_CASE("Closing a world drops its request-driven pause")
{
    TypeRegistry types;
    RegisterRequests(types);
    SystemRegistry systems;
    WorldRunner runner(WorldRunnerInfo{.Types = &types, .Systems = &systems});
    const WorldInstanceId closing = OpenSimulated(runner);
    const WorldInstanceId peer = OpenSimulated(runner);

    PauseRequestScopes scopes;
    const RequestDispatch dispatch = PauseDispatch(runner, scopes);
    runner.SetWorldClosedHook([&](const WorldInstanceId world)
                              { ForgetWorldPause(scopes, world); });

    Stamp<PauseRequest>(runner, closing);
    Stamp<PauseRequest>(runner, peer);
    DrainRequests(runner, dispatch);
    REQUIRE(scopes.size() == 2);

    runner.CloseWorld(closing);
    CHECK(scopes.size() == 1);
    CHECK(runner.IsWorldPaused(peer));
}

namespace
{
    // The policies a drain consults, keyed by world id, as Application holds them.
    using PolicyMap = std::map<u64, WorldRequestPolicy>;

    WorldRequestPolicyLookup LookupIn(const PolicyMap& policies)
    {
        return [&policies](const WorldInstanceId world) -> const WorldRequestPolicy*
        {
            const auto it = policies.find(world.Value);
            return it != policies.end() ? &it->second : nullptr;
        };
    }
}

TEST_CASE("A sandboxed world's travel fails, its focus reconciles, and its exit ends only itself")
{
    TypeRegistry types;
    RegisterRequests(types);
    SystemRegistry systems;
    WorldRunner runner(WorldRunnerInfo{.Types = &types, .Systems = &systems});
    const WorldInstanceId world = OpenEmpty(runner);

    PolicyMap policies;
    vector<WorldInstanceId> exited;
    // The session's exit closes its world, as a tool ending its play session does; the close drops
    // the policy its exit handler was read from.
    policies[world.Value] = WorldRequestPolicy{
        .Mode = WorldRequestMode::Sandboxed,
        .OnExit =
            [&](const WorldInstanceId from)
        {
            exited.push_back(from);
            policies.erase(from.Value);
            runner.CloseWorld(from);
        },
    };

    bool applicationExit = false;
    int travels = 0;
    int stops = 0;
    vector<WorldInstanceId> focused;
    RequestDispatch dispatch;
    dispatch.Travel = [&](WorldInstanceId, const TravelRequest&, std::string&)
    {
        ++travels;
        return RequestResult::Handled;
    };
    dispatch.StopNet = [&](WorldInstanceId, const StopNetRequest&, std::string&)
    {
        ++stops;
        return RequestResult::Handled;
    };
    dispatch.Focus = [&](const WorldInstanceId from, const FocusRequest&, std::string&)
    {
        focused.push_back(from);
        return RequestResult::Handled;
    };
    dispatch.Exit = [&](WorldInstanceId, const ExitRequest&, std::string&)
    {
        applicationExit = true;
        return RequestResult::Handled;
    };
    const RequestDispatch policed = ApplyRequestPolicies(dispatch, LookupIn(policies));

    Stamp<TravelRequest>(runner, world);
    Stamp<StopNetRequest>(runner, world);
    Stamp<FocusRequest>(runner, world);
    DrainRequests(runner, policed);

    // Travel never reached the application and is held failed with the reason; stop-net is a
    // handled no-op; focus reconciled as in any world.
    const auto* travel = Find<TravelRequest>(runner, world);
    REQUIRE(travel != nullptr);
    CHECK(travel->Status == RequestStatus::Failed);
    CHECK(travel->Error == "not available in a sandboxed world");
    CHECK(travels == 0);
    CHECK(Find<StopNetRequest>(runner, world) == nullptr);
    CHECK(stops == 0);
    CHECK(focused == vector<WorldInstanceId>{world});

    Stamp<ExitRequest>(runner, world);
    DrainRequests(runner, policed);

    // The exit ended the world it came from and nothing else.
    CHECK(exited == vector<WorldInstanceId>{world});
    CHECK(runner.ResolveWorld(world) == nullptr);
    CHECK_FALSE(applicationExit);
}

TEST_CASE(
    "A full world's exit handler takes its exit and it still travels; an unpoliced world exits")
{
    TypeRegistry types;
    RegisterRequests(types);
    SystemRegistry systems;
    WorldRunner runner(WorldRunnerInfo{.Types = &types, .Systems = &systems});
    const WorldInstanceId handled = OpenEmpty(runner);
    const WorldInstanceId plain = OpenEmpty(runner);

    vector<WorldInstanceId> exited;
    PolicyMap policies;
    policies[handled.Value] =
        WorldRequestPolicy{.OnExit = [&](const WorldInstanceId from) { exited.push_back(from); }};

    vector<WorldInstanceId> travelled;
    vector<WorldInstanceId> applicationExits;
    RequestDispatch dispatch;
    dispatch.Travel = [&](const WorldInstanceId from, const TravelRequest&, std::string&)
    {
        travelled.push_back(from);
        return RequestResult::Handled;
    };
    dispatch.Exit = [&](const WorldInstanceId from, const ExitRequest&, std::string&)
    {
        applicationExits.push_back(from);
        return RequestResult::Handled;
    };

    Stamp<TravelRequest>(runner, handled);
    Stamp<ExitRequest>(runner, handled);
    Stamp<ExitRequest>(runner, plain);
    DrainRequests(runner, ApplyRequestPolicies(dispatch, LookupIn(policies)));

    CHECK(travelled == vector<WorldInstanceId>{handled});
    CHECK(exited == vector<WorldInstanceId>{handled});
    CHECK(Find<ExitRequest>(runner, handled) == nullptr);
    CHECK(applicationExits == vector<WorldInstanceId>{plain});
    CHECK(Find<ExitRequest>(runner, plain) == nullptr);
}
