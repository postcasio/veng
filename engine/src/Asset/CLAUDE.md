# Runtime assets — loading, meshes, textures, shaders & materials

This module is the runtime side of the asset system: loading cooked assets by `AssetId` through
`AssetManager` and `AssetHandle`, the per-type loaders (textures, meshes, prefabs, materials), and
the runtime material model (`Material` parent / `MaterialInstance` override). The on-disk archive
format is [assetpack/CLAUDE.md](../../../assetpack/CLAUDE.md); the offline cook that produces it is
[cooker/CLAUDE.md](../../../cooker/CLAUDE.md). Project-wide conventions live in
[the root CLAUDE.md](../../../CLAUDE.md), the runtime overview in [engine/CLAUDE.md](../../CLAUDE.md),
the renderer that consumes these assets in [../Renderer/CLAUDE.md](../Renderer/CLAUDE.md), prefab
spawning and the scene in [../Scene/CLAUDE.md](../Scene/CLAUDE.md), and reflection/serializers in
[../Reflection/CLAUDE.md](../Reflection/CLAUDE.md).

## Cook offline, load by `AssetId`

Assets are **cooked offline** into a binary archive, never imported at runtime — there is no
cook-on-demand, no source parser, no re-cook path in `libveng`. The `vengc cook` tool (always built
from source in-tree; a downstream `find_package(veng)` consumer gets it as a **prebuilt imported
executable** whose unqualified `vengc` name `veng-config` recreates, so `$<TARGET_FILE:vengc>`
resolves either way) turns a hand-written JSON **asset pack** into a single `.vengpack` archive; the
engine *mounts* archives and resolves assets against them.

- **`.vengpack` archives (format v6) carry content hashes.** Every cooked blob gets a content hash
  and the table of contents gets a digest (over the serialized TOC bytes), cooker-written via
  xxh3-128 and checkable with **`vengc verify`** (it re-hashes the blobs + digest and exits nonzero
  on any mismatch). **The loader never verifies** — hashing is tooling, not the hot path; the
  runtime trusts its packs. The hash function lives **only** in the cooker/verify tool, so
  `assetpack` (which stores the raw 16 bytes and computes nothing) and `libveng` gain no hash
  dependency. Blobs are stored **per-blob zstd-compressed or raw**; `assetpack` inflates a
  compressed entry lazily on resolve (the codec + sizes live in the TOC, the inflate in `assetpack`
  — see [assetpack/CLAUDE.md](../../../assetpack/CLAUDE.md)).
- **An asset pack is a pure `{ id, type, source }` manifest.** It carries no per-asset settings.
  **Every** asset type — texture, mesh, shader, material, prefab — has its own per-asset JSON
  source file (`*.tex.json` / `*.mesh.json` / `*.shader.json` / `*.vmat.json` / `*.prefab.json`)
  that the manifest entry points at; the sampler settings, import options, shader source/entry,
  material fields, and prefab entities/components live in those files.
- **`AssetTypes::InputMap` — an `InputMappingContext`, a CPU-only asset** (like
  `Skeleton`/`Animation`/`Level`, no GPU resource). Its `*.inputmap.json` source declares the
  actions a scheme defines (id + name + `ActionKind`) and its raw-source → action bindings; the
  cook validates every binding against the declared actions. The runtime loads it by `AssetId`
  through the ordinary `Load`/`LoadSync` path and hot-reloads it through `MountMemory`; a seat's
  `InputContextStack` references one or more by id, and `InputMappingSystem` resolves the active
  set against the raw snapshot — the gameplay control flow is
  [../Scene/CLAUDE.md](../Scene/CLAUDE.md).
- **`AssetTypes::CollisionShape` — solver-neutral collision geometry, CPU-only** (no GPU
  resource). Its `*.collision.json` source names a model and a mode (`"convex"` / `"mesh"`); the
  loaded asset is a point list plus, for a triangle mesh, its indices. A `Collider` component
  references one through an `AssetHandle<CollisionShape>` and it resolves as an ordinary load-time
  dependency, so a prefab naming one has it resident before the entity spawns. The physics module
  builds the solver's own shape from it — see
  [../Physics/CLAUDE.md](../Physics/CLAUDE.md).
