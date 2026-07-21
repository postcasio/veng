// IsFieldVisible / IsFieldEnabled / IsFieldEditable decision cases: the pure, device-free
// gate the inspector applies per field before drawing it. No ImGui, no Vulkan symbol
// touched — the conditional-display decision is exercised through authored predicates,
// including a nested-struct case asserting the predicate evaluates against the *nested*
// instance's base, not the outer one, and the composition of the consumer-supplied
// InspectorHooks::FieldEnabled gate with a field's own EnabledIf and ReadOnly.

#include <doctest/doctest.h>

#include <Veng/Reflection/FieldDescriptor.h>
#include <Veng/Reflection/FieldGate.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/UI/Inspector.h>

using namespace Veng;

namespace
{
    enum class Mode : u32
    {
        Off = 0,
        On = 1,
    };

    // An owning struct whose fields' VisibleIf/EnabledIf depend on its own siblings, authored
    // with VE_WHEN — the real authoring path the dogfood Light gate uses.
    struct Gated
    {
        Mode Kind = Mode::Off;
        bool Locked = false;
        f32 Conditional = 0.0f;
    };

    // A nested struct whose field carries a predicate over *its* sibling, embedded in an outer
    // struct that flips the same-named control the other way — so a predicate evaluated against
    // the wrong base would give the wrong answer.
    struct Inner
    {
        bool Show = false;
        f32 Value = 0.0f;
    };

    struct Outer
    {
        bool Show = true;
        Inner Child;
    };
}

VE_LEAF(::Mode, 0x51A7ED5CA1A40010ULL, FieldClass::Enum);

VE_REFLECT(::Gated, 0x51A7ED5CA1A40011ULL)
VE_FIELD(Kind)
VE_FIELD(Locked)
VE_FIELD(Conditional, .VisibleIf = VE_WHEN(self.Kind == ::Mode::On),
         .EnabledIf = VE_WHEN(!self.Locked))
VE_REFLECT_END();

VE_REFLECT(::Inner, 0x51A7ED5CA1A40012ULL)
VE_FIELD(Show)
VE_FIELD(Value, .VisibleIf = VE_WHEN(self.Show))
VE_REFLECT_END();

VE_REFLECT(::Outer, 0x51A7ED5CA1A40013ULL)
VE_FIELD(Show)
VE_FIELD(Child)
VE_REFLECT_END();

namespace
{
    // Looks up a field by name in a registered type's descriptor list.
    const FieldDescriptor& FieldNamed(const TypeInfo& info, string_view name)
    {
        for (const FieldDescriptor& field : info.Fields)
        {
            if (field.Name == name)
            {
                return field;
            }
        }
        REQUIRE_MESSAGE(false, "field not found");
        return info.Fields.front();
    }
}

TEST_CASE("An empty predicate is unconditionally visible and enabled")
{
    const FieldDescriptor bare;
    Gated value;
    CHECK(IsFieldVisible(bare, &value));
    CHECK(IsFieldEnabled(bare, &value));
}

TEST_CASE("VisibleIf flips with the owning value")
{
    TypeRegistry registry;
    registry.Register<Gated>();
    const TypeInfo& info = registry.Info(TypeIdOf<Gated>());
    const FieldDescriptor& conditional = FieldNamed(info, "Conditional");

    Gated value;

    value.Kind = Mode::Off;
    CHECK_FALSE(IsFieldVisible(conditional, &value));

    value.Kind = Mode::On;
    CHECK(IsFieldVisible(conditional, &value));
}

TEST_CASE("EnabledIf flips with the owning value, independent of VisibleIf")
{
    TypeRegistry registry;
    registry.Register<Gated>();
    const TypeInfo& info = registry.Info(TypeIdOf<Gated>());
    const FieldDescriptor& conditional = FieldNamed(info, "Conditional");

    Gated value;
    value.Kind = Mode::On;

    value.Locked = false;
    CHECK(IsFieldEnabled(conditional, &value));

    value.Locked = true;
    CHECK_FALSE(IsFieldEnabled(conditional, &value));
    // Still visible — the two conditions are independent.
    CHECK(IsFieldVisible(conditional, &value));
}

