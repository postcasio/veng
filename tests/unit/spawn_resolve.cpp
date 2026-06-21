// Spawn-resolve seam wiring: a VE_RESOLVE'd component leaves TypeInfo::SpawnResolve
// non-null after Register<T>(), an opt-out type leaves it null, and
// HasSpawnResolver<T> reflects the opt-in. Pure CPU — the resolver here is
// device-free, and the test only checks the wired pointer, never fires it (firing
// needs an AssetManager, hence a Context, exercised in the gpu band).

#include <doctest/doctest.h>

#include <Veng/Reflection/Reflect.h>
#include <Veng/Reflection/TypeRegistry.h>

namespace Veng
{
    class Scene;
    class AssetManager;
    struct Entity;
}

using namespace Veng;

namespace
{
    // A throwaway resolver-bearing component: the resolver sets Marker, proving the
    // typed resolver runs through the erased thunk without any GPU dependency.
    struct ResolveProbe
    {
        i32 Marker = 0;
    };

    // A second component with no VE_RESOLVE — the opt-out case whose SpawnResolve
    // must stay null.
    struct PlainProbe
    {
        i32 Value = 0;
    };

    void ResolveProbeFn(ResolveProbe& probe, Scene&, Entity, AssetManager&)
    {
        probe.Marker = 42;
    }
}

VE_REFLECT(ResolveProbe, 0x6B3F0C9A41D27E58ULL)
VE_FIELD(Marker)
VE_REFLECT_END();

VE_RESOLVE(ResolveProbe, ResolveProbeFn);

VE_REFLECT(PlainProbe, 0x2C8E15A7D9B40F36ULL)
VE_FIELD(Value)
VE_REFLECT_END();

TEST_CASE("VE_RESOLVE opts a component into the spawn-resolve seam")
{
    static_assert(HasSpawnResolver<ResolveProbe>,
                  "ResolveProbe carries a VE_RESOLVE specialisation");
    static_assert(!HasSpawnResolver<PlainProbe>, "PlainProbe declares no resolver");

    TypeRegistry registry;
    registry.Register<ResolveProbe>();
    registry.Register<PlainProbe>();

    const TypeInfo& resolverInfo = registry.Info(registry.IdOf<ResolveProbe>());
    CHECK(resolverInfo.SpawnResolve != nullptr);

    const TypeInfo& plainInfo = registry.Info(registry.IdOf<PlainProbe>());
    CHECK(plainInfo.SpawnResolve == nullptr);
}
