# Physics — rigid bodies, collision content, queries, and the solver's containment

`Veng/Physics/` is the engine's rigid-body simulation: a **`PhysicsWorld`** a `Scene` optionally
owns, **`RigidBody`/`Collider`** as ordinary reflected components, cooked **`CollisionShape`**
geometry behind them, **sensors** and **constraints**, the **`Raycast`/`ShapeCast`/`Overlap`**
query trio, a fixed step inside the Sim phase, a small closed collision-layer table, and a debug
visualization through the existing `Renderer::DebugDraw`. Project-wide conventions live in
[the root CLAUDE.md](../../../CLAUDE.md); the ECS world and the Sim/View tick split it steps
inside are in [../Scene/CLAUDE.md](../Scene/CLAUDE.md); the prediction/reconciliation layer the
replay gate below defers to is in [../Net/CLAUDE.md](../Net/CLAUDE.md); the cooked-blob layout
behind `CollisionShape` is in [../../../assetpack/CLAUDE.md](../../../assetpack/CLAUDE.md) and the
importer that writes it in [../../../cooker/CLAUDE.md](../../../cooker/CLAUDE.md).

## The world is optional, per-`Scene`, and has its own frame

A `Scene` optionally owns a `PhysicsWorld` exactly the way it optionally owns a `SceneSimulation`
— `Scene::SetPhysicsWorld` / `GetPhysicsWorld`, `Clone()` copies neither. **It owns none by
default**, so a scene with no physics instantiates nothing, allocates nothing, and steps nothing:
the minimal template and the editor's preview scenes are untouched. The world is created from a
**`PhysicsWorldInfo`** carrying gravity, the collision matrix, and the body budget — and **no step
rate**, because a world steps in the Sim phase at that phase's fixed `SimTickRate`. A second
independent rate would need an accumulator and a substep policy while giving up the determinism,
replay and transform interpolation the Sim phase already supplies.

**The world's frame is a property of the world, not of any viewer.** Every pose crossing the API —
`PhysicsPose`, a velocity, a debug-draw vertex — is in the one frame the world was created in, and
that frame does not move because a camera, a seat, or a presenting peer moved. Anchoring a
simulation at a per-viewer origin makes two peers integrate the same world in different frames, and
the divergence is silent rather than loud. `PhysicsPose::Position` is `dvec3` and the solver is
built double-precision for the same reason: a large-extent world keeps sub-millimetre positions far
from its origin without a rebasing scheme.

The budget is not free. `PhysicsWorldInfo::MaxBodies` sizes the per-step scratch as well as the body
table — the solver reserves it up front and treats exhausting it as fatal — so raising the ceiling
costs resident memory whether or not the bodies exist.

## The components are the authority; the bodies are their shadow

```
RigidBody       { Motion (Static|Kinematic|Dynamic); Layer; Mass; LinearDamping; AngularDamping; SyncTransform; }
Collider        { Shape (Box|Sphere|Capsule|Mesh); Extents; Offset; Friction; Restitution; Geometry; }
PhysicsPose     { dvec3 Position; quat Rotation; }
Sensor          { Layers; }
SensorOverlaps  { vector<Entity> Current, Entered, Exited; }
FixedConstraint { Entity Target; }
PointConstraint { Entity Target; vec3 Point; }
HingeConstraint { Entity Target; vec3 Point; vec3 Axis; }
```

`RigidBody`, `Collider`, `Sensor` and the three constraints are plain reflected components,
registered in `RegisterBuiltinTypes`, authorable in the editor inspector and cooked into prefabs
through the existing pipeline — **no ABI surface**. An entity needs a `RigidBody` and a `Collider`
to be simulated; a `RigidBody` with no `Collider` has no shape and is ignored. Adding the pair at
runtime creates the backing body on the next step, removing either destroys it, and editing a field
re-creates the body with the new settings. The three primitives are the shapes that need no cooked
data, so a `Collider` describing one is complete on its own.

`PhysicsPose` is the solver's own pose channel, added by the step to every physics entity. It is
registered fieldless (`VE_TYPE`), so it never serializes and never rides the wire — it is derived
state, not authored state. `SensorOverlaps` is the same kind of thing for a sensor's overlap set.

**A body's pose is world space.** A physics entity is expected to be a scene-graph root: its
`Transform` is read and written as a world pose and a parent's transform is not composed into it.

## Cooked collision geometry — `CollisionShape`

