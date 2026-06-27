# planset-36 — reflection display options for the editor

**Phase goal:** give the reflection layer a structured way to say **how a reflected value should
appear and behave in the editor**, and teach the inspector to honor it. Today
`FieldClass` ([`engine/include/Veng/Reflection/ReflectionTypes.h`](../../engine/include/Veng/Reflection/ReflectionTypes.h))
decides *both* a field's data shape *and* its widget, the inspector flattens every nested struct and
array element into an unscrollable wall of indented rows, enums fall back to a raw integer drag, and
there is no way to vary a widget, group fields, or show/hide one by context without writing a custom
`RegisterFieldWidget` per type. This planset adds the missing **presentation axis** — orthogonal to
`FieldClass`, validated against it — and the inspector machinery that reads it.

This is an **editor-and-reflection** planset. It touches the reflection public headers
(`Veng/Reflection/`) and the editor's shared field-widget helper
([`editor/src/FieldWidget.cpp`](../../editor/src/FieldWidget.cpp)). Mind the **two tiers** of that helper:
the **leaf** path (`DrawFieldWidget`, drawing one field) is genuinely shared — the entity inspector, the
node-property inspector, the project-settings panel, and the level editor all call it — so a leaf-level
win (Plans 01, 02) lands in every surface at once. The **walk** path (iterating a struct's fields) is
*not* a single function today: **five** caller loops drive it (`DrawStructFields` plus
`InspectorPanel`/`MaterialEditorPanel`/`ProjectSettingsPanel`/`LevelEditorPanel`), so the walk-level
features (Plans 03, 04) land a shared `DrawFields` helper those loops route through — see Plan 03. It
changes **no cooked format and no render path**: the presentation metadata is editor-only data the reflection
serializer already ignores ("the serializer ignores all editor metadata fields below",
[`FieldDescriptor.h`](../../engine/include/Veng/Reflection/FieldDescriptor.h)), so **`smoke_golden`
never moves across the whole planset**, and the cooker is untouched.

## The model — one presentation axis, three homes

The keystone is separating **what a value is** (`FieldClass`, the closed data shape the serializer
shares — unchanged) from **how it should be presented** (a new, optional presentation channel). Sorting
every display option by *"does this vary per usage?"* puts each in one of three homes:

```
  Identity / usage  →  FieldDescriptor (flat, as today)
    DisplayName, Tooltip, Category, Hidden, ReadOnly        — vary per field, no meaning on a type
    VisibleIf, EnabledIf                                    — predicates over the owning struct (Plan 04)

  Presentation      →  shared FieldDisplay, on BOTH TypeInfo and FieldDescriptor (a cascade)
    Widget kind, Min/Max/Step, Collapsible/DefaultOpen      — a sensible type default AND a field override

  Intrinsic         →  TypeInfo only
    enumerator {name, value} table                          — no per-field variation (Plan 01)
```

**The cascade is the heart of it.** A shared `FieldDisplay` struct lives on both `TypeInfo` (the
type's default) and `FieldDescriptor` (the field's override); the editor resolves each presentation
option **field-first, then type-default, then a hard default**. Define a `Color` (or a future `Angle`)
type once with `.Widget = Color` and every field of that type is a color picker for free — but a bare
`vec3` field can still say `.Display = {.Widget = Color}` without a wrapper type, and a per-field
`.Min`/`.Max` always wins. This matches veng's semantic-type grain (`CameraView` not bare `Camera`;
`CompressionRole` not a string) while keeping the bare-field escape, so no wrapper types are *forced*
before they earn their place.

