#include <Veng/Cook/BuiltinImporters.h>

namespace Veng::Cook
{
    void RegisterBuiltinImporters(Cooker& cooker)
    {
        RegisterCoreImporters(cooker);
        RegisterPrefabImporter(cooker);
        RegisterLevelImporter(cooker);
    }
}