`Collider::Shape = Mesh` takes its shape from the `AssetHandle<CollisionShape>` in
`Collider::Geometry` instead of from `Extents`. **`CollisionShape` is an ordinary cooked asset**
(`AssetTypes::CollisionShape`, `Veng/Asset/CollisionShape.h`) carrying either a **convex hull** as
its hull vertices or an **indexed triangle mesh**, authored as a `*.collision.json` naming a source
model and a mode:

```json
{ "model": "hull.glb", "mode": "convex" }
```

Two properties are load-bearing:

- **The blob is engine-owned geometry, not a solver blob.** A point cloud or an indexed triangle
  soup as raw `f32`/`u32`; the solver's shape is built at load. A solver's serialized shape form is
  version-bound, so writing it would make a version bump silently invalidate every cooked shape in
  every pack — and the blob layout is a public contract living in `assetpack`, a library that must
  not know the solver exists. This way a solver bump is a **rebuild**, not a re-cook. The cost is
  one shape construction per asset at load, which is the right trade. **The cooker links no solver
  at all**: the hulling is the cook's own, so the offline toolchain's dependency set is unchanged.
- **A triangle mesh may not back a `Dynamic` body.** A triangle mesh is a surface: it has no
  interior and no inertia, and a dynamic one is a well-known way to get a solver that misbehaves
  for reasons the consumer cannot see. `PhysicsWorld::CreateBody` **asserts** on the pair rather
  than letting it be discovered at runtime. `Static` and `Kinematic` are both permitted — a moving
  piece of architecture is exactly a kinematic triangle mesh — and a kinematic one is given
  synthetic mass properties, because the solver cannot derive them from a surface and refuses to
  build the motion state otherwise.

A `ColliderShape::Mesh` collider whose handle is **not resident** has no shape, so the step skips
it exactly as it skips a `RigidBody` with no `Collider`, and the body is created on the tick the
asset arrives. Nothing blocks and nothing asserts.

## Sensors are state a system drains

A `Sensor` makes an entity's body detect overlap and resolve no contact. What it touched is
published on the same entity as a **`SensorOverlaps`** — `Current`, plus this tick's `Entered` and
`Exited` — rather than delivered as a callback, so a gameplay system reads it in its own phase and
**no game code runs inside the solver's step**. That is the `FocusRequest`/`TravelRequest` idiom
applied to contact.

A body on `PhysicsLayer::Trigger` is a sensor whether or not it carries the component — the layer
means exactly that — but only an entity carrying a `Sensor` is given a `SensorOverlaps`.
`Sensor::Layers` filters the published set on top of the world's `CollisionMatrix`, which decides
what the sensor is told about at all.

**The set is "overlapping and active", not "overlapping".** The solver reports contacts for the
bodies it simulates, so a dynamic body that falls asleep inside a sensor leaves `Current` and is
reported in `Exited`. Sensor bodies are created with the solver's kinematic-vs-non-dynamic pairing
enabled, so a kinematic mover crossing a trigger volume *is* reported.

## Constraints — three, and the fixed one is the load-bearing one

`FixedConstraint`, `PointConstraint` and `HingeConstraint` are components naming two entities:
enough to latch one body to another rigidly, pin a pivot, or hang a door. Deliberately the small
set, because each constraint type is a tuning surface and an unused one is a maintenance cost.

The reconcile pass runs them **after** the body pass, so a constraint stamped in the same tick as
its bodies finds them; one naming an entity with no body is inert and is built on the tick that
body arrives. Removing the component destroys the constraint, and destroying either body destroys
it too — the component is the authority here as everywhere else in the module.

**Fixed is the one consumers lean on**: attaching a *dynamic* body to a moving *kinematic* one is
the robust way to carry something, where friction on a fast-moving surface is not. A constraint
solves for velocity on bodies the solver integrates, so it has **no effect between two non-dynamic
bodies** — a consumer carrying a kinematic body uses parenting, not a constraint.

## The query surface — `Raycast` / `ShapeCast` / `Overlap`

`Veng/Physics/Queries.h` is three free functions over a `PhysicsWorld`. Each takes an explicit
**`QueryFilter`** (a layer mask, an ignore-entity span, and whether sensors count) rather than
reading ambient state, and each returns the **`Entity`** alongside the geometric result so a caller
lands back in the ECS without a second lookup. All three are **pure** — they mutate nothing, so a
View-phase consumer may call them.

```cpp
optional<RayHit>   Raycast  (const PhysicsWorld*, dvec3 origin, vec3 direction, f32 maxDistance,
                             const QueryFilter& = {});
optional<ShapeHit> ShapeCast(const PhysicsWorld*, const Collider& shape, const PhysicsPose& from,
                             dvec3 to, const QueryFilter& = {});
usize              Overlap  (const PhysicsWorld*, const Collider& shape, const PhysicsPose& at,
                             const QueryFilter&, vector<Entity>& out);
```

