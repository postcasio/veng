#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Reflection/Variant.h>

#include <Veng/Asset/AssetHandle.h>

namespace Veng
{
    class Mesh;
    class Material;
    class MaterialInstance;
    class Prefab;
    class Environment;
    struct Animation;

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

    /// @brief Cube shape recipe: the parameters of Primitives::Cube plus its material.
    struct CubeShape
    {
        /// @brief Full width across each axis, in units.
        f32 Extent = 1.0f;
        /// @brief Material instance recorded on the generated submesh.
        AssetHandle<MaterialInstance> Material;
    };

    /// @brief Plane shape recipe: the parameters of Primitives::Plane plus its material.
    struct PlaneShape
    {
        /// @brief Plane dimensions in the XZ axes.
        vec2 Size = vec2(1.0f);
        /// @brief Quad count per axis.
        uvec2 Subdivisions = uvec2(1);
        /// @brief Material instance recorded on the generated submesh.
        AssetHandle<MaterialInstance> Material;
    };

    /// @brief UV-sphere shape recipe: the parameters of Primitives::Sphere plus its material.
    struct SphereShape
    {
        /// @brief Sphere radius.
        f32 Radius = 0.5f;
        /// @brief Latitude band count.
        u32 Rings = 16;
        /// @brief Longitude band count.
        u32 Segments = 32;
        /// @brief Material instance recorded on the generated submesh.
        AssetHandle<MaterialInstance> Material;
    };

    /// @brief Icosphere shape recipe: the parameters of Primitives::Icosphere plus its material.
    struct IcosphereShape
    {
        /// @brief Sphere radius.
        f32 Radius = 0.5f;
        /// @brief Icosahedron subdivision count.
        u32 Subdivisions = 3;
        /// @brief Material instance recorded on the generated submesh.
        AssetHandle<MaterialInstance> Material;
    };

    /// @brief Cylinder shape recipe: the parameters of Primitives::Cylinder plus its material.
    struct CylinderShape
    {
        /// @brief Cylinder radius.
        f32 Radius = 0.5f;
        /// @brief Full height along the Y axis.
        f32 Height = 1.0f;
        /// @brief Longitude band count around the side.
        u32 Segments = 32;
        /// @brief Material instance recorded on the generated submesh.
        AssetHandle<MaterialInstance> Material;
    };

    /// @brief Cone shape recipe: the parameters of Primitives::Cone plus its material.
    struct ConeShape
    {
        /// @brief Base radius.
        f32 Radius = 0.5f;
        /// @brief Full height from base to apex along the Y axis.
        f32 Height = 1.0f;
        /// @brief Longitude band count around the base.
        u32 Segments = 32;
        /// @brief Material instance recorded on the generated submesh.
        AssetHandle<MaterialInstance> Material;
    };

    /// @brief Torus shape recipe: the parameters of Primitives::Torus plus its material.
    struct TorusShape
    {
        /// @brief Distance from the center to the tube center.
        f32 MajorRadius = 0.5f;
        /// @brief Tube radius.
        f32 MinorRadius = 0.2f;
        /// @brief Band count around the ring.
        u32 MajorSegments = 32;
        /// @brief Band count around the tube.
        u32 MinorSegments = 16;
        /// @brief Material instance recorded on the generated submesh.
        AssetHandle<MaterialInstance> Material;
    };

    /// @brief Capsule shape recipe: the parameters of Primitives::Capsule plus its material.
    struct CapsuleShape
    {
        /// @brief Cap radius and cylinder radius.
        f32 Radius = 0.5f;
        /// @brief Height of the central cylinder along the Y axis (excludes the caps).
        f32 Height = 1.0f;
        /// @brief Longitude band count shared by the band and caps.
        u32 Segments = 32;
        /// @brief Latitude band count per hemisphere cap.
        u32 Rings = 8;
        /// @brief Material instance recorded on the generated submesh.
        AssetHandle<MaterialInstance> Material;
    };

