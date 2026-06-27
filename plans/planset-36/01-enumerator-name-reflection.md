# Plan 01 — enumerator-name reflection

**Goal:** make a reflected enum carry its **`{name, value}` table** on `TypeInfo`, authored once with a
`VE_ENUM`/`VE_ENUMERATOR` describe-block, so the generic `FieldClass::Enum` widget becomes an
**automatic named combo** — retiring the three hand-written enum combos the editor maintains today
(`LightType`, `CompressionRole`, `CompressionFormat`). This is the single biggest day-to-day friction
win in the planset: an enum field has read like a raw integer drag unless someone wrote a custom widget
per enum type.

**Depends on 00** — it shares the editor's per-`FieldClass` dispatch seam in
[`FieldWidget.cpp`](../../editor/src/FieldWidget.cpp) (merge in number order to avoid colliding there);
the enumerator table itself is independent of the `FieldDisplay` cascade.

## Why it is its own plan

Enumerator names are **intrinsic to the type** — there is no per-field variation — so they belong on
`TypeInfo`, not in the `FieldDisplay` cascade. That makes this a self-contained reflection capability:
an authoring macro, a `TypeInfo` slot, the registrar wiring, and one rewrite of the generic enum widget.
Folding it into another plan would tangle a reflection-layer change with a widget-dispatch change; kept
separate, it is a clean "author the table, draw the combo, delete the custom widgets" pass with its own
device-free test.

## What lands

- **`VE_ENUM` / `VE_ENUMERATOR` / `VE_ENUM_END` — the describe-block.** Mirrors the existing
  `VE_REFLECT`/`VE_FIELD`/`VE_REFLECT_END` shape so it reads the same and the field names/values are
  written exactly once:
  ```cpp
  VE_ENUM(::Veng::LightType, 0x6B1D62EF4B5A16ULL)
      VE_ENUMERATOR(Directional)
      VE_ENUMERATOR(Point)
      VE_ENUMERATOR(Spot)
  VE_ENUM_END();
  ```
  The second argument is the enum's **`TypeId`**, exactly as `VE_REFLECT`/`VE_LEAF` take today — the
  migration reuses each enum's *existing* id verbatim (the example reuses `LightType`'s current id), so no
  id is minted and no cooked artifact moves. It specializes `VengReflect<T>` with
  `Class = FieldClass::Enum` (exactly as `VE_LEAF(…, Enum)` does today) **plus** an `Enumerators()`
  accessor returning `vector<EnumEntry>`. `VE_ENUMERATOR(Name)` records
  `{ #Name, static_cast<i64>(Owner::Name) }`, so an enum with **explicit values** (gaps, non-zero starts)
  is captured correctly off the enum constant itself — never by positional assumption.

- **`EnumEntry` + `TypeInfo::Enumerators`.** A tiny `struct EnumEntry { string Name; i64 Value; };` in
  the reflection headers, and a `vector<EnumEntry> Enumerators;` on `TypeInfo` (empty for non-enums).
  `TypeRegistry::RegisterImpl` fills it from `VengReflect<T>::Enumerators()` when
  `Class == FieldClass::Enum`, the same conditional shape the variant thunks already use
  ([`TypeRegistry.h`](../../engine/include/Veng/Reflection/TypeRegistry.h)).

- **The generic enum widget becomes a named combo.** `DrawEnum` in
  [`FieldWidget.cpp`](../../editor/src/FieldWidget.cpp) is rewritten: when the field's `TypeInfo` has a
  non-empty `Enumerators` table, it draws a `UI::Combo` over the enumerator names. The **read** path
  matches the existing integer path (`DrawEnum` already does this): widen the `TypeInfo::Size` backing
  bytes into an `i64` (`memcpy` the low `Size` bytes — little-endian, which the codebase already assumes),
  then find the `EnumEntry` whose `Value` equals it to seed the combo's selected index. **A backing value
  matching no enumerator** (schema drift, an authored out-of-range value) selects no entry and the combo
  shows a synthesized `"(unknown <decimal>)"` label, so it is never silently wrong and editing it to a
  named entry repairs it. The **write** path narrows the chosen entry's `i64 Value` back into the low
  `Size` bytes, the same truncation the integer path uses. An enum with **no** table (a bare
  `VE_LEAF(…, Enum)` a game has not migrated) falls back to today's editable-integer drag — so the change
  is backward-compatible and the named combo is purely additive.