- **`AssetTypes::TableSchema` + `AssetTypes::DataTable` — structured data**, both CPU-only.
  **A column carries a reflection `TypeId`** — there is no table-specific type vocabulary, and a
  cell is encoded, decoded, and validated by the same `WriteFieldValue`/`ReadFieldValue` and
  `JsonReadFieldValue` walkers every other authored blob binds through. Any registered type is a
  legal column: scalars, vectors, enums, asset handles, nested structs, and (through a struct with
  a `VE_ARRAY_FIELD`) arrays. A `TableSchema` is the loaded column set (names, types, cell
  offsets, the key column); a `DataTable` holds a handle to the schema it was cooked against — an
  ordinary streamed dependency — plus the resident row region, the row directory, and a sorted key
  index.
  `FindRow(key)` is an allocation-free binary search over that index, which stays a **separate**
  structure from the directory: the two answer different questions (key → row index; row index →
  bytes). A key column is restricted to the ordered, stably-encoded types — the integer scalars
  and string (`TableKeyKindForType`). **What makes a set of columns a legal schema is one function,
  `LayOutTableSchema`** (`Veng/Asset/DataTable.h`): unique non-empty names within the cooked name
  capacity, every type registered and not a `Reference`, a key column that exists and can be
  ordered — plus the offset assignment itself. The cooker's `ParseTableSchema` and the editor's
  schema panel both call it, so a schema the editor accepts is one the cook accepts and neither
  carries a second copy of the rules.
  **Rows are variable-size**, addressed through a `u32` row directory; when every column's type
  encodes to a constant width the importer omits the directory and records a stride instead, and
  addressing is arithmetic. That is a property of the cooked blob, not of the format contract —
  the accessor API is identical either way, so neither the runtime nor the editor branches on it.
  `GetColumn<T>(name)` is the zero-copy path for a fixed-size column at a constant offset (a
  column keeps one only while every preceding column is fixed-size); `ReadCell<T>` is the general
  reflected read, and `ReadRow<T>` binds a whole row into a reflected struct by field name. The
  type checks on those are **fatal** (API misuse) where a malformed blob is instead rejected by
  the loader as `AssetError::Corrupt` — including a blob whose key index, row directory, and row
  region disagree on how many rows exist.
  Tables are sized for full residency — 10 MB is a normal large table, 100 MB the working extreme
  — so an asset-handle cell yields a bare `AssetId` (`GetAssetIdColumn`) and the table **never**
  loads what it references; the consumer decides.
- **Load is by opaque `u64` `AssetId`** through mounted archives.
  `AssetManager::Load<T>(AssetId)` is **async by default**: it returns a not-yet-resident
  `AssetHandle<T>` immediately and runs the decode + GPU upload on the task system (transfer
  queue, no frame stall); poll `IsLoaded()` before using it. `AssetManager::LoadSync<T>(AssetId)`
  is the **blocking** sibling — it runs the whole pipeline inline and returns a resident handle or
  a structured error, `AssetResult<AssetHandle<T>>` (`std::expected<…, AssetLoadError>` — branch
  on `AssetError::Kind`, not a string).
- **`AssetManager::MountMemory(vector<u8>, string) → MountHandle`** shadow-mounts an **in-memory
  archive** over the on-disk mounts: a later resolve of an `AssetId` the in-memory archive carries
  hits it first. The returned `MountHandle` is **RAII** — drop it to unmount and reveal the
  underlying mount again. The cook-on-demand loop uses it: the editor cooks a source into a
  scratch archive in memory, mounts it, reloads the handle behind it, and replaces the
  `MountHandle` on the next recook.
- **`AssetManager` is owned by `Application` and constructed with a `Context&`, a `TaskSystem&`,
  and the borrowed `TypeRegistry&`** (the registry the prefab loader reflects components through).
  The async `Load` is the obvious call and the non-stalling one; `LoadSync` is the marked-verbose
  blocking spelling for tests, tools, and the smoke path.
- **The cooker loads the game module to reflect its types.** `vengc cook --module <lib>` `dlopen`s
  the game module and reflects its component types into a `TypeRegistry` (reusing `ModuleLoader`,
  ABI-version check included), so the `PrefabImporter` validates a prefab's components against the
  **real** reflected descriptors — an unknown component, a wrong field type, or a malformed value
  is a located cook-time error, the way the material importer validates `*.vmat.json` against a
  shader's reflected parameters. A field absent from the source keeps its default-constructed
  value (schema tolerance), so omission is allowed, type-mismatch is not. `vengc generate-type-id`
  mints a collision-free `TypeId` (the `TypeId` analogue of `generate-id`) and `vengc` can emit a
  type manifest.
- **Type registration is GPU-free, by contract.** `RegisterBuiltinTypes(TypeRegistry&)` (public,
  in `Veng/Scene/BuiltinTypes.h`), `Register<T>()`, and a module's `VengModuleRegister` (the
  `Application` factory + the type registration) touch **no** `Context`/device — the headless
  cooker reflects a module's types with no ICD present, and a no-device cooker test pins the
  contract. The **host** (launcher or cooker) owns the `TypeRegistry`: it constructs it,
  pre-registers the builtins, puts it in the `VengModuleHost` as `Types`, calls
  `VengModuleRegister` (at which point the module registers its component types), and threads it
  onward.