    /// @brief The inline procedural source of a MeshRenderer's mesh: one shape recipe or empty.
    ///
    /// The active alternative is the primitive kind and carries that kind's parameters plus
    /// its material. Empty means the MeshRenderer's cooked Mesh is used as authored; a
    /// non-empty alternative is built into the MeshRenderer's Mesh at spawn/edit.
    using MeshSource = Variant<CubeShape, PlaneShape, SphereShape, IcosphereShape, CylinderShape,
                               ConeShape, TorusShape, CapsuleShape>;

    /// @brief Component that binds a scene entity to a renderable mesh.
    ///
    /// The mesh owns its materials, so a renderer queries (Transform, MeshRenderer)
    /// and draws each submesh with its material. The mesh's source is either a cooked
    /// AssetId (Mesh, authored directly) or an inline procedural recipe (Source, the
    /// active shape variant). A non-empty Source is built into Mesh during the prefab
    /// populate pass, yielding a pending handle exactly like a cooked async load — the
    /// renderer query (Transform, MeshRenderer) and every draw path read the one Mesh
    /// handle regardless of which source produced it.
    struct MeshRenderer
    {
        /// @brief The resolved mesh this entity draws.
        ///
        /// Holds the cooked mesh when Source is empty, or the built recipe mesh when
        /// Source carries a shape. The renderer query and draw paths read this handle.
        AssetHandle<Mesh> Mesh;
        /// @brief Inline procedural source for Mesh, or empty to use the cooked Mesh as authored.
        ///
        /// The active alternative is the primitive kind, carrying that kind's parameters
        /// and material. When non-empty it is built into Mesh at spawn/edit through the
        /// ordinary async load path, replacing any authored cooked Mesh; an empty Source
        /// leaves the authored cooked Mesh in place.
        MeshSource Source;
    };

    /// @brief How an Animator treats a clip's baked root motion.
    ///
    /// Root motion is the locomotion translation a clip bakes onto its root bone, which would
    /// otherwise slide the whole skeleton out from under the entity. Every mode strips that
    /// translation from the rendered pose; they differ only in where the extracted per-tick
    /// delta goes. Integer values are stable — persisted in prefabs.
    enum class RootMotionMode : u32
    {
        /// @brief Strip the root translation and discard it: the entity stays in place.
        Discard = 0,
        /// @brief Strip it and apply the delta to the entity's own Transform (View-phase, cosmetic).
        Presentation = 1,
        /// @brief Strip it and publish the delta as a RootMotionDelta for a Sim mover to consume.
        Drive = 2,
    };

    /// @brief Plays an Animation clip on a skinned-mesh entity.
    ///
    /// The animation system advances Time each tick (when Playing), samples Clip against the
    /// entity mesh's Skeleton, and writes the result into the entity's SkinnedPose for the
    /// renderer to upload. A skinned mesh with no Animator shows its bind pose.
    struct Animator
    {
        /// @brief The animation clip to play.
        AssetHandle<Animation> Clip;
        /// @brief Current playback time in seconds (advanced by the system).
        f32 Time = 0.0f;
        /// @brief Playback rate multiplier.
        f32 Speed = 1.0f;
        /// @brief Whether playback loops at the clip's end.
        bool Loop = true;
        /// @brief Whether playback is advancing.
        bool Playing = true;
        /// @brief How the clip's baked root motion is handled.
        RootMotionMode RootMotion = RootMotionMode::Discard;
    };

    /// @brief Runtime-only skinning palette for a skinned-mesh entity.
    ///
    /// Holds the bone matrices the animation system computes each tick and the renderer
    /// uploads into the GPU skinning palette. Never serialized (a derived, per-frame product);
    /// added automatically to a skinned entity by the animation system.
    struct SkinnedPose
    {
        /// @brief Per-bone skinning matrices (GlobalInverse * modelBone * InverseBind).
        vector<mat4> Skinning;
    };

