#pragma once

#include <Veng/Veng.h>
#include <Veng/Reflection/ReflectionTypes.h>

namespace Veng
{
    class TypeRegistry;
    struct FieldDescriptor;

    /// @brief The cascade-able presentation options of a reflected field.
    ///
    /// Lives on both TypeInfo (a type's default) and FieldDescriptor (a field's
    /// override); ResolveFieldDisplay merges them field-first, then type-default,
    /// then this struct's all-unset hard default. Every member is representable as
    /// *unset* — Widget via the Auto sentinel, the rest via optional — so the merge
    /// overlays a single set option without clobbering the others. It is editor
    /// metadata the reflection serializer never touches.
    struct FieldDisplay
    {
        /// @brief The presentation kind; Auto means "infer from FieldClass" (the unset/inherit state).
        WidgetKind Widget = WidgetKind::Auto;
        /// @brief Optional minimum value hint for a numeric widget.
        optional<f64> Min;
        /// @brief Optional maximum value hint for a numeric widget.
        optional<f64> Max;
        /// @brief Optional step size hint for a drag/slider widget.
        optional<f64> Step;
        /// @brief When set, whether a nested struct or category renders as a collapsible section.
        optional<bool> Collapsible;
        /// @brief When set, whether a collapsible section starts expanded.
        optional<bool> DefaultOpen;
    };

    /// @brief Resolves a field's presentation by merging its override over its type default.
    ///
    /// Starts from the field type's TypeInfo::Display (the all-unset hard default when
    /// the type is unregistered), then overlays each *set* member of field.Display: a
    /// non-Auto Widget and each present optional win over the inherited value. Pure and
    /// device-free — no ImGui, no GPU — so both inspector surfaces resolve identically.
    /// @param field     The field whose presentation is resolved.
    /// @param registry  The registry holding the field type's TypeInfo default.
    /// @return The fully-resolved FieldDisplay; the consumer reads optionals via value_or.
    FieldDisplay ResolveFieldDisplay(const FieldDescriptor& field, const TypeRegistry& registry);

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
    /// @param cls        The field's data-shape class.
    /// @param type       The field's concrete TypeId (selects vec3 vs. vec4 for Color).
    /// @param hasRange   Whether the resolved presentation carries both a Min and a Max.
    /// @return The widget kind that actually draws.
    [[nodiscard]] WidgetKind EffectiveWidget(WidgetKind requested, FieldClass cls, TypeId type,
                                             bool hasRange);

    /// @brief Primary template authoring a type's default FieldDisplay; all-unset unless specialised.
    ///
    /// TypeRegistry::Register<T>() reads VengDisplay<T>::Get() into TypeInfo::Display.
    /// Specialise it per type with VE_DISPLAY — a separate specialisation point from
    /// VengReflect<T>, so it composes with every reflection macro without touching them.
    /// @tparam T  The type whose presentation default is authored.
    template <class T>
    struct VengDisplay
    {
        /// @brief Returns the type's default presentation; all-unset for the primary template.
        static FieldDisplay Get() { return {}; }
    };
}

/// @brief Authors a type's default FieldDisplay by specialising VengDisplay\<T\>.
///
/// The trailing … are the designated initialisers of a FieldDisplay aggregate
/// (`VE_DISPLAY(::Veng::Foo, .Widget = WidgetKind::Color, .Min = 0.0)`). A separate
/// specialisation point from VengReflect<T>, so it composes uniformly with VE_LEAF /
/// VE_TYPE / VE_REFLECT / VE_ENUM. The type is named fully qualified from global scope.
#define VE_DISPLAY(Type, ...)                                                                      \
    template <>                                                                                    \
    struct ::Veng::VengDisplay<Type>                                                               \
    {                                                                                              \
        static ::Veng::FieldDisplay Get() { return ::Veng::FieldDisplay{__VA_ARGS__}; }            \
    }
