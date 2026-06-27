// ResolveFieldDisplay cascade cases: the pure, device-free merge of a field's
// presentation override over its type default, then the all-unset hard default.
// No Context, no Vulkan symbol touched.

#include <doctest/doctest.h>

#include <Veng/Reflection/FieldDescriptor.h>
#include <Veng/Reflection/FieldDisplay.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/TypeRegistry.h>

using namespace Veng;

// ---- A leaf carrying a non-trivial type default through VE_DISPLAY -----------

namespace
{
    // A styled scalar whose type default is a slider with a [0, …] range, a step,
    // and a collapsible-true — the type-default arm every field of it inherits.
    struct StyledScalar
    {
        f32 Value = 0.0f;
    };
}

VE_LEAF(::StyledScalar, 0x51A7ED5CA1A40001ULL, FieldClass::Scalar);
VE_DISPLAY(::StyledScalar, .Widget = WidgetKind::Slider, .Min = 0.0, .Step = 0.1,
           .Collapsible = true);

namespace
{
    // Builds a bare descriptor naming `type` with the given Display override.
    FieldDescriptor MakeField(TypeId type, FieldDisplay display)
    {
        FieldDescriptor field;
        field.Name = "F";
        field.Type = type;
        field.Display = std::move(display);
        return field;
    }
}

TEST_CASE("ResolveFieldDisplay inherits the type default with no override")
{
    TypeRegistry registry;
    registry.Register<StyledScalar>();

    const FieldDescriptor field = MakeField(TypeIdOf<StyledScalar>(), {});
    const FieldDisplay resolved = ResolveFieldDisplay(field, registry);

    CHECK(resolved.Widget == WidgetKind::Slider);
    CHECK(resolved.Min == doctest::Approx(0.0));
    CHECK(resolved.Step == doctest::Approx(0.1));
    CHECK_FALSE(resolved.Max.has_value());
    REQUIRE(resolved.Collapsible.has_value());
    CHECK(*resolved.Collapsible == true);
}

TEST_CASE("A field override of one member keeps the rest of the inherited default")
{
    TypeRegistry registry;
    registry.Register<StyledScalar>();

    // Overriding only Max leaves the inherited Widget/Min/Step/Collapsible intact.
    const FieldDescriptor field = MakeField(TypeIdOf<StyledScalar>(), {.Max = 5.0});
    const FieldDisplay resolved = ResolveFieldDisplay(field, registry);

    CHECK(resolved.Widget == WidgetKind::Slider);
    CHECK(resolved.Min == doctest::Approx(0.0));
    CHECK(resolved.Step == doctest::Approx(0.1));
    REQUIRE(resolved.Max.has_value());
    CHECK(*resolved.Max == doctest::Approx(5.0));
    REQUIRE(resolved.Collapsible.has_value());
    CHECK(*resolved.Collapsible == true);
}

TEST_CASE("A field override of the widget wins over the type default")
{
    TypeRegistry registry;
    registry.Register<StyledScalar>();

    const FieldDescriptor field = MakeField(TypeIdOf<StyledScalar>(), {.Widget = WidgetKind::Drag});
    const FieldDisplay resolved = ResolveFieldDisplay(field, registry);

    CHECK(resolved.Widget == WidgetKind::Drag);
    // The non-overridden members still inherit.
    CHECK(resolved.Min == doctest::Approx(0.0));
}

TEST_CASE("A field override of Collapsible=false overrides an inherited true")
{
    TypeRegistry registry;
    registry.Register<StyledScalar>();

    const FieldDescriptor field = MakeField(TypeIdOf<StyledScalar>(), {.Collapsible = false});
    const FieldDisplay resolved = ResolveFieldDisplay(field, registry);

    REQUIRE(resolved.Collapsible.has_value());
    CHECK(*resolved.Collapsible == false);
}

TEST_CASE("An unregistered field type falls to the hard default")
{
    const TypeRegistry registry;

    // The type is never registered; the resolver starts from an all-unset default and
    // overlays the override.
    const FieldDescriptor field =
        MakeField(TypeIdOf<StyledScalar>(), {.Widget = WidgetKind::Color, .Max = 2.0});
    const FieldDisplay resolved = ResolveFieldDisplay(field, registry);

    CHECK(resolved.Widget == WidgetKind::Color);
    REQUIRE(resolved.Max.has_value());
    CHECK(*resolved.Max == doctest::Approx(2.0));
    // Nothing inherited — the type default never participated.
    CHECK_FALSE(resolved.Min.has_value());
    CHECK_FALSE(resolved.Step.has_value());
    CHECK_FALSE(resolved.Collapsible.has_value());
}

TEST_CASE("An all-unset FieldDisplay resolves to Auto/empty")
{
    const TypeRegistry registry;

    // An unregistered type with no override: the all-unset hard default throughout.
    const FieldDescriptor field = MakeField(0xDEADBEEFULL, {});
    const FieldDisplay resolved = ResolveFieldDisplay(field, registry);

    CHECK(resolved.Widget == WidgetKind::Auto);
    CHECK_FALSE(resolved.Min.has_value());
    CHECK_FALSE(resolved.Max.has_value());
    CHECK_FALSE(resolved.Step.has_value());
    CHECK_FALSE(resolved.Collapsible.has_value());
    CHECK_FALSE(resolved.DefaultOpen.has_value());
}
