#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Reflection/Variant.h>

#include <Veng/Asset/AssetHandle.h>

namespace Veng
{
    class Mesh;
    class Material;

    /// @brief Human-readable label for an entity.
    ///
    /// Display and logging only; never an identity key.
    struct Name
    {
        /// @brief The display label.
        string Value;
    };

    /// @brief Local TRS — relative to the entity's Hierarchy parent, or to world for a root.
    ///
    /// World matrices are derived by the Hierarchy-chain walk in Transforms.h;
    /// this struct never stores a world matrix.
    struct Transform
    {
        /// @brief Local position in parent space.
        vec3 Position{0.0f};
        /// @brief Local rotation in parent space.
        quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
        /// @brief Local scale in parent space.
        vec3 Scale{1.0f};
    };

    /// @brief Intrusive scene-graph link for one entity: up-edge plus a doubly-linked sibling list.
    ///
    /// Parent is the up-edge (Entity::Null = root); FirstChild heads an ordered,
    /// doubly-linked sibling list (Entity::Null = leaf); PrevSibling / NextSibling
    /// thread that list. All four are maintained together by the Scene's
    /// SetParent / Detach / MoveBefore operations and are never written directly,
    /// so the structure stays consistent. Only Parent is persisted (the reflected
    /// edge); the three list links are derived and rebuilt on spawn, so they carry
    /// no reflected field.
    struct Hierarchy
    {
        /// @brief The parent entity, or Entity::Null for a root.
        Entity Parent = Entity::Null;
        /// @brief First child in the sibling list, or Entity::Null for a leaf.
        Entity FirstChild = Entity::Null;
        /// @brief Previous sibling in the parent's child list, or Entity::Null at the head.
        Entity PrevSibling = Entity::Null;
        /// @brief Next sibling in the parent's child list, or Entity::Null at the tail.
        Entity NextSibling = Entity::Null;
    };

    /// @brief Component that binds a scene entity to a renderable mesh.
    ///
    /// The mesh owns its materials, so a renderer queries (Transform, MeshRenderer)
    /// and draws each submesh with its material.
    struct MeshRenderer
    {
        /// @brief The mesh this entity draws.
        AssetHandle<Mesh> Mesh;
    };

    /// @brief Cube shape recipe: the parameters of Primitives::Cube plus its material.
    struct CubeShape
    {
        /// @brief Full width across each axis, in units.
        f32 Extent = 1.0f;
        /// @brief Material recorded on the generated submesh.
        AssetHandle<Material> Material;
    };

    /// @brief Plane shape recipe: the parameters of Primitives::Plane plus its material.
    struct PlaneShape
    {
        /// @brief Plane dimensions in the XZ axes.
        vec2 Size = vec2(1.0f);
        /// @brief Quad count per axis.
        uvec2 Subdivisions = uvec2(1);
        /// @brief Material recorded on the generated submesh.
        AssetHandle<Material> Material;
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
        /// @brief Material recorded on the generated submesh.
        AssetHandle<Material> Material;
    };

    /// @brief Icosphere shape recipe: the parameters of Primitives::Icosphere plus its material.
    struct IcosphereShape
    {
        /// @brief Sphere radius.
        f32 Radius = 0.5f;
        /// @brief Icosahedron subdivision count.
        u32 Subdivisions = 3;
        /// @brief Material recorded on the generated submesh.
        AssetHandle<Material> Material;
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
        /// @brief Material recorded on the generated submesh.
        AssetHandle<Material> Material;
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
        /// @brief Material recorded on the generated submesh.
        AssetHandle<Material> Material;
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
        /// @brief Material recorded on the generated submesh.
        AssetHandle<Material> Material;
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
        /// @brief Material recorded on the generated submesh.
        AssetHandle<Material> Material;
    };

    /// @brief The tagged union of shape recipes a Primitive can hold.
    using PrimitiveShapeVariant = Variant<CubeShape, PlaneShape, SphereShape, IcosphereShape,
                                          CylinderShape, ConeShape, TorusShape, CapsuleShape>;

