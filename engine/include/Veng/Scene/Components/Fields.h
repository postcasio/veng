#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Renderer/InstancedMesh.h>
#include <Veng/Renderer/PointField.h>
#include <Veng/Renderer/VolumeField.h>

#include <limits>

namespace Veng
{
    class Mesh;
    class MaterialInstance;

    /// @brief A scene-authored field of GPU-resident points the renderer draws.
    ///
    /// Resolved by the renderer via View<PointField> each Execute — the lights model: every
    /// component with a live Field contributes one field to the point-field pass, whose presence
    /// is driven by any live field existing (no consumer toggle). Lod and CellSize are authored
    /// knobs (reflected, cooked, editable in the inspector); Field is the built GPU resource,
    /// runtime-only and never serialized. A consumer builds a Renderer::PointField from its point
    /// set and assigns it; a null Field draws nothing. The field's points are world-space (the
    /// resource's contract) — the entity's Transform is not applied.
    struct PointField
    {
        /// @brief Screen-density LOD knobs applied to this field's draw.
        Renderer::PointFieldLod Lod;
        /// @brief Cull-cell edge length in world units a consumer builds the field with.
        f32 CellSize = 8.0f;
        /// @brief Where in the frame the field accumulates (see Renderer::PointFieldPlacement).
        ///
        /// HdrTail (the default) rides the post-TAA tail; SceneColor draws into the lit scene
        /// color ahead of the translucent pass, so translucents blend over the field and a
        /// refractive material's scene-color grab includes it.
        Renderer::PointFieldPlacement Placement = Renderer::PointFieldPlacement::HdrTail;
        /// @brief The built GPU field this entity draws, or null for none.
        ///
        /// Runtime-only: carries no VE_FIELD, so reflection, the cooker, and the inspector never
        /// see it — it serializes as absent and default-constructs to null on load. A consumer
        /// (a system or app code) builds a Renderer::PointField from its point set and assigns it.
        Ref<Renderer::PointField> Field;
    };

    /// @brief A scene-authored bounded emissive volumetric medium the renderer ray-marches.
    ///
    /// Resolved by the renderer via View<VolumeField> each Execute — the lights model: every
    /// component with a live Field contributes one field to the volume pass, whose presence is
    /// driven by any live field existing (no consumer toggle). Opacity, EmissionScale,
    /// ExtinctionScale, and Steps are authored knobs (reflected, cooked, editable in the inspector);
    /// Field is the built GPU resource, runtime-only and never serialized. A consumer builds a
    /// Renderer::VolumeField from its voxel data and assigns it; a null Field draws nothing. The
    /// field's bounds are world-space (the resource's contract) — the entity's Transform is not
    /// applied.
    struct VolumeField
    {
        /// @brief Overall fade: scales emission and extinction toward zero (1 full, 0 invisible).
        f32 Opacity = 1.0f;
        /// @brief Unit remap over the baked emission radiance density.
        f32 EmissionScale = 1.0f;
        /// @brief Unit remap over the baked extinction density.
        f32 ExtinctionScale = 1.0f;
        /// @brief Fixed ray-march step count through the field (the quality knob).
        u32 Steps = 64;
        /// @brief The built GPU field this entity draws, or null for none.
        ///
        /// Runtime-only: carries no VE_FIELD, so reflection, the cooker, and the inspector never
        /// see it — it serializes as absent and default-constructs to null on load. A consumer (a
        /// system or app code) builds a Renderer::VolumeField from its voxel data and assigns it.
        Ref<Renderer::VolumeField> Field;
    };

    /// @brief The InstancedMesh projected-size band's "no upper bound".
    inline constexpr f32 InstancedMeshUnboundedPixels = std::numeric_limits<f32>::max();