    /// @brief This tick's root-motion displacement, published by an Animator in Drive mode.
    ///
    /// Written by the View-phase animation system each tick for an Animator whose RootMotion is
    /// Drive, in the entity's model-local frame (the character's own forward/right/up). A
    /// Sim-phase mover rotates it by the entity's orientation and adds it to the entity's
    /// position. Because the producer runs in the View phase after the Sim phase, a Sim consumer
    /// reads the value one tick late. Never serialized (a derived, per-frame product).
    struct RootMotionDelta
    {
        /// @brief Model-local translation extracted from the root bone this tick.
        vec3 Translation{0.0f};
    };

    /// @brief Selects how the deferred lighting pass attenuates a light.
    ///
    /// Directional has no position or falloff. Point and Spot are placed by the
    /// entity's Transform and fall off with distance (Spot adds a cone). Integer
    /// values are stable — packed into the light SSBO and persisted in prefabs.
    enum class LightType : u32
    {
        /// @brief Infinite directional light; no position or falloff.
        Directional = 0,
        /// @brief Omnidirectional point light; falls off within Range.
        Point = 1,
        /// @brief Cone spot light; falls off within Range and the cone angles.
        Spot = 2,
    };

    /// @brief Light component shaded by the deferred lighting pass.
    ///
    /// Type selects directional, point, or spot. Direction is the world-space
    /// travel direction (directional and spot); Color is linear RGB; Intensity
    /// scales it. Range is the point/spot falloff radius. InnerCone/OuterCone
    /// are the spot's half-angles in radians: full intensity within InnerCone,
    /// zero beyond OuterCone, smooth between.
    ///
    /// The light's world position comes from the entity's Transform — never stored
    /// here — so a parented or animated light moves with its entity.
    struct Light
    {
        /// @brief Light shape.
        LightType Type{LightType::Directional};
        /// @brief World-space travel direction (directional and spot).
        vec3 Direction{0.0f, -1.0f, 0.0f};
        /// @brief Linear RGB color.
        vec3 Color{1.0f, 1.0f, 1.0f};
        /// @brief Scales the color at full brightness.
        f32 Intensity{1.0f};
        /// @brief Falloff radius for point and spot lights.
        f32 Range{10.0f};
        /// @brief Spot inner half-angle in radians; full intensity within.
        f32 InnerCone{0.0f};
        /// @brief Spot outer half-angle in radians; zero intensity beyond.
        f32 OuterCone{0.5f};
    };

    /// @brief Per-player input snapshot — the serializable control chokepoint.
    ///
    /// Captures this tick's control state for one player: movement axes, look axes,
    /// and a button bitset. For a local player it is filled each tick from the engine
    /// Veng::Input service; a remote player's would be filled from the wire. It is a
    /// component snapshot rather than a direct device read so the downstream control
    /// system runs identically regardless of where the input originated — the seam a
    /// net layer serializes and replays.
    struct PlayerInput
    {
        /// @brief Movement axes this tick: X strafe, Y vertical, Z forward, each in [-1, 1].
        vec3 Move{0.0f};
        /// @brief Look axes this tick: X yaw, Y pitch, in arbitrary device units.
        vec2 Look{0.0f};
        /// @brief Pressed-button bitset; bit meanings are game policy.
        u32 Buttons = 0;
    };

    /// @brief Abstract, source-agnostic command for what a pawn wants to do this tick.
    ///
    /// The interface between "who decides" (player, AI, remote) and "what happens"
    /// (movement and gameplay systems). A control or AI system writes it; the movement
    /// system consumes it. It is overwritten each tick by its producer, so a zero Intent
    /// is a pawn at rest. Move is expressed in the pawn's local frame.
    struct Intent
    {
        /// @brief Desired move direction in the pawn's local frame: X right, Y up, Z forward.
        vec3 Move{0.0f};
        /// @brief Desired look delta this tick: X yaw, Y pitch, in radians-scaling units.
        vec2 Look{0.0f};
        /// @brief Action-flag bitset (jump/fire/...); bit meanings are game policy.
        u32 Actions = 0;
    };

