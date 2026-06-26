#include <Veng/Scene/BuiltinTypes.h>

#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Camera.h>

namespace Veng
{
    void RegisterBuiltinTypes(TypeRegistry& registry)
    {
        // Builtins go through the same Register<T> path as game types; no special-casing.
        registry.Register<Name>();
        registry.Register<Transform>();
        registry.Register<Hierarchy>();
        registry.Register<Camera>();
        registry.Register<Viewer>();
        registry.Register<MeshRenderer>();
        registry.Register<RootMotionMode>();
        registry.Register<Animator>();
        registry.Register<SkinnedPose>();
        registry.Register<RootMotionDelta>();
        registry.Register<LightType>();
        registry.Register<Light>();

        // The control pipeline: per-player snapshot, abstract command, seat→pawn link,
        // and per-pawn movement tuning.
        registry.Register<PlayerInput>();
        registry.Register<Intent>();
        registry.Register<Possesses>();
        registry.Register<Mover>();

        // Net-anticipation seam: the ownership annotation and the camera-rig follow
        // relationship the View-phase rig reads.
        registry.Register<Tier>();
        registry.Register<Authority>();
        registry.Register<CameraFollow>();

        // Game mode as data: the replicated Session state and the per-scene config a
        // spawn rule reads. SessionPhase registers transitively through Session.
        registry.Register<Session>();
        registry.Register<GameModeConfig>();

        // Level-scoped render settings a Level carries and the app maps onto the renderer.
        registry.Register<LevelRenderSettings>();
    }
}