    /// @brief Draws one mesh with one material at every instance of a store, from one entity.
    ///
    /// The standard tool for scattered geometry — debris, vegetation, props — where an entity
    /// per copy would cost per-entity work every frame and a draw candidate per copy. The entity's
    /// Transform is the set's frame; each instance's transform (Renderer::MeshInstance) places a copy
    /// within it, and its datum is the material's to read. The renderer culls and selects the level of
    /// detail per instance (on the GPU where the device supports GPU-driven culling, whatever the
    /// renderer's entity cull mode), and the copies feed the g-buffer and every shadow view like any
    /// opaque mesh, as a handful of draws per level rather than a candidate per copy.
    ///
    /// The material must declare "instancedSets": true; a set drawn with one that does not is
    /// reported once and draws nothing. Every submesh of the mesh draws with this one material.
    struct InstancedMesh
    {
        /// @brief The mesh every instance draws.
        AssetHandle<Mesh> Mesh;
        /// @brief The material every instance draws with; it must declare "instancedSets".
        AssetHandle<MaterialInstance> Material;
        /// @brief Whether the instances cast shadows.
        bool CastsShadows = true;
        /// @brief Whether the set is drawn at all.
        bool Visible = true;
        /// @brief Instances projecting smaller than this many pixels in the main camera are culled.
        ///
        /// Measured by Renderer::ReferenceProjectedPixels in every view, so a shadow view casts
        /// exactly the instances the main view's band keeps, and a consumer drawing the instances
        /// below the band another way can partition them with the identical expression.
        f32 MinProjectedPixels = 0.0f;
        /// @brief Instances projecting larger than this many pixels in the main camera are culled.
        f32 MaxProjectedPixels = InstancedMeshUnboundedPixels;
        /// @brief A shadow view skips an instance spanning fewer of its texels than this.
        ///
        /// A caster smaller than a shadow texel flickers in and out of the map as it moves, and its
        /// shadow with it. 0 skips none.
        f32 MinShadowCasterTexels = 0.0f;
        /// @brief The instances drawn, or null for none.
        ///
        /// Runtime-only: carries no VE_FIELD, so it is never reflected, cooked or serialized. A
        /// consumer builds the store (Renderer::InstanceSet::Create) and writes it.
        Ref<Renderer::InstanceSet> Instances;
    };
}

VE_REFLECT(::Veng::Renderer::PointFieldLod, 0x9A3C1F6E4B2D08A7ULL)
VE_FIELD(AggregateThreshold, .DisplayName = "Aggregate Threshold", .Display = {.Min = 0.0})
VE_FIELD(Hysteresis, .DisplayName = "Hysteresis", .Display = {.Min = 0.0, .Max = 1.0})
VE_FIELD(AggregateSplatPixels, .DisplayName = "Aggregate Splat Pixels", .Display = {.Min = 0.0})
VE_FIELD(AggregateIntensity, .DisplayName = "Aggregate Intensity", .Display = {.Min = 0.0})
VE_FIELD(MinPixels, .DisplayName = "Min Pixels", .Display = {.Min = 0.0})
VE_FIELD(MaxPixels, .DisplayName = "Max Pixels", .Display = {.Min = 0.0})
VE_FIELD(MaxIntensity, .DisplayName = "Max Intensity", .Display = {.Min = 0.0})
VE_REFLECT_END();

VE_ENUM(::Veng::Renderer::PointFieldPlacement, 0xB0D259EFB3B7BF16ULL)
VE_ENUMERATOR(HdrTail)
VE_ENUMERATOR(SceneColor)
VE_ENUM_END();

VE_REFLECT(::Veng::PointField, 0x1D7F4A0C6E5B8392ULL)
VE_FIELD(Lod, .DisplayName = "LOD")
VE_FIELD(CellSize, .DisplayName = "Cell Size", .Display = {.Min = 0.001})
VE_FIELD(Placement, .DisplayName = "Placement",
         .Tooltip = "HDR tail (post-TAA, pre-bloom) or lit scene color (ahead of translucents)")
VE_REFLECT_END();

VE_REFLECT(::Veng::VolumeField, 0xA8C07107902C21E0ULL)
VE_FIELD(Opacity, .DisplayName = "Opacity", .Tooltip = "Fades emission and extinction toward zero",
         .Display = {.Min = 0.0, .Max = 1.0})
VE_FIELD(EmissionScale, .DisplayName = "Emission Scale", .Display = {.Min = 0.0})
VE_FIELD(ExtinctionScale, .DisplayName = "Extinction Scale", .Display = {.Min = 0.0})
VE_FIELD(Steps, .DisplayName = "Steps", .Tooltip = "Ray-march step count", .Display = {.Min = 1.0})
VE_REFLECT_END();

VE_REFLECT(::Veng::InstancedMesh, 0x5E86528760746AF8ULL)
VE_FIELD(Mesh, .DisplayName = "Mesh")
VE_FIELD(Material, .DisplayName = "Material", .Tooltip = "Must declare instancedSets")
VE_FIELD(CastsShadows, .DisplayName = "Casts shadows")
VE_FIELD(Visible, .DisplayName = "Visible")
VE_FIELD(MinProjectedPixels, .DisplayName = "Min Projected Pixels",
         .Tooltip = "Instances smaller than this in the main camera are culled",
         .Display = {.Min = 0.0})
VE_FIELD(MaxProjectedPixels, .DisplayName = "Max Projected Pixels",
         .Tooltip = "Instances larger than this in the main camera are culled",
         .Display = {.Min = 0.0})
VE_FIELD(MinShadowCasterTexels, .DisplayName = "Min Shadow Caster Texels",
         .Tooltip = "A shadow view skips instances spanning fewer of its texels",
         .Display = {.Min = 0.0})
VE_REFLECT_END();