- **A game module defines whole asset types of its own.** The type's identity registers exactly
  once per process, through `VengModuleRegister` — `host->AssetTypes.Register(AssetTypeInfo{...})`
  for the minted id, manifest name, and editor display metadata, and
  `host->AssetLoaders.Register(type, factory)` for a factory the `AssetManager` instantiates at
  construction. Registration is inert and GPU-free, which is why it can precede any live manager;
  the module points `ApplicationInfo::AssetTypes` / `AssetLoaders` at the same host-owned
  registries so the manager it later builds finds them. A factory claiming a type the engine
  already handles is fatal — override semantics for builtin types stay engine-owned. The offline
  half is a separate **cook module** carrying importers only (see
  [cooker/CLAUDE.md](../../../cooker/CLAUDE.md)); registering an id from both seams would deliver
  it twice in the editor, where both images load, and duplicate ids abort.
  @warning The registries hold owned polymorphic objects whose code lives in a `dlclose`-able
  image, so the module handle must outlive them *and* the `AssetManager` built from them. Hosts
  enforce this structurally, by declaring the handle first in the owning struct.
- **`AssetLoaderRegistry`'s storage lives behind an implementation pointer.** Its
  `std::unordered_map` sits in an `Impl` struct defined in the registry's own implementation TU
  rather than in the public class definition, so a TU that merely parses the class instantiates no
  map; every accessor keeps its exact signature and the class is consequently **move-only**. See
  [the root CLAUDE.md](../../../CLAUDE.md#a-registrys-container-storage-lives-behind-an-implementation-pointer)
  for the rule.
- **`AssetHandle<T>` on a component resolves through `AssetTypeInfo::HandleFieldType`.** A
  reflected field is a leaf `TypeId`, not an asset type, so something must map the two. That
  something is the asset type's own registration: `HandleFieldType` carries the reflection
  `TypeId` of the `AssetHandle<T>` leaf that references it (a bare `u64`, since assetpack takes
  no reflection dependency), and `AssetTypeRegistry::FindByHandleField` is the reverse index.
  The prefab loader's dependency collection, the cooker's prefab and table validation hooks, and
  the editor's asset picker all go through that one lookup, so none of them can answer
  differently. A game therefore does three things for `AssetHandle<MyType>` to work on a
  component: `VE_LEAF` the handle leaf with a minted `TypeId`, register the component type that
  holds it, and set `HandleFieldType` to that same leaf id on the `AssetTypeInfo` it registers.
  A leaf no registered type claims is an **error** at both load and cook — never a skipped check.
  Eighteen of the twenty builtins claim a leaf. The two that do not — `Shader` and
  `VertexLayout` — are wiring inside the material system: a draw binds a `MaterialInstance`, and
  nothing outside that system can consume a bare shader or vertex layout, so a reference to one
  would be authorable but unusable. They leave `HandleFieldType` 0 and cannot sit on a component.
- **The `AssetManager` always owns a complete `AssetTypeRegistry`.** It fills its own with
  `RegisterBuiltinAssetTypes` at construction and merges `AssetManagerInfo::AssetTypes` on top —
  the same shape as the builtin-then-module loader registration beside it. `GetAssetTypes()` is
  therefore never empty, which is what lets a prefab full of `AssetHandle<Texture>` fields load
  in a host that registers no game types at all.
- **Build-order edge: `veng_add_asset_pack(... MODULE <lib>)`.** A pack containing prefabs names
  its game module; the build graph grows a `lib → cook → bundle` edge so the pack cooks after its
  lib is built. Packs without prefabs stay module-independent. `veng_add_game` wires the example's
  prefab pack to depend on `libhello_triangle`.

## Upload tiers

The same split runs underneath at the resource level: `Buffer/Image::Upload` (taking a
`TaskSystem&`) is **async by default** — it returns a `Task<void>`, records the copy on the
transfer queue, and never blocks — while `UploadSync` is the blocking path (host memcpy +
`WaitIdle`) the sync loaders, tests, and smoke render use.

## Textures

**Textures load multi-mip and block-compressed.** A cooked texture carries a full mip chain
(largest-first) in a GPU block format (ASTC 4×4 by default, BC7 selectable) or an uncompressed
format. `TextureLoader` walks the levels with **`Renderer::FormatInfo`** — a header-only
block-geometry helper (`Veng/Renderer/FormatInfo.h`, no backend include) whose
`BytesForLevel(format, w, h)` is `ceil(w/bw)·ceil(h/bh)·bytesPerBlock`; an uncompressed format
reports a **1×1 block**, so one helper sizes every format and the blob needs no per-level offset
table. Upload records **one `VkBufferImageCopy` region per level** from a single staging buffer
through the multi-region `CommandBuffer::CopyBufferToImage` overload (no `GenerateMipmaps` — a
block-compressed image cannot be blit-mipgen'd; GPU mipgen stays scoped to runtime-built
uncompressed textures). A **compressed format must be enabled at `createDevice`, not merely
queried** — `textureCompressionBC` / `textureCompressionASTC_LDR` are core
`VkPhysicalDeviceFeatures` booleans `Context` enables when the physical device supports them, and
`Context::IsBlockCompressionSupported()` / `IsAstcSupported()` reflect the **enabled** state
(sampling a block image without the enable is a validation error). The runtime does **not**
transcode: on a device lacking the cooked codec's feature the loader logs **once** and returns a
recoverable `AssetError::Unsupported`, so the affected materials sample their fallback (untextured)
and the app still runs — only `smoke_golden` (gated to skip on a non-ASTC device) would diverge.

## Meshes

- **`AssetHandle<T>` is refcounted indirection into the manager's cache**, not a `Ref` to a GPU
  resource (see the root CLAUDE.md ownership rule). Apps hold their handles as members the app
  destructor frees like any other engine resource; `CollectGarbage()` evicts entries no handle
  references, retiring
  their GPU resources through the per-frame deferred-destruction path.
  **`AssetManager::Adopt<T>(Ref<T>)`** wraps an already-resident, runtime-created resource in an
  `AssetHandle<T>` so it is usable everywhere a cooked, `AssetId`-loaded handle is. The adopted
  handle carries the invalid `AssetId` (`Id().IsValid() == false`), and its cache entry is
  **detached** — never inserted into the `AssetId` map, so `CollectGarbage()` ignores it; it stays
  alive exactly as long as a handle references it. A reflective serializer records the invalid id
  as "no asset", so a runtime resource is not a persistable content reference.
- **A `Mesh` can also be built at runtime, no cooker.** `Primitives::Cube` / `Plane` / `Sphere`
  (`Veng/Asset/Primitives.h`) generate CPU-side `MeshData` (canonical-layout vertices + `u32`
  indices + a resident material list + an indexed submesh table) with analytic normals/tangents/UVs
  and an optional `AssetHandle<Material>`; `Mesh::BuildSync(Renderer::Context&, const MeshData&,
  const string&)` uploads that into a resident `Ref<Mesh>` via the blocking `UploadSync` (its
  async sibling `AssetManager::Build<Mesh>` streams the same geometry in off the render thread). A
  runtime primitive is **not** an `AssetId`-addressable asset and never touches an archive — it is
  owned by whoever calls the factory and retires through the per-frame deferred-destruction path
  like any other `Mesh`. It is interchangeable with a cooked mesh at every pipeline and draw call,
  both being in the canonical layout (`Mesh::CanonicalLayout()`), and `AssetManager::Adopt` wraps
  its `Ref<Mesh>` in an (id-less) `AssetHandle<Mesh>` so it is equally usable anywhere a cooked
  handle is — e.g. a `MeshRenderer`.
- **One generator is projection-derived: `Primitives::ProjectionShell`.** Every other generator's
  shape is a function of its own dimensions; this one's is a function of a **camera projection**. A
  grid over a normalized screen rect is unprojected through a given `(fovY, aspect)` into camera-space
  rays and placed at a fixed radius, so the resulting spherical-cap section reproduces that screen
  rect exactly when viewed from its own eye point down −Z — and is ordinary geometry from any other
  pose. It is generated **in camera space**, so the consumer parents it at the pose the display is
  designed to be viewed from. `Primitives::ProjectionShellReprojectionBound` is its companion: the
  closed-form between-vertex displacement in logical points, `O(cell²)` and independent of radius,
  which is what an alignment budget is stated in rather than a tuned constant. It is deliberately
  **not** a `MeshSource` alternative — its natural inputs (a live aspect, a live rect) are runtime
  values, so a shell is built by code and rebuilt when they move, never authored into a prefab. The
  authoring story is
  [docs/guides/diegetic-ui.md](../../../docs/guides/diegetic-ui.md#perspective-true-shells-a-panel-that-agrees-with-a-screen-space-layout).
- **`Primitives::CurvedPanel` is the other display shape, and the two answer different questions.**
  A section of a cylinder's lateral surface, curved about its local +Y and flat along it — a curved
  monitor, a curved instrument fascia, a bent signage board. Its `size.x` is an **arc length**, its
  centre of curvature sits at local `(0, 0, +curvatureRadius)` so the flanks bend toward the viewer,
  and its vertices are spaced uniformly in arc length (a panel's pixels are evenly spaced on the
  glass). The distinction from the shell is that a panel's **curvature is decoupled from its viewing
  distance**, so it genuinely looks curved where a shell — whose centre of curvature *is* its eye
  point — cannot; it makes no claim to reproduce a screen rect, which is why it has no bound function.
  Two closed-form companions come with it: **`CurvedPanelHit`** (the panel-space ray → UV that places a
  world-anchored marker on one, front face only, taking the nearer root *that lands on the panel*) and
  **`CurvedPanelSizeForRect`** (the size covering a normalized screen rect's angular footprint,
  clamping to the curvature's silhouette rather than emitting a NaN when a combination is unsolvable).
  Both primitives keep their own contract; the consumer-facing choice is written up in
  [docs/guides/diegetic-ui.md](../../../docs/guides/diegetic-ui.md#curved-panels-a-display-that-looks-like-a-curved-physical-object).
- **A mesh reference's source is `cooked AssetId | inline recipe`.** `MeshRenderer`
  (`Veng/Scene/Components.h`) carries one runtime `AssetHandle<Mesh> Mesh` (the renderer query
  `(Transform, MeshRenderer)` and every draw path read it) plus a serialized **`MeshSource
  Source`** — a `Variant<CubeShape, PlaneShape, SphereShape, IcosphereShape, CylinderShape,
  ConeShape, TorusShape, CapsuleShape, AnnulusShape>`, each alternative carrying that shape's parameters plus an
  `AssetHandle<Material>`. An empty `Source` means the authored cooked `Mesh` is used as-is; a
  non-empty `Source` is the inline procedural recipe, so a prefab persists "icosphere, radius 0.8,
  4 subdivisions, brick material" inline rather than as an unaddressable runtime handle. Both
  forms cook and load through the ordinary prefab path; the embedded material in the active
  alternative (and the cooked `Mesh` id) resolve as ordinary load-time dependencies. The recipe
  becomes a renderable mesh **during the populate pass**: `Prefab::SpawnInto`, right after it
  rehydrates a component's fields, builds a non-empty `Source` into the entity's `Mesh` via
  `BuildPrimitiveMesh(AssetManager&, const MeshSource&) → AssetHandle<Mesh>` — which builds the
  active shape's CPU geometry (`BuildShapeMeshData`) and streams it in through
  `AssetManager::Build<Mesh>`, yielding a pending handle identical in kind to a cooked async load.
  So a recipe-sourced mesh **appears** a few frames after spawn exactly as a cooked mesh would
  (the renderer skips a not-yet-resident mesh), with no second spawn pass and no per-frame scan.
  There is no dedup cache: identical recipes build independent meshes, and a consumer wanting N
  entities on one mesh calls `BuildPrimitiveMesh` once and assigns the shared handle N times. The
  hand-built `Primitives::`/`Adopt` path above stays public for tests and tools.
- **A mesh owns its materials; submeshes index them.** A `Mesh` holds a resident
  `vector<AssetHandle<Material>>` (`GetMaterials()`) and each `SubMesh` carries a `u32
  MaterialIndex` into it (`SubMesh::NoMaterial` = unassigned). The cooked on-disk mesh format
  stores u64 material ids; `MeshLoader` eager-resolves those ids into material instances and
  builds the list, exactly as `Material` resolves its own texture/shader dependencies — so every
  asset eager-loads its dependencies. A draw iterates submeshes, binding
  `GetMaterials()[MaterialIndex]` per range.
- **A mesh carries the model's named attachment points.** `Mesh::GetSockets()` is the cooked
  `MeshSocket` list — name plus a mesh-space TRS — sorted by name, and `Mesh::FindSocket(name)`
  binary-searches it, returning `nullptr` for a name the model does not carry (a content error the
  caller reports, never an assert). The orientation is contract: a socket's local **-Z is forward**
  and **+Y is up**. Which authored nodes become sockets is the cook's decision
  ([cooker/CLAUDE.md](../../../cooker/CLAUDE.md)); the runtime consumes the table as given.
  Attaching an entity to one is `AttachToSocket` — see [../Scene/CLAUDE.md](../Scene/CLAUDE.md).
  A socket is **mesh-space and static**: it does not follow a skinned mesh's animated skeleton, and
  a joint anchor is a different mechanism.
- **Skinned meshes carry a skeleton and animate through GPU skinning.** A `Mesh` with a
  `SkeletonId` is **skinned** (`Mesh::IsSkinned()`): its vertices use the skinned layout
  (`Mesh::SkinnedLayout()` — canonical attributes plus `RGBA16Uint` bone indices + `RGBA32Sfloat`
  weights) and it eager-resolves an `AssetHandle<Skeleton>` (`Skeleton` and `Animation` are
  CPU-only assets, loaded by `AssetId` like any other, no GPU resource). An **`Animator`**
  component (`AssetHandle<Animation>` + time/speed/loop/playing) plays a clip; the View-phase
  **`AnimationSystem`** samples it against the mesh's `Skeleton` each tick into a transient
  **`SkinnedPose`** component (the bone palette, `Skeleton::ComputeSkinningMatrices` =
  `GlobalInverse · modelBone · inverseBind`). The `SceneRenderer` splits its g-buffer draw plan
  into a static path (the existing GPU-driven-cull pipeline) and a **skinned path** drawn
  CPU-direct. The skinned path's pipeline is a **per-Surface-material sibling of the static
  g-buffer pipeline** — the core `surface_skinned.vert` (4-influence linear-blend skinning) paired
  with the material's own fragment, built **lazily the first time the material is drawn on a
  skinned mesh** (`Material::EnsureSkinnedPipeline`, driven by the renderer) and reachable through
  `Material::GetSkinnedPipeline()`, so a material never skinned never pays for it and a static-only
  material (one whose fragment does not consume the full surface interpolant set) still loads. Its
  layout carries a third descriptor set the
  static `surface.vert` layout does not: the per-instance **skinning palette** SSBO (ring-buffered,
  **set 2**; `DrawData.PaletteBase` is each instance's offset), which the g-buffer pass binds for a
  skinned draw. The directional `ShadowScenePass` and the
  `PunctualShadowScenePass` both cast a skinned caster's posed shadow through a parallel
  `shadow_depth_skinned.vert` + the palette at set 1, and `surface_skinned.vert` skins both the
  current and previous position (the latter through the previous-frame palette base the renderer
  tracks, valid because the palette is ring-buffered) so a skinned mesh's deformation writes its
  motion vector into the g-buffer velocity channel. An entity with no `SkinnedPose` (e.g. the
  editor with systems paused) renders at the skeleton's bind pose. The core pack ships the
  `skinned` vertex layout and the skinned surface/shadow vertex shaders.

## Prefabs

**Cooked prefabs load like every other asset; a `Scene` is what you spawn into.** A `*.prefab.json`
(entities + components + field values) cooks into an `AssetTypes::Prefab` blob and loads through the
**identical** `AssetManager::Load`/`LoadSync` path — a cached `AssetHandle<Prefab>` whose embedded
asset references (a `MeshRenderer`'s mesh, a `Material`, …) are resolved as ordinary load-time
dependencies, exactly as a `Material` resolves its textures and shaders. The cooked blob **is** the
reflection serializer's name-keyed `WriteFields` record encoding, per component, wrapped in an
entity/component table — not a new format. A `Scene` is an engine primitive, **never loaded**; you
create one and spawn into it: `Prefab::SpawnInto(Scene&, AssetManager&) const → vector<Entity>`
(the spawned roots) creates the entities, `ReadFields` each component, remaps intra-prefab `Entity`
reference fields to the fresh handles, and rehydrates the embedded `AssetHandle` fields. Spawning
the same prefab twice spawns two independent copies — a prefab is a reusable recipe, not a
singleton. `SpawnInto` lives on `Prefab`, so the dependency points asset → primitive; the `Scene`
primitive gains no asset-system dependency. The per-component populate loop also builds a
`MeshRenderer`'s inline recipe `Source` into its `Mesh` (`BuildPrimitiveMesh`) right where it
rehydrates the cooked-handle fields, so a recipe resolves to a pending handle through the same
single pass as a cooked load — there is no second spawn pass and no resolver seam.

**A prefab can stand inside another prefab.** An entity in a `*.prefab.json` may carry one optional
key beside `components` — `"prefab": "0x…"` — naming the prefab that is that entity's **body**. The
named prefab is an **ordinary load-time dependency** (`PrefabLoader` fans it out beside the embedded
`AssetHandle` fields and keeps it resident), and `Prefab::SpawnInto` **recurses**: it spawns the
named prefab into the same scene and **the nesting entity is that expansion's first root** — the
nesting entity's component records are applied to it as **whole-component overrides** (a component
the root does not carry is added, one it carries is replaced outright), and any further roots the
expansion had are parented under it. There is **no field-level merge**: a partial record has no
defined meaning against a record the child authored, and whole-component replacement is what an
authored placement wants (the child says what the thing *is*, the parent says where it sits). Each
spawn's `ResidencyBatch` absorbs every expansion's (`ResidencyBatch::Merge`), so one `SpawnResult`
still reports everything the spawn left pending, and `SpawnOptions::SkipServerAuthoritative` flows
through the recursion so a client-mode load skips authoritative entities at every depth. A nesting
entity is never itself skipped: it authors no body of its own.

**One authored entity is one spawned entity, and that is what makes nesting compose.** The nesting
entity is not a container standing in front of its body — it *is* the body's root — so a prefab
nesting a prefab that itself nests one lands every level's records on a single entity, a `Reference`
field naming a nesting entity resolves to the composed thing, and a single-tier nest leaves no
componentless entity behind. Two consequences to author against: an expansion that materializes
nothing (an empty prefab, or one whose every entity a client-mode load skipped) leaves a plain entity
carrying the nesting entity's records and nothing else — so **a prefab meant to survive a client-mode
load authors its own `Veng::Authority`**, exactly as any other authored entity must, since a body is
authored content and the skip pass reads it as such; and `Hierarchy` is no longer exempt from the
override rule, because the nesting entity's parent edge and the expansion root's are now one edge —
`ReplaceComponent` resets only its authored `Parent` field, leaving the scene's derived child links
into the expansion intact.

Two properties fall out of expanding at spawn rather than flattening at cook:

- **`PrefabSource` names the outermost prefab that spawned an entity as one of its roots**, which is
  the id that reproduces it with every level's overrides applied — what replication instantiates
  from. A nested root the outer prefab did not compose onto keeps the nested prefab's own id.
- **A parent never goes stale behind its child.** A flattened parent would need a cook-time
  dependency edge to avoid exactly the silent staleness the asset system is built to prevent.

**Entity references stay prefab-local.** A `Reference` field cooks to an index into its *own*
prefab's entity table and is remapped within its own nesting level, so a parent cannot name an
entity inside its child and a child cannot name one in its parent — the property that makes a prefab
reusable wherever it is instanced. A prefab that transitively names itself is a **cook error**
([cooker/CLAUDE.md](../../../cooker/CLAUDE.md)), which is what keeps the runtime recursion finite.
The editor spawns a prefab into a live document scene and round-trips it back to `*.prefab.json`; it
does not yet author or preserve a nesting edge, so it treats a nested subtree as it treats any
spawned entity and saves it flattened.

## Shaders & materials

The offline shader compile + reflection and `.vmat` validation run in the cooker
([cooker/CLAUDE.md](../../../cooker/CLAUDE.md)); this section covers the runtime material.

Shaders are a first-class asset authored in **Slang** — a `*.shader.json` names its `.slang`
source, entry point, and optional vertex-layout `AssetId`. The cooker **always** compiles from
source (there is no precompiled-inline path) and **reflects the shader offline** into a
serializable `ShaderInterface`; the engine loads plain **SPIR-V** and gains no Slang dependency.
Shaders are Slang only; there is no GLSL path.

A material (`*.vmat.json`) references its vertex/fragment shaders by `AssetId` and declares an
**ordered, explicitly-typed** field list; the cook validates those fields against the fragment
shader's reflected parameters.

**A material is split into a parent and an instance — the standard cross-engine division.** A
**`Material`** (`AssetTypes::Material`) is the **parent**: it owns the expensive half — the
graphics pipeline, the pipeline layout, the resident shader/texture dependencies, the reflected
`MaterialField` **schema** (`GetFields()`), and a cooked **default parameter block** (held as
bytes, its bindless handle slots patched at `Finalize`). A parent owns **no** per-draw SSBO slot
and **no** per-instance mutators. A **`MaterialInstance`** (`AssetTypes::MaterialInstance`) is a
cheap **override** over a parent: an `AssetHandle<Material> Parent` kept resident, **one**
per-material SSBO slot seeded from the parent's default block and patched by its overrides, its
resident texture overrides, and the ring-buffered
`SetParam`/`SetTexture`/`SetTextureHandle`/`SetSamplerHandle` writes. `MaterialInstance::Bind`
binds the **parent's** pipeline and pushes *this* instance's selector; its
domain/layout/schema/modules delegate to the parent. So many instances share one parent's pipeline
and differ only by a per-material slot — 30 tinted bricks are 1 generated shader + 30 cheap slots,
not 30 pipelines. The mesh material list, the `MeshSource` shape fields, `Primitives`,
`MeshRenderer`, and the prefab/level reflected `AssetHandle` fields all hold
`AssetHandle<MaterialInstance>`; the draw path binds `GetMaterials()[MaterialIndex]` (an instance).

**A parent declares an explicit default instance.** A parent `*.vmat.json` declares a minted
**`defaultInstance`** `AssetId`; the cook emits a real zero-override `MaterialInstance` at that id
beside the parent `Material` blob. Every material reference (a cooked mesh's material list, a
reflected prefab/level `AssetHandle<MaterialInstance>` field, a C++ literal) names the
default-instance id, not the parent id — so a reference resolves an ordinary `MaterialInstance`
archive entry through the same `Load`/`LoadSync` path any cooked asset takes. **One `AssetId`
names one asset of one type:** the asset cache is keyed by **id alone**, and a `MaterialInstance`
request for a bare `Material` id is an ordinary `WrongType`, not a synthesized default — the
parent id and its default-instance id are distinct assets. A **MID** (Material Instance Dynamic)
is just a runtime-built instance: `AssetManager::Build<MaterialInstance>(MaterialInstanceInfo{
.Parent = …, .Overrides = … })` (or `Adopt`) plus per-frame `SetParam` — no separate dynamic type;
the editor previews a parent through a runtime default instance over it (no cooked
default-instance id needed for the preview).

A material instance's GPU parameters are **one reflection-sized block** per instance (set 0
binding 4, byte-addressed at `index * MaterialParamStride`): its bindless handle slots (`uint`
members seeded from the parent and overridden by a texture override) and its authored
scalar/vector params share that single block, laid out by reflection at each field's offset in
**scalar/tight layout** — the exact byte layout a shader's `g_MaterialParams.Load<MaterialParams>()`
reads (4-byte packed, vectors *not* 16-aligned), so the offset the cook packs a value at is the
offset the shader reads it from regardless of field order. (The cooker reflects that layout
directly rather than the std140 uniform layout, whose 16-byte vector alignment would disagree with
the `Load` for any vector placed after a scalar — see `cooker/src/Importers/SlangReflect.cpp`.)
There is no fixed engine struct and no second SSBO — a material declares an arbitrary, shader-defined
handle set (zero, one, or several), and `CookedMaterialField::Kind` (handle vs. param) is the seam
the loader patches by offset. `CookedMaterialHeader` carries `Version` (`CookedMaterialVersion`),
`Domain`, and `BlockBytes`; a stale blob rejects loudly. `Material::GetFields()` (delegated by
`MaterialInstance::GetFields()`) exposes the reflected `MaterialField` table — the editor's
parameter-schema source and an instance's override surface, so the node editor reads a material's
authorable parameters with no Slang in `libveng_editor`.

The block buffer is **N-buffered for frames-in-flight, host-visible + persistently mapped**: it
holds `framesInFlight` copies of the `MaxMaterials * MaterialParamStride` table, and each
frame-in-flight owns one region. `Register/UpdateMaterial` mark an instance dirty for
`framesInFlight` frames and write only the *current* frame's region (safe because that frame is
not yet submitted); `OnFrameAcquired` flushes each still-dirty instance into the region it just
made current. A per-frame `SetParam` / `SetTexture` is therefore a direct, stall-free write — no
staging, no `WaitIdle`, no hazard. **The current frame's region is selected by folding the frame
base (`currentFrame * MaxMaterials`, via `BindlessRegistry::GetCurrentFrameBase()`) into the
pushed material selector index in `MaterialInstance::Bind`** — not by a dynamic descriptor offset:
a `STORAGE_BUFFER_DYNAMIC` descriptor mistranslates inside set 0's bindless Metal argument buffer
on MoltenVK. The buffer stays a plain storage buffer bound at full range, and the shader's
`index * MaterialParamStride` load is unchanged.

A `Material` carries a first-class **`MaterialDomain`** (`Surface` / `PostProcess` / `Sky` /
`Translucent` / `GuiFill`, `Veng/Asset/Material.h`) selecting its output contract, pipeline shape, standard vertex shader,
and invocation site — the parameter schema, bindless handles, `.vmat` authoring, and editor
inspector are shared across domains. `Surface` is the opaque path made explicit (canonical-layout
vertex stage, g-buffer MRT output, drawn per-submesh by the geometry pass); `PostProcess` is the
fullscreen path (screenspace vertex stage, a single `SV_Target0` color, invoked by the post
chain). The lowercase `"domain"` `.vmat.json` key selects it (default `surface`), and the cook
validates the fragment shader's outputs against the domain's contract (Surface → the five-target
g-buffer MRT, `SV_Target0`..`SV_Target4`, velocity and emissive included; PostProcess → a single
`SV_Target0`).

**`GuiFill` is the UI-fill domain** (`MaterialDomain::GuiFill`): the engine's gui vertex stage, a
single premultiplied linear `SV_Target0`, drawn per UI quad by the GUI pass. Its analogy to the
other pass-built domains stops at the color format — a GUI pipeline also needs the cooked gui
vertex layout, a premultiplied-over blend state, and the pass's own push block, none of which a
fullscreen domain has an equivalent of. A material in this domain is a **fill source, not a
silhouette**: the generated entry point wraps the authored graph in the engine's fixed rounded-rect
SDF coverage and border ring (`GuiFillResolve`), so corner radius, border, clip, and rotation
compose with it for free and a material can never widen or replace the shape.

The per-draw selector push offset is domain-keyed — Surface and Translucent read their material
index from the per-draw `DrawData` SSBO and push no selector (`Material::NoSelectorPush`);
PostProcess and Sky push it at 0; GuiFill pushes it at **`GuiFillSelectorPushOffset` (20)**,
immediately after the GUI pass's own push block, which it reserves verbatim. That reservation is
the load-bearing part: without the block's `InvScreenSize` the gui vertex stage cannot reach clip
space, so a GuiFill pipeline whose layout dropped it would not draw at all. `GuiScenePass`
`static_assert`s `sizeof(GuiPushConstants)` against the constant, so the two definitions cannot
drift.

The engine ships the **standard vertex shader per domain in the core pack**: `surface.vert`
(canonical layout), `fullscreen.vert` (screenspace), and `gui.vert` (the gui vertex layout). The
material contract is **one importable engine header per domain**
(`engine/assets/core/shaders/Veng/`): `Veng/surface.slang`, `Veng/postprocess.slang`,
`Veng/sky.slang`, and `Veng/guifill.slang`. Each declares the set-0 bindless declarations, the
per-frame view block, and its own domain's push block + fragment-input struct; `surface.slang`
also holds `DrawData`, `GBufferOutput`, and `ComputeMotionVector` (the deferred geometry
contract). The split is because a fullscreen domain (PostProcess, Sky) declares exactly one
push_constant block, so it cannot coexist with the surface push in one header. A consumer (or
generated) shader `#include`s its domain's header and **declares its own `MaterialParams`** beside
it — the parameter block is per-shader by definition (the cooker reflects each shader's own struct
to pack its fields at the reflected offsets), so it is not part of the engine header. The
cross-pack include resolves because the cook threads the engine core shader dir onto every Slang
session's search path (see [cooker/CLAUDE.md](../../../cooker/CLAUDE.md)). A game references the
core `surface.vert` rather than shipping its own surface vertex stage.
