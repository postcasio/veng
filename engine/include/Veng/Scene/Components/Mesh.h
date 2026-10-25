#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/MeshLod.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Reflection/Variant.h>
#include <Veng/Scene/RenderLayer.h>

namespace Veng
{
    class Mesh;
    class MaterialInstance;

    /// @brief Cube shape recipe: the parameters of Primitives::Cube plus its material.
    struct CubeShape
    {
        /// @brief Full width across each axis, in units.
        f32 Extent = 1.0f;
        /// @brief Material instance recorded on the generated submesh.
        AssetHandle<MaterialInstance> Material;
    };

    /// @brief Plane shape recipe: the parameters of Primitives::Plane plus its material.
    struct PlaneShape
    {
        /// @brief Plane dimensions in the XZ axes.
        vec2 Size = vec2(1.0f);
        /// @brief Quad count per axis.
        uvec2 Subdivisions = uvec2(1);
        /// @brief Material instance recorded on the generated submesh.
        AssetHandle<MaterialInstance> Material;
    };

    /// @brief UV-sphere shape recipe: the parameters of Primitives::Sphere plus its material.
    struct SphereShape
    {
        /// @brief Sphere radius.
        f32 Radius = 0.5f;
        /// @brief Latitude band count.
        u32 Rings = 16;
        /// @brief Longitude band count.
        u32 Segments = 32;
        /// @brief Material instance recorded on the generated submesh.
        AssetHandle<MaterialInstance> Material;
    };

    /// @brief Icosphere shape recipe: the parameters of Primitives::Icosphere plus its material.
    struct IcosphereShape
    {
        /// @brief Sphere radius.
        f32 Radius = 0.5f;
        /// @brief Icosahedron subdivision count.
        u32 Subdivisions = 3;
        /// @brief Material instance recorded on the generated submesh.
        AssetHandle<MaterialInstance> Material;
    };

    /// @brief Cylinder shape recipe: the parameters of Primitives::Cylinder plus its material.
    struct CylinderShape
    {
        /// @brief Cylinder radius.
        f32 Radius = 0.5f;
        /// @brief Full height along the Y axis.
        f32 Height = 1.0f;
        /// @brief Longitude band count around the side.
        u32 Segments = 32;
        /// @brief Material instance recorded on the generated submesh.
        AssetHandle<MaterialInstance> Material;
    };

    /// @brief Cone shape recipe: the parameters of Primitives::Cone plus its material.
    struct ConeShape
    {
        /// @brief Base radius.
        f32 Radius = 0.5f;
        /// @brief Full height from base to apex along the Y axis.
        f32 Height = 1.0f;
        /// @brief Longitude band count around the base.
        u32 Segments = 32;
        /// @brief Material instance recorded on the generated submesh.
        AssetHandle<MaterialInstance> Material;
    };

    /// @brief Torus shape recipe: the parameters of Primitives::Torus plus its material.
    struct TorusShape
    {
        /// @brief Distance from the center to the tube center.
        f32 MajorRadius = 0.5f;
        /// @brief Tube radius.
        f32 MinorRadius = 0.2f;
        /// @brief Band count around the ring.
        u32 MajorSegments = 32;
        /// @brief Band count around the tube.
        u32 MinorSegments = 16;
        /// @brief Material instance recorded on the generated submesh.
        AssetHandle<MaterialInstance> Material;
    };

    /// @brief Capsule shape recipe: the parameters of Primitives::Capsule plus its material.
    struct CapsuleShape
    {
        /// @brief Cap radius and cylinder radius.
        f32 Radius = 0.5f;
        /// @brief Height of the central cylinder along the Y axis (excludes the caps).
        f32 Height = 1.0f;
        /// @brief Longitude band count shared by the band and caps.
        u32 Segments = 32;
        /// @brief Latitude band count per hemisphere cap.
        u32 Rings = 8;
        /// @brief Material instance recorded on the generated submesh.
        AssetHandle<MaterialInstance> Material;
    };

