# Plan 04 — Editor variant inspector widget

**Goal:** render and edit a `FieldClass::Variant` field in the editor inspector — a combo
to choose the active alternative (or none), and the active alternative's own fields drawn
as indented rows beneath it. Because the variant's active alternative *is* the primitive
kind (plan 07), this widget is what gives the editor primitive selection and per-kind
parameter editing. Depends on plans 01 + 02; independent of plan 03.

## Why this is its own plan

`DrawFieldWidget` (`editor/src/FieldWidget.cpp`) switches on `FieldClass` and is shared
by the entity inspector and the node-property inspector, so one new `case` here lights up
both. It is the only place a human picks a variant alternative, so the UX (combo + recurse
+ switch semantics) is worth isolating from the data-model and cook plans.

## The widget — `FieldWidget.cpp`

Add a `FieldClass::Variant` case to the dispatch (beside `Struct`, which already recurses
nested fields as flattened indented rows):

```cpp
case FieldClass::Variant:
    DrawVariant(fieldPtr, field, valueLabel, ctx);
    break;
```

`DrawVariant`:

1. Read the variant's `TypeInfo` via `ctx.Assets.GetTypeRegistry().Info(field.Type)` — the
   same accessor the existing `Struct` case uses (`FieldWidgetContext` has no `Registry`
   member; the registry is reached through `Assets`).
2. Build the combo item list: `"(none)"` plus one entry per `VariantAlternatives` id,
   labelled by `ctx.Assets.GetTypeRegistry().Info(altId).Name` (the display name; later a
   `DisplayName` override is possible but the type name is the honest default).
3. Current selection index = position of `VariantActiveType(fieldPtr)` in the list (0 for
   empty). On change:
   - index 0 → `info.VariantClear(fieldPtr)` (plan 01's op resets the variant to empty;
     `VariantSetActive(InvalidTypeId)` returns null and does **not** clear).
   - index N → `info.VariantSetActive(fieldPtr, alternatives[N-1])`, switching the active
     alternative to a **default-constructed** value (switching kind resets params, which
     is the expected editor behavior).
4. If an alternative is active, draw its fields: fetch `VariantActivePtr(fieldPtr)` and
   the active alternative's `TypeInfo.Fields`, then recurse `DrawFieldWidget` per nested
   field under an indent — the same loop the `Struct` case uses (factor a shared
   `DrawStructFields(void*, const TypeInfo&, ctx)` helper if the `Struct` case does not
   already expose one).

`DrawVariant` returns a `bool` ("changed") for its own composition (combo change or any
nested field change), which the `DrawFieldWidget` dispatch discards like every other case —
`DrawFieldWidget` returns `void`. Re-resolution after an edit does **not** ride a return
value: the prefab editor calls the idempotent `ResolvePrimitiveMeshes` each frame (plan 07),
so a shape or parameter change is picked up on the next frame with no changed-signal plumbing.

## Bumping the spatial version

The inspector edits components through `Scene::ForEachComponent`'s erased `void*`, which
already bumps the scene's spatial version (the editor inspector's edit path is one of the
documented bump triggers). Switching a variant alternative mutates the component in place
through that same pointer, so no extra version handling is needed — but confirm the edit
flows through the version-bumping accessor and not a cached pointer.

## Tests

The inspector is GPU/ImGui-driven and not unit-tested directly; coverage is manual + the
plan 08 sample. Add no automated test here. Manually verify in the editor (plan 08's
prefab) that:

- A `PrimitiveComponent`'s `Shape` shows a combo of the four shapes plus "(none)".
- Picking a shape reveals exactly that shape's parameters; switching shapes swaps the
  parameter set and resets to defaults.
- Editing a parameter re-resolves the mesh on the next frame (the prefab editor's per-frame
  idempotent `ResolvePrimitiveMeshes` call, plan 07, picks up the changed shape key).

## Acceptance

- Clean build of the editor exe.
- A variant field renders as a combo (alternatives + "(none)") with the active
  alternative's fields indented beneath; switching the combo activates a
  default-constructed alternative or clears to empty.
- The widget is exercised identically by the entity inspector and the node-property
  inspector (both call `DrawFieldWidget`).
- No `include_hygiene`/link regressions (the widget uses only the registry + existing
  `Veng::UI` combo/child scaffolding).
