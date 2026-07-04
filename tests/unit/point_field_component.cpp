// PointField-component reflection cases: the component mixes registered authored knobs (Lod,
// CellSize) with a runtime-only unregistered Ref<Renderer::PointField> Field. These cases pin the
// unregistered-field shape — the field carries no VE_FIELD, so reflection, the cooker/serializer,
// and the inspector never see it, while the authored knobs round-trip. Pure CPU — no Context, no
// Vulkan symbol touched (the Field Ref stays null, exercising only the reflection walk).

#include <doctest/doctest.h>

#include <algorithm>

#include <Veng/Reflection/JsonSerialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Entity.h>

using namespace Veng;
using Json = nlohmann::json;

namespace
{
    bool HasField(const TypeInfo& info, const char* name)
    {
        return std::ranges::any_of(info.Fields,
                                   [name](const FieldDescriptor& f) { return f.Name == name; });
    }

    JsonFieldHooks StubHooks()
    {
        // A PointField carries no Reference field; the hooks are supplied for the walker's signature
        // but are never invoked by this component.
        JsonFieldHooks hooks;
        hooks.ReadReference = [](const Json&) -> Result<Entity> { return Entity::Null; };
        hooks.WriteReference = [](Entity) -> Json { return Json(nullptr); };
        return hooks;
    }
}

TEST_CASE("PointField reflects its authored knobs but not the runtime-only Field")
{
    TypeRegistry registry;
    registry.Register<PointField>();
    const TypeInfo& info = registry.Info(registry.IdOf<PointField>());

    // The authored knobs are reflected; the runtime-only Ref is not — the inspector, cooker, and
    // serializer all walk this field list, so an absent Field is invisible to every one of them.
    CHECK(HasField(info, "Lod"));
    CHECK(HasField(info, "CellSize"));
    CHECK_FALSE(HasField(info, "Field"));
}

TEST_CASE("PointField authored knobs round-trip and the unregistered Field is never touched")
{
    TypeRegistry registry;
    registry.Register<PointField>();
    const JsonFieldHooks hooks = StubHooks();
    const TypeInfo& info = registry.Info(registry.IdOf<PointField>());

    PointField src;
    src.Lod.AggregateThreshold = 0.5f;
    src.Lod.Hysteresis = 0.1f;
    src.Lod.AggregateSplatPixels = 32.0f;
    src.Lod.AggregateIntensity = 2.0f;
    src.CellSize = 16.0f;

    const Json doc = JsonWriteFields(&src, info, registry, hooks);

    // The runtime-only Field never appears in the serialized document.
    CHECK_FALSE(doc.contains("Field"));

    PointField dst;
    REQUIRE(JsonReadFields(&dst, info, doc, registry, hooks));

    // The authored knobs load back exactly.
    CHECK(dst.Lod.AggregateThreshold == doctest::Approx(0.5f));
    CHECK(dst.Lod.Hysteresis == doctest::Approx(0.1f));
    CHECK(dst.Lod.AggregateSplatPixels == doctest::Approx(32.0f));
    CHECK(dst.Lod.AggregateIntensity == doctest::Approx(2.0f));
    CHECK(dst.CellSize == doctest::Approx(16.0f));

    // Field default-constructs to null on load and the walker never writes it — the runtime
    // resource is neither cooked nor serialized.
    CHECK(dst.Field == nullptr);
}