The `Widget` kind is a **closed enum** (house style — exhaustive switch, like `FieldClass`); the
existing `RegisterFieldWidget(TypeId)` stays the escape hatch for anything the closed set cannot
express. Conditional display (`VisibleIf`/`EnabledIf`) is a **type-erased predicate**
(`function<bool(const void*)>` over the owning struct) rather than a declarative `{field, value}` —
the predicate subsumes the declarative case (`self.X == V` is just an expression), reaches compound
conditions a struct cannot, and fits veng's single-tree module model — `FieldDescriptor` already carries
type-erased payloads across the dlopen boundary (`string`, `vector`, the raw function-pointer array
shims), safe because the engine and every game module build from one tree against one STL. A
`std::function` is heavier than those raw shims (heap-allocating, STL-layout-dependent), so the boundary
safety rests on that one-tree rebuild rule, not on the predicate being ABI-trivial — see Plan 04.

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 00 | `FieldDisplay` data model + cascade | The presentation axis: a closed `WidgetKind` enum + a shared `FieldDisplay` struct on both `TypeInfo` and `FieldDescriptor`; `Min`/`Max`/`Step` move into it; a pure, device-free `ResolveFieldDisplay` cascade (field → type-default → hard default) with a unit test; a `VengDisplay<T>` trait + `VE_DISPLAY` companion macro for type defaults, read into `TypeInfo`. No new widget behavior — the editor routes today's behavior through the resolved values. Migrates the existing `.Min`/`.Max` authoring sites. Foundational. | done |
| 01 | Enumerator-name reflection | A `VE_ENUM`/`VE_ENUMERATOR` describe-block (mirroring `VE_REFLECT`/`VE_FIELD`) recording each enumerator's `{name, value}` onto `TypeInfo`; the generic `FieldClass::Enum` widget becomes an automatic named combo; the hand-written `LightType`/`CompressionRole`/`CompressionFormat` combos are retired. Migrates the engine builtin enums from `VE_LEAF(…, Enum)` to `VE_ENUM`. Device-free table test. Depends on 00 (shares the editor enum seam). | done |
| 02 | Widget-kind dispatch | `DrawFieldWidget` honors the resolved `Widget`: `Slider` vs `Drag` for scalars/vectors, `Color` → ColorEdit3/4 for vec3/vec4, `Multiline` for string. Adds the matching `Veng::UI` widgets where missing (`UI::Slider`, `UI::ColorEdit3/4`, `UI::InputTextMultiline`). `Auto` keeps today's per-`FieldClass` path, so it is backward-compatible. Depends on 00. | done |
| 03 | Collapsible structs, array elements, categories | One `CollapsingHeader`-inside-`PropertyTable` mechanism (a header row spanning both columns mid-table — the layout the current code avoids via nested tables) serving three consumers: nested structs as accordions (`.Collapsible`/`.DefaultOpen`), **array elements as foldable headers** with the Remove button moved into the header row, and `Category` grouping sibling fields under collapsible sections. Depends on 00. | done |
| 04 | Conditional display — `VisibleIf`/`EnabledIf` | A type-erased `FieldPredicate` (`function<bool(const void*)>`) over the owning struct on `FieldDescriptor`, the conditional twins of `Hidden`/`ReadOnly`; a `VE_WHEN(self…)` authoring sugar that captures the owning instance; `ownerBase` threaded through the inspector walk (`DrawStructFields` already has it; `DrawComponent`/the component walk needs it) so a failing condition skips the row (`VisibleIf`) or wraps it in `UI::Disabled` (`EnabledIf`). Reads best with 01's name-based enums. Depends on 00; benefits from 01. | proposed |

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = implemented, migrated, verified, committed.

## Dependencies

- **00** is foundational — the `FieldDisplay` axis, the cascade resolver, and the type-default
  authoring surface every later plan reads. Pure data + a device-free resolver test; no widget
  behavior change.
- **01**, **02**, **03**, **04** each build on **00**. They are *mostly independent in concept* but
  **all four touch [`editor/src/FieldWidget.cpp`](../../editor/src/FieldWidget.cpp)** (the per-`FieldClass`
  dispatch), so they **merge in number order** to avoid colliding in that one file — the same
  shared-file sequencing planset-35 used for the cooker entry. Beyond `FieldWidget.cpp`: 01 also touches
  the reflection headers + `InspectorPanel.cpp` (deleting the custom enum combos); 02 also touches
  `Veng/UI/` (new widgets); 03 also touches `Veng/UI/` (the table/header guards) **and factors the shared
  `DrawFields` walk helper the five field-walk loops route through**; 04 also touches `InspectorPanel.cpp`
  and the other panel walks (the predicate's owner base is threaded on `FieldWidgetContext`, not as a new
  `DrawFieldWidget` parameter).
