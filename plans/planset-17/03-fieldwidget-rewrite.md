# Plan 03 — the reflection inspector on `Veng::UI`

**Goal:** rewrite `editor/src/FieldWidget.cpp` (`DrawFieldWidget` + `DrawAssetPicker`) on
`Veng::UI` overloads. This is the **showcase** of the toolkit: the per-`FieldClass`
hand-written dispatch — the `DragFloat`/`DragFloat2`/`DragFloat3` multiplication the design
overview calls out — collapses into one overloaded `Drag`, and every inspector widget
routes through the engine surface. Shared by the entity inspector and the node-property
inspector, so both gain the consistency in one edit.

**Depends on** plans 00 (widgets) and 01 (scopes).

## The collapse

`FieldWidget.cpp` is a `switch (field.Class)` over the closed `FieldClass` set
(`FieldWidget.cpp:110–227`). The migration maps each arm onto `Veng::UI`; the headline is
`FieldClass::Vector`:

```cpp
// today — a three-way if over the concrete vector type:
case FieldClass::Vector:
    if      (vec2) ImGui::DragFloat2(labelText, value_ptr(*v2), step);
    else if (vec3) ImGui::DragFloat3(labelText, value_ptr(*v3), step);
    else if (vec4) ImGui::DragFloat4(labelText, value_ptr(*v4), step);
    break;

// after — one overloaded Drag per branch, same DragOptions:
case FieldClass::Vector:
    const UI::DragOptions opts{ .Speed = step };
    if      (vec2) UI::Drag(label, *v2, opts);
    else if (vec3) UI::Drag(label, *v3, opts);
    else if (vec4) UI::Drag(label, *v4, opts);
    break;
```

The branch on concrete type stays (the erased `void* fieldPtr` still needs the `TypeId`
discriminator to pick the cast), but every leaf now calls **one name** with **one options
struct**, so drag speed, clamp, and format are set in a single place — the consistency knob
the toolkit exists to provide.

### Full per-arm mapping

| `FieldClass` arm | Today | `Veng::UI` |
|---|---|---|
| `Scalar` (f32) | `ImGui::DragFloat(l, p, step, min, max, "%.3f", clampFlag)` | `UI::Drag(l, *f, { .Speed = step, .Min = min, .Max = max })` |
| `Scalar` (i32) | `ImGui::DragInt(l, p)` | `UI::Drag(l, *i)` |
| `Scalar` (u32) | `ImGui::DragInt(l, &value, 1, 0)` | `UI::Drag(l, *i, { .Min = 0 })` (via the signed view) |
| `Scalar` (bool) | `ImGui::Checkbox(l, p)` | `UI::Checkbox(l, *b)` |
| `Vector` | `DragFloat2/3/4` | one `UI::Drag` (above) |
| `Quaternion` | `ImGui::DragFloat3(euler, …)` | `UI::Drag(eulerLabel, euler, { .Speed = 0.5f })` |
| `String` | `ImGui::InputText(l, buf, …)` + `IsItemDeactivatedAfterEdit` | `UI::InputText(l, str)` (returns committed) |
| `AssetHandle` | `DrawAssetPicker` (combo) | `DrawAssetPicker` on `UI::Combo`/`UI::Selectable` |
| `Reference` | `ImGui::LabelText(l, "Entity %u:%u", …)` | `UI::Label(l, fmt::format("Entity {}:{}", …))` |
| `Matrix` | `ImGui::TreeNodeEx` + `Text` rows + `TreePop` | `if (auto t = UI::TreeNode(l, TreeFlags::SpanAvailWidth))` + `UI::Text(fmt::format(...))` |
| `Enum` | `ImGui::LabelText(l, "%d", value)` | `UI::Label(l, fmt::format("{}", value))` |
| `Struct` | `ImGui::TreeNodeEx` + recurse + `TreePop` | `if (auto t = UI::TreeNode(l, TreeFlags::SpanAvailWidth))` + recurse |
| (wrapper) | `ImGui::PushID(l)` / `PopID()` | `auto id = UI::PushId(l);` |
| (tooltip) | `if (IsItemHovered()) SetTooltip("%s", tip)` | `UI::Tooltip(field.Tooltip)` |

