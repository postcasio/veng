# planset-42 — input actions: bindings as data, `PlayerInput` as `ActionState`

**Phase goal:** stop gameplay from reading raw keys. Today a game's control system polls
`Veng::Input` for `Key::W`/`Key::Space` and hand-fills a fixed `PlayerInput` snapshot
([examples/hello-triangle/main.cpp](../../examples/hello-triangle/main.cpp)). This planset
introduces an **action-mapping layer**: a game authors **named actions** ("Move", "Jump") and
binds raw device inputs to them through **cooked, remappable data** (`InputMappingContext`
assets, active per-seat as a context stack), and an engine system resolves those bindings each
tick into the seat's input snapshot. `PlayerInput` **becomes** that resolved action snapshot
(the `ActionState`) — a game-defined set of action values, not a fixed `{Move, Look, Buttons}`
struct — and the control system reads actions **by name** (`input.GetAxis(Actions::Move)`,
`input.WasTriggered(Actions::Jump)`) instead of polling hardware.

`Intent` is **untouched**. The action layer feeds `Intent`, it does not replace it, and gameplay
keeps reading `Intent` — so **AI and remote players stay drop-in `Intent` producers** that never
consult the action layer at all. Only the local human seat's input flows through actions. This is
the first half of [future area 4](../future/README.md#4-event--input-systems) (the input half);
**multi-seat routing and the networking layer** are the named next planset, built directly on the
seat seam this establishes.

## Why now

The event-routed input core is delivered and the seams the action layer needs are all in place, so
this is mostly *assembly on existing surface*, not new engine mechanism:

- **The seat model exists.** `Viewer` (a seat naming its camera), `Possesses` (seat → pawn),
  `PlayerInput` (the per-seat serializable snapshot), and `Intent` (the sim/net chokepoint) all
  landed in planset-29. This planset repurposes `PlayerInput` and adds one per-seat component; it
  writes no new seat concept.
- **Raw input is already an event-fed snapshot.** planset-30 made `Veng::Input` an event-fed
  snapshot behind the `InputRouter`'s focus stack, and `SystemContext.Input` is always present
  (neutral all-zeros in headless). The action layer reads that snapshot — it adds no new device
  plumbing.
- **Reflection gives the asset its cook, load, and editor for free.** An `InputMappingContext` is
  reflected data: it cooks and loads like every other asset, and its binding table draws an
  editable inspector through `DrawFieldWidget` with **no bespoke widget code** — exactly as
  `ProjectSettings::Configurations` (a `FieldClass::Array`) does. The editor panel is therefore
  *almost free*.
- **The system + id conventions are settled.** `InputMappingSystem` is a builtin `SceneSystem`
  registered exactly like `MovementSystem`; `ActionId` is a minted `u64` leaf authored exactly like
  `AssetId`/`TypeId`/`SystemId`. Nothing here is a new host-ABI institution — the module ABI stays
  at **version 4**.

## The unifying design — resolve raw input into named actions, once, per seat

```
        raw device snapshot (Veng::Input, routed by focus)
                     │
          ┌──────────▼───────────┐   per seat, reads:
          │  InputMappingSystem   │     · InputContextStack (active InputMappingContexts)
          │  (builtin, Sim,       │     · the raw snapshot
          │   ordered first)      │   the ONLY reader of raw device state
          └──────────┬───────────┘
                     │ writes
          ┌──────────▼───────────┐
          │ PlayerInput           │  ← IS the ActionState: a game-defined set of
          │  = resolved actions   │    { ActionId, vec2 Value, ActionPhase } per seat
          └──────────┬───────────┘    (serializable — the per-tick wire input)
                     │ read by name
          ┌──────────▼───────────┐   game-specific policy; knows Actions::Move / ::Jump;
          │  control system       │   transforms camera-space → pawn frame
          │  (game, Sim)          │
          └──────────┬───────────┘
                     │ writes
               ┌─────▼──────┐
               │  Intent    │  ← UNCHANGED. gameplay + MovementSystem read this.
               └─────┬──────┘      AI and net write Intent directly, bypassing all of the above.
                     │
             MovementSystem / rule systems (Sim)
```

- **`ResolveActions(activeContexts, rawState) → ActionState`** is a pure, device-free function —
  bindings × active contexts × the raw input snapshot → each action's value + phase. It is
  unit-tested with no window, foundation-first, mirroring `DecideBarrier` / `ComputeCascades`.
  `InputMappingSystem` is the thin ECS wrapper that feeds it a seat's data and stores the result in
  that seat's `PlayerInput`.
- **`PlayerInput` becomes the `ActionState`.** It holds a small set of
  `ActionSample { ActionId Id; vec2 Value; ActionPhase Phase }`. `Value` covers all three action
  shapes (button → `x∈{0,1}`; 1D axis → `x`; 2D axis → `xy`); `ActionPhase` is `Started` /
  `Ongoing` / `Completed` (pressed-this-frame / held / released). It stays the **serializable**
  per-seat chokepoint the type was always documented as — now with a game-defined action set
  instead of an opaque `u32 Buttons`.
- **Bindings are a cooked asset.** `InputMappingContext` (`AssetType::InputMap`) declares its
  actions (id + name + kind) and a `vector<Binding>` (raw source → action, with minimal
  scale/invert/axis-component modifiers). A game authors a `*.inputmap.json` source; the cook
  validates every binding against the context's declared actions; the runtime loads it by `AssetId`
  and hot-reloads it through the existing `MountMemory` path.
- **Contexts are a per-seat stack.** An **`InputContextStack`** component holds the ordered active
  `AssetHandle<InputMappingContext>` for a seat, highest priority last; gameplay systems push/pop it
  (enter vehicle → push the `vehicle` context). It is the fine-grained sibling of the router's
  coarse focus stack.
- **The editor panel is the free win.** An `InputMappingEditorPanel` registered for
  `AssetType::InputMap` draws the reflected context (the binding array through `DrawFieldWidget`),
  with the same live recook → hot-reload preview loop the texture/material editors use. It is
  deliberately **basic** — the reflected inspector plus action-name labels — not a bespoke
  press-a-key-to-bind capture UX (deferred with the rest of the rich-editor investment).

## Plans

| # | Plan | Summary | Status |
|---|------|---------|--------|
| 00 | Action types + the pure resolve core | The `ActionId` leaf, `ActionKind`/`ActionSample`/`ActionPhase`, `InputSource`/`Binding`/`InputAction`, the in-memory `ResolvedContext`, and the device-free `ResolveActions(activeContexts, rawState) → ActionState` pure function. Fully unit-tested, no window. Foundation-first. | proposed |
| 01 | `PlayerInput` → `ActionState` + `InputMappingSystem` (single seat) | Repurpose `PlayerInput` to the resolved action snapshot (the stable-ordered-schema wire representation decided here); add the `InputContextStack` component and the builtin **`InputMappingSystem`** (Sim, ordered first, the sole raw-input reader), writing each locally-owned seat's `PlayerInput`. Migrate hello-triangle: delete the `Key::` polling, keep only the named-action `PlayerInput→Intent` policy. Depends on 00. | proposed |
| 02 | The cooked `InputMappingContext` asset | `AssetType::InputMap`, the `*.inputmap.json` source, the cooker `InputMapImporter` (binding→action validation), the runtime loader, `AssetManager::Load`, hot-reload via `MountMemory`. hello-triangle's bindings move from C++ into a cooked asset the game references; the level/game names the default context to push. Depends on 01. | proposed |
| 03 | The editor panel | `InputMappingEditorPanel` registered for `AssetType::InputMap`: the reflected binding-table inspector (free `DrawFieldWidget`) + action-name labels + live recook→hot-reload preview, matching the texture/material editor idiom. Basic by design. Depends on 02. | proposed |
| 04 | Docs, template co-migration, roadmap pass | `docs/guides/` input-mapping entry; `engine/CLAUDE.md` + `editor/CLAUDE.md` updates; `examples/template` co-migration (it drives a spinning cube with no player input — verify it still needs none, or gains a minimal binding); the `future/README.md` area-4 update naming multi-seat routing + networking as the next planset built on the seat seam; the full verification band. The closer. Depends on 00–03. | proposed |

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = implemented, migrated, verified, committed.

## Dependencies

- **00 → 01 → 02 → 03 → 04**, a linear chain: the pure core, then the runtime seat wiring, then the
  cooked asset, then the editor over it, then the closer. 00 is pure and testable in isolation; 01
  can migrate hello-triangle to an in-C++ context before 02 makes it a cooked asset (so 01 is
  independently verifiable).
- Builds on **planset-29** (the `Viewer`/`Possesses`/`PlayerInput`/`Intent` seat model + the
  `SystemRegistry`/`VE_SYSTEM` catalog), **planset-30** (the event-fed `Input` snapshot +
  `InputRouter` focus stack), and the reflection layer's `FieldClass::Array`/`VE_LEAF`/`VE_ENUM`
  (planset-10/16/26/36) for the asset + free inspector.

## The decisions this planset settles

- **`PlayerInput` is the `ActionState`.** The resolved per-seat action snapshot *is* the
  serializable input the type always advertised. There is no separate `ActionState` struct beside
  `PlayerInput`; they are one object. The wire form is a **fixed positional vector in the active
  context's declared action order** (the schema resolved once when the context stack changes), not a
  per-tick map of `ActionId`s — so the game-defined action set costs no serialization width over a
  fixed struct. This is the one representation decision worth pinning early (like `Authority`,
  painful to reshape once it is on the wire).
- **Gameplay reads `Intent`, only the local human control system reads actions.** The action layer
  is client-local and device-facing; it produces `Intent` and stops there. AI systems and the future
  net layer produce `Intent` directly, never touching an action or a context. This preserves the
  planset-29 drop-in-producer property — the whole reason `Intent` exists.
- **Bindings are a cooked asset; the action vocabulary is C++.** An action's *meaning* is a C++
  constant a control system references (`Actions::Jump`); an action's *bindings* are remappable data.
  The cook validates bindings against a context's declared actions. There is **no `ActionRegistry`
  on the host struct** — an action exists by being declared in a context, resolution only matches
  ids, and the module ABI is unchanged. A global action registry (for cross-context validation and
  editor name lookup) is a clean later addition if a real editor or visual-scripting consumer wants
  it.
- **The editor panel is basic and free, not bespoke.** The reflected binding table draws through
  `DrawFieldWidget` like any reflected asset; the panel adds only action-name labels and the live
  recook loop. A press-a-key-to-bind capture widget, an action dropdown driven by a registry, and a
  drag-to-reorder binding UX are deferred — there is no data consumer of actions yet (everything is
  C++), so the investment is not justified until visual scripting or a runtime remapping screen
  needs it.
- **`InputMappingSystem` is a builtin Sim system, ordered first.** It is the single reader of raw
  device state, registered in `RegisterBuiltinSystems` beside `MovementSystem`. It runs for
  **locally-owned** seats only; a remote/AI seat's `PlayerInput` arrives replicated or synthesized,
  the same drop-in pattern one layer down. In headless it reads the neutral snapshot and writes empty
  actions, with no guard.

## What remains future

- **Multi-seat input routing + the networking layer** — the other half of area 4, the **named next
  planset**. Multi-seat routes raw events *per seat*: pointer/keyboard by region (planset-31's
  `WindowToViewport`) and device events by id, each into the right seat's `InputContextStack` →
  `PlayerInput`. The net layer replicates `PlayerInput` (now the tight action snapshot) and re-derives
  `Intent` server-side. This planset builds the seat seam both consume: `InputMappingSystem` already
  resolves *per seat*, so multi-seat is additive routing, not a rewrite. A **playable splitscreen
  sample** is the demanding second consumer that lands with it.
- **Runtime remapping UI.** An in-game settings screen that rebinds keys is a game-side `Veng::UI`
  feature (mutating the active context, serializing user overrides to the per-OS user-data dir), not
  an editor panel. Distinct surface, separate planset.
- **Richer triggers/modifiers.** A first cut keeps modifiers minimal (scale, invert, axis component)
  and phases to started/ongoing/completed. Unreal-style chorded / tap-vs-hold / hold-and-release
  triggers and the full modifier zoo (swizzle, response curves, dead-zone shapes) are a later
  refinement behind the same `ResolveActions` core.
- **A global `ActionRegistry` + the bespoke editor.** A host-owned action catalog (the `SystemId`
  analogue) enabling cross-context validation, editor action dropdowns, and a press-to-bind capture
  UX — taken up when a data consumer of actions (visual scripting, a data-driven command palette)
  earns it.
- **Action-driven data consumers.** The moment a node graph, event system, or command palette can
  bind behavior to a named action, the action asset becomes a first-class authoring surface and the
  basic editor grows up. Not built until that consumer exists.
