#include <Veng/Scene/BuiltinTypes.h>

#include <Veng/Asset/AssetHandleType.h>
#include <Veng/Asset/InputMappingContext.h>
#include <Veng/Gui/Overlay.h>
#include <Veng/Gui/Surface.h>
#include <Veng/Net/Session.h>
#include <Veng/Physics/Components.h>
#include <Veng/Physics/Gravity.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/Atmosphere.h>
#include <Veng/Renderer/CaptureSurface.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/RemoteInterpolationSystem.h>
#include <Veng/Scene/Requests.h>

namespace Veng
{
    void RegisterBuiltinTypes(TypeRegistry& registry)
    {
        // Builtins go through the same Register<T> path as game types; no special-casing.
        registry.Register<Name>();
        registry.Register<Transform>();
        registry.Register<Hierarchy>();
        // Per-frame (View-phase) authored pose: the render gather draws the entity's live
        // transform instead of blending the sim-tick history. Runtime-only, fieldless.
        registry.Register<ViewPose>();
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

        // Rigid-body simulation: what the solver simulates, the shape it collides with, the
        // overlap-only sensor, and the three constraints. MotionType, PhysicsLayer and
        // ColliderShape register transitively through their fields. PhysicsPose and
        // SensorOverlaps are the solver's own output channels — runtime-only, so they carry no
        // reflected field and neither serializes nor rides the wire.
        registry.Register<RigidBody>();
        registry.Register<Collider>();
        registry.Register<PhysicsPose>();
        registry.Register<Sensor>();
        registry.Register<SensorOverlaps>();
        registry.Register<FixedConstraint>();
        registry.Register<PointConstraint>();
        registry.Register<HingeConstraint>();

        // Gravity as a field of sources, evaluated per body per step rather than a world constant.
        // GravityKind, RegionShape and the Region struct register transitively through the
        // component's fields.
        registry.Register<GravitySource>();

        // Net-anticipation seam: the ownership annotation and the camera-rig relationships
        // (third-person follow, first-person look, point orbit) the View-phase rig reads.
        registry.Register<Tier>();
        registry.Register<Authority>();
        registry.Register<CameraFollow>();
        registry.Register<CameraLook>();
        registry.Register<CameraOrbit>();

        // The wire identity of a replicated entity (server-assigned, runtime-only). Reflected so the
        // inspector can surface the assigned id; the net layer keys snapshots by it.
        registry.Register<NetIdentity>();

        // The opaque 128-bit anchor binding a replicated entity to its live local twin. Reflected so a
        // consumer authors/inspects it; the net layer resolves a claimant by it at spawn time.
        registry.Register<NetAnchor>();

        // The account a seat entity belongs to, stamped server-side at seat spawn. Not replicated —
        // the id stays server-local. Net::AccountId registers transitively through its field.
        registry.Register<SeatAccount>();

        // The prefab a spawned root came from, stamped by Prefab::SpawnInto, and the opt-in mark
        // that turns it into an engine-driven prefab association on a hosted world. Both carry no
        // reflected field, so neither serializes nor rides the wire.
        registry.Register<PrefabSource>();
        registry.Register<NetSpawn>();

        // The pawn a presenting viewport's own seat controls, derived and stamped by the engine
        // each frame. Runtime-only: it carries no reflected field, so it never serializes and never
        // rides the wire.
        registry.Register<LocalControl>();

        // The client-side pose-sample buffer a replicated entity carries, filled by snapshots and read
        // by the View-phase RemoteInterpolationSystem. Runtime-only: it carries no reflected field, so
        // it never serializes and never rides the wire.
        registry.Register<RemoteInterpolation>();

        // The decaying render residual a reconciliation correction leaves on a predicted entity,
        // eased to zero by the View-phase decay and applied only at the gather. Runtime-only.
        registry.Register<PredictionError>();

        // Game mode as data: the per-scene config a spawn rule reads. A game authors whatever
        // further mode-state components its own rule systems read.
        registry.Register<GameModeConfig>();

        // The per-account session record, registered so its reflection-binary encoding (the
        // durability blob) has a schema everywhere a SessionRegistry runs. Not a component — it
        // lives at the host tier, keyed by account, never in a scene. Net::WorldKey and
        // Net::Blob register transitively through its fields.
        registry.Register<Net::SessionRecord>();

        // Local-only runtime requests a gameplay system stamps and the engine drains at its
        // frame-safe point: travel, start-hosting, connect, stop-net, exit, and input-focus
        // capture/release. None is replicated (they never ride a snapshot); RequestStatus registers
        // transitively through each request's Status, and InputFocus through FocusRequest's Focus.
        registry.Register<TravelRequest>();
        registry.Register<HostRequest>();
        registry.Register<ConnectRequest>();
        registry.Register<StopNetRequest>();
        registry.Register<ExitRequest>();
        registry.Register<FocusRequest>();

        // Level-scoped post/pipeline render knobs a Level carries and the app maps onto the renderer.
        registry.Register<Renderer::Tonemapper>();
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

        // A scene-authored volume field, resolved by the renderer per Execute. Its authored knobs
        // register transitively; the runtime-only Field carries no reflected field, so it neither
        // registers Renderer::VolumeField nor serializes.
        registry.Register<VolumeField>();

        // A document mapped onto a world mesh, driven into an HDR target and glowing through the
        // scene's bloom. GuiSurfaceDomain and the AssetHandle<Gui::UIDocument> recipe leaf register
        // transitively through its fields.
        registry.Register<GuiSurface>();

        // A document presented on the presenting viewport's screen-space layer stack, discovered and
        // driven by the Viewport. The AssetHandle<Gui::UIDocument> recipe leaf and the Entity seat
        // reference register transitively through its fields.
        registry.Register<GuiOverlay>();

        // A render-to-texture capture declared on an entity, discovered and driven by the engine, its
        // output sampled by the entity's material. Renderer::CaptureShape and CaptureRefresh register
        // transitively through its fields; the runtime-only Unique carries no reflected field.
        registry.Register<Renderer::CaptureSurface>();

        // Every leaf type the engine declares, whether or not a builtin component happens to
        // reference one. They are the vocabulary a consumer names a type *by* — a data table
        // declares each column's type this way — so the registry must hold them all rather than
        // only those a builtin component drags in transitively.
        registry.Register<bool>();
        registry.Register<u8>();
        registry.Register<i32>();
        registry.Register<u32>();
        registry.Register<i64>();
        registry.Register<u64>();
        registry.Register<f32>();
        registry.Register<vec2>();
        registry.Register<vec3>();
        registry.Register<vec4>();
        registry.Register<uvec2>();
        registry.Register<quat>();
        registry.Register<mat4>();
        registry.Register<string>();
        registry.Register<Entity>();

        registry.Register<AssetHandle<Texture>>();
        registry.Register<AssetHandle<Mesh>>();
        registry.Register<AssetHandle<Material>>();
        registry.Register<AssetHandle<MaterialInstance>>();
        registry.Register<AssetHandle<Prefab>>();
        registry.Register<AssetHandle<Level>>();
        registry.Register<AssetHandle<Skeleton>>();
        registry.Register<AssetHandle<Animation>>();
        registry.Register<AssetHandle<EnvironmentMap>>();
        registry.Register<AssetHandle<InputMappingContext>>();
        registry.Register<AssetHandle<Font>>();
        registry.Register<AssetHandle<Gui::StyleSheet>>();
        registry.Register<AssetHandle<Gui::UIDocument>>();
        registry.Register<AssetHandle<TableSchema>>();
        registry.Register<AssetHandle<DataTable>>();
        registry.Register<AssetHandle<RawAsset>>();
    }
}
