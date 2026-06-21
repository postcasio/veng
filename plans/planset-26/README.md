# planset-26 — primitive recipes in prefabs (reflection variants + async mesh streaming)

**Phase goal:** let a prefab **store the recipe of a procedural mesh** — "this is an
icosphere, radius 0.8, 4 subdivisions, brick material" — rather than a baked mesh or a
dangling runtime handle, load it through the ordinary asset/prefab path, and select it
for an entity in the editor. The recipe is regenerated into a real `Mesh` at spawn,
**streamed in off the render thread**, and lands in the entity's `MeshRenderer` a few
frames later exactly as a cooked mesh would.

Today a primitive mesh is built at runtime (`Primitives::Icosphere` →
`Mesh::Create` → `AssetManager::Adopt`) and the adopted handle carries the **invalid
`AssetId`** — so a `MeshRenderer` pointing at it serializes as "no mesh" and round-trips
to empty ([`Serialize.cpp` `AssetHandle`](../../engine/src/Reflection/Serialize.cpp)).
The resulting mesh is unaddressable; the only thing worth persisting is the *recipe*.
Persisting a recipe means the reflection layer must be able to serialize a **tagged
union** — a value that is one of several typed shapes — which `FieldClass` cannot
express. So this planset **adds variant support to reflection first**, then models the
primitive on it.

It builds on two earlier plansets and changes neither's contracts:
[planset-7](../planset-7/README.md) (the `Primitives::` generators + `MeshData` +
`Mesh::Create`) supplies the geometry; [planset-6](../planset-6/README.md) (the
`TaskSystem`, async `Buffer/Image::Upload`, the main-thread continuation pump) supplies
the streaming machinery. The deliverable that outlives the primitive use case is the
reusable **`FieldClass::Variant`** — the first sum type the serializer, the cooker, and
the editor inspector all understand.

## The decisions that shape this planset

1. **Add `FieldClass::Variant`, do not fake it.** A C++ `union`, a discriminator +
   pointer, or a type-erased inline buffer are all invisible to the reflection walker:
   it resolves fields by `offsetof` + `TypeId` and writes **every** field at a static
   offset, with no notion of an active member. So a variant is a genuine new
   `FieldClass`, threaded through the **five** consumers that switch on the closed set —
   the engine serializer, the prefab spawn-time rehydration walk (`Prefab::SpawnInto`'s
   `Resolve`), the cooker's `PrefabImporter`, the editor inspector, and
   `RegisterDependencies`. This is the only way to a single, clean `PrimitiveComponent`
   type with a non-wasteful payload, and it is a reusable reflection primitive (any
   future tagged-union component — a constraint, a collider shape, an animation track —
   rides it).

2. **`Variant<Ts...>` is `std::variant<std::monostate, Ts...>` behind a reflectable
   thunk interface.** Reflection never reaches into the variant's storage by offset; it
   goes through type-erased thunks recorded on the variant's `TypeInfo`:
   `VariantActiveType(const void*) → TypeId` (`InvalidTypeId` = empty/monostate),
   `VariantActivePtr(void*) → void*` / `VariantActivePtrConst(const void*) → const void*`,
   `VariantSetActive(void*, TypeId) → void*` (emplace the default-constructed alternative,
   return its storage; unknown id → `nullptr`), `VariantClear(void*)` (reset to empty),
   and a `VariantAlternatives` list of the alternative `TypeId`s. `std::variant` is movable and
   destructible, so the existing `MoveConstruct`/`Destruct` pool thunks work unchanged
   — a `Variant` member is a normal poolable value, not a heap-owning one. The
   alternatives must each be registered `Struct`-class types; `VE_VARIANT` registers
   them as dependencies like `VE_FIELD` does for a struct field.

