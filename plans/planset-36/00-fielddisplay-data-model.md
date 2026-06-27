# Plan 00 — the `FieldDisplay` data model + cascade

**Goal:** stand up the **presentation axis** — a closed `WidgetKind` enum and a shared `FieldDisplay`
struct that lives on **both** `TypeInfo` (a type's default) and `FieldDescriptor` (a field's override) —
plus the pure, device-free **`ResolveFieldDisplay`** cascade that merges them (field → type-default →
hard default), and the **`VengDisplay<T>` trait + `VE_DISPLAY` macro** that author a type default. This
is the foundation every later plan reads. It ships **no new widget behavior**: the editor is rewired to
route *today's* behavior through the resolved `FieldDisplay`, so the inspector looks identical after this
plan. **Foundational; nothing precedes it.**

## Why it is its own plan

The whole planset hangs off one struct and one resolver. `WidgetKind`/`FieldDisplay` are consumed by
the widget-kind dispatch (Plan 02), the collapsible machinery (Plan 03 reads `Collapsible`/`DefaultOpen`
+ `Category`), and — through the same cascade slot — the enum combo (Plan 01). The cascade must be
settled and tested in isolation before any consumer binds to it. Proving the **merge** device-free here
is the same foundation-first move as planset-20's `AABB` and planset-21's `Frustum`: a pure function
with a unit test, no editor or GPU in the loop. Routing the existing behavior through the resolver in
this plan (with no visible change) is what keeps Plans 01–04 small — each becomes "read one more resolved
field."

## What lands

- **`WidgetKind` — the closed presentation enum.** `Auto`, `Drag`, `Slider`, `Color`, `Multiline` (a
  `u8` enum beside `FieldClass` in the reflection headers). **`Auto` is the default and means "infer from
  `FieldClass`"** — exactly today's per-class path — so an unstyled field is unchanged. The concrete
  kinds are *honored* by Plan 02; this plan only defines the enum and carries it through the cascade.
  Adding a kind is a deliberate enum change (house style); `RegisterFieldWidget(TypeId)` remains the
  escape hatch for anything outside the set.

- **`FieldDisplay` — the shared presentation struct.** A small, glm/fmt/std-only value type holding the
  **cascade-able** presentation options, every member representable as *unset* so the merge can fall
  through:
  - `WidgetKind Widget = WidgetKind::Auto;` (`Auto` = unset)
  - `optional<f64> Min, Max, Step;` (**moved here** from `FieldDescriptor`)
  - `optional<bool> Collapsible, DefaultOpen;` (consumed by Plan 03; `optional` so a field can inherit a
    type default rather than only override-to-true)

  It is `default`-constructible to all-unset, copyable, and has no function members (the conditional
  predicates are flat on `FieldDescriptor`, Plan 04 — not in the cascade).

- **`FieldDisplay` on `FieldDescriptor` and `TypeInfo`.** `FieldDescriptor` gains `FieldDisplay Display;`
  (the per-field override); `TypeInfo` gains `FieldDisplay Display;` (the per-type default). `Min`/`Max`/
  `Step` are **removed from `FieldDescriptor`** and live only inside `Display` now. The serializer still
  ignores all of it (the existing "editor metadata the serializer never touches" rule extends to
  `Display`).

