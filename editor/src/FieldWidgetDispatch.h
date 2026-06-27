#pragma once

#include <Veng/Reflection/ReflectionTypes.h>

namespace VengEditor
{
    /// @brief Resolves the widget a field actually draws, degrading an incompatible hint.
    ///
    /// Pure and device-free — no ImGui, no GPU — so the whole degrade matrix is
    /// unit-testable. Given the cascade-resolved WidgetKind, the field's FieldClass, its
    /// concrete TypeId, and whether a numeric range is present, it returns the effective
    /// kind: a `Slider` without a range degrades to `Drag`; a `Color` on a non-vec3/vec4
    /// type degrades to `Drag`; a `Multiline` on a non-string type degrades to `Auto`
    /// (the class default). A compatible hint passes through unchanged, and every
    /// FieldClass other than Scalar/Vector/String ignores a non-`Auto` hint (returning
    /// `Auto`). The caller draws the returned kind and warns when it differs from the
    /// requested one.
    /// @param requested  The cascade-resolved WidgetKind the field asks for.
    /// @param cls         The field's data-shape class.
    /// @param type        The field's concrete TypeId (selects vec3 vs. vec4 for Color).
    /// @param hasRange    Whether the resolved presentation carries both a Min and a Max.
    /// @return The widget kind that actually draws.
    [[nodiscard]] Veng::WidgetKind EffectiveWidget(Veng::WidgetKind requested, Veng::FieldClass cls,
                                                   Veng::TypeId type, bool hasRange);
}