    /// @brief Annulus shape recipe: the parameters of Primitives::Annulus plus its material.
    struct AnnulusShape
    {
        /// @brief Radius of the hole; 0 gives a filled disc.
        f32 InnerRadius = 0.25f;
        /// @brief Radius of the outer edge.
        f32 OuterRadius = 0.5f;
        /// @brief Quad band count across the ring.
        u32 RadialSegments = 1;
        /// @brief Quad column count around the ring.
        u32 AngularSegments = 32;
        /// @brief Equal angular sectors the ring splits into, one submesh each.
        u32 AngularSubmeshes = 1;
        /// @brief Material instance recorded on the generated submeshes.
        AssetHandle<MaterialInstance> Material;
    };

    /// @brief The inline procedural source of a MeshRenderer's mesh: one shape recipe or empty.
    ///
    /// The active alternative is the primitive kind and carries that kind's parameters plus
    /// its material. Empty means the MeshRenderer's cooked Mesh is used as authored; a
    /// non-empty alternative is built into the MeshRenderer's Mesh at spawn/edit.
    using MeshSource = Variant<CubeShape, PlaneShape, SphereShape, IcosphereShape, CylinderShape,
                               ConeShape, TorusShape, CapsuleShape, AnnulusShape>;

    /// @brief Component that binds a scene entity to a renderable mesh.
    ///
    /// The mesh owns its materials, so a renderer queries (Transform, MeshRenderer)
    /// and draws each submesh with its material. The mesh's source is either a cooked
    /// AssetId (Mesh, authored directly) or an inline procedural recipe (Source, the
    /// active shape variant). A non-empty Source is built into Mesh during the prefab
    /// populate pass, yielding a pending handle exactly like a cooked async load — the
    /// renderer query (Transform, MeshRenderer) and every draw path read the one Mesh
    /// handle regardless of which source produced it.
    struct MeshRenderer
    {
        /// @brief The resolved mesh this entity draws.
        ///
        /// Holds the cooked mesh when Source is empty, or the built recipe mesh when
        /// Source carries a shape. The renderer query and draw paths read this handle.
        AssetHandle<Mesh> Mesh;
        /// @brief Inline procedural source for Mesh, or empty to use the cooked Mesh as authored.
        ///
        /// The active alternative is the primitive kind, carrying that kind's parameters
        /// and material. When non-empty it is built into Mesh at spawn/edit through the
        /// ordinary async load path, replacing any authored cooked Mesh; an empty Source
        /// leaves the authored cooked Mesh in place.
        MeshSource Source;
        /// @brief Whether this mesh is rendered into the shadow maps (and bounds the shadow fit).
        ///
        /// True (the default) draws the mesh as a caster in every shadow view and folds its world
        /// bound into the caster bound the shadow projections fit to. False excludes it from both:
        /// the mesh still draws in the camera view but casts no shadow and never extends a shadow
        /// frustum. Set it false for geometry that must not occlude — an emissive body co-located
        /// with its own light, a skybox proxy, a held first-person prop.
        bool CastsShadows = true;
        /// @brief Added to each Translucent submesh's material sort priority when this entity draws.
        ///
        /// Translucent draws order by priority group, then back to front within one. A material's
        /// own `sortPriority` is a statement about every surface drawn with it; this is the same
        /// statement about one entity — glass a viewer always sits behind, a visor, anything known to
        /// be nearer than everything translucent around it however its centre sorts — so a shared
        /// material need not be split into a second parent to order one use of it. Ignored by every
        /// other domain.
        i32 SortPriority = 0;
        /// @brief The distance from the eye this entity's Translucent submeshes sort at, in place
        ///        of where they are drawn; zero (the default) sorts them where they are drawn.
        ///
        /// Within a priority group translucent draws order back to front by their submeshes'
        /// view-space depth. An impostor — a distant body drawn on a shell at a fixed distance and
        /// scaled to hold its apparent size — is drawn nowhere near where it stands, so its drawn
        /// depth says nothing about which of two such bodies is nearer; the caller that placed it
        /// knows, and states it here as the view depth its draws sort at (world units, positive).
        /// It orders the draws and nothing else: what they occlude and what occludes them is the
        /// depth test's. Ignored by every other domain.
        f32 SortDistance = 0.0f;
        /// @brief Whether this mesh is drawn at all.
        ///
        /// True (the default) draws it normally. False excludes it from the render gather
        /// entirely — no camera draw, no shadow, and no contribution to the scene bound — so it is
        /// the whole-mesh switch that CastsShadows is the shadow-only half of. It changes nothing
        /// else about the entity: the mesh stays resident, the transform chain still composes, and
        /// children (a socket-attached part, a camera riding the hull) are untouched, so hiding a
        /// parent does not hide what hangs off it.
        ///
        /// This is what a caller wanting to hide something should reach for. Removing the
        /// MeshRenderer instead loses the resolved mesh and any sibling component bound onto it,
        /// and collapsing the Transform scale drags every child down with it.
        bool Visible = true;
        /// @brief The render-visibility layer this mesh sits on; a view draws it only when its mask
        ///        names the layer (see RenderLayer).
        ///
        /// Default (RenderLayer::Default) is ordinary scene geometry, drawn by every view. Set it to
        /// RenderLayer::ViewAnchored for a mesh slaved to the viewing camera — a near-field particle
        /// shell, a billboard — which the ordinary camera view still draws but an environment probe
        /// skips, so the mesh never leaks into a reflection or a lens sampling that capture.
        RenderLayer Layer = RenderLayer::Default;
        /// @brief Per-entity material instances that replace the mesh asset's list when non-empty.
        ///
        /// Empty (the default) draws with the shared Mesh asset's GetMaterials(), so every entity
        /// drawing that mesh shares one instance per submesh. Non-empty, this vector **replaces**
        /// that list — same length, indexed the same way by SubMesh::MaterialIndex — so this entity
        /// draws with its own instances while other sharers keep the asset's. It is runtime-only
        /// presentation state: **not** a reflected field, so it never serializes, cooks, or
        /// replicates (a scene Clone copies it like any member). The engine populates it for a
        /// capturing entity — a CaptureSurface writes its probe into a private clone here so the
        /// output reaches only the entity it drives, never every sharer of the mesh — and a game
        /// may set it for a per-entity material override of its own.
        vector<AssetHandle<MaterialInstance>> InstanceMaterials;
    };