    /// @brief A procedural-mesh recipe: regenerated into the entity's MeshRenderer at spawn.
    ///
    /// The active alternative of Shape is the primitive kind and carries that kind's
    /// parameters plus its material. A registered resolver turns the active shape into a
    /// streamed Mesh at spawn/edit and stores the handle in the entity's MeshRenderer; an
    /// empty Shape produces no mesh.
    struct Primitive
    {
        /// @brief The active shape recipe, or empty for no mesh.
        PrimitiveShapeVariant Shape;
    };

    /// @brief Resolver for Primitive: builds the active shape's mesh and points the
    ///        entity's MeshRenderer at it.
    ///
    /// Builds the active shape through BuildPrimitiveMesh, adding a MeshRenderer when the
    /// entity has none. An empty Shape leaves the renderer untouched. Wired onto the type by
    /// VE_RESOLVE; fired by Prefab::SpawnInto and ResolveComponents.
    /// @param primitive  The component carrying the shape recipe.
    /// @param scene      The scene holding the entity.
    /// @param entity     The entity whose MeshRenderer receives the mesh.
    /// @param manager    The asset manager the generated mesh streams through.
    void ResolvePrimitive(Primitive& primitive, Scene& scene, Entity entity, AssetManager& manager);

    /// @brief Selects how the deferred lighting pass attenuates a light.
    ///
    /// Directional has no position or falloff. Point and Spot are placed by the
    /// entity's Transform and fall off with distance (Spot adds a cone). Integer
    /// values are stable — packed into the light SSBO and persisted in prefabs.
    enum class LightType : u32
    {
        /// @brief Infinite directional light; no position or falloff.
        Directional = 0,
        /// @brief Omnidirectional point light; falls off within Range.
        Point = 1,
        /// @brief Cone spot light; falls off within Range and the cone angles.
        Spot = 2,
    };

    /// @brief Light component shaded by the deferred lighting pass.
    ///
    /// Type selects directional, point, or spot. Direction is the world-space
    /// travel direction (directional and spot); Color is linear RGB; Intensity
    /// scales it. Range is the point/spot falloff radius. InnerCone/OuterCone
    /// are the spot's half-angles in radians: full intensity within InnerCone,
    /// zero beyond OuterCone, smooth between.
    ///
    /// The light's world position comes from the entity's Transform — never stored
    /// here — so a parented or animated light moves with its entity.
    struct Light
    {
        /// @brief Light shape.
        LightType Type{LightType::Directional};
        /// @brief World-space travel direction (directional and spot).
        vec3 Direction{0.0f, -1.0f, 0.0f};
        /// @brief Linear RGB color.
        vec3 Color{1.0f, 1.0f, 1.0f};
        /// @brief Scales the color at full brightness.
        f32 Intensity{1.0f};
        /// @brief Falloff radius for point and spot lights.
        f32 Range{10.0f};
        /// @brief Spot inner half-angle in radians; full intensity within.
        f32 InnerCone{0.0f};
        /// @brief Spot outer half-angle in radians; zero intensity beyond.
        f32 OuterCone{0.5f};
    };

    /// @brief Per-player input snapshot — the serializable control chokepoint.
    ///
    /// Captures this tick's control state for one player: movement axes, look axes,
    /// and a button bitset. For a local player it is filled each tick from the engine
    /// Veng::Input service; a remote player's would be filled from the wire. It is a
    /// component snapshot rather than a direct device read so the downstream control
    /// system runs identically regardless of where the input originated — the seam a
    /// net layer serializes and replays.
    struct PlayerInput
    {
        /// @brief Movement axes this tick: X strafe, Y vertical, Z forward, each in [-1, 1].
        vec3 Move{0.0f};
        /// @brief Look axes this tick: X yaw, Y pitch, in arbitrary device units.
        vec2 Look{0.0f};
        /// @brief Pressed-button bitset; bit meanings are game policy.
        u32 Buttons = 0;
    };

