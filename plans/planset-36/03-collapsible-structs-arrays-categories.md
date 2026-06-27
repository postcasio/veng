# Plan 03 — collapsible structs, array elements, and categories

**Goal:** give the inspector a way to **fold** — so a chunky nested struct, a long array, and a
many-field component stop being an unscrollable wall of indented rows. One
`CollapsingHeader`-inside-`PropertyTable` mechanism serves three consumers: nested structs as accordions,
**array elements as foldable headers**, and `Category` grouping sibling fields under collapsible
sections. This is the headline ergonomics win, and the array-element fold is the specific gap called out
in review (`DrawArray` flattens every element today —
[`FieldWidget.cpp:332`](../../editor/src/FieldWidget.cpp)).

**Depends on 00** (the `FieldDisplay.Collapsible`/`DefaultOpen` cascade members and `Category`). Touches
[`FieldWidget.cpp`](../../editor/src/FieldWidget.cpp) (merge after 02) and the
[`Veng/UI/`](../../engine/include/Veng/UI) table/header guards.

## Why it is its own plan

The hard part is **one piece of layout**: rendering a `CollapsingHeader` (a header spanning *both*
columns) inside the two-column `UI::PropertyTable` the inspector runs everything through. The current code
deliberately avoids this — "A nested struct flattens into further indented rows in the same table — never
a nested `BeginTable` inside a cell" ([`FieldWidget.cpp:411`](../../editor/src/FieldWidget.cpp)) — so the
header-in-table primitive has to be built and proven once. Once it exists, three consumers fall out of it,
which is exactly why they share a plan rather than three: structs, arrays, and categories are the *same*
fold applied at three call sites. Splitting them would duplicate the layout work.

## What lands

- **The header-in-`PropertyTable` primitive.** A `Veng::UI` helper that opens a row spanning the full
  table width and draws a `CollapsingHeader` into it (returning a `[[nodiscard]] bool` open-state +
  honoring an initial `DefaultOpen`), so a section header sits cleanly between two-column rows without a
  nested table. This is the one new `Veng::UI` capability; the three consumers below call it.

- **Collapsible nested structs.** The `FieldClass::Struct` arm of `DrawFieldWidget`
  ([`FieldWidget.cpp:413`](../../editor/src/FieldWidget.cpp)) reads the resolved `Collapsible`: when set,
  the struct draws as a full-width `CollapsingHeader` (labelled with the field's `DisplayName`, initial
  state from `DefaultOpen`, default open) whose body is the existing `DrawStructFields` recursion; when
  unset it keeps **today's** flat indented rows. So folding is opt-in per field (or per type, via the
  cascade) and the default layout is unchanged.

- **Foldable array elements.** `DrawArray` ([`FieldWidget.cpp:332`](../../editor/src/FieldWidget.cpp)) is
  reworked: when the array field resolves `Collapsible`, each element `[i]` becomes a full-width
  `CollapsingHeader` instead of a bare label row, and **the Remove button moves into the header row**
  (the Add button stays on the array's own row). Element bodies (`DrawStructFields` on the element) draw
  only when the header is open — so a 20-element array is 20 fold-headers, not 20× its fields. `DefaultOpen`
  controls the initial fold state (a long array authored `.DefaultOpen = false` opens collapsed).
  Reorder stays **out of scope** (the config-list order-independence rule is unchanged). When `Collapsible`
  is unset the array keeps today's flat per-element rows, so the change is opt-in.

  The element fold state is ImGui-owned, keyed on the existing positional per-element id
  (`PushId("{label}elem{i}")`), which has a known consequence: **removing element `[i]` slides every later
  element's fold state down one** (`[i+1]`'s open/closed becomes `[i]`'s). The elements are bare structs
  with no stable identity to key on, so positional is the only option; this is acceptable because the
  config list is order-independent (Decision 4) — it is documented here so the slide is not later filed as
  a bug. The Remove button living in the always-visible header keeps a collapsed element's removal reachable,
  and the deferred-`removeAt` discipline is unchanged by which element bodies were drawn.

- **Category grouping.** When a struct's (or component's) fields carry a `Category`
  ([`FieldDescriptor.h`](../../engine/include/Veng/Reflection/FieldDescriptor.h) — defined but never
  consumed), the field walk groups consecutive fields by category and draws each group under a full-width
  `CollapsingHeader` named for the category, with un-categorized fields rendered first (or under a default
  group). This is the first use of `Category`. Grouping is **stable** — fields keep their declared order
  within a category, and categories appear in first-seen order — so authoring is predictable.

