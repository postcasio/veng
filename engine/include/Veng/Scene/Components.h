#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Reflection/Reflect.h>

#include <Veng/Asset/AssetHandle.h>

namespace Veng
{
    class Mesh;

    // The engine's builtin components. Each is a plain reflected type a Scene
    // pools; they are registered into the TypeRegistry by RegisterBuiltinTypes
    // through the same Register<T> a game uses — not special-cased.

    // A human-readable label for an entity. Display/logging only; never an
    // identity key.
    struct Name
    {
        string Value;
    };

    // Local TRS — relative to the entity's Parent, or to world for a root.
    // World matrices are derived by the Parent-chain walk in Transforms.h; this
    // struct never stores a world matrix.
    struct Transform
    {
        vec3 Position{0.0f};
        quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
        vec3 Scale{1.0f};
    };

    // Links an entity to its parent. The world transform composes
    // parent.world * local up this chain. Entity::Null (the default) marks a
    // root.
    struct Parent
    {
        Entity Value = Entity::Null;
    };

    // The bridge from a scene entity to rendering: the mesh it draws. The mesh
    // owns its materials, so a renderer queries (world Transform, MeshRenderer)
    // and draws each submesh with its material.
    struct MeshRenderer
    {
        AssetHandle<Mesh> Mesh;
    };

    // The shape of a punctual light, selecting how the deferred lighting pass
    // attenuates it. Directional has no position or falloff; Point and Spot are
    // placed by the entity's Transform and fall off with distance (Spot adds a
    // cone). The integer values are stable: they are packed into the light SSBO
    // and persisted in prefabs.
    enum class LightType : u32
    {
        Directional = 0,
        Point = 1,
        Spot = 2,
    };

    // A light shaded by the deferred lighting pass. Type selects directional,
    // point, or spot. Direction is the world-space travel direction (directional
    // and spot); Color is linear RGB; Intensity scales it. Range is the point/spot
    // falloff radius. InnerCone/OuterCone are the spot's half-angles in radians:
    // full intensity within InnerCone, zero beyond OuterCone, smooth between.
    //
    // The light's world position is the entity's Transform — never stored here —
    // so a parented or animated light moves with its entity.
    struct Light
    {
        LightType Type{LightType::Directional};
        vec3 Direction{0.0f, -1.0f, 0.0f};
        vec3 Color{1.0f, 1.0f, 1.0f};
        f32 Intensity{1.0f};
        f32 Range{10.0f};
        f32 InnerCone{0.0f};
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

VE_REFLECT(::Veng::Parent, 0x5C9855E287465C5EULL)
    VE_FIELD(Value, .DisplayName = "Parent", .ReadOnly = true)
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