- **`ResolveFieldDisplay` — the pure cascade.** A free function in the reflection layer:
  `FieldDisplay ResolveFieldDisplay(const FieldDescriptor& field, const TypeRegistry& registry)`. It
  starts from the field's type default (`registry.IsRegistered(field.Type) ? Info(field.Type).Display :
  {}`), then overlays each **set** member of `field.Display` (a non-`Auto` `Widget`, a present
  `optional`). The result is the fully-resolved `FieldDisplay` the editor consumes — concrete `Widget`
  (still possibly `Auto`, meaning "use the `FieldClass` default"), and `optional`s the consumer reads via
  `value_or`. Pure, device-free, no ImGui — **unit-testable in the `unit` band**.

- **`VengDisplay<T>` trait + `VE_DISPLAY` macro — authoring a type default.** A primary template
  `template <class T> struct VengDisplay { static FieldDisplay Get() { return {}; } };` defaulting to
  all-unset, specialized per type by a companion macro
  `VE_DISPLAY(::Veng::Foo, .Widget = WidgetKind::Color, …)`. `TypeRegistry::Register<T>()` reads
  `VengDisplay<T>::Get()` into `TypeInfo::Display` at registration. The macro is a **separate
  specialization point** from `VengReflect<T>`, so it composes uniformly with `VE_LEAF`/`VE_TYPE`/
  `VE_REFLECT`/`VE_ENUM` without touching them and without a positional-argument collision. No engine
  builtin type *needs* a non-trivial default yet (light color stays a bare `vec3`); the mechanism ships
  proven, the first real use arrives with a semantic type later.

- **`VE_FIELD` carries `.Display`.** `VE_FIELD(Member, …)` already forwards designated initializers into
  the `FieldDescriptor` aggregate, so `.Display = {.Min = 0.001}` works the moment `FieldDescriptor` has a
  `Display` member — no macro change. The doc comment in
  [`Reflect.h`](../../engine/include/Veng/Reflection/Reflect.h) is updated to show the new spelling.

- **A device-free cascade test.** A `unit` test builds a `TypeRegistry`, registers a type carrying a
  `VengDisplay` default (`.Widget = Slider`, `.Min = 0`, `.Step = 0.1`, `.Collapsible = true`), and asserts
  `ResolveFieldDisplay`: a field with no override inherits the type default; a field overriding `.Max`
  keeps the inherited `.Min`/`.Step`/`.Widget`; a field overriding `.Widget` wins; a field overriding
  `.Collapsible = false` overrides an inherited `true` (the optional-`bool` inherit-vs-override case Plan 03
  binds to); an unregistered field type falls to the hard default. Plus a trivial "all-unset `FieldDisplay`
  resolves to `Auto`/empty" case. `Step` and `Collapsible`/`DefaultOpen` are covered here because Plans 02
  and 03 are the consumers that read them off the resolved value.

## The migration

`Min`/`Max`/`Step` move from flat `FieldDescriptor` fields into `FieldDisplay`, so every authoring site
that wrote `.Min`/`.Max`/`.Step` directly changes from
`VE_FIELD(Scale, .DisplayName = "Scale", .Min = 0.001)` to
`VE_FIELD(Scale, .DisplayName = "Scale", .Display = {.Min = 0.001})`. `DisplayName`/`Tooltip`/`Category`/
`Hidden`/`ReadOnly` stay flat (identity metadata). This is **mechanical** — ~39 `VE_FIELD` sites:
**37** in [`engine/include/Veng/Scene/Components.h`](../../engine/include/Veng/Scene/Components.h)
(primitive shapes, lights, motion, session, render settings), **2** in
[`engine/include/Veng/Scene/Camera.h`](../../engine/include/Veng/Scene/Camera.h) (`FovY`/`Near`), and
**2** in [`examples/hello-triangle/main.cpp`](../../examples/hello-triangle/main.cpp). `examples/template`
has **no** such site, but is rebuilt under the co-migration rule regardless. The multi-key sites — the
spot-cone fields and `Range`, which today pass `.Min`/`.Max`/`.Step` as *three separate* `VE_FIELD`
macro arguments — collapse into one braced `.Display = {.Min = …, .Max = …, .Step = …}` argument; that
brace-wrapping is the only non-trivial part of the otherwise rote edit. The sole editor read site
(`field.Min`/`Max`/`Step` → `resolved` in [`FieldWidget.cpp`](../../editor/src/FieldWidget.cpp), the
`DragOptions` assembly in the scalar/vector path) moves to the resolver in the same pass; a tree-wide grep
confirms no other code reads `FieldDescriptor::Min`/`Max`/`Step` (the `.Min`/`.Max` hits in `cooker/` and
`tests/` are all `AABB`/box bounds, unrelated).

## Decisions

1. **`WidgetKind` is closed; `Auto` is the default and means "today's behavior."** An unstyled field
   resolves to `Auto` and renders exactly as it does now, so this plan is behavior-preserving. Concrete
   kinds are honored in Plan 02.
2. **`FieldDisplay` members are all *unset*-representable.** `Widget` uses an `Auto` sentinel; everything
   else is `optional`. This is what makes the cascade a genuine merge (a field overrides one option
   without clobbering the rest) and what lets a `bool` like `Collapsible` *inherit* a type default rather
   than only flip to true. **One asymmetry to accept:** because `Auto` is *both* "unset/inherit" *and*
   "infer from `FieldClass`," a field cannot override a type's `.Widget = Slider` default back to the
   bare `FieldClass` widget — setting `Auto` reads as inherit, yielding `Slider`. This is inert today (no
   builtin sets a `Widget` type default — see Decision 4), so it ships documented rather than fixed; if a
   type default ever needs a per-field "reset to class widget," `Widget` becomes `optional<WidgetKind>`
   (`nullopt` = inherit, `Auto` = infer) at that point, consistent with the rest of the struct.
3. **The cascade resolver lives in `libveng` and is pure.** `ResolveFieldDisplay` is a free function over
   `FieldDescriptor` + `TypeRegistry`, no ImGui, device-free — so it is unit-tested here and reused
   unchanged by both inspector surfaces. The editor never re-implements the merge.
4. **Type defaults are authored through a separate `VengDisplay<T>`/`VE_DISPLAY` point.** Keeping it off
   `VengReflect<T>` means the existing reflection macros are untouched and it composes with every type
   kind. **The type-default arm of the cascade ships with no in-tree consumer** — every win in Plans
   01–04 is field-authored, and no builtin sets a `Widget`/`Min`/`Collapsible` type default yet. This is
   deliberate foundation-laying: the `VengDisplay<T>` trait, the `TypeInfo::Display` slot, and the
   type-default overlay are validated only by the cascade unit test until a semantic value type (`Color`,
   `Angle`) first uses them in a later planset. It is the one piece a reviewer could defer; it is kept
   here so that later planset is "add a `VE_DISPLAY` line," not "build the cascade."
5. **`Min`/`Max`/`Step` move into `FieldDisplay`.** They are presentation, and the cascade's value is
   precisely that a type can default its range (a future `Angle` defaults `0…2π`) while a field overrides
   it. Leaving them flat would split presentation across two homes. The migration is mechanical.
6. **No serializer, cooker, or render change.** `Display` is editor metadata the reflection serializer
   already skips; the cooked format, the cooker, and every render path are untouched.
7. **`FieldDescriptor` layout is an implicit cross-module ABI surface.** `VE_FIELD` builds the
   `FieldDescriptor` aggregate *in the game module*; the engine reads it *by member offset*. Removing flat
   `Min`/`Max`/`Step` and adding `FieldDisplay Display` shifts that layout, and `VENG_MODULE_ABI_VERSION`
   guards only `VengModuleHost`/the entry contract — it would **not** reject a stale module built against
   the old layout, which would then read `Display` at the wrong offset (silent corruption). This is
   contained by veng's one-tree, full-rebuild norm (the same norm that lets `string`/`vector` cross the
   boundary at all), which the planset's co-migration + per-plan rebuild already enforce. **Bump
   `VENG_MODULE_ABI_VERSION` in this plan** as belt-and-suspenders: it converts a partial-rebuild mismatch
   from silent corruption into the loud load-time reject the project prefers.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Reflection/ReflectionTypes.h` | Add the closed `WidgetKind` enum (documented Doxygen enumerators). |
