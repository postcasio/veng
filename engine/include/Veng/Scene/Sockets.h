#pragma once

#include <string_view>

#include <Veng/Veng.h>
#include <Veng/Asset/AssetError.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Entity.h>

/// @file
/// @brief Reaching a model's sockets: resident reads in a live scene, CPU reads from cooked data.
///
/// The **resident** reads — FindMeshSocket and AttachToSocket — resolve a live entity's loaded
/// mesh, and are what places something in a presented scene. The **CPU** reads —
/// AssetManager::ReadMeshSockets, ReadPrefabSockets and ReadPrefabMeshes — decode the cooked data
/// directly and make nothing resident, for a process that reasons about where things attach
/// without presenting the model (a headless process, a planner, a tool). Both decode the same
/// cooked socket table, so they cannot disagree.

namespace Veng
{
    class AssetManager;
    class Scene;

    /// @brief Returns the named socket on the mesh `meshEntity` draws, or nullptr.
    ///
    /// Resolves the entity's MeshRenderer, requires its mesh handle to be resident, and looks the
    /// name up through Mesh::FindSocket. Returns nullptr — never asserts — when the entity has no
    /// MeshRenderer, its mesh is not yet resident, or the mesh has no socket by that name, since
    /// each of those is a content or timing condition a consumer wants to report.
    /// @param scene       The scene the entity lives in.
    /// @param meshEntity  The entity carrying the MeshRenderer whose mesh owns the socket.
    /// @param socketName  The authored socket name, matched exactly.
    /// @return The socket, valid for the mesh's lifetime, or nullptr.
    [[nodiscard]] const MeshSocket* FindMeshSocket(const Scene& scene, Entity meshEntity,
                                                   std::string_view socketName);

    /// @brief Parents `child` to `meshEntity` and places it at the named socket.
    ///
    /// A socket attachment is plain parenting: the child is reparented under the mesh entity and
    /// its local Transform is overwritten with the socket's mesh-space transform (a Transform is
    /// added when the child has none). Everything downstream therefore works unchanged — the
    /// world matrix composes up the chain, DestroyEntity recurses through it, and a socket on a
    /// moving parent carries its child for free.
    ///
    /// The socket's rotation is applied, not only its position: local -Z is the socket's forward
    /// and local +Y its up (see MeshSocket).
    ///
    /// The socket is **mesh-space and static**. Attaching to a skinned mesh's socket does not
    /// follow the animated skeleton.
    /// @param scene       The scene both entities live in.
    /// @param child       The entity to attach; must be alive.
    /// @param meshEntity  The entity carrying the MeshRenderer whose mesh owns the socket.
    /// @param socketName  The authored socket name, matched exactly.
    /// @return True when the socket resolved and the child was attached; false when it did not,
    ///         leaving both entities untouched.
    bool AttachToSocket(Scene& scene, Entity child, Entity meshEntity, std::string_view socketName);

    /// @brief One socket of a prefab, placed in prefab-root space.
    struct PrefabSocket
    {
        /// @brief The Name component of the prefab entity drawing the socket's mesh.
        ///
        /// Empty when that entity carries no Name. It is how the socket is reached once the
        /// prefab is spawned: AttachToSocket onto the entity by this name, with Local.Name.
        string EntityName;
        /// @brief The cooked mesh the socket belongs to.
        AssetId Mesh;
        /// @brief The socket as the mesh authored it, in mesh space.
        MeshSocket Local;
        /// @brief The socket's transform relative to its prefab root.
        ///
        /// Composed through the entity chain from the socket's entity up to — but not including
        /// — the root entity it descends from, so the root's own Transform is left out: an
        /// entity placing that root at world transform W finds the socket at W · RootSpace, the
        /// same world pose AttachToSocket gives an entity attached there.
        Transform RootSpace;
    };

