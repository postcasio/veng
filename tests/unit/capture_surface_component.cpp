// CaptureSurface-component reflection cases: the component mixes registered authored config (Shape,
// Resolution, Refresh) with a runtime-only unregistered Unique<CaptureSurfaceRuntime>. These pin the
// reflected shape — the runtime carries no VE_FIELD, so reflection, the cooker/serializer, and the
// inspector never see it, while the authored config round-trips authored → load → re-serialize (the
// on-disk cook/load surface). Pure CPU — no Context, no Vulkan symbol touched (the runtime stays null,
// so only the reflection walk runs).

#include <doctest/doctest.h>

#include <algorithm>

#include <Veng/Reflection/JsonSerialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/CaptureSurface.h>
#include <Veng/Scene/Entity.h>

using namespace Veng;
using namespace Veng::Renderer;
using Json = nlohmann::json;

namespace
{
    bool HasField(const TypeInfo& info, const char* name)
    {
        return std::ranges::any_of(info.Fields,
                                   [name](const FieldDescriptor& f) { return f.Name == name; });
    }

    // A CaptureSurface carries no Reference field; the hooks are supplied for the walker's signature
    // but are never invoked by this component.
    JsonFieldHooks StubHooks()
    {
        JsonFieldHooks hooks;
        hooks.ReadReference = [](const Json&) -> Result<Entity> { return Entity::Null; };
        hooks.WriteReference = [](Entity) -> Json { return Json(nullptr); };
        return hooks;
    }
}

TEST_CASE("CaptureSurface reflects its authored config but not the runtime record")
{
    TypeRegistry registry;
    registry.Register<CaptureSurface>();
    const TypeInfo& info = registry.Info(registry.IdOf<CaptureSurface>());

    // The authored config is reflected; the runtime-only Unique is not — the inspector, cooker, and
    // serializer all walk this field list, so the runtime is invisible to every one of them.
    CHECK(HasField(info, "Shape"));
    CHECK(HasField(info, "Resolution"));
    CHECK(HasField(info, "Refresh"));
    CHECK(HasField(info, "TextureSlot"));
    CHECK(HasField(info, "SamplerSlot"));
    CHECK_FALSE(HasField(info, "Runtime"));
}

TEST_CASE("CaptureSurface authored config round-trips through the reflection serializer")
{
    TypeRegistry registry;
    registry.Register<CaptureSurface>();
    const JsonFieldHooks hooks = StubHooks();
    const TypeInfo& info = registry.Info(registry.IdOf<CaptureSurface>());

    // Author the component as a cook would emit it — a planar mirror at 512, refreshing on demand,
    // binding onto descriptive non-default slot names — then read it back through the same walker the
    // prefab loader runs. Enums serialize as their enumerator names.
    const Json authored = {
        {"Shape", "PlanarReflection"},     {"Resolution", 512},
        {"Refresh", "OnDemand"},           {"TextureSlot", "CaptureMap"},
        {"SamplerSlot", "CaptureSampler"},
    };

    CaptureSurface surface;
    REQUIRE(JsonReadFields(&surface, info, authored, registry, hooks));

    CHECK(surface.Shape == CaptureShape::PlanarReflection);
    CHECK(surface.Resolution == 512u);
    CHECK(surface.Refresh == CaptureRefresh::OnDemand);
    CHECK(surface.TextureSlot == "CaptureMap");
    CHECK(surface.SamplerSlot == "CaptureSampler");

    // Re-serializing yields the same authored record — the on-disk field identity is stable, and the
    // runtime never appears in the document.
    const Json out = JsonWriteFields(&surface, info, registry, hooks);
    CHECK(out["Shape"] == "PlanarReflection");
    CHECK(out["Resolution"] == 512);
    CHECK(out["Refresh"] == "OnDemand");
    CHECK(out["TextureSlot"] == "CaptureMap");
    CHECK(out["SamplerSlot"] == "CaptureSampler");
    CHECK_FALSE(out.contains("Runtime"));
}

TEST_CASE("CaptureSurface defaults are the every-frame environment probe")
{
    // Default construction is the live environment-probe case: a probe sampling model, a modest
    // resolution, and an every-frame refresh, with no runtime materialized.
    const CaptureSurface surface;
    CHECK(surface.Shape == CaptureShape::EnvironmentProbe);
    CHECK(surface.Resolution == 256u);
    CHECK(surface.Refresh == CaptureRefresh::EveryFrame);
    // The slot names default to the built-in Texture / Sampler binding.
    CHECK(surface.TextureSlot == "Texture");
    CHECK(surface.SamplerSlot == "Sampler");
    CHECK(surface.GetCapture() == nullptr);
    CHECK_FALSE(surface.GetOutputHandle().IsValid());
    // An every-frame capture reports refreshing before it ever drives — it re-renders each frame.
    CHECK(surface.IsRefreshing());
}

TEST_CASE("CaptureSurface MarkDirty re-arms an on-demand refresh before the runtime materializes")
{
    // An on-demand capture that has never driven still starts armed (its first drive renders the
    // map); MarkDirty is callable before the runtime exists and re-arms it, materializing the runtime
    // record without any GPU resource (the capture stays null until Drive supplies a context).
    CaptureSurface surface;
    surface.Refresh = CaptureRefresh::OnDemand;
    CHECK(surface.IsRefreshing());

    surface.MarkDirty();
    CHECK(surface.IsRefreshing());
    CHECK(surface.GetCapture() == nullptr);
}
