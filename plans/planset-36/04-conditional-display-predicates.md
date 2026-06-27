# Plan 04 — conditional display (`VisibleIf` / `EnabledIf`)

**Goal:** let a field show, hide, or grey itself out **by context** — a spot light's cone params only
when the light is a spot, a shadow knob only when shadows are on. The mechanism is a **type-erased
predicate over the owning struct** (`function<bool(const void*)>`), the conditional twins of the static
`Hidden` / `ReadOnly` flags `FieldDescriptor` already carries
([`FieldDescriptor.h:39`](../../engine/include/Veng/Reflection/FieldDescriptor.h)). A failing `VisibleIf`
skips the row; a failing `EnabledIf` wraps it in `UI::Disabled`.

**Depends on 00** (the `FieldDescriptor` extension lands beside the cascade) **and on 03** — it gates
fields inside the shared `DrawFields(base, fields, ctx)` walk helper that Plan 03 factors across the five
field-walk sites, so it threads the predicate's owner pointer through *that* helper rather than
re-plumbing each loop. **Sequenced after 01** so a condition can match an enumerator by **name**
(`self.Type == LightType::Spot`) instead of a magic ordinal. Touches
[`FieldWidget.cpp`](../../editor/src/FieldWidget.cpp) (merge last),
[`InspectorPanel.cpp`](../../editor/src/panels/InspectorPanel.cpp), and the other panel walks.

## Why it is its own plan

The mechanism is small but it threads a new value — the **owning struct's base pointer** — through every
place the inspector iterates a struct's fields, and that is the real work. Because Plan 03 has already
funnelled all five field-walk loops through one `DrawFields(base, fields, ctx)` helper, this plan threads
the owner pointer **on `FieldWidgetContext`** (which `DrawFields` already passes through), *not* as a new
`DrawFieldWidget` parameter — so the five `DrawFieldWidget` call signatures are untouched and the predicate
is evaluated in `DrawFields`, where the enclosing struct's base is in hand. Doing this last, after Plan 03
reshaped the walk, is what avoids re-plumbing each loop twice. It is also the one plan that adds a
*function* member to `FieldDescriptor`, distinct from the pure-data cascade.

## What lands

- **`FieldPredicate` + the two condition fields.** `using FieldPredicate = function<bool(const void*
  ownerBase)>;` in the reflection headers, and on `FieldDescriptor`:
  ```cpp
  FieldPredicate VisibleIf;   // empty == always visible
  FieldPredicate EnabledIf;   // empty == always enabled
  ```
  An empty `function` means "unconditional," mirroring the nullable array shims — no `optional` wrapper
  needed. `<functional>` is std, so `include_hygiene` is preserved. These are **flat on
  `FieldDescriptor`, not in the `FieldDisplay` cascade**: a predicate references the owning struct's
  siblings, which only has meaning at the usage site, so it has no type-default form.

- **`VE_WHEN` authoring sugar.** Inside a `VE_REFLECT` block the `Owner` alias is in scope, so the macro
  captures a typed lambda over the owning instance and type-erases it:
  ```cpp
  VE_FIELD(InnerCone, .DisplayName = "Inner Cone",
      .VisibleIf = VE_WHEN(self.Type == ::Game::LightType::Spot))
  VE_FIELD(ShadowBias, .DisplayName = "Shadow Bias",
      .EnabledIf = VE_WHEN(self.CastsShadows))
  ```
  `VE_WHEN(expr)` expands to a `FieldPredicate` wrapping `[](const Owner& self){ return (expr); }`
  adapted from `const void*` (`[](const void* p){ return pred(*static_cast<const Owner*>(p)); }`). It is
  **single-argument** (`#define VE_WHEN(expr)`, *not* variadic) so the captured expression is reproduced
  verbatim — which is exactly why a top-level comma in the expression (e.g. a comma operator, or a
  multi-arg call at paren-depth zero) would be misread by the preprocessor as a macro-argument separator.
  For that rare case, the companion `VE_WHEN_FN(lambda)` takes a full `[](const Owner& self){ … }` whose
  body the macro forwards as one brace-protected argument.

- **Evaluation in the shared walk.** In `DrawFields` (the helper Plan 03 factored), each field is gated
  **before** drawing: `VisibleIf && !VisibleIf(ownerBase)` → skip the row entirely;
  `EnabledIf && !EnabledIf(ownerBase)` → push a `UI::Disabled` scope around the row (composing with the
  existing `ReadOnly` disable).

- **`ownerBase` is always the *immediately-enclosing* reflected struct's base — not a fixed top-level
  pointer.** `VE_WHEN(expr)` captures `Owner` = the type whose `VE_REFLECT` block the field sits in, so the
  predicate does `*static_cast<const Owner*>(ownerBase)`; handing it any other object's base is undefined
  behavior. The contract, per walk level:
  - **Top-level component field** → the component's base pointer. The four panel walks
    (`InspectorPanel::DrawComponent` and the `MaterialEditorPanel`/`ProjectSettingsPanel`/`LevelEditorPanel`
    walks) seed `ownerBase` with the struct/component they iterate — set on `FieldWidgetContext` at the
    call site, so no `DrawFieldWidget` signature changes.
  - **Nested struct field** → the *nested* struct's base (`base + field.Offset` at that recursion level),
    which `DrawStructFields` already computes — **not** the top-level component. The recursion re-seeds
    `ownerBase` each time it descends, so a predicate on a nested field reads *its* siblings.
  - **Array element field** → the *element's* base. A predicate on an array-element struct field therefore
    sees only its own element's siblings, not the array's parent — a deliberate, stated limit.

