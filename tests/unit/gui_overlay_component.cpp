// GuiOverlay-component reflection cases: the component mixes registered authored fields (Document,
// Layer, Interactive, TargetSeat) with a runtime-only unregistered Unique<GuiOverlayRuntime>. These
// pin the reflected shape — the runtime carries no VE_FIELD, so reflection, the cooker/serializer,
// and the inspector never see it, while the authored fields round-trip authored → load → re-serialize
// (the on-disk cook/load surface). Pure CPU — no Context, no Vulkan symbol touched (the runtime stays
// null, exercising only the reflection walk).

#include <doctest/doctest.h>

#include <algorithm>

#include <Veng/Gui/Overlay.h>
#include <Veng/Reflection/JsonSerialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Entity.h>

#include <nlohmann/json.hpp>

using namespace Veng;
using Json = nlohmann::json;

namespace
{
    bool HasField(const TypeInfo& info, const char* name)
    {
        return std::ranges::any_of(info.Fields,
                                   [name](const FieldDescriptor& f) { return f.Name == name; });
    }

    // A Reference hook pair addressing entities by their raw Index — enough for a symmetric
    // round-trip of the TargetSeat field with no live Scene on either side.
    JsonFieldHooks StubHooks()
    {
        JsonFieldHooks hooks;
        hooks.ReadReference = [](const Json& value) -> Result<Entity>
        {
            if (value.is_null())
            {
                return Entity::Null;
            }
            if (!value.is_number_unsigned())
            {
                return std::unexpected(string("expected an unsigned entity index"));
            }
            return Entity{.Index = value.get<u32>(), .Generation = 0};
        };
        hooks.WriteReference = [](Entity entity) -> Json
        {
            if (entity.IsNull())
            {
                return Json(nullptr);
            }
            return entity.Index;
        };
        return hooks;
    }
}

TEST_CASE("GuiOverlay reflects its authored fields but not the runtime record")
{
    TypeRegistry registry;
    registry.Register<GuiOverlay>();
    const TypeInfo& info = registry.Info(registry.IdOf<GuiOverlay>());

    // The authored fields are reflected; the runtime-only Unique is not — the inspector, cooker, and
    // serializer all walk this field list, so the runtime is invisible to every one of them.
    CHECK(HasField(info, "Document"));
    CHECK(HasField(info, "Layer"));
    CHECK(HasField(info, "Driver"));
    CHECK(HasField(info, "Interactive"));
    CHECK(HasField(info, "TargetSeat"));
    CHECK_FALSE(HasField(info, "Runtime"));
}

TEST_CASE("GuiOverlay authored fields round-trip through the reflection serializer")
{
    TypeRegistry registry;
    registry.Register<GuiOverlay>();
    const JsonFieldHooks hooks = StubHooks();
    const TypeInfo& info = registry.Info(registry.IdOf<GuiOverlay>());

    // Author the component as a cook would emit it — a document id, a stack layer, interactive on,
    // and a target-seat reference — then read it back through the same walker the prefab loader runs.
    const Json authored = {
        {"Document", "0xA09AA8B60AEAA8BE"},
        {"Layer", 3},
        {"Driver", "0xE9906144475EB699"},
        {"Interactive", true},
        {"TargetSeat", 5u},
    };

    GuiOverlay overlay;
    REQUIRE(JsonReadFields(&overlay, info, authored, registry, hooks));

    CHECK(overlay.Document.Id().Value == 0xA09AA8B60AEAA8BEULL);
    CHECK(overlay.Layer == 3);
    // The GuiDriverId leaf authors as a hex-id string, exactly like a minted id.
    CHECK(static_cast<u64>(overlay.Driver) == 0xE9906144475EB699ULL);
    CHECK(overlay.Interactive);
    CHECK(overlay.TargetSeat.Index == 5u);

    // Re-serializing yields the same authored record — the on-disk field identity is stable.
    const Json out = JsonWriteFields(&overlay, info, registry, hooks);
    CHECK(out["Document"] == "0xA09AA8B60AEAA8BE");
    CHECK(out["Layer"] == 3);
    CHECK(out["Driver"] == "0xE9906144475EB699");
    CHECK(out["Interactive"] == true);
    CHECK(out["TargetSeat"] == 5u);
}

TEST_CASE("GuiOverlay defaults are the single-viewport display-only HUD")
{
    // Default construction is the every-single-player-HUD case: no document, bottom layer, display
    // only, unbound seat (claimed by the sole/primary presenting viewport).
    const GuiOverlay overlay;
    CHECK_FALSE(overlay.Document.Id().IsValid());
    CHECK(overlay.Layer == 0);
    CHECK(overlay.Driver == GuiDriverId::Null); // undriven by default
    CHECK_FALSE(overlay.Interactive);
    CHECK(overlay.TargetSeat.IsNull());
    CHECK(overlay.GetHost() == nullptr);
    CHECK(overlay.GetDocument() == nullptr);
}