    /// @brief Abstract, source-agnostic command for what a pawn wants to do this tick.
    ///
    /// The interface between "who decides" (player, AI, remote) and "what happens"
    /// (movement and gameplay systems). A control or AI system writes it; the movement
    /// system consumes it. It is overwritten each tick by its producer, so a zero Intent
    /// is a pawn at rest. Move is expressed in the pawn's local frame.
    struct Intent
    {
        /// @brief Desired move direction in the pawn's local frame: X right, Y up, Z forward.
        vec3 Move{0.0f};
        /// @brief Desired look delta this tick: X yaw, Y pitch, in radians-scaling units.
        vec2 Look{0.0f};
        /// @brief Action-flag bitset (jump/fire/...); bit meanings are game policy.
        u32 Actions = 0;
    };

    /// @brief Seat-to-pawn link: names the pawn entity a player/seat controls.
    ///
    /// Possession is just this reference — nothing inherits or owns through it, and it
    /// is independent of the seat's Viewer.Camera (a spectator views without possessing;
    /// a cutscene retargets the camera without un-possessing). The Pawn field is a
    /// reflected Entity reference, so it remaps on prefab spawn like any intra-prefab
    /// reference.
    struct Possesses
    {
        /// @brief The pawn entity this seat controls.
        Entity Pawn = Entity::Null;
    };

    /// @brief Per-pawn movement tuning the movement system scales its integration by.
    ///
    /// Authored data so a pawn's feel is tunable. A pawn without a Mover moves at the
    /// component's default speeds.
    struct Mover
    {
        /// @brief Local-space move speed in units per second.
        f32 MoveSpeed = 4.0f;
        /// @brief Look/turn speed scaling the Intent's look delta, in radians per unit.
        f32 TurnSpeed = 2.0f;
    };
}

VE_LEAF(::Veng::LightType, 0x6B1D62EF4B5A16ULL, FieldClass::Enum);

VE_REFLECT(::Veng::Name, 0xDA40E8FAC8A6DB84ULL)
VE_FIELD(Value, .DisplayName = "Name")
VE_REFLECT_END();

VE_REFLECT(::Veng::Transform, 0x0AB8E30B2F638555ULL)
VE_FIELD(Position, .DisplayName = "Position", .Tooltip = "Local position, parent space")
VE_FIELD(Rotation, .DisplayName = "Rotation")
VE_FIELD(Scale, .DisplayName = "Scale", .Min = 0.001)
VE_REFLECT_END();

VE_REFLECT(::Veng::Hierarchy, 0x5C9855E287465C5EULL)
VE_FIELD(Parent, .DisplayName = "Parent", .ReadOnly = true)
VE_REFLECT_END();

VE_REFLECT(::Veng::MeshRenderer, 0x3C5CB13E46E0450BULL)
VE_FIELD(Mesh, .DisplayName = "Mesh")
VE_REFLECT_END();

VE_REFLECT(::Veng::CubeShape, 0x2B758A3FE238BAA5ULL)
VE_FIELD(Extent, .DisplayName = "Extent", .Min = 0.001)
VE_FIELD(Material, .DisplayName = "Material")
VE_REFLECT_END();

VE_REFLECT(::Veng::PlaneShape, 0xE53DEFF5662A295ULL)
VE_FIELD(Size, .DisplayName = "Size", .Min = 0.001)
VE_FIELD(Subdivisions, .DisplayName = "Subdivisions", .Min = 1)
VE_FIELD(Material, .DisplayName = "Material")
VE_REFLECT_END();

VE_REFLECT(::Veng::SphereShape, 0xCF4BE61837AB5179ULL)
VE_FIELD(Radius, .DisplayName = "Radius", .Min = 0.001)
VE_FIELD(Rings, .DisplayName = "Rings", .Min = 3)
VE_FIELD(Segments, .DisplayName = "Segments", .Min = 3)
VE_FIELD(Material, .DisplayName = "Material")
VE_REFLECT_END();

VE_REFLECT(::Veng::IcosphereShape, 0x7D9FC0055D5978BBULL)
VE_FIELD(Radius, .DisplayName = "Radius", .Min = 0.001)
VE_FIELD(Subdivisions, .DisplayName = "Subdivisions", .Min = 1)
VE_FIELD(Material, .DisplayName = "Material")
VE_REFLECT_END();

