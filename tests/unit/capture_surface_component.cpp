// CaptureSurface-component reflection cases: the component mixes registered authored config (Shape,
// Resolution, Refresh) with a runtime-only unregistered Unique<CaptureSurfaceRuntime>. These pin the
// reflected shape — the runtime carries no VE_FIELD, so reflection, the cooker/serializer, and the
// inspector never see it, while the authored config round-trips authored → load → re-serialize (the
// on-disk cook/load surface). Pure CPU — no Context, no Vulkan symbol touched (the runtime stays null,
// so only the reflection walk runs).

#include <doctest/doctest.h>

#include <algorithm>

#include <glm/gtc/epsilon.hpp>
#include <glm/gtc/matrix_transform.hpp>

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

    // A consuming fragment's own read of the orientation slot: xyz imaginary, w real.
    quat Unpack(const vec4& packed)
    {
        return {packed.w, packed.x, packed.y, packed.z};
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
    CHECK(HasField(info, "Alignment"));
    CHECK(HasField(info, "Shadows"));
    CHECK(HasField(info, "TextureSlot"));
    CHECK(HasField(info, "SamplerSlot"));
    CHECK(HasField(info, "CenterSlot"));
    CHECK(HasField(info, "OrientationSlot"));
    CHECK_FALSE(HasField(info, "Runtime"));
}

TEST_CASE("CaptureSurface authored config round-trips through the reflection serializer")
{
    TypeRegistry registry;
    registry.Register<CaptureSurface>();
    const JsonFieldHooks hooks = StubHooks();
    const TypeInfo& info = registry.Info(registry.IdOf<CaptureSurface>());

    // Author the component as a cook would emit it — a planar mirror at 512, refreshing on demand in
    // its carrier's own frame with shadows asked back, binding onto descriptive non-default slot
    // names — then read it back through the same walker the prefab loader runs. Every field is
    // authored away from its default, so a field the walker drops fails rather than reading as its
    // default. Enums serialize as their enumerator names.
    const Json authored = {
        {"Shape", "PlanarReflection"},
        {"Resolution", 512},
        {"Refresh", "OnDemand"},
        {"Alignment", "Entity"},
        {"Shadows", true},
        {"TextureSlot", "CaptureMap"},
        {"SamplerSlot", "CaptureSampler"},
        {"CenterSlot", "CaptureCenter"},
        {"OrientationSlot", "CaptureFrame"},
    };

    CaptureSurface surface;
    REQUIRE(JsonReadFields(&surface, info, authored, registry, hooks));

    CHECK(surface.Shape == CaptureShape::PlanarReflection);
    CHECK(surface.Resolution == 512u);
    CHECK(surface.Refresh == CaptureRefresh::OnDemand);
    CHECK(surface.Alignment == CaptureAlignment::Entity);
    CHECK(surface.Shadows);
    CHECK(surface.TextureSlot == "CaptureMap");
    CHECK(surface.SamplerSlot == "CaptureSampler");
    CHECK(surface.CenterSlot == "CaptureCenter");
    CHECK(surface.OrientationSlot == "CaptureFrame");

    // Re-serializing yields the same authored record — the on-disk field identity is stable, and the
    // runtime never appears in the document.
    const Json out = JsonWriteFields(&surface, info, registry, hooks);
    CHECK(out["Shape"] == "PlanarReflection");
    CHECK(out["Resolution"] == 512);
    CHECK(out["Refresh"] == "OnDemand");
    CHECK(out["Alignment"] == "Entity");
    CHECK(out["Shadows"] == true);
    CHECK(out["TextureSlot"] == "CaptureMap");
    CHECK(out["SamplerSlot"] == "CaptureSampler");
    CHECK(out["CenterSlot"] == "CaptureCenter");
    CHECK(out["OrientationSlot"] == "CaptureFrame");
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
    // World-aligned faces and no shadows are the lean exterior-probe defaults: an interior probe asks
    // both back, and pays for them, because an enclosure renders flooded without its own occlusion.
    CHECK(surface.Alignment == CaptureAlignment::World);
    CHECK_FALSE(surface.Shadows);
    // The slot names default to the built-in Texture / Sampler binding.
    CHECK(surface.TextureSlot == "Texture");
    CHECK(surface.SamplerSlot == "Sampler");
    // The centre and frame slots are off by default: a material sampling only by direction in world
    // space declares neither field.
    CHECK(surface.CenterSlot.empty());
    CHECK(surface.OrientationSlot.empty());
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

TEST_CASE("PackCaptureOrientation publishes the identity for a world-aligned capture")
{
    // A World-aligned capture is driven with the identity basis — its faces are the world axes, so
    // the map's frame is world space. Publishing the identity rotation rather than nothing is what
    // lets a consumer read the slot with no branch on the alignment.
    const vec4 packed = PackCaptureOrientation(mat3(1.0f));
    CHECK(packed.x == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(packed.y == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(packed.z == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(packed.w == doctest::Approx(1.0f).epsilon(1e-6));
}

TEST_CASE("PackCaptureOrientation packs the capture-frame → world rotation, xyz then w")
{
    // An Entity-aligned capture is driven with its carrier's draw rotation. The published quaternion
    // has to agree with that basis on every direction, since the consumer rotates by its conjugate
    // to express a world direction in the map's frame — a transposed or misordered packing lands
    // here rather than as a reflection sampled from the wrong part of the map.
    const mat3 basis =
        mat3(glm::rotate(mat4(1.0f), glm::radians(50.0f), glm::normalize(vec3(0.3f, 1.0f, -0.6f))));
    const quat rotation = Unpack(PackCaptureOrientation(basis));
    CHECK(glm::length(rotation) == doctest::Approx(1.0f).epsilon(1e-5));

    const vec3 directions[] = {vec3(1.0f, 0.0f, 0.0f), vec3(0.0f, 1.0f, 0.0f),
                               vec3(0.0f, 0.0f, 1.0f), glm::normalize(vec3(0.4f, -0.7f, 0.5f))};
    for (const vec3& local : directions)
    {
        const vec3 world = basis * local;
        CHECK(glm::all(glm::epsilonEqual(rotation * local, world, 1e-5f)));
        CHECK(glm::all(glm::epsilonEqual(glm::conjugate(rotation) * world, local, 1e-5f)));
    }
}

TEST_CASE("CaptureSurface Unbind is idempotent and reachable before anything is bound")
{
    // The teardown inverse holds no material until a drive gives it one, so it is callable before the
    // runtime exists, callable again once MarkDirty has materialized the record, and repeatable —
    // which is what lets the destructor call it unconditionally. All three paths reach no material
    // and therefore no GPU resource, so this runs with no Context.
    const CaptureSurface surface;
    surface.Unbind();
    CHECK(surface.GetCapture() == nullptr);

    surface.MarkDirty();
    surface.Unbind();
    surface.Unbind();
    CHECK(surface.GetCapture() == nullptr);
}
