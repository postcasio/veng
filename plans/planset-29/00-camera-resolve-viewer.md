# Plan 00 — camera resolve + Viewer selection

**Goal:** add the foundational engine seams the gameplay layer is written against, without
touching the renderer or any draw caller. The camera seam: a **`Viewer { Entity Camera }`**
component names which camera entity a seat renders through, and **`ResolveCameraView`** /
**`ResolvePrimaryCameraView`** turn the selected `Camera` + `Transform` into a `CameraView` at the
caller's aspect. The input seam: **`Input` becomes an always-present service** so headless reads as
the neutral all-zeros state instead of a missing object, letting later systems read input with no
null-guard. Engine additions only — one component, two resolve functions, the `Input` change, a
unit test. It migrates no draw caller (Plan 01 does that) and moves no golden.

## Why it is its own plan

It is the foundational seam every later plan builds on, independently verifiable in isolation —
behavior-free machinery (a seat component, a query, a resolve) with a pure-logic test — landed
before any runtime path, control pipeline, or game mode depends on it. Splitting it from the
runtime migration (Plan 01, which carries the golden risk) keeps the engine-API surface
reviewable against the renderer's `SceneView` contract on its own.

## What lands

- **`Viewer` — the seat-to-camera selection.** A reflected component `Viewer { Entity Camera; }`
  registered in `RegisterBuiltinTypes`
  ([BuiltinTypes.h](../../engine/include/Veng/Scene/BuiltinTypes.h)). A *seat* — a local player,
  a render target, the editor — carries a `Viewer` naming the camera entity it looks through.
  This separates the seat from the camera (scope decision 2): the camera is `(Transform, Camera)`
  data; the `Viewer` is "this seat sees through that one." Its `Camera` field is a reflected
  `Entity` **reference**, so it remaps correctly on prefab spawn like any intra-prefab reference.

- **`ResolveCameraView(const Scene& scene, Entity viewer, f32 aspect) → optional<CameraView>`**
  beside `MakeCameraView` ([Camera.h:106](../../engine/include/Veng/Scene/Camera.h:106)). Reads
  the `Viewer` on `viewer`, looks up its `Camera` entity, and returns
  `MakeCameraView(camera, aspect, WorldMatrix(scene, cameraEntity))` — or `nullopt` if the seat
  has no `Viewer`, names `Entity::Null`, or the named entity lacks `(Transform, Camera)`.
  `WorldMatrix` ([Transforms.h](../../engine/include/Veng/Scene/Transforms.h)) walks the `Parent`
  edge, so a camera parented under a rig resolves correctly. **Aspect is the caller's** (scope
  decision 3).

- **`ResolvePrimaryCameraView(const Scene& scene, f32 aspect) → optional<CameraView>`** — the
  single-viewer convenience the runtime uses: resolve through the **first** `Viewer` entity, else
  fall back to the first bare `(Transform, Camera)` entity (so a one-camera scene with no
  explicit seat still renders), else `nullopt`. More than one `Viewer` is fine (multi-seat is the
  future case); this convenience just takes the first and is documented as the single-output
  helper.

- **`Input` is always present — headless is the all-zeros reading, not a missing service.** `Input`
  borrows the window as a nullable `Window*` (was a `Window&`); `Update()` refreshes its state from
  the window when one is present and otherwise leaves the zero-initialized state (nothing pressed,
  no mouse/scroll delta), and the mouse-capture calls no-op without a window. `Application` **always**
  constructs `m_Input`, so `GetInput()` and the `SystemContext::Input` reference are always valid. A
  headless run is then indistinguishable from a windowed app with nothing pressed — the same neutral
  reading every system already handles — so no system, and no `SystemContext`, ever sees a null. The
  only nullable that remains is `Input`'s own `Window*`, confined to the question "is there a window
  to poll." This is the seam Plan 02's control system is written against.

## Why a Viewer, not an `ActiveCamera` tag

A tag on the camera entity says "this entity *is* the view"; a `Viewer` on a seat says "this seat
*looks through* that camera." The latter is the generalization multi-view and per-client
networking need — two seats can name two cameras, a spectator seat names a camera it does not
possess, a cutscene retargets a seat without moving the camera — and it is barely larger than a
tag. The runtime starts with exactly one seat, so the convenience helper hides the difference for
now while the model stays correct for later.

