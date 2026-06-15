#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Reflection/TypeRegistry.h>

namespace Veng
{
    // The engine's builtin components. Each is a plain reflected type a Scene
    // pools; they are registered into the TypeRegistry at Application startup
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
}

VE_TYPE(::Veng::Name, 0xDA40E8FAC8A6DB84ULL);
VE_TYPE(::Veng::Transform, 0x0AB8E30B2F638555ULL);
VE_TYPE(::Veng::Parent, 0x5C9855E287465C5EULL);