| `engine/include/Veng/Reflection/FieldDisplay.h` (new) | The `FieldDisplay` struct + the `ResolveFieldDisplay` declaration; the `VengDisplay<T>` primary template + `VE_DISPLAY` macro. |
| `engine/include/Veng/Reflection/FieldDescriptor.h` | Remove flat `Min`/`Max`/`Step`; add `FieldDisplay Display;`. |
| `engine/include/Veng/Reflection/TypeRegistry.h` | Add `FieldDisplay Display;` to `TypeInfo`; `Register<T>()` reads `VengDisplay<T>::Get()` into it. |
| `engine/include/Veng/Reflection/Reflect.h` | Update the `VE_FIELD` doc comment to the `.Display = {…}` spelling. |
| `engine/src/Reflection/FieldDisplay.cpp` (new) | `ResolveFieldDisplay` definition (the field-over-type merge). |
| `engine/include/Veng/Scene/Components.h`, `Camera.h` | Migrate ~40 `.Min`/`.Max`/`.Step` sites into `.Display = {…}`. |
| `examples/hello-triangle/…`, `examples/template/…` | Migrate any `.Min`/`.Max`/`.Step` authoring sites (co-migration rule). |
| `editor/src/FieldWidget.cpp` | Read `ResolveFieldDisplay` once per field; route the existing drag `Min`/`Max`/`Step` off the resolved value (no behavior change). |
| `engine/CMakeLists.txt` | Add the new `Reflection/FieldDisplay.cpp` source. |
| `engine/include/Veng/Module/Module.h` | Bump `VENG_MODULE_ABI_VERSION` (`3u` → `4u`) — `FieldDescriptor` layout changed (see Decision 7). |
| `tests/unit/…` | A device-free `ResolveFieldDisplay` cascade test (field/type/default merge cases, incl. `Step` and `Collapsible`/`DefaultOpen` inherit-vs-override). |

## Verification

- Clean build; `ctest` green — the new cascade test passes (`unit`, no ICD); the existing reflection /
  prefab round-trip tests still pass (the serializer ignores `Display`, so the cooked format is byte-for-
  byte unchanged).
- `include_hygiene` unaffected — the new public header is over `Veng.h` + the reflection headers only
  (`optional`/`f64`/`bool` and the `WidgetKind` enum), no backend/editor include.
- The editor builds and the inspector is **visually identical** — every field resolves to `Auto` and the
  same `Min`/`Max`/`Step`, so no widget changes yet.
- `smoke_golden` does **not** move — no cook or render path changes.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
