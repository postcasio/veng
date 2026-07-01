# Plan 01 — reflection→JSON bridge + the read-only world tools

**Goal:** give agents the *current world* — list entities, inspect an entity's components as JSON,
query entities by component type, and read scene stats. This introduces the **`McpHost`** provider
seam (how the server reaches the live scene) and the **`ReflectToJson`** walk (how a reflected
component becomes JSON), both reused by every later plan. All tools are **read-only** and run on the
render thread at the `Pump()` point. Depends on Plan 00.

## The starting point

- `Scene` is a `TypeId`-keyed sparse-set ECS (`Veng/Scene/Scene.h`,
  [engine/CLAUDE.md](../../engine/CLAUDE.md)): a generational `Entity { u32 Index; u32 Generation; }`
  (`Entity::Null`), templated `Add`/`Get`/`Has`/`View<Ts…>`/`Each<Ts…>`, and — the key primitive
  here — `ForEachComponent(Entity, const function<void(TypeId, void*)>&)`, the type-agnostic
  enumeration the editor inspector walks. A `const View`/`Each` does **not** bump the spatial version;
  a read-only tool iterates without forcing a version move.
- The builtins (`Name`, `Transform`, `Hierarchy`, `Camera`, `MeshRenderer`, `Light`, `Animator`, …)
  are plain reflected components; a game's own types register identically.
- The `TypeRegistry` (`Veng/Reflection/TypeRegistry.h`) resolves a `TypeId` to a `TypeInfo`
  (`Name`/`Namespace`/`QualifiedName`, its `FieldDescriptor`s). A `FieldDescriptor` carries `Name`,
  `Offset`, the field's `TypeId`, and its closed `FieldClass`
  (`Scalar`/`Vector`/`Quaternion`/`Matrix`/`String`/`AssetHandle`/`Reference`/`Struct`/`Enum`/
  `Variant`/`Array`), plus the type-erased container/variant shims (`ArraySize`/`ArrayElement`/…,
  `VariantActiveType`/`VariantActivePtr`/…).
- The binary serializer `WriteFields`/`ReadFields` (`Veng/Reflection/Serialize.h`) walks these same
  descriptors — but produces a **binary** blob, not JSON. This plan writes a JSON-emitting walk with
  the identical `FieldClass` switch (the read side of the editor's `DrawFieldWidget` and the cooker's
  JSON→field parse).
- `Application` exposes `GetTypeRegistry()`, `GetAssetManager()`, `GetPrimaryViewport()`, and the
  managed world; the editor exposes the active document's `Scene*` (Plan 04 fills the seam from
  there).

## What lands

### 1. `McpHost` — the provider seam

A new public struct (`mcp/include/Veng/Mcp/McpHost.h`), mirroring `VengModuleHost`:

```cpp
struct McpHost
{
    TypeRegistry& Types;                                   // resolve TypeId → fields
    AssetManager& Assets;                                  // asset queries (Plan 03) + name lookups
    function<Scene*()> CurrentWorld;                       // the scene an agent inspects; may return null
    function<Renderer::Viewport*(string_view name)> Viewport; // Plan 02; unset here is fine
};
```

`McpServer::Create` gains an `McpHost` (or a `SetHost` / a second `Create` overload — the agent picks
the shape; `Create(const McpServerInfo&, const McpHost&)` is the clean one since the host is required
for the built-in tools). The built-in tools capture the host by reference (the host outlives the
server — the app owns both). A null `CurrentWorld()` return means "no world"; every world tool
handles it by returning an empty/So-stated result, never dereferencing.

### 2. `ReflectToJson` — the reflected-value → JSON walk

A library-internal helper (`mcp/src/ReflectToJson.{h,cpp}`, not a public header — it names
`nlohmann::json`) with the read-side `FieldClass` switch:

- `json FieldsToJson(const void* obj, const TypeInfo&, const TypeRegistry&)` — walks the type's
  `FieldDescriptor`s, emitting `{ <field name>: <value> }`, keyed by the serialization `Name` (not the
  display label — matches on-disk identity).
