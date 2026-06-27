#include "FieldGate.h"

namespace VengEditor
{
    using namespace Veng;

    bool IsFieldVisible(const FieldDescriptor& field, const void* ownerBase)
    {
        return !field.VisibleIf || field.VisibleIf(ownerBase);
    }

    bool IsFieldEnabled(const FieldDescriptor& field, const void* ownerBase)
    {
        return !field.EnabledIf || field.EnabledIf(ownerBase);
    }
}