- **A pure, device-free evaluation test.** The skip/disable *decision* is factored as a tiny pure helper
  (`bool IsFieldVisible(const FieldDescriptor&, const void* ownerBase)` /
  `IsFieldEnabled(...)`) so it can be unit-tested without ImGui: build a struct value, author
  `VisibleIf`/`EnabledIf` predicates, and assert the decision flips with the owning value. **The test
  includes a nested case** — a struct whose *sub*-struct's field carries a predicate over the sub-struct's
  siblings — asserting the predicate is evaluated against the nested instance's base, not the outer one
  (the re-basing that, done wrong, is silent UB). The ImGui consumption (skip vs `Disabled`) is covered by
  the editor build + smoke.

## Decisions

1. **A predicate, not a declarative rule.** `function<bool(const void*)>` over the owning struct is
   maximally expressive (compound conditions, ranges, anything), subsumes the `{field, value}` case
   (`self.X == V` is just an expression), and fits the module model — `FieldDescriptor` already carries
   type-erased payloads across the dlopen boundary (the existing raw array shims, plus `string`/`vector`),
   safe because the module model is rich C++ from one tree, not a binary-plugin platform. A
   `std::function` is heavier than those raw function-pointer shims (heap-allocating, STL-layout-dependent),
   so the boundary safety rests on the one-tree rebuild rule (see Decision 6), not on the predicate being
   ABI-trivial.
2. **`VisibleIf` and `EnabledIf` are the conditional twins of `Hidden`/`ReadOnly`.** Two effects, the
   same predicate machinery — `VisibleIf` is a data-driven `Hidden` (remove clutter when irrelevant),
   `EnabledIf` a data-driven `ReadOnly` (show-but-lock when relevant-but-not-editable). Both earn their
   place; shipping only one would leave the symmetry half-built.
3. **They are flat on `FieldDescriptor`, not in the cascade.** A condition names the owning struct's
   siblings — a usage-site fact with no type-default meaning — so it sits beside `Hidden`/`ReadOnly`, not
   in `FieldDisplay`.
4. **`EnabledIf` composes with `ReadOnly`.** A field is editable only when *both* allow it; the disable
   scope is the union, so an always-`ReadOnly` field stays read-only regardless of its condition.
5. **The predicate is opaque — accepted tradeoff.** It can be run but not introspected, serialized, or
   rendered as text, and cannot be authored from pure data. For code-authored component fields that is
   fine; a declarative, data-authored condition (for JSON/node-editor property structs) is a separate
   future mechanism layered beside the predicate, not a replacement for it.
6. **A predicate's code lives in the registering module, so the registry shares the module's lifetime.**
   The host-owned `TypeRegistry` stores `FieldDescriptor`s whose `VisibleIf`/`EnabledIf` `std::function`
   targets (and the existing raw array shims) are *code in the game module's image*. This plan adds
   **per-frame invocation** of that module-owned code from the inspector, so the standing invariant must be
   stated and held: **the host never `dlclose`s a module while its descriptors remain in the registry**
   (it clears the registry first). veng has no game-code hot-reload (a play session restarts), so this
   holds today; the `FieldPredicate` doc comment records it so a future reload path cannot quietly break
   it. A `FieldPredicate` doc comment also notes it is invoked only on the render thread.
7. **Sequenced after Plan 01 for readable conditions.** A name-based enum comparison
   (`self.Type == LightType::Spot`) reads far better than an ordinal; the dependency is for authoring
   clarity, not code.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Reflection/FieldDescriptor.h` | Add `FieldPredicate VisibleIf; FieldPredicate EnabledIf;` and the `FieldPredicate` alias, whose `@brief` records the lifetime + render-thread-only contract (Decision 6). |
| `engine/include/Veng/Reflection/Reflect.h` | The `VE_WHEN(expr)` + `VE_WHEN_FN(lambda)` macros (capture `Owner`, type-erase to `FieldPredicate`). |
| `editor/src/FieldWidget.cpp` | Factor `IsFieldVisible`/`IsFieldEnabled`; gate each field inside `DrawFields` (skip vs `UI::Disabled`); read `ownerBase` off `FieldWidgetContext` (no `DrawFieldWidget` signature change). |
| `editor/src/panels/InspectorPanel.cpp`, `MaterialEditorPanel.cpp`, `ProjectSettingsPanel.cpp`, `LevelEditorPanel.cpp` | Each walk seeds `FieldWidgetContext`'s `ownerBase` with the struct/component it iterates. |
| `engine/include/Veng/Scene/Components.h` (+ example) | A real first use: gate the spot-only `Light` cone fields on `Type == Spot` (demonstration + dogfood). |
| `tests/unit/…` | A device-free `IsFieldVisible`/`IsFieldEnabled` decision test driven by authored predicates, including a nested-struct re-basing case. |

## Verification

- Clean build; `ctest` green — the new predicate-decision test passes (`unit`, no ICD); the editor
  `gpu`-band smoke launches and exits 0.
- `include_hygiene` unaffected — `function` is std; no backend/editor include enters a public header.
- Manual editor check: selecting a non-spot `Light` hides the inner/outer cone fields (`VisibleIf`);
  a field authored `EnabledIf` greys out when its condition is false and composes with `ReadOnly`.
- `smoke_golden` does **not** move — inspector-only change; the reflection serializer ignores the
  predicates exactly as it ignores the rest of the editor metadata, so no cooked artifact moves.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
