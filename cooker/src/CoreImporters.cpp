#include <Veng/Cook/BuiltinImporters.h>

#include "Importers/AnimationImporter.h"
#include "Importers/MaterialImporter.h"
#include "Importers/MeshImporter.h"
#include "Importers/RawImporter.h"
#include "Importers/ShaderImporter.h"
#include "Importers/SkeletonImporter.h"
#include "Importers/TextureImporter.h"
#include "Importers/VertexLayoutImporter.h"

namespace Veng::Cook
{
    void RegisterCoreImporters(Cooker& cooker)
    {
        cooker.Register(CreateUnique<RawImporter>());
        cooker.Register(CreateUnique<TextureImporter>());
        cooker.Register(CreateUnique<MeshImporter>());
        cooker.Register(CreateUnique<ShaderImporter>());
        cooker.Register(CreateUnique<VertexLayoutImporter>());
        cooker.Register(CreateUnique<MaterialImporter>());
        cooker.Register(CreateUnique<SkeletonImporter>());
        cooker.Register(CreateUnique<AnimationImporter>());
    }
}
