#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Reflection/Reflect.h>

#include <Veng/Asset/AssetHandle.h>

namespace Veng
{
    class Mesh;

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

VE_REFLECT(::Veng::Light, 0xECF6442708DF7C00ULL)
VE_FIELD(Type, .DisplayName = "Type")
VE_FIELD(Direction, .DisplayName = "Direction")
VE_FIELD(Color, .DisplayName = "Color")
VE_FIELD(Intensity, .DisplayName = "Intensity", .Min = 0.0)
VE_FIELD(Range, .DisplayName = "Range", .Min = 0.0, .Step = 0.1)
VE_FIELD(InnerCone, .DisplayName = "Inner Cone", .Min = 0.0, .Max = 3.14159265, .Step = 0.01)
VE_FIELD(OuterCone, .DisplayName = "Outer Cone", .Min = 0.0, .Max = 3.14159265, .Step = 0.01)
VE_REFLECT_END();
