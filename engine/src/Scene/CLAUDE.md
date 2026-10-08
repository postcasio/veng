# Scene & gameplay — ECS, input control flow, systems, levels

`Scene` is the runtime ECS world, and the gameplay layer is built from its primitives — components
marking entities plus systems acting on them. **Simulation logic is components + systems**, never
controller/manager/mode objects; the one narrow exception is **presentation binding**, which may be
a per-instance driver (a `GuiOverlay` names a registered `GuiDriver` the engine instantiates with its
document — see [../Gui/CLAUDE.md](../Gui/CLAUDE.md)). A driver reads scene state and stamps
request/command and `VE_VIEW_OUTPUT`-tagged components; it never advances authoritative simulation or
writes a replicated or Sim-input component. It is shaped for
veng's data-oriented grain and for the networking built on top. Project-wide conventions live in
[the root CLAUDE.md](../../../CLAUDE.md); the runtime overview and the engine-managed world drive
in [engine/CLAUDE.md](../../CLAUDE.md); reflection and the `TypeRegistry` in
[../Reflection/CLAUDE.md](../Reflection/CLAUDE.md); the renderer that consumes scenes in
[../Renderer/CLAUDE.md](../Renderer/CLAUDE.md); networking in [../Net/CLAUDE.md](../Net/CLAUDE.md).
Task-oriented guides:
[writing gameplay systems](../../../docs/guides/writing-gameplay-systems.md) and
[wiring a level](../../../docs/guides/wiring-a-level.md).

## The ECS world

A `Scene` is a runtime **ECS world**: a generational `Entity` handle
(`{ u32 Index; u32 Generation; }`, `Entity::Null` empty) over a `TypeId`-keyed, type-erased
**sparse-set** per-component storage, with templated `Add`/`Remove`/`Get`/`TryGet`/`Has` and the
multi-component queries `View<Ts...>` (range-for, yields `(Entity, Ts&...)`, supports `break`) and
`Each<Ts...>`. A query drives off the smallest participating pool. **Structural changes during
iteration are illegal** — adding/removing components or destroying entities mid-`View`/`Each` is
API misuse; the single-threaded model offers no re-entrancy guard. A stale `Entity` (its slot
recycled, generation bumped) accessed through the API is a fatal `VE_ASSERT`, not silent UB.
`DestroyEntity` is **recursive** — it walks the `Hierarchy` `FirstChild` → `NextSibling` links to
destroy the entity's whole subtree in **O(subtree)**, detaching the destroyed root from any
surviving parent's child list first so no sibling is left dangling. A **component is just a
reflected type a `Scene` pools** — see [../Reflection/CLAUDE.md](../Reflection/CLAUDE.md) for
`TypeId` and registration; pools are made lazily on first `Add` of a type, and there is no
separate component-id space.

**A component access never hashes.** The scene keeps its pools in a table indexed by the type's
registry ordinal (`TypeInfo::Ordinal`, see [../Reflection/CLAUDE.md](../Reflection/CLAUDE.md)), so
`Get`/`TryGet`/`Has` resolve their pool by array index and then do one sparse-set membership check
(index → dense slot → whole-handle compare), inlined into the calling unit from
`Veng/Scene/ComponentPool.h`. A `View`/`Each` resolves every participating pool once, when it is
created; its iterator keeps the dense slots its match test found, so dereferencing looks nothing up
again. The pool stores its components at the type's alignment and relocates them through the type's
move constructor, so a component that points into itself is safe to pool.

**A component declares the siblings it resolves, and removal honours the declaration.**
`VE_REQUIRES(::Ns::Component, ::Ns::Sibling, …)` beside a describe block records the required
`TypeId`s in `TypeInfo::Requires`, and `Scene::RemoveComponent` / `Remove<T>` then return a
**`VoidResult`** that **fails** — the component untouched, the error naming both types — while a
requirer sits on the same entity. `Scene::FindRequirer(entity, id)` is the same question asked
ahead of the call, which is what a tool offering removal (the MCP `entity.remove_component` verb,
the editor inspector) uses so it reports the reason instead of issuing a call it knows fails, and
so it never stacks an undo entry over a removal that did not happen. The declaration constrains
**removal only**: an entity mid-assembly may carry the requirer before its siblings, so `Add` is
never gated and a requirement may sit unmet. `DestroyEntity` is not gated either — it tears its
pools down directly, and the requirer goes with the entity. `GuiSurface` → `MeshRenderer` is the
declaration the engine ships: the document is drawn into the sibling renderer's material and has
nowhere else to land, so stripping the renderer would leave the surface resolving a component that
has gone.

`Scene::ForEachComponent(Entity, const function<void(TypeId, void*)>&)` iterates every pool that
holds the entity, calling the visitor with each component's `TypeId` and an erased pointer — the
type-agnostic enumeration the editor inspector walks (templated `Get`/`Has` need the type at
compile time; this does not).

## Hierarchy

