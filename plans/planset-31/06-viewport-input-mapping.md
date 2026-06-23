# Plan 06 — viewport input mapping

**Goal:** give the `Viewport` the window↔view mapping its owned region makes possible —
`WindowToViewport` (hit-test a window point into the region, remap to normalized view coordinates)
and `ScreenToWorldRay` (that point plus the camera from the last `ViewState`, into a world-space
ray). Add a small `Ray` primitive in `Veng/Math/` beside `AABB`/`Frustum`. The mapping is
**gameplay-agnostic** — the viewport never sees `Viewer`/`PlayerInput` — and is the seam editor
entity-picking consumes immediately and multi-seat *pointer* routing consumes later.

**Depends on Plan 02 (a registered, region-owning viewport the editor and tests drive).**

## Why it is its own plan

The mapping is the input-facing half of "a view owns its rectangle," and it is what makes the
inversion pay off beyond rendering: with it, the editor can pick entities and a future router can ask
"which viewport did this click land in?" It carries a new math primitive (`Ray`) and a pure-logic
test, independent of the rendering plans — so it is cleanly reviewable on its own, and isolating it
keeps the rendering plans rendering-only.

## What lands

- **`Ray` — a math primitive.** `struct Ray { vec3 Origin; vec3 Direction; }` in `Veng/Math/Ray.h`
  beside `AABB`/`Frustum`, with the construction + a parametric `At(t)` helper. Device-free,
  reflected nowhere — a plain value type. (Scene raycast / broadphase intersection is **not** here;
  this plan adds the ray, not the query.)

- **`Viewport::WindowToViewport`** — `optional<vec2> WindowToViewport(ivec2 windowPoint) const`.
  Hit-tests the point against `GetRegion()`; returns the point remapped into the view as **normalized
  `[0,1]` coordinates** across the region (`(0,0)` top-left of the region, `(1,1)` bottom-right), or
  `nullopt` when the point lies outside the region. `[0,1]` is the contract; `ScreenToWorldRay`
  converts to NDC internally where the unproject needs it. The pointer→viewport primitive: a router
  hit-tests a click against each registered viewport's region to find the one it belongs to; the
  editor uses it for hover/pick coordinates.

- **`Viewport::ScreenToWorldRay`** — `optional<Ray> ScreenToWorldRay(ivec2 windowPoint) const`.
  Composes `WindowToViewport` with the **camera retained from the last `ViewState`**: maps the `[0,1]`
  point to NDC and unprojects it through the inverse view-projection into a world-space `Ray` (origin
  at the camera, direction through the pixel), or `nullopt` outside the region or before a `ViewState`
  is set. `CameraView` exposes `ViewProjection()` but **no** inverse accessor, so the impl computes
  `glm::inverse(camera.ViewProjection())` itself. Self-contained — the viewport already holds the
  camera, so picking needs no external plumbing.

- **The design note (carried in the plan + the docs in Plan 07): two routing mechanisms, one seam.**
  Multi-seat input has two independent fan-outs — *pointer* events routed by **viewport-region
  hit-test** (this mapping), and *device* (gamepad) events routed by **device id** (independent of
  viewports, an `InputRouter`/`Input` extension). The viewport supplies only the pointer seam and
  stays gameplay-agnostic; the seat model (`Viewer` + `PlayerInput` + `Possesses` + a device + a
  viewport) and the router fan-out are area-4 work this seam unblocks but does not build.

## Decisions

1. **The viewport supplies mapping, not picking.** It maps window↔view and produces a ray; *what the
   ray hits* (a broadphase raycast over the scene) is gameplay/editor code or a later plan. This
   keeps the viewport a rendering-and-placement object that happens to expose its own geometry, not a
   scene-query engine.

2. **`ScreenToWorldRay` reuses the retained camera.** The viewport already keeps the last
   `ViewState` to render; reusing its camera makes picking a self-contained call rather than forcing
   every caller to re-pass a `CameraView`. A point pushed before any `ViewState` returns `nullopt`,
   not a fabricated ray.

3. **Gameplay-agnostic, on purpose.** The viewport exposes region + mapping and imports no gameplay
   type. The viewport↔seat association lives in the router/app (area 4), so the rendering layer never
   depends on `Viewer`/`PlayerInput`.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Math/Ray.h` *(new)* | `Ray { Origin, Direction }` + `At(t)`; device-free. |
| `engine/include/Veng/Renderer/Viewport.h` | `WindowToViewport`, `ScreenToWorldRay` (full Doxygen, including the gameplay-agnostic + retained-camera contract). |
| `engine/src/Renderer/Viewport.cpp` | Hit-test + remap; the inverse-view-projection unproject to a world ray. |
| `tests/unit/…` | Pure-logic mapping suite (no device). |

## The unit test

- **Hit-test:** a point inside the region maps to a normalized coordinate matching its fractional
  position; a point outside returns `nullopt`; region corners map to the expected extremes.
- **Remap under offset:** a viewport with a non-zero region offset (a splitscreen quadrant) maps a
  window point to the same normalized coordinate as the equivalent point in a zero-offset region.
- **Ray unproject:** with a known camera and region, a center point yields a ray through the camera's
  forward axis; an off-center point yields the expected skew; before any `ViewState`, `nullopt`.
- **`Ray::At`:** `At(0)` is the origin; `At(t)` advances along a normalized direction by `t`.

## Verification

- Clean build; `ctest` green; the new unit suite passes (no device — pure math).
- `include_hygiene` green — `Ray.h` is glm-only; `Viewport.h` gains no backend include.
- `smoke_golden` / `validation_gate` unaffected — no render path changes.
- The editor gains a usable `ScreenToWorldRay` for entity picking (consumed by the editor in a later
  pass, not wired here).

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
