#include <Veng/Scene/BuiltinTypes.h>

#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/Atmosphere.h>
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

        // Autonomous constant transform velocity (drift + spin) the ConstantMotionSystem
        // integrates. MotionSpace registers transitively through ConstantMotion.
        registry.Register<ConstantMotion>();

        // Net-anticipation seam: the ownership annotation and the camera-rig follow
        // relationship the View-phase rig reads.
        registry.Register<Tier>();
        registry.Register<Authority>();
        registry.Register<CameraFollow>();

        // Game mode as data: the replicated Session state and the per-scene config a
        // spawn rule reads. SessionPhase registers transitively through Session.
        registry.Register<Session>();
        registry.Register<GameModeConfig>();

        // Level-scoped post/pipeline render knobs a Level carries and the app maps onto the renderer.
        registry.Register<LevelRenderSettings>();

        // Author-opt-in sky/lighting components: presence drives the renderer's sky/IBL topology.
        // Renderer::Atmosphere registers transitively through the Atmosphere component's Params field.
        registry.Register<Environment>();
        registry.Register<Atmosphere>();
        registry.Register<Skylight>();
    }
}