3. **The serialized form of a variant is a `TypeId` tag + the active member's record.**
   `WriteValue` appends the active `TypeId` (a `u64`; `InvalidTypeId` for empty) then,
   if non-empty, recurses `WriteFields` on the active member through
   `registry.Info(activeType)`. `ReadValue` reads the tag, `SetActive`s it, and
   `ReadFieldsInner`s into the returned storage. **On-disk identity stays the `TypeId`**
   (consistent with the rest of the format), and the schema-drift tolerance generalizes:
   an unknown or unregistered tag leaves the variant empty and skips the record, exactly
   as an unknown field name is skipped today.

4. **The `.prefab.json` shape for a variant is `{ "type": <registered name>, "value":
   { …fields… } }`.** The cooker matches `"type"` against the registered `TypeInfo.Name`
   of the variant's alternatives (a name not among them is a located cook error),
   recurses `BindField` into `"value"`, and emits the same tag-plus-record bytes the
   engine serializer reads. JSON keeps the author-facing **name**; the cooked bytes keep
   the **`TypeId`** — the same name/identity split the rest of the format uses. An
   absent variant field stays empty (schema tolerance).

5. **The primitive is one `PrimitiveComponent`, separate from `MeshRenderer`.**
   `PrimitiveComponent { Variant<CubeShape, PlaneShape, SphereShape, IcosphereShape>
   Shape; }` is the recipe; each alternative is a small reflected struct of just that
   shape's parameters plus its `AssetHandle<Material>`. `MeshRenderer` is **unchanged**
   (`AssetHandle<Mesh> Mesh`) — the renderer query and the "renderable" concept stay as
   they are. A resolution step fills `MeshRenderer.Mesh` from the active shape. The
   variant's active alternative *is* the primitive kind, so the editor's variant widget
   (decision 7) gives primitive selection and per-kind parameter editing for free.

6. **Generation streams in through an async sibling of `Adopt`.** Residency is already
   "`AssetCacheEntry.Resource != nullptr`" and the async `Load` path already creates an
   empty entry, returns the handle, runs the work off-thread, and fills the entry from a
   main-thread continuation. `Adopt` makes a *detached* entry but fills it
   synchronously. The new `AssetManager::CreateAsync<T>` is the intersection: a detached
   **pending** entry returned immediately, a `Task<Ref<T>>` that builds the resource
   off-thread, and a main-thread continuation that sets `Resource`. The renderer's `GatherMeshes`
   already skips a not-yet-resident mesh, so an unresolved primitive renders nothing for
   a few frames and then **appears** — no `WaitIdle`, no spawn stall, identical handle
   semantics to a cooked-mesh load. `CreateAsync` itself does not dedup (like `Adopt`);
   identical recipes dedup through a caller-owned **`PrimitiveMeshCache`** (a strong-handle
   map keyed on the shape value, touched only on the render thread) so N cubes share one
   GPU mesh.

7. **Resolution is an explicit engine entry point, not magic in the deserializer.**
   `ResolvePrimitiveMeshes(Scene&, AssetManager&, PrimitiveMeshCache&)` scans
   `(PrimitiveComponent)` entities whose `MeshRenderer.Mesh` does not already hold the
   current shape's mesh, fires `CreateAsync` from the active shape (deduped through the
   cache), and stores the pending handle (adding a `MeshRenderer` when absent). It is
   idempotent — an entity already pointing at its current shape's mesh is skipped — so the
   app calls it once after `SpawnInto` and the prefab editor calls it every frame.
   `Prefab::SpawnInto` stays generic and component-agnostic; no primitive logic leaks into
   the prefab spawner.

## Scope of this phase