    /// @brief Pins the level of detail an entity's MeshRenderer draws at, in every view.
    ///
    /// Without it a mesh carrying levels of detail is drawn at the level each view selects from its
    /// projected screen size. With it every view — the camera, each shadow view, a capture — draws
    /// Level, which clamps to the mesh's coarsest (CoarsestLod selects that for any mesh). It wins
    /// over a renderer's forced level (SceneRendererSettings::ForcedLod, SceneView::ForcedLod). A
    /// mesh with a single level is unaffected.
    struct LodOverride
    {
        /// @brief The level drawn: 0 is the full-detail mesh, higher is coarser.
        i32 Level = 0;
    };
}

VE_REFLECT(::Veng::CubeShape, 0x2B758A3FE238BAA5ULL)
VE_FIELD(Extent, .DisplayName = "Extent", .Display = {.Min = 0.001})
VE_FIELD(Material, .DisplayName = "Material")
VE_REFLECT_END();

VE_REFLECT(::Veng::PlaneShape, 0x0E53DEFF5662A295ULL)
VE_FIELD(Size, .DisplayName = "Size", .Display = {.Min = 0.001})
VE_FIELD(Subdivisions, .DisplayName = "Subdivisions", .Display = {.Min = 1})
VE_FIELD(Material, .DisplayName = "Material")
VE_REFLECT_END();

VE_REFLECT(::Veng::SphereShape, 0xCF4BE61837AB5179ULL)
VE_FIELD(Radius, .DisplayName = "Radius", .Display = {.Min = 0.001})
VE_FIELD(Rings, .DisplayName = "Rings", .Display = {.Min = 3})
VE_FIELD(Segments, .DisplayName = "Segments", .Display = {.Min = 3})
VE_FIELD(Material, .DisplayName = "Material")
VE_REFLECT_END();

VE_REFLECT(::Veng::IcosphereShape, 0x7D9FC0055D5978BBULL)
VE_FIELD(Radius, .DisplayName = "Radius", .Display = {.Min = 0.001})
VE_FIELD(Subdivisions, .DisplayName = "Subdivisions", .Display = {.Min = 1})
VE_FIELD(Material, .DisplayName = "Material")
VE_REFLECT_END();

