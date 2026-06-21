# Plan 05 — Component naming consistency

**Goal:** every component is a **bare noun** — no `Component` suffix anywhere.
`PrimitiveComponent` → `Primitive`, `CameraComponent` → `Camera`, and the
value-type `Camera` (the render-ready view-projection it was mislabelling) →
`CameraView`. Codify the rule in CLAUDE.md so the convention holds for future
components. Runs **last** in the planset — a final mechanical sweep after the
primitive work (02, 04) and the consumer migration (03) have settled the set of
references.

## Why this is its own plan

It is a pure naming/clarity pass — no behavior, no mechanism, no format change. Two
of the three components today are bare nouns; `CameraComponent` carries a *forced*
suffix (the value type `Camera` owned the bare name) and `PrimitiveComponent` a
*gratuitous* one (`Primitive` was free). Fixing both, and renaming the genuinely
mislabelled value type, makes the rule exception-free. Keeping it separate keeps the
seam plans (01–04) free of rename churn and lands the convention in one reviewable
commit.

## The renames

1. **value `Camera` → `CameraView`** (`engine/include/Veng/Scene/Camera.h`). It is
   not the authored camera — it is the **computed view + projection** the renderer
   consumes through `SceneView`, the *resolved* side of the recipe→resolved pairing
   (a `Camera` component produces a `CameraView`, as a `Primitive` produces a
   `Mesh`). Rename the `class`, `MakeCamera` → `MakeCameraView`, and the **type** of
   the `SceneView::Camera` field — its field *name* stays `Camera`, only the type
   changes (`const Camera& Camera` → `const CameraView& Camera`). Sweep every
   `const Camera&` across the renderer and its passes (the public headers
   `SceneRenderer.h`, `ShadowCascades.h`, and `PunctualShadows.h` each take one), the
   editor camera, and tests — **~50 lines is a floor**; a raw grep of the value-type
   name is larger (much is comments / `Camera.h`). Also update `engine/CLAUDE.md`'s
   `SceneView` description (the `const Camera& Camera` field). Not a reflected type —
   no `TypeId`.

2. **`CameraComponent` → `Camera`** (same header). The struct and its
   `VE_REFLECT(::Veng::CameraComponent, …)` → `VE_REFLECT(::Veng::Camera, …)`. **The
   `TypeId` literal is unchanged** (`0x6598EF5F5C0A7B10ULL`) — identity is the id, not
   the name — so cooked archives are unaffected. ~9 usages.

3. **`PrimitiveComponent` → `Primitive`** (`engine/include/Veng/Scene/Components.h`).
   The struct, its `VE_REFLECT` (same `TypeId` `0x491B7EC1B0DF276BULL`), the resolver
   `ResolvePrimitiveComponent` → `ResolvePrimitive` and its `VE_RESOLVE`
   registration, and the sample `.prefab.json` key `::Veng::PrimitiveComponent` →
   `::Veng::Primitive`. `Primitive` (bare) is a free name; nothing conflicts.

The shape recipe structs (`CubeShape`/…/`CapsuleShape`) and `MeshRenderer` are
**unchanged** — they are already bare nouns (`MeshRenderer` reads as a component; no
value type owns the name).

## The rule — root `CLAUDE.md`, Identifier naming section

Add, beside the existing "no Hungarian / no kind-tags" rules (which this extends —
`Component` is a kind tag):

> **A component is named as a bare noun**, not suffixed with its kind: `Transform`,
> `Light`, `Camera`, `Primitive` — never `TransformComponent` / `PrimitiveComponent`.
> The type system already says it is a component; the name says *what* it is. When a
> value type would own the bare name, **the value type takes the precise role-name**
> so the component keeps the natural noun — the render-ready view-projection is
> `CameraView`, leaving `Camera` for the component.

This makes the convention exception-free: with the value type renamed, no component
needs a disambiguating suffix, so the editor needs no display-name stripping — a
component's registered name *is* its display name.

## Compat

- **Cooked archives:** untouched. A component's on-disk identity is its `TypeId`,
  and every `TypeId` literal is unchanged; only the registered `Name` strings change.
- **Source assets:** the registered name is an author-facing key in `.prefab.json`,
  so the sample prefab's `::Veng::PrimitiveComponent` key updates to `::Veng::Primitive`.
  **No `.prefab.json` references `CameraComponent`**, so the camera renames touch no
  asset source.

## Tests

- Mechanical reference updates across `tests/` (`Camera` value type → `CameraView`,
  `CameraComponent` → `Camera`, `PrimitiveComponent` → `Primitive`); no new test
  logic — the renames are behavior-preserving.
- **Smoke golden unchanged** — no rendered geometry moves.

## Acceptance

- Clean build; `ctest` green; `smoke_golden` unchanged; `VE_DEBUG` gate clean;
  `include_hygiene` green.
- No component type carries a `Component` suffix; `CameraView` names the value type;
  `Camera`/`Primitive` name their components, with `TypeId`s unchanged.
- The CLAUDE.md naming section states the bare-noun component rule.
