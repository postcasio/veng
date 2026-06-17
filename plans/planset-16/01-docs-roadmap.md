# Plan 01 — Docs + roadmap re-cut

**Goal:** record the single-trait reflection model in the living docs. No code.

## `CLAUDE.md`

The **Scene & ECS** / reflection paragraphs currently describe authoring as "a `VE_REFLECT`
describe-block next to the struct, read back by the zero-arg `Register<T>()`". They are
accurate for structs but silent on how a **leaf or enum** is authored, and they predate the
trait merge. Update:

- In the `TypeRegistry` paragraph, after the `VE_REFLECT` sentence, note that a **leaf or enum**
  type is authored with **`VE_LEAF(Type, 0x…ULL, FieldClass::Kind)`** and a **fieldless
  component** with `VE_TYPE` — all three macros specialise the **single `VengReflect<T>`**
  identity trait with a uniform member set, so `TypeIdOf<T>()`/`FieldClassOf<T>()` read it
  directly and the registry has one `Register<T>()` path (no separate leaf registration).
- In the reflection-layer paragraph, where it lists "a game adds a leaf/struct/component with no
  engine change", make explicit that a leaf/enum is the `VE_LEAF` seam (the open `TypeId` space
  is unchanged; only the spelling is named).

Keep the edits tight — these are factual present-tense statements, no "used to be two traits"
narrative (CLAUDE.md comment/doc policy).

## `plans/README.md`

Add the planset-16 entry in sequence, in the established voice. Match the existing entries'
status-marker convention (e.g. `✅ done, N plans`) — set it to the real plan count and status
when the planset lands:

> - **[planset-16](planset-16/README.md)** — grab bag: reflection trait, asset-library rename,
>   ImGui composite (✅ done, 4 plans). A small cleanup planset (not a future-area chain) bundling
>   three independent wins. Win 1 collapses the reflection layer's two type-identity traits
>   (`ReflectLeaf<T>` for leaves/enums, `VengReflect<T>` for structs) into a single
>   `VengReflect<T>` every reflected type specialises uniformly, deletes the `IsReflectLeaf`
>   SFINAE and the parallel `TypeRegistry::EnsureLeaf<T>()` path, and adds a `VE_LEAF` authoring
>   macro beside `VE_TYPE`/`VE_REFLECT` — behaviour-preserving (no `TypeId`, `FieldClass`, or
>   on-disk format change). Win 2 renames the shared archive library `assetformat` → `assetpack`.
>   Win 3 adds an engine-provided `ImGuiCompositePass` for the scene-behind-ImGui composite.

## This planset's README

Flip the plan 00 + plan 01 status column to `done` on landing.

## Acceptance

`CLAUDE.md` describes the single `VengReflect<T>` trait and names `VE_LEAF` as the leaf/enum
authoring surface; `plans/README.md` carries the planset-16 entry; this README's status table is
current. No code, no test change. Commit:
`planset-16: docs + roadmap — single reflection trait, VE_LEAF authoring surface`.