- Per `FieldClass`: `Scalar` → number/bool; `Vector`/`Quaternion` → array; `Matrix` → nested array;
  `String` → string; `Enum` → the enumerator name (via the registry's enum metadata) with the raw
  integer alongside; `AssetHandle` → the referenced `AssetId` (decimal, plus the resolved asset name
  if `Assets` can name it); `Reference` → the referenced `Entity` (index+generation); `Struct` →
  recurse; `Variant` → `{ "type": <active QualifiedName>, "value": <recursed> }` (the same shape the
  cooker reads); `Array` → a JSON array driven by the `ArraySize`/`ArrayElement` shims.
- This is the **canonical component encoding** for MCP; Plan 03 adds the inverse
  (`JsonToFields`, the `ReadFields` analogue) for `entity.set_field`.

### 3. The world tools (registered by the library into the server)

Registered in a `RegisterWorldTools(McpServer&, const McpHost&)` the server calls internally on
construction (so a consumer that just links `veng::mcp` and constructs a server gets them). All run
under `Pump()` on the render thread; all read through `const` scene accessors (no version bump):

- **`world.list_entities`** — `{ entities: [{ id: {index, generation}, name, components: [<QualifiedName>…] }], nextCursor? }`.
  Iterates the scene's entities; `name` from the `Name` component if present; `components` from
  `ForEachComponent` collecting each `TypeId`'s `QualifiedName`. Optional arg `component`
  (`QualifiedName`) filters to entities having it. **Paginated** per the shared list convention:
  args `{ limit?, cursor? }`, `limit` defaulting to a sensible cap (e.g. 200) so an unbounded world is
  not dumped in one call, and the response carries `nextCursor` while more entities remain — the agent
  pages through the whole set rather than losing the tail. The cursor is opaque (internally the resume
  entity index; a cursor whose entity was destroyed resumes at the next live one). (An
  information-volume convention, not a DoS defense — the server has a single trusted local client.)
- **`entity.get`** — arg `{ id: {index, generation} }` (or `{ index, generation }`) → the full
  component dump: `{ id, name, components: { <QualifiedName>: <FieldsToJson> } }`. A stale/invalid
  entity is an `isError` result (never the silent-UB stale-handle path — validate against the scene
  before access).
- **`world.query`** — arg `{ component: <QualifiedName>, limit?, cursor? }` → the entities having that
  component (ids + names), `{ entities, nextCursor? }`; the "which entities have a `Light`?"-style
  question, distinct from `world.list_entities`'s full listing. **Paginated** the same way.
- **`scene.stats`** — `{ entity_count, spatial_version, bounds: {min, max} }` from the scene's entity
  count, `GetSpatialVersion()`, and `SceneBounds(scene)` (`Veng/Scene/Transforms.h`). Cheap
  situational awareness for an agent.

Tool names use a `noun.verb` / `noun.property` convention (`world.*`, `entity.*`, `scene.*`) so the
later `render.*` / `editor.*` families read consistently.

## Notes & constraints

- **Entity identity in JSON is the generational handle**, `{ index, generation }` — an agent that
  holds an id across a destroy+respawn gets a validation error on the stale handle, not a wrong
  entity. Names are display-only and non-unique; ids are the addressing key.
- **`QualifiedName` is the type key** in tool arguments and output (`"Veng::Light"`,
  `"MyGame::Spinner"`), matched with `TypeNameMatches` (strict) — the same key the editor and cooker
  match on. Namespaces disambiguate.
- **An agent-supplied type name is validated against the registry before any asserting lookup.**
  `TypeRegistry` resolution that assumes registration (`registry.Info(TypeId)` / `IdOf`) is a fatal
  assert on an unregistered type — a schema fault, not recoverable. So every tool that takes a
  `QualifiedName` (here `component`; in Plan 03 the mutation types, enum values, nested struct types)
  looks it up through the *fallible* path first and returns an `isError` result on a miss, never
  letting an unknown agent-supplied string reach a codepath that aborts the whole process.
- The walk **tolerates schema drift** the same way the serializer does: an unregistered nested type
  or an empty variant is reported as such, not a crash.

## Files (sketch)

- `mcp/include/Veng/Mcp/McpHost.h` (new) — the provider seam.
- `mcp/include/Veng/Mcp/McpServer.h` — `Create` takes the `McpHost`; a note that world tools
  auto-register.
- `mcp/src/ReflectToJson.{h,cpp}` (new) — the `FieldClass` → JSON walk.
- `mcp/src/WorldTools.cpp` (new) — `RegisterWorldTools` + the four handlers.
- `mcp/src/McpServer.cpp` — call `RegisterWorldTools` on construction.

## Verification

- Extend/add a headless test (`mcp_world`, no GPU): build a `TypeRegistry` with the builtins + a
  test component, spawn a small scene, wire an `McpHost` returning it, drive the pump, and over
  loopback call `world.list_entities` / `entity.get` / `world.query` / `scene.stats`, asserting the
  JSON (entity count, a known component's field values round-trip through `FieldsToJson`, the type
  filter works). This exercises `ReflectToJson` across `Scalar`/`Vector`/`Enum`/`AssetHandle`/`Struct`
  at least. Include a case calling `world.query` / `entity.get`'s `component` filter with an
  **unregistered** `QualifiedName` and assert an `isError` result with the process **still alive**
  (guards the fatal-assert path above). Add a **pagination** case: `world.list_entities` with a small
  `limit` over a scene larger than it returns `nextCursor`, and paging with that cursor walks the
  **full** entity set exactly once with no duplicates or gaps.
- `include_hygiene` still green (`ReflectToJson.h` is under `src/`, never public).
- `ctest` green; no example or GPU change.
