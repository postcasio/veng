#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/Material.h>

#include <span>

namespace VengEditor
{
    /// @brief Material parameter schema consumed by the catalog and compiler.
    ///
    /// Wraps the loaded cooked Material's reflected field table (exactly
    /// Material::GetFields()) plus its two shader ids. The panel passes the loaded
    /// material's table in rather than re-reflecting the shader, keeping libveng_cook
    /// out of the editor framework.
    struct MaterialShaderInterface
    {
        /// @brief Reflected field table from the cooked material (Name/Offset/Size/Kind per entry).
        std::span<const Veng::MaterialField> Fields;
        /// @brief Vertex shader asset id.
        Veng::AssetId VertexShader;
        /// @brief Fragment shader asset id.
        Veng::AssetId FragmentShader;
    };
}