| In scope | Out of scope (later / other phases) |
|---|---|
| `FieldClass::Variant` + `Variant<Ts...>` + `VE_VARIANT` + `TypeInfo` variant ops | A general `FieldClass::List`/array (a separate reflection increment) |
| Variant (de)serialization in the engine serializer + the prefab spawn-time `Resolve` walk, with skip-tolerant unknown tags | Node→Slang or any non-reflection consumer of variants |
| Variant JSON binding + validation in the cooker `PrefabImporter` | Cylinder/cone/torus/capsule shapes (easy follow-ons to the shape set) |
| Variant widget in the editor inspector (entity + node-property paths) | A bespoke "mesh source" combo merging cooked-vs-primitive into one control |
| Async `Mesh` upload factory (built on planset-6's `Buffer::Upload`) | Async `Material`/`Texture` adopt (same mechanism, not needed here) |
| `AssetManager::CreateAsync` (async sibling of `Adopt`): pending detached entry + continuation; a caller-owned `PrimitiveMeshCache` for shape-keyed dedup | Hot-reload / re-resolution when a recipe is edited at runtime beyond the editor loop |
| `PrimitiveComponent` (variant payload) + `ResolvePrimitiveMeshes` | Storing a baked primitive as an `AssetId`-addressable mesh asset |
| hello-triangle authored as a primitive **prefab**; sample `.prefab.json` | A scene/level format beyond the existing prefab |
| Unit + cooker + GPU tests for each layer; docs/CLAUDE updates | Editing the cooked **on-disk** mesh format (untouched) |

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 01 | [Reflection variants — `FieldClass::Variant`](01-field-class-variant.md) | Add `FieldClass::Variant`; a `Variant<Ts...>` value type over `std::variant<monostate, Ts...>`; the `VE_VARIANT(Type, id)` authoring macro specialising `VengReflect` (Class=Variant, alternatives auto-registered via `Variant<Ts...>::RegisterAlternatives`); `TypeInfo` variant thunks (`VariantActiveType`/`VariantActivePtr`/`VariantActivePtrConst`/`VariantSetActive`/`VariantClear`/`VariantAlternatives`), populated by an `if constexpr` branch in `RegisterImpl<T>`; `FieldClassOf` mapping. Unit-tested with a throwaway test variant; `include_hygiene` stays green. | done |
| 02 | [Variant (de)serialization](02-variant-serialization.md) | `WriteValue`/`ReadValue` Variant cases in `Serialize.cpp`: active `TypeId` tag + recursed active-member record; empty = `InvalidTypeId`; unknown/unregistered tag is skip-tolerant. Round-trip + tolerance + empty-state unit tests. | done |
| 03 | [Cooker variant binding + validation](03-cooker-variant.md) | `PrefabImporter` `FieldClass::Variant` case: parse `{ "type", "value" }`, match `"type"` to an alternative's registered name (located error otherwise), recurse `BindField`, emit tag-plus-record bytes byte-identical to the engine reader. Cooker test for valid + bad-tag + omitted. | done |
| 04 | [Editor variant inspector widget](04-editor-variant-widget.md) | `DrawFieldWidget` Variant case: a combo over the alternatives' display names (plus "(none)"), `SetActive` on change, recurse the active member's fields as indented rows. Shared by the entity inspector and the node-property inspector. | done |
| 05 | [Async `Mesh` upload factory](05-async-mesh-build.md) | A `Mesh` async build path that runs CPU geometry generation + the host-visible vertex/index `Buffer::Upload(TaskSystem&)` memcpys on a worker and assembles the `Ref<Mesh>` in a main-thread continuation — the async sibling of planset-7's blocking `Mesh::Create`. Buffers are host-visible (no staging, no transfer-queue copy). | done |
| 06 | [`AssetManager::CreateAsync`](06-async-adopt.md) | `CreateAsync<T>(Task<Ref<T>>)`: a detached **pending** `AssetCacheEntry`, the handle returned immediately, the task finalized into `Resource` through the existing main-thread pump; pending entries are kept alive (never GC-evicted) until resident. `IsLoaded()` flips when resident. Dedup is **not** here — it lives in plan 07's `PrimitiveMeshCache`. | done |
| 07 | [`PrimitiveComponent` + resolution](07-primitive-component.md) | Per-shape param structs (`CubeShape`/`PlaneShape`/`SphereShape`/`IcosphereShape`, each with its params + `AssetHandle<Material>`); `PrimitiveComponent { Variant<…> Shape; }`; `ResolvePrimitiveMeshes(Scene&, AssetManager&, PrimitiveMeshCache&)` generating from the active shape via `CreateAsync` + the shape-keyed `PrimitiveMeshCache` and storing the pending handle in `MeshRenderer.Mesh`. Builtin-type registration. | ready |
| 08 | [Sample primitive prefab + docs](08-sample-prefab-docs.md) | Author hello-triangle's geometry as a `*.prefab.json` carrying `PrimitiveComponent`s; cook it (module-reflected validation), spawn + resolve at startup; regenerate the smoke golden only if the rendered geometry moves (it should not); update `CLAUDE.md`, the asset docs, and `plans/README.md`. | ready |

## Dependency & order

Three arcs, two of them independent:

```
Arc A (variants):   01 → 02 → { 03 , 04 }
Arc B (async):      05   ‖   06            (independent of A)
Arc C (primitive):  { 01 , 02 , 05 , 06 } → 07 → 08   (08 also needs 03 + 04)
```

- **01 → 02** strictly: the serializer needs the `TypeInfo` ops and the `Variant` type.
  **03** (cooker) and **04** (editor) both consume 01+02 and are independent of each
  other — parallelizable.
- **05 ‖ 06** are pure asset/renderer infrastructure with **no dependency on the variant
  arc** — they can run in parallel with A from the start.
- **07** is the join: it needs the variant data model (01, 02) and the async streaming
  (05, 06). **08** needs 07 plus the cooker (03) to cook the sample prefab and the editor
  (04) to select primitives in it.

## Process & conventions

Same cadence as every planset: implement → migrate `examples/hello-triangle` in the
same pass as any breaking change (07 and 08 touch it) → verify (clean build, `ctest`
green, smoke binary writes a correct-sized PPM) → update this table → one commit per
plan, `Plan NN: <summary>` with a `Co-Authored-By` trailer (`planset-26:` for
roadmap-only edits).

- **Use placeholder `0x…ULL` `TypeId`s** for the new variant/shape/component types while
  implementing; once the build is green, mint the real ones with `vengc
  generate-type-id` and replace the placeholders (hex in C++, decimal in the sample
  `.prefab.json`). Never hand-invent a final id.
- **Plans 01–04, 06 are CPU/logic** — their tests run under `ctest -L unit` (engine) and
  the cooker suite (03), driver-free. **Plans 05, 07, 08 create/draw GPU resources** and
  must pass the `VE_DEBUG` validation gate (`ctest --test-dir build-debug -L validation`);
  the async mesh build uses planset-6's host-visible `Buffer::Upload` (a worker-thread
  memcpy into HOST_VISIBLE memory — no staging, no transfer-queue copy), already
  validation-clean, so no allowlist widening is expected — no plan may widen it.
