#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Scene/Entity.h>

namespace Veng
{
    class Prefab;

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

    /// @brief Marks an entity whose Transform is authored per frame, in the View phase.
    ///
    /// The render gather blends every transform between the scene's last two Sim-tick snapshots,
    /// but a View-authored pose — a camera-anchored impostor, a billboard, any presentation
    /// entity re-placed each frame — is already this frame's pose: the snapshots hold earlier
    /// frames' writes, so blending renders the entity a frame stale, visibly lagging its anchor
    /// while the camera moves. This tag makes the entity's own local transform resolve live in
    /// Scene::GetInterpolatedWorldTransform; ancestor levels keep their own interpolation.
    /// Runtime-only display state, added by the system that authors the pose.
    struct ViewPose
    {
    };

    /// @brief Per-scene game-mode configuration: the data a scene names to pick its mode.
    ///
    /// Held on the level's settings entity. Names the player prefab a spawn rule
    /// instantiates; a game authors whatever further mode-state components its own rule
    /// systems read, beside it. Selecting a different mode is choosing a different config
    /// plus a different registered rule set — no C++ path picks the mode. Its authored
    /// JSON key is "gameMode".
    struct GameModeConfig
    {
        /// @brief The player prefab a spawn rule instantiates at start.
        AssetHandle<Prefab> PlayerPrefab;
    };
}

VE_REFLECT(::Veng::Name, 0xDA40E8FAC8A6DB84ULL)
VE_FIELD(Value, .DisplayName = "Name")
VE_REFLECT_END();

VE_REFLECT(::Veng::Transform, 0x0AB8E30B2F638555ULL)
VE_FIELD(Position, .DisplayName = "Position", .Tooltip = "Local position, parent space")
VE_FIELD(Rotation, .DisplayName = "Rotation")
VE_FIELD(Scale, .DisplayName = "Scale", .Display = {.Min = 0.001})
VE_REFLECT_END();
// The spatial pose is the canonical replicated state — every visible remote entity carries it.
VE_REPLICATED(::Veng::Transform);

VE_REFLECT(::Veng::Hierarchy, 0x5C9855E287465C5EULL)
VE_FIELD(Parent, .DisplayName = "Parent", .ReadOnly = true)
VE_REFLECT_END();

VE_TYPE(::Veng::ViewPose, 0xC8BD67E5A1ED1A82ULL);

VE_REFLECT(::Veng::GameModeConfig, 0xAE57419CF98B07F8ULL)
VE_FIELD(PlayerPrefab, .DisplayName = "Player Prefab")
VE_REFLECT_END();