    /// @brief Seat-to-pawn link: names the pawn entity a player/seat controls.
    ///
    /// Possession is just this reference — nothing inherits or owns through it, and it
    /// is independent of the seat's Viewer.Camera (a spectator views without possessing;
    /// a cutscene retargets the camera without un-possessing). The Pawn field is a
    /// reflected Entity reference, so it remaps on prefab spawn like any intra-prefab
    /// reference.
    struct Possesses
    {
        /// @brief The pawn entity this seat controls.
        Entity Pawn = Entity::Null;
    };

    /// @brief Per-pawn movement tuning the movement system scales its integration by.
    ///
    /// Authored data so a pawn's feel is tunable. A pawn without a Mover moves at the
    /// component's default speeds.
    struct Mover
    {
        /// @brief Local-space move speed in units per second.
        f32 MoveSpeed = 4.0f;
        /// @brief Look/turn speed scaling the Intent's look delta, in radians per unit.
        f32 TurnSpeed = 2.0f;
    };

    /// @brief Frame a ConstantMotion's velocities are applied in.
    ///
    /// Integer values are stable — persisted in prefabs.
    enum class MotionSpace : u32
    {
        /// @brief Apply in the entity's own frame: linear velocity along its local axes,
        ///        rotation about its local axis (post-multiplied onto the orientation).
        Local = 0,
        /// @brief Apply in the parent frame: linear velocity along the parent axes,
        ///        rotation about the parent axis (pre-multiplied onto the orientation).
        World = 1,
    };

    /// @brief Constant per-tick transform velocity: drifts and/or spins an entity at a fixed rate.
    ///
    /// Authored data the ConstantMotionSystem integrates each tick — a rate of change of the
    /// Transform, not a curve. Both velocities are zero by default, so a drift-only or
    /// spin-only entity sets just the one it needs. AngularVelocity is an axis-angle vector:
    /// its direction is the spin axis and its magnitude is the angular speed in radians per
    /// second (a zero vector does not spin). Space selects whether the velocities are applied
    /// in the entity's local frame or its parent frame.
    struct ConstantMotion
    {
        /// @brief Linear velocity in units per second.
        vec3 LinearVelocity = vec3(0.0f);
        /// @brief Angular velocity as an axis-angle vector: direction is the spin axis,
        ///        magnitude is radians per second.
        vec3 AngularVelocity = vec3(0.0f);
        /// @brief Frame the velocities are applied in.
        MotionSpace Space = MotionSpace::World;
    };

    /// @brief Who simulates and owns an entity: the ownership tier of an Authority.
    ///
    /// The minimal set ahead of a net layer. Integer values are stable — persisted in
    /// prefabs. A net layer that introduces predicted or remote ownership extends the
    /// enum; the two tiers here commit to no replication strategy.
    enum class Tier : u32
    {
        /// @brief Server-authoritative: the replicated, deterministic owner.
        Server = 0,
        /// @brief Client-local: never replicated, derived per client (view entities).
        Local = 1,
    };

    /// @brief Ownership annotation marking who simulates an entity, ahead of the net layer.
    ///
    /// Threaded onto entities with sensible defaults (authored entities are Server;
    /// client-local view entities like cameras are Local) and read by nothing in the
    /// runtime — its consumer is the future net layer. It is cheap to thread now and
    /// expensive to retrofit across every spawn site later, so the defaulting discipline
    /// is locked in early. It commits to no replication strategy.
    struct Authority
    {
        /// @brief The ownership tier.
        Tier Tier{Tier::Server};
        /// @brief Owning connection/player id; meaning is net-layer policy, 0 by default.
        u32 Owner = 0;
    };

