#include <Veng/Scene/BuiltinTypes.h>

#include <Veng/Asset/InputMappingContext.h>
#include <Veng/Gui/Overlay.h>
#include <Veng/Gui/Surface.h>
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
        // The seat's ordered active input contexts, referenced by AssetId; authored on a prefab
        // and pushed/popped by gameplay, resolved through the ordinary load path.
        registry.Register<InputContextStack>();
        // The reflected on-disk payload of an input map, read by InputMapLoader.
        registry.Register<InputMapData>();
        registry.Register<Intent>();
        registry.Register<Possesses>();
        // The seat's device assignment: which keyboard/pad feed this seat, read per seat by
        // InputMappingSystem to build a filtered raw view. Absent ⇒ the seat reads no local device.
        registry.Register<SeatInput>();
        registry.Register<Mover>();

        // Autonomous constant transform velocity (drift + spin) the ConstantMotionSystem
        // integrates. MotionSpace registers transitively through ConstantMotion.
        registry.Register<ConstantMotion>();

        // Net-anticipation seam: the ownership annotation and the camera-rig relationships
        // (third-person follow, first-person look) the View-phase rig reads.
        registry.Register<Tier>();
        registry.Register<Authority>();
        registry.Register<CameraFollow>();
        registry.Register<CameraLook>();

        // Game mode as data: the replicated Session state and the per-scene config a
        // spawn rule reads. SessionPhase registers transitively through Session.
        registry.Register<Session>();
        registry.Register<GameModeConfig>();

        // Level-scoped post/pipeline render knobs a Level carries and the app maps onto the renderer.
        registry.Register<LevelRenderSettings>();

        // The scene's one authored sky, resolved by the renderer per Execute. SkySource and its
        // alternatives (EnvironmentSky / AtmosphereSky / MaterialSky), SkyLighting, and
        // Renderer::Atmosphere register transitively through the Sky component's fields; TimeOfDay's
        // Renderer::SunOrbit registers through its Orbit field.
        registry.Register<Sky>();
        registry.Register<TimeOfDay>();

        // A scene-authored point field, resolved by the renderer per Execute. Its authored Lod
        // (Renderer::PointFieldLod) and CellSize register transitively; the runtime-only Field
        // carries no reflected field, so it neither registers Renderer::PointField nor serializes.
        registry.Register<PointField>();

        // A document mapped onto a world mesh, driven into an HDR target and glowing through the
        // scene's bloom. GuiSurfaceDomain and the AssetHandle<Gui::UIDocument> recipe leaf register
        // transitively through its fields.
        registry.Register<GuiSurface>();

        // A document presented on the presenting viewport's screen-space layer stack, discovered and
        // driven by the Viewport. The AssetHandle<Gui::UIDocument> recipe leaf and the Entity seat
        // reference register transitively through its fields.
        registry.Register<GuiOverlay>();
    }
}
