# Plan 03 — mutation tools + capability gating

**Goal:** let an agent *change* the world — add/remove a component on an entity, set a reflected
component field, spawn/destroy an entity, load a prefab — behind an explicit opt-in. This adds the
write side of the reflection bridge (`JsonToFields`), the `AllowMutations` gate, and an **optional
mutation-routing seam** so an editor host can make each edit undoable (Plan 04a fills it). All
mutations run at the `Pump()` point, which is render-thread-safe and outside any `View`/`Each`
iteration, so no `Scene` contract is bent. Depends on Plan 01 (the `McpHost` seam + `ReflectToJson`).
Independent of Plan 02.

## The starting point

- `Scene` mutation is main-thread-only and **structural edits mid-iteration are illegal** — adding/
  removing components or destroying entities during a `View`/`Each` is API misuse
  ([engine/CLAUDE.md](../../engine/CLAUDE.md)). The editor's own structural edits are "queued during
  the draw and applied after the walk returns" for exactly this reason. The MCP pump point (top of
  the app's per-frame update, before render, after `TickSimulation` completes) is outside any engine
  iteration, so a tool may mutate directly there.
- The engine already has the **type-erased component-add** primitive an agent needs:
  `Scene::AddComponent(Entity, TypeId) → void*` (`engine/include/Veng/Scene/Scene.h:217`)
  default-constructs a component of a runtime `TypeId` onto an entity — the exact call the editor's
  Add-Component picker makes (through an `AddComponentCommand`). Removal and `Scene::DestroyEntity`
  (recursive over the `Hierarchy` subtree) are its counterparts. Spawning a whole authored subtree is
  `Prefab::SpawnInto(Scene&, AssetManager&)`.
- `ReadFields` (`Veng/Reflection/Serialize.h`) is the binary write-back; this plan's `JsonToFields`
  is its JSON analogue over the same `FieldDescriptor` walk (the inverse of Plan 01's `FieldsToJson`,
  mirroring how the cooker parses JSON → fields).
- A mutable scene access **bumps the spatial version** (`GetSpatialVersion()`), so the
  `SceneBroadphase` rebuilds correctly after a tool edits geometry — no extra bookkeeping needed.
- `MeshRenderer` carries an inline recipe `Source` built into the entity's `Mesh` by
  `Prefab::SpawnInto`; the editor rebuilds it after an inspector edit via `ResolveEntity`. A
  `set_field` (or `add_component` of a `MeshRenderer`) that touches `Source` needs the same rebuild.

## What lands

### 1. The gate

`McpServerInfo::AllowMutations` (introduced as a field in Plan 00) becomes load-bearing: when
`false` (the default), the mutation tools are **not registered** — a read-only server exposes only
the Plan 01/02 inspection tools. When `true`, `RegisterMutationTools(McpServer&, const McpHost&)`
runs on construction. `tools/list` therefore honestly reflects what the server can do. Combined with
the loopback-only default bind, a default server is a safe local read surface.

### 2. `JsonToFields` — the write-side reflection walk

Library-internal (`mcp/src/ReflectToJson.{h,cpp}` — the same TU as `FieldsToJson`): `VoidResult
JsonToFields(const json&, void* obj, const TypeInfo&, const TypeRegistry&)`, the `FieldClass` switch
in reverse — parse a JSON object into a component's bytes by field `Name`, with the serializer's
schema-drift tolerance (an unknown key is skipped, not an error; a type-mismatched value is a located
error). Enum by name-or-integer, `AssetHandle` from an `AssetId`, `Variant` from `{ type, value }`,
`Array` via the `ArrayResize`/`ArrayElement` shims, `Struct` recursion — the same coverage
`FieldsToJson` has. Every agent-supplied type name it resolves (a `Variant`'s active-type
`QualifiedName`, an enum enumerator) goes through the fallible registry lookup and yields a located
error on a miss — it never reaches an asserting `registry.Info()` (see Plan 01). The `Array` arm
resizes to the incoming JSON array's element count with a sensible sanity cap, so a malformed
`values` array can't trigger a pathological allocation (single trusted local client, so this is a
guard against an accidental huge count, not a hostile one).

### 3. The optional mutation-routing seam

`McpHost` gains an optional hook the mutation tools consult before touching the scene:

```cpp
// null in a game host (mutations apply raw to the scene);
// set by an editor host so each edit is undoable + marks the document dirty (Plan 04a).
function<bool(const McpMutation&)> ApplyMutation;   // returns true if it handled the edit
```