    /// @brief Camera-rig follow relationship: the target a camera entity trails and how.
    ///
    /// Read by the View-phase camera rig: each tick it reads the target's world Transform
    /// and writes the camera entity's Transform to trail it by Offset (the target's local
    /// frame), optionally smoothed by Damping. Target is a reflected Entity reference, so
    /// it remaps on prefab spawn like any intra-prefab reference. Because the rig runs in
    /// the View phase, the produced camera pose is purely local — never authoritative,
    /// never on the wire.
    struct CameraFollow
    {
        /// @brief The entity whose world Transform the camera trails, or Entity::Null for no follow.
        Entity Target = Entity::Null;
        /// @brief Position offset from the target, expressed in the target's local frame.
        vec3 Offset{0.0f, 5.0f, 10.0f};
        /// @brief Exponential-smoothing rate per second; 0 snaps the camera to the target each tick.
        f32 Damping = 0.0f;
        /// @brief Orbit pitch about the target, in radians.
        ///
        /// Tilts the camera up and down around the target without rotating the target. This is
        /// runtime view state driven by look input — not authored and not serialized; it starts
        /// at zero on spawn.
        f32 Pitch = 0.0f;
    };

    /// @brief Lifecycle phase of a game-mode Session.
    ///
    /// A rule system reads the phase to decide when to act (spawn the player on
    /// entering Playing, tear down on Ended) and writes it to advance the mode.
    /// Integer values are stable — persisted in prefabs.
    enum class SessionPhase : u32
    {
        /// @brief The mode has not begun; rule systems wait.
        NotStarted = 0,
        /// @brief The mode is live; rule systems run and the timer advances.
        Playing = 1,
        /// @brief The mode has finished; rule systems tear down.
        Ended = 2,
    };

    /// @brief The game mode's replicated state, held on a well-known session entity.
    ///
    /// A game mode is rule systems over this state plus a GameModeConfig naming the
    /// rule set's data — no object, no registry. The rule systems read and write the
    /// phase, accumulate the elapsed timer, and track score. It is server-authoritative
    /// (it carries Authority::Server); a future net layer replicates it. Today it is
    /// plain scene data.
    struct Session
    {
        /// @brief The mode's lifecycle phase; an authored session typically begins in Playing.
        SessionPhase Phase{SessionPhase::NotStarted};
        /// @brief Seconds elapsed since the mode entered Playing; accumulated by a rule system.
        f32 Elapsed = 0.0f;
        /// @brief Mode score / objective counter; meaning is game policy.
        i32 Score = 0;
    };

    /// @brief Per-scene game-mode configuration: the data a scene names to pick its mode.
    ///
    /// Held on the session entity beside Session. Names the player prefab a spawn rule
    /// instantiates and the mode parameters the rule systems read. Selecting a different
    /// mode is choosing a different config plus a different registered rule set — no C++
    /// path picks the mode. Its authored JSON key is "gameMode".
    struct GameModeConfig
    {
        /// @brief The player prefab a spawn rule instantiates when the session enters Playing.
        AssetHandle<Prefab> PlayerPrefab;
        /// @brief Score required to end the mode; a win-condition rule reads it.
        i32 ScoreToWin = 0;
    };

    /// @brief Level-scoped render settings: a first-cut subset of the app's render knobs.
    ///
    /// Carried on a Level and seeded into the renderer the app drives — a reflected,
    /// tolerantly-serialized struct, not a renderer type, so the renderer stays untouched
    /// and a new field does not invalidate existing level blobs. The app maps these onto
    /// its SceneRendererSettings (the topology toggles) and its per-frame SceneView
    /// (Exposure, the bloom knobs) at load.
    struct LevelRenderSettings
    {
        /// @brief Tonemap exposure fed into the per-frame SceneView.
        f32 Exposure = 1.0f;
        /// @brief Whether the bloom battery is enabled.
        bool Bloom = true;
        /// @brief Bloom composite intensity fed into the per-frame SceneView.
        f32 BloomIntensity = 1.0f;
        /// @brief Whether the directional cascaded-shadow battery is enabled.
        bool Shadows = true;
        /// @brief Whether the SSAO battery is enabled.
        bool AO = true;
        /// @brief The environment map for image-based lighting + skybox; empty for none.
        ///
        /// Resolved as a load-time dependency and pushed into the per-frame SceneView. Its
        /// presence drives image-based lighting; the Skybox toggle drives the skybox pass.
        AssetHandle<Environment> Environment;
        /// @brief Scales the IBL ambient + skybox radiance, fed into the per-frame SceneView.
        f32 EnvironmentIntensity = 1.0f;
        /// @brief Whether the environment renders as the background skybox (a topology toggle).
        bool Skybox = true;
    };
}

