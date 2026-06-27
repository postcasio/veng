# Plan 02 — widget-kind dispatch

**Goal:** make the inspector **honor the resolved `Widget` kind**. With Plan 00's cascade in place, a
field (or its type) can ask for a `Slider` instead of a `Drag`, a `Color` picker for a `vec3`/`vec4`, or
a `Multiline` text box for a string. This plan teaches `DrawFieldWidget` to switch on the resolved
`WidgetKind` and adds the matching `Veng::UI` widgets where the toolkit lacks them.

**Depends on 00** (the resolved `FieldDisplay.Widget`). Touches
[`FieldWidget.cpp`](../../editor/src/FieldWidget.cpp) (merge after 01) and adds widgets to
[`Veng/UI/`](../../engine/include/Veng/UI).

## Why it is its own plan

Plan 00 carries `WidgetKind` through the cascade but every field still resolves to `Auto`. This plan is
the consumption half: a focused change to the leaf-drawing switch in `DrawFieldWidget` plus the new
`Veng::UI` overloads it calls. It is separable from the structural work (Plan 03, collapsible) and the
conditional work (Plan 04) — those rewrite the *struct/array* path and the *visibility* path, while this
one rewrites the *leaf* path. Keeping it alone makes the new `Veng::UI` widgets a self-contained,
reviewable toolkit addition, and both inspector surfaces (entity + node-property) pick up the new widgets
for free since they share `DrawFieldWidget`.

## What lands

- **New `Veng::UI` widgets.** Added to the engine-tier vocabulary, imgui-free signatures (matching the
  `Veng::UI` contract — `[[nodiscard]] bool` "changed", designated-initializer options, no raw
  `ImGui*Flags`):
  - `UI::Slider` — the toolkit today has exactly two: `Slider(label, f32&, SliderOptions)` and
    `Slider(label, i32&, i32 min, i32 max)` (note the int form takes **bare** min/max, *not*
    `SliderOptions`). This plan **adds** the vector overloads `Slider(label, vec2&/vec3&/vec4&,
    SliderOptions)` and leaves the existing two as-is — so there is no ambiguous/duplicate `i32` overload
    and no signature break on a shipped widget. `SliderOptions::Min`/`Max` are `f32`, while the cascade's
    resolved `Min`/`Max` are `optional<f64>`; the dispatch narrows `f64 → f32` at the call site and
    handles the unset case as the no-range degrade below (a slider needs a range).
  - `UI::ColorEdit3(label, vec3&)` / `UI::ColorEdit4(label, vec4&)` — ImGui `ColorEdit` behind the
    engine surface (swatch + picker popup).
  - `UI::InputTextMultiline(label, string&, vec2 size = {})` — the multiline sibling of the existing
    `UI::InputText`.
  - The out-of-line ImGui calls live under [`engine/src/UI/`](../../engine/src/UI) as the rest do, so
    `include_hygiene` is preserved (`<imgui.h>` never reaches a public header).

- **`DrawFieldWidget` switches on the resolved `Widget`.** The per-`FieldClass` leaf `switch (field.Class)`
  in `DrawFieldWidget` gains a `WidgetKind` layer; the resolved `Min`/`Max`/`Step` it reads is the
  `DragOptions` assembly just above that switch (the block that today reads `field.Min`/`Max`/`Step`,
  now `resolved` after Plan 00). (Cite by name, not line: Plans 01–04 all rewrite `FieldWidget.cpp` and
  merge in number order, so any pre-planset line number has shifted by the time this plan lands.)
  - **Scalar / Vector** — `Auto` (or `Drag`) keeps today's `UI::Drag`; `Slider` routes to `UI::Slider`
    with the resolved `Min`/`Max` (a `Slider` resolved without a range logs once and degrades to a
    drag — a slider with no bounds is meaningless); `Color` routes a `vec3`→`ColorEdit3`,
    `vec4`→`ColorEdit4` (a `Color` on a non-3/4 type degrades to the drag with a one-time log).
  - **String** — `Auto`/unset keeps `UI::InputText`; `Multiline` routes to `UI::InputTextMultiline`.
  - Every other `FieldClass` ignores a non-`Auto` `Widget` (a `Color` on a quaternion is meaningless)
    and draws its normal widget — incompatible hints degrade gracefully, they never assert.