- **04 also depends on 03**, not only 00: it gates fields inside the shared `DrawFields` walk helper that
  Plan 03 factors. The merge order (03 before 04) already guarantees the helper exists.
- **04** is sequenced after **01** so a condition can match an enumerator by *name*
  (`self.Type == LightType::Spot` reads far better than a magic ordinal) rather than because it needs
  01's code.

Dependent plans must build on the **prior plan's integration commit**, not `origin/main`. Per
[[project_megaexec_worktree_base]], `isolation: "worktree"` branches from `origin/main` and will not
see a locally-committed-but-unpushed base: only **00** (foundational) can use `isolation: "worktree"`
directly; **01 → 02 → 03 → 04** each dispatch against a manual worktree cut from the prior plan's
integration commit, because of the shared `FieldWidget.cpp`.

## The decisions this planset settles

- **Presentation is a second axis, orthogonal to `FieldClass`.** `FieldClass` stays the closed data
  shape the serializer shares; a new optional `FieldDisplay` decides the widget. An f32 stays `Scalar`
  but can be a slider; a vec3 stays `Vector` but can be a color picker. The serializer never reads it.
- **Presentation cascades: type default, field override.** A shared `FieldDisplay` on both `TypeInfo`
  and `FieldDescriptor`; the editor resolves field-first, then the type's default, then a hard default.
  Define `Color` once and style every field of it; override any field; per-field `Min`/`Max` always
  wins.
- **`WidgetKind` is a closed enum; `RegisterFieldWidget` is the escape hatch.** The engine owns the
  widget vocabulary (exhaustive switch, house style); a game needing something novel registers a custom
  widget by `TypeId` as it does today.
- **Identity metadata stays per-field and flat.** `DisplayName`/`Tooltip`/`Category`/`Hidden`/`ReadOnly`
  — and the conditional `VisibleIf`/`EnabledIf` — reference a usage site (and, for the predicates, the
  owning struct's siblings), so they have no meaning on a type and stay on `FieldDescriptor`.
- **Enumerator names are intrinsic to the type.** A `{name, value}` table on `TypeInfo`, authored once
  with `VE_ENUM`, drives an automatic named combo — no per-enum editor code. (The cooker may later read
  the same table for JSON enum-by-name; that is a free follow-on, out of scope here.)
- **A nested struct, an array element, and a category collapse by one mechanism.** A
  `CollapsingHeader` inside the two-column `PropertyTable`, driven by the same `FieldDisplay` cascade —
  so the long-array problem and the chunky-struct problem are one fix.
- **Conditional display is a predicate, not a declarative rule.** A type-erased
  `function<bool(const void*)>` over the owning struct — maximally expressive, matching the type-erased
  payloads `FieldDescriptor` already carries across the dlopen boundary (safe under veng's one-tree,
  one-STL module model), and authored with a typed `VE_WHEN(self…)` sugar.
  The tradeoff (a predicate is opaque — it can be run but not introspected, serialized, or rendered as
  text, and cannot be authored from pure data) is acceptable for code-authored component fields; a
  data-authored condition, if ever needed, is a separate future addition.

## What remains future

- **A data-authored / introspectable condition** — the predicate is code-only by design. A declarative,
  serializable condition (for JSON-authored or node-editor property structs, or for an editor that
  renders "visible when Type == Spot" as text) is a separate mechanism layered beside the predicate when
  a real consumer needs it.
- **Cooker enum-by-name** — Plan 01's `{name, value}` table lives on `TypeInfo`, so the cooker *could*
  accept enum field values by name in prefab/level JSON. That is a free follow-on, deliberately out of
  scope; the cook path is untouched this planset.
- **Semantic value types** (`Color`, `Angle`, `Normalized`) — the cascade *supports* a type default, and
  Plan 00 ships the `VE_DISPLAY` authoring surface, but this planset introduces no such wrapper type
  (light color stays a bare `vec3`). Introducing them — with the serialization/shader ripple they carry
  — is its own scoped pass, now unblocked.
- **Richer widgets** — `FilePath`, curve/gradient editors, a min/max range slider, and an enum *flags*
  (multi-select bitmask) widget are named extensions behind the delivered `WidgetKind` enum.
