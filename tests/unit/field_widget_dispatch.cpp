// EffectiveWidget degrade-matrix cases: the pure, device-free decision of which widget a
// field draws given its resolved hint, class, type, and whether a range is present. No
// ImGui, no Context, no Vulkan symbol touched.

#include <doctest/doctest.h>

#include "FieldWidgetDispatch.h"

#include <Veng/Reflection/TypeId.h>

using namespace Veng;
using VengEditor::EffectiveWidget;

TEST_CASE("Auto always passes through unchanged")
{
    CHECK(EffectiveWidget(WidgetKind::Auto, FieldClass::Scalar, TypeIdOf<f32>(), true) ==
          WidgetKind::Auto);
    CHECK(EffectiveWidget(WidgetKind::Auto, FieldClass::String, TypeIdOf<string>(), false) ==
          WidgetKind::Auto);
}

TEST_CASE("A Slider without a range degrades to Drag")
{
    CHECK(EffectiveWidget(WidgetKind::Slider, FieldClass::Scalar, TypeIdOf<f32>(), false) ==
          WidgetKind::Drag);
    CHECK(EffectiveWidget(WidgetKind::Slider, FieldClass::Vector, TypeIdOf<vec3>(), false) ==
          WidgetKind::Drag);
}

TEST_CASE("A Slider with a range passes through")
{
    CHECK(EffectiveWidget(WidgetKind::Slider, FieldClass::Scalar, TypeIdOf<f32>(), true) ==
          WidgetKind::Slider);
    CHECK(EffectiveWidget(WidgetKind::Slider, FieldClass::Vector, TypeIdOf<vec2>(), true) ==
          WidgetKind::Slider);
}

TEST_CASE("A Color on a scalar degrades to Drag")
{
    CHECK(EffectiveWidget(WidgetKind::Color, FieldClass::Scalar, TypeIdOf<f32>(), false) ==
          WidgetKind::Drag);
}

TEST_CASE("A Color on a vec2 degrades to Drag")
{
    CHECK(EffectiveWidget(WidgetKind::Color, FieldClass::Vector, TypeIdOf<vec2>(), false) ==
          WidgetKind::Drag);
}

TEST_CASE("A Color on a vec3/vec4 passes through")
{
    CHECK(EffectiveWidget(WidgetKind::Color, FieldClass::Vector, TypeIdOf<vec3>(), false) ==
          WidgetKind::Color);
    CHECK(EffectiveWidget(WidgetKind::Color, FieldClass::Vector, TypeIdOf<vec4>(), false) ==
          WidgetKind::Color);
}

TEST_CASE("A Multiline on a non-string degrades to the class default")
{
    // Scalar/Vector are Drag-class, so an incompatible Multiline degrades to Drag; an
    // unrelated class (quaternion) ignores the hint and falls to Auto.
    CHECK(EffectiveWidget(WidgetKind::Multiline, FieldClass::Scalar, TypeIdOf<f32>(), false) ==
          WidgetKind::Drag);
    CHECK(EffectiveWidget(WidgetKind::Multiline, FieldClass::Vector, TypeIdOf<vec3>(), false) ==
          WidgetKind::Drag);
    CHECK(EffectiveWidget(WidgetKind::Multiline, FieldClass::Quaternion, TypeIdOf<quat>(), false) ==
          WidgetKind::Auto);
}

TEST_CASE("A Multiline on a string passes through")
{
    CHECK(EffectiveWidget(WidgetKind::Multiline, FieldClass::String, TypeIdOf<string>(), false) ==
          WidgetKind::Multiline);
}

TEST_CASE("A non-Auto hint on an unrelated class is ignored")
{
    // Slider/Color/Multiline mean nothing on a quaternion, enum, etc.; they fall to Auto.
    CHECK(EffectiveWidget(WidgetKind::Slider, FieldClass::Quaternion, TypeIdOf<quat>(), true) ==
          WidgetKind::Auto);
    CHECK(EffectiveWidget(WidgetKind::Color, FieldClass::String, TypeIdOf<string>(), false) ==
          WidgetKind::Auto);
}
