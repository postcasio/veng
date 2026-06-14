#include <Veng/Cook/BuiltinImporters.h>

#include "Importers/RawImporter.h"

namespace Veng::Cook
{
    void RegisterBuiltinImporters(Cooker& cooker)
    {
        cooker.Register(CreateUnique<RawImporter>());
    }
}