VE_ENUM(::Veng::LightType, 0x6B1D62EF4B5A16ULL)
VE_ENUMERATOR(Directional)
VE_ENUMERATOR(Point)
VE_ENUMERATOR(Spot)
VE_ENUM_END();

VE_REFLECT(::Veng::Name, 0xDA40E8FAC8A6DB84ULL)
VE_FIELD(Value, .DisplayName = "Name")
VE_REFLECT_END();

VE_REFLECT(::Veng::Transform, 0x0AB8E30B2F638555ULL)
VE_FIELD(Position, .DisplayName = "Position", .Tooltip = "Local position, parent space")
VE_FIELD(Rotation, .DisplayName = "Rotation")
VE_FIELD(Scale, .DisplayName = "Scale", .Display = {.Min = 0.001})
VE_REFLECT_END();

VE_REFLECT(::Veng::Hierarchy, 0x5C9855E287465C5EULL)
VE_FIELD(Parent, .DisplayName = "Parent", .ReadOnly = true)
VE_REFLECT_END();

VE_REFLECT(::Veng::CubeShape, 0x2B758A3FE238BAA5ULL)
VE_FIELD(Extent, .DisplayName = "Extent", .Display = {.Min = 0.001})
VE_FIELD(Material, .DisplayName = "Material")
VE_REFLECT_END();

VE_REFLECT(::Veng::PlaneShape, 0xE53DEFF5662A295ULL)
VE_FIELD(Size, .DisplayName = "Size", .Display = {.Min = 0.001})
VE_FIELD(Subdivisions, .DisplayName = "Subdivisions", .Display = {.Min = 1})
VE_FIELD(Material, .DisplayName = "Material")
VE_REFLECT_END();

VE_REFLECT(::Veng::SphereShape, 0xCF4BE61837AB5179ULL)
VE_FIELD(Radius, .DisplayName = "Radius", .Display = {.Min = 0.001})
VE_FIELD(Rings, .DisplayName = "Rings", .Display = {.Min = 3})
VE_FIELD(Segments, .DisplayName = "Segments", .Display = {.Min = 3})
VE_FIELD(Material, .DisplayName = "Material")
VE_REFLECT_END();

VE_REFLECT(::Veng::IcosphereShape, 0x7D9FC0055D5978BBULL)
VE_FIELD(Radius, .DisplayName = "Radius", .Display = {.Min = 0.001})
VE_FIELD(Subdivisions, .DisplayName = "Subdivisions", .Display = {.Min = 1})
VE_FIELD(Material, .DisplayName = "Material")
VE_REFLECT_END();

VE_REFLECT(::Veng::CylinderShape, 0x3BB833585C0DAD4CULL)
VE_FIELD(Radius, .DisplayName = "Radius", .Display = {.Min = 0.001})
VE_FIELD(Height, .DisplayName = "Height", .Display = {.Min = 0.001})
VE_FIELD(Segments, .DisplayName = "Segments", .Display = {.Min = 3})
VE_FIELD(Material, .DisplayName = "Material")
VE_REFLECT_END();

VE_REFLECT(::Veng::ConeShape, 0x64D0B47E06329960ULL)
VE_FIELD(Radius, .DisplayName = "Radius", .Display = {.Min = 0.001})
VE_FIELD(Height, .DisplayName = "Height", .Display = {.Min = 0.001})
VE_FIELD(Segments, .DisplayName = "Segments", .Display = {.Min = 3})
VE_FIELD(Material, .DisplayName = "Material")
VE_REFLECT_END();