VE_REFLECT(::Veng::CylinderShape, 0x3BB833585C0DAD4CULL)
VE_FIELD(Radius, .DisplayName = "Radius", .Min = 0.001)
VE_FIELD(Height, .DisplayName = "Height", .Min = 0.001)
VE_FIELD(Segments, .DisplayName = "Segments", .Min = 3)
VE_FIELD(Material, .DisplayName = "Material")
VE_REFLECT_END();

VE_REFLECT(::Veng::ConeShape, 0x64D0B47E06329960ULL)
VE_FIELD(Radius, .DisplayName = "Radius", .Min = 0.001)
VE_FIELD(Height, .DisplayName = "Height", .Min = 0.001)
VE_FIELD(Segments, .DisplayName = "Segments", .Min = 3)
VE_FIELD(Material, .DisplayName = "Material")
VE_REFLECT_END();

VE_REFLECT(::Veng::TorusShape, 0xFF864410096026A2ULL)
VE_FIELD(MajorRadius, .DisplayName = "Major Radius", .Min = 0.001)
VE_FIELD(MinorRadius, .DisplayName = "Minor Radius", .Min = 0.001)
VE_FIELD(MajorSegments, .DisplayName = "Major Segments", .Min = 3)
VE_FIELD(MinorSegments, .DisplayName = "Minor Segments", .Min = 3)
VE_FIELD(Material, .DisplayName = "Material")
VE_REFLECT_END();

VE_REFLECT(::Veng::CapsuleShape, 0xDECC3D44F06725DCULL)
VE_FIELD(Radius, .DisplayName = "Radius", .Min = 0.001)
VE_FIELD(Height, .DisplayName = "Height", .Min = 0.001)
VE_FIELD(Segments, .DisplayName = "Segments", .Min = 3)
VE_FIELD(Rings, .DisplayName = "Rings", .Min = 1)
VE_FIELD(Material, .DisplayName = "Material")
VE_REFLECT_END();

VE_VARIANT(::Veng::PrimitiveShapeVariant, 0xC64CE2B415C54D22ULL);

VE_REFLECT(::Veng::Primitive, 0x491B7EC1B0DF276BULL)
VE_FIELD(Shape, .DisplayName = "Shape")
VE_REFLECT_END();

VE_RESOLVE(::Veng::Primitive, ::Veng::ResolvePrimitive);

VE_REFLECT(::Veng::Light, 0xECF6442708DF7C00ULL)
VE_FIELD(Type, .DisplayName = "Type")
VE_FIELD(Direction, .DisplayName = "Direction")
VE_FIELD(Color, .DisplayName = "Color")
VE_FIELD(Intensity, .DisplayName = "Intensity", .Min = 0.0)
VE_FIELD(Range, .DisplayName = "Range", .Min = 0.0, .Step = 0.1)
VE_FIELD(InnerCone, .DisplayName = "Inner Cone", .Min = 0.0, .Max = 3.14159265, .Step = 0.01)
VE_FIELD(OuterCone, .DisplayName = "Outer Cone", .Min = 0.0, .Max = 3.14159265, .Step = 0.01)
VE_REFLECT_END();

VE_REFLECT(::Veng::PlayerInput, 0x5401D36B1EF55045ULL)
VE_FIELD(Move, .DisplayName = "Move")
VE_FIELD(Look, .DisplayName = "Look")
VE_FIELD(Buttons, .DisplayName = "Buttons")
VE_REFLECT_END();

VE_REFLECT(::Veng::Intent, 0x27F416122B525965ULL)
VE_FIELD(Move, .DisplayName = "Move")
VE_FIELD(Look, .DisplayName = "Look")
VE_FIELD(Actions, .DisplayName = "Actions")
VE_REFLECT_END();

VE_REFLECT(::Veng::Possesses, 0xC7D4144C7DF95B9BULL)
VE_FIELD(Pawn, .DisplayName = "Pawn")
VE_REFLECT_END();

VE_REFLECT(::Veng::Mover, 0x7774F1C2B00DE07EULL)
VE_FIELD(MoveSpeed, .DisplayName = "Move Speed", .Min = 0.0)
VE_FIELD(TurnSpeed, .DisplayName = "Turn Speed", .Min = 0.0)
VE_REFLECT_END();
