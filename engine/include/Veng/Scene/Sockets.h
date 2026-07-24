#pragma once

#include <string_view>

#include <Veng/Veng.h>
#include <Veng/Scene/Entity.h>

namespace Veng
{
    class Scene;
    struct MeshSocket;

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
}