VE_REFLECT(::Veng::TorusShape, 0xFF864410096026A2ULL)
VE_FIELD(MajorRadius, .DisplayName = "Major Radius", .Display = {.Min = 0.001})
VE_FIELD(MinorRadius, .DisplayName = "Minor Radius", .Display = {.Min = 0.001})
VE_FIELD(MajorSegments, .DisplayName = "Major Segments", .Display = {.Min = 3})
VE_FIELD(MinorSegments, .DisplayName = "Minor Segments", .Display = {.Min = 3})
VE_FIELD(Material, .DisplayName = "Material")
VE_REFLECT_END();

VE_REFLECT(::Veng::CapsuleShape, 0xDECC3D44F06725DCULL)
VE_FIELD(Radius, .DisplayName = "Radius", .Display = {.Min = 0.001})
VE_FIELD(Height, .DisplayName = "Height", .Display = {.Min = 0.001})
VE_FIELD(Segments, .DisplayName = "Segments", .Display = {.Min = 3})
VE_FIELD(Rings, .DisplayName = "Rings", .Display = {.Min = 1})
VE_FIELD(Material, .DisplayName = "Material")
VE_REFLECT_END();

VE_VARIANT(::Veng::MeshSource, 0xC64CE2B415C54D22ULL);

VE_REFLECT(::Veng::MeshRenderer, 0x3C5CB13E46E0450BULL)
VE_FIELD(Mesh, .DisplayName = "Mesh")
VE_FIELD(Source, .DisplayName = "Source")
VE_REFLECT_END();

VE_ENUM(::Veng::RootMotionMode, 0x2F4A31CEE94569AFULL)
VE_ENUMERATOR(Discard)
VE_ENUMERATOR(Presentation)
VE_ENUMERATOR(Drive)
VE_ENUM_END();

VE_REFLECT(::Veng::Animator, 0x2B56DF7335B89F8DULL)
VE_FIELD(Clip, .DisplayName = "Clip")
VE_FIELD(Speed, .DisplayName = "Speed", .Display = {.Min = 0.0})
VE_FIELD(Loop, .DisplayName = "Loop")
VE_FIELD(Playing, .DisplayName = "Playing")
VE_FIELD(RootMotion, .DisplayName = "Root Motion")
VE_FIELD(Time, .DisplayName = "Time", .Display = {.Min = 0.0}, .ReadOnly = true)
VE_REFLECT_END();

VE_TYPE(::Veng::SkinnedPose, 0x063C1245B8912FC3ULL);

VE_TYPE(::Veng::RootMotionDelta, 0x10C7034D936A12CEULL);

VE_REFLECT(::Veng::Light, 0xECF6442708DF7C00ULL)
VE_FIELD(Type, .DisplayName = "Type")
VE_FIELD(Direction, .DisplayName = "Direction")
VE_FIELD(Color, .DisplayName = "Color")
VE_FIELD(Intensity, .DisplayName = "Intensity", .Display = {.Min = 0.0})
VE_FIELD(Range, .DisplayName = "Range", .Display = {.Min = 0.01, .Step = 0.1},
         .VisibleIf = VE_WHEN(self.Type != ::Veng::LightType::Directional))
VE_FIELD(InnerCone, .DisplayName = "Inner Cone",
         .Display = {.Min = 0.0, .Max = 3.14159265, .Step = 0.01},
         .VisibleIf = VE_WHEN(self.Type == ::Veng::LightType::Spot))
VE_FIELD(OuterCone, .DisplayName = "Outer Cone",
         .Display = {.Min = 0.0, .Max = 3.14159265, .Step = 0.01},
         .VisibleIf = VE_WHEN(self.Type == ::Veng::LightType::Spot))
VE_REFLECT_END();

VE_REFLECT(::Veng::PlayerInput, 0x5401D36B1EF55045ULL)
VE_FIELD(Move, .DisplayName = "Move")
VE_FIELD(Look, .DisplayName = "Look")
VE_FIELD(Buttons, .DisplayName = "Buttons")
VE_REFLECT_END();

VE_REFLECT(::Veng::Intent, 0x27F416122B525965ULL)
VE_FIELD(Move, .DisplayName = "Move")
VE_FIELD(Look, .DisplayName = "Look")
VE_FIELD(Actions, .DisplayName = "Actions")
VE_REFLECT_END();

