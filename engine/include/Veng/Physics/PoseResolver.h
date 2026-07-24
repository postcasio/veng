#pragma once

#include <Veng/Veng.h>
#include <Veng/Physics/Components.h>
#include <Veng/Scene/Entity.h>

namespace Veng
{
    class Scene;

    /// @brief The consumer seam mapping an entity between a scene's Transform chain and its physics
    ///        world's frame.
    ///
    /// The gameplay systems that reason in the solver's space — InteractionSystem's focus resolution
    /// and VehicleSystem's enter/exit — derive a pose from an entity, test it against the physics
    /// world, and write the result back. Composing that pose up the Transform chain is correct only
    /// while the chain and the solver share an origin. A consumer whose authoritative positions live
    /// *outside* the f32 Transform — a large-extent world where Transform is a render-relative
    /// projection and the physics space is anchored elsewhere — installs a resolver through
    /// Scene::SetPhysicsPoseResolver, and those systems then work in the frame the solver actually
    /// integrates in.
    ///
    /// It is the read-side counterpart of RigidBody::SyncTransform. That flag hands a consumer the
    /// Transform write-back the step performs; this seam hands it the poses the engine *reads*, plus
    /// the write-back those reads feed.
    ///
    /// Either hook may be left empty, in which case the matching default below is used — so a
    /// consumer overrides only the direction it needs, and a scene with no resolver installed at all
    /// behaves exactly as DefaultResolvePhysicsPose and DefaultPlaceAtPhysicsPose describe.
    ///
    /// Both hooks are called from the Sim phase, on the thread ticking the scene, and the scene owns
    /// the resolver for as long as it is installed.
    struct PhysicsPoseResolver
    {
        /// @brief Resolves @p entity's pose, offset within its own frame, into the world's frame.
        ///
        /// @p localOffset is a pose in the entity's own frame — identity asks for the entity itself,
        /// a mesh socket's local matrix asks for that socket's place. The result is in the physics
        /// world's frame, so it may be handed straight to a query or to PhysicsWorld.
        ///
        /// @warning It may be invoked from inside a live scene query, so it must make no structural
        /// change to the scene. The `const Scene&` is what enforces that.
        function<PhysicsPose(const Scene& scene, Entity entity, const mat4& localOffset)> Resolve;

        /// @brief Records a pose expressed in the physics world's frame back onto @p entity.
        ///
        /// The reverse direction, and the report of where a resolved placement landed: a consumer
        /// holding an authoritative store outside the Transform writes the pose there, and leaves
        /// the entity's Transform to its own projection pass. It is called outside any live query,
        /// so it may make structural changes.
        function<void(Scene& scene, Entity entity, const PhysicsPose& pose)> Place;
    };

    /// @brief Composes @p entity's pose up its Transform chain — the resolve used when none is installed.
    ///
    /// Evaluates `WorldMatrix(scene, entity) * localOffset`, taking the translation as the position
    /// and the normalized basis as the rotation, so a uniformly scaled chain resolves to a rigid
    /// pose. This is what makes the seam optional: a scene whose Transform chain *is* the solver's
    /// frame needs nothing installed. A resolver covering only some entities delegates the rest
    /// here.
    /// @param scene        The scene to read the chain from.
    /// @param entity       The entity to resolve.
    /// @param localOffset  A pose in @p entity's own frame; identity resolves the entity itself.
    /// @return The composed pose, read as being in the physics world's frame.
    [[nodiscard]] VE_API PhysicsPose DefaultResolvePhysicsPose(const Scene& scene, Entity entity,
                                                               const mat4& localOffset);

    /// @brief Writes @p pose onto @p entity's Transform — the place used when none is installed.
    ///
    /// Adds a Transform when the entity has none, writes the position and rotation, and resets the
    /// scale to one: the pose is a rigid placement, and an entity just detached from a mesh socket
    /// otherwise carries that socket's scale. The Transform is *local*, so the caller is expected to
    /// have made the entity a scene-graph root — the same expectation a physics body carries.
    /// @param scene   The scene to write into.
    /// @param entity  The entity to place.
    /// @param pose    The pose, in the physics world's frame.
    VE_API void DefaultPlaceAtPhysicsPose(Scene& scene, Entity entity, const PhysicsPose& pose);

    /// @brief Resolves @p entity's pose in the physics world's frame through @p scene's resolver.
    ///
    /// Forwards to the scene's installed PhysicsPoseResolver::Resolve, or to
    /// DefaultResolvePhysicsPose when the scene has no resolver or its resolve hook is empty.
    /// @param scene        The scene whose resolver to use.
    /// @param entity       The entity to resolve.
    /// @param localOffset  A pose in @p entity's own frame; identity resolves the entity itself.
    /// @return The entity's pose in the physics world's frame.
    [[nodiscard]] VE_API PhysicsPose ResolvePhysicsPose(const Scene& scene, Entity entity,
                                                        const mat4& localOffset = mat4(1.0f));

    /// @brief Records @p pose onto @p entity through @p scene's resolver.
    ///
    /// Forwards to the scene's installed PhysicsPoseResolver::Place, or to
    /// DefaultPlaceAtPhysicsPose when the scene has no resolver or its place hook is empty.
    /// @param scene   The scene whose resolver to use.
    /// @param entity  The entity to place.
    /// @param pose    The pose, in the physics world's frame.
    VE_API void PlaceAtPhysicsPose(Scene& scene, Entity entity, const PhysicsPose& pose);
}
