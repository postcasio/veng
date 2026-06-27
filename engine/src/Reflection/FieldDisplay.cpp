#include <Veng/Reflection/FieldDisplay.h>

#include <Veng/Reflection/FieldDescriptor.h>
#include <Veng/Reflection/TypeRegistry.h>

namespace Veng
{
    FieldDisplay ResolveFieldDisplay(const FieldDescriptor& field, const TypeRegistry& registry)
    {
        FieldDisplay resolved =
            registry.IsRegistered(field.Type) ? registry.Info(field.Type).Display : FieldDisplay{};

        const FieldDisplay& override_ = field.Display;
        if (override_.Widget != WidgetKind::Auto)
        {
            resolved.Widget = override_.Widget;
        }
        if (override_.Min)
        {
            resolved.Min = override_.Min;
        }
        if (override_.Max)
        {
            resolved.Max = override_.Max;
        }
        if (override_.Step)
        {
            resolved.Step = override_.Step;
        }
        if (override_.Collapsible)
        {
            resolved.Collapsible = override_.Collapsible;
        }
        if (override_.DefaultOpen)
        {
            resolved.DefaultOpen = override_.DefaultOpen;
        }
        return resolved;
    }
}
