#pragma once

#include <Veng/Veng.h>
#include <Veng/Input.h>
#include <Veng/Input/Actions.h>
#include <Veng/Renderer/Atmosphere.h>
#include <Veng/Renderer/DofTile.h>
#include <Veng/Renderer/PointField.h>
#include <Veng/Renderer/SunPosition.h>
#include <Veng/Renderer/VolumeField.h>
#include <Veng/Renderer/Tonemapper.h>
#include <Veng/Net/AccountId.h>
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
    class EnvironmentMap;
    class InputMappingContext;
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
        /// @brief Whether this mesh is rendered into the shadow maps (and bounds the shadow fit).
        ///
        /// True (the default) draws the mesh as a caster in every shadow view and folds its world
        /// bound into the caster bound the shadow projections fit to. False excludes it from both:
        /// the mesh still draws in the camera view but casts no shadow and never extends a shadow
        /// frustum. Set it false for geometry that must not occlude — an emissive body co-located
        /// with its own light, a skybox proxy, a held first-person prop.
        bool CastsShadows = true;
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
    /// entity's Transform and fall off with distance (Spot adds a cone). Rect,
    /// Sphere, and Polygon are area lights: their emission is integrated over a
    /// finite shape (a linearly-transformed-cosine evaluation of the GGX BRDF), so
    /// they produce a soft, physically-sized specular highlight and penumbra rather
    /// than a punctual point response. Integer values are stable — packed into the
    /// light SSBO and persisted in prefabs.
    enum class LightType : u32
    {
        /// @brief Infinite directional light; no position or falloff.
        Directional = 0,
        /// @brief Omnidirectional point light; falls off within Range.
        Point = 1,
        /// @brief Cone spot light; falls off within Range and the cone angles.
        Spot = 2,
        /// @brief Rectangular area light in the entity's local XY plane (Width × Height).
        Rect = 3,
        /// @brief Spherical area light of the given Radius; correct at any angular size.
        Sphere = 4,
        /// @brief Convex-polygon area light from PolygonVertices in the entity's local frame.
        Polygon = 5,
    };

    /// @brief Light component shaded by the deferred lighting pass.
    ///
    /// Type selects the light's shape. Direction is the world-space travel
    /// direction (directional and spot); Color is linear RGB; Intensity scales it.
    /// Range is the falloff radius for every positioned light (point, spot, and the
    /// area lights). InnerCone/OuterCone are the spot's half-angles in radians: full
    /// intensity within InnerCone, zero beyond OuterCone, smooth between.
    ///
    /// The area shapes are placed and oriented by the entity's Transform: a Rect
    /// spans Width × Height in the local XY plane and emits along local +Z; a Sphere
    /// has the given Radius; a Polygon takes its vertices from PolygonVertices in the
    /// local frame. TwoSided lets a Rect or Polygon emit from both faces.
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
        /// @brief Falloff radius for positioned lights (point, spot, and area).
        f32 Range{10.0f};
        /// @brief Spot inner half-angle in radians; full intensity within.
        f32 InnerCone{0.0f};
        /// @brief Spot outer half-angle in radians; zero intensity beyond.
        f32 OuterCone{0.5f};
        /// @brief Full width of a Rect area light along the entity's local X axis.
        f32 Width{1.0f};
        /// @brief Full height of a Rect area light along the entity's local Y axis.
        f32 Height{1.0f};
        /// @brief Radius of a Sphere area light.
        f32 Radius{1.0f};
        /// @brief Whether a Rect or Polygon area light emits from both faces.
        bool TwoSided{false};
        /// @brief Convex polygon vertices in entity-local space, wound CCW about local +Z (Polygon).
        vector<vec3> PolygonVertices;
    };

    /// @brief Per-seat resolved input for this tick — the serializable action snapshot.
    ///
    /// The resolved values of the seat's active actions this tick (see Veng/Input/Actions.h),
    /// produced by InputMappingSystem from the seat's InputContextStack. It is the control
    /// chokepoint a net layer replicates and the uniform surface a control system reads by
    /// action id — filled locally for an owned seat, from the wire for a remote one. It
    /// serializes through the reflection serializer's name-keyed FieldClass::Array encoding
    /// for its ActionState, each sample self-describing by its ActionId; there is no bespoke
    /// wire format. The Get*/Was* methods delegate to State, a read-through convenience so a
    /// control system reads input.WasTriggered(Actions::Jump) directly.
    struct PlayerInput
    {
        /// @brief The resolved action set for the seat this tick.
        ActionState State;

        /// @brief Resolved value of an action, or zero when absent.
        /// @param id  The action to look up.
        /// @return The action's Value, or a zero vector when absent.
        [[nodiscard]] vec2 GetValue(ActionId id) const { return State.GetValue(id); }

        /// @brief Resolved x component of an action, the 1D-axis convenience.
        /// @param id  The action to look up.
        /// @return The action's Value.x, or zero when absent.
        [[nodiscard]] f32 GetAxis(ActionId id) const { return State.GetAxis(id); }

        /// @brief Whether an action is currently active (Started or Ongoing).
        /// @param id  The action to look up.
        /// @return True while the action is held.
        [[nodiscard]] bool IsHeld(ActionId id) const { return State.IsHeld(id); }

        /// @brief Whether an action became active this tick (Started).
        /// @param id  The action to look up.
        /// @return True on the tick the action activated.
        [[nodiscard]] bool WasTriggered(ActionId id) const { return State.WasTriggered(id); }

        /// @brief Whether an action was released this tick (Completed).
        /// @param id  The action to look up.
        /// @return True on the tick the action released.
        [[nodiscard]] bool WasReleased(ActionId id) const { return State.WasReleased(id); }

        /// @brief Whether an action started on any Sim tick since this frame began.
        ///
        /// The query a once-per-frame reader (a View system) uses so a Started edge on a non-final
        /// tick of a multi-tick frame is not lost; a per-tick Sim system uses WasTriggered.
        /// @param id  The action to look up.
        /// @return True if the action activated on any tick this frame.
        [[nodiscard]] bool WasTriggeredThisFrame(ActionId id) const
        {
            return State.WasTriggeredThisFrame(id);
        }

        /// @brief Whether an action released on any Sim tick since this frame began.
        ///
        /// The once-per-frame release query; the companion to WasTriggeredThisFrame.
        /// @param id  The action to look up.
        /// @return True if the action released on any tick this frame.
        [[nodiscard]] bool WasReleasedThisFrame(ActionId id) const
        {
            return State.WasReleasedThisFrame(id);
        }
    };

    /// @brief The ordered active input contexts for a seat, highest priority last.
    ///
    /// InputMappingSystem resolves these against the raw snapshot into the seat's PlayerInput.
    /// Gameplay systems push/pop entries to switch schemes (enter a vehicle, open a modal). The
    /// fine-grained, per-seat sibling of the InputRouter's coarse focus stack.
    ///
    /// Each entry is a cooked InputMappingContext referenced by AssetId — authored on a prefab
    /// and resolved as an ordinary load-time dependency, so a seat's base scheme is data. A
    /// not-yet-resident handle resolves to empty actions until it streams in.
    struct InputContextStack
    {
        /// @brief The active contexts, lowest priority first; resolved as a stack each tick.
        vector<AssetHandle<InputMappingContext>> Active;
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
        /// @brief Desired rotational command this tick: X yaw, Y pitch, Z roll, in radians-scaling units.
        vec3 Look{0.0f};
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

    /// @brief The physical devices that feed one local seat's input this session.
    ///
    /// Lives on the Viewer entity beside Possesses and InputContextStack. InputMappingSystem
    /// builds a per-seat filtered view (SeatInputView) from it — gamepad arms scoped to Gamepad,
    /// keyboard/mouse arms present only when UsesKeyboardMouse — so two seats with different
    /// assignments resolve to distinct PlayerInputs. Authorable in a level and serialized through
    /// the reflection path. A seat with no SeatInput takes no local device input:
    /// InputMappingSystem skips it, so its PlayerInput is synthesized or replicated (the AI/remote
    /// path).
    struct SeatInput
    {
        /// @brief Whether this seat reads the keyboard (and, region-gated elsewhere, the pointer).
        bool UsesKeyboardMouse = true;
        /// @brief The pad slot assigned to this seat, or GamepadId::None for no pad.
        GamepadId Gamepad = GamepadId::None;
        /// @brief Whether DeviceAssignmentSystem auto-fills the Gamepad slot when a pad connects.
        ///
        /// Independent of UsesKeyboardMouse: the ordinary single-player seat opts into both a
        /// keyboard and an auto-assigned pad. The flag fills a None Gamepad slot on connect; a
        /// level-authored slot is left untouched.
        bool WantsGamepad = false;
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
    /// Integer values are stable — persisted in prefabs. Server and Local are the two authored tiers;
    /// Remote and Predicted are runtime client-side stances a replicated entity carries. Remote is the
    /// marking every replicated entity arrives from the wire with; Predicted is the promotion the
    /// client applies to the entities its own seat controls, so it re-runs the real Sim systems for
    /// them locally. Only Server and Local are ever authored or persisted; Remote and Predicted are
    /// each peer's stance toward an entity, never replicated.
    enum class Tier : u32
    {
        /// @brief Server-authoritative: the replicated, deterministic owner.
        Server = 0,
        /// @brief Client-local: never replicated, derived per client (view entities).
        Local = 1,
        /// @brief Client-side mirror of a server-owned entity: never simulated, interpolated for display.
        ///
        /// Stamped by the client's replication layer on every entity that arrives from the spawn
        /// stream. A Remote entity's state is server truth applied latest-wins; its Transform is
        /// presentation written by the View-phase remote-interpolation system, never authoritative.
        Remote = 2,
        /// @brief Client-side prediction stance over a server-owned entity the local seat controls.
        ///
        /// The client promotes the entities its own seat controls (its pawn and any attached subtree
        /// carrying replicated state) from Remote to Predicted, then re-runs the real Sim systems
        /// (control + movement) for them each client tick ahead of the server — so the local pawn
        /// responds on the tick its input is sampled. It is a client-side-only stance: a server never
        /// holds it, it is never replicated (tier is each peer's stance, not wire state), never
        /// authored (prefab/level data carries Server/Local only), and never persisted by the
        /// serializer. The authoritative snapshot corrects its accumulated prediction error.
        Predicted = 3,
    };

    /// @brief Ownership annotation marking who simulates an entity.
    ///
    /// Threaded onto entities with sensible defaults (authored entities are Server;
    /// client-local view entities like cameras are Local; an absent component defaults
    /// to Server). The net layer is its consumer: HasAuthority gates the authoritative
    /// Sim advancers by tier against the peer's NetRole, the replication server selects
    /// Server-tier entities for the snapshot stream, the hosts stamp each connection's
    /// seat with its Owner id, and the client marks replicated arrivals Remote.
    struct Authority
    {
        /// @brief The ownership tier.
        Tier Tier{Tier::Server};
        /// @brief Owning connection id (server-assigned; see NetEvents.h); 0 = server/none.
        u32 Owner = 0;
    };

    /// @brief Wire identity of a replicated entity: a server-assigned id the two ends agree on.
    ///
    /// The net layer's key for an entity across the wire. The authority owning an entity assigns it
    /// a fresh Id from a monotonic counter (never reused, never zero — zero is the null-reference
    /// sentinel on the wire), and the displaying end keeps its own NetId → Entity map, rebuilt from
    /// the NetIdentity components on the entities it spawns. It is runtime-only: server-assigned at
    /// spawn, never authored in a prefab and never persisted (like SkinnedPose/RootMotionDelta, it
    /// carries no on-disk lifetime), so it never appears in a prefab source. It is *not* itself
    /// replicated (VE_REPLICATED) — it *is* the wire key, carried in each snapshot's per-entity
    /// header rather than as a replicated component. It is reflected so the inspector can surface
    /// the assigned id (read-only — a consumer never edits it).
    struct NetIdentity
    {
        /// @brief The server-assigned wire id; 0 until assigned (and the wire's null-reference value).
        u32 Id = 0;
    };

    /// @brief An opaque 128-bit stable identity binding a replicated entity to its live local twin.
    ///
    /// The WorldKey discipline applied to entities: content derived on *both* peers that also carries
    /// server-authoritative dynamic state names itself with an anchor the consumer mints (from its own
    /// stable ids), and the engine never interprets the two halves. An authoritative entity carrying a
    /// NetAnchor replicates it in its spawn record; on the displaying peer the net layer resolves a
    /// live local entity carrying the equal anchor (its claimant) and binds the wire id to it — applying
    /// the replicated state onto the derived entity instead of spawning a duplicate. A claimant-less
    /// anchored spawn falls back to an ordinary wire-owned spawn. Reflected so a consumer may author or
    /// inspect it; not itself replicated (VE_REPLICATED) — it rides the spawn record, read before any
    /// entity is created, so the claimant is resolvable at spawn time rather than a snapshot later.
    struct NetAnchor
    {
        /// @brief Low 64 bits of the opaque anchor id.
        u64 Lo = 0;
        /// @brief High 64 bits of the opaque anchor id.
        u64 Hi = 0;

        /// @brief Equality over both halves — two entities share an anchor iff both halves match.
        [[nodiscard]] bool operator==(const NetAnchor&) const = default;
    };

    /// @brief The account a seat entity belongs to, stamped by the server when it spawns the seat.
    ///
    /// Server-local bookkeeping: the host stamps each spawned seat with the connection's admitted
    /// account so server-side game code can key player decisions on the person, not the transport
    /// link. Deliberately not replicated — the account id is never broadcast to world members; a
    /// game wanting a public identity replicates its own display component instead. Runtime-only,
    /// never authored (the account exists only once a connection is admitted).
    struct SeatAccount
    {
        /// @brief The seat's owning account (valid — admission precedes the seat spawn).
        Net::AccountId Account;
    };

    /// @brief The prefab an entity was spawned from, stamped on each spawned root by Prefab::SpawnInto.
    ///
    /// Spawn provenance: it ties a live entity back to the recipe that produced it, which nothing
    /// else records once a spawn has run. The net layer is its consumer — a host-authoritative
    /// entity carrying provenance *and* the NetSpawn marker has that prefab associated with its wire
    /// id, so a joiner instantiates it through the ordinary prefab path.
    ///
    /// Stamped only for a prefab loaded by AssetId; a runtime-built prefab is not addressable, so it
    /// records nothing. Provenance is derivable state, so it is registered without a reflected
    /// field: it never serializes into a prefab or a save and never rides the wire. It is inert on
    /// its own — carrying it changes no behavior anywhere.
    struct PrefabSource
    {
        /// @brief The prefab this entity was spawned from (always valid where the component exists).
        AssetId Prefab;
    };

    /// @brief Opt-in mark: replicate this locally-created host entity as a spawn of its source prefab.
    ///
    /// A server-spawned seat gets a prefab association for free, so a joiner instantiates it as a
    /// real prefab spawn. An entity a game spawns itself gets none, so its Spawn rides the
    /// runtime-constructed arm and a joiner receives only its replicated leaves — no prefab
    /// structure, no non-replicated components. Adding this marker beside the entity's PrefabSource
    /// closes that asymmetry: while the world is a live replication instance, the host associates
    /// the recorded prefab with the entity's wire id each pump, so an entity marked before
    /// replication begins is picked up when it does.
    ///
    /// **The mark is opt-in, and deliberately so.** Authority::Tier defaults to Server and every
    /// prefab spawn records provenance, so an authority-plus-provenance rule would enroll every
    /// host-local prop, effect, and editor-placed fixture a game meant to stay local, each at a
    /// per-connection bookkeeping cost and with no opt-out. Only marked entities are enrolled.
    ///
    /// It names *how* an entity replicates, never *whether*: Authority still decides that, so a
    /// Local-tier entity carrying the mark stays unreplicated. Carries no field and, being authored
    /// intent rather than derived state, is safe to author on a prefab.
    struct NetSpawn
    {
    };

    /// @brief Marks the pawn a presenting viewport's own seat controls on this machine.
    ///
    /// The engine's answer to "which pawn is mine?", derived in two steps that are both required:
    /// **a presenting viewport → the seat bound to it → that seat's Possesses → the pawn**. The
    /// first step is the one that cannot be dropped: on a client every mirrored pawn carries its
    /// own instantiated Tier::Local seat, so "possessed by a local seat" would mark every pawn on
    /// screen. Only the seat a viewport actually presents through yields a marker, so a client
    /// marks exactly its own pawn, a listen host marks the pawn its local seat controls, and a
    /// dedicated host — which presents nothing — marks nothing.
    ///
    /// The marker is singular per presenting viewport and carries the seat it was derived from, so
    /// under split-screen each viewport reads back its own pawn: a consumer resolves its viewport's
    /// seat and asks ResolveLocalControlledPawn (Veng/Scene/LocalControl.h) rather than treating a
    /// bare "some entity is marked" as its own. A flat every-locally-possessed-pawn marker is not
    /// what this is.
    ///
    /// Never replicated and never persisted — it is per-process presentation state, meaningless off
    /// the machine that derived it, so it carries no reflected field and rides no snapshot or save.
    /// @warning A consumer only ever **reads** this marker. The engine owns its whole lifecycle
    ///          (ReconcileLocalControl); adding, removing, or editing it in application code is
    ///          misuse and is overwritten at the next reconcile.
    /// @warning Viewport↔seat binding changes stamp it eagerly, but a possession change does not:
    ///          Possesses is a plain component a game writes directly and, on a client, one that
    ///          arrives through snapshot apply — neither raises an engine-side event. That case,
    ///          and only that case, is covered by a reconciling sweep the engine runs once per
    ///          frame over the presenting viewports (never a scan of the scene's entities), so the
    ///          marker reflects the possession state the frame renders.
    struct LocalControl
    {
        /// @brief The presenting viewport's seat whose Possesses named this pawn.
        Entity Seat = Entity::Null;
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

    /// @brief Camera-rig first-person look: a yaw/pitch heading the rig writes as the
    /// entity's rotation.
    ///
    /// Read by the View-phase camera rig: each tick it clamps Pitch into ±PitchLimit and
    /// writes the entity's Transform rotation as yaw about world up composed with pitch
    /// about the yawed right axis. The entity's position is untouched — the camera sits
    /// where it is placed (or parented). A game's control system drives Yaw/Pitch from its
    /// look action with its own sensitivity, the way it drives CameraFollow::Pitch; the
    /// authored values are the spawn-time facing. Because the rig runs in the View phase,
    /// the produced rotation is purely local — never authoritative, never on the wire.
    struct CameraLook
    {
        /// @brief Heading about world up, in radians; positive turns left.
        f32 Yaw = 0.0f;
        /// @brief Elevation about the yawed right axis, in radians; positive looks up.
        f32 Pitch = 0.0f;
        /// @brief Maximum |Pitch| in radians; the rig clamps Pitch into this range each tick.
        f32 PitchLimit = 1.5f;
    };

    /// @brief Camera-rig orbit relationship: a camera that circles a focus point at a distance.
    ///
    /// Read by the View-phase camera rig: each tick it places the camera entity a Distance from
    /// Focus along the Yaw/Pitch heading and orients it back at Focus. Unlike CameraFollow, the
    /// orbit tracks a point rather than a target entity, so it is the rig any scene inspector,
    /// model viewer, or map/chart view uses. FocusTarget with a positive FocusDamping glides
    /// Focus toward the target over time, so recentering the view is smooth rather than an
    /// instant cut; a zero FocusDamping snaps Focus to FocusTarget each tick. Distance is held
    /// within [MinDistance, MaxDistance] and Pitch within ±PitchLimit — the pitch clamp keeps the
    /// pose off the pole where the look-at up vector collapses. Because the rig runs in the View
    /// phase, the produced camera pose is purely local — never authoritative, never on the wire.
    struct CameraOrbit
    {
        /// @brief The point orbited, in world space.
        vec3 Focus{0.0f, 0.0f, 0.0f};
        /// @brief Eye distance from Focus, clamped into [MinDistance, MaxDistance].
        f32 Distance = 10.0f;
        /// @brief Smallest allowed Distance, so the eye never reaches the focus.
        f32 MinDistance = 0.1f;
        /// @brief Largest allowed Distance.
        f32 MaxDistance = 1000.0f;
        /// @brief Heading about world up, in radians; positive turns the eye left around Focus.
        f32 Yaw = 0.0f;
        /// @brief Elevation about the yawed right axis, in radians; positive swings the eye
        ///        below Focus so the camera tilts up toward it (matching CameraLook::Pitch).
        f32 Pitch = 0.0f;
        /// @brief Maximum |Pitch| in radians; the rig clamps Pitch off the degenerate pole.
        f32 PitchLimit = 1.5f;
        /// @brief Where Focus glides toward when FocusDamping is positive, in world space.
        vec3 FocusTarget{0.0f, 0.0f, 0.0f};
        /// @brief Exponential-smoothing rate per second of Focus toward FocusTarget; 0 snaps.
        f32 FocusDamping = 0.0f;
    };

    /// @brief Camera-rig first-person relationship: an eye-anchored camera that yaws about the
    ///        target's up rather than the world's.
    ///
    /// Read by the View-phase camera rig: each tick it reads the target's resolved up — its
    /// CharacterState::Up when the target carries one and is not Seated, else the target's own
    /// transform up — and builds the camera basis against it, so the horizon stays level even where
    /// "up" is not a world constant (a curved habitat, a rotating deck). A seated target reads its
    /// own transform because its frame is the seat's, not the one it was last standing in; its
    /// CharacterState is not removed on entry and stops being advanced, so following it would hold
    /// the horizon to the ground the occupant walked in on. The eye sits at the target's position
    /// offset by EyeOffset in the target's local frame, or — when EyeSocket names a mesh socket on
    /// the target — at that socket's world position with EyeOffset applied on top. The heading is
    /// read from a sibling CameraLook (its accumulated Yaw about the up axis and Pitch about the
    /// horizon), so a control system drives look input into CameraLook exactly as it does for the
    /// plain look rig; the rig clamps that pitch into [MinPitch, MaxPitch]. Because the rig runs in
    /// the View phase, the produced camera pose is purely local — never authoritative, never on the
    /// wire.
    ///
    /// Yaw is a heading *relative to the target's forward*, resolved against the target's up each
    /// tick rather than stored as an absolute direction — an absolute forward would need
    /// re-projecting every tick and would drift as the up changed. An entity carrying a
    /// FirstPersonRig is skipped by the plain CameraLook arm, so the two never both write the pose.
    struct FirstPersonRig
    {
        /// @brief The entity the camera looks out of; its up and forward drive the basis.
        Entity Target = Entity::Null;
        /// @brief Eye position offset, in the target's (or the named socket's) local frame.
        vec3 EyeOffset{0.0f, 1.6f, 0.0f};
        /// @brief Optional mesh socket on the target naming the eye anchor; empty uses EyeOffset alone.
        string EyeSocket;
        /// @brief Smallest allowed pitch about the horizon, in radians; negative looks down.
        f32 MinPitch = -1.4f;
        /// @brief Largest allowed pitch about the horizon, in radians; positive looks up.
        f32 MaxPitch = 1.4f;
        /// @brief View-bob amplitude in metres; 0 disables the bob.
        f32 BobAmplitude = 0.0f;
        /// @brief View-bob cycles per metre travelled along the ground plane.
        f32 BobFrequency = 1.0f;
        /// @brief Accumulated bob phase, in radians.
        ///
        /// Runtime view state advanced from the target's planar speed each tick — not authored and
        /// not serialized; it starts at zero on spawn.
        f32 BobPhase = 0.0f;
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

    /// @brief Sky source: an environment map drives the background and image-based lighting.
    ///
    /// One alternative of the SkySource variant. The map's radiance cube is the visible sky, and
    /// its convolved maps feed image-based lighting when the Sky component's lighting tier requests
    /// it. An empty Map is a no-op.
    struct EnvironmentSky
    {
        /// @brief The environment map (radiance/irradiance/prefiltered/BRDF) for the sky and IBL.
        AssetHandle<EnvironmentMap> Map;
    };

    /// @brief How a procedural or authored sky reaches the screen and whether it can light the scene.
    ///
    /// Keyed to how the sky changes over time. Direct evaluates the sky per pixel every frame —
    /// right for a sky that animates continuously — and cannot light the scene (a direct sky
    /// displays only). Baked renders the sky into the six faces of a radiance cubemap once per
    /// change and samples that cube per frame through the skybox path — right for the common
    /// static-between-changes case — and is the mode that can light the scene. Integer values are
    /// stable — persisted in prefabs.
    enum class SkyMode : u8
    {
        /// @brief Evaluate the sky per pixel every frame; display-only.
        Direct = 0,
        /// @brief Bake the sky into a radiance cube on change and sample it; can light the scene.
        Baked = 1,
    };

    /// @brief Sky source: a procedural physically-based atmosphere fills the sky.
    ///
    /// One alternative of the SkySource variant. The atmosphere renders the background sky along
    /// each view ray; the toward-sun direction is the inverse of the scene's first directional
    /// Light's travel direction (itself derivable from a TimeOfDay component), so the sky and the
    /// direct lighting share one sun. Mode selects the direct per-pixel path (right for a
    /// continuously-moving sun) or the baked-cube path (bake on the sun/params dirty signal, sample
    /// a cube per frame); the two render the same sky, and only the baked path can light the scene.
    struct AtmosphereSky
    {
        /// @brief Physically-based atmosphere parameters; the renderer regenerates its LUTs on change.
        ///
        /// The defaults describe Earth at sea level.
        Renderer::Atmosphere Params;
        /// @brief Whether the atmosphere is composited per pixel (Direct) or baked to a cube (Baked).
        SkyMode Mode = SkyMode::Baked;
    };

    /// @brief Sky source: an authored Sky-domain material fills the background sky.
    ///
    /// One alternative of the SkySource variant. The material owns its own parameters and any
    /// buffers/textures it reads; the engine supplies the view ray and the g-buffer depth mask.
    /// An empty Material is a no-op. Mode selects the direct per-pixel path or the baked-cube path;
    /// the two render the same sky, and the author picks per the sky's dynamics.
    struct MaterialSky
    {
        /// @brief The Sky-domain material instance rendered as the background sky.
        AssetHandle<MaterialInstance> Material;
        /// @brief Whether the material is composited per pixel (Direct) or baked to a cube (Baked).
        SkyMode Mode = SkyMode::Baked;
    };

    /// @brief The active source of a Sky component: environment map, atmosphere, or material.
    ///
    /// The active alternative selects the sky kind and carries that kind's parameters. Empty means
    /// no sky (the flat-ambient fallback). The renderer resolves the active alternative per frame
    /// and recompiles its own pass set when the source kind changes.
    using SkySource = Variant<EnvironmentSky, AtmosphereSky, MaterialSky>;

    /// @brief How the sky lights the scene, beyond displaying it.
    ///
    /// The lighting tier of a Sky component: None displays the sky without lighting the scene, SH
    /// projects it to a spherical-harmonic ambient (the cheap diffuse arm), and IBL runs the full
    /// split-sum image-based lighting. A tier is a request — the renderer activates it per source:
    /// a cube-backed source (an environment map, or a material sky in SkyMode::Baked) drives both
    /// SH and IBL; a per-pixel source (a direct material sky) cannot light and degrades either to
    /// background-only (bake to light).
    ///
    /// Cost is explicit and authored. None is free; SH pays a one-time projection of the sky's
    /// radiance cube on the sky's dirty signal (the cheap tier — a faint, low-frequency glow); IBL
    /// pays a full split-sum prefilter on that same signal (the expensive tier — justified for a
    /// bright, directional sky). A near-black sky should stay None. The engine never rate-limits:
    /// every committed dirty signal pays its tier's cost, so a live edit of a lit sky's params pays
    /// a full re-convolution per commit — bounding that frequency (debouncing before commit) is the
    /// consumer's responsibility.
    enum class SkyLighting : u8
    {
        /// @brief Display the sky only; the scene keeps its flat ambient fallback.
        None,
        /// @brief Light the diffuse term from a spherical-harmonic projection of the sky.
        SH,
        /// @brief Light the scene with full split-sum image-based lighting from the sky.
        IBL,
    };

    /// @brief The scene's one authored sky: a source, an intensity, and a lighting tier.
    ///
    /// One per scene, resolved by the renderer via TryGetFirst<Sky> each Execute — the lights
    /// model. Source selects the sky kind (environment map / atmosphere / material) and carries its
    /// parameters; an empty Source is no sky (the flat fallback). Intensity scales the background
    /// and any ambient radiance the tier casts. Lighting requests how the sky lights the scene. If
    /// several Sky components exist the first walked wins and a warning logs once. No transform is
    /// read — the sky is scene-global.
    struct Sky
    {
        /// @brief The active sky source, or empty for no sky.
        SkySource Source;
        /// @brief Scales the sky's background and ambient radiance.
        f32 Intensity = 1.0f;
        /// @brief How the sky lights the scene, beyond displaying it.
        SkyLighting Lighting = SkyLighting::None;
    };

    /// @brief Time-of-day sun drive: the author's opt-in to derive the sun from a clock time.
    ///
    /// An author adds this component to a scene entity to position the sun by time of day
    /// instead of authoring a direction: TimeOfDaySystem derives the toward-sun direction from
    /// Hours, DayOfYear, and the orbital parameters and writes the scene's first directional
    /// Light's travel direction from it each tick — so direct lighting, shadows, and the sky all
    /// track the one derived sun. While present, the light's authored Direction is overwritten;
    /// a game animates the cycle by advancing Hours.
    struct TimeOfDay
    {
        /// @brief Time of day in solar hours; 12 is solar noon. Wraps over 24.
        f32 Hours = 12.0f;
        /// @brief Day of the year, measured from the northern spring equinox (days).
        f32 DayOfYear = 0.0f;
        /// @brief Orbital and site parameters the sun path is derived from; Earth defaults.
        Renderer::SunOrbit Orbit;
    };

    /// @brief A scene-authored field of GPU-resident points the renderer draws.
    ///
    /// Resolved by the renderer via View<PointField> each Execute — the lights model: every
    /// component with a live Field contributes one field to the point-field pass, whose presence
    /// is driven by any live field existing (no consumer toggle). Lod and CellSize are authored
    /// knobs (reflected, cooked, editable in the inspector); Field is the built GPU resource,
    /// runtime-only and never serialized. A consumer builds a Renderer::PointField from its point
    /// set and assigns it; a null Field draws nothing. The field's points are world-space (the
    /// resource's contract) — the entity's Transform is not applied.
    struct PointField
    {
        /// @brief Screen-density LOD knobs applied to this field's draw.
        Renderer::PointFieldLod Lod;
        /// @brief Cull-cell edge length in world units a consumer builds the field with.
        f32 CellSize = 8.0f;
        /// @brief Where in the frame the field accumulates (see Renderer::PointFieldPlacement).
        ///
        /// HdrTail (the default) rides the post-TAA/SSR tail; SceneColor draws into the lit scene
        /// color ahead of the translucent pass, so translucents blend over the field and a
        /// refractive material's scene-color grab includes it.
        Renderer::PointFieldPlacement Placement = Renderer::PointFieldPlacement::HdrTail;
        /// @brief The built GPU field this entity draws, or null for none.
        ///
        /// Runtime-only: carries no VE_FIELD, so reflection, the cooker, and the inspector never
        /// see it — it serializes as absent and default-constructs to null on load. A consumer
        /// (a system or app code) builds a Renderer::PointField from its point set and assigns it.
        Ref<Renderer::PointField> Field;
    };

    /// @brief A scene-authored bounded emissive volumetric medium the renderer ray-marches.
    ///
    /// Resolved by the renderer via View<VolumeField> each Execute — the lights model: every
    /// component with a live Field contributes one field to the volume pass, whose presence is
    /// driven by any live field existing (no consumer toggle). Opacity, EmissionScale,
    /// ExtinctionScale, and Steps are authored knobs (reflected, cooked, editable in the inspector);
    /// Field is the built GPU resource, runtime-only and never serialized. A consumer builds a
    /// Renderer::VolumeField from its voxel data and assigns it; a null Field draws nothing. The
    /// field's bounds are world-space (the resource's contract) — the entity's Transform is not
    /// applied.
    struct VolumeField
    {
        /// @brief Overall fade: scales emission and extinction toward zero (1 full, 0 invisible).
        f32 Opacity = 1.0f;
        /// @brief Unit remap over the baked emission radiance density.
        f32 EmissionScale = 1.0f;
        /// @brief Unit remap over the baked extinction density.
        f32 ExtinctionScale = 1.0f;
        /// @brief Fixed ray-march step count through the field (the quality knob).
        u32 Steps = 64;
        /// @brief The built GPU field this entity draws, or null for none.
        ///
        /// Runtime-only: carries no VE_FIELD, so reflection, the cooker, and the inspector never
        /// see it — it serializes as absent and default-constructs to null on load. A consumer (a
        /// system or app code) builds a Renderer::VolumeField from its voxel data and assigns it.
        Ref<Renderer::VolumeField> Field;
    };

    /// @brief Level-scoped post/pipeline render knobs.
    ///
    /// Carried on a Level and seeded into the renderer the app drives — a reflected,
    /// tolerantly-serialized struct, not a renderer type, so the renderer stays untouched
    /// and a new field does not invalidate existing level blobs. The sky/environment knobs
    /// are not here: they are the author-opt-in Sky and TimeOfDay scene components, resolved by
    /// the renderer itself. This struct carries the view-wide post and pipeline toggles the app maps
    /// onto its SceneRendererSettings (Bloom / Shadows / AO) and its per-frame SceneView
    /// (Exposure, BloomThreshold, BloomIntensity, BloomRadius).
    struct LevelRenderSettings
    {
        /// @brief Tonemap exposure fed into the per-frame SceneView.
        f32 Exposure = 1.0f;
        /// @brief The tone curve the terminal tonemap pass maps the exposed HDR through, fed into
        ///        the per-frame SceneView. Serialized by name in the level "render" block.
        Renderer::Tonemapper Tonemapper = Renderer::Tonemapper::ACES;
        /// @brief Whether auto-exposure adapts the tonemap exposure to the scene's luminance.
        ///
        /// When set, Exposure becomes a manual bias on the adapted result instead of the absolute
        /// exposure. Off matches the renderer's own default.
        bool AutoExposure = false;
        /// @brief Auto-exposure upper clamp on the adapted average luminance.
        ///
        /// Bounds how dark the metering lets a bright scene drive the exposure; a large value suits a
        /// high-dynamic-range scene (a bright sky, a starfield). The default matches the renderer's
        /// own ViewState default, so a level authoring none is unchanged.
        f32 AutoExposureMaxLuminance = 8.0f;
        /// @brief Lower percentile of the lit-pixel histogram the metering averages from, in [0, 1].
        ///
        /// Discards the darkest tail before averaging (0 keeps it). The default matches the
        /// renderer's own ViewState default.
        f32 AutoExposureLowPercentile = 0.0f;
        /// @brief Upper percentile of the lit-pixel histogram the metering averages to, in [0, 1].
        ///
        /// Discards the brightest tail before averaging (1 keeps it). The default matches the
        /// renderer's own ViewState default.
        f32 AutoExposureHighPercentile = 1.0f;
        /// @brief Whether the bloom battery is enabled.
        bool Bloom = true;
        /// @brief Bloom bright-pass luminance knee fed into the per-frame SceneView.
        f32 BloomThreshold = 1.0f;
        /// @brief Bloom composite intensity fed into the per-frame SceneView.
        f32 BloomIntensity = 1.0f;
        /// @brief Bloom upsample spread fed into the per-frame SceneView.
        f32 BloomRadius = 1.0f;
        /// @brief Whether the directional cascaded-shadow battery is enabled.
        bool Shadows = true;
        /// @brief Whether the punctual shadow atlas is enabled.
        ///
        /// Drives the point/spot shadow maps and the soft (PCSS) shadows cast by Sphere/Rect/Polygon
        /// area lights — independent of the directional Shadows toggle above.
        bool PunctualShadows = true;
        /// @brief Far distance (world units) the directional cascades are fit and rendered out to.
        ///
        /// Caps the shadowed range: cascades pack their resolution into this distance from the
        /// camera, and geometry past it is unshadowed. A large world needs a large value so distant
        /// casters still shadow; the default matches the renderer's own MaxShadowDistance default.
        f32 MaxShadowDistance = 100.0f;
        /// @brief Directional cascade shadow-map resolution: the per-cascade tile edge, in texels.
        ///
        /// A larger value sharpens the cascades at a memory/bandwidth cost. Clamped to the device
        /// maximum by the renderer; the default matches the renderer's own ShadowResolution default.
        u32 ShadowResolution = 1024;
        /// @brief Whether the SSAO battery is enabled.
        bool AO = true;
        /// @brief Whether screen-space reflections run.
        ///
        /// Off matches the renderer's own default. SSR renders the g-buffer at full resolution, so
        /// it disables the dynamic-resolution sub-rect while active.
        bool SSR = false;
        /// @brief Whether the pre-translucent scene-color copy is populated each frame.
        ///
        /// Enables the grab a Translucent-domain material samples the scene behind its fragment
        /// through (SampleSceneColor); without it those samples read black. Off matches the
        /// renderer's own default — a level whose translucents refract or distort the scene
        /// authors it on.
        bool Refraction = false;
        /// @brief Whether the depth-of-field battery runs.
        ///
        /// Off matches the renderer's own default.
        bool DepthOfField = false;
        /// @brief Depth-of-field focus plane distance in metres.
        ///
        /// Not consulted while the resolved camera is Physical — that camera's lens authors the
        /// focus distance, and this value is stored but inert until it stops being Physical. The
        /// default matches the renderer's own ViewState default.
        f32 DofFocusDistance = 10.0f;
        /// @brief Depth-of-field aperture diameter in metres.
        ///
        /// Not consulted while the resolved camera is Physical, exactly as DofFocusDistance is not.
        /// The default matches the renderer's own ViewState default (50mm f/2.8).
        f32 DofAperture = 0.0179f;
        /// @brief Depth-of-field blur radius ceiling in half-resolution pixels.
        ///
        /// A quality knob rather than a lens value, so it applies in every camera mode. Clamped to
        /// DofCocCeiling on the way in — a cooked level is untrusted input.
        f32 DofMaxCoc = 16.0f;
        /// @brief Depth-of-field gather ring count.
        ///
        /// A quality knob rather than a lens value, so it applies in every camera mode. Clamped to
        /// MaxDofRings on the way in: it is a GPU loop bound, and an unclamped authored value
        /// reaching the shader is a device hang rather than a recoverable error.
        u32 DofRingCount = 4;
    };
}

VE_ENUM(::Veng::LightType, 0x006B1D62EF4B5A16ULL)
VE_ENUMERATOR(Directional)
VE_ENUMERATOR(Point)
VE_ENUMERATOR(Spot)
VE_ENUMERATOR(Rect)
VE_ENUMERATOR(Sphere)
VE_ENUMERATOR(Polygon)
VE_ENUM_END();

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

VE_REFLECT(::Veng::CubeShape, 0x2B758A3FE238BAA5ULL)
VE_FIELD(Extent, .DisplayName = "Extent", .Display = {.Min = 0.001})
VE_FIELD(Material, .DisplayName = "Material")
VE_REFLECT_END();

VE_REFLECT(::Veng::PlaneShape, 0x0E53DEFF5662A295ULL)
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
VE_FIELD(CastsShadows, .DisplayName = "Casts shadows")
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
VE_FIELD(Width, .DisplayName = "Width", .Display = {.Min = 0.0, .Step = 0.05},
         .VisibleIf = VE_WHEN(self.Type == ::Veng::LightType::Rect))
VE_FIELD(Height, .DisplayName = "Height", .Display = {.Min = 0.0, .Step = 0.05},
         .VisibleIf = VE_WHEN(self.Type == ::Veng::LightType::Rect))
VE_FIELD(Radius, .DisplayName = "Radius", .Display = {.Min = 0.0, .Step = 0.05},
         .VisibleIf = VE_WHEN(self.Type == ::Veng::LightType::Sphere))
VE_FIELD(TwoSided, .DisplayName = "Two Sided",
         .VisibleIf = VE_WHEN(self.Type == ::Veng::LightType::Rect ||
                              self.Type == ::Veng::LightType::Polygon))
VE_ARRAY_FIELD(PolygonVertices, .DisplayName = "Polygon Vertices",
               .VisibleIf = VE_WHEN(self.Type == ::Veng::LightType::Polygon))
VE_REFLECT_END();

VE_REFLECT(::Veng::PlayerInput, 0x5401D36B1EF55045ULL)
VE_FIELD(State, .DisplayName = "State")
VE_REFLECT_END();
// Not VE_REPLICATED: PlayerInput flows client→server on the dedicated input channel (last-N-ticks
// redundant, unreliable), not in the server→client state snapshot — a different direction and
// discipline from replicated state.

VE_REFLECT(::Veng::InputContextStack, 0x89B0625016A01BE2ULL)
VE_ARRAY_FIELD(Active, .DisplayName = "Active")
VE_REFLECT_END();

VE_REFLECT(::Veng::Intent, 0x27F416122B525965ULL)
VE_FIELD(Move, .DisplayName = "Move")
VE_FIELD(Look, .DisplayName = "Look")
VE_FIELD(Actions, .DisplayName = "Actions")
VE_REFLECT_END();
// Not VE_REPLICATED: Intent is re-derived per tick server-side from the seat's input and is never
// sent — the client sees only its effect (the moved Transform), never the command.

VE_REFLECT(::Veng::Possesses, 0xC7D4144C7DF95B9BULL)
VE_FIELD(Pawn, .DisplayName = "Pawn")
VE_REFLECT_END();
// A seat's possession is server-authoritative; the Pawn reference replicates as its target's NetId.
VE_REPLICATED(::Veng::Possesses);

VE_REFLECT(::Veng::SeatInput, 0x2D178569EDDBC215ULL)
VE_FIELD(UsesKeyboardMouse, .DisplayName = "Uses Keyboard/Mouse")
VE_FIELD(Gamepad, .DisplayName = "Gamepad")
VE_FIELD(WantsGamepad, .DisplayName = "Wants Gamepad")
VE_REFLECT_END();
// Not VE_REPLICATED: SeatInput is a client-local device assignment (which keyboard/pad feeds a
// seat) — meaningless on another peer, never sent.

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
VE_ENUMERATOR(Remote)
VE_ENUMERATOR(Predicted)
VE_ENUM_END();

VE_REFLECT(::Veng::Authority, 0xA934C4B9009D7735ULL)
VE_FIELD(Tier, .DisplayName = "Tier")
VE_FIELD(Owner, .DisplayName = "Owner")
VE_REFLECT_END();

// Reflected so the inspector surfaces the server-assigned id (read-only), but *not* replicated:
// NetIdentity is the wire key itself, carried in each snapshot's per-entity header.
VE_REFLECT(::Veng::NetIdentity, 0x9E7C4A1B6D3F0852ULL)
VE_FIELD(Id, .DisplayName = "Net Id", .ReadOnly = true)
VE_REFLECT_END();

// Reflected so a consumer authors/inspects the anchor, but *not* replicated: NetAnchor rides the
// spawn record (read before the entity is created) so the claimant resolves at spawn time.
VE_REFLECT(::Veng::NetAnchor, 0x6B5366CCAC328A6CULL)
VE_FIELD(Lo, .DisplayName = "Anchor Lo")
VE_FIELD(Hi, .DisplayName = "Anchor Hi")
VE_REFLECT_END();

// Registered without a reflected field, so it neither serializes nor rides the wire: provenance is
// derivable state recorded at spawn, never authored and never persisted.
VE_TYPE(::Veng::PrefabSource, 0xD0EA6653C1F9B14DULL);

// A fieldless mark: the component's presence is the whole signal, so there is nothing to reflect.
VE_TYPE(::Veng::NetSpawn, 0xF6B2DEC39DC3F319ULL);

// Registered without a reflected field, so it neither serializes nor rides the wire: the marker is
// per-process presentation state the engine derives per frame, meaningless off its machine.
VE_TYPE(::Veng::LocalControl, 0x7B0B171ABA0821EDULL);

// Reflected so the inspector surfaces the seat's account (read-only), but *not* replicated: the
// account id stays server-local, never broadcast to world members.
VE_REFLECT(::Veng::SeatAccount, 0xF3DBE3736F6A92EDULL)
VE_FIELD(Account, .DisplayName = "Account", .ReadOnly = true)
VE_REFLECT_END();

VE_REFLECT(::Veng::CameraFollow, 0xF8BD924F0A0F9DB0ULL)
VE_FIELD(Target, .DisplayName = "Target")
VE_FIELD(Offset, .DisplayName = "Offset")
VE_FIELD(Damping, .DisplayName = "Damping", .Display = {.Min = 0.0})
VE_REFLECT_END();

VE_REFLECT(::Veng::CameraLook, 0x08B30475EB8C5788ULL)
VE_FIELD(Yaw, .DisplayName = "Yaw",
         .Tooltip = "Heading about world up, radians; positive turns left")
VE_FIELD(Pitch, .DisplayName = "Pitch", .Tooltip = "Elevation, radians; positive looks up")
VE_FIELD(PitchLimit, .DisplayName = "Pitch Limit", .Tooltip = "Maximum |Pitch| in radians",
         .Display = {.Min = 0.0})
VE_REFLECT_END();

VE_REFLECT(::Veng::CameraOrbit, 0xE2510C54F8FF9F38ULL)
VE_FIELD(Focus, .DisplayName = "Focus", .Tooltip = "Orbited point, world space")
VE_FIELD(Distance, .DisplayName = "Distance", .Tooltip = "Eye distance from the focus",
         .Display = {.Min = 0.0})
VE_FIELD(MinDistance, .DisplayName = "Min Distance", .Display = {.Min = 0.0})
VE_FIELD(MaxDistance, .DisplayName = "Max Distance", .Display = {.Min = 0.0})
VE_FIELD(Yaw, .DisplayName = "Yaw",
         .Tooltip = "Heading about world up, radians; positive turns left")
VE_FIELD(Pitch, .DisplayName = "Pitch",
         .Tooltip = "Elevation, radians; positive tilts the camera up toward the focus")
VE_FIELD(PitchLimit, .DisplayName = "Pitch Limit", .Tooltip = "Maximum |Pitch| in radians",
         .Display = {.Min = 0.0})
VE_FIELD(FocusTarget, .DisplayName = "Focus Target", .Tooltip = "Point the focus glides toward")
VE_FIELD(FocusDamping, .DisplayName = "Focus Damping",
         .Tooltip = "Exponential-smoothing rate per second; 0 snaps", .Display = {.Min = 0.0})
VE_REFLECT_END();
VE_REFLECT(::Veng::FirstPersonRig, 0xDDD797C278D97169ULL)
VE_FIELD(Target, .DisplayName = "Target")
VE_FIELD(EyeOffset, .DisplayName = "Eye Offset",
         .Tooltip = "Eye position in the target's (or the socket's) local frame")
VE_FIELD(
    EyeSocket, .DisplayName = "Eye Socket",
    .Tooltip = "Optional mesh socket on the target naming the eye anchor; empty uses Eye Offset")
VE_FIELD(MinPitch, .DisplayName = "Min Pitch",
         .Tooltip = "Smallest pitch about the horizon, radians")
VE_FIELD(MaxPitch, .DisplayName = "Max Pitch",
         .Tooltip = "Largest pitch about the horizon, radians")
VE_FIELD(BobAmplitude, .DisplayName = "Bob Amplitude",
         .Tooltip = "View-bob amplitude in metres; 0 disables", .Display = {.Min = 0.0})
VE_FIELD(BobFrequency, .DisplayName = "Bob Frequency",
         .Tooltip = "View-bob cycles per metre travelled", .Display = {.Min = 0.0})
VE_REFLECT_END();

// CameraFollow / CameraLook / CameraOrbit / FirstPersonRig are not VE_REPLICATED: View-phase camera
// rig state, derived locally per client and "never on the wire" (see each struct's doc) — the client
// owns its own camera.

VE_REFLECT(::Veng::GameModeConfig, 0xAE57419CF98B07F8ULL)
VE_FIELD(PlayerPrefab, .DisplayName = "Player Prefab")
VE_REFLECT_END();

VE_REFLECT(::Veng::EnvironmentSky, 0x51902800E072B6E9ULL)
VE_FIELD(Map, .DisplayName = "Map")
VE_REFLECT_END();

VE_ENUM(::Veng::SkyMode, 0x9C2A0D5E7B1F4488ULL)
VE_ENUMERATOR(Direct)
VE_ENUMERATOR(Baked)
VE_ENUM_END();

VE_REFLECT(::Veng::AtmosphereSky, 0x2091BE830E0AA76DULL)
VE_FIELD(Params, .DisplayName = "Parameters")
VE_FIELD(Mode, .DisplayName = "Mode")
VE_REFLECT_END();

VE_REFLECT(::Veng::MaterialSky, 0x278971B85ADDA928ULL)
VE_FIELD(Material, .DisplayName = "Material")
VE_FIELD(Mode, .DisplayName = "Mode")
VE_REFLECT_END();

VE_ENUM(::Veng::SkyLighting, 0xB2C6211BC808C7BDULL)
VE_ENUMERATOR(None)
VE_ENUMERATOR(SH)
VE_ENUMERATOR(IBL)
VE_ENUM_END();

VE_VARIANT(::Veng::SkySource, 0x3638DADE35D35C41ULL);

VE_REFLECT(::Veng::Sky, 0xC7D64305B199222CULL)
VE_FIELD(Source, .DisplayName = "Source")
VE_FIELD(Intensity, .DisplayName = "Intensity", .Display = {.Min = 0.0})
VE_FIELD(Lighting, .DisplayName = "Lighting")
VE_REFLECT_END();

VE_REFLECT(::Veng::TimeOfDay, 0x3096812B98FEC8D2ULL)
VE_FIELD(Hours, .DisplayName = "Hours", .Tooltip = "Solar hours; 12 is noon",
         .Display = {.Min = 0.0, .Max = 24.0})
VE_FIELD(DayOfYear, .DisplayName = "Day of year",
         .Tooltip = "Days since the northern spring equinox", .Display = {.Min = 0.0})
VE_FIELD(Orbit, .DisplayName = "Orbit")
VE_REFLECT_END();

VE_REFLECT(::Veng::Renderer::PointFieldLod, 0x9A3C1F6E4B2D08A7ULL)
VE_FIELD(AggregateThreshold, .DisplayName = "Aggregate Threshold", .Display = {.Min = 0.0})
VE_FIELD(Hysteresis, .DisplayName = "Hysteresis", .Display = {.Min = 0.0, .Max = 1.0})
VE_FIELD(AggregateSplatPixels, .DisplayName = "Aggregate Splat Pixels", .Display = {.Min = 0.0})
VE_FIELD(AggregateIntensity, .DisplayName = "Aggregate Intensity", .Display = {.Min = 0.0})
VE_FIELD(MinPixels, .DisplayName = "Min Pixels", .Display = {.Min = 0.0})
VE_FIELD(MaxPixels, .DisplayName = "Max Pixels", .Display = {.Min = 0.0})
VE_FIELD(MaxIntensity, .DisplayName = "Max Intensity", .Display = {.Min = 0.0})
VE_REFLECT_END();

VE_ENUM(::Veng::Renderer::PointFieldPlacement, 0xB0D259EFB3B7BF16ULL)
VE_ENUMERATOR(HdrTail)
VE_ENUMERATOR(SceneColor)
VE_ENUM_END();

VE_REFLECT(::Veng::PointField, 0x1D7F4A0C6E5B8392ULL)
VE_FIELD(Lod, .DisplayName = "LOD")
VE_FIELD(CellSize, .DisplayName = "Cell Size", .Display = {.Min = 0.001})
VE_FIELD(Placement, .DisplayName = "Placement",
         .Tooltip = "HDR tail (post-TAA, pre-bloom) or lit scene color (ahead of translucents)")
VE_REFLECT_END();

VE_REFLECT(::Veng::VolumeField, 0xA8C07107902C21E0ULL)
VE_FIELD(Opacity, .DisplayName = "Opacity", .Tooltip = "Fades emission and extinction toward zero",
         .Display = {.Min = 0.0, .Max = 1.0})
VE_FIELD(EmissionScale, .DisplayName = "Emission Scale", .Display = {.Min = 0.0})
VE_FIELD(ExtinctionScale, .DisplayName = "Extinction Scale", .Display = {.Min = 0.0})
VE_FIELD(Steps, .DisplayName = "Steps", .Tooltip = "Ray-march step count", .Display = {.Min = 1.0})
VE_REFLECT_END();

VE_REFLECT(::Veng::LevelRenderSettings, 0x28E4618C66455E21ULL)
VE_FIELD(Exposure, .DisplayName = "Exposure", .Display = {.Min = 0.0})
VE_FIELD(Tonemapper, .DisplayName = "Tonemapper")
VE_FIELD(AutoExposure, .DisplayName = "Auto Exposure")
VE_FIELD(AutoExposureMaxLuminance, .DisplayName = "Auto Exposure Max Luminance",
         .Display = {.Min = 0.0})
VE_FIELD(AutoExposureLowPercentile, .DisplayName = "Auto Exposure Low Percentile",
         .Display = {.Min = 0.0, .Max = 1.0})
VE_FIELD(AutoExposureHighPercentile, .DisplayName = "Auto Exposure High Percentile",
         .Display = {.Min = 0.0, .Max = 1.0})
VE_FIELD(Bloom, .DisplayName = "Bloom")
VE_FIELD(BloomThreshold, .DisplayName = "Bloom Threshold", .Display = {.Min = 0.0})
VE_FIELD(BloomIntensity, .DisplayName = "Bloom Intensity", .Display = {.Min = 0.0})
VE_FIELD(BloomRadius, .DisplayName = "Bloom Radius", .Display = {.Min = 0.0})
VE_FIELD(Shadows, .DisplayName = "Shadows")
VE_FIELD(PunctualShadows, .DisplayName = "Punctual / Area Shadows")
VE_FIELD(MaxShadowDistance, .DisplayName = "Max Shadow Distance", .Display = {.Min = 0.0})
VE_FIELD(ShadowResolution, .DisplayName = "Shadow Resolution", .Display = {.Min = 1})
VE_FIELD(AO, .DisplayName = "SSAO")
VE_FIELD(SSR, .DisplayName = "Screen-Space Reflections")
VE_FIELD(Refraction, .DisplayName = "Refraction (Scene-Color Grab)")
VE_FIELD(DepthOfField, .DisplayName = "Depth of Field")
VE_FIELD(DofFocusDistance, .DisplayName = "DoF Focus Distance", .Display = {.Min = 0.0})
VE_FIELD(DofAperture, .DisplayName = "DoF Aperture", .Display = {.Min = 0.0})
VE_FIELD(DofMaxCoc, .DisplayName = "DoF Max Blur Radius",
         .Display = {.Min = 0.0, .Max = static_cast<f64>(::Veng::Renderer::DofCocCeiling)})
VE_FIELD(DofRingCount, .DisplayName = "DoF Ring Count",
         .Display = {.Min = 1, .Max = static_cast<f64>(::Veng::Renderer::MaxDofRings)})
VE_REFLECT_END();
