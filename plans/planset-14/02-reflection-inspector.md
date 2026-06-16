# Plan 02 — reflection-driven inspector

**Goal:** wire the `TypeRegistry` / `FieldDescriptor` layer into the `InspectorPanel` so
clicking an entity in the asset browser (or a future scene hierarchy panel) populates the
inspector with each component's fields, drawn with the correct widget per `FieldClass`.
Deliver built-in widgets for every `FieldClass` the engine uses today. The `Spinner` and
built-in components (`Name`, `Transform`, `Camera`, `MeshRenderer`, `Light`) all render
correctly in the inspector.

## Inspector selection model

`InspectorPanel` holds a `optional<Entity> m_SelectedEntity` and a `const Scene*
m_Scene`. These are set by the host; a future scene-hierarchy panel will set them directly.

For this plan, the asset browser's entity list (the scene viewport's scene is visible to the
editor host) drives selection: selecting a row in the asset browser that corresponds to a
scene entity sets `m_SelectedEntity`.

`InspectorPanel::OnImGui()`:
1. If `m_SelectedEntity` is null → "Nothing selected".
2. Otherwise, for each component type registered in `m_Scene->GetTypeRegistry()` that the
   entity has (`scene.Has<T>(entity)` per type in the sparse-set), draw a collapsing header
   with the component's `TypeInfo.Name`.
3. Inside each header, call `DrawFields(componentPtr, typeInfo)`.

`DrawFields` is the recursive walker:

```cpp
void DrawFields(void* base, const TypeInfo& type);
void DrawField(void* fieldPtr, const FieldDescriptor& field);
```

`DrawFields` iterates `type.Fields`, skipping any with `FieldDescriptor.Hidden == true`,
then calls `DrawField` for each.

## Widget per `FieldClass`

`DrawField` switches on `field.Class`:

| `FieldClass` | Widget |
|---|---|
| `Scalar` (f32) | `ImGui::DragFloat` with `field.Step` (default 0.01f), `field.Min`/`Max` if set |
| `Scalar` (i32/u32) | `ImGui::DragInt` |
| `Scalar` (bool) | `ImGui::Checkbox` |
| `Vector` (vec2/vec3/vec4) | `ImGui::DragFloat2/3/4` |
| `Quaternion` | convert to Euler (`glm::eulerAngles`), `DragFloat3`, convert back; label "(Euler °)" |
| `String` | `ImGui::InputText` (fixed 256-byte staging buffer; write back on Enter/deactivate) |
| `Enum` | `ImGui::Combo` from the enum's `TypeInfo.EnumValues` |
| `AssetHandle` | display the asset id (hex) + name if resolvable from the asset manager; clicking opens the asset browser to that id |
| `Struct` | nested `DrawFields` call under a `TreeNodeEx` |
| `Matrix` | read-only 4-column display (matrices are typically derived state; `ReadOnly` descriptor attribute respected) |
| `Reference` (Entity) | display `entity.Index`/`Generation` as text (entity handles are not editable via the inspector yet) |

Custom widgets registered in `EditorRegistry::RegisterFieldWidget` override the built-in for
a given field `TypeId`. The lookup key is the field's `TypeId`; the override receives the
raw field pointer and the descriptor.

## Iterating components on an entity

`Scene` exposes `View<Ts...>()` and `Each<Ts...>()` for known compile-time types, but the
inspector needs to iterate **all** components on a given entity without knowing their types
at compile time. Extend `Scene` with:

```cpp
// Calls fn(typeId, componentPtr) for every component the entity has.
void ForEachComponent(Entity entity, function<void(TypeId, void*)> fn) const;
```

This iterates the sparse-set map (one pool per `TypeId`) and calls `fn` for any pool that
has the entity. The `TypeRegistry` then resolves `TypeId → TypeInfo` so `DrawFields` can
walk the descriptors.

`ForEachComponent` is added to `engine/include/Veng/Scene/Scene.h` and implemented in
`engine/src/Scene/Scene.cpp`. It touches no backend and has no GPU interaction; the
`include_hygiene` test stays green.

## Connecting the inspector to the live scene

`EditorHost` passes `&m_Scene` and the `SceneViewportPanel`'s rendered scene into the
`InspectorPanel` each frame (the viewport builds the scene; the inspector reads it as
`const Scene*`). A future hierarchy panel will own the selection model; for now, the asset
browser row click is the driver.

## Built-in component smoke test

`hello_triangle-editor` launches and selects the sphere entity. The inspector shows:
- `Name` — a string InputText showing "Sphere" (or the prefab entity name).
- `Transform` — Position (DragFloat3), Rotation (Euler DragFloat3), Scale (DragFloat3).
- `MeshRenderer` — Mesh asset handle (id + "sphere-mesh" resolved name).
- `Spinner` — Speed (DragFloat, min/max from its VE_FIELD descriptor if present).

Editing the `Spinner.Speed` field in the inspector immediately affects the running scene
(the `Spinner` system reads it each frame); this is the live-edit proof.

## Tests

- No new GPU test — the inspector is pure CPU/ImGui.
- `include_hygiene`: `Scene::ForEachComponent` is in a public header; confirm it pulls in no
  backend includes.
- Manual: inspector shows all components for the selected entity with correct widgets; edits
  to `Spinner.Speed` take effect in the viewport immediately.

## Acceptance

Clean build; `hello_triangle-editor` shows a populated inspector for the selected entity
with correct widgets per `FieldClass`; editing `Spinner.Speed` changes the visible rotation
rate; `ctest` green; smoke PPM unchanged. Commit: `Plan 02: reflection-driven inspector,
Scene::ForEachComponent, per-FieldClass widgets`.