TEST_CASE("A nested field's predicate evaluates against the nested base, not the outer one")
{
    TypeRegistry registry;
    registry.Register<Outer>();
    const TypeInfo& innerInfo = registry.Info(TypeIdOf<Inner>());
    const FieldDescriptor& value = FieldNamed(innerInfo, "Value");

    Outer outer;
    // Drive the outer and inner controls in *opposite* directions: a predicate handed the
    // outer base would read Outer::Show (true) where it must read Inner::Show (false).
    outer.Show = true;
    outer.Child.Show = false;

    // Against the nested base, Inner::Show is false → hidden.
    CHECK_FALSE(IsFieldVisible(value, &outer.Child));

    // Flip only the nested control; the outer one is unchanged.
    outer.Child.Show = true;
    CHECK(IsFieldVisible(value, &outer.Child));
}

TEST_CASE("The hooks' FieldEnabled gate disables a field the owning struct admits")
{
    TypeRegistry registry;
    registry.Register<Gated>();
    const TypeInfo& info = registry.Info(TypeIdOf<Gated>());
    const FieldDescriptor& conditional = FieldNamed(info, "Conditional");

    Gated value;
    value.Kind = Mode::On;
    value.Locked = false;

    UI::InspectorHooks hooks;
    hooks.Registry = &registry;
    hooks.OwnerBase = &value;

    // Unset admits every field, so the walk is exactly as it is without the hook.
    CHECK(UI::IsFieldEditable(conditional, hooks));

    // The gate answers per field: the named one goes inert, its siblings stay live.
    hooks.FieldEnabled = [](const FieldDescriptor& field) { return field.Name != "Conditional"; };
    CHECK_FALSE(UI::IsFieldEditable(conditional, hooks));
    CHECK(UI::IsFieldEditable(FieldNamed(info, "Locked"), hooks));

    // It gates editing only — the row is still drawn.
    CHECK(IsFieldVisible(conditional, &value));
}

TEST_CASE("The hooks' FieldEnabled gate composes with EnabledIf rather than overriding it")
{
    TypeRegistry registry;
    registry.Register<Gated>();
    const TypeInfo& info = registry.Info(TypeIdOf<Gated>());
    const FieldDescriptor& conditional = FieldNamed(info, "Conditional");

    Gated value;
    value.Kind = Mode::On;

    UI::InspectorHooks hooks;
    hooks.Registry = &registry;
    hooks.OwnerBase = &value;
    hooks.FieldEnabled = [](const FieldDescriptor&) { return true; };

    // An admitting hook cannot re-enable what the field's own EnabledIf refused.
    value.Locked = true;
    CHECK_FALSE(UI::IsFieldEditable(conditional, hooks));

    value.Locked = false;
    CHECK(UI::IsFieldEditable(conditional, hooks));

    // Either gate refusing is enough to disable.
    hooks.FieldEnabled = [](const FieldDescriptor&) { return false; };
    CHECK_FALSE(UI::IsFieldEditable(conditional, hooks));
}

TEST_CASE("The hooks' FieldEnabled gate composes with ReadOnly rather than overriding it")
{
    FieldDescriptor readOnly;
    readOnly.Name = "Frozen";
    readOnly.ReadOnly = true;

    Gated value;

    UI::InspectorHooks hooks;
    hooks.OwnerBase = &value;

    CHECK_FALSE(UI::IsFieldEditable(readOnly, hooks));

    // An admitting hook cannot re-enable a ReadOnly field.
    hooks.FieldEnabled = [](const FieldDescriptor&) { return true; };
    CHECK_FALSE(UI::IsFieldEditable(readOnly, hooks));

    // Clearing the flag leaves the hook the only gate, and it admits.
    readOnly.ReadOnly = false;
    CHECK(UI::IsFieldEditable(readOnly, hooks));
}