VE_REFLECT(::Veng::CylinderShape, 0x3BB833585C0DAD4CULL)
VE_FIELD(Radius, .DisplayName = "Radius", .Display = {.Min = 0.001})
VE_FIELD(Height, .DisplayName = "Height", .Display = {.Min = 0.001})
VE_FIELD(Segments, .DisplayName = "Segments", .Display = {.Min = 3})
VE_FIELD(Material, .DisplayName = "Material")
VE_REFLECT_END();

VE_REFLECT(::Veng::ConeShape, 0x64D0B47E06329960ULL)
VE_FIELD(Radius, .DisplayName = "Radius", .Display = {.Min = 0.001})
VE_FIELD(Height, .DisplayName = "Height", .Display = {.Min = 0.001})
VE_FIELD(Segments, .DisplayName = "Segments", .Display = {.Min = 3})
VE_FIELD(Material, .DisplayName = "Material")
VE_REFLECT_END();

VE_REFLECT(::Veng::TorusShape, 0xFF864410096026A2ULL)
VE_FIELD(MajorRadius, .DisplayName = "Major Radius", .Display = {.Min = 0.001})
VE_FIELD(MinorRadius, .DisplayName = "Minor Radius", .Display = {.Min = 0.001})
VE_FIELD(MajorSegments, .DisplayName = "Major Segments", .Display = {.Min = 3})
VE_FIELD(MinorSegments, .DisplayName = "Minor Segments", .Display = {.Min = 3})
VE_FIELD(Material, .DisplayName = "Material")
VE_REFLECT_END();

VE_REFLECT(::Veng::CapsuleShape, 0xDECC3D44F06725DCULL)
VE_FIELD(Radius, .DisplayName = "Radius", .Display = {.Min = 0.001})
VE_FIELD(Height, .DisplayName = "Height", .Display = {.Min = 0.001})
VE_FIELD(Segments, .DisplayName = "Segments", .Display = {.Min = 3})
VE_FIELD(Rings, .DisplayName = "Rings", .Display = {.Min = 1})
VE_FIELD(Material, .DisplayName = "Material")
VE_REFLECT_END();

VE_REFLECT(::Veng::AnnulusShape, 0xC5E6FA972870D259ULL)
VE_FIELD(InnerRadius, .DisplayName = "Inner Radius", .Display = {.Min = 0.0})
VE_FIELD(OuterRadius, .DisplayName = "Outer Radius", .Display = {.Min = 0.001})
VE_FIELD(RadialSegments, .DisplayName = "Radial Segments", .Display = {.Min = 1})
VE_FIELD(AngularSegments, .DisplayName = "Angular Segments", .Display = {.Min = 3})
VE_FIELD(AngularSubmeshes, .DisplayName = "Angular Submeshes", .Display = {.Min = 1})
VE_FIELD(Material, .DisplayName = "Material")
VE_REFLECT_END();

VE_VARIANT(::Veng::MeshSource, 0xC64CE2B415C54D22ULL);

VE_REFLECT(::Veng::MeshRenderer, 0x3C5CB13E46E0450BULL)
VE_FIELD(Mesh, .DisplayName = "Mesh")
VE_FIELD(Source, .DisplayName = "Source")
VE_FIELD(CastsShadows, .DisplayName = "Casts shadows")
VE_FIELD(SortPriority, .DisplayName = "Sort priority")
VE_FIELD(
    SortDistance, .DisplayName = "Sort distance",
    .Tooltip = "The view depth translucent submeshes sort at; 0 sorts them where they are drawn",
    .Display = {.Min = 0.0})
VE_FIELD(Visible, .DisplayName = "Visible")
VE_FIELD(Layer, .DisplayName = "Render layer")
VE_REFLECT_END();

VE_REFLECT(::Veng::LodOverride, 0xF4018529AB2F8DF7ULL)
VE_FIELD(Level, .DisplayName = "Level", .Tooltip = "Level of detail drawn; 0 is full detail",
         .Display = {.Min = 0.0, .Max = 7.0})
VE_REFLECT_END();