- **The world is a pointer, and null is empty.** `Raycast(scene.GetPhysicsWorld(), …)` on a scene
  with no world returns `nullopt` rather than asserting, so an optional-physics scene degrades
  quietly.
- **The shape is a `Collider`**, the same vocabulary a body carries — including a
  `ColliderShape::Mesh` naming a cooked hull. So a mover sweeps *its own shape* with no second
  description of it. Sweeping a **triangle-mesh** `CollisionShape` is a fatal assert: a concave
  shape has no sweep.
- **`ShapeCast` is the one the consumers lean on.** It is what "sweep this hull along this frame's
  motion and tell me where it stops" reduces to, and it is the primitive under both a kinematic
  mover and a character controller. `ShapeHit::Fraction` is the fraction of `to - from.Position`
  travelled, so the stopping pose is `from.Position + (to - from.Position) * Fraction` at
  `from.Rotation`; `Position` and `Normal` are the contact point and the outward normal of the body
  hit.
- **`Overlap` reports intersection, not contact.** A body resting exactly against the volume's face
  is not inside it — two shapes placed face to face report a penetration of a few times 1e-8 from
  float rounding alone, so the test admits a hit only past a tenth of a millimetre.

## The step, and the two-writer hazard

`StepPhysics(Scene&, delta)` is the whole step as a free function, so a headless tool or a test
drives a scene's physics without building a `SceneSimulation`. It runs five passes:

1. **Reconcile.** Gather every `(RigidBody, Collider)` entity through the *const* view (a structural
   change during iteration is illegal, and adding a `PhysicsPose` is one, so the pass that finds
   them cannot be the pass that makes them), add the missing `PhysicsPose`s and `SensorOverlaps`,
   bring every body into line through the idempotent `PhysicsWorld::CreateBody`, then sweep
   `GetBodyEntities` for bodies whose entity has lost its components or died. Constraints reconcile
   the same way, after the bodies, through `CreateConstraint`/`GetConstraintOwners`.
2. **Push.** A `Static` body is placed outright; a `Kinematic` one is *swept* toward its target via
   `MoveKinematicBody`, which is what lets it push dynamic bodies instead of passing through them. A
   `Dynamic` body's pose belongs to the solver and is not pushed.
3. **Step.** One `PhysicsWorld::Step(delta)`.
4. **Pull.** Write each body's result into `PhysicsPose`, and into `Transform` for a body whose
   `RigidBody` sets `SyncTransform`.
5. **Publish.** Write each `Sensor` entity's overlap set into its `SensorOverlaps`, diffing against
   last tick's for the enter/exit deltas.

**Contacts are found against the pose a step starts from.** A kinematic body teleported across a
sensor in one tick is therefore reported on the *next* tick, once the step begins with it inside.

**`PhysicsSystem` is the builtin Sim system wrapping that.** Registration in
`RegisterBuiltinSystems` makes it *resolvable*, not ordered — so **a level that wants physics names
`PhysicsSystem` in its own `systems` array**, placed after the systems that produce motion, so a
kinematic body's target pose for the tick is already written. A level that does not name it runs no
solver, which is what keeps the "absent by default costs nothing" claim honest.

**The two-writer hazard.** A consumer whose `Transform` is driven by its own per-frame pass must not
also carry a `Dynamic` `RigidBody`: both write the same field and the last writer each frame wins.
`Kinematic` is the mode for *I own the transform, tell me what I hit*.

**`SyncTransform` is the seam past that**, for a consumer whose `Transform` is a *derived per-frame
projection* rather than a pose at all — a floating-origin renderer, a re-placed impostor. Cleared,
the step neither reads nor writes `Transform`: `PhysicsPose` alone is the solver's interface, the
consumer writes it from whatever it considers authoritative and reads the result back, and the
`Transform` is left entirely to the consumer's own pass. Set (the default), the two stay in step and
the simple case needs no thought.

## The replay gate — a physics scene does not roll back

`PhysicsSystem::OnUpdate` **returns early when `SystemContext::IsReplay` is set.** Reconciliation
replays the entire Sim phase, but the solver is stateful in ways the prediction history does not
carry — velocities, the contact cache, sleep state — so nothing restores them before the replay. A
replayed step would advance the physics clock (`GetStepCount`) an extra time against state that was
never rewound, and the drift from the sim tick is permanent rather than self-correcting. The gate
makes a mispredict of N ticks advance the world by **exactly one** step.