`DrawAssetPicker` (`FieldWidget.cpp:40–82`) is the asset **picker** combo — it migrates onto
`UI::Combo`/`UI::Selectable` (or the `BeginCombo`/`Selectable`/`EndCombo` scope form via a
`UI::ComboBox` scope if the open-state matters; the simple `UI::Combo(label, index, span)`
suffices since the candidate list is enumerated up front). It stays a free function on
`Veng::UI` primitives — the editor-tier-but-stateless widget the design overview names.

## Decisions

1. **The `void*`-to-typed cast branch stays; only the draw call changes.** The reflection
   walker is erased over `FieldClass` + `TypeId`; `Veng::UI` does not remove the need to
   recover the concrete type before drawing (the overload is resolved at the C++ call site,
   not at runtime). The win is the *uniform call*, not erasing the dispatch — every leaf
   draws through one `Drag`/`Checkbox`/`Label`/`InputText` family with one options idiom.

2. **`DragOptions` carries the field's editor metadata.** `FieldDescriptor`'s `Min`/`Max`/
   `Step` (the optional editor metadata the serializer ignores) flow into
   `DragOptions{ .Speed = Step, .Min = Min, .Max = Max }` — the single place clamp/speed are
   applied, replacing the per-arm `rangeFlag`/`minV`/`maxV` plumbing. Absent metadata →
   default `DragOptions` (the `0.01f` speed, unclamped).

3. **Tooltip via `UI::Tooltip`.** The `IsItemHovered() + SetTooltip("%s", tip)` pair at the
   arm tail (`FieldWidget.cpp:224`) becomes one `UI::Tooltip(field.Tooltip)` call (no-op on
   empty), guarded by the same "drawn an item this arm" position. The `%s`-printf is gone
   (the tip is already a `string`).

4. **No behavior change to the inspector.** Drag speeds, clamp ranges, tree-node framing,
   the Euler-from-quaternion edit, and the asset-picker write-back (`ApplyAssetPick`) are
   preserved — the migration is a call-surface change, not an inspector redesign. The
   String arm's commit-on-deactivate semantics live **inside** `UI::InputText` (plan 00
   decision 5): the arm becomes `if (UI::InputText(label, *str)) { … }` and writes back when
   the call returns "committed", replacing the hand-written `EnterReturnsTrue` +
   `IsItemDeactivatedAfterEdit` pair — so the arm no longer calls `UI::ItemEdited()` itself.
   The node-property inspector and entity inspector keep sharing `DrawFieldWidget`, so both
   move together.

## Files

| File | Change |
|---|---|
| `editor/src/FieldWidget.cpp` | Rewrite `DrawFieldWidget` + `DrawAssetPicker` on `Veng::UI`; `#include <Veng/UI/UI.h>`; drop raw `ImGui::`. |
| `editor/src/FieldWidget.h` | Unchanged signatures (the `FieldWidgetContext` surface is stable); no imgui in the header today, none added. |

`FieldWidget.h` already names no imgui type — only reflection/asset types — so the public
editor-src surface is unchanged.

## Verification

- Clean build — `libveng_editor` compiles the rewritten inspector against `Veng::UI`.
- `grep "ImGui::" editor/src/FieldWidget.cpp` returns nothing — the showcase file is fully
  migrated.
- The editor exe builds and runs: select an entity, confirm every `FieldClass` draws and
  edits as before — a `vec3` `Transform.Position` drags, a `Quaternion` edits via Euler, a
  `String` `Name` commits on deactivate, an `AssetHandle` `MeshRenderer.Mesh` opens the
  picker combo and re-binds, a nested `Struct` and a `Matrix` expand as tree nodes. The
  node-property inspector (material editor) draws node properties identically (shared
  helper).
- `ctest` green; no `gpu`/golden change (the inspector is editor-only, off the smoke path).
- `include_hygiene` unaffected.
