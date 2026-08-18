// The GuiSurface component's reflected shape: the authored fields a cook/load round-trip carries,
// and the Driver id among them — the field naming the per-instance presentation binding the engine
// instantiates, authored exactly like GuiOverlay's. Pure CPU: the runtime record stays null, so only
// the reflection walk runs (no Context, no Vulkan symbol touched).

#include <doctest/doctest.h>

#include <algorithm>

#include <Veng/Gui/Surface.h>
#include <Veng/Reflection/JsonSerialize.h>
#include <Veng/Reflection/TypeRegistry.h>

using namespace Veng;
using Json = nlohmann::json;

namespace
{
    bool HasField(const TypeInfo& info, const char* name)
    {
        return std::ranges::any_of(info.Fields,
                                   [name](const FieldDescriptor& f) { return f.Name == name; });
    }
}

TEST_CASE("GuiSurface reflects Driver beside its authored fields, and never its runtime")
{
    TypeRegistry registry;
    registry.Register<GuiSurface>();
    const TypeInfo& info = registry.Info(registry.IdOf<GuiSurface>());

    CHECK(HasField(info, "Document"));
    CHECK(HasField(info, "Resolution"));
    CHECK(HasField(info, "PixelScale"));
    CHECK(HasField(info, "Domain"));
    CHECK(HasField(info, "Driver"));
    CHECK(HasField(info, "Seat"));
    CHECK_FALSE(HasField(info, "Runtime"));

    // Undriven is the default, so a surface authored before the field existed loads unchanged.
    const GuiSurface fresh;
    CHECK(fresh.Driver == GuiDriverId::Null);
}

TEST_CASE("GuiSurface's Driver id round-trips through the authoring hex-id spelling")
{
    TypeRegistry registry;
    registry.Register<GuiSurface>();
    const TypeInfo& info = registry.Info(registry.IdOf<GuiSurface>());

    GuiSurface surface;
    const Json authored = {{"Driver", "0xE9906144475EB699"}};
    REQUIRE(JsonReadFields(&surface, info, authored, registry, {}));
    // The GuiDriverId leaf authors as a hex-id string, exactly like a minted id.
    CHECK(static_cast<u64>(surface.Driver) == 0xE9906144475EB699ULL);

    const Json out = JsonWriteFields(&surface, info, registry, {});
    CHECK(out["Driver"] == "0xE9906144475EB699");
}