- **One shared `DrawFields` walk helper, routed from all five field-walk sites.** Category grouping is a
  **walk-level** feature, and the walk is *not* one function today — `DrawFieldWidget` (the leaf) is
  shared, but the loops that iterate a struct's fields and call it are duplicated across **five** sites:
  the nested recursion in `DrawStructFields`, and four panel walks — `InspectorPanel::DrawComponent`
  ([`InspectorPanel.cpp`](../../editor/src/panels/InspectorPanel.cpp)),
  `MaterialEditorPanel` (the node-property walk over `type->Properties`),
  `ProjectSettingsPanel` (over `ProjectSettings::Fields()`), and `LevelEditorPanel`. This plan **factors a
  single `DrawFields(base, fields, ctx)` helper** holding the grouping logic (and, in Plan 04, the
  per-field predicate gate) and routes **all five** loops through it, so grouping lands uniformly in every
  inspector surface — the entity inspector, the node-property inspector, project settings, and the level
  editor — rather than only the two the leaf-sharing story would suggest. (If any surface is later judged
  to *not* want grouping, that is a one-line opt-out in its call, not a divergent walk.)

## Decisions

1. **One header-in-table primitive, three consumers.** Structs, array elements, and categories are the
   same `CollapsingHeader`-in-`PropertyTable` fold; building it once and calling it three times is the
   whole point of the plan.
2. **Folding is opt-in; the flat layout stays the default.** An un-`Collapsible` struct/array renders
   exactly as today, and an un-categorized component is unchanged. Nothing folds unless authored to —
   so existing components look identical until someone opts in.
3. **Array-element Remove moves into the header; Add stays on the array row.** The header row is the
   natural home for the per-element action, and it keeps a collapsed element's controls reachable without
   expanding it.
4. **Reorder is not added.** Only folding was asked for; the order-independence rule that already governs
   the array widget is unchanged. A drag-reorder is a separate future widget.
5. **Category order is first-seen, field order within a category is declared order.** Deterministic and
   predictable from the `VE_REFLECT` block; no alphabetical surprise.
6. **The fold state is ImGui-owned (per-widget-id), not stored in the model.** `CollapsingHeader` keeps
   its open/closed state in ImGui's storage keyed by the widget id (the same `PushId` discipline the
   inspector already uses), so the reflected data carries no UI state — `DefaultOpen` only seeds the
   initial frame.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/UI/UI.h` (+ src) | The full-width header-in-`PropertyTable` helper (imgui-free signature; out-of-line over `<imgui.h>`). |
| `editor/src/FieldWidget.cpp` | Factor the shared `DrawFields(base, fields, ctx)` walk helper (category grouping); collapsible arm in the `Struct` case; reworked `DrawArray` with foldable elements + header Remove. |
| `editor/src/panels/InspectorPanel.cpp` | Route `DrawComponent`'s field walk through `DrawFields`. |
| `editor/src/panels/MaterialEditorPanel.cpp` | Route the node-property walk (over `type->Properties`) through `DrawFields`. |
| `editor/src/panels/ProjectSettingsPanel.cpp` | Route the settings walk (over `ProjectSettings::Fields()`) through `DrawFields`. |
| `editor/src/panels/LevelEditorPanel.cpp` | Route its field walk through `DrawFields`. |
| `tests/…` | No new unit test (ImGui layout); covered by the editor build + `gpu`-band editor smoke. |

## Verification

- Clean build; `ctest` green — the editor `gpu`-band smoke launches and exits 0; no `unit`/`death`
  regressions.
- `include_hygiene` unaffected — the new `Veng::UI` helper is imgui-free in signature.
- Manual editor check: a struct field tagged `.Display = {.Collapsible = true}` renders as an accordion;
  an array tagged `.Collapsible` shows fold-headers per element with Remove in the header and Add on the
  array row; a component whose fields carry `Category` groups them under collapsible sections;
  un-annotated structs/arrays/components are unchanged.
- `smoke_golden` does **not** move — inspector-only change.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