## Where this sits relative to the renderer

Both helpers return `optional<CameraView>` — the exact type `SceneView::Camera` borrows
([SceneRenderer.h:285](../../engine/include/Veng/Renderer/SceneRenderer.h:285)). This plan adds a
*producer*; no consumer changes here (Plan 01 wires the runtime call site). The resolve is a free
function over `const Scene&`, deliberately **not** a `SceneRenderer` method (scope decision 1), so
the renderer stays source-agnostic and the editor keeps sourcing its view from `EditorCamera`.

## Decisions

1. **The seat owns the selection, as a reflected reference.** `Viewer.Camera` persists and remaps
   through the prefab path with no new serialization, and an authored prefab can declare a seat
   and its camera as data.

2. **First-camera fallback, not an assert, in the convenience helper.** A one-camera scene with
   no authored `Viewer` still renders — the common early case, and what the sample starts from —
   so `ResolvePrimaryCameraView` degrades gracefully.

3. **`nullopt`, not a baked default, when there is no camera.** The helper stays pure; the caller
   decides what "no camera" looks like (Plan 01's runtime uses a fixed default view).

4. **Aspect is a parameter.** A `Camera` has no aspect field; the render target owns aspect, so
   the signature forces the caller to pass it.

5. **Input is a service that always exists, never one that is absent.** Modeling headless as a null
   `Input` would force a null-guard into every system that reads it and would smear nullability
   across all of gameplay; modeling it as an always-present service that reports the neutral state
   keeps `SystemContext::Input` a plain `const Input&` and makes a headless tick automatically a
   pure function of state (zero input is the same input an idle windowed frame produces). The
   nullability collapses to one place — whether `Input` has a window to poll — handled inside
   `Input` itself.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Scene/Camera.h` | Add the `Viewer` component (`VE_REFLECT`, `Entity Camera` reference field) and declare `ResolveCameraView` / `ResolvePrimaryCameraView`. |
| `engine/src/Scene/Camera.cpp` *(new, or the matching TU)* | Define both resolves (`Viewer` lookup, `WorldMatrix`, `MakeCameraView`, the fallback in the primary helper). |
| `engine/include/Veng/Scene/BuiltinTypes.h` / `.cpp` | Register `Viewer` in `RegisterBuiltinTypes`. |
| `engine/include/Veng/Input.h` / `engine/src/Input.cpp` | Borrow the window as `Window*`; `Update()` and the mouse-capture calls no-op without a window, leaving the zero-initialized state. |
| `engine/src/Application.cpp` | Always construct `m_Input` (windowed or headless), so `GetInput()` is always valid. |
| `engine/include/Veng/Scene/SceneSystem.h` | Correct the `SystemContext::Input` doc comment: present-but-neutral in headless, not "unavailable (null)". |
| `tests/unit/…` | A resolve unit suite (see below). |

## The unit test

Pure-logic (no device — CPU math + queries):

- **Seat selection:** a `Viewer` naming a `(Transform, Camera)` entity resolves it;
  `ResolvePrimaryCameraView` picks the first `Viewer`, else the first bare camera, else `nullopt`.
- **World-matrix path:** a camera parented under a translated `Transform` resolves to a
  `CameraView` whose position equals the composed world translation (the `Parent` walk is honored,
  not just the local transform).
- **Aspect plumb-through:** two aspects produce two projections.
- **Degenerate refs:** a `Viewer` with `Entity::Null` or naming an entity lacking `Camera` returns
  `nullopt` (no crash).

## Verification

- Clean build; `ctest` green across the bands; the new unit suite passes.
- The **no-device cooker test stays green** — `Viewer` is a GPU-free reflected type.
- `include_hygiene` stays green — `Camera.h` gains no backend include.
- A headless (windowless) `Input` is constructible and reports the all-zeros state, so a test or
  tool can build a real empty `Input` instead of fabricating one.
- `smoke_golden` does **not** move — no caller resolves through the new helpers yet, and the
  always-present headless `Input` reads identically to the prior pinned-pose path.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