The scene hierarchy is an intrusive, sibling-linked **`Hierarchy`** component: a `Parent` up-edge
plus a doubly-linked, ordered child list (`FirstChild`/`PrevSibling`/`NextSibling`). Topology is
mutated **only** through `Scene` operations — `SetParent(child, parent)` (detach from the old
list, append under the new in O(1); `Entity::Null` parent re-parents to root), `Detach(child)`,
and `MoveBefore(child, sibling)` (the editor's drag-reorder / insert-at) — which maintain all four
links as a set and bump the spatial version. `GetParent(entity)` and `ForEachChild(entity, fn)`
(forward, insertion order) are the read side. A cycle (a descendant adopting an ancestor) is API
misuse and a fatal `VE_ASSERT`. Only `Parent` is a reflected, persisted field; the three list
links are derived and rebuilt on prefab spawn, so the serializer and cooker never touch them.

**Attaching to a model's authored place is plain parenting.** `AttachToSocket(scene, child,
meshEntity, socketName)` (`Veng/Scene/Sockets.h`) resolves the mesh entity's `MeshRenderer`, finds
the named `MeshSocket` on its resident mesh (see [../Asset/CLAUDE.md](../Asset/CLAUDE.md)),
`SetParent`s the child under it, and writes the socket's mesh-space transform — position,
**orientation**, and scale — onto the child's `Transform`. No new scene-graph concept is involved,
so everything downstream works unchanged: `WorldMatrix` composes, `ComputeWorldMatrices` walks it,
`DestroyEntity` recurses through it, and a socket on a moving parent carries its child for free. It
returns **false** rather than asserting when the entity draws nothing, its mesh is not yet resident,
or the model carries no socket by that name — each is a content or timing condition a consumer
reports. `FindMeshSocket` is the same resolution without the attachment.

**Where a prefab's sockets are, without spawning it.** Those two are the **resident** reads — for
placing something in a presented scene. `ReadPrefabSockets(assets, prefab) →
AssetResult<vector<PrefabSocket>>` (same header) is the **CPU** read, for a process that reasons
about where things attach without presenting the model (a headless process, a planner, a test with
no render context). It walks the cooked prefab directly — never `Load`ing it, since a prefab load
makes every mesh it names resident — expands nested prefabs exactly as `Prefab::SpawnInto` does,
and reads each rendered mesh's socket table through `AssetManager::ReadMeshSockets` (see
[../Asset/CLAUDE.md](../Asset/CLAUDE.md)). Each `PrefabSocket { EntityName; Mesh; Local; RootSpace }`
carries the socket in **prefab-root space**: composed up the entity chain to, but not including, the
root, so the root's own `Transform` is left out and a root placed at world transform `W` puts the
socket at `W · RootSpace` — exactly where `AttachToSocket` onto the entity named `EntityName` lands.
Results sort by entity name then socket name. Nothing is cached; the caller caches.

**Which meshes a prefab renders, without spawning it.** `ReadPrefabMeshes(assets, prefab) →
AssetResult<vector<PrefabMesh>>` (same header) is the same walk asked a different question: it
reports every entity whose `MeshRenderer` names a cooked mesh (a recipe `Source` has no id, so it is
skipped), each as a `PrefabMesh { EntityName; Mesh; RootSpace }` with `RootSpace` composed exactly
as `PrefabSocket::RootSpace` is — the root's own `Transform` left out, so a root entity reports the
identity. It shares `ReadPrefabSockets`' flatten and compose code, so the two cannot disagree about
where an entity sits. It reads **no** mesh: the ids it returns are the input to the per-mesh CPU
reads, which is the point — a headless server finds a prefab's skinned mesh here, reads its skeleton
through `AssetManager::ReadMeshSkeleton`, and places joints by `W · RootSpace ·
Skeleton::JointModelTransform` with nothing made resident (see [../Asset/CLAUDE.md](../Asset/CLAUDE.md)).
Results sort by entity name, flattened authored order breaking a tie; an entity sharing another's
mesh is still reported on its own. Nothing is cached.

## Spatial version

A `Scene` carries a monotonic **spatial version counter** (`GetSpatialVersion()`): it bumps on any
change to a **spatial pool** (`Transform`/`Hierarchy`/`MeshRenderer`) — a structural
`Add`/`Remove`, a `DestroyEntity` touching one, a **non-`const`** access (the mutable
`Get`/`View`/`Each` path, a potential in-place edit), or a `ForEachComponent` visit (the editor
inspector's erased-`void*` edit path). A mutable `View`/`Each` over a spatial type bumps it **once**,
when it is created over a non-empty driving pool, not once per entity it visits; each component it
hands out is still stamped with the change tick below. A **`const`** `View`/`Each` does **not** bump
it, so a read-only consumer iterates without forcing a version move. This is the access-as-write
change-tick a consumer (the `SceneBroadphase`) gates its re-gather on: it caches the version it
last built against and re-gathers only when the version moved. A narrower **topology version**
moves (with the spatial version) only on a change to which entities carry a `Transform` or
`Hierarchy` or to who parents whom; the [world-transform pass](#world-transforms) rebuilds its
order on it. One constraint: a `Transform&`
retained across frames and written without re-acquiring it bypasses the bump (and so leaves the world-transform pass current over a stale matrix) — write transforms
through the scene accessors each frame, as all engine and sample code does.

## Change ticks — and why zero is reserved

Beside the scene-wide spatial version, a `Scene` keeps a **per-(entity, component) change tick**. A
non-`const` access stamps the touched pair with the scene's current change tick
(`SetChangeTick`/`GetChangeTick`); `GetComponentChangeTick(entity, id)` reads it back. The world
drive sets the scene's change tick from `SystemContext::Tick` each phase, so an edit made during a
tick is stamped with that tick. It is the per-component granularity the net layer's delta gate keys
off — a component enters a snapshot for a connection when its tick is **strictly greater** than what
that connection has acked.

**Zero is a reserved sentinel meaning *before any tick*, and no write ever produces it.** The scene's
change tick floors at `Scene::MinChangeTick` (one): `m_ChangeTick` initializes to it, `SetChangeTick`
raises anything lower to it, and the world drive's assignment routes through the setter, so it cannot
be lowered either. The consequences are the contract:

- `GetChangeTick()` is **never zero**, from construction onward. There is no value that means "this
  world has not ticked yet" — a consumer needing that fact must key off its own load/boot state.
- `GetComponentChangeTick()` returns zero **if and only if** the entity does not carry the component.
  A component it does carry was stamped when it was added, at a tick of at least `MinChangeTick`, so
  it reads non-zero however early it was written.

The reason is the delta gate. Zero is also what a replication baseline holds before anything is
acked, so overloading it as a real tick during which writes happen made a genuine pre-tick write —
level load, prefab spawn, editor authoring — indistinguishable from never-written, and every
`changeTick <= sinceTick` gate read it as clean. Flooring at one fixes the ambiguity at its source
rather than special-casing each comparison, and leaves every comparison correct and unedited. The net
side of this is [Veng/Net](../Net/CLAUDE.md), "The tick-zero floor".

## Ownership & the owned simulation

A `Scene` is **`Unique`, single-owner** — nothing holds a `Ref` to it; the app (or the
engine-managed world) owns it and a renderer reads it per frame as a `const Scene&`. The
`TypeRegistry` it was created with (`Scene::Create(TypeRegistry&)`) must outlive it and must
already have every component type registered.

**A `Scene` optionally owns the `SceneSimulation` that drives it.** `Scene::SetSimulation`
attaches one and `GetSimulation` returns it; `StartSimulation` / `TickSimulation` /
`StopSimulation` forward `*this` to the held simulation (no-ops when none). `Level::LoadInto`
builds the level's simulation and attaches it here, so the running scene is a self-contained
bundle. The simulation is optional — a bare `Scene` (a static render source, a test world) has
none. `Clone()` does **not** copy the simulation: the editor's Play clones its document's scene and
opens the clone as a runner world (`WorldRunner::OpenWorld` over a scene), which attaches one built
from the document's system set.

## Builtin components

The builtins are plain reflected components, pre-registered identically to a game's own: `Name` (a
display label), `Transform` (**local** TRS — `Position`/`Rotation`/`Scale`, never a world matrix),
`Hierarchy` (the intrusive scene-graph link — a `Parent` up-edge plus the ordered child list,
mutated through `SetParent`/`Detach`/`MoveBefore`; world matrices compose along the `Parent` edge
as `parent.world * local` — see [World transforms](#world-transforms) for the per-frame pass), `Camera`
(the component whose FovY/Near/Far and world transform build a `CameraView`, the value type
carrying the view/projection — Y flipped for Vulkan clip space), `MeshRenderer` (holds the
`AssetHandle<Mesh>` a draw queries — the mesh owns its materials, so a renderer queries
`(Transform, MeshRenderer)` and draws each submesh with its material — plus two independent
switches and a layer: `CastsShadows` drops it from the shadow views alone, **`Visible`** drops it from
the gather entirely, so it is neither drawn, nor a caster, nor part of the scene bound, and **`Layer`**
(a `RenderLayer`, default `Default`) names the render-visibility layer it sits on — a view draws it
only when the view's mask names the layer, so `RenderLayer::ViewAnchored` marks a camera-anchored mesh
the ordinary view still draws but an environment probe skips, and `RenderLayer::Display` marks content
presented to a viewer — a readout, a holographic instrument — which a probe skips likewise. `Ribbon`,
`Trail` and `RibbonPath` carry the same `Layer`, honoured the same way. `Visible` is
what hiding something reaches for: removing the component loses the resolved mesh and any sibling
bound onto it — a `GuiSurface` draws its document *into* that mesh — and collapsing the
`Transform` scale takes every child with it, a camera riding the hull included; and a runtime,
never-cooked **`InstanceMaterials`** override that, when non-empty, replaces the mesh asset's shared
material list for *this* entity's draw only — same length, indexed by `SubMesh::MaterialIndex` — so
one entity draws with its own instances while every other sharer of the mesh keeps the asset's, the
seam the capture path uses to scope a probe to one entity, see
[../Renderer/CLAUDE.md](../Renderer/CLAUDE.md)), `Animator` (plays an
`AssetHandle<Animation>` on a skinned-mesh entity; the `AnimationSystem` writes the result into a
transient `SkinnedPose` the renderer uploads — or, when the entity also carries an `AnimationBlend`
or `AnimationStateSet` (`Veng/Scene/AnimationBlend.h`), the phase-synced 1-D blend of the bracketing
locomotion clips with a named state crossfaded over it, composed in pose space into the same
`SkinnedPose`; an Animator with neither is the single-clip path unchanged, and the builtin
`CharacterAnimationSystem` maps a `CharacterState` onto the blend's `Parameter` and requested
state), `JointOverrides` (local rotations by joint name on a skinned-mesh entity — a swivelling
mount, a spinning part — posed by the same `AnimationSystem`, the one writer of `SkinnedPose`: with
no `Animator` the pose is the bind pose with each rotation post-multiplied onto its joint's bind
rotation, and with one each rotation is post-multiplied onto the sampled or blended local rotation,
so a clip and a procedural turn compose; a joint turns about its own axes and carries exactly its
subtree. Names resolve once per skeleton into a cache on the component, an unknown name is ignored
with one warning, and there are no limits — the caller clamps. The CPU read of a posed joint,
`Skeleton::JointModelTransform`, is in [../Asset/CLAUDE.md](../Asset/CLAUDE.md)), `Light` (a directional light —
`Direction`/`Color`/`Intensity`; `SceneRenderer::Execute` selects the first `Light` entity into
the `SceneView`, or a zero-intensity default → flat ambient when the scene has none), and
`ViewPose` (a fieldless runtime-only tag marking an entity whose `Transform` is authored per
frame in the View phase — a camera-anchored impostor, a billboard: the render gather resolves
its own local transform live instead of blending the scene's two-tick history, which holds
earlier frames' writes and would render it a frame stale; ancestor levels keep their own
interpolation), and `FlipbookSprite` (plays an `AssetHandle<Flipbook>` as a camera-facing sprite
— size, HDR tint, opacity, a playback rate or a duration override, a screen-plane roll, and a
`SpriteBlend` of `Asset`, `Alpha` or `Additive`; the View-phase `FlipbookSystem` advances its `Time`
and sets its runtime-only `Finished` once a one-shot sequence has played, and a finished sprite
draws nothing — see [../Renderer/CLAUDE.md](../Renderer/CLAUDE.md), "Flipbook sprites"), and
`Ribbon`, `Trail` and `RibbonPath` (see [Ribbons and trails](#ribbons-and-trails) below).

## Ribbons and trails

**A `Ribbon` is a straight camera-facing band between two world points; a `Trail` is a band through
the recent positions of the entity it sits on; a `RibbonPath` is an authored shape of polylines that
rides its entity.** All three are presentation (never replicated, drawn by
the renderer's ribbon pass — [../Renderer/CLAUDE.md](../Renderer/CLAUDE.md), "Ribbons and trails"),
carry HDR colour that feeds bloom, and choose `Additive` (order-free light) or alpha-over.

- **`Ribbon`** — `From`/`To` are **world space and ignore the entity's `Transform`**, so the owner
  moves the endpoints directly (a beam from a muzzle to an impact, a streak advanced along a path).
  Width, colour and opacity run linearly `From` → `To`. A positive `Lifetime` fades it linearly to
  nothing as `Age` advances; `Lifetime` 0 holds it.
- **`Trail`** — the View-phase **`RibbonSystem`** calls `AdvanceTrail` each frame with the entity's
  world position at the frame's render fraction: samples age, those at `Lifetime` drop, the head is
  recorded when the trail is empty or has moved more than `MinSampleDistance`, and the ring is
  trimmed to `MaxSamples`. The runtime `Samples` carry no `VE_FIELD`. The drawn band is full at the
  head and fades (opacity to 0, width to `Width × TailWidthScale`) by each sample's age, so a trail
  that stops moving empties within `Lifetime`. Clearing `Emitting` stops recording and detaches the
  head, so the trail drains where it lies; destroying the entity takes the trail with it.
  **`AttachTrail(scene, entity, trail)`** adds or replaces one with its samples cleared, so a reused
  or teleported entity starts fresh instead of streaking from where it stood. A prefab may carry a
  `Trail`; it stands on every peer that instantiates the prefab.
- **`RibbonPath`** — `Strips`, each a `RibbonStrip` of `Points` in the entity's **local** space with
  its own `Width`, `Color`, `Opacity` and `Closed`, all drawn `Additive` or alpha-over together. The
  points follow the entity's drawn (interpolated) world transform, so a wireframe, a drawn orbit, a
  range ring or an in-world reticle moves, turns and scales with what carries it — one entity and no
  per-frame world-space bookkeeping, where a `Ribbon` per segment would need both. The entity's world
  scale multiplies `Width`; a path on an entity without a `Transform` draws nothing. A closed strip
  joins its last point to its first with a shared joint (a circle of N points is N segments with no
  seam), coincident consecutive points merge, a strip of one distinct point draws a round,
  camera-facing dot of diameter `Width` (a marker, a vertex, a node, with no second point to invent),
  and a strip of none draws nothing. **`Placement`** (`RibbonPlacement`) picks where in the frame
  the path draws: `Scene` (the default) with the rest of the scene, resolved by TAA and resampled by
  the upscale with it; `PostResolve` at the output resolution after both, through the unjittered
  projection, so thin bright linework (a gizmo, a projected orbit, a wireframe hologram, a
  measurement guide) stays crisp — occluded per fragment by the scene depth and still blooming
  (renderer detail in [../Renderer/CLAUDE.md](../Renderer/CLAUDE.md), "Ribbons and trails"). The
  strips and the placement are fully reflected (`VE_ARRAY_FIELD`, `VE_FIELD`), so a prefab can
  author a static shape, and are equally plain data a system rewrites every frame; nothing advances
  them, and a floating origin's re-base reaches them through the entity's `Transform`.
- **`RibbonSystem`** (`Veng/Scene/RibbonSystem.h`) also advances every `Ribbon`'s `Age`, so a level
  lists it for its trails to record and its ribbons to fade.
- **A floating origin re-bases them with `OffsetRibbons(scene, offset)`.** A scene drawn about a
  moving origin moves every world position by one displacement each time the origin moves; a
  ribbon's endpoints and a trail's recorded samples are world positions written on earlier frames,
  so they do not follow by themselves, and a trail left alone streams off along its viewer's own
  motion. The origin's owner calls it with the displacement before `RibbonSystem` records the frame;
  entity `Transform`s stay the owner's to re-base.

## Transient effects — a bounded pool, spawned and forgotten

**`SpawnTransientEffect(scene, desc, pose, lifetime) → Entity`** (`Veng/Scene/EffectPool.h`) stands
a short-lived effect — an `EffectDesc` of an optional `FlipbookSprite`, an optional `Ribbon` and an
optional `Light` — at a pose, and the caller forgets it: the scene's **`EffectPool`** returns the
entity to a free list when its lifetime ends, or once every visual it carries has ended (its sprite
finished and its ribbon faded; a ribbon with no `Lifetime` and a looping sprite never end by
themselves). **`SpawnTransientBeam(scene, ribbon, lifetime)`** is the beam front door: the ribbon
alone, its `Lifetime` set to `lifetime` so it fades out as it goes, posed at its `From` end; the
entity may be moved or its ribbon edited while `IsLive`, which is how a moving streak is advanced,
and the level must run `RibbonSystem` (the fade) beside `FlipbookSystem` (the retire). The pool is **scene-owned** (`Scene::SetEffectPool` /
`GetEffectPool`, a `Unique` like the pose history; `Clone()` does not copy it), so its bound is per
scene, and the first `SpawnTransientEffect` on a scene installs one of `DefaultEffectPoolCapacity`
(64) — a consumer wanting a different bound installs a sized `EffectPool` first. `FlipbookSystem`
updates it each frame after advancing the sprites, so the level must run `FlipbookSystem`.

- **The bound is hard.** A spawn reuses a free entity, creates one while under capacity, and
  otherwise recycles the **oldest** live effect — the pool never grows past its cap.
- **Pooled entities are `Tier::Local` roots carrying `ViewPose`.** An effect is each peer's own
  presentation and never replicates; `ViewPose` makes a reused entity resolve its new pose live
  rather than blending from where its previous effect stood. A free entity keeps its `Transform`
  and carries no sprite, ribbon or light, so it draws and lights nothing.
- **An `Entity` from `Spawn` is only this effect's until it ends.** Entities are reused, so a kept
  handle names a later effect afterwards; `IsLive` answers until then, and a caller wanting to follow
  an effect copies what it needs at spawn. `Retire` ends one early. A lifetime of 0 or less lets the
  sprite alone decide, so a looping sprite with no lifetime lives until recycled or retired.

## Presentation scopes — what a scene's sound and rumble does this frame

**A scene owns its presentation scope the way it owns its effect pool.** A
**`PresentationScope`** (`Veng/Scene/PresentationScope.h`) is a `Unique` handle the scene holds
(`Scene::SetPresentationScope` / `GetPresentationScope`): installing one replaces and closes the old,
passing null detaches and closes it, destroying the scene closes it, and `Clone()` does not copy it.
Its **`PresentationScopeId`** is a `u64` minted by the **`PresentationScopes`** registry and never
reused, the tag a device engine files the output a scene owns under. Nothing in the scene points at the
scope — no component holds it — so the order `~Scene` destroys its members in is irrelevant to it: the
scope's closure is what ends everything it owns. The `WorldRunner` installs a fresh scope on every
scene it holds (at `OpenWorld`, and on `InstallScene`'s replacement, the replaced scene's scope closing
with it); a scene it does not hold — an editor preview, a capture's private scene — has none, presents
nothing, and gets no `SystemContext` (`WorldRunner::BuildContext` asserts the scope). A test's runner
takes its registry from the `TestServices` bundle (`GetPresentationScopes()`), declared ahead of the
runner since every scope borrows the registry until it closes.

**The View phase holds the scope by lease.** `TickSimulationPhase` renews the scope at the top of every
`Phase::View` pass, before any View system runs, audible when the pass's `SystemContext::View` is set
— so the lease belongs to the phase, and a level listing no audio or haptics system still holds and
releases its scope. Once per frame, after every world's View pass and `OnUpdate`, the application's
presentation step calls `PresentationScopes::Resolve`, which latches each open scope's
**`PresentationState`** and clears the renewals, so a lease is exactly one frame:

| state | latched when | the contract for output a device files under the scope |
|---|---|---|
| `Live` | renewed, audibly | advances, and is heard or felt |
| `Muted` | renewed, not audibly | advances in time and produces nothing (zero gain, zero rumble), so a scene that becomes presented resumes in step |
| `Held` | not renewed (and from `Open` until the first renewal) | frozen — no cursor or time advance, nothing heard or felt — resuming exactly where it stopped |
| `Closed` | the handle dropped, or an id never handed out | stopped and released at the device's next update |

What follows with no further code: a **paused** world runs no View pass, so its scope is `Held`; a
dedicated server runs no View phase, so every scope it holds is `Held`; a world **closed mid-tick** has
its scope closed by the deferred drain, before that frame's `Resolve`; and an open world **no viewport
presents** has an empty `View` and is `Muted`. Presentation is exactly what the context factory resolves
`View` from (`FindPresentingViewport`, then a self-driven `Presented` viewport), so a viewport that is
registered and retains the world's scene counts as presenting it. A viewport retains its scene until
its owner stops showing it: an **on-demand** viewport (`ViewportInfo::RenderOnDemand`) whose owner lets
a whole frame pass without pushing a view — an editor document's Play viewport in a hidden dock tab —
releases its scene at that render (`Viewport::IsShown` reads false), so its world reads `Muted` while
its time advances, and a bound viewport so released stops counting for `IsWorldPresented` and
`CollectPresentingSeats` too, until its owner pushes again. The registry's one **application scope** (`GetApplicationScope()`) is always `Live`; it
owns what plays outside any scene, and nothing a scene's system starts belongs in it.

Two device engines file under scopes: the **audio engine** — every voice, started through the
`ScopedAudio` facade a scene's `SystemContext::Audio` (or a Gui driver's frame) is, with a listener per
scope ([../Audio/CLAUDE.md](../Audio/CLAUDE.md)) — and the **haptics engine** — every rumble one-shot
and layer ([../Haptics/CLAUDE.md](../Haptics/CLAUDE.md)). Each judges what it holds once per frame in
the presentation step, after `Resolve`.

## Bounds & broadphase inputs

A `Scene` reduces to a world-space bound on demand: `SceneBounds(scene)`
(`Veng/Scene/Transforms.h`) unions every resident `(Transform, MeshRenderer)` entity's world-space
mesh bound, reading the scene's [world-transform pass](#world-transforms); the bound itself is not
cached. `GatherMeshes` (`Veng/Scene/Visibility.h`) is the pure one-shot candidate gather over the
`MeshRenderer` pool and that pass (world matrix + world-space `AABB` + resident mesh per entity,
skipping a renderer whose `Visible` is clear or whose `MeshRenderer::Layer` is absent from the
caller's `layerMask` — the one place both filters are honoured, so nothing downstream re-tests
them); the `SceneBroadphase` caches that gather and builds the BVH from it, re-gathering only when
the scene's spatial version moves (or a still-loading mesh becomes resident, or the exclude or layer
mask the caller's view carries changes). A re-gather that yields the same candidates — same
entities, same meshes, same order — **refits** the tree bottom-up to their moved bounds instead of
rebuilding it; a changed set rebuilds, and so does a refit that has pushed the tree's surface-area
cost past `SceneBroadphase::RefitCostLimit` times its cost at build, so a refit never leaves a
degraded tree in place for long. The broadphase is a BVH
with **per-submesh leaves**, the granularity the renderer's GPU-driven hi-Z occlusion culling
operates at. Each `Mesh` carries a local-space `GetBounds()` derived from its canonical vertex
positions at load, and each `SubMesh` a local-space `AABB` folded over its index range. Both build
on `AABB` (`Veng/Math/AABB.h`), the engine's glm-only bounds primitive — a min/max `vec3` pair
with the union/expand/center/extents/corners/transform algebra and an empty sentinel. `Frustum`
(`Veng/Math/Frustum.h`) is its visibility companion — six bounding planes extracted
Gribb-Hartmann from a view-projection matrix (Vulkan ZO clip), with a conservative
`Intersects(Frustum, AABB)` p-vertex test (never a false cull).

## World transforms

A `Scene` computes its world matrices in **one parent-first pass**:
`Scene::UpdateWorldTransforms()` writes every entity's world matrix into an array indexed by
`Entity::Index`, visiting entities in an order where a parent precedes its children, so each costs
one multiply onto its parent's entry. The order is rebuilt only when the **topology version**
moves — a `Transform` or `Hierarchy` added or removed, a `SetParent`/`MoveBefore`, a destroy, or a
non-`const` `Hierarchy` access — and a cycle or dangling parent asserts when it is built. The pass
itself is skipped while the spatial version has not moved, and allocates nothing at steady state.
`WorldMatrix(scene, entity)` reads the entry while the pass is current and walks the chain
otherwise, so a reader is never handed a stale matrix: any spatial change makes the pass stale
until the next update. The update is `const` — the pass is a cache derived from the scene, moving
no version and stamping no change tick — so the read-only render gather brings it current; it must
not run while another thread reads the same scene's matrices. `GatherMeshes`, `SceneBounds` and
`ComputeWorldMatrices` bring it current themselves; any other reader that wants O(1) lookups across
many entities calls the update once first.

`Scene::UpdateInterpolatedWorldTransforms(alpha)` is the same pass over the two-tick history,
composing each level exactly as `GetInterpolatedWorldTransform` does (a `ViewPose` level live), and
`GetInterpolatedWorldTransform` at that alpha then reads its entry. It is keyed by the spatial
version, the two history captures and the alpha, so the renderer's interpolation computes it once a
frame and a later reader at the frame's alpha (the sprite and ribbon gathers) reuses it.

## Cameras & seats

**Camera is selected per seat and resolved to a `CameraView`.** A camera is `(Transform, Camera)`
data; a **`Viewer { Entity Camera }`** component is a *seat* (a local player, a render target, the
editor viewport) naming the camera entity it renders through, separating seat from camera. Two
pure helpers beside `MakeCameraView` (`Veng/Scene/Camera.h`) resolve a seat to the view the
renderer consumes: **`ResolveCameraView(const Scene&, Entity viewer, f32 aspect) →
optional<CameraView>`** reads the seat's `Viewer`, looks up its `Camera` entity, and projects
through its `WorldMatrix` (so a camera parented under a rig resolves correctly);
**`ResolvePrimaryCameraView(const Scene&, f32 aspect)`** is the one-seat convenience — first
`Viewer`, else the first bare `(Transform, Camera)` entity. **Aspect is a render-target property,
never a `Camera` field** — the caller passes it (output extent in the runtime, panel extent in the
editor). The renderer consumes only a resolved `CameraView` through `SceneView`, so the runtime's
in-scene camera and the editor's external orbit `EditorCamera` (its own non-ECS camera,
`editor/src/EditorCamera.h`) coexist with no renderer involvement. The prefab editor's Play mode
can preview the scene's authored `Viewer` camera through `ResolvePrimaryCameraView` instead of its
orbit camera.

## Input → actions → PlayerInput → Intent

**Control flows raw input → actions → `PlayerInput` → Intent → Movement.** Raw device state is
bound to **named actions** through cooked, remappable data and resolved into a per-seat snapshot,
which a game-specific control system reads to produce the abstract `Intent` gameplay consumes:

- **The action-mapping layer** (`Veng/Input/`). An **`ActionId`** is a minted `u64` leaf (authored
  like `AssetId`/`TypeId`); an action *exists* by being declared in a context, so there is no
  registry. An **`InputMappingContext`** (`AssetTypes::InputMap`) declares its actions
  (id + name + `ActionKind`, and optionally an engine `ActionRole` with its repeat) and a
  `vector<Binding>` (raw `InputSource` → action, with a signed scale, an axis component, and a
  threshold and response exponent). **`ResolveActions(activeContexts, raw, previous) →
  ActionState`** (`Veng/Input/Actions.h`) is the pure, device-free core — bindings × the active
  context stack × the raw snapshot → each action's value + phase, phase derived by comparing
  against the previous `ActionState`. It is unit-tested with no window, mirroring the
  `DecideBarrier`/`ComputeCascades` pure-core pattern; `RawInput` (`Veng/Input/RawInput.h`) is the
  thin adapter from `Veng::Input` to the resolver's `RawInputView`.
- **`PlayerInput` *is* the resolved `ActionState`.** The per-seat serializable snapshot is a
  game-defined set of `ActionSample { ActionId; vec2 Value; ActionPhase }`, read by name
  (`input.GetValue(Actions::Move)`, `input.WasTriggered(Actions::Jump)`) — not a fixed
  `{Move, Look, Buttons}` struct. It serializes through the reflection serializer's name-keyed
  `FieldClass::Array` encoding (each sample self-describing by its `ActionId`), so it rides the
  ordinary cook/load/replicate path with no bespoke wire format.
- **`InputContextStack`** is a per-seat component holding the ordered active
  `AssetHandle<InputMappingContext>` (highest priority last), authored on the player prefab.
  Gameplay systems push/pop it to switch schemes (enter a vehicle → push the `vehicle` context);
  popping to empty neutralizes the seat's input. It is the fine-grained sibling of the
  `InputRouter`'s coarse focus stack. A system drives that **coarse** stack — capturing or releasing
  a seat's gameplay focus — through the builtin **`FocusRequest`** component (`Veng/Scene/Requests.h`):
  it stamps `FocusRequest{ Focus = Gameplay }` (a `Null` seat means the cursor seat) to capture and
  `{ Focus = UI }` to release, and the engine drain owns a single per-seat focus token behind it,
  reconciling idempotently. This lets a stateless system drive focus — which a `FocusToken` held
  across frames otherwise could not — and the request-driven token composes with, and never pops,
  a token pushed by an overlay suspend or a `SeatFocusScope`. When the seat's world closes the engine
  drops that token along with every router entry naming the world (`InputRouter::ForgetWorld`, which
  retires the tokens it drops, so their holders' later pops are silent). It is a local-only request like its
  siblings; see **The system catalog** and the request family in `Veng/Scene/Requests.h`.
  **The engine binds no key to releasing it.** An application declares a Button action with the
  **`ReleaseFocus`** role in a map its seat resolves under gameplay focus; the role resolver (below)
  pops that seat's top gameplay entry on the press — through the `FocusRequest` drain's token when
  that is the one holding it, so a later `FocusRequest{ Gameplay }` captures afresh. **Window-focus
  loss suspends rather than pops**: the cursor seat's gameplay entry reads as UI while the window is
  away, its token still live, and refocusing resumes it and recaptures with no click, whatever device
  the player uses. Only what focus loss took is restored — a release made before it, or a token its
  owner pops while the window is away, is not brought back — and `--background-input` suspends
  nothing. An application that binds no release still cannot trap the cursor, since alt-tab frees it.
- **`InputMappingSystem`** (`Veng/Scene/InputMappingSystem.h`) is the builtin Sim system that
  resolves each locally-owned seat's `InputContextStack` against the raw snapshot into that seat's
  `PlayerInput`. It is the **sole Sim-side reader of raw device state**, registered in
  `RegisterBuiltinSystems` ahead of any control system — a level's explicit `systems` order must
  place it before the control system that reads `PlayerInput` (registration order does not reorder
  the list). It iterates `(Viewer, InputContextStack, PlayerInput, SeatInput)` seats, so a world
  with none — the input-free minimal template — resolves nothing, a clean no-op with no guard; in
  headless the neutral snapshot resolves to all-`None`. `IsLocallyOwned` decides which seat this peer
  owns: a joining client publishes a `LocalSeat` marker on its own seat (removed on release) and the
  predicate answers `true` only for the marked one; a host with no marker answers from `Authority`
  (a seat a remote connection owns, `Owner != 0`, is that peer's); with nothing published — single
  player, headless, a host's own seat — every seat resolves locally, the pre-replication default. The
  three first-match seat scans (`ResolvePresentationSeat`, `StampLocalSeatInput`, `FirstKeyboardSeat`)
  prefer the locally-owned seat through it. The last two fall back to the first seat; a presenting
  viewport does not, so a host scene seating only remote peers leaves the viewport unseated until its
  own seat exists, rather than presenting another peer's.
    **An edge has two cadences, per tick and per frame.** `WasTriggered`/`WasReleased` read the
  tick's `Phase`, for a Sim system that sees every tick. A View system or per-frame code reads
  `WasTriggeredThisFrame`/`WasReleasedThisFrame`, which `InputMappingSystem` ORs across a frame's
  steps and resets on its first, so an edge on a non-final step of a multi-step frame survives to the
  View pass. A frame that runs **no** step — every other frame when the display outpaces the tick
  rate — never reaches the system, so the world drive (`WorldRunner::Tick`, the editor's Play
  included, since Play is a runner world) clears the frame edges with `ResetFrameActionEdges` before that frame's View pass, leaving
  `Phase` and `Value` for the next step to derive from. A **paused or unstarted** world runs no step
  either and is cleared the same way, though it gets no View pass, so per-frame code reading its
  `PlayerInput` sees no edge for as long as it is paused. A frame edge is therefore read on exactly
  one frame: the first that ticks after the input lands.
    **The raw snapshot is held for a step the same way, across every simulation driving a frame.**
  `Input::BeginFrame` holds the pressed/released edges after a frame on which something simulated
  and nothing stepped, deferring a tap's release so the next step still reads it down. Per-tick
  motion follows the same rule: what no step has consumed is kept while anything simulates, and
  dropped at the top of a frame following one on which nothing did. Both are decided by the
  `SimInputFrame` `Application` owns (`Veng/Input/SimInputFrame.h`), which also carries the frame's
  pointer scope and prepares every step — the pointer latch for the routed scene's steps, the
  touchpad latch for every step. The runner's worlds report into it as one — the editor's Play among
  them — so there is no second drive to feed it.
    **A pause drops a press it lands on; a key held through it is not lost.** A frame on which nothing
  simulates rolls the raw snapshot like a UI, so a tap pressed and released while the world is paused
  — or still latched for a step when the pause lands — never reaches a step, as the paused clock
  chases no backlog. A key still down when the world resumes is
  read by level: the first step derives `Started` from the `Phase` the world paused on, so a press
  made during the pause fires on resume, and one already `Ongoing` at the pause does not fire again.
    **A context can be gated on gameplay focus as authored data.** An `InputMapData`
  (`Veng/Asset/InputMappingContext.h`) carries a reflected **`RequiresGameplayFocus`** flag
  (authored `"RequiresGameplayFocus"`, tolerant-read so existing cooked maps are unchanged); when
  it is set, `InputMappingSystem` **excludes** that context from the seat's effective active list
  whenever the seat lacks gameplay focus (`SystemContext::GameplayFocused`, stamped from
  `InputRouter::IsGameplayFocused()` and `false` headless). This is **pure evaluation at list
  assembly** — the authored `InputContextStack::Active` is never mutated and the order of the
  remaining contexts is unchanged — so a mouse-look context that should not resolve while a menu
  holds focus declares the gate rather than a system lifting and re-inserting it from a saved
  index. It composes orthogonally with the coarse `FocusRequest`/`SeatFocusScope` focus stack (a
  map screen's authored stack *swap* is deliberate state change; this gate is evaluation).
  **The click that captures the cursor never reaches a gated context.** The capture lands while that
  click is still held, so a gated binding on the button would read it held from the first tick the
  context resolves — a click to take the cursor back would also fire. The router therefore withholds
  every mouse button held as the cursor is captured (`Input::WithholdHeldMouseButtons`): it reads up,
  its release is no edge, and the next press is the game's. Keys are not withheld, so a key held
  through the click keeps driving.
- **`SeatInput` scopes the raw read *per seat*.** A reflected **`SeatInput`** component
  (`Veng/Scene/Components.h`, `UsesKeyboardMouse` + a `Gamepad` id + `WantsGamepad`) on the
  `Viewer` seat names that seat's devices; `InputMappingSystem` builds each seat a filtered
  **`SeatInputView`** (`Veng/Input/RawInput.h`) and resolves against it, so two seats with
  different assignments produce distinct `PlayerInput`s. The view's arms route two ways: a
  **gamepad** arm reads *only* the seat's assigned pad (by `GamepadId`); a **keyboard** arm reads
  the shared keyboard only when the seat sets `UsesKeyboardMouse`; a **pointer** arm is gated
  *both* by `UsesKeyboardMouse` *and* by owning the cursor's viewport region this frame (position
  reported viewport-local, look-delta left raw and sensitivity-invariant). Region routing is
  **inert while the cursor is captured** — a captured pointer belongs wholly to the single
  `UsesKeyboardMouse` seat (delta-look needs no position); it applies only for a free cursor. The
  `InputRouter` computes the per-frame `PointerRouting` (which seat owns the free pointer,
  hit-testing `WindowToViewport` against each associated viewport's region in association order);
  `Application` threads it onto the `SystemContext`. A **`DeviceAssignmentSystem`** (a Sim system
  registered before `InputMappingSystem`) reconciles each seat's `Gamepad` against
  `Veng::Input::ConnectedGamepads`: a connected-but-unheld pad is auto-assigned to the first
  `WantsGamepad` seat with a `None` slot, a disconnected slot is cleared, a level-authored slot is
  respected. **A seat with no `SeatInput` is skipped** — its `PlayerInput` is synthesized or
  replicated (the AI/remote path) — so every local human seat must author one.

A game-specific **control** system reads `PlayerInput` by action id and writes the abstract
**`Intent`** command (local-frame move, a three-axis rotational command — yaw, pitch, roll, of
which the built-in upright mover consumes only yaw and pitch — and an action bitset); a
**movement** system
(`MovementSystem`, `Veng/Scene/Movement.h`) and gameplay systems generally consume `Intent` and
mutate state, scaled per pawn by an optional **`Mover`**. **The layering invariant:** actions →
`PlayerInput` → control system → `Intent` → gameplay; **only** the control system reads actions,
and gameplay reads `Intent`. `Intent` is the serializable chokepoint the net layer predicts and
rolls back and the uniform interface AI and remote players write through — both are drop-in
`Intent` producers that never touch an action or a context, no movement change. (The net layer
replicates `PlayerInput` — the action snapshot — for a human seat and re-derives its `Intent`
server-side; AI and server-authoritative producers write `Intent` directly.) `Veng::Input`
(`Veng/Input.h`) carries a gamepad device surface backing the gamepad `InputSource` arms. Pads are
read through **SDL3's gamepad subsystem** (`engine/src/Platform/GamepadBackend`; GLFW keeps the
window, keyboard and mouse), only when the app has a window, and polled once per frame into the
same snapshot as keyboard/mouse. A pad is tracked by `GamepadId`, a small **slot** (0..15) the
backend's slot table assigns: SDL's own ids grow per connection, the slot does not, and a freed
slot reads disconnected for a frame before another pad may take it, so `DeviceAssignmentSystem`
always sees a seat's pad leave. The surface is queried through `IsGamepadButtonDown` /
`GetGamepadAxis` / `GetGamepadType` / `GetGamepadName` / `ConnectedGamepads`, with
connect/disconnect raised as events. The button and axis enums are positional and
**append-only** (a cooked binding stores the index): the Xbox-layout set, then `Misc`, the
touchpad's click and touch, and four back paddles; the axes add the first touchpad finger's
position and motion. **Touchpad motion has two cadences, like the mouse**: per frame through
`GetGamepadAxis`, per Sim tick through `GetSimGamepadAxis`, latched per pad on every simulation's
step (`Input::BeginGamepadSimTick`, not tied to the pointer routing), zero on the tick a finger lands.
**Physical pads read neutral while the window is unfocused** (still connected, so a seat keeps its
pad) unless background input is retained, matching keyboard and mouse. **Virtual pads** occupy
slots like physical ones and are driven through `VirtualGamepadEvent`, posted to
`InputRouter::PostInjectedEvent` (MCP's `input.send` pad events use exactly this), so automation
reaches every pad path without hardware. A `SeatInputView`'s gamepad arm reads the seat's assigned
pad through it, at the per-tick cadence. A pad's **motors** are written only by the haptics engine;
a seat's rumble resolves through the same `SeatInput::Gamepad`, read in the seat's own scene — see
[../Haptics/CLAUDE.md](../Haptics/CLAUDE.md).

**A source's value is shaped in two places, and nowhere else.** The device layer shapes a pad as
`Input::IngestGamepadStates` takes it in: each stick's two axes go through one **radial** deadzone
(`ShapeStick`), so a diagonal leaves the zone on both axes at once and keeps its direction, and each
trigger through its own (`ShapeTrigger`); both rescale the remainder so the value is continuous at
the zone's edge and full at full deflection, and a resting stick reads exactly zero. The zones are
per pad (`SetGamepadDeadzones`, reverting to the `GamepadDeadzones` defaults when the pad leaves),
virtual pads are shaped like physical ones, the touchpad's position axes are not deflections and
pass through, and `GetRawGamepadAxis` keeps the unshaped value for a diagnostic display. A stick
needs this at the device layer because only there are both of its axes seen together. Then each
**`Binding`** shapes its scaled source for the action it drives: a `Threshold` (a Button action
reads the source as a half-axis pressed at that pull point, positive side only; an axis action
drops a source under it), and an `Exponent` response curve on axis actions. Both default to
a pass-through, so a binding authoring neither resolves its source unchanged; `ResolveActions`'s fake
`RawInputView` never passes through the device layer, which is why the zones are pure functions
tested on their own. A binding naming a **`Modifier`** (an `InputSource`; `InputDeviceType::None`,
the default, means none) is a **chord**: it contributes only while the modifier is down, read as a
button on its positive half-axis at `ModifierThreshold`. While a chord is **live** (modifier down,
and its action resolving from the chord's own context rather than shadowed by a higher one), every
plain binding on the same source, in any active context, is silent; `ResolveActions` gathers the
live chords' sources before accumulating anything, so suppression is order-independent. A **`Possesses { Entity Pawn }`** link names the pawn a
seat controls; possession is independent of `Viewer.Camera` (a spectator views without possessing;
a cutscene retargets the camera without un-possessing).

**An action can carry an engine meaning — a role — beside the value a control system reads.** An
`InputAction` tagged with an **`ActionRole`** is one the engine itself acts on: the navigation roles
(`NavigateUp`…`NavigatePrevious`, `Confirm`, `Cancel`) drive Gui focus (see
[../Gui/CLAUDE.md](../Gui/CLAUDE.md), "Navigation is mapped actions, never keys"), and
`ReleaseFocus` releases the pressing seat's gameplay focus (above). Role actions are
**not** read off `PlayerInput`. The engine's **role resolver** (`engine/src/Input/RoleResolver`)
resolves them once per **frame**, after the input lands and before any world ticks, for the implicit
seat and every locally-owned `SeatInput` seat in every world — paused ones included — against a
`FrameInputView` (`SeatInputView`'s device gating with the per-frame deltas; a key a router consumer
claimed reads up). A seat resolves its `InputContextStack` over the application's **default UI
context** (`Application::SetDefaultUiContext`, from the project's `"defaultUiContext"`); a stack a
`SeatFocusScope` swapped is marked **`Exclusive`** and the default beneath it fires nothing. It keeps
its own previous `ActionState` and **repeat timers** per seat (`RepeatDelay`/`RepeatRate` on the
action), and resolves whatever the seat's focus — only the dispatch is gated on focus, navigation
under UI and `ReleaseFocus` under Gameplay — so a press held across a focus change is never
`Started` twice: one key may carry both `Cancel` and `ReleaseFocus`, and the press that releases
focus never also cancels in the menu it uncovers. So `InputMappingSystem` is the sole reader of
raw device state on the **Sim** side; the role resolver is its frame-rate counterpart, which
reconciliation never replays.

## LocalControl — which pawn is mine

**`LocalControl { Entity Seat }`** (`Veng/Scene/LocalControl.h`) is the engine's answer to "which
pawn is mine?", carried by the pawn a **presenting viewport's own seat** possesses. Its derivation
is two steps and both are required: **the presenting viewport → the seat bound to it → that seat's
`Possesses` → the pawn**. *"An entity possessed by a `Tier::Local` seat" is not the rule* — on a
client every mirrored pawn carries its own instantiated local seat, so that test marks every pawn on
screen, which is the precise failure this marker exists to prevent. A client therefore marks exactly
its own pawn, a listen host marks the pawn its local seat controls, and a dedicated host —
presenting nothing — marks nothing.

The engine owns the whole lifecycle and **a consumer only ever reads it**.
`ReconcileLocalControl(scene, presentingSeats)` stamps and clears a scene's markers against the seats
presenting it; `ManagedViewportSet` runs it at a viewport↔seat rebind (both ends, so a departed world
keeps nothing stale), and `Application` runs it once per frame over every live world after the sim
ticks and the net pump. The presenting seats are every managed viewport's and every bound
presentation's (`CollectPresentingSeats`) — so an overlay's seat and the editor's Play seat are
marked as a managed viewport's is. That per-frame pass is a **reconciling sweep**, because possession raises no
engine-side event to listen to: `Possesses` is a plain component a game writes directly and, on a
client, one that changes through snapshot apply. Its cost is the presenting-viewport count, never a
scan of a scene's entities. Each marker move raises `Application::OnClientPossession`, which is the
marker's **event form** and fires in every mode rather than only under `--join`.

The marker is singular per presenting viewport and carries the seat it came from, so the consumer
read is **per viewport** — `ResolveLocalControlledPawn(scene, seat)` against the viewport's bound
seat (`ManagedViewportSet::GetViewportViewer`) — and split-screen gives each viewport its own pawn. A
flat "every locally possessed pawn" marker is deliberately not what this is. It is registered
fieldless (`VE_TYPE`), so it is **never replicated and never persisted**: it is per-process
presentation state, meaningless off the machine that derived it.

**The derivation requires the `Possesses` indirection, and that is a modelling commitment, not a
neutral rule.** A presenting seat that *is itself* the controlled entity — one entity carrying both
the `Viewer` and the gameplay state, with no separate pawn to possess — resolves to `Entity::Null`
and is never marked. That shape is reachable in ordinary use: a server-spawned seat whose
`SeatPrefab` is left null is created bare and then associated with a prefab whose **root is the
controlled entity**, so on the client the seat's wire id names that root and its replicated
`Possesses.Pawn` arrives `Entity::Null` — not because a reference cannot cross the wire (it can:
`MakeEncodeRef`/`MakeDecodeRef` carry a replicated `Entity` field as its target's `NetId`), but
because the host never assigned `Possesses.Pawn`, so null is what replicates. A consumer
in that shape gets no marker and no `OnClientPossession`, and — because the marker's absence reads
exactly like "this peer controls nothing" — the failure is silent rather than loud. Giving the seat
a real `Possesses` link, or a non-null `SeatPrefab` so seat and pawn are distinct entities, is what
brings a consumer inside the rule. (Where the pawn *is* assigned but names an unreplicated entity,
the encoder now says so — see [../Net/CLAUDE.md](../Net/CLAUDE.md), "The four faces of a null
reference".) Widening the engine rule to model a self-controlling seat is a
design question that has not been settled, so the limit is documented rather than papered over.

## Interaction — proximity focus as data, firing as a request

**`Interactable { string Verb; f32 Range; bool Enabled }`** and
**`Interactor { f32 Reach; f32 ConeAngle; Entity Focused }`** (`Veng/Scene/Interaction.h`) are the
two halves of "walk up to something and use it". The builtin **`InteractionSystem`** runs an
`Overlap` (the physics query) within each interactor's `Reach` every tick, keeps the `Interactable`
entities that are `Enabled`, fall inside the interactor's view cone (its local −Z is forward,
matching the socket/camera convention) and within their own `Range`, picks the best by angle then
distance, and writes it to `Focused` (`Entity::Null` with no candidate). It publishes the resolution
and nothing more — a prompt is a UI concern reading `Focused` and `Verb`, so the engine supplies the
resolution and never the presentation. A no-op scene with no `PhysicsWorld` clears every `Focused`.
Every pose it reads — the interactor's origin and facing, each candidate's position — comes through
the scene's [physics-pose resolver](#the-physics-pose-resolver--when-the-transform-chain-is-not-the-solvers-frame),
so focus and range resolve in the frame the solver integrates in, and a candidate's separation from
the interactor is differenced at double precision before it narrows to f32.

**The cone is a bearing to the candidate's origin, so a body the interactor is *inside* is exempt from
it.** That origin is a well-defined direction only for a candidate the interactor stands outside of;
for an enclosing body it is an interior point that may lie anywhere, including straight behind — so
without the exemption an interactable large enough to be **entered** (a vehicle cabin, a lift car, a
room-scale machine) is unfocusable from within it at *every* orientation, and no `ConeAngle` under a
half turn fixes it. It is the same statement the near-coincident exemption already makes, at body
scale rather than at the numerical limit. The set of enclosing bodies is one small-sphere `Overlap`
about the interactor's origin, resolved at most once per interactor and only when the cone rejects a
candidate, so an interactor with nothing behind it pays nothing. An enclosing candidate keeps its
**true bearing** for the best-candidate ranking, so anything genuinely looked at still wins over the
room one is standing in. **`Range` is deliberately not exempt** — it stays a plain authored distance
budget to the origin, so an enterable interactable authors a `Range` that covers the offset from its
origin to an interactor inside it.

**Firing is a request, not a callback.** An **`InteractRequest { Entity Interactor; … }`** is stamped
on the focused entity and drained by whatever system owns that kind of interactable, matching the
`FocusRequest`/`TravelRequest` idiom exactly (handled → removed, unhandleable → left Pending, failed
→ marked `Failed` and held a frame). So no game code runs inside the resolve query, and one
interactable kind — a vehicle — is served by the `VehicleSystem` below without the interaction system
knowing anything about it.

## The physics-pose resolver — when the Transform chain is not the solver's frame

The two systems below both **derive a pose from an entity, test it against the physics world, and
write the result back**. Composing that pose up the `Transform` chain is right only while the chain
and the solver share an origin. **`PhysicsPoseResolver`** (`Veng/Physics/PoseResolver.h`) is the
optional per-scene seam past that, installed beside the world it pairs with
(`Scene::SetPhysicsPoseResolver` / `GetPhysicsPoseResolver`, a `Unique` the scene owns; `Clone()`
does not copy it). It is two hooks: **`Resolve(const Scene&, Entity, const mat4& localOffset)`
→ `PhysicsPose`** (the read — an entity's pose, optionally offset within its own frame, in the
world's frame) and **`Place(Scene&, Entity, const PhysicsPose&)`** (the write-back, and the report of
where a resolved placement landed). Either may be left empty and falls back to its default;
`ResolvePhysicsPose` / `PlaceAtPhysicsPose` are the free functions the engine and a consumer both
call, and `DefaultResolvePhysicsPose` / `DefaultPlaceAtPhysicsPose` are the fallbacks — `WorldMatrix
* localOffset` decomposed, and a write onto the entity's local `Transform` with the scale reset. A
scene installing nothing therefore behaves exactly as before, and installing nothing is the right
answer for every consumer whose `Transform` *is* the solver's frame.

**Who needs it:** a consumer whose authoritative positions live *outside* the f32 `Transform` — a
large-extent world where `Transform` is a render-relative projection and the physics space is
anchored elsewhere. Without the seam its interaction focus never resolves (the overlap sweeps a place
the solver holds nothing at) and its vehicle exits are validated against the wrong space. It is the
read-side counterpart of `RigidBody::SyncTransform` (see
[../Physics/CLAUDE.md](../Physics/CLAUDE.md)): that flag hands a consumer the `Transform` write-back
the step performs, this seam hands it the poses the engine *reads*. `Resolve` may run inside a live
query, so it makes no structural change — its `const Scene&` enforces that; `Place` runs outside one
and may.

## Vehicles — the possession-and-seating seam

**`Vehicle { vector<Entity> Seats }`** and
**`VehicleSeat { string Socket; Entity Occupant; bool IsDriver; string ExitSocket; AssetHandle<InputMappingContext> Context }`**
(`Veng/Scene/Vehicle.h`) make a vehicle *a thing a character can be inside and control* — the
possession-and-seating half, deliberately **movement-agnostic**: no wheels, no suspension, no
constraint, so how a vehicle moves is a consumer's own system attached to the vehicle pawn. Seat
placement is **entirely mesh sockets** (`AttachToSocket`), so where a pilot sits and climbs out are
facts about the model.

The builtin **`VehicleSystem`** drains an `InteractRequest` landing on a `Vehicle`. The request's
`Interactor` chooses the direction — already occupying a seat leaves, otherwise it boards the first
free seat in preference order. **Enter** disables the character's `CharacterController` (and removes
its capsule), parents the character to the seat socket, sets `Occupant`, and — for a **driver** seat
— re-points the controlling seat's `Possesses` at the vehicle and swaps its input context (popping
the character's top context, pushing the seat's `Context`). `LocalControl` follows for free: the
per-frame reconcile sees the new `Possesses` and moves the marker, so **no vehicle code runs in the
marker path**. **Exit** is the exact inverse in reverse order, placing the character at `ExitSocket`
and re-enabling its controller seeded with the vehicle's current velocity (no discontinuity leaving a
moving vehicle) — and it is **validated before performed**: the exit socket is overlap-tested against
solid geometry, and a blocked exit fails and reports rather than placing a character inside a wall.
**One resolved pose serves all three uses** — the overlap validation, the re-created capsule, and the
character's placement: the exit socket's local matrix is resolved through the scene's
[physics-pose resolver](#the-physics-pose-resolver--when-the-transform-chain-is-not-the-solvers-frame)
and handed back through its `Place` hook, which is how a consumer holding its authority outside the
`Transform` records where the character actually landed. A
runtime-only **`Seated`** component on the occupant records what entry changed so exit undoes it
exactly (`VE_TYPE`, never serialized). The `CharacterController` gained an **`Enabled`** flag for
this: a disabled controller is skipped by `CharacterMovementSystem` and its capsule released, and
`PhysicsWorld::SetCharacterVelocity` seeds the re-created capsule's velocity on exit.

## ConstantMotion — the input-free counterpart

**`ConstantMotion` is the input-free counterpart** (`Veng/Scene/Motion.h`): an authored
**`ConstantMotion { vec3 LinearVelocity; vec3 AngularVelocity; MotionSpace Space }`** is a
constant rate of change of the `Transform` — a drift and/or spin — that the engine
**`ConstantMotionSystem`** integrates each Sim tick. `AngularVelocity` is an axis-angle vector
(direction is the spin axis, magnitude is radians/sec); `Space` applies both velocities in the
entity's `Local` frame or its parent `World` frame. Unlike `MovementSystem` it reads no `Intent` —
the motion is autonomous, authored data, not a command — so a spinning prop carries no controller
and rides no wire. It is a builtin component (`RegisterBuiltinTypes`) selected per level like any
other system; the minimal template uses it to spin its cube as data, naming the host-registered
builtin `ConstantMotionSystem` in its level — its module registers no system of its own.

## Simulation & the Sim/View split

**The tick is split Sim / View, and entities carry `Authority`.** A `SceneSystem`
(`Veng/Scene/SceneSystem.h`) declares a **`Phase { Sim, View }`** (default `Sim`); a
**`SceneSimulation`** (`Veng/Scene/SceneSimulation.h`) runs all Sim systems, then all View
systems, each tick — so a View system reads the state the Sim phase finalized this tick. **Sim**
is the deterministic, replicable simulation (control, movement, rule systems); a headless Sim tick
is a pure function of state + intents (the `SystemContext.Input` service is always present,
reporting the neutral all-zeros state in headless, so an input-reading system needs no guard).
**View** is client-local presentation derived from finalized Sim state — the `CameraRigSystem`
(`Veng/Scene/CameraRig.h`) trails a possessed pawn via a `CameraFollow` component, orbits a point
via a `CameraOrbit` component, looks out of a character's eye via a `FirstPersonRig` component, and
resolves a plain first-person `CameraLook` (a clamped yaw/pitch heading written as the entity's
rotation), never authoritative and never on the wire. The rig's arms are the same shape: each is a
pure device-free function — `FollowCamera` / `OrbitCamera` / `FirstPersonCamera` / `LookRotation` —
that the system walks the matching `(Transform, …)` archetype to apply. `CameraOrbit` is the
point-orbit case the follow rig is not: it circles a `Focus` at a clamped `Distance` under
`Yaw`/`Pitch` (the pitch clamped by `PitchLimit` off the pole where the look-at up collapses),
reusing `LookRotation` for the y-up pose and orienting back at the focus, so a scene inspector,
model viewer, or map/chart view stops hand-rolling the spherical-to-cartesian eye placement. Its
optional `FocusTarget`/`FocusDamping` glide the focus with the same frame-rate-independent
`1 − exp(−damping·delta)` smoothing `FollowCamera` uses (zero snaps). `FirstPersonRig` is the
eye-anchored case whose horizon stays level when "up" is not a world constant: it builds the camera
basis each tick from its `Target`'s resolved up (a `CharacterState::Up` when the target carries
one), yawing about that up rather than world up — so a character walking a curved habitat keeps a
level horizon — reads its heading from a sibling `CameraLook`, anchors the eye at `EyeOffset` or a
named mesh socket, and clamps pitch into `[MinPitch, MaxPitch]`. An entity carrying a
`FirstPersonRig` is skipped by the plain `CameraLook` arm, so the two never both write its pose.

**Both rigs resolve their target at the pose it is *drawn* at, not the one it was simulated at.**
The renderer blends a drawn mesh's world transform between the last two Sim-tick snapshots by the
frame's alpha, so `CameraRigSystem` reads its target through `Scene::GetInterpolatedWorldTransform`
at the same `SystemContext::Alpha`. Resolving against the un-interpolated pose instead puts the
camera a partial tick ahead of everything rigidly attached to that target — a cockpit interior, a
mounted weapon, a held prop — which reads as those pieces swimming against the view by a fraction of
a tick's motion, changing every frame as the alpha sweeps, and growing with the target's speed and
turn rate. The camera's *own* transform stays un-interpolated and must: it is authored per frame
after the tick snapshot, so its live pose already is this frame's pose (the same reason
`InterpolatedLocalMatrix` exempts a `ViewPose`). An **`Authority { Tier, Owner }`** component marks who
simulates an entity: authored entities default `Server`, client-local view entities are `Local`
(only those two tiers are authored or persisted; `Remote` and `Predicted` are each peer's runtime
stance toward an entity, never replicated). Its consumer is the net layer's authority filter —
**`HasAuthority(context, scene, entity)`**, a `SystemContext` role × `Authority::Tier` query the
builtin authoritative Sim advancers (`MovementSystem`, the motion systems, `RootMotionDriveSystem`)
consult before touching an entity; an entity with no `Authority` defaults to `Server`-tier (see
[../Net/CLAUDE.md](../Net/CLAUDE.md)). The two-pass split is the whole scheduling mechanism: no
dependency graph, no parallelism.

**A paused world runs neither phase.** The pause lives on the `SceneSimulation` — a refcount of held
pauses beside an explicit toggle (see [engine/CLAUDE.md](../../CLAUDE.md), "Pause is a refcount") —
and while it holds, the world drive runs no Sim step and no View pass, so a View system does not keep
presenting a frozen world and a Sim system does not need to check a flag. The scene sees its own pause
through **`Scene::IsSimulationPaused()`**, the query for code that runs outside a phase (`OnStart`,
`OnStop`, a Gui driver presenting the scene); `SystemContext` carries no pause field, since a phase
that could read one never runs paused. Gameplay pauses its own world by stamping the builtin
**`PauseRequest`** (`Veng/Scene/Requests.h`), drained like its siblings; the resume comes from outside
the paused world.

**A `SystemContext` is built, never assembled by hand.** Every service on it is a required reference
with no default, so an omission is a compile error; the defaulted fields (`Pointer`, `View`, `Debug`,
`Tick`, `Alpha`, `Role`, `World`, `GameplayFocused`, the step edges, `IsReplay`) are per-call data. At
runtime one factory builds every context — start, Sim step, View pass, stop and replay — from a
`SystemContextRequest` naming the world, scene and step (`WorldRunner::SetContextFactory` /
`BuildContext`, installed by `Application`; see [../../CLAUDE.md](../../CLAUDE.md)). A unit test
takes real device-free services from `tests/support/TestServices.h` instead.

**A Sim system can run less often than every step.** `SceneSystem::GetTickPolicy()` returns a
`TickPolicy` — `EveryStep()` (the default), `FirstStepOfFrame()`, `LastStepOfFrame()`, or
`EveryNth(n, offset)` — and `SceneSimulation::UpdatePhase` runs a Sim system only on the steps it
selects. `EveryNth` runs where `Tick % n == offset` and hands the system `n × delta`; it keys on the
tick, not the frame, so every peer and a reconciliation replay pick the same ticks, and staggering
offsets spreads several such systems across a period. The two frame-keyed cadences read
`SystemContext::FirstStepThisFrame` / `LastStepThisFrame`, hand the system the live steps since it
last ran times the step delta, and neither count nor run on a replayed tick — so a system whose
state a client predicts declares `EveryStep` or `EveryNth`. A View system runs once per frame
whatever it declares. Each system documents its policy, and changing one is a behaviour change made
in the system's own repo.

## Game modes & world config

**A game mode is mode-state components + rule systems — no object, no registry.** A
**`GameModeConfig`** on the level's settings entity names the player prefab (its JSON key is
`"gameMode"`); a game authors whatever further mode-state components its own rule systems read
and write, beside it. The "mode" is a *selectable set of rule systems* (spawn-on-start, scoring,
win-condition) over those components; begin/end-play is the systems' `OnStart`/`OnStop`.
Selecting a mode is choosing a config plus a registered rule set — no C++ path picks it, no
`GameModeRegistry`, no ABI bump. The engine ships no mode-state component of its own — mode state
is game vocabulary. (The word "session" means something else entirely: the per-account
`Net::SessionRecord` the host tier keeps — see [../Net/CLAUDE.md](../Net/CLAUDE.md).)

**World-scoped config is a component found by type, not on a designated entity.** A rule system
reads the `GameModeConfig` (and the engine reads `LevelRenderSettings`) through
**`Scene::TryGetFirst<T>()`** — the first component of a type, or `nullptr`. So world/level config
lives on *some* settings entity without any consumer naming a well-known one: a `Level` seeds
level-scoped config onto one (see **Levels**), and genuinely world-scoped config (a hypothetical
`PhysicsSettings` holding gravity, say) is just authored as a component on an entity in the world
prefab. One such component is the expected case; with several the first wins and the rest are
ignored — a loose convention, deliberately **not** an enforced singleton or a side-channel
resource store (the data stays ordinary component data, riding the one
cook/serialize/inspector/replication pipeline). Absent → the consumer falls back to a default, the
same schema-tolerance a missing input gets.

## The system catalog

**Systems are a catalog; selection and order are level data; config is components.** A
`SceneSystem` declares a stable **`SystemId`** (a `u64` id space alongside `AssetId`/`TypeId`,
minted with `vengc generate-id`) + a display name through the **`VE_SYSTEM(Type, 0x…ULL,
"Name")`** trait macro — the system analogue of `VE_REFLECT`'s identity. The host-owned
**`SystemRegistry`** (`Veng/Scene/SystemRegistry.h`, mirroring the `TypeRegistry`: the host
constructs it, **pre-registers the engine's reusable systems with `RegisterBuiltinSystems`**
(`Veng/Scene/BuiltinSystems.h` — the system analogue of `RegisterBuiltinTypes`), the module fills
its own through `VengModuleRegister`, `Application` borrows it) stores `{ SystemId, Name,
factory }`, **enumerates the catalog** without instantiating anything, resolves an id, and fatally
rejects a duplicate id. The builtins register in this order (`BuiltinSystems.cpp`):
`DeviceAssignmentSystem`, `InputMappingSystem`, `BehaviorSystem`, `MovementSystem`, `CharacterMovementSystem`,
`RootMotionDriveSystem`, `InteractionSystem`, `VehicleSystem`, `CameraRigSystem`,
`CharacterAnimationSystem`, `AnimationSystem`, `ConstantMotionSystem`, `RemoteCharacterBodySystem`,
`PhysicsSystem`, `PoseHistorySystem`, `RemoteInterpolationSystem`, `RibbonSystem` (View-phase —
ribbon ages and trail samples, ahead of the pool's retire), `FlipbookSystem` (View-phase —
sprite playback and the effect pool), `TimeOfDaySystem`, `AudioSystem` (View-phase — it
places, spatializes, and publishes the scene's `AudioSource`s against the `AudioListener` at the
interpolated poses the frame draws; see [../Audio/CLAUDE.md](../Audio/CLAUDE.md)). Registration is GPU-free (building a system touches no `Context`/device), so
`RegisterBuiltinSystems` is callable in the headless cooker with no ICD — the cook reflects a
level's named systems against the same builtins + module catalog the runtime resolves. A
`SceneSimulation` is built either from an **ordered `SystemId` set** selecting catalog entries
(run in that order, honoring the phase split) or from the whole registry as the "all registered"
convenience. A system's **parameters are authored as components** — a settings entity the system
reads — reusing the entire reflection inspector and keeping systems pure logic; there is no
reflected-system-config mechanism. `SceneSimulation::FindSystem<T>()` returns the running instance
of a system the simulation holds (null when it holds none), the read-out seam for state a system
keeps between ticks — the remote-interpolation playback clock a client stamps on its input is read
this way (`RemotePlaybackTick`).

## Levels

**A `Level` is the authored wiring artifact — a thin wrapper by reference.** A **`Level`** asset
(`AssetTypes::Level`, `Veng/Asset/Level.h`) does not embed world entities: it *references* a
**world prefab** by `AssetId` and adds the data that is not reusable-recipe data — the ordered
active `SystemId` set, the `GameModeConfig`, and a tolerant **`LevelRenderSettings`** subset (the
view-wide post/pipeline knobs — exposure, bloom, shadow/AO toggles the app maps onto its
`SceneRendererSettings`/`SceneView`). The sky/environment is **not** a level field: it is the
scene's one author-opt-in `Sky` component (plus an optional `TimeOfDay`) on the world prefab,
resolved by the renderer itself each `Execute`. The level *reuses* prefab serialization rather
than embedding a second copy: a prefab is a reusable recipe, the `Level` is the once-loaded
playable unit (named `Level`, not `Scene`, to avoid colliding with the runtime `Scene`). It is CPU
data with no GPU resource, loaded through the ordinary `AssetManager::Load`/`LoadSync` path; its
world prefab and that prefab's embedded asset refs resolve as ordinary load-time dependencies.
**`Level::LoadInto(AssetManager&, const SystemRegistry&) → LevelInstance`** is what *starts the
game*: it spawns the world prefab into a fresh `Scene`, builds a `SceneSimulation` from the
level's `SystemId` set against the catalog and **attaches it to the `Scene`**
(`Scene::SetSimulation` — the scene owns its simulation), and **`SeedLevel`s a settings entity**
carrying the level's `GameModeConfig` and `LevelRenderSettings` as components, returning a `LevelInstance { Unique<Scene> World; ResidencyBatch Pending; }` the app
ticks (via `Scene::TickSimulation`) and renders. The level's config (game mode, render settings)
stays **authored on the `Level`** (edited as separate level-editor panels, cooked into the level
blob) but enters the running world as scene components — so rule systems and the engine read it by
`Scene::TryGetFirst<T>()`, the engine resolving `LevelRenderSettings` onto the renderer from the
scene rather than the `Level` object. A game is assembled as authored data, not hand-spawned in
`main.cpp`; the engine-managed game world (see **Application** in
[engine/CLAUDE.md](../../CLAUDE.md)) drives this end to end so a minimal `main.cpp` writes none of
it.