- **The three custom enum combos are deleted.** `RegisterCompressionWidgets` (the `CompressionRole`/
  `CompressionFormat` combos) and the `LightType` combo registered in `InspectorPanel`'s constructor
  ([`InspectorPanel.cpp`](../../editor/src/panels/InspectorPanel.cpp)) become redundant — the generic
  combo now presents the same named list. Their backing name tables that *also* serve a non-editor
  consumer stay: the `CompressionRole`/`CompressionFormat` `ToString`/`Parse` tables are read by the
  cooker's JSON parser and the editor's JSON writer, so those remain; only the **editor combo
  registrations** are removed.

  **One behavior delta to note:** a custom field widget's signature carries no change signal, so
  `DrawFieldWidget` returns `false` after a custom combo edit (`FieldWidget.cpp` — "a custom-widget edit
  does not re-resolve"). The generic `DrawEnum` returns `true` on change, so editing one of these enums now
  re-runs the inspector's `ResolveEntity` (e.g. a `LightType` change re-resolves the entity). This is more
  correct, not a regression — but it is a real change from today's no-signal behavior; confirm nothing
  downstream relied on these edits *not* signalling change.

- **Engine builtin enums migrate `VE_LEAF(…, Enum)` → `VE_ENUM`.** `LightType`, `RootMotionMode`,
  `MotionSpace`, `Tier`, `SessionPhase` ([`Components.h`](../../engine/include/Veng/Scene/Components.h)),
  and `CompressionRole`/`CompressionFormat`
  ([`Veng/Project/`](../../engine/include/Veng/Project/CompressionRole.h)) gain enumerator blocks. The
  `TypeId`s are unchanged, so no cooked artifact moves.

- **A device-free table test.** A `unit` test registers an enum authored with `VE_ENUM` (including one
  with an explicit non-contiguous value) and asserts `TypeInfo::Enumerators` holds the right
  `{name, value}` pairs in declaration order — guarding the macro and the registrar wiring with no ICD
  and no ImGui.

## Decisions

1. **Enumerator names live on `TypeInfo`, not the cascade.** They are intrinsic to the type with no
   per-field meaning, so they are a `TypeInfo` slot like the variant thunks — not a `FieldDisplay` member.
2. **Values are read off the enum constant, not assumed by position.** `VE_ENUMERATOR(Name)` casts
   `Owner::Name`, so explicit/gapped enum values reflect correctly. Declaration order is preserved for
   the combo's display order.
3. **The named combo is additive and backward-compatible.** An enum without a table draws the existing
   integer path, so migrating an enum to `VE_ENUM` is opt-in and a game's un-migrated enums keep working.
4. **Only the editor combo registrations are deleted; the cook-facing name tables stay.** The
   `CompressionRole`/`CompressionFormat` `ToString`/`Parse` tables serve the cooker and the editor's JSON
   round-trip, a separate concern from the inspector combo. The `LightType` combo, which had **no**
   non-editor consumer, goes away entirely.
5. **The cook path is untouched.** Prefab/level enum *values* still serialize exactly as before; this
   plan does not add enum-by-name to the cooker's JSON (the `{name, value}` table now *makes that
   possible*, but it is a deliberately out-of-scope free follow-on — see the README's "what remains
   future").

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Reflection/ReflectionTypes.h` | Add `struct EnumEntry { string Name; i64 Value; }`. |
| `engine/include/Veng/Reflection/Reflect.h` (or a new `Enum.h`) | The `VE_ENUM`/`VE_ENUMERATOR`/`VE_ENUM_END` describe-block macros. |
| `engine/include/Veng/Reflection/TypeRegistry.h` | `vector<EnumEntry> Enumerators;` on `TypeInfo`; fill it for `Class == Enum` in `RegisterImpl`. |
| `engine/include/Veng/Scene/Components.h` | Migrate `LightType`/`RootMotionMode`/`MotionSpace`/`Tier`/`SessionPhase` to `VE_ENUM`. |
| `engine/include/Veng/Project/CompressionRole.h`, `CompressionFormat.h` | Migrate to `VE_ENUM` (keeping the `ToString`/`Parse` tables). |
| `editor/src/FieldWidget.cpp` | Rewrite `DrawEnum` as a name combo (table present) with the integer drag as fallback. |
| `editor/src/panels/InspectorPanel.cpp` | Delete the `LightType` combo registration and the `CompressionRole`/`CompressionFormat` combo registrations. |
| `tests/unit/…` | A device-free `VE_ENUM` → `TypeInfo::Enumerators` table test (incl. an explicit-value enum). |

## Verification

- Clean build; `ctest` green — the new enum-table test passes (`unit`); the existing reflection/prefab/
  level tests still pass (enum `TypeId`s and serialized values unchanged).
- `include_hygiene` unaffected — `EnumEntry` and the macros are over the reflection headers only.
- Editor: the `LightType`, `CompressionRole`, and `CompressionFormat` fields now show a **named combo**
  with no per-type registration; a bare un-migrated `VE_LEAF(…, Enum)` still draws the integer fallback.
- `smoke_golden` does **not** move — no cook or render path changes.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
