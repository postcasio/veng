#include <Veng/Cook/BuiltinImporters.h>

#include "Importers/MaterialImporter.h"
#include "Importers/MeshImporter.h"
#include "Importers/RawImporter.h"
#include "Importers/ShaderImporter.h"
#include "Importers/TextureImporter.h"
#include "Importers/VertexLayoutImporter.h"

namespace Veng::Cook
{
    void RegisterBuiltinImporters(Cooker& cooker)
    {
        cooker.Register(CreateUnique<RawImporter>());
        cooker.Register(CreateUnique<TextureImporter>());
        cooker.Register(CreateUnique<MeshImporter>());
        cooker.Register(CreateUnique<ShaderImporter>());
        cooker.Register(CreateUnique<VertexLayoutImporter>());
        cooker.Register(CreateUnique<MaterialImporter>());
    }
}
