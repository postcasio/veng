#include <Veng/Cook/BuiltinImporters.h>

#include "Importers/AnimationImporter.h"
#include "Importers/AudioImporter.h"
#include "Importers/CollisionShapeImporter.h"
#include "Importers/EnvironmentImporter.h"
#include "Importers/FlipbookImporter.h"
#include "Importers/FontImporter.h"
#include "Importers/LocaleCatalogImporter.h"
#include "Importers/LocaleIndexImporter.h"
#include "Importers/MaterialImporter.h"
#include "Importers/MaterialInstanceImporter.h"
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
        cooker.Register(CreateUnique<MaterialInstanceImporter>());
        cooker.Register(CreateUnique<SkeletonImporter>());
        cooker.Register(CreateUnique<AnimationImporter>());
        cooker.Register(CreateUnique<CollisionShapeImporter>());
        cooker.Register(CreateUnique<AudioImporter>());
        cooker.Register(CreateUnique<EnvironmentImporter>());
        cooker.Register(CreateUnique<FlipbookImporter>());
        cooker.Register(CreateUnique<FontImporter>());
        cooker.Register(CreateUnique<LocaleCatalogImporter>());
        cooker.Register(CreateUnique<LocaleIndexImporter>());
    }
}
