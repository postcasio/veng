#include "FieldWidgetDispatch.h"

#include <Veng/Reflection/TypeId.h>

namespace VengEditor
{
    using namespace Veng;

    WidgetKind EffectiveWidget(WidgetKind requested, FieldClass cls, TypeId type, bool hasRange)
    {
        // Auto inherits the FieldClass default; nothing to validate.
        if (requested == WidgetKind::Auto)
        {
            return WidgetKind::Auto;
        }

        switch (cls)
        {
        case FieldClass::Scalar:
        case FieldClass::Vector:
        {
            // A scalar or vector accepts Drag always, Slider only with a range, and Color
            // only on a vec3/vec4. Multiline is meaningless here.
            if (requested == WidgetKind::Drag)
            {
                return WidgetKind::Drag;
            }
            if (requested == WidgetKind::Slider)
            {
                return hasRange ? WidgetKind::Slider : WidgetKind::Drag;
            }
            if (requested == WidgetKind::Color)
            {
                const bool colorable = type == TypeIdOf<vec3>() || type == TypeIdOf<vec4>();
                return colorable ? WidgetKind::Color : WidgetKind::Drag;
            }
            return WidgetKind::Drag;
        }
        case FieldClass::String:
        {
            // A string accepts only Multiline as a non-Auto hint; anything else falls back
            // to the class default (the single-line InputText).
            return requested == WidgetKind::Multiline ? WidgetKind::Multiline : WidgetKind::Auto;
        }
        default:
        {
            // Every other class draws its natural widget regardless of the hint.
            return WidgetKind::Auto;
        }
        }
    }
}