- **Compatibility validation is graceful + logged, not fatal.** An incompatible `Widget`/`FieldClass`
  pairing (Slider without a range, Color on a scalar, Multiline on a non-string) falls back to the
  natural widget and `Log::Warn`s **once per field descriptor**. The seen-set is keyed on
  `const FieldDescriptor*` — stable for the registry's lifetime (descriptors live in `TypeInfo::Fields`)
  and read on the single render thread, so no synchronization is needed; it is naturally bounded by the
  count of distinct mis-authored fields. If a registry is ever torn down and rebuilt mid-session, the set
  is cleared with it so a freed descriptor's pointer cannot be reused as a stale key. The editor is the
  only consumer and a mis-authored hint should not abort an editing session; the warning makes the
  mistake loud without being fatal.

- **The compatibility rule is a pure, unit-tested function.** The decision — *given a resolved
  `WidgetKind` and a `FieldClass` (+ the field's concrete type and whether a range is present), what
  widget actually draws* — is factored as a pure free function
  (`WidgetKind EffectiveWidget(WidgetKind requested, FieldClass cls, TypeId type, bool hasRange)`) that
  returns the effective kind (degrading `Slider`-without-range → `Drag`, `Color`-on-non-vec3/4 → `Drag`,
  `Multiline`-on-non-string → its class default). `DrawFieldWidget` calls it, then draws; the warning
  fires when the effective kind differs from the requested one. This makes the whole degrade matrix
  device-free and unit-testable without ImGui (the gap Plans 02/03 otherwise leave — see Verification).

## Decisions

1. **`Auto` is the universal default and means "today's widget."** The dispatch only diverges on an
   explicit kind, so unstyled fields are unchanged and the diff is contained to the new branches.
2. **Incompatible hints degrade gracefully with a one-time warning, never an assert.** Display metadata
   is editor-only; a bad hint is an authoring nuisance, not API misuse. (This is the one place the
   planset departs from the "loud assert on misuse" reflex — deliberately, because the metadata cannot
   break the program and an editing session must survive it.)
3. **New widgets land in `Veng::UI`, not raw `ImGui::` at the call site.** The inspector is authored
   against the engine surface like everything else (planset-17), so `ColorEdit`/`Slider`/multiline join
   the vocabulary rather than leaking ImGui into `FieldWidget.cpp`.
4. **A `Slider` reads the cascade's `Min`/`Max`.** This is the first concrete payoff of moving the range
   into `FieldDisplay` (Plan 00): the same `Min`/`Max` that bound a drag now bound a slider, and a type
   can default both.
5. **Color is a widget hint on a bare vector, not a new type — for now.** A field tags `vec3`/`vec4` as
   `Color` via the cascade (per-field or, once a `Color` type exists, via its `VE_DISPLAY` default). The
   planset does not introduce a `Color` type; this is the bare-field escape the cascade was designed to
   keep.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/UI/UI.h` (+ siblings) | Declare `UI::Slider` overloads, `UI::ColorEdit3/4`, `UI::InputTextMultiline` (imgui-free signatures). |
| `engine/src/UI/…` | Define them out-of-line over `<imgui.h>`. |
| `editor/src/FieldWidget.cpp` | Factor the pure `EffectiveWidget` decision; add the `WidgetKind` layer to the Scalar/Vector/String leaf cases; the one-time incompatible-hint warning keyed on `const FieldDescriptor*`. |
| `tests/unit/…` | A device-free `EffectiveWidget` degrade-matrix test (Slider-without-range → Drag, Color-on-scalar → Drag, Multiline-on-non-string → class default, compatible pairings pass through). The ImGui draw itself stays covered by the editor build + the `gpu`-band editor smoke. |

## Verification

- Clean build; `ctest` green — the new `EffectiveWidget` degrade-matrix test passes (`unit`, no ICD); the
  editor `gpu`-band smoke still launches and exits 0; no `unit`/`death` regressions.
- `include_hygiene` unaffected — the new `Veng::UI` signatures are imgui-free (the contract the existing
  `UI::Image` exception already meets); `<imgui.h>` stays under `engine/src/UI/`.
- Manual editor check: a `vec3` tagged `.Display = {.Widget = WidgetKind::Color}` shows a color swatch; a
  scalar with `.Widget = Slider` + a range shows a slider; a string with `.Widget = Multiline` shows a
  multiline box; an incompatible hint degrades and logs once.
- `smoke_golden` does **not** move — inspector-only change, no render/cook path.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