`McpMutation` is a small tagged description of one edit (add/remove component, set field, spawn,
destroy, load prefab) with its resolved arguments. When `ApplyMutation` is null (the game runtime),
the tool applies the edit **directly** to `McpHost::CurrentWorld()`; when it is set (the editor), the
tool hands the `McpMutation` to the host, which pushes the corresponding editor command onto the
`CommandStack` — so an agent's edit undoes exactly like a human's. Same generic tools, two backends;
the tools never branch on "am I in the editor", they just consult the hook.

A null hook is a *silent* raw-scene path, which is correct for a game but a bug for an editor host
that meant to route through the `CommandStack`. The fallback is therefore deliberately load-bearing:
Plan 04a's editor host **asserts/logs** if it constructs an `McpHost` over a document scene without
setting `ApplyMutation`, so a forgotten wiring is caught rather than silently producing
un-undoable agent edits.

### 4. The mutation tools (registered only when `AllowMutations`)

Each resolves + validates the target entity (a stale generational id is an error, not silent UB),
routes through `ApplyMutation` if present else applies raw, and runs at the safe pump point:

- **`entity.add_component`** — arg `{ id, component: <QualifiedName>, values?: { <field>: … } }`.
  `Scene::AddComponent(id, TypeIdOf(component))`, then `JsonToFields` over `values` (partial). If the
  component is a `MeshRenderer` with a `Source`, trigger the same mesh rebuild the editor's
  `ResolveEntity` does (`BuildPrimitiveMesh`). Error if the component already present or the type is
  unregistered. **This is the "add a Light to entity 3" verb.**
- **`entity.remove_component`** — arg `{ id, component: <QualifiedName> }`. The type-erased remove (the
  `Hierarchy` component is not removable — reject it, as the inspector does).
- **`entity.set_field`** — arg `{ id, component: <QualifiedName>, values: { <field>: … } }`. Resolve
  the component (error if absent), apply `JsonToFields` over `values` (a **partial** update), through
  the mutable accessor so the spatial version bumps; the `MeshRenderer`/`Source` rebuild trigger as
  above.
- **`entity.spawn`** — arg `{ name?, parent?: id, components?: { <QualifiedName>: {…}, … } }`. Create
  an entity, add `Name`/`Transform` defaults as appropriate, add + `JsonToFields` each named
  component, parent it (`SetParent`) if given. Returns the new entity id.
- **`entity.destroy`** — arg `{ id }`. `DestroyEntity` (recursive). Returns the count destroyed.
- **`world.load_prefab`** — arg `{ asset: <AssetId>, parent?: id }`. Load through `McpHost::Assets`
  and `SpawnInto`, optionally under `parent`. Returns the spawned root ids. A prefab that is not
  resident cooks nothing at runtime (the engine has no cook-on-demand); a missing asset is an error.
  (The editor's cook-on-demand load is a Plan 04b editor tool, not this runtime one.)

## Notes & constraints

- **All run at the pump point**, so a spawn/destroy/add-component never lands mid-`View`; the tool
  code needs no defer-queue of its own (unlike the editor's draw-time edits — the pump *is* the safe
  point).
- **A stale `Entity` is validated before use** in every mutation tool — the ECS gives no re-entrancy
  guard and a stale handle is silent UB, so the tool checks liveness (`Scene::IsAlive`) and returns
  an error rather than trusting the agent's id.
- **This is not a transaction system.** Each tool call is one atomic edit at one pump; there is no
  multi-call rollback. Multi-entity transactions are noted future work — but single-edit **undo** in
  the editor comes for free through the `ApplyMutation` command routing.

## Files (sketch)

- `mcp/include/Veng/Mcp/McpHost.h` — add `ApplyMutation` + the `McpMutation` description type.
- `mcp/src/ReflectToJson.{h,cpp}` — add `JsonToFields`.
- `mcp/src/MutationTools.cpp` (new) — `RegisterMutationTools` + the six handlers + the route-or-raw
  helper.
- `mcp/src/McpServer.cpp` — call `RegisterMutationTools` only when `AllowMutations`.

## Verification

- A headless test (`mcp_mutation`, no GPU for the scene-only ops; gate the `world.load_prefab` mesh
  build `gpu`): construct a server with `AllowMutations = true` and a **null** `ApplyMutation`
  (raw-scene path), and over loopback `entity.spawn`, `entity.add_component` a `Light`, `entity.get`
  it back (round-trips `JsonToFields` → `FieldsToJson`), `entity.set_field` and re-read,
  `entity.remove_component`, `entity.destroy` and confirm the stale id now errors. A second server
  with `AllowMutations = false` asserts the mutation tools are **absent** from `tools/list`.
- Assert the spatial version moved after a mutation.
- `ctest` green; `include_hygiene` green.
