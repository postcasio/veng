#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/Material.h>

#include <span>

namespace VengEditor
{
    // The material's parameter schema the catalog and compiler read — the loaded
    // cooked Material's reflected field table plus its two shader ids. Fields is
    // exactly Material::GetFields(): the set MaterialImporter validated and cooked,
    // each field carrying Name/Offset/Size/Kind. Reading it keeps libveng_cook (the
    // Slang reflector) out of the editor framework — the panel passes the loaded
    // material's table in rather than re-reflecting the shader.
    struct MaterialShaderInterface
    {
        std::span<const Veng::MaterialField> Fields;
        Veng::AssetId VertexShader;
        Veng::AssetId FragmentShader;
    };
}