    /// @brief Reads every socket a prefab's meshes carry, in prefab-root space, making nothing resident.
    ///
    /// Walks the cooked prefab directly — never loading it, since a prefab load makes each mesh
    /// it names resident — and reads each rendered mesh's sockets through
    /// AssetManager::ReadMeshSockets. A socket is reported for every entity carrying a
    /// MeshRenderer whose cooked Mesh is set and whose inline recipe Source is empty (a recipe
    /// replaces the cooked mesh at spawn, and a built primitive carries no sockets);
    /// MeshRenderer::Visible is not consulted. Nested prefabs are expanded exactly as
    /// Prefab::SpawnInto expands them — the nesting entity is its body's first root, its records
    /// replace the body root's whole components, and the body's further roots hang under it — so
    /// the result matches what a spawn would present.
    ///
    /// No render context is used, nothing is cached (the caller caches), and a prefab whose
    /// entities render the same mesh twice reads it once. A prefab with several roots reports each
    /// socket relative to the root its entity descends from.
    /// @param assets  The manager whose mounted archives hold the prefab and its meshes; its
    ///                TypeRegistry must know the builtin component types.
    /// @param prefab  The AssetTypes::Prefab asset to read.
    /// @return The sockets, sorted by EntityName then socket name (authored order breaks a tie),
    ///         or the first NotFound / WrongType / Corrupt error met reading the prefab, a nested
    ///         prefab, or a mesh.
    [[nodiscard]] AssetResult<vector<PrefabSocket>> ReadPrefabSockets(const AssetManager& assets,
                                                                      AssetId prefab);

    /// @brief One cooked mesh a prefab renders, placed in prefab-root space.
    struct PrefabMesh
    {
        /// @brief The Name component of the prefab entity drawing the mesh.
        ///
        /// Empty when that entity carries no Name. It is how the drawing entity is reached once the
        /// prefab is spawned.
        string EntityName;
        /// @brief The cooked mesh the entity's MeshRenderer names.
        AssetId Mesh;
        /// @brief The drawing entity's transform relative to its prefab root.
        ///
        /// Composed exactly as PrefabSocket::RootSpace is: up the entity chain to — but not
        /// including — the root the entity descends from, so the root's own Transform is left out
        /// and a root entity reports the identity. An entity placing that root at world transform W
        /// draws the mesh at W · RootSpace.
        Transform RootSpace;
    };

    /// @brief Reads which cooked meshes a prefab renders, in prefab-root space, making nothing resident.
    ///
    /// Walks the cooked prefab the way ReadPrefabSockets does — never loading it, expanding nested
    /// prefabs exactly as Prefab::SpawnInto expands them — and reports one entry per entity whose
    /// MeshRenderer names a cooked Mesh and carries an empty inline recipe Source (a recipe
    /// replaces the cooked mesh at spawn, so it has no AssetId to report). MeshRenderer::Visible is
    /// not consulted, and an entity rendering the same mesh as another is reported on its own.
    ///
    /// The meshes themselves are not read: the result names them, and a caller reads what it needs
    /// through the CPU reads on AssetManager — AssetManager::ReadMeshSkeleton for a skinned mesh's
    /// joints, AssetManager::ReadMeshSockets for its sockets — so a headless process places a
    /// prefab's skinned-mesh joints with no GPU residency at all. No render context is used and
    /// nothing is cached (the caller caches).
    /// @param assets  The manager whose mounted archives hold the prefab; its TypeRegistry must
    ///                know the builtin component types.
    /// @param prefab  The AssetTypes::Prefab asset to read.
    /// @return The rendered meshes, sorted by EntityName (flattened authored order breaks a tie) —
    ///         empty for a prefab that renders nothing — or the first NotFound / WrongType / Corrupt
    ///         error met reading the prefab or a nested prefab.
    [[nodiscard]] AssetResult<vector<PrefabMesh>> ReadPrefabMeshes(const AssetManager& assets,
                                                                   AssetId prefab);
}
