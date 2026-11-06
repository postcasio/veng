#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Renderer/SphereTerrain.h>
#include <Veng/Scene/RenderLayer.h>

namespace Veng
{
    class MaterialInstance;

    /// @brief A surface a sphere terrain draws over its own at a constant height: a water level, a
    ///        lava level, a flat datum.
    ///
    /// The layer is drawn over the same tile selection as the terrain, at the terrain's placement,
    /// morph and scale, with its height in place of the height channel, so it stands exactly where
    /// the terrain would at that height at every distance. A tile draws it only where the tile's
    /// height range dips below it, so a layer costs nothing over higher ground.
    ///
    /// A Surface material draws it opaque, into the g-buffer and the shadow views; a Translucent
    /// one draws it in the translucent pass over the lit scene, casting nothing, so what lies
    /// beneath it can show through (`Veng/sphere_terrain_translucent.slang`).
    struct SphereTerrainLevelLayer
    {
        /// @brief The Surface or Translucent material the layer is drawn with; it must declare
        ///        "sphereTerrain".
        AssetHandle<MaterialInstance> Material;
        /// @brief The layer's height above the sphere's radius, in metres.
        f32 Height = 0.0f;
    };

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
    ///
    /// Its tiles draw into the g-buffer and, at a scale of one, into the shadow views, through a
    /// Surface material declaring "sphereTerrain" (`Veng/sphere_terrain.slang`), and a level layer
    /// may draw in the translucent pass instead (SphereTerrainLevelLayer); a terrain drawn with
    /// another material is reported once and draws nothing. A view whose layer mask omits the
    /// terrain's Layer neither draws it nor selects, marks or requests its tiles. It is not pickable, not drawn by
    /// the depth-and-normal path or under the g-buffer shading override, and not occlusion-tested.
    struct SphereTerrain
    {
        /// @brief The Surface material the terrain is drawn with; it must declare "sphereTerrain".
        AssetHandle<MaterialInstance> Material;
        /// @brief Whether the terrain is selected and drawn at all.
        bool Visible = true;
        /// @brief The render-visibility layer the terrain sits on; a view selects and draws it only
        ///        when its mask names the layer (see RenderLayer).
        ///
        /// A renderer selects a terrain's tiles for its own view and marks and requests them on the
        /// shared source, so a view that leaves the layer out costs the source nothing: a scene
        /// capture whose mask omits it — an image-based-lighting probe drawing only
        /// RenderLayer::Environment — neither draws the ground nor competes for its tiles.
        RenderLayer Layer = RenderLayer::Default;
        /// @brief Whether the terrain casts into the shadow views.
        bool CastsShadows = true;
        /// @brief The pixels one grid quad's edge may span on screen before its tile splits.
        f32 TargetPixelsPerQuad = 8.0f;
        /// @brief A uniform scale about the eye applied to the drawn terrain; one draws at true
        ///        size.
        ///
        /// A scale about the eye moves nothing on screen and changes only depth, so a consumer can
        /// draw a distant sphere inside a bounded depth range at its true apparent size. Selection
        /// and culling measure unscaled distances, so the same tiles draw at any scale; a terrain
        /// at a scale other than one casts no shadow, since its drawn geometry does not stand
        /// where the scene's casters and receivers are; and the deferred pass reconstructs a
        /// scaled fragment at its scaled position unless the material writes a depth of its own
        /// (`TerrainTrueDeviceDepth`).
        f32 Scale = 1.0f;
        /// @brief The level layers drawn over the terrain; past MaxSphereTerrainLevelLayers the
        ///        rest are ignored.
        vector<SphereTerrainLevelLayer> LevelLayers;
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

VE_REFLECT(::Veng::SphereTerrainLevelLayer, 0xDF994550C064BA0AULL)
VE_FIELD(Material, .DisplayName = "Material",
         .Tooltip = "A Surface or Translucent material declaring sphereTerrain")
VE_FIELD(Height, .DisplayName = "Height", .Tooltip = "Metres above the sphere's radius")
VE_REFLECT_END();

VE_REFLECT(::Veng::SphereTerrain, 0xB41B55C78CFC7E4FULL)
VE_FIELD(Material, .DisplayName = "Material",
         .Tooltip = "A Surface material declaring sphereTerrain")
VE_FIELD(Visible, .DisplayName = "Visible")
VE_FIELD(Layer, .DisplayName = "Render layer")
VE_FIELD(CastsShadows, .DisplayName = "Casts shadows")
VE_FIELD(TargetPixelsPerQuad, .DisplayName = "Target pixels per quad",
         .Tooltip = "Pixels a grid quad may span before its tile splits", .Display = {.Min = 1.0})
VE_FIELD(Scale, .DisplayName = "Scale", .Tooltip = "Uniform scale about the eye",
         .Display = {.Min = 0.0})
VE_ARRAY_FIELD(LevelLayers, .DisplayName = "Level layers",
               .Tooltip = "Surfaces drawn at a constant height over the terrain")
VE_REFLECT_END();