So **a scene with a `PhysicsWorld` does not participate in rollback.** That is a stated limitation,
not an oversight, and `SaveState`/`RestoreState` are the seam a real rewind is built on: they
capture and restore the whole solver state (poses, velocities, contacts, sleep) as opaque bytes,
round-trip tested, and `RestoreState` reports unreadable bytes as a `Result` error rather than
asserting. Nothing predicts through them yet.

## Layers are a closed table

`PhysicsLayer` is a **closed** five-member enum — `Static`, `Moving`, `Character`, `Trigger`,
`Query` — and a `CollisionMatrix` is one bitmask row per layer saying which pairs may produce
contacts. `PhysicsWorldInfo` *selects* a matrix; it does not author new layers. Closed rather than an
open registry because filtering is the thing that silently breaks a world when it drifts, and a
table you can read in one screen is worth more than an extensible one.

The matrix must be **symmetric** — the solver consults it in one direction only, so an asymmetric
table means "a hits b but b does not hit a", which no solver can honour. `IsSymmetric` checks it and
`PhysicsWorld::Create` asserts it. `DefaultCollisionMatrix` is what a default `PhysicsWorldInfo`
selects: static geometry stops movers and characters but not other static geometry; movers,
characters and triggers all see each other; a `Query` body collides with nothing, which is what
makes it query-only. A body on the `Trigger` layer is created as a sensor — it reports overlaps and
pushes nothing.

Object layers map onto **two** broad-phase layers: `Static` and `Query` are non-moving, the rest
move.

## The solver is contained by the Native idiom

Jolt Physics is vendored at a pinned tag through `FetchContent` and linked **PRIVATE** to `libveng`.
It is reached only through `PhysicsWorld::Native`, a forward-declared struct defined in
`PhysicsWorld.cpp`, exactly as Vulkan, GLFW, VMA and nfd are contained — **no public header names a
`JPH::` type**, and the `include_hygiene` test fails the build if one leaks.

Four vendored build options are load-bearing and are set in the top-level `CMakeLists.txt`:

- **`CPP_EXCEPTIONS_ENABLED` off** — veng builds `-fno-exceptions` and a stray `throw` is a compile
  error. This is the property that made this solver reachable at all.
- **`CPP_RTTI_ENABLED` off** — the solver carries its own type system and needs no C++ RTTI. The
  flag is solver-scoped; veng itself does **not** build `-fno-rtti`.
- **`DOUBLE_PRECISION` on** — real-valued positions, for the large-extent worlds described above,
  at roughly a 5–10 % step cost.
- **`CROSS_PLATFORM_DETERMINISTIC` on** — no fast math, no FMA contraction, so two peers on
  different hardware step to the same bits. It is paid for in every build, so it is **gated by a
  test**: `PhysicsWorld::HashPoses` folds every body's raw pose bits order-independently, and
  `tests/unit/physics.cpp` compares a fixed fixture stepped a fixed number of ticks against a
  checked-in golden hash. **A solver version bump regenerates that constant in the same commit that
  moves the pin.**

Beside those, the solver's GPU-compute backends (DX12/Vulkan/Metal) are off — they compile shaders
through an external toolchain this build does not carry and nothing here uses them — as are its own
debug renderer and profiler, since veng draws through `Renderer::DebugDraw` and profiles through
`VE_PROFILE_*`.

The solver's trace and assertion hooks route to `Log::Info` and `VE_ASSERT`, so a physics assertion
breaks in the debugger like any other engine assert. Allocation uses the solver's default allocator:
veng has no house general-purpose allocator to route it through (VMA is GPU memory and is not one).
The process-wide registration is refcounted by live worlds, so a process may create and destroy any
number of them.

**The job system is single-threaded.** The solver's work runs entirely on the thread that calls
`Step`. veng's render thread is single and sharing a pool with the `TaskSystem` is a separate design
question; the step is not the bottleneck at this scale.

## Debug draw

`PhysicsWorld::SetDebugDrawEnabled` is the runtime toggle; when it is set and the tick has a
`SystemContext::Debug` sink, `PhysicsSystem` calls `DrawDebug` each step. Shapes are drawn as
wireframes tinted by motion type and dimmed while a dynamic body sleeps, and each active contact is
a short normal spike. Contacts are collected by a `ContactListener` that only records — it never
touches `ContactSettings`, so it cannot perturb the simulation — and recording is gated on the same
toggle, so a world with the visualization off pays nothing beyond a virtual call.
