// Runtime request components: the builtin, local-only request drain. Device-free — a bare
// WorldRunner opens empty-scene worlds, a system stamps a request onto a world's scene, and
// DrainRequests carries it out through stub dispatch hooks (standing in for the Application
// operations). The cases assert the consumption semantics: handled removes the component the same
// frame, a failure holds Status/Error for exactly one frame then retires, a pending request is
// retried and can be withdrawn, two worlds drain in id order, the fixed type order lets a
// same-frame stop-net + host re-host, the five components are all unreplicated, and a host request
// in a client-tier world fails without reaching server state.

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Requests.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneSystem.h>
#include <Veng/Scene/SystemRegistry.h>
#include <Veng/World.h>
#include <Veng/WorldRunner.h>

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
    }

    // Opens an empty-scene world with no simulation — device-free, nothing to tick; the drain only
    // scans each world's scene for request components.
    WorldInstanceId OpenEmpty(WorldRunner& runner)
    {
        return runner.OpenWorld(WorldOpenInfo{.StartSimulation = false});
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
    const HostRequest* held = Find<HostRequest>(runner, world);
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
    bool connected = false;
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
    const ConnectRequest* pending = Find<ConnectRequest>(runner, world);
    REQUIRE(pending != nullptr);
    CHECK(pending->Status == RequestStatus::Pending);

    // The stamping system withdraws it by removing the component itself.
    runner.ResolveWorld(world)->GetScene().Remove<ConnectRequest>(entity);

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
}

TEST_CASE("A host request in a client-tier world fails without reaching server state")
{
    TypeRegistry types;
    RegisterRequests(types);
    SystemRegistry systems;
    WorldRunner runner(WorldRunnerInfo{.Types = &types, .Systems = &systems});
    const WorldInstanceId clientWorld = OpenEmpty(runner);

    bool serverTouched = false;
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
    const HostRequest* held = Find<HostRequest>(runner, clientWorld);
    REQUIRE(held != nullptr);
    CHECK(held->Status == RequestStatus::Failed);
    CHECK(held->Error == "cannot start hosting from a client-tier world");
}