- **`include_hygiene`** must stay green: the variant headers pull in only the existing
  public reflection headers (glm/fmt/std), and `Variant<Ts...>` adds `<variant>` only.
- **Delegation.** Good `model: sonnet` subagent work once the contract above the seam is
  fixed: the cooker case (03) after 01/02 land, the editor widget (04) after 01/02, the
  per-shape param structs and their generators' wiring (07) once the `Variant`/`CreateAsync`
  shapes are fixed, and the sample/test bodies (08). Keep on the main thread: the `Variant`
  type + `TypeInfo` thunks + the `Register<T>()` variant branch + macro (01), the serializer
  + `Prefab::Resolve` seam (02), the `CreateAsync` pending-entry lifecycle (06), the
  `ResolvePrimitiveMeshes`/`PrimitiveMeshCache` lifecycle (07), and verification/commits.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.

## On completion

A prefab can carry a procedural-mesh recipe, cook and load it through the ordinary path,
and have it stream into the scene; the editor selects a primitive kind and edits its
parameters through a reflection-driven variant widget. Reflection gains a reusable sum
type (`FieldClass::Variant`) understood end to end — engine, cooker, editor. Update
[plans/README.md](../README.md) with the planset-26 line and note in `CLAUDE.md` /
the asset docs that primitives are now a persistable, prefab-authored, async-streamed
component, and that reflection supports tagged-union fields.