VE_REFLECT(::Veng::Possesses, 0xC7D4144C7DF95B9BULL)
VE_FIELD(Pawn, .DisplayName = "Pawn")
VE_REFLECT_END();

VE_REFLECT(::Veng::Mover, 0x7774F1C2B00DE07EULL)
VE_FIELD(MoveSpeed, .DisplayName = "Move Speed", .Display = {.Min = 0.0})
VE_FIELD(TurnSpeed, .DisplayName = "Turn Speed", .Display = {.Min = 0.0})
VE_REFLECT_END();

VE_ENUM(::Veng::MotionSpace, 0x46914AC0C743D776ULL)
VE_ENUMERATOR(Local)
VE_ENUMERATOR(World)
VE_ENUM_END();

VE_REFLECT(::Veng::ConstantMotion, 0xEBB74CB78D872F9FULL)
VE_FIELD(LinearVelocity, .DisplayName = "Linear Velocity", .Tooltip = "Units per second")
VE_FIELD(AngularVelocity, .DisplayName = "Angular Velocity",
         .Tooltip = "Axis-angle vector: direction is the spin axis, magnitude is radians/sec")
VE_FIELD(Space, .DisplayName = "Space", .Tooltip = "Local (own frame) or World (parent frame)")
VE_REFLECT_END();

VE_ENUM(::Veng::Tier, 0x45470D3410320AB9ULL)
VE_ENUMERATOR(Server)
VE_ENUMERATOR(Local)
VE_ENUM_END();

VE_REFLECT(::Veng::Authority, 0xA934C4B9009D7735ULL)
VE_FIELD(Tier, .DisplayName = "Tier")
VE_FIELD(Owner, .DisplayName = "Owner")
VE_REFLECT_END();

VE_REFLECT(::Veng::CameraFollow, 0xF8BD924F0A0F9DB0ULL)
VE_FIELD(Target, .DisplayName = "Target")
VE_FIELD(Offset, .DisplayName = "Offset")
VE_FIELD(Damping, .DisplayName = "Damping", .Display = {.Min = 0.0})
VE_REFLECT_END();

VE_ENUM(::Veng::SessionPhase, 0x6DF15084654B59E7ULL)
VE_ENUMERATOR(NotStarted)
VE_ENUMERATOR(Playing)
VE_ENUMERATOR(Ended)
VE_ENUM_END();

VE_REFLECT(::Veng::Session, 0x5EC76128049D9629ULL)
VE_FIELD(Phase, .DisplayName = "Phase")
VE_FIELD(Elapsed, .DisplayName = "Elapsed", .Display = {.Min = 0.0}, .ReadOnly = true)
VE_FIELD(Score, .DisplayName = "Score")
VE_REFLECT_END();

VE_REFLECT(::Veng::GameModeConfig, 0xAE57419CF98B07F8ULL)
VE_FIELD(PlayerPrefab, .DisplayName = "Player Prefab")
VE_FIELD(ScoreToWin, .DisplayName = "Score to Win", .Display = {.Min = 0})
VE_REFLECT_END();

VE_REFLECT(::Veng::LevelRenderSettings, 0x28E4618C66455E21ULL)
VE_FIELD(Exposure, .DisplayName = "Exposure", .Display = {.Min = 0.0})
VE_FIELD(Bloom, .DisplayName = "Bloom")
VE_FIELD(BloomIntensity, .DisplayName = "Bloom Intensity", .Display = {.Min = 0.0})
VE_FIELD(Shadows, .DisplayName = "Shadows")
VE_FIELD(AO, .DisplayName = "SSAO")
VE_FIELD(Environment, .DisplayName = "Environment")
VE_FIELD(EnvironmentIntensity, .DisplayName = "Environment Intensity", .Display = {.Min = 0.0})
VE_FIELD(Skybox, .DisplayName = "Skybox")
VE_REFLECT_END();
