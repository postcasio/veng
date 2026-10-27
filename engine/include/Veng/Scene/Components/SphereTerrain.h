#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Renderer/SphereTerrain.h>

namespace Veng
{
    class MaterialInstance;

    /// @brief A sphere's terrain, drawn from a shared Renderer::SphereTerrainSource.
    ///
    /// Each renderer gathers the scene's terrains every Execute, forms the eye in the sphere's
    /// body-fixed frame in double, selects the tiles its view needs, and marks and requests them on
    /// the source. A source may be handed to another entity or scene, and a renderer's selection
    /// memory is keyed by the source, so a sphere moved between scenes keeps its resident tiles and
    /// draws what it drew the frame before.
    ///
    /// The sphere stands where Center and Orientation say, in the frame a FloatingOrigin rests in
    /// (scene space for a scene without one); the entity's Transform is not read, and nothing
    /// re-bases the two, since they rest in the frame that does not move.
    struct SphereTerrain
    {
        /// @brief The Surface material the terrain is drawn with.
        AssetHandle<MaterialInstance> Material;
        /// @brief Whether the terrain is selected and drawn at all.
        bool Visible = true;
        /// @brief Whether the terrain casts into the shadow views.
        bool CastsShadows = true;
        /// @brief The pixels one grid quad's edge may span on screen before its tile splits.
        f32 TargetPixelsPerQuad = 8.0f;
        /// @brief A uniform scale about the eye applied to the drawn terrain; one draws at true
        ///        size. Selection measures unscaled distances whatever it is.
        f32 Scale = 1.0f;
        /// @brief The sphere's centre in the stable frame, in metres.
        ///
        /// Runtime-only: carries no VE_FIELD, so it is never reflected, cooked or serialized.
        dvec3 Center{0.0};
        /// @brief The rotation from the sphere's body-fixed frame into the stable frame.
        ///
        /// Runtime-only.
        glm::dquat Orientation{1.0, 0.0, 0.0, 0.0};
        /// @brief The tiles and their generator, or null for none.
        ///
        /// Runtime-only: a consumer creates it (Renderer::SphereTerrainSource::Create) and shares it
        /// between whatever entities and scenes draw the sphere.
        Ref<Renderer::SphereTerrainSource> Source;
    };
}

VE_REFLECT(::Veng::SphereTerrain, 0xB41B55C78CFC7E4FULL)
VE_FIELD(Material, .DisplayName = "Material", .Tooltip = "A Surface material for terrain tiles")
VE_FIELD(Visible, .DisplayName = "Visible")
VE_FIELD(CastsShadows, .DisplayName = "Casts shadows")
VE_FIELD(TargetPixelsPerQuad, .DisplayName = "Target pixels per quad",
         .Tooltip = "Pixels a grid quad may span before its tile splits", .Display = {.Min = 1.0})
VE_FIELD(Scale, .DisplayName = "Scale", .Tooltip = "Uniform scale about the eye",
         .Display = {.Min = 0.0})
VE_REFLECT_END();
